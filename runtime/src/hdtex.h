/* HD texture packs matched by picture (hdtex.c) */
#pragma once
#include <stddef.h>
#include <stdint.h>
int hdtex_enabled(void);                                    /* UG_HDTEX=<folder>: scans it once */
int hdtex_find(const uint32_t *rgba, int w, int h);         /* level 0 RGBA8 (R low) -> pack index, -1 none */
/* I8: linear indices, guest A8R8G8B8 palette, rgba (for palette variants) -> pack index by the exact PS2 hashes */
int hdtex_find_indexed(const uint8_t *idx, int w, int h, const uint32_t *pal, uint32_t pal_len, const uint32_t *rgba);
int hdtex_info(int idx, int *w, int *h, int *levels, uint32_t *fourcc);
uint8_t *hdtex_load(int idx, size_t *len);                  /* whole .dds; data at byte 128; caller frees */
