/* Differential test DLL: lifted code + minimal runtime, driven from Python.
 * Exports oracle_init() -> g_mem, oracle_run(va, ctx, err) -> 0 ok / -1 trap / -2 no func / -3 host fault.
 */
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "x86rt.h"

uint8_t *g_mem;
void ug_fncount(uint32_t va) { (void)va; }

typedef struct { uint32_t va; GuestFunc fn; } FuncEntry;
extern const FuncEntry g_functable[];
extern const uint32_t g_functable_count;

static jmp_buf g_jb;
static char g_err[512];

static GuestFunc lookup(uint32_t va) {
    uint32_t lo = 0, hi = g_functable_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_functable[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    return (lo < g_functable_count && g_functable[lo].va == va) ? g_functable[lo].fn : NULL;
}

void x86_unimpl(X86Context *c, uint32_t va, const char *what) {
    (void)c;
    snprintf(g_err, sizeof g_err, "%08X: %s", va, what);
    longjmp(g_jb, 1);
}

void x86_call_indirect(X86Context *c, uint32_t va) {
    GuestFunc f = lookup(va);
    if (!f) x86_unimpl(c, va, "indirect target not lifted");
    f(c);
}

void x86_int(X86Context *c, uint32_t va, uint32_t vector) { (void)vector; x86_unimpl(c, va, "int"); }
uint32_t x86_in(X86Context *c, uint32_t port, int size) { (void)size; x86_unimpl(c, port, "in"); return 0; }
void x86_out(X86Context *c, uint32_t port, uint32_t v, int size) { (void)v; (void)size; x86_unimpl(c, port, "out"); }
uint32_t x86_eflags_id;
void x86_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    switch (*eax) {
    case 0: *eax = 2; *ebx = 0x756E6547; *edx = 0x49656E69; *ecx = 0x6C65746E; break;   /* "GenuineIntel" */
    case 1: *eax = 0x0683; *ebx = 0; *ecx = 0; *edx = 0x0383F9FF; break;                /* family 6 model 8: MMX SSE */
    case 2: *eax = 0x03020101; *ebx = 0; *ecx = 0; *edx = 0x0C040841; break;            /* cache descriptors */
    default: *eax = *ebx = *ecx = *edx = 0; break;
    }
}
uint64_t x86_rdtsc(X86Context *c) { (void)c; return 0; }
void x86_hle(X86Context *c, uint32_t va) { x86_unimpl(c, va, "hle"); }
void x86_hook(X86Context *c, uint32_t va) { (void)c; (void)va; }
uint32_t mmio_read(uint32_t a, int size) { (void)size; return RD32(a); }
void mmio_write(uint32_t a, uint32_t v, int size) { (void)size; WR32(a, v); }

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
        double m = frexp(fabs(v), &e); /* v = m * 2^e, m in [0.5,1) */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}

__declspec(dllexport) uint8_t *oracle_init(void) {
    if (!g_mem) {
        g_mem = VirtualAlloc(NULL, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
        VirtualAlloc(g_mem, 0x04000000, MEM_COMMIT, PAGE_READWRITE);
    }
    return g_mem;
}

__declspec(dllexport) uint32_t oracle_ctx_size(void) { return (uint32_t)sizeof(X86Context); }

/* Host faults inside lifted code: mingw clang has no __try/__except in C. run_guarded captures the full
 * CPU context (incl. the Win64 callee-saved xmm6-15, which __builtin_longjmp would not restore); a
 * vectored handler that sees a fault inside the guest call resumes that context with g_faulted set,
 * so RtlCaptureContext "returns" a second time with identical registers. */
static CONTEXT g_saved;
static volatile LONG g_in_guest, g_faulted;

static LONG CALLBACK fault_veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (!g_in_guest || r->ExceptionCode == 0x406D1388 /* thread name */ || r->ExceptionCode == DBG_PRINTEXCEPTION_C)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = r->NumberParameters >= 2 ? (uintptr_t)r->ExceptionInformation[1] : 0;
    snprintf(g_err, sizeof g_err, "host exception 0x%08lX at %p, data %p (guest 0x%llX)", r->ExceptionCode,
             r->ExceptionAddress, (void *)addr, (unsigned long long)(addr - (uintptr_t)g_mem));
    g_in_guest = 0;
    g_faulted = 1;
    *ep->ContextRecord = g_saved;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static __attribute__((noinline)) int run_guarded(GuestFunc f, X86Context *c) {
    static int veh;
    if (!veh) { AddVectoredExceptionHandler(1, fault_veh); veh = 1; }
    g_faulted = 0;
    RtlCaptureContext(&g_saved);
    if (g_faulted) return -3;
    g_in_guest = 1;
    f(c);
    g_in_guest = 0;
    return 0;
}

__declspec(dllexport) int oracle_run(uint32_t va, X86Context *c, char *err) {
    GuestFunc f = lookup(va);
    if (!f) return -2;
    g_err[0] = 0;
    int rc;
    if (setjmp(g_jb)) rc = -1;
    else rc = run_guarded(f, c);
    memcpy(err, g_err, sizeof g_err);
    return rc;
}
