/* Cooperative guest threads on host fibers. One host thread runs everything; a guest thread
 * runs until it waits, sleeps or yields. Guest time follows host time. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#ifndef _WIN32
/* Linux: fibers on ucontext, each with an 8 MB lazily committed stack (DXVK, vkd3d's HLSL compiler and the
 * Vulkan driver also run on these stacks: more room than the 256 KB Windows fibers). */
#include <sys/mman.h>
#ifdef __ANDROID__
/* Android's libc has no ucontext functions: a minimal aarch64 switch (callee-saved x19-x30, d8-d15, sp). A new
 * fiber's first switch "returns" into ug_fiber_start, which calls fiber_tramp. */
#define FIBER_STACK (8u << 20)
#define FRAME 176   /* 12 x + 8 d registers = 160 bytes, rounded to keep sp 16-byte aligned */
void ug_ctx_swap(void **save_sp, void *load_sp);
void ug_fiber_start(void);
__asm__(".text\n"
        ".global ug_ctx_swap\n.type ug_ctx_swap,%function\nug_ctx_swap:\n"
        "    sub sp, sp, #176\n"
        "    stp x19, x20, [sp, #0]\n    stp x21, x22, [sp, #16]\n    stp x23, x24, [sp, #32]\n"
        "    stp x25, x26, [sp, #48]\n    stp x27, x28, [sp, #64]\n    stp x29, x30, [sp, #80]\n"
        "    stp d8, d9, [sp, #96]\n    stp d10, d11, [sp, #112]\n    stp d12, d13, [sp, #128]\n"
        "    stp d14, d15, [sp, #144]\n"
        "    mov x2, sp\n    str x2, [x0]\n    mov sp, x1\n"
        "    ldp x19, x20, [sp, #0]\n    ldp x21, x22, [sp, #16]\n    ldp x23, x24, [sp, #32]\n"
        "    ldp x25, x26, [sp, #48]\n    ldp x27, x28, [sp, #64]\n    ldp x29, x30, [sp, #80]\n"
        "    ldp d8, d9, [sp, #96]\n    ldp d10, d11, [sp, #112]\n    ldp d12, d13, [sp, #128]\n"
        "    ldp d14, d15, [sp, #144]\n"
        "    add sp, sp, #176\n    ret\n"
        ".global ug_fiber_start\n.type ug_fiber_start,%function\nug_fiber_start:\n"
        "    mov x29, #0\n    bl ug_fiber_entry\n    brk #0\n");
typedef struct { void *sp; } Fiber;
static Fiber main_fb, *running_fb = &main_fb;
static void fiber_tramp(void);
void ug_fiber_entry(void) { fiber_tramp(); }
static void *ConvertThreadToFiber(void *p) { (void)p; return &main_fb; }
static void *CreateFiber(size_t size, void (*fn)(void *), void *arg) {
    (void)size; (void)fn; (void)arg;   /* fiber_tramp starts sched_current() */
    Fiber *f = calloc(1, sizeof *f);
    uint8_t *stk = mmap(NULL, FIBER_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (!f || stk == MAP_FAILED) return NULL;
    mprotect(stk, 65536, PROT_NONE);   /* guard (covers 4/16/64 KB pages) */
    uint64_t *frame = (uint64_t *)(stk + FIBER_STACK - FRAME);
    memset(frame, 0, FRAME);
    frame[11] = (uint64_t)(uintptr_t)ug_fiber_start;   /* x30 */
    f->sp = frame;
    return f;
}
static void SwitchToFiber(void *to) {
    Fiber *from = running_fb;
    running_fb = to;
    ug_ctx_swap(&from->sp, ((Fiber *)to)->sp);
}
#else
#include <ucontext.h>
static ucontext_t main_uc;
static ucontext_t *running_uc = &main_uc;
static void fiber_tramp(void);
#define FIBER_STACK (8u << 20)
static void *ConvertThreadToFiber(void *p) { (void)p; return &main_uc; }
static void *CreateFiber(size_t size, void (*fn)(void *), void *arg) {
    (void)size; (void)fn; (void)arg;   /* fiber_tramp starts sched_current() */
    ucontext_t *uc = calloc(1, sizeof *uc);
    uint8_t *stk = mmap(NULL, FIBER_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stk == MAP_FAILED) return NULL;
    mprotect(stk, 4096, PROT_NONE);   /* guard page */
    getcontext(uc);
    uc->uc_stack.ss_sp = stk;
    uc->uc_stack.ss_size = FIBER_STACK;
    uc->uc_link = NULL;
    makecontext(uc, fiber_tramp, 0);
    return uc;
}
static void SwitchToFiber(void *f) {
    ucontext_t *from = running_uc;
    running_uc = f;
    swapcontext(from, f);
}
#endif
#endif

/* Guest KPCR layout (subset used by XDK code). */
#define KPCR_SIZE        0x300
#define KPCR_EXCEPTION   0x00
#define KPCR_STACKBASE   0x04
#define KPCR_STACKLIMIT  0x08
#define KPCR_SELF_TIB    0x18
#define KPCR_SELF_PCR    0x1C
#define KPCR_PRCB        0x20
#define KPCR_IRQL        0x24
#define KPCR_CURTHREAD   0x28   /* KPRCB starts here: CurrentThread */
#define KTHREAD_SIZE     0x200
#define KTHREAD_TLSDATA  0x28

enum { T_READY, T_DONE };

struct GThread {
    X86Context ctx;
    void *fiber;
    int state;
    uint32_t kthread, kpcr;
    uint32_t start, arg1, arg2, routine;
    uint32_t exit_status;
    void (*host_fn)(X86Context *c);
    GThread *next;
};

static void *main_fiber;
static GThread *threads, *current;
static LARGE_INTEGER qpf, qpc0;

uint64_t sched_now_100ns(void) {
    LARGE_INTEGER t;
    if (!qpf.QuadPart) { QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qpc0); }
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)(t.QuadPart - qpc0.QuadPart) * 1e7 / (double)qpf.QuadPart);
}

GThread *sched_current(void) { return current; }

/* Guest threads only switch when they wait; a thread that polls (input, timers) without waiting
 * would starve the others. Polling points call this to yield after 1 ms of continuous running. */
static uint64_t slice_start;
void sched_maybe_yield(void) {
    /* called on every MMIO read, kernel and HLE call: read the clock only every 32nd time (it was ~10% of the CPU
     * on a phone in busy scenes) */
    static uint32_t n;
    if (!current || (++n & 31)) return;
    if (sched_now_100ns() - slice_start >= 10000) ug_sched_yield();
}
uint32_t sched_thread_object(GThread *t) { return t->kthread; }

static void WINAPI fiber_main(void *p) {
    GThread *t = p;
    X86Context *c = &t->ctx;
    if (t->host_fn) {
        t->host_fn(c);
        sched_exit_thread(0);
    }
    /* Guest call: routine(arg1, arg2) stdcall, or start(arg2) when there is no routine. */
    if (t->routine) {
        c->esp -= 4; WR32(c->esp, t->arg2);
        c->esp -= 4; WR32(c->esp, t->arg1);
        c->esp -= 4; WR32(c->esp, THREAD_EXIT_VA);
        x86_call_indirect(c, t->routine);
    } else {
        c->esp -= 4; WR32(c->esp, t->arg2);
        c->esp -= 4; WR32(c->esp, THREAD_EXIT_VA);
        x86_call_indirect(c, t->start);
    }
    sched_exit_thread(c->eax);
}
#ifndef _WIN32
static void fiber_tramp(void) { fiber_main(sched_current()); }
#endif

GThread *sched_create_thread(uint32_t start, uint32_t arg1, uint32_t arg2, uint32_t routine, uint32_t stack_size,
                             uint32_t tls_size, int suspended) {
    (void)suspended;
    GThread *t = calloc(1, sizeof *t);
    if (stack_size < 0x10000) stack_size = 0x10000;
    uint32_t stack = mem_alloc(stack_size, 0x1000);
    if (!stack) rt_fatal("no memory for thread stack");
    t->kthread = karea_alloc(KTHREAD_SIZE);
    t->kpcr = karea_alloc(KPCR_SIZE);
    WR8(t->kthread, 6); /* DISPATCHER_HEADER.Type = ThreadObject */
    WR8(t->kthread + 2, KTHREAD_SIZE / 4);
    if (tls_size) {
        uint32_t tls = mem_alloc(tls_size + 16, 16);
        WR32(t->kthread + KTHREAD_TLSDATA, tls);
    }
    WR32(t->kpcr + KPCR_EXCEPTION, 0xFFFFFFFF);
    WR32(t->kpcr + KPCR_STACKBASE, stack + stack_size);
    WR32(t->kpcr + KPCR_STACKLIMIT, stack);
    WR32(t->kpcr + KPCR_SELF_TIB, t->kpcr);
    WR32(t->kpcr + KPCR_SELF_PCR, t->kpcr);
    WR32(t->kpcr + KPCR_PRCB, t->kpcr + KPCR_CURTHREAD);
    WR32(t->kpcr + KPCR_CURTHREAD, t->kthread);
    t->ctx.esp = stack + stack_size - 16;
    t->ctx.fs_base = t->kpcr;
    t->ctx.fcw = 0x027F;
    t->ctx.mxcsr = 0x1F80;
    t->start = start;
    t->arg1 = arg1;
    t->arg2 = arg2;
    t->routine = routine;
    t->fiber = CreateFiber(256 * 1024, fiber_main, t);
    if (!t->fiber) rt_fatal("CreateFiber failed");
    /* append to keep creation order */
    GThread **pp = &threads;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    LOG("[sched] thread %08X start=%08X routine=%08X stack=%08X+%X\n", t->kthread, start, routine, stack, stack_size);
    return t;
}

GThread *sched_create_host_thread(void (*fn)(X86Context *c)) {
    GThread *t = sched_create_thread(0, 0, 0, 0, 0x10000, 0, 0);
    t->host_fn = fn;
    return t;
}

void guest_call(X86Context *c, uint32_t va, int nargs, const uint32_t *args) {
    for (int i = nargs - 1; i >= 0; i--) { c->esp -= 4; WR32(c->esp, args[i]); }
    c->esp -= 4;
    WR32(c->esp, THREAD_EXIT_VA);
    x86_call_indirect(c, va);
}

void ug_sched_yield(void) {
    SwitchToFiber(main_fiber);
}

_Noreturn void sched_exit_thread(uint32_t status) {
    current->state = T_DONE;
    current->exit_status = status;
    WR32(current->kthread + 4, 1); /* SignalState: thread objects signal on exit */
    LOG("[sched] thread %08X exited (%u)\n", current->kthread, status);
    SwitchToFiber(main_fiber);
    rt_fatal("dead thread resumed");
}

/* A round in which every thread only polled (waits, the frame-end wait) takes a few microseconds; then the host
 * thread sleeps a quarter millisecond instead of spinning a whole core (heat and battery on phones, and the frame
 * pacing does not need more: the game clock has 1 ms steps). UG_NOIDLE=1 keeps the old spin. */
#ifdef _WIN32
static void idle_sleep(void) {
    static HANDLE timer;
    static int init;
    if (!init) {
        init = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
    }
    if (!timer) { Sleep(0); return; }
    LARGE_INTEGER due;
    due.QuadPart = -2500;   /* 250 us, relative, 100 ns units */
    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
}
#else
#include <time.h>
static void idle_sleep(void) {
    struct timespec ts = {0, 250000};
    nanosleep(&ts, NULL);
}
#endif

void sched_run(void) {
    sched_now_100ns(); /* start the clock */
    int idle_ok = !(getenv("UG_NOIDLE") && *getenv("UG_NOIDLE") == '1');
    main_fiber = ConvertThreadToFiber(NULL);
    for (;;) {
        int alive = 0;
        uint64_t round_start = sched_now_100ns();
        for (GThread *t = threads; t; t = t->next) {
            if (t->state == T_DONE) continue;
            alive = 1;
            current = t;
            slice_start = sched_now_100ns();
            SwitchToFiber(t->fiber);
            current = NULL;
            {   /* UG_SLICELOG=<ms>: log guest slices longer than ms (threads that do not reach a yield point) */
                static int slicelog = -1;
                if (slicelog < 0) { const char *e = getenv("UG_SLICELOG"); slicelog = e ? atoi(e) : 0; }
                uint64_t d = sched_now_100ns() - slice_start;
                if (slicelog && d >= (uint64_t)slicelog * 10000)
                    LOG("[sched] thread %08X ran %.1f ms without yielding (t=%.1f s)\n", t->kthread, d / 1e4,
                        sched_now_100ns() / 1e7);
            }
        }
        if (!alive) return;
        kernel_poll();
#ifndef _WIN32
        linux_pump_events();   /* SDL events belong to this (main) thread; the Windows build has a window thread */
#endif
#ifdef __ANDROID__
        android_pad_poll();   /* SDL may call into Java: only from this (the thread's own) stack */
        if (g_android_relaunch) android_relaunch();
#endif
        uint64_t round_len = sched_now_100ns() - round_start;
        if (idle_ok && round_len < 400) idle_sleep();   /* < 40 us: nothing but polling */
        else Sleep(0);
        {   /* UG_ROUNDLOG=1: scheduler rounds per second, idle sleeps, round length histogram */
            static int on = -1; static uint64_t next, n, idle, h[6];
            if (on < 0) on = getenv("UG_ROUNDLOG") != NULL;
            if (on) {
                n++; idle += round_len < 400;
                h[round_len < 100 ? 0 : round_len < 400 ? 1 : round_len < 1000 ? 2 : round_len < 10000 ? 3 : round_len < 50000 ? 4 : 5]++;
                uint64_t now = sched_now_100ns();
                if (now >= next) {
                    if (next) LOG("[sched] %llu rounds/s, %llu idle; <10us %llu <40us %llu <100us %llu <1ms %llu <5ms %llu more %llu\n",
                                  n / 5, idle / 5, h[0], h[1], h[2], h[3], h[4], h[5]);
                    next = now + 50000000; n = idle = 0; memset(h, 0, sizeof h);
                }
            }
        }
    }
}

#ifdef __ANDROID__
volatile int g_android_relaunch;
#endif

void sched_sleep_until(uint64_t t100ns) {
    while (sched_now_100ns() < t100ns) ug_sched_yield();
}
