/* XBE loader: header + sections into guest memory, kernel thunks to sentinel/data addresses. */
#include <stdlib.h>
#include "rt.h"

uint32_t g_kernel_thunk;
uint32_t g_xbe_stack = 0x10000;

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size);
    if (fread(buf, 1, *size, f) != *size) rt_fatal("short read %s", path);
    fclose(f);
    return buf;
}

uint32_t kernel_data_export(uint32_t ordinal); /* kernel.c */

uint32_t xbe_load(const char *path) {
    size_t size;
    uint8_t *x = read_file(path, &size);
    uint32_t base, hdr_size, nsec, sec_va, entry, thunk;
    memcpy(&base, x + 0x104, 4);
    memcpy(&hdr_size, x + 0x108, 4);
    memcpy(&nsec, x + 0x11C, 4);
    memcpy(&sec_va, x + 0x120, 4);
    memcpy(&entry, x + 0x128, 4);
    memcpy(&thunk, x + 0x158, 4);
    uint32_t image_size;
    memcpy(&image_size, x + 0x10C, 4);
    memcpy(&g_xbe_stack, x + 0x130, 4);
    if (memcmp(x, "XBEH", 4) || base != 0x10000) rt_fatal("bad XBE");
    if (base + image_size > HEAP_BASE) rt_fatal("XBE image ends %08X, above HEAP_BASE", base + image_size);
    entry ^= 0xA8FC57AB;   /* retail keys */
    thunk ^= 0x5B6D40B6;
    memcpy(g_mem + base, x, hdr_size);
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = x + (sec_va - base) + i * 56;
        uint32_t va, vsize, raw, rsize;
        memcpy(&va, s + 4, 4);
        memcpy(&vsize, s + 8, 4);
        memcpy(&raw, s + 12, 4);
        memcpy(&rsize, s + 16, 4);
        memset(g_mem + va, 0, vsize);
        memcpy(g_mem + va, x + raw, rsize);
    }
    /* XAPI reads the kernel's PE header through 0x8001003C (e_lfanew - 0x7FFF0000) to find its INIT section.
     * Here 0x80010000 aliases guest 0x10000 (the XBE header), and offset 0x3C is in the XBE signature, which
     * nothing reads at run time: point it at zeroes (0 sections), so the INIT lookup fails and is skipped. */
    {
        uint32_t fake = karea_alloc(0x40) + 0x20;
        WR32(base + 0x3C, fake + 0x7FFF0000u);
    }
    g_kernel_thunk = thunk;
    for (uint32_t a = thunk;; a += 4) {
        uint32_t v = RD32(a);
        if (!v) break;
        uint32_t ord = v & 0x7FFFFFFF;
        uint32_t data = kernel_data_export(ord);
        WR32(a, data ? data : KSENTINEL_BASE + ord * 4);
    }
    free(x);
    return entry;
}
