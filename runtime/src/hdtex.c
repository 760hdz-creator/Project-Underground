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
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "hdtex.h"

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#define T 16                     /* thumbnail: larger side */

/* name: <tex0>-<clut>-<bits>.dds (clut 0: <tex0>-<bits>.dds); bits = PSM:6 TW:4 TH:4 */
typedef struct {
    char *path; int w, h, levels, tw, th; uint32_t fourcc; uint8_t thumb[T * T * 3];
    uint64_t tex0, clut; uint32_t bits;
} HdTex;
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
    if (!bsz || w < 4 || h < 4 || w > 8192 || h > 8192) { fclose(f); return; }
    if (mips < 1) mips = 1;
    long off = 128;
    uint32_t mw = w, mh = h;
    int lvl = 0;
    while ((mw > T || mh > T) && lvl + 1 < (int)mips) {
        off += (long)((mw + 3) / 4) * ((mh + 3) / 4) * bsz;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; lvl++;
    }
    /* thumbnail only from a mip whose larger side is exactly 16 (no thumbnail: exact PS2-hash matches only) */
    int thumb_ok = (mw == T || mh == T) && mw <= T && mh <= T;
    uint32_t bw = (mw + 3) / 4, bh = (mh + 3) / 4;
    uint8_t buf[16 * 4 * 4 * 16];
    if (thumb_ok && (bw * bh * bsz > sizeof buf || fseek(f, off, SEEK_SET) || fread(buf, 1, bw * bh * bsz, f) != bw * bh * bsz))
        thumb_ok = 0;
    fclose(f);
    if (npacks == cap_packs) {
        cap_packs = cap_packs ? cap_packs * 2 : 1024;
        packs = realloc(packs, sizeof *packs * cap_packs);
    }
    HdTex *p = &packs[npacks];
    p->w = (int)w; p->h = (int)h; p->levels = (int)mips; p->fourcc = fourcc;
    p->tw = thumb_ok ? (int)mw : 0; p->th = thumb_ok ? (int)mh : 0;
    if (thumb_ok)
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
    p->tex0 = p->clut = 0; p->bits = 0;
    {   /* PCSX2 name: <tex0>-<clut>-<bits> or <tex0>-<bits>, hex */
        const char *b = path + strlen(path);
        while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
        char *e1, *e2, *e3;
        unsigned long long a = strtoull(b, &e1, 16);
        if (*e1 == '-') {
            unsigned long long c = strtoull(e1 + 1, &e2, 16);
            if (*e2 == '.' && e2 - e1 - 1 == 8) { p->tex0 = a; p->bits = (uint32_t)c & 0x3FFF; }
            else if (*e2 == '-') {
                unsigned long long d = strtoull(e2 + 1, &e3, 16);
                if (*e3 == '.') { p->tex0 = a; p->clut = c; p->bits = (uint32_t)d & 0x3FFF; }
            }
        }
    }
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

/* Only the worker thread scans and matches (see the async part at the end); state: -1 not started, 0 off,
 * 1 on (the worker may still be scanning: the pack is used once the scan is done). */
static volatile int scanned;
static void scan_pack(void) {
    const char *e = getenv("UG_HDTEX");
    scan(e, 0);
    LOG("[hdtex] %d textures in %s\n", npacks, e);
    scanned = 1;
}
static void start_worker(void);
int hdtex_enabled(void) {
    if (state >= 0) return state;
    const char *e = getenv("UG_HDTEX");
    state = 0;
    if (!e || !*e || !strcmp(e, "0")) return 0;
    state = 1;
    start_worker();
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

/* ---- palettized (I8) textures: the PS2 hashes rebuilt exactly, as in Project Gauntlet's hdtex.c ----
 * The Xbox I8 texture keeps the PS2 indices and palette, so PCSX2's hashes can be computed:
 *  - TEX0 hash: XXH3-64 over the GS memory blocks in raster block order. PSMT8 block = 16x16 texels (256 bytes),
 *    PSMT4 block = 32x16 (two texels per byte, low nibble first). Smaller than one block: linear indices.
 *  - CLUT hash: XXH3-64 over the palette as PS2 RGBA8 words (R low) with GS alpha (0x80 = opaque):
 *    Xbox alpha 0xFF -> 0x80, else (a + 1) / 2. 256 entries for PSMT8, 16 for PSMT4.
 * PSMT8 is tried first, PSMT4 when every index is < 16. No CLUT match but files for that TEX0 hash: the one whose
 * thumbnail is closest to the texture (palette variants) is used. */
static unsigned off8(unsigned x, unsigned y) {
    unsigned c = y >> 2, r = y & 3, xs = x ^ ((((r >> 1) ^ c) & 1) * 4);
    return c * 64 + (xs & 1) * 4 + ((xs >> 1) & 3) * 16 + ((x >> 3) & 1) * 2 + (r & 1) * 8 + ((r >> 1) & 1);
}
static unsigned off4(unsigned x, unsigned y) {
    unsigned c = y >> 2, r = y & 3, xs = x ^ ((((r >> 1) ^ c) & 1) * 4);
    return c * 128 + (xs & 1) * 8 + ((xs >> 1) & 3) * 32 + ((x >> 3) & 3) * 2 + (r & 1) * 16 + ((r >> 1) & 1);
}
static uint64_t tex_hash(const uint8_t *idx, int w, int h, int psmt4) {
    int bw = psmt4 ? 32 : 16, bh = 16;
    if (w < bw || h < bh) return XXH3_64bits(idx, (size_t)w * h);
    static unsigned short o8[256], o4[512];
    if (!o8[1]) {
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) o8[y * 16 + x] = (unsigned short)off8(x, y);
            for (unsigned x = 0; x < 32; x++) o4[y * 32 + x] = (unsigned short)off4(x, y);
        }
    }
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    uint8_t blk[256];
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            if (psmt4) {
                memset(blk, 0, sizeof blk);
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 32; x++) {
                        unsigned n = o4[y * 32 + x];
                        blk[n >> 1] |= (uint8_t)((idx[(by + y) * w + bx + x] & 15) << ((n & 1) * 4));
                    }
            } else {
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++) blk[o8[y * 16 + x]] = idx[(by + y) * w + bx + x];
            }
            XXH3_64bits_update(&st, blk, sizeof blk);
        }
    return XXH3_64bits_digest(&st);
}
static int log2i(int v) { int n = 0; while ((1 << n) < v) n++; return n; }

/* thumbnail distance of pack texture i to the RGBA texture (UINT32_MAX: no thumbnail / other shape) */
static uint32_t thumb_dist(int i, const uint32_t *rgba, int w, int h) {
    int tw = w >= h ? T : T * w / h, th = h >= w ? T : T * h / w;
    if (packs[i].tw != tw || packs[i].th != th || tw < 1 || th < 1 || w % tw || h % th) return UINT32_MAX;
    int sx = w / tw, sy = h / th;
    uint32_t d = 0;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            uint32_t acc[3] = {0};
            for (int yy = 0; yy < sy; yy++)
                for (int xx = 0; xx < sx; xx++) {
                    uint32_t c = rgba[(y * sy + yy) * w + x * sx + xx];
                    acc[0] += c & 255; acc[1] += c >> 8 & 255; acc[2] += c >> 16 & 255;
                }
            for (int ch = 0; ch < 3; ch++)
                d += (uint32_t)abs((int)(acc[ch] / (uint32_t)(sx * sy)) - (int)packs[i].thumb[(y * tw + x) * 3 + ch]);
        }
    return d;
}

int hdtex_find_indexed(const uint8_t *idx, int w, int h, const uint32_t *pal, uint32_t pal_len, const uint32_t *rgba) {
    if (!hdtex_enabled()) return -1;
    uint32_t gs[256] = {0};
    for (uint32_t k = 0; k < pal_len && k < 256; k++) {
        uint32_t c = pal[k], a = c >> 24;   /* guest D3DCOLOR A8R8G8B8 */
        gs[k] = (c >> 16 & 255) | (c & 0xFF00) | (c & 255) << 16 | (a == 255 ? 0x80u : (a + 1) / 2) << 24;
    }
    uint8_t mx = 0;
    for (int i = 0, n = w * h; i < n; i++) if (idx[i] > mx) mx = idx[i];
    uint32_t size_bits = (uint32_t)log2i(w) << 6 | (uint32_t)log2i(h) << 10;
    uint64_t t8 = tex_hash(idx, w, h, 0), t4 = mx < 16 ? tex_hash(idx, w, h, 1) : 0;
    uint64_t c8 = XXH3_64bits(gs, (size_t)(pal_len < 256 ? pal_len : 256) * 4), c4 = XXH3_64bits(gs, 16 * 4);
    struct { uint64_t t, c; uint32_t psm; } tries[4] = {{t8, c8, 0x13}, {t4, c4, 0x14}, {t4, c8, 0x14}, {t8, c4, 0x13}};
    int id = -1;
    const char *how = "miss";
    for (int k = 0; k < 4 && id < 0; k++) {
        if (tries[k].psm == 0x14 && mx >= 16) continue;
        for (int i = 0; i < npacks; i++)
            if (packs[i].tex0 == tries[k].t && packs[i].clut == tries[k].c && packs[i].bits == (tries[k].psm | size_bits)) {
                id = i; how = "exact"; break;
            }
    }
    for (int k = 0; k < 2 && id < 0; k++) {   /* this TEX0 hash with another CLUT: closest thumbnail, or the only file */
        if (k == 1 && mx >= 16) break;
        uint64_t t = k ? t4 : t8;
        uint32_t bits = (k ? 0x14 : 0x13) | size_bits, bd = UINT32_MAX;
        int nfound = 0, first = -1;
        for (int i = 0; i < npacks; i++) {
            if (packs[i].tex0 != t || packs[i].bits != bits) continue;
            nfound++;
            if (first < 0) first = i;
            uint32_t d = rgba ? thumb_dist(i, rgba, w, h) : UINT32_MAX;
            if (d < bd) { bd = d; id = i; }
        }
        if (nfound == 1) { id = first; how = "tex0"; bd = rgba ? thumb_dist(first, rgba, w, h) : UINT32_MAX; }
        else if (id >= 0) how = "variant";
        /* the palette (colours) did not match: Create-a-Skater recolours clothes with other palettes over the same
         * indices, so take the file only when its colours match too (no thumbnail: no) */
        if (id >= 0) {
            int n = packs[id].tw * packs[id].th * 3;
            if (bd == UINT32_MAX || !n || (double)bd / n > 12.0) { id = -1; how = "colours differ"; }
        }
    }
    if (getenv("UG_HDLOG"))
        LOG("[hdtex] I8 %dx%d pal %u: t8 %llx clut %llx -> %s %s\n", w, h, pal_len, (unsigned long long)t8,
            (unsigned long long)c8, how, id >= 0 ? packs[id].path : "");
    return id;
}

/* ---- async: the pack scan, matching and file reads run on one worker thread, so the game never waits for them.
 * The renderer submits a new texture once (hdtex_submit, keyed by its content hash) and asks again on later frames
 * (hdtex_result); until then the original texture is drawn. Files are read ahead the same way (hdtex_file). ---- */
#ifdef _WIN32
static CRITICAL_SECTION mu;
static CONDITION_VARIABLE cv;
#define LOCK() EnterCriticalSection(&mu)
#define UNLOCK() LeaveCriticalSection(&mu)
#define WAIT() SleepConditionVariableCS(&cv, &mu, INFINITE)
#define WAKE() WakeConditionVariable(&cv)
#else
#include <pthread.h>
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mu)
#define UNLOCK() pthread_mutex_unlock(&mu)
#define WAIT() pthread_cond_wait(&cv, &mu)
#define WAKE() pthread_cond_signal(&cv)
#endif

typedef struct Job {
    uint64_t key; int w, h, file; uint32_t *rgba; uint8_t *idx; uint32_t pal[256], pal_len; struct Job *next;
    uint8_t *raw; int kind, bpp, i8; HdDecode dec;   /* raw: decode on the worker first */
} Job;
static Job *q_head, *q_tail;   /* file >= 0: read pack file `file`; else match */
#define RES_BITS 16
typedef struct { uint64_t key; int pack; int state; } Res;   /* state 1 queued, 2 done */
static Res res[1 << RES_BITS];
typedef struct { int pack; uint8_t *buf; size_t len; int state; } File;   /* state 1 queued, 2 ready */
static File files[256];

static Res *res_slot(uint64_t key, int add) {   /* under the lock */
    uint32_t i = (uint32_t)(key ^ key >> 32) & ((1 << RES_BITS) - 1);
    for (int n = 0; n < 64; n++, i = (i + 1) & ((1 << RES_BITS) - 1)) {
        if (res[i].key == key && res[i].state) return &res[i];
        if (!res[i].state) {
            if (!add) return NULL;
            res[i].key = key;
            return &res[i];
        }
    }
    return NULL;
}
static void push(Job *j) {
    LOCK();
    j->next = NULL;
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    WAKE();
    UNLOCK();
}
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
#else
static void *worker(void *unused) {
    (void)unused;
#endif
    scan_pack();
    for (;;) {
        LOCK();
        while (!q_head) WAIT();
        Job *j = q_head;
        q_head = j->next;
        if (!q_head) q_tail = NULL;
        UNLOCK();
        if (j->file >= 0) {
            size_t len = 0;
            uint8_t *b = hdtex_load(j->file, &len);
            LOCK();
            for (int i = 0; i < 256; i++)
                if (files[i].state == 1 && files[i].pack == j->file) { files[i].buf = b; files[i].len = len; files[i].state = 2; b = NULL; break; }
            UNLOCK();
            free(b);
        } else {
            if (j->raw) {   /* decode the guest texture here, off the render thread */
                j->rgba = malloc((size_t)j->w * j->h * 4);
                j->idx = j->i8 ? malloc((size_t)j->w * j->h) : NULL;
                if (j->rgba && (!j->i8 || j->idx)) j->dec(j->raw, j->kind, j->bpp, j->w, j->h, j->pal, j->rgba, j->idx);
                free(j->raw); j->raw = NULL;
            }
            int p = !j->rgba ? -1 : npacks ? (j->idx ? hdtex_find_indexed(j->idx, j->w, j->h, j->pal, j->pal_len, j->rgba)
                                     : hdtex_find(j->rgba, j->w, j->h)) : -1;
            LOCK();
            Res *r = res_slot(j->key, 0);
            if (r) { r->pack = p; r->state = 2; }
            UNLOCK();
        }
        free(j->rgba); free(j->idx); free(j);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void start_worker(void) {
#ifdef _WIN32
    InitializeCriticalSection(&mu);
    InitializeConditionVariable(&cv);
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
#else
    pthread_t t;
    if (!pthread_create(&t, NULL, worker, NULL)) pthread_detach(t);
#endif
}

/* -3 not submitted, -2 waiting, -1 no HD texture, >= 0 pack index */
int hdtex_result(uint64_t key) {
    if (!hdtex_enabled()) return -1;
    LOCK();
    Res *r = res_slot(key, 0);
    int v = !r ? -3 : r->state == 2 ? r->pack : -2;
    UNLOCK();
    return v;
}
/* takes ownership of rgba / idx (malloc'd); idx != NULL: palettized (exact PS2 hashes) */
void hdtex_submit(uint64_t key, uint32_t *rgba, uint8_t *idx, const uint32_t *pal, uint32_t pal_len, int w, int h) {
    LOCK();
    Res *r = res_slot(key, 1);
    int dup = !r || r->state;
    if (r && !r->state) { r->state = 1; r->pack = -1; }
    UNLOCK();
    if (dup) { free(rgba); free(idx); return; }
    Job *j = calloc(1, sizeof *j);
    j->key = key; j->w = w; j->h = h; j->file = -1; j->rgba = rgba; j->idx = idx; j->pal_len = pal_len;
    if (pal) memcpy(j->pal, pal, (pal_len < 256 ? pal_len : 256) * 4);
    push(j);
}
/* pack file `pack`: 1 = *buf / *len handed over (caller frees), 0 = being read (ask again next frame) */
int hdtex_file(int pack, uint8_t **buf, size_t *len) {
    LOCK();
    int free_slot = -1;
    for (int i = 0; i < 256; i++) {
        if (files[i].state && files[i].pack == pack) {
            if (files[i].state == 1) { UNLOCK(); return 0; }
            *buf = files[i].buf; *len = files[i].len;
            files[i].state = 0; files[i].buf = NULL;
            UNLOCK();
            return *buf != NULL;
        }
        if (!files[i].state && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) { UNLOCK(); return 0; }   /* 256 reads in flight: later */
    files[free_slot].pack = pack; files[free_slot].state = 1;
    UNLOCK();
    Job *j = calloc(1, sizeof *j);
    j->file = pack;
    push(j);
    return 0;
}
void hdtex_submit_raw(uint64_t key, uint8_t *raw, int kind, int bpp, int i8, int w, int h, const uint32_t *pal,
                      uint32_t pal_len, HdDecode dec) {
    LOCK();
    Res *r = res_slot(key, 1);
    int dup = !r || r->state;
    if (r && !r->state) { r->state = 1; r->pack = -1; }
    UNLOCK();
    if (dup) { free(raw); return; }
    Job *j = calloc(1, sizeof *j);
    j->key = key; j->w = w; j->h = h; j->file = -1; j->raw = raw; j->kind = kind; j->bpp = bpp; j->i8 = i8; j->dec = dec;
    j->pal_len = pal_len;
    if (pal) memcpy(j->pal, pal, (pal_len < 256 ? pal_len : 256) * 4);
    push(j);
}
