/* xboxkrnl HLE. Kernel imports resolve to sentinel VAs (KSENTINEL_BASE + ordinal*4) or to
 * data objects in the kernel area. kernel_call() runs the handler and emulates the return
 * (stdcall/fastcall pop, cdecl leaves args). */
#include <windows.h>
#include <stdlib.h>
#include "rt.h"
#include "kexports.h"

int g_log_kernel;

#undef STATUS_TIMEOUT
#undef STATUS_PENDING
#undef STATUS_INVALID_HANDLE
#undef STATUS_NO_MEMORY
#define STATUS_SUCCESS          0x00000000u
#define STATUS_TIMEOUT          0x00000102u
#define STATUS_PENDING          0x00000103u
#define STATUS_UNSUCCESSFUL     0xC0000001u
#define STATUS_NOT_IMPLEMENTED  0xC0000002u
#define STATUS_INVALID_HANDLE   0xC0000008u
#define STATUS_NO_MEMORY        0xC0000017u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u

/* dispatcher object types */
enum { NotificationEvent = 0, SynchronizationEvent = 1, MutantObject = 2, SemaphoreObject = 5, ThreadObject = 6,
       NotificationTimer = 8, SynchronizationTimer = 9 };

/* ---------------------------------------------------------------- data exports */
static uint32_t data_va[379];

static void init_data_exports(void) {
    uint32_t a;
    /* KeTickCount (ULONG, ms) */
    data_va[156] = karea_alloc(4);
    /* XboxHardwareInfo: Flags, GpuRevision, McpRevision, ... */
    data_va[322] = a = karea_alloc(8);
    WR32(a, 0x00000020); WR8(a + 4, 0xA1); WR8(a + 5, 0xB1);
    data_va[323] = karea_alloc(16);   /* XboxHDKey */
    data_va[325] = karea_alloc(16);   /* XboxSignatureKey */
    data_va[354] = karea_alloc(16 * 16); /* XboxAlternateSignatureKeys */
    data_va[40] = karea_alloc(4);     /* HalDiskCachePartitionCount */
    WR32(data_va[40], 3);
    data_va[16] = karea_alloc(64);    /* ExEventObjectType */
    data_va[356] = karea_alloc(4);    /* HalBootSMCVideoMode */
    /* LaunchDataPage: pointer variable, NULL = cold boot */
    data_va[164] = karea_alloc(4);
    /* XboxKrnlVersion {Major, Minor, Build, Qfe}: 1.0.5838.1 (a retail kernel). XAPI 5659 patches kernel code
     * for builds 3944/4034/4039/4816-4818 only; any other build skips that. */
    data_va[324] = a = karea_alloc(8);
    WR16(a, 1); WR16(a + 2, 0); WR16(a + 4, 5838); WR16(a + 6, 1);
    /* XeImageFileName: ANSI_STRING {Length, MaximumLength, Buffer} */
    {
        static const char img[] = "\\Device\\CdRom0\\default.xbe";
        uint32_t buf = karea_alloc(sizeof img);
        memcpy(g_mem + buf, img, sizeof img);
        data_va[326] = a = karea_alloc(8);
        WR16(a, (uint16_t)(sizeof img - 1)); WR16(a + 2, (uint16_t)sizeof img); WR32(a + 4, buf);
    }
    /* HalDiskModelNumber / HalDiskSerialNumber: STRING like XeImageFileName */
    {
        static const char model[] = "PROJECT UNDERGROUND HDD", serial[] = "0000000000000000";
        uint32_t m = karea_alloc(sizeof model), sn = karea_alloc(sizeof serial);
        memcpy(g_mem + m, model, sizeof model);
        memcpy(g_mem + sn, serial, sizeof serial);
        data_va[41] = a = karea_alloc(8);
        WR16(a, (uint16_t)(sizeof model - 1)); WR16(a + 2, (uint16_t)sizeof model); WR32(a + 4, m);
        data_va[42] = a = karea_alloc(8);
        WR16(a, (uint16_t)(sizeof serial - 1)); WR16(a + 2, (uint16_t)sizeof serial); WR32(a + 4, sn);
    }
    data_va[353] = karea_alloc(16);   /* XboxLANKey */
    data_va[355] = karea_alloc(284);  /* XePublicKeyData (RSA public key; only used by signature checks) */
    data_va[357] = karea_alloc(64);   /* IdexChannelObject */
}

uint32_t kernel_data_export(uint32_t ordinal) {
    if (ordinal < 379 && k_exports[ordinal].conv == KC_DATA) {
        if (!data_va[ordinal]) {
            data_va[ordinal] = karea_alloc(64);
            LOG("[kernel] data export %u %s: zero placeholder\n", ordinal, k_exports[ordinal].name);
        }
        return data_va[ordinal];
    }
    return 0;
}

/* ---------------------------------------------------------------- handles */
#define MAX_HANDLES 4096
static Handle handles[MAX_HANDLES];

uint32_t handle_new(int kind, uint32_t obj, void *host) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i].used) {
            handles[i] = (Handle){1, kind, obj, host};
            return (uint32_t)i * 4;
        }
    }
    rt_fatal("out of handles");
}

Handle *handle_get(uint32_t h) {
    uint32_t i = h / 4;
    if ((h & 3) || i == 0 || i >= MAX_HANDLES || !handles[i].used) return NULL;
    return &handles[i];
}

/* An object pointer for waits: handle -> kernel object; thread handle -> KTHREAD. */
uint32_t handle_object(uint32_t h) {
    if (h == 0xFFFFFFFE) return sched_thread_object(sched_current()); /* NtCurrentThread() */
    Handle *p = handle_get(h);
    return p ? p->obj : 0;
}

/* ---------------------------------------------------------------- waits */
static int obj_signaled(uint32_t obj) {
    uint8_t type = RD8(obj);
    int32_t sig = (int32_t)RD32(obj + 4);
    if (type == MutantObject) {
        return sig > 0 || RD32(obj + 16) == sched_thread_object(sched_current());
    }
    return sig > 0;
}

static void obj_consume(uint32_t obj) {
    uint8_t type = RD8(obj);
    switch (type) {
    case SynchronizationEvent:
    case SynchronizationTimer:
        WR32(obj + 4, 0);
        break;
    case SemaphoreObject:
        WR32(obj + 4, RD32(obj + 4) - 1);
        break;
    case MutantObject:
        WR32(obj + 4, RD32(obj + 4) - 1);
        WR32(obj + 16, sched_thread_object(sched_current()));
        break;
    default:
        break;
    }
}

static uint32_t kernel_wait_inner(uint32_t *objects, int count, int wait_any, int64_t timeout, int has_timeout);

/* UG_WAITDUMP=<s>: every s seconds, list guest threads blocked in a kernel wait for more than 2 s (deadlock hunting):
 * thread, objects (type, signal state), and the guest return addresses on its stack. */
typedef struct { uint32_t kthread, objs[4], n, esp; uint64_t since; int active; } WaitRec;
static WaitRec waits[64];
static void wait_dump(void) {
    static int period = -1;
    static uint64_t next;
    if (period < 0) { const char *e = getenv("UG_WAITDUMP"); period = e ? atoi(e) : 0; }
    if (!period) return;
    uint64_t now = sched_now_100ns();
    if (now < next) return;
    next = now + (uint64_t)period * 10000000ull;
    for (int i = 0; i < 64; i++) {
        WaitRec *w = &waits[i];
        if (!w->active || now - w->since < 20000000ull) continue;
        char buf[512];
        int k = snprintf(buf, sizeof buf, "[wait] thread %08X %.1f s:", w->kthread, (now - w->since) / 1e7);
        for (uint32_t j = 0; j < w->n && j < 4; j++)
            k += snprintf(buf + k, sizeof buf - k, " %08X(t%u s%d)", w->objs[j], RD8(w->objs[j]), (int32_t)RD32(w->objs[j] + 4));
        k += snprintf(buf + k, sizeof buf - k, " stack");
        for (uint32_t a = w->esp, m = 0; a < w->esp + 0x400 && m < 10; a += 4) {
            uint32_t v = RD32(a);
            if (v >= 0x11000 && v < 0x205000 && g_mem[v - 5] == 0xE8) { k += snprintf(buf + k, sizeof buf - k, " %X", v); m++; }
        }
        LOG("%s\n", buf);
    }
}

/* Returns STATUS_WAIT_0 + index, or STATUS_TIMEOUT. timeout_100ns: relative (<0) or absolute (>=0). */
uint32_t kernel_wait(uint32_t *objects, int count, int wait_any, int64_t timeout, int has_timeout) {
    WaitRec *wr = NULL;
    if (sched_current()) {
        uint32_t kt = sched_thread_object(sched_current());
        for (int i = 0; i < 64 && !wr; i++) if (waits[i].kthread == kt || !waits[i].kthread) wr = &waits[i];
        if (wr) {
            wr->kthread = kt; wr->n = (uint32_t)count; wr->since = sched_now_100ns(); wr->active = 1;
            wr->esp = ((X86Context *)sched_current())->esp;
            for (int j = 0; j < count && j < 4; j++) wr->objs[j] = objects[j];
        }
    }
    uint32_t r = kernel_wait_inner(objects, count, wait_any, timeout, has_timeout);
    if (wr) wr->active = 0;
    return r;
}

static uint32_t kernel_wait_inner(uint32_t *objects, int count, int wait_any, int64_t timeout, int has_timeout) {
    uint64_t deadline = 0;
    if (has_timeout) {
        deadline = timeout < 0 ? sched_now_100ns() + (uint64_t)(-timeout) : (uint64_t)timeout; /* absolute ~ boot time */
    }
    for (;;) {
        if (wait_any) {
            for (int i = 0; i < count; i++) {
                if (obj_signaled(objects[i])) {
                    obj_consume(objects[i]);
                    return (uint32_t)i;
                }
            }
        } else {
            int all = 1;
            for (int i = 0; i < count; i++) all &= obj_signaled(objects[i]);
            if (all) {
                for (int i = 0; i < count; i++) obj_consume(objects[i]);
                return 0;
            }
        }
        if (has_timeout && sched_now_100ns() >= deadline) return STATUS_TIMEOUT;
        wait_dump();
        ug_sched_yield();
    }
}

static int read_timeout(uint32_t p, int64_t *out) {
    if (!p) return 0;
    *out = (int64_t)RD64(p);
    return 1;
}

/* ---------------------------------------------------------------- handlers */
static KHandler handlers[379];
#define K(ord, name) static uint32_t k_##name(X86Context *c)
#define REG(ord, name) handlers[ord] = k_##name

static uint8_t irql;

/* memory */
K(14, ExAllocatePool) { return mem_alloc(ARG(0), 16); }
K(15, ExAllocatePoolWithTag) { return mem_alloc(ARG(0), 16); }
K(17, ExFreePool) { mem_free(ARG(0)); return 0; }
K(23, ExQueryPoolBlockSize) { return mem_size(ARG(0)); }
K(165, MmAllocateContiguousMemory) {
    uint32_t a = mem_alloc(ARG(0), 0x1000);
    return a ? a | RAM_ALIAS : 0;
}
K(166, MmAllocateContiguousMemoryEx) {
    /* (NumberOfBytes, LowestAcceptable, HighestAcceptable, Alignment, ProtectionType) */
    uint32_t align = ARG(3) < 0x1000 ? 0x1000 : ARG(3);
    uint32_t a = mem_alloc(ARG(0), align);
    return a ? a | RAM_ALIAS : 0;
}
K(171, MmFreeContiguousMemory) { mem_free(ARG(0)); return 0; }
K(180, MmQueryAllocationSize) { return mem_size(ARG(0)); }
K(173, MmGetPhysicalAddress) { return ARG(0) & (RAM_SIZE - 1); }
K(178, MmPersistContiguousMemory) { return 0; }
K(175, MmLockUnlockBufferPages) { return 0; }
K(176, MmLockUnlockPhysicalPage) { return 0; }
K(168, MmClaimGpuInstanceMemory) {
    /* (NumberOfBytes, *NumberOfPaddingBytes): return end of the instance memory (top of RAM) */
    if (ARG(1)) WR32(ARG(1), 0);
    return RAM_ALIAS + RAM_SIZE;
}
K(184, NtAllocateVirtualMemory) {
    /* (*BaseAddress, ZeroBits, *RegionSize, AllocationType, Protect) */
    uint32_t base = RD32(ARG(0)), size = RD32(ARG(2));
    uint32_t a;
    if (base) {
        a = base; /* commit inside an earlier reservation: memory is already backed */
    } else {
        a = mem_alloc((size + 0xFFF) & ~0xFFFu, 0x10000);
        if (!a) return STATUS_NO_MEMORY;
    }
    WR32(ARG(0), a);
    WR32(ARG(2), (size + 0xFFF) & ~0xFFFu);
    return STATUS_SUCCESS;
}
K(199, NtFreeVirtualMemory) {
    /* (*BaseAddress, *RegionSize, FreeType): MEM_RELEASE = 0x8000 */
    if (ARG(2) & 0x8000) mem_free(RD32(ARG(0)));
    return STATUS_SUCCESS;
}
K(217, NtQueryVirtualMemory) {
    /* (BaseAddress, MEMORY_BASIC_INFORMATION*) */
    uint32_t a = ARG(0), mbi = ARG(1);
    WR32(mbi + 0, a & ~0xFFFu);
    WR32(mbi + 4, a & ~0xFFFu);
    WR32(mbi + 8, 4);           /* PAGE_READWRITE */
    WR32(mbi + 12, 0x1000);
    WR32(mbi + 16, 0x1000);     /* MEM_COMMIT */
    WR32(mbi + 20, 4);
    WR32(mbi + 24, 0x20000);    /* MEM_PRIVATE */
    return STATUS_SUCCESS;
}

/* time */
K(128, KeQuerySystemTime) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    WR32(ARG(0), ft.dwLowDateTime);
    WR32(ARG(0) + 4, ft.dwHighDateTime);
    return 0;
}
K(125, KeQueryInterruptTime) {
    uint64_t t = sched_now_100ns();
    c->edx = (uint32_t)(t >> 32);
    return (uint32_t)t;
}
K(99, KeDelayExecutionThread) {
    /* (WaitMode, Alertable, *Interval) */
    int64_t t = (int64_t)RD64(ARG(2));
    uint64_t until = t < 0 ? sched_now_100ns() + (uint64_t)(-t) : (uint64_t)t;
    if (t == 0) ug_sched_yield();
    else sched_sleep_until(until);
    return STATUS_SUCCESS;
}
K(151, KeStallExecutionProcessor) { Sleep(0); return 0; }
K(305, RtlTimeToTimeFields) {
    uint64_t t = RD64(ARG(0));
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    uint32_t f = ARG(1);
    WR16(f + 0, st.wYear); WR16(f + 2, st.wMonth); WR16(f + 4, st.wDay); WR16(f + 6, st.wHour);
    WR16(f + 8, st.wMinute); WR16(f + 10, st.wSecond); WR16(f + 12, st.wMilliseconds); WR16(f + 14, st.wDayOfWeek);
    return 0;
}

/* IRQL */
K(129, KeRaiseIrqlToDpcLevel) { uint8_t old = irql; irql = 2; return old; }
K(160, KfRaiseIrql) { uint8_t old = irql; irql = (uint8_t)c->ecx; return old; }
K(161, KfLowerIrql) { irql = (uint8_t)c->ecx; return 0; }

/* events and waits */
K(189, NtCreateEvent) {
    /* (*EventHandle, ObjectAttributes, EventType, InitialState) */
    uint32_t ev = karea_alloc(16);
    WR8(ev, (uint8_t)ARG(2));
    WR8(ev + 2, 4);
    WR32(ev + 4, ARG(3) ? 1 : 0);
    WR32(ARG(0), handle_new(H_OBJECT, ev, NULL));
    return STATUS_SUCCESS;
}
static uint32_t set_event(uint32_t ev, uint32_t prev_ptr) {
    uint32_t prev = RD32(ev + 4);
    WR32(ev + 4, 1);
    if (prev_ptr) WR32(prev_ptr, prev);
    return prev;
}
K(225, NtSetEvent) {
    uint32_t ev = handle_object(ARG(0));
    if (!ev) return STATUS_INVALID_HANDLE;
    set_event(ev, ARG(1));
    return STATUS_SUCCESS;
}
K(186, NtClearEvent) {
    uint32_t ev = handle_object(ARG(0));
    if (!ev) return STATUS_INVALID_HANDLE;
    WR32(ev + 4, 0);
    return STATUS_SUCCESS;
}
K(205, NtPulseEvent) {
    uint32_t ev = handle_object(ARG(0));
    if (!ev) return STATUS_INVALID_HANDLE;
    if (ARG(1)) WR32(ARG(1), RD32(ev + 4));
    WR32(ev + 4, 1);
    ug_sched_yield(); /* let waiters see it */
    WR32(ev + 4, 0);
    return STATUS_SUCCESS;
}
K(145, KeSetEvent) { return set_event(ARG(0), 0); }
K(159, KeWaitForSingleObject) {
    /* (Object, WaitReason, WaitMode, Alertable, *Timeout) */
    uint32_t obj = ARG(0);
    int64_t t;
    int ht = read_timeout(ARG(4), &t);
    return kernel_wait(&obj, 1, 1, t, ht);
}
K(158, KeWaitForMultipleObjects) {
    /* (Count, Object[], WaitType(0=All,1=Any), WaitReason, WaitMode, Alertable, *Timeout, WaitBlockArray) */
    uint32_t objs[64];
    uint32_t n = ARG(0) > 64 ? 64 : ARG(0);
    for (uint32_t i = 0; i < n; i++) objs[i] = RD32(ARG(1) + 4 * i);
    int64_t t;
    int ht = read_timeout(ARG(6), &t);
    return kernel_wait(objs, (int)n, ARG(2) == 1, t, ht);
}
K(233, NtWaitForSingleObject) {
    /* (Handle, Alertable, *Timeout) */
    uint32_t obj = handle_object(ARG(0));
    if (!obj) return STATUS_INVALID_HANDLE;
    int64_t t;
    int ht = read_timeout(ARG(2), &t);
    return kernel_wait(&obj, 1, 1, t, ht);
}
K(234, NtWaitForSingleObjectEx) {
    /* (Handle, WaitMode, Alertable, *Timeout) */
    uint32_t obj = handle_object(ARG(0));
    if (!obj) return STATUS_INVALID_HANDLE;
    int64_t t;
    int ht = read_timeout(ARG(3), &t);
    return kernel_wait(&obj, 1, 1, t, ht);
}
K(187, NtClose) {
    Handle *h = handle_get(ARG(0));
    if (!h) return STATUS_INVALID_HANDLE;
    if (h->kind == H_FILE) fs_close(h);
    h->used = 0;
    return STATUS_SUCCESS;
}
K(246, ObReferenceObjectByHandle) {
    /* (Handle, ObjectType, *ReturnedObject) */
    uint32_t obj = handle_object(ARG(0));
    if (!obj) return STATUS_INVALID_HANDLE;
    WR32(ARG(2), obj);
    return STATUS_SUCCESS;
}
K(250, ObfDereferenceObject) { return 0; }

/* critical sections: { DISPATCHER_HEADER Event (16); LockCount; RecursionCount; OwningThread } */
K(291, RtlInitializeCriticalSection) {
    uint32_t cs = ARG(0);
    memset(g_mem + cs, 0, 28);
    WR8(cs, SynchronizationEvent);
    WR8(cs + 2, 4);
    WR32(cs + 16, 0xFFFFFFFF);
    return 0;
}
K(277, RtlEnterCriticalSection) {
    uint32_t cs = ARG(0), me = sched_thread_object(sched_current());
    int32_t lock = (int32_t)RD32(cs + 16) + 1;
    WR32(cs + 16, (uint32_t)lock);
    if (lock == 0) {
        WR32(cs + 24, me);
        WR32(cs + 20, 1);
    } else if (RD32(cs + 24) == me) {
        WR32(cs + 20, RD32(cs + 20) + 1);
    } else {
        kernel_wait(&cs, 1, 1, 0, 0);
        WR32(cs + 24, me);
        WR32(cs + 20, 1);
    }
    return 0;
}
K(294, RtlLeaveCriticalSection) {
    uint32_t cs = ARG(0);
    uint32_t rec = RD32(cs + 20) - 1;
    WR32(cs + 20, rec);
    if (rec == 0) {
        WR32(cs + 24, 0);
        int32_t lock = (int32_t)RD32(cs + 16) - 1;
        WR32(cs + 16, (uint32_t)lock);
        if (lock >= 0) WR32(cs + 4, 1); /* wake one waiter */
    } else {
        WR32(cs + 16, RD32(cs + 16) - 1);
    }
    return 0;
}

/* threads */
K(255, PsCreateSystemThreadEx) {
    /* (*ThreadHandle, ThreadExtraSize, KernelStackSize, TlsDataSize, *ThreadId, StartContext1,
     *  StartContext2, CreateSuspended, DebuggerThread, SystemRoutine) */
    GThread *t = sched_create_thread(ARG(5), ARG(5), ARG(6), ARG(9), ARG(2) ? ARG(2) : 0x40000, ARG(3), ARG(7));
    uint32_t kt = sched_thread_object(t);
    if (ARG(0)) WR32(ARG(0), handle_new(H_OBJECT, kt, NULL));
    if (ARG(4)) WR32(ARG(4), kt);
    return STATUS_SUCCESS;
}
K(258, PsTerminateSystemThread) { sched_exit_thread(ARG(0)); }

/* Rtl misc */
K(289, RtlInitAnsiString) {
    /* (STRING* Dest, PCSZ Source): Length, MaximumLength, Buffer */
    uint32_t d = ARG(0), s = ARG(1);
    uint32_t n = s ? (uint32_t)strlen((char *)g_mem + s) : 0;
    WR16(d, (uint16_t)n);
    WR16(d + 2, (uint16_t)(s ? n + 1 : 0));
    WR32(d + 4, s);
    return 0;
}
K(279, RtlEqualString) {
    uint32_t a = ARG(0), b = ARG(1);
    uint16_t la = RD16(a), lb = RD16(b);
    if (la != lb) return 0;
    const char *pa = (char *)g_mem + RD32(a + 4), *pb = (char *)g_mem + RD32(b + 4);
    return ARG(2) ? _strnicmp(pa, pb, la) == 0 : memcmp(pa, pb, la) == 0;
}
K(269, RtlCompareMemoryUlong) {
    uint32_t p = ARG(0), n = ARG(1) & ~3u, v = ARG(2), i = 0;
    while (i < n && RD32(p + i) == v) i += 4;
    return i;
}
K(301, RtlNtStatusToDosError) {
    uint32_t s = ARG(0);
    return s == 0 ? 0 : s == STATUS_OBJECT_NAME_NOT_FOUND ? 2 : s == STATUS_NO_MEMORY ? 8 : 31;
}

/* system */
K(8, DbgPrint) {
    LOG("[DbgPrint] %s", (char *)g_mem + ARG(0));
    return 0;
}
/* HalReturnToFirmware(routine): 0 halt, 1 reboot, 2 quick reboot, 3 hard reboot. XAPI quick-reboots on the first
 * boot after it prepares the hard disk partitions (a fresh out\hdd): relaunch this exe the way the console relaunches
 * the title. UG_INPUT runs just exit (a relaunch would replay the script). */
K(49, HalReturnToFirmware) {
    LOG("[kernel] HalReturnToFirmware(%u)\n", ARG(0));
    if (ARG(0) >= 1 && ARG(0) <= 3 && !getenv("UG_INPUT")) {
#ifndef _WIN32
        linux_relaunch();
#else
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
#endif
    }
    ExitProcess(0);
}
K(360, HalInitiateShutdown) { LOG("[kernel] HalInitiateShutdown\n"); ExitProcess(0); }
K(95, KeBugCheck) { rt_fatal("KeBugCheck(%08X)", ARG(0)); }

/* crypto: SHA-1 and HMAC-SHA1, used by XAPI to sign save games (XCalculateSignature*).
 * The guest's A_SHA_CTX (0x74 bytes) holds our state: h[5], byte count (u64), 64-byte block buffer. */
typedef struct { uint32_t h[5]; uint64_t n; uint8_t buf[64]; } Sha1;

static uint32_t rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80], a = s->h[0], b = s->h[1], cc = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += cc; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) {
    static const uint32_t h0[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, h0, sizeof h0);
    s->n = 0;
}
static void sha1_update(Sha1 *s, const uint8_t *p, uint32_t len) {
    while (len--) {
        s->buf[s->n++ & 63] = *p++;
        if ((s->n & 63) == 0) sha1_block(s, s->buf);
    }
}
static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->n * 8;
    uint8_t pad = 0x80, z = 0, len[8];
    sha1_update(s, &pad, 1);
    while ((s->n & 63) != 56) sha1_update(s, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

K(335, XcSHAInit) { Sha1 s; sha1_init(&s); memcpy(g_mem + ARG(0), &s, sizeof s); return 0; }
K(336, XcSHAUpdate) {
    Sha1 s;
    memcpy(&s, g_mem + ARG(0), sizeof s);
    sha1_update(&s, g_mem + ARG(1), ARG(2));
    memcpy(g_mem + ARG(0), &s, sizeof s);
    return 0;
}
K(337, XcSHAFinal) {
    Sha1 s;
    memcpy(&s, g_mem + ARG(0), sizeof s);
    sha1_final(&s, g_mem + ARG(1));
    return 0;
}
K(340, XcHMAC) {
    /* (key, keylen, data1, len1, data2, len2, digest): HMAC-SHA1 over data1 || data2 */
    uint8_t k[64] = {0}, pad[64], inner[20];
    uint32_t kl = ARG(1);
    Sha1 s;
    if (kl > 64) { sha1_init(&s); sha1_update(&s, g_mem + ARG(0), kl); sha1_final(&s, k); }
    else memcpy(k, g_mem + ARG(0), kl);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&s); sha1_update(&s, pad, 64);
    if (ARG(2)) sha1_update(&s, g_mem + ARG(2), ARG(3));
    if (ARG(4)) sha1_update(&s, g_mem + ARG(4), ARG(5));
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    sha1_init(&s); sha1_update(&s, pad, 64); sha1_update(&s, inner, 20);
    sha1_final(&s, g_mem + ARG(6));
    return 0;
}
K(327, XeLoadSection) { WR32(ARG(0) + 8, RD32(ARG(0) + 8) + 1); return STATUS_SUCCESS; } /* sections preloaded */
K(328, XeUnloadSection) { return STATUS_SUCCESS; }
K(24, ExQueryNonVolatileSetting) {
    /* (ValueIndex, *Type, Value, ValueLength, *ResultLength) */
    uint32_t idx = ARG(0), v = 0;
    switch (idx) {
    case 7: v = 1; break;            /* XC_LANGUAGE: English */
    case 8: v = 0x00400000; break;   /* XC_VIDEO: widescreen off, 480p allowed */
    case 9: v = 0; break;            /* XC_AUDIO */
    case 0x103: v = 1; break;        /* XC_FACTORY_GAME_REGION: NA */
    case 0x104: v = 0x00400100; break; /* XC_FACTORY_AV_REGION: NTSC-M */
    default: break;
    }
    if (ARG(1)) WR32(ARG(1), 4);     /* REG_DWORD */
    if (ARG(2) && ARG(3) >= 4) WR32(ARG(2), v);
    if (ARG(4)) WR32(ARG(4), 4);
    return STATUS_SUCCESS;
}
K(1, AvGetSavedDataAddress) { return 0; }
K(2, AvSendTVEncoderOption) { if (ARG(3)) WR32(ARG(3), 0); return 0; }
K(3, AvSetDisplayMode) { return 0; }
K(4, AvSetSavedDataAddress) { return 0; }
K(47, HalRegisterShutdownNotification) { return 0; }
K(44, HalGetInterruptVector) { if (ARG(1)) WR8(ARG(1), 0); return ARG(0) + 0x30; }
/* KINTERRUPT: ServiceRoutine(+0) ServiceContext(+4) BusInterruptLevel(+8) Irql(+12) Connected(+16) ShareVector(+17)
 * Mode(+18). Connected interrupts are raised by device emulation (kernel_raise_irq) and run on the DPC thread. */
static uint32_t irq_objects[32];
static volatile uint32_t irq_pending;
K(109, KeInitializeInterrupt) {
    /* (Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, InterruptMode, ShareVector) */
    uint32_t i = ARG(0);
    WR32(i, ARG(1));
    WR32(i + 4, ARG(2));
    WR32(i + 8, (ARG(3) - 0x30) & 31);
    WR32(i + 12, ARG(4) & 0xFF);
    WR8(i + 16, 0);
    WR8(i + 17, (uint8_t)ARG(6));
    WR8(i + 18, (uint8_t)ARG(5));
    return 0;
}
K(98, KeConnectInterrupt) {
    uint32_t i = ARG(0), level = RD32(i + 8) & 31;
    if (RD8(i + 16)) return 0;
    WR8(i + 16, 1);
    irq_objects[level] = i;
    return 1;
}
K(100, KeDisconnectInterrupt) {
    uint32_t i = ARG(0), level = RD32(i + 8) & 31;
    if (!RD8(i + 16)) return 0;
    WR8(i + 16, 0);
    if (irq_objects[level] == i) irq_objects[level] = 0;
    return 1;
}

void kernel_raise_irq(int level) { irq_pending |= 1u << level; }
K(46, HalReadWritePCISpace) {
    /* (Bus, Slot, Register, Buffer, Length, WritePCISpace): reads return zeros */
    if (!ARG(5)) memset(g_mem + ARG(3), 0, ARG(4));
    return 0;
}
K(142, KeSaveFloatingPointState) { return STATUS_SUCCESS; }
K(139, KeRestoreFloatingPointState) { return STATUS_SUCCESS; }


/* timers and DPCs
 * KDPC:   Type(2) Inserted(1) pad(1) ListEntry(8) DeferredRoutine(+12) DeferredContext(+16) Arg1(+20) Arg2(+24)
 * KTIMER: Header(16) DueTime(+16,8) TimerListEntry(+24,8) Dpc(+32) Period(+36) */
#define MAX_TIMERS 256
static struct { uint32_t timer; uint64_t due; uint32_t period_ms; } timers[MAX_TIMERS];
static int ntimers;
#define MAX_DPCS 256
static uint32_t dpc_queue[MAX_DPCS];
static int ndpcs;

static void queue_dpc(uint32_t dpc, uint32_t a1, uint32_t a2, int set_args) {
    if (RD8(dpc + 2)) return; /* already inserted */
    if (ndpcs == MAX_DPCS) rt_fatal("DPC queue full");
    WR8(dpc + 2, 1);
    if (set_args) { WR32(dpc + 20, a1); WR32(dpc + 24, a2); }
    dpc_queue[ndpcs++] = dpc;
}

static void timer_remove(uint32_t timer) {
    for (int i = 0; i < ntimers; i++)
        if (timers[i].timer == timer) { timers[i] = timers[--ntimers]; return; }
}

K(107, KeInitializeDpc) {
    uint32_t d = ARG(0);
    memset(g_mem + d, 0, 28);
    WR16(d, 0x13);
    WR32(d + 12, ARG(1));
    WR32(d + 16, ARG(2));
    return 0;
}
K(119, KeInsertQueueDpc) {
    uint32_t d = ARG(0);
    if (RD8(d + 2)) return 0;
    queue_dpc(d, ARG(1), ARG(2), 1);
    return 1;
}
K(137, KeRemoveQueueDpc) {
    uint32_t d = ARG(0);
    for (int i = 0; i < ndpcs; i++)
        if (dpc_queue[i] == d) { memmove(&dpc_queue[i], &dpc_queue[i + 1], (ndpcs - i - 1) * 4); ndpcs--; WR8(d + 2, 0); return 1; }
    return 0;
}
K(113, KeInitializeTimerEx) {
    uint32_t t = ARG(0);
    memset(g_mem + t, 0, 40);
    WR8(t, (uint8_t)(NotificationTimer + ARG(1)));
    WR8(t + 2, 10);
    return 0;
}
static uint32_t set_timer(uint32_t t, int64_t due, uint32_t period, uint32_t dpc) {
    int was = 0;
    for (int i = 0; i < ntimers; i++) if (timers[i].timer == t) was = 1;
    timer_remove(t);
    if (ntimers == MAX_TIMERS) rt_fatal("too many timers");
    uint64_t now = sched_now_100ns();
    uint64_t abs_due = due < 0 ? now + (uint64_t)(-due) : now; /* absolute times: fire now */
    WR32(t + 4, 0);
    WR32(t + 32, dpc);
    WR32(t + 36, period);
    WR8(t + 3, 1);
    timers[ntimers].timer = t;
    timers[ntimers].due = abs_due;
    timers[ntimers].period_ms = period;
    ntimers++;
    return was;
}
K(149, KeSetTimer) { return set_timer(ARG(0), (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1)), 0, ARG(3)); }
K(150, KeSetTimerEx) { return set_timer(ARG(0), (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1)), ARG(3), ARG(4)); }
K(97, KeCancelTimer) {
    uint32_t t = ARG(0);
    int was = 0;
    for (int i = 0; i < ntimers; i++) if (timers[i].timer == t) was = 1;
    timer_remove(t);
    WR8(t + 3, 0);
    return was;
}
K(153, KeSynchronizeExecution) {
    /* (Interrupt, SynchronizeRoutine, SynchronizeContext): run routine(context) */
    uint32_t arg = ARG(2);
    uint32_t routine = ARG(1);
    X86Context sub = *c;
    sub.esp = c->esp - 64;
    guest_call(&sub, routine, 1, &arg);
    return sub.eax;
}

void kernel_poll(void) {
    uint64_t now = sched_now_100ns();
    static uint64_t next_vblank;
    if (now >= next_vblank) {
        static uint64_t period;
        if (!period) { const char *e = getenv("UG_VBLANKHZ"); int hz = e ? atoi(e) : 60; period = 10000000ull / (hz > 0 ? hz : 60); }
        next_vblank = now + period;   /* 60 Hz (UG_VBLANKHZ: timing experiments) */
        nv2a_vblank();
    }
    for (int i = 0; i < ntimers; i++) {
        if (timers[i].due > now) continue;
        uint32_t t = timers[i].timer;
        WR32(t + 4, 1);
        uint32_t dpc = RD32(t + 32);
        if (dpc) queue_dpc(dpc, (uint32_t)now, (uint32_t)(now >> 32), 1);
        if (timers[i].period_ms) {
            timers[i].due = now + (uint64_t)timers[i].period_ms * 10000;
        } else {
            WR8(t + 3, 0);
            timers[i--] = timers[--ntimers];
        }
    }
}

/* DPC thread: runs queued DeferredRoutine(Dpc, DeferredContext, Arg1, Arg2) */
static void dpc_thread(X86Context *c) {
    for (;;) {
        while (irq_pending) {
            int level = __builtin_ctz(irq_pending);
            irq_pending &= ~(1u << level);
            uint32_t i = irq_objects[level];
            if (!i) continue;
            uint32_t args[2] = {i, RD32(i + 4)};
            if (g_log_kernel) LOG("[k] IRQ %d ISR %08X\n", level, RD32(i));
            uint8_t old = irql;
            irql = (uint8_t)RD32(i + 12);
            guest_call(c, RD32(i), 2, args);
            irql = old;
        }
        while (ndpcs) {
            uint32_t d = dpc_queue[0];
            memmove(&dpc_queue[0], &dpc_queue[1], (ndpcs - 1) * 4);
            ndpcs--;
            WR8(d + 2, 0);
            uint32_t args[4] = {d, RD32(d + 16), RD32(d + 20), RD32(d + 24)};
            if (g_log_kernel) LOG("[k] DPC %08X routine %08X\n", d, RD32(d + 12));
            uint8_t old = irql;
            irql = 2;
            guest_call(c, RD32(d + 12), 4, args);
            irql = old;
        }
        ug_sched_yield();
    }
}

static void register_handlers(void) {
    REG(14, ExAllocatePool); REG(15, ExAllocatePoolWithTag); REG(17, ExFreePool); REG(23, ExQueryPoolBlockSize);
    REG(165, MmAllocateContiguousMemory); REG(166, MmAllocateContiguousMemoryEx); REG(171, MmFreeContiguousMemory);
    REG(180, MmQueryAllocationSize); REG(173, MmGetPhysicalAddress); REG(178, MmPersistContiguousMemory);
    REG(175, MmLockUnlockBufferPages); REG(176, MmLockUnlockPhysicalPage); REG(168, MmClaimGpuInstanceMemory);
    REG(184, NtAllocateVirtualMemory); REG(199, NtFreeVirtualMemory); REG(217, NtQueryVirtualMemory);
    REG(128, KeQuerySystemTime); REG(125, KeQueryInterruptTime); REG(99, KeDelayExecutionThread);
    REG(151, KeStallExecutionProcessor); REG(305, RtlTimeToTimeFields);
    REG(129, KeRaiseIrqlToDpcLevel); REG(160, KfRaiseIrql); REG(161, KfLowerIrql);
    REG(189, NtCreateEvent); REG(225, NtSetEvent); REG(186, NtClearEvent); REG(205, NtPulseEvent);
    REG(145, KeSetEvent); REG(159, KeWaitForSingleObject); REG(158, KeWaitForMultipleObjects);
    REG(233, NtWaitForSingleObject); REG(234, NtWaitForSingleObjectEx); REG(187, NtClose);
    REG(246, ObReferenceObjectByHandle); REG(250, ObfDereferenceObject);
    REG(291, RtlInitializeCriticalSection); REG(277, RtlEnterCriticalSection); REG(294, RtlLeaveCriticalSection);
    REG(255, PsCreateSystemThreadEx); REG(258, PsTerminateSystemThread);
    REG(289, RtlInitAnsiString); REG(279, RtlEqualString); REG(269, RtlCompareMemoryUlong);
    REG(301, RtlNtStatusToDosError);
    REG(8, DbgPrint); REG(49, HalReturnToFirmware); REG(360, HalInitiateShutdown); REG(95, KeBugCheck);
    REG(327, XeLoadSection); REG(328, XeUnloadSection); REG(24, ExQueryNonVolatileSetting);
    REG(335, XcSHAInit); REG(336, XcSHAUpdate); REG(337, XcSHAFinal); REG(340, XcHMAC);
    REG(1, AvGetSavedDataAddress); REG(2, AvSendTVEncoderOption); REG(3, AvSetDisplayMode);
    REG(4, AvSetSavedDataAddress); REG(47, HalRegisterShutdownNotification); REG(44, HalGetInterruptVector);
    REG(109, KeInitializeInterrupt); REG(98, KeConnectInterrupt); REG(100, KeDisconnectInterrupt);
    REG(107, KeInitializeDpc); REG(119, KeInsertQueueDpc); REG(137, KeRemoveQueueDpc);
    REG(113, KeInitializeTimerEx); REG(149, KeSetTimer); REG(150, KeSetTimerEx); REG(97, KeCancelTimer);
    REG(153, KeSynchronizeExecution);
    REG(46, HalReadWritePCISpace); REG(142, KeSaveFloatingPointState); REG(139, KeRestoreFloatingPointState);
}

void kernel_tick(void);

static DWORD WINAPI ticker(void *p) {
    (void)p;
    timeBeginPeriod(1);
    for (;;) {
        kernel_tick();
        Sleep(1);
    }
}

void kernel_init(void) {
    init_data_exports();
    register_handlers();
    fs_register(handlers);
    kernel2_register(handlers);
    sched_create_host_thread(dpc_thread);
    g_log_kernel = getenv("UG_KLOG") != NULL;
    /* KeTickCount must advance while guest code spins on it without calling the kernel. */
    CreateThread(NULL, 0, ticker, NULL, 0, NULL);
}

void kernel_tick(void) {
    /* KeTickCount in ms */
    WR32(data_va[156], (uint32_t)(sched_now_100ns() / 10000));
}

int kernel_call(X86Context *c, uint32_t va) {
    if (va < KSENTINEL_BASE || va >= KSENTINEL_BASE + 379 * 4 || (va & 3)) return 0;
    uint32_t ord = (va - KSENTINEL_BASE) / 4;
    const KExport *e = &k_exports[ord];
    if (!handlers[ord]) rt_fatal("kernel export %u %s not implemented (caller ret %08X)", ord, e->name ? e->name : "?",
                                 RD32(c->esp));
    kernel_tick();
    sched_maybe_yield();
    if (g_log_kernel) LOG("[k] %s(%08X, %08X, %08X, %08X) ret %08X\n", e->name, ARG(0), ARG(1), ARG(2), ARG(3), RD32(c->esp));
    c->eax = handlers[ord](c);
    uint32_t pop = 4;
    if (e->conv == KC_STDCALL) pop += e->argbytes;
    else if (e->conv == KC_FASTCALL && e->argbytes > 8) pop += e->argbytes - 8;
    c->esp += pop;
    return 1;
}
