/* HD texture packs matched by picture (hdtex.c) */
#pragma once
#include <stddef.h>
#include <stdint.h>
int hdtex_enabled(void);                                    /* UG_HDTEX=<folder>: scans it once */
int hdtex_find(const uint32_t *rgba, int w, int h);         /* level 0 RGBA8 (R low) -> pack index, -1 none */
int hdtex_info(int idx, int *w, int *h, int *levels, uint32_t *fourcc);
uint8_t *hdtex_load(int idx, size_t *len);                  /* whole .dds; data at byte 128; caller frees */
