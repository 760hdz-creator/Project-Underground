/* Project Underground runtime entry: map memory, load default.xbe, start the entry thread. */
#include <windows.h>
#include <setjmp.h>
#include <stdlib.h>
#include "rt.h"
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>   /* SDLActivity calls SDL_main in libmain.so */
#endif

typedef struct { uint32_t va; GuestFunc fn; } FuncEntry;
extern const FuncEntry g_functable[];
extern const uint32_t g_functable_count;

static FILE *g_missed;

static GuestFunc lookup(uint32_t va) {
    uint32_t lo = 0, hi = g_functable_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_functable[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    return (lo < g_functable_count && g_functable[lo].va == va) ? g_functable[lo].fn : NULL;
}

void x86_spin(void) { sched_maybe_yield(); }

void x86_unimpl(X86Context *c, uint32_t va, const char *what) {
    rt_fatal("unimplemented at %08X: %s (esp %08X, [esp] %08X)", va, what, c->esp, RD32(c->esp));
}

void x86_call_indirect(X86Context *c, uint32_t va) {
    if (kernel_call(c, va)) return;
    if (dsound_vcall(c, va)) return;
    GuestFunc f = lookup(va);
    if (!f) {
        if (va == 0) {   /* null function pointer: registers and the guest stack for diagnosis */
            LOG("[rt] call 0: eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X\n", c->eax, c->ecx,
                c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
            for (int i = 0; i < 16; i++) LOG("[rt]   [esp+%02X] %08X\n", i * 4, RD32(c->esp + i * 4));
            const char *d = getenv("UG_DUMP");   /* UG_DUMP=<hex va>,<hex len>: guest memory at the fatal */
            uint32_t a, n;
            if (d && sscanf(d, "%x,%x", &a, &n) == 2)
                for (uint32_t i = 0; i < n; i += 16)
                    LOG("[rt]   %08X: %08X %08X %08X %08X\n", a + i, RD32(a + i), RD32(a + i + 4), RD32(a + i + 8),
                        RD32(a + i + 12));
        }
        if (g_missed && va) { fprintf(g_missed, "%X\n", va); fflush(g_missed); }
        rt_fatal("indirect call to unlifted %08X (return %08X); added to out/missed_targets.txt", va, RD32(c->esp));
    }
    f(c);
}

void x86_int(X86Context *c, uint32_t va, uint32_t vector) {
    if (vector == 3) { LOG("[rt] int3 at %08X ignored\n", va); c->esp += 0; return; }
    rt_fatal("int %u at %08X", vector, va);
}
uint32_t x86_in(X86Context *c, uint32_t port, int size) {
    static uint32_t logged[16];
    static int nlogged;
    (void)c;
    for (int i = 0; i < nlogged; i++) if (logged[i] == port) return 0;
    if (nlogged < 16) logged[nlogged++] = port;
    LOG("[rt] in port %X size %d (logged once)\n", port, size);
    return 0;
}
void x86_out(X86Context *c, uint32_t port, uint32_t v, int size) { (void)c; LOG("[rt] out port %X = %X size %d\n", port, v, size); }
uint32_t x86_eflags_id;
void x86_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    switch (*eax) {
    case 0: *eax = 2; *ebx = 0x756E6547; *edx = 0x49656E69; *ecx = 0x6C65746E; break;   /* "GenuineIntel" */
    case 1: *eax = 0x0683; *ebx = 0; *ecx = 0; *edx = 0x0383F9FF; break;                /* family 6 model 8: MMX SSE */
    case 2: *eax = 0x03020101; *ebx = 0; *ecx = 0; *edx = 0x0C040841; break;            /* cache descriptors */
    default: *eax = *ebx = *ecx = *edx = 0; break;
    }
}
uint64_t x86_rdtsc(X86Context *c) { (void)c; return sched_now_100ns() * 733 / 10; /* 733 MHz */ }
int ug_widescreen(void) { const char *e = getenv("UG_WIDESCREEN"); return !(e && *e == '0'); }

double x87_load80(uint32_t a) {
    uint64_t mant = RD64(a);
    uint16_t se = RD16(a + 8);
    int exp = se & 0x7FFF;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (exp == 0 && mant == 0) return sign * 0.0;
    if (exp == 0x7FFF) return (mant << 1) ? NAN : sign * INFINITY;
    return sign * ldexp((double)mant, exp - 16383 - 63);
}

void x87_store80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e);
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}

#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    uintptr_t addr = r->NumberParameters >= 2 ? (uintptr_t)r->ExceptionInformation[1] : 0;
    if (r->ExceptionCode < 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH; /* debug prints etc. */
    HMODULE base = GetModuleHandleA(NULL);
    LOG("FATAL: host exception %08lX at +%llX, data %p (guest %08llX)\n", r->ExceptionCode,
        (unsigned long long)((uintptr_t)r->ExceptionAddress - (uintptr_t)base), (void *)addr,
        (unsigned long long)(addr - (uintptr_t)g_mem));
    uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(sp, &mbi, sizeof mbi);
    uintptr_t *end = (uintptr_t *)((uint8_t *)mbi.BaseAddress + mbi.RegionSize);
    LOG("FATAL: stack:");
    for (int i = 0, n = 0; sp + i < end && n < 24; i++) {
        uintptr_t v = sp[i];
        if (v > (uintptr_t)base && v < (uintptr_t)base + 0x2000000) { LOG(" +%llX", (unsigned long long)(v - (uintptr_t)base)); n++; }
    }
    LOG("\n");
    ExitProcess(3);
}

static HANDLE g_main_thread;

static DWORD WINAPI watchdog(void *p) {
    DWORD secs = (DWORD)(uintptr_t)p;
    for (int sample = 0; sample < 60; sample++) {   /* 60 samples, 50 ms apart */
        Sleep(sample ? 50 : secs * 1000);
        SuspendThread(g_main_thread);
        CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
        GetThreadContext(g_main_thread, &ctx);
        HMODULE base = GetModuleHandleA(NULL);
        LOG("[watchdog] sample %d rip +%llX stack:", sample, (unsigned long long)(ctx.Rip - (uintptr_t)base));
        uintptr_t *sp = (uintptr_t *)ctx.Rsp;
        MEMORY_BASIC_INFORMATION mbi; VirtualQuery(sp, &mbi, sizeof mbi);
        uintptr_t *end = (uintptr_t *)((uint8_t *)mbi.BaseAddress + mbi.RegionSize);
        for (int i = 0, n = 0; sp + i < end && n < 20; i++) {
            uintptr_t v = sp[i];
            if (v > (uintptr_t)base && v < (uintptr_t)base + 0x2000000) { LOG(" +%llX", (unsigned long long)(v - (uintptr_t)base)); n++; }
        }
        LOG("\n");
        /* UG_PEEK="va,len;*ptr_va+off,len": guest memory hex dumps (sample 0); '*' dereferences a guest pointer. */
        const char *pk = sample ? NULL : getenv("UG_PEEK");
        while (pk && *pk) {
            int deref = *pk == '*';
            char *e;
            uint32_t va = (uint32_t)strtoul(pk + deref, &e, 16), off = 0, len = 0x40;
            if (deref) va = RD32(va);
            if (*e == '+') off = (uint32_t)strtoul(e + 1, &e, 16);
            if (*e == ',') len = (uint32_t)strtoul(e + 1, &e, 16);
            va += off;
            for (uint32_t i = 0; i < len; i += 16) {
                LOG("[peek] %08X:", va + i);
                for (uint32_t j = 0; j < 16 && i + j < len; j += 4) LOG(" %08X", RD32(va + i + j));
                LOG("\n");
            }
            pk = strchr(e, ';');
            if (pk) pk++;
        }
        ResumeThread(g_main_thread);
    }
    ExitProcess(4);
}


/* settings.ini [settings] (written by the launcher): UG_* keys become environment variables unless already set,
 * so the env still overrides for tests. */
static void load_settings(void) {
    wchar_t ini[MAX_PATH], buf[8192];
    if (!GetFullPathNameW(L"settings.ini", MAX_PATH, ini, NULL)) return;
    DWORD n = GetPrivateProfileSectionW(L"settings", buf, 8192, ini);
    for (wchar_t *p = buf; p < buf + n && *p; p += wcslen(p) + 1) {
        wchar_t *eq = wcschr(p, L'=');
        if (!eq || wcsncmp(p, L"UG_", 3)) continue;
        *eq = 0;
        if (!_wgetenv(p)) _wputenv_s(p, eq + 1);
        *eq = L'=';
    }
}
#else
/* Linux: SIGSEGV/SIGBUS/SIGILL/SIGFPE report the faulting guest address (on an alternate stack: a fiber's stack
 * may be the thing that overflowed), the same FATAL line as Windows. */
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <execinfo.h>
static void crash_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uintptr_t addr = (uintptr_t)si->si_addr;
#ifdef __aarch64__
    void *pc = (void *)uc->uc_mcontext.pc;
#else
    void *pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
#endif
    LOG("FATAL: host signal %d at %p, data %p (guest %08llX)\n", sig, pc,
        (void *)addr, (unsigned long long)(addr - (uintptr_t)g_mem));
    void *bt[24];
    int n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 2);
    _exit(3);
}
static void crash_handler_install(void) {
    static uint8_t alt[256 * 1024];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}


/* settings.ini [settings]: UG_* keys become environment variables unless already set (as on Windows). */
static void load_settings(void) {
    FILE *f = fopen("settings.ini", "r");
    if (!f) return;
    char line[1024];
    int in_settings = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *e = line + strlen(line);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_settings = !strncasecmp(s, "[settings]", 10); continue; }
        char *eq = strchr(s, '=');
        if (!in_settings || !eq || strncmp(s, "UG_", 3)) continue;
        *eq = 0;
        if (!getenv(s)) setenv(s, eq + 1, 0);
    }
    fclose(f);
}
#endif

/* Debug lifts (lift.py with UG_LIFT_FNCOUNT=1) call this at every function entry. UG_FNCOUNT=<t1>,<t2> (guest
 * seconds) counts the calls in that window and writes out/fncount.txt (va count, most called first). */
void ug_fncount(uint32_t va) {
    static uint32_t keys[1 << 16], vals[1 << 16];
    static double t1 = -1, t2;
    static int done;
    if (t1 < 0) {
        const char *e = getenv("UG_FNCOUNT");
        if (!e || sscanf(e, "%lf,%lf", &t1, &t2) != 2) { t1 = 1e30; t2 = 0; }
    }
    if (done) return;
    double now = sched_now_100ns() / 1e7;
    if (now < t1) return;
    if (now >= t2) {
        done = 1;
        FILE *f = fopen("out/fncount.txt", "w");
        if (!f) return;
        for (int i = 0; i < (1 << 16); i++) if (keys[i]) fprintf(f, "%X %u\n", keys[i], vals[i]);
        fclose(f);
        LOG("[fncount] written (%.1f..%.1f s)\n", t1, t2);
        return;
    }
    uint32_t h = (va * 2654435761u) >> 16;
    while (keys[h] && keys[h] != va) h = (h + 1) & 0xFFFF;
    keys[h] = va;
    vals[h]++;
}
void updater_start(void);
int main(int argc, char **argv) {
    const char *xbe = argc > 1 ? argv[1] : "game/default.xbe";
#ifdef _WIN32
    if (argc <= 1 && GetFileAttributesA(xbe) == INVALID_FILE_ATTRIBUTES) {   /* installed: run from the exe's folder */
        char dir[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
        while (n && dir[n - 1] != '\\') n--;
        if (n) { dir[n - 1] = 0; SetCurrentDirectoryA(dir); }
    }
#elif defined(__ANDROID__)
    android_start();   /* sdl_plat.c: the app's files folder, log to out/underground.log, window (on the main stack) */
    linux_set_argv(argv);
#else
    if (argc <= 1 && GetFileAttributesA(xbe) == INVALID_FILE_ATTRIBUTES) {   /* installed: run from the exe's folder */
        char dir[MAX_PATH];
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        while (n > 0 && dir[n - 1] != '/') n--;
        if (n > 1) { dir[n - 1] = 0; if (chdir(dir)) {} }
    }
    linux_set_argv(argv);
#endif
    load_settings();
#ifdef __ANDROID__
    setenv("UG_SCALE", "2", 0);   /* phone defaults (settings.ini in the files folder overrides) */
#endif
    CreateDirectoryA("out", NULL);
#ifndef _WIN32
    /* installed: underground.sh sends stderr to out/underground.log */
    updater_start();
    crash_handler_install();
    g_missed = fopen("out/missed_targets.txt", "a");
#else
    if (!GetConsoleWindow() && (GetStdHandle(STD_ERROR_HANDLE) == NULL ||
                                GetStdHandle(STD_ERROR_HANDLE) == INVALID_HANDLE_VALUE))
        freopen("out\\underground.log", "w", stderr);   /* installed build (windows subsystem): log to a file */
    updater_start();   /* updater.c: forge release check (installed builds only) */
    ULONG guarantee = 0x10000;
    SetThreadStackGuarantee(&guarantee);
    AddVectoredExceptionHandler(1, crash_handler);
    g_missed = fopen("out/missed_targets.txt", "a");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    if (getenv("UG_WATCHDOG")) CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)atoi(getenv("UG_WATCHDOG")), 0, NULL);
#endif
    mem_init();
    kernel_init();
    uint32_t entry = xbe_load(xbe);
    LOG("[rt] g_mem %p, entry %08X, kernel thunk %08X\n", g_mem, entry, g_kernel_thunk);
    /* The kernel calls the XBE entry point with no arguments on the initial thread. */
    sched_create_thread(entry, 0, 0, 0, g_xbe_stack < 0x40000 ? 0x40000 : g_xbe_stack, 0, 0);
    sched_run();
    LOG("[rt] all guest threads exited\n");
    return 0;
}
