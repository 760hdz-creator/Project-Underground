/* Guest address space: 4 GB reserved; RAM mapped three times (0, 0x80000000 and the write-combined 0xF0000000) from one section. */
#ifndef _WIN32
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rt.h"

uint8_t *g_mem;

_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    ExitProcess(3);
}

#ifdef _WIN32
static void commit_fixed(uint32_t va, uint32_t size) {
    if (!VirtualAlloc(g_mem + va, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
        rt_fatal("commit %08X+%X failed (%lu)", va, size, GetLastError());
}

void mem_init(void) {
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, RAM_SIZE, NULL);
    if (!sec) rt_fatal("CreateFileMapping failed");
    for (int attempt = 0; attempt < 16 && !g_mem; attempt++) {
        uint8_t *base = VirtualAlloc(NULL, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
        if (!base) rt_fatal("cannot reserve 4 GB");
        VirtualFree(base, 0, MEM_RELEASE);
        void *a = MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, RAM_SIZE, base);
        void *b = MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, RAM_SIZE, base + RAM_ALIAS);
        void *wc = MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, RAM_SIZE, base + RAM_ALIAS_WC);
        if (a == base && b == base + RAM_ALIAS && wc == base + RAM_ALIAS_WC) {
            g_mem = base;
        } else {
            if (a) UnmapViewOfFile(a);
            if (b) UnmapViewOfFile(b);
            if (wc) UnmapViewOfFile(wc);
        }
    }
    if (!g_mem) rt_fatal("cannot map guest RAM");
    commit_fixed(KAREA_BASE, KAREA_SIZE);
    commit_fixed(KSENTINEL_BASE, 0x00100000);
    commit_fixed(HW_BASE, HW_SIZE);
}
#else
static void commit_fixed(uint32_t va, uint32_t size) {
    if (mmap(g_mem + va, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
        rt_fatal("commit %08X+%X failed (%d)", va, size, errno);
}

/* 4 GB reserved (PROT_NONE); RAM is one memfd mapped at guest 0 and at RAM_ALIAS, like the Windows section view. */
void mem_init(void) {
    int fd = memfd_create("gauntlet-ram", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, RAM_SIZE)) rt_fatal("memfd_create failed (%d)", errno);
    uint8_t *base = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) rt_fatal("cannot reserve 4 GB");
    if (mmap(base, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(base + RAM_ALIAS, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(base + RAM_ALIAS_WC, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
        rt_fatal("cannot map guest RAM (%d)", errno);
    close(fd);
    g_mem = base;
    commit_fixed(KAREA_BASE, KAREA_SIZE);
    commit_fixed(KSENTINEL_BASE, 0x00100000);
    commit_fixed(HW_BASE, HW_SIZE);
}
#endif

/* ---- guest heap: first-fit over [HEAP_BASE, HEAP_END), host-side block table ---- */
typedef struct { uint32_t va, size; } Block;
static Block *blocks;   /* sorted by va, allocated blocks only */
static int nblocks, capblocks;

uint32_t mem_alloc(uint32_t size, uint32_t align) {
    if (!size) size = 1;
    if (align < 16) align = 16;
    size = (size + 15) & ~15u;
    uint32_t cur = HEAP_BASE;
    int i = 0;
    for (;; i++) {
        uint32_t start = (cur + align - 1) & ~(align - 1);
        uint32_t limit = i < nblocks ? blocks[i].va : HEAP_END;
        if (start + size <= limit && start + size > start) {
            if (nblocks == capblocks) {
                capblocks = capblocks ? capblocks * 2 : 256;
                blocks = realloc(blocks, capblocks * sizeof(Block));
            }
            memmove(&blocks[i + 1], &blocks[i], (nblocks - i) * sizeof(Block));
            blocks[i].va = start;
            blocks[i].size = size;
            nblocks++;
            memset(g_mem + start, 0, size);
            return start;
        }
        if (i >= nblocks) return 0;
        cur = blocks[i].va + blocks[i].size;
    }
}

static int find_block(uint32_t va) {
    if (va >= RAM_ALIAS && va < RAM_ALIAS + RAM_SIZE) va -= RAM_ALIAS;
    if (va >= RAM_ALIAS_WC && va < RAM_ALIAS_WC + RAM_SIZE) va -= RAM_ALIAS_WC;
    int lo = 0, hi = nblocks;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (blocks[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    return (lo < nblocks && blocks[lo].va == va) ? lo : -1;
}

void mem_free(uint32_t va) {
    int i = find_block(va);
    if (i < 0) { LOG("[mem] free of unknown block %08X\n", va); return; }
    memmove(&blocks[i], &blocks[i + 1], (nblocks - i - 1) * sizeof(Block));
    nblocks--;
}

uint32_t mem_size(uint32_t va) {
    int i = find_block(va);
    return i < 0 ? 0 : blocks[i].size;
}

static uint32_t karea_next = KAREA_BASE + 0x1000;

uint32_t karea_alloc(uint32_t size) {
    uint32_t va = karea_next;
    karea_next += (size + 15) & ~15u;
    if (karea_next > KAREA_BASE + KAREA_SIZE) rt_fatal("kernel area exhausted");
    memset(g_mem + va, 0, size);
    return va;
}
