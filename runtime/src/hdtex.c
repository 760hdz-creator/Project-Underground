/* HD texture packs (PCSX2 replacement format, made for the PS2 version): UG_HDTEX=<folder> (scanned recursively, used
 * in place). The pack's file names are PS2 GS hashes, which cannot be rebuilt from the Xbox data (the Xbox textures
 * are DXT, the PS2 ones palettized), so textures are matched by picture instead:
 *   - every .dds in the pack contributes a thumbnail: its mip level whose larger side is 16 px (colour only);
 *   - gpu_vulkan.c hands over each new guest texture (level 0 as RGBA8); it is box-averaged to the same thumbnail
 *     size, compared (mean absolute RGB difference) with every pack thumbnail of the same shape, and taken when the
 *     best match is very close, or close and clearly better than the next different-looking candidate.
 * Flat textures (little detail) are never matched: too easy to confuse and nothing to gain.
 * Checked offline on 261 Slam City textures: correct matches had d <= ~6, or a clear margin over the runner-up. */
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdtex.h"

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#define T 16                     /* thumbnail: larger side */

typedef struct { char *path; int w, h, levels, tw, th; uint32_t fourcc; uint8_t thumb[T * T * 3]; } HdTex;
static HdTex *packs;
static int npacks, cap_packs, state = -1;   /* -1 not started, 0 off, 1 on */

static uint32_t c565(uint16_t v, int ch) {
    if (ch == 0) return (v >> 11 & 31) * 255 / 31;
    if (ch == 1) return (v >> 5 & 63) * 255 / 63;
    return (v & 31) * 255 / 31;
}
/* colour of one DXT block (DXT1 8 bytes, DXT3/5 16 bytes with the colour half last) -> 16 RGB texels */
static void block_rgb(const uint8_t *b, int bsz, uint8_t out[16][3]) {
    const uint8_t *cb = bsz == 16 ? b + 8 : b;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint32_t p[4][3];
    for (int ch = 0; ch < 3; ch++) {
        uint32_t a = c565(c0, ch), z = c565(c1, ch);
        p[0][ch] = a; p[1][ch] = z;
        if (bsz == 16 || c0 > c1) { p[2][ch] = (2 * a + z) / 3; p[3][ch] = (a + 2 * z) / 3; }
        else { p[2][ch] = (a + z) / 2; p[3][ch] = 0; }
    }
    uint32_t idx = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24);
    for (int i = 0; i < 16; i++)
        for (int ch = 0; ch < 3; ch++) out[i][ch] = (uint8_t)p[(idx >> (2 * i)) & 3][ch];
}

/* reads the DDS header and the 16-px mip of one file into the index */
static void add_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hd[128];
    if (fread(hd, 1, 128, f) != 128 || memcmp(hd, "DDS ", 4)) { fclose(f); return; }
    uint32_t h = *(uint32_t *)(hd + 12), w = *(uint32_t *)(hd + 16), mips = *(uint32_t *)(hd + 28);
    uint32_t fourcc = *(uint32_t *)(hd + 84);
    int bsz = fourcc == 0x31545844u ? 8 : (fourcc == 0x33545844u || fourcc == 0x35545844u) ? 16 : 0;   /* DXT1 / 3 / 5 */
    if (!bsz || w < 16 || h < 16 || w > 8192 || h > 8192) { fclose(f); return; }
    if (mips < 1) mips = 1;
    long off = 128;
    uint32_t mw = w, mh = h;
    int lvl = 0;
    while ((mw > T || mh > T) && lvl + 1 < (int)mips) {
        off += (long)((mw + 3) / 4) * ((mh + 3) / 4) * bsz;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; lvl++;
    }
    if (mw > T || mh > T || mw < 1 || mh < 1) { fclose(f); return; }   /* no small mip: skip (rare) */
    uint32_t bw = (mw + 3) / 4, bh = (mh + 3) / 4;
    uint8_t buf[16 * 4 * 4 * 16];
    if (bw * bh * bsz > sizeof buf || fseek(f, off, SEEK_SET) || fread(buf, 1, bw * bh * bsz, f) != bw * bh * bsz) {
        fclose(f);
        return;
    }
    fclose(f);
    if (npacks == cap_packs) {
        cap_packs = cap_packs ? cap_packs * 2 : 1024;
        packs = realloc(packs, sizeof *packs * cap_packs);
    }
    HdTex *p = &packs[npacks];
    p->w = (int)w; p->h = (int)h; p->levels = (int)mips; p->fourcc = fourcc; p->tw = (int)mw; p->th = (int)mh;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][3];
            block_rgb(buf + (by * bw + bx) * bsz, bsz, px);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < mw && y < mh) memcpy(p->thumb + (y * mw + x) * 3, px[i], 3);
            }
        }
    p->path = strdup(path);
    npacks++;
}

static int ends_dds(const char *n) {
    size_t l = strlen(n);
    return l > 4 && (n[l - 4] == '.') && (n[l - 3] | 32) == 'd' && (n[l - 2] | 32) == 'd' && (n[l - 1] | 32) == 's';
}
static void scan(const char *dir, int depth) {
    if (depth > 8) return;
    char path[1024];
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof path, "%s\\*", dir);
    HANDLE hf = FindFirstFileA(path, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan(path, depth + 1);
        else if (ends_dds(fd.cFileName)) add_file(path);
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) scan(path, depth + 1);
        else if (ends_dds(e->d_name)) add_file(path);
    }
    closedir(d);
#endif
}

int hdtex_enabled(void) {
    if (state >= 0) return state;
    const char *e = getenv("UG_HDTEX");
    state = 0;
    if (!e || !*e || !strcmp(e, "0")) return 0;
    scan(e, 0);
    LOG("[hdtex] %d textures in %s\n", npacks, e);
    state = npacks > 0;
    return state;
}

/* rgba: level 0, w x h, R in the low byte. Returns a pack index or -1. */
int hdtex_find(const uint32_t *rgba, int w, int h) {
    if (!hdtex_enabled() || w < 32 || h < 32) return -1;
    int tw = w >= h ? T : T * w / h, th = h >= w ? T : T * h / w;
    if (tw < 1 || th < 1 || w % tw || h % th) return -1;
    int sx = w / tw, sy = h / th;
    uint8_t t[T * T * 3];
    double sum[3] = {0}, sq[3] = {0};
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            uint32_t acc[3] = {0};
            for (int yy = 0; yy < sy; yy++)
                for (int xx = 0; xx < sx; xx++) {
                    uint32_t c = rgba[(y * sy + yy) * w + x * sx + xx];
                    acc[0] += c & 255; acc[1] += c >> 8 & 255; acc[2] += c >> 16 & 255;
                }
            for (int ch = 0; ch < 3; ch++) {
                uint8_t v = (uint8_t)(acc[ch] / (uint32_t)(sx * sy));
                t[(y * tw + x) * 3 + ch] = v;
                sum[ch] += v; sq[ch] += (double)v * v;
            }
        }
    int n = tw * th;
    double var = 0;
    for (int ch = 0; ch < 3; ch++) var += sq[ch] / n - (sum[ch] / n) * (sum[ch] / n);
    if (var < 3 * 36.0) return -1;   /* flat (std < 6 per channel): skip */
    int best = -1, nb = n * 3;
    uint32_t bd = UINT32_MAX;
    for (int i = 0; i < npacks; i++) {
        if (packs[i].tw != tw || packs[i].th != th) continue;
        const uint8_t *q = packs[i].thumb;
        uint32_t d = 0;
        for (int k = 0; k < nb && d < bd; k++) d += (uint32_t)abs((int)t[k] - (int)q[k]);
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return -1;
    /* runner-up: the best candidate that does not look like the winner (near-copies in the pack are the same art) */
    uint32_t sd = UINT32_MAX;
    for (int i = 0; i < npacks; i++) {
        if (i == best || packs[i].tw != tw || packs[i].th != th) continue;
        const uint8_t *q = packs[i].thumb, *b = packs[best].thumb;
        uint32_t d = 0, same = 0;
        for (int k = 0; k < nb; k++) { d += (uint32_t)abs((int)t[k] - (int)q[k]); same += (uint32_t)abs((int)b[k] - (int)q[k]); }
        if (same > (uint32_t)nb * 3 && d < sd) sd = d;
    }
    double dm = (double)bd / nb, sm = sd == UINT32_MAX ? 1e9 : (double)sd / nb;
    int ok = dm < 6.0 || (dm < 14.0 && sm > 2.0 * dm);
    static int logged;
    if (getenv("UG_HDLOG") || logged < 3) {
        logged++;
        LOG("[hdtex] %dx%d d %.1f next %.1f -> %s %s\n", w, h, dm, sm, ok ? "use" : "skip", packs[best].path);
    }
    return ok ? best : -1;
}

int hdtex_info(int idx, int *w, int *h, int *levels, uint32_t *fourcc) {
    if (idx < 0 || idx >= npacks) return 0;
    *w = packs[idx].w; *h = packs[idx].h; *levels = packs[idx].levels; *fourcc = packs[idx].fourcc;
    return 1;
}

/* the whole file; the texel data starts at byte 128. Caller frees. */
uint8_t *hdtex_load(int idx, size_t *len) {
    if (idx < 0 || idx >= npacks) return NULL;
    FILE *f = fopen(packs[idx].path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 128 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}
