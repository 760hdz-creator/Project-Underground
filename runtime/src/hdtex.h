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
/* async (worker thread): submit once per content hash, then poll; the original texture is drawn meanwhile */
void hdtex_submit(uint64_t key, uint32_t *rgba, uint8_t *idx, const uint32_t *pal, uint32_t pal_len, int w, int h);
int hdtex_result(uint64_t key);                             /* -3 not submitted, -2 waiting, -1 none, >= 0 pack */
/* the same, decoding on the worker: raw = level 0 bytes (owned by the worker after the call); dec fills rgba (and
 * idx for palettized textures) */
typedef void (*HdDecode)(const uint8_t *raw, int kind, int bpp, int w, int h, const uint32_t *pal, uint32_t *rgba, uint8_t *idx);
void hdtex_submit_raw(uint64_t key, uint8_t *raw, int kind, int bpp, int i8, int w, int h, const uint32_t *pal,
                      uint32_t pal_len, HdDecode dec);
int hdtex_file(int pack, uint8_t **buf, size_t *len);       /* 1 = file handed over (caller frees), 0 = later */
