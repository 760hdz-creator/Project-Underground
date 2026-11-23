/* xboxkrnl exports THUG imports beyond the Project Gauntlet set (kernel.c). Registered by kernel2_register. */
#include <windows.h>
#include "rt.h"

#define STATUS_SUCCESS                0x00000000u
#define STATUS_INVALID_HANDLE         0xC0000008u
#define STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define STATUS_NO_MEMORY              0xC0000017u
#define STATUS_NOT_IMPLEMENTED        0xC0000002u
#define STATUS_MUTANT_NOT_OWNED       0xC0000046u

#define K(name) static uint32_t k_##name(X86Context *c)

enum { MutantObject = 2 };

/* ---- I/O manager: device objects for the XPP (USB peripheral) driver. No IRPs are ever queued here. */
K(IoCreateDevice) {
    /* (DriverObject, DeviceExtensionSize, DeviceName, DeviceType, Exclusive, *DeviceObject) */
    uint32_t ext = ARG(1), d = mem_alloc(0x60 + ext, 16);
    if (!d) return STATUS_NO_MEMORY;
    WR16(d + 0x00, 3);                     /* IO_TYPE_DEVICE */
    WR16(d + 0x02, (uint16_t)(0x60 + ext));
    WR32(d + 0x04, 1);                     /* ReferenceCount */
    WR32(d + 0x08, ARG(0));                /* DriverObject */
    WR32(d + 0x0C, d);                     /* MountedOrSelfDevice */
    WR32(d + 0x18, ext ? d + 0x60 : 0);    /* DeviceExtension */
    WR8(d + 0x1C, (uint8_t)ARG(3));        /* DeviceType */
    WR8(d + 0x1E, 1);                      /* StackSize */
    WR32(d + 0x20, 512);                   /* SectorSize */
    WR32(ARG(5), d);
    return STATUS_SUCCESS;
}
K(IoDeleteSymbolicLink) { return STATUS_SUCCESS; }
K(IoInvalidDeviceRequest) { return STATUS_INVALID_DEVICE_REQUEST; }
K(IoStartNextPacket) { return 0; }
K(IoStartPacket) { return 0; }
K(IofCompleteRequest) { return 0; }         /* fastcall (Irp, PriorityBoost) */
K(IoMarkIrpMustComplete) { return 0; }

/* ---- time: the performance counter runs at the Xbox CPU clock (733.33 MHz) */
#define PERF_FREQ 733333333ull
K(KeQueryPerformanceCounter) {
    uint64_t t = sched_now_100ns();
    uint64_t v = (t / 10000000ull) * PERF_FREQ + (t % 10000000ull) * PERF_FREQ / 10000000ull;
    c->edx = (uint32_t)(v >> 32);
    return (uint32_t)v;
}
K(KeQueryPerformanceFrequency) { c->edx = 0; return (uint32_t)PERF_FREQ; }
K(RtlTimeFieldsToTime) {
    /* (TIME_FIELDS*, LARGE_INTEGER*) -> BOOLEAN */
    uint32_t f = ARG(0);
    SYSTEMTIME st = {RD16(f), RD16(f + 2), RD16(f + 14), RD16(f + 4), RD16(f + 6), RD16(f + 8), RD16(f + 10), RD16(f + 12)};
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    WR32(ARG(1), ft.dwLowDateTime);
    WR32(ARG(1) + 4, ft.dwHighDateTime);
    return 1;
}

/* ---- memory manager */
K(MmQueryAddressProtect) { return PAGE_READWRITE; }
K(MmSetAddressProtect) { return 0; }
K(MmQueryStatistics) {
    /* MM_STATISTICS: Length, TotalPhysicalPages, AvailablePages, VirtualMemoryBytesCommitted,
     * VirtualMemoryBytesReserved, CachePagesCommitted, PoolPagesCommitted, StackPagesCommitted, ImagePagesCommitted */
    uint32_t s = ARG(0);
    if (RD32(s) != 36) return 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
    WR32(s + 4, RAM_SIZE / 4096);
    WR32(s + 8, (HEAP_END - HEAP_BASE) / 2 / 4096);
    for (int i = 3; i < 9; i++) WR32(s + 4 * i, 0);
    return STATUS_SUCCESS;
}

/* ---- mutants: DISPATCHER_HEADER {Type, Absolute, Size, Inserted; SignalState} + list (8) + OwnerThread at +16 */
K(NtCreateMutant) {
    /* (*MutantHandle, ObjectAttributes, InitialOwner) */
    uint32_t m = karea_alloc(32);
    WR8(m, MutantObject);
    WR8(m + 2, 8);
    WR32(m + 4, ARG(2) ? 0 : 1);
    WR32(m + 16, ARG(2) ? sched_thread_object(sched_current()) : 0);
    WR32(ARG(0), handle_new(H_OBJECT, m, NULL));
    return STATUS_SUCCESS;
}
K(NtReleaseMutant) {
    /* (Handle, *PreviousCount) */
    uint32_t m = handle_object(ARG(0));
    if (!m) return STATUS_INVALID_HANDLE;
    int32_t prev = (int32_t)RD32(m + 4);
    if (ARG(1)) WR32(ARG(1), (uint32_t)prev);
    if (prev > 0) return STATUS_MUTANT_NOT_OWNED;
    WR32(m + 4, (uint32_t)(prev + 1));
    if (prev + 1 > 0) WR32(m + 16, 0);
    return STATUS_SUCCESS;
}

/* ---- threads and waits */
K(NtResumeThread) { if (ARG(1)) WR32(ARG(1), 1); return STATUS_SUCCESS; }   /* threads never start suspended (sched.c) */
K(NtYieldExecution) { ug_sched_yield(); return STATUS_SUCCESS; }
K(NtWaitForMultipleObjectsEx) {
    /* (Count, Handles[], WaitType (0 = all, 1 = any), WaitMode, Alertable, *Timeout) */
    uint32_t objs[64], n = ARG(0) > 64 ? 64 : ARG(0);
    for (uint32_t i = 0; i < n; i++) {
        objs[i] = handle_object(RD32(ARG(1) + 4 * i));
        if (!objs[i]) return STATUS_INVALID_HANDLE;
    }
    int64_t t = 0;
    int ht = ARG(5) != 0;
    if (ht) t = (int64_t)RD64(ARG(5));
    return kernel_wait(objs, (int)n, ARG(2) == 1, t, ht);
}

/* ---- exceptions: not expected during normal play; fail loudly with the caller */
K(RtlRaiseException) { rt_fatal("RtlRaiseException code %08X (ret %08X)", RD32(ARG(0)), RD32(c->esp)); }
K(RtlUnwind) { rt_fatal("RtlUnwind (ret %08X)", RD32(c->esp)); }

/* ---- HAL */
K(HalIsResetOrShutdownPending) { return 0; }

/* ---- Ethernet PHY: no network cable (XNET then reports no link) */
K(PhyGetLinkState) { return 0; }
K(PhyInitialize) { return STATUS_SUCCESS; }

/* ---- crypto used by XNET and save signing */
K(XcRC4Key) {
    /* (pbKeyStruct (258 bytes: i, j, S[256]), dwKeyLength, pbKey) */
    uint32_t ks = ARG(0), n = ARG(1), key = ARG(2);
    uint8_t *s = g_mem + ks + 2;
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + g_mem[key + (n ? (uint32_t)i % n : 0)]) & 255;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
    }
    WR8(ks, 0); WR8(ks + 1, 0);
    return 0;
}
K(XcRC4Crypt) {
    /* (pbKeyStruct, dwInputLength, pbInput): in place */
    uint32_t ks = ARG(0), n = ARG(1), p = ARG(2);
    uint8_t *s = g_mem + ks + 2, i = RD8(ks), j = RD8(ks + 1);
    for (uint32_t k = 0; k < n; k++) {
        i++; j += s[i];
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        g_mem[p + k] ^= s[(uint8_t)(s[i] + s[j])];
    }
    WR8(ks, i); WR8(ks + 1, j);
    return 0;
}
K(XcDESKeyParity) {
    /* (pbKey, dwKeyLength): set odd parity in each byte */
    for (uint32_t k = 0; k < ARG(1); k++) {
        uint8_t b = RD8(ARG(0) + k) & 0xFE;
        WR8(ARG(0) + k, b | !(__builtin_popcount(b) & 1));
    }
    return 0;
}
K(XcKeyTable) { LOG("[kernel] XcKeyTable (DES) not implemented: ignored\n"); return 0; }
K(XcBlockCryptCBC) { LOG("[kernel] XcBlockCryptCBC (DES) not implemented: ignored\n"); return 0; }
K(XcModExp) { LOG("[kernel] XcModExp not implemented: ignored\n"); return 0; }
K(XcVerifyPKCS1Signature) { return 1; }

void kernel2_register(KHandler *h) {
    h[65] = k_IoCreateDevice;      h[69] = k_IoDeleteSymbolicLink; h[74] = k_IoInvalidDeviceRequest;
    h[81] = k_IoStartNextPacket;   h[83] = k_IoStartPacket;        h[87] = k_IofCompleteRequest;
    h[359] = k_IoMarkIrpMustComplete;
    h[126] = k_KeQueryPerformanceCounter; h[127] = k_KeQueryPerformanceFrequency; h[304] = k_RtlTimeFieldsToTime;
    h[179] = k_MmQueryAddressProtect; h[182] = k_MmSetAddressProtect; h[181] = k_MmQueryStatistics;
    h[192] = k_NtCreateMutant;     h[221] = k_NtReleaseMutant;
    h[224] = k_NtResumeThread;     h[238] = k_NtYieldExecution;    h[235] = k_NtWaitForMultipleObjectsEx;
    h[302] = k_RtlRaiseException;  h[312] = k_RtlUnwind;
    h[358] = k_HalIsResetOrShutdownPending;
    h[252] = k_PhyGetLinkState;    h[253] = k_PhyInitialize;
    h[338] = k_XcRC4Key;           h[339] = k_XcRC4Crypt;          h[346] = k_XcDESKeyParity;
    h[347] = k_XcKeyTable;         h[349] = k_XcBlockCryptCBC;     h[345] = k_XcModExp;
    h[344] = k_XcVerifyPKCS1Signature;
}
