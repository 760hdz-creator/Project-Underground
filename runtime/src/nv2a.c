/* NV2A register emulation for the recompiled XDK D3D library.
 * Only the D3D section's memory accesses reach here (lifter emits RD32X/WR32X there).
 * Registers default to RAM semantics (backed by the dummy HW range in guest memory);
 * the cases below override reads and writes where the driver waits on hardware.
 */
#include <stdlib.h>
#include <windows.h>
#include <string.h>
#include "rt.h"

volatile int g_fps = -1;   /* flip pacing (UG_FPS; the in-game menu sets 30 / 60) */
#include "kelvin.h"

#define NV_BASE         0xFD000000u
#define NV_SIZE         0x01000000u

#define NV_PMC_BOOT_0   0x000000u
#define NV_PTIMER_TIME0 0x009400u
#define NV_PTIMER_TIME1 0x009410u
#define NV_PFB_WBC      0x100410u     /* bit 16: write-back cache flush pending */
#define NV_USER_PUT     0x800040u     /* channel 0 DMA put (physical) */
#define NV_USER_GET     0x800044u     /* channel 0 DMA get (physical) */

static int nv_log = -1;
static uint8_t *seen;                 /* one bit per register dword, for first-touch logging */

static uint32_t *reg(uint32_t off) { return (uint32_t *)(g_mem + NV_BASE + (off & ~3u)); }

static void touch(uint32_t a, int write, uint32_t v) {
    if (nv_log < 0) {
        const char *e = getenv("UG_NVLOG");
        nv_log = e && *e && *e != '0';
        if (nv_log) seen = calloc(NV_SIZE / 32 + 1, 1);
    }
    if (!nv_log) return;
    uint32_t i = ((a - NV_BASE) & (NV_SIZE - 1)) >> 2;
    uint8_t bit = (uint8_t)(1u << (i & 7));
    if (nv_log > 1 || !(seen[i >> 3] & bit)) {
        if (nv_log == 1) seen[i >> 3] |= bit;
        LOG("[nv2a] %s %08X = %08X\n", write ? "W" : "R", a, v);
    }
}

/* ---- PGRAPH ---- */
static int pb_log = -1;
static uint32_t sub_class[8];          /* object handle bound to each subchannel */

#define NV_PRAMIN 0x700000u

/* Resolve an object handle through RAMHT (linear scan) to its PRAMIN instance offset; 0 if absent. */
static uint32_t ramht_lookup(uint32_t handle) {
    uint32_t ht = *reg(0x002210);
    uint32_t base = ((ht >> 4) & 0x1F) << 12, size = 0x1000u << ((ht >> 16) & 3);
    for (uint32_t e = base; e < base + size; e += 8)
        if (*reg(NV_PRAMIN + e) == handle && *reg(NV_PRAMIN + e + 4))
            return (*reg(NV_PRAMIN + e + 4) & 0xFFFF) << 4;
    return 0;
}

/* Base physical address of a DMA context object, by handle. */
static uint32_t dma_base(uint32_t handle) {
    uint32_t inst = ramht_lookup(handle);
    if (!inst) { LOG("[pb] DMA handle %X not in RAMHT\n", handle); return 0; }
    uint32_t w0 = *reg(NV_PRAMIN + inst), w2 = *reg(NV_PRAMIN + inst + 8);
    if (pb_log) LOG("[pb] DMA %X inst %X: %08X %08X %08X\n", handle, inst, w0, *reg(NV_PRAMIN + inst + 4), w2);
    static int dumped;
    if (pb_log && !dumped++) {
        for (uint32_t e = 0; e < 0x1000; e += 8)
            if (*reg(NV_PRAMIN + e + 4)) LOG("[pb] ramht %03X: %08X %08X\n", e, *reg(NV_PRAMIN + e), *reg(NV_PRAMIN + e + 4));
        for (uint32_t o = 0x1000; o < 0x20000; o += 16)
            if (*reg(NV_PRAMIN + o)) LOG("[pb] inst %05X: %08X %08X %08X %08X\n", o, *reg(NV_PRAMIN + o), *reg(NV_PRAMIN + o + 4),
                                         *reg(NV_PRAMIN + o + 8), *reg(NV_PRAMIN + o + 12));
    }
    return ((w2 & 0xFFFFF000u) | (w0 >> 20)) & 0x03FFFFFF;
}

/* Kelvin state the "GPU" acts on. */
static uint32_t k_dma_semaphore, k_semaphore_offset;
uint32_t g_kreg[0x800];
uint32_t g_vp_prog[VP_SLOTS][4];
float g_vp_const[VP_CONSTS][4];
uint32_t g_vp_start, g_vp_serial, g_vc_serial;
static uint32_t vp_load, vc_load;   /* upload cursors in dwords */

uint32_t nv2a_dma_base(uint32_t handle) {
    static uint32_t cache_h[16], cache_b[16];
    static int n;
    for (int i = 0; i < n; i++) if (cache_h[i] == handle) return cache_b[i];
    uint32_t b = dma_base(handle);
    if (n < 16) { cache_h[n] = handle; cache_b[n] = b; n++; }
    LOG("[pb] DMA handle %X base %08X\n", handle, b);
    return b;
}

uint64_t g_flip_exit;   /* end of the last flip, pacing wait included (gpu_vulkan.c: busy time) */

/* ---- vertex fetch and primitive assembly ---- */
static float k_cur[16][4];             /* current (inline) attribute values for disabled arrays */
static uint32_t k_prim;                /* NV097_SET_BEGIN_END op, 0 = outside begin/end */
static KVertex *kv; static uint32_t kv_n, kv_cap;
static uint32_t *ki; static uint32_t ki_n, ki_cap;
static uint32_t *kelem; static uint32_t kelem_n, kelem_cap;   /* ARRAY_ELEMENT indices */

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t sz) {
    if (need <= *cap) return p;
    uint32_t c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    p = realloc(p, c * sz);
    if (!p) rt_fatal("nv2a: out of memory");
    *cap = c;
    return p;
}

static float s16n(int16_t v) { float f = v / 32767.0f; return f < -1.f ? -1.f : f; }

static inline uint16_t ld16(const uint8_t *q) { uint16_t v; memcpy(&v, q, 2); return v; }
static inline uint32_t ld32(const uint8_t *q) { uint32_t v; memcpy(&v, q, 4); return v; }

/* One vertex attribute of NV097 type/size at host pointer q -> float4 */
static void decode_attr(int a, uint32_t type, uint32_t size, const uint8_t *q, float *o) {
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    switch (type) {
    case 0:  /* UB_D3D: D3DCOLOR (B,G,R,A bytes) -> R,G,B,A */
        o[0] = q[2] / 255.f; o[1] = q[1] / 255.f; o[2] = q[0] / 255.f; o[3] = q[3] / 255.f;
        if (size < 4) o[3] = 1;
        break;
    case 4:  /* UB_OGL */
        for (uint32_t i = 0; i < size && i < 4; i++) o[i] = q[i] / 255.f;
        break;
    case 1:  /* S1: signed normalized short */
        for (uint32_t i = 0; i < size && i < 4; i++) o[i] = s16n((int16_t)ld16(q + 2 * i));
        break;
    case 5:  /* S32K: short */
        for (uint32_t i = 0; i < size && i < 4; i++) o[i] = (float)(int16_t)ld16(q + 2 * i);
        break;
    case 2:  /* F */
        for (uint32_t i = 0; i < size && i < 4; i++) { uint32_t u = ld32(q + 4 * i); memcpy(&o[i], &u, 4); }
        break;
    case 6: { /* CMP: 11:11:10 signed normalized */
        uint32_t v = ld32(q);
        o[0] = ((int32_t)(v << 21) >> 21) / 1023.f;
        o[1] = ((int32_t)(v << 10) >> 21) / 1023.f;
        o[2] = ((int32_t)v >> 22) / 511.f;
        break;
    }
    default: {
        static int warned;
        if (!warned++) LOG("[pb] vertex attribute %d: unknown type %u\n", a, type);
        break;
    }
    }
}

/* bytes one attribute occupies in an inline array (dword aligned) */
static uint32_t attr_bytes(uint32_t type, uint32_t size) {
    uint32_t b = type == 0 || type == 4 ? size : type == 1 || type == 5 ? 2 * size : type == 6 ? 4 : 4 * size;
    return (b + 3) & ~3u;
}

/* Vertex arrays: the enabled attributes, resolved once per batch; the disabled ones are constant (k_cur) and come from
 * the template vertex. */
typedef struct { uint32_t a, type, size, stride, addr, past; } VtxAttr;
static VtxAttr va[16];
static int n_va;
static KVertex va_template;
static float u8f[256];

static void vertex_setup(void) {
    if (!u8f[255]) for (int i = 0; i < 256; i++) u8f[i] = i / 255.f;
    n_va = 0;
    memcpy(&va_template, k_cur, sizeof va_template);
    for (uint32_t a = 0; a < 16; a++) {
        uint32_t fmt = KREG(K_VTX_FORMAT + a * 4), size = (fmt >> 4) & 0xF;
        if (!size) continue;
        uint32_t off = KREG(0x1720 + a * 4);
        VtxAttr *t = &va[n_va++];
        t->a = a; t->type = fmt & 0xF; t->size = size; t->stride = fmt >> 8;
        t->addr = nv2a_dma_base(KREG((off & 0x80000000u) ? K_DMA_VERTEX_B : K_DMA_VERTEX_A)) + (off & 0x7FFFFFFF);
        t->past = (off & 0x7FFFFFFF) - (KREG(0x1720) & 0x7FFFFFFF) + 4 * size > t->stride;
    }
}

static void fetch_vertex(uint32_t index, KVertex *out) {
    static float last[16][4];
    memcpy(out, &va_template, sizeof *out);
    for (int k = 0; k < n_va; k++) {
        const VtxAttr *t = &va[k];
        const uint8_t *q = g_mem + ((t->addr + index * t->stride) & 0x03FFFFFF);
        float *o = out->a[t->a];
        if (t->type == 0 && t->size == 4) { o[0] = u8f[q[2]]; o[1] = u8f[q[1]]; o[2] = u8f[q[0]]; o[3] = u8f[q[3]]; continue; }
        if (t->type != 2) { decode_attr((int)t->a, t->type, t->size, q, o); continue; }
        /* float garbage (denormal / NaN / inf): an attribute that points at the next vertex in the array (THUG's cutscene
         * shadow receivers: stride 24, texcoords at +0x18 / +0x20) reads past the last vertex; take the previous vertex's
         * value (smeared, it drew dark shadow wedges). A denormal inside the vertex is real data (NJ has texcoords with
         * u = 5.6e-45): kept. Bit tests: above 1e30 (0x7149F2CA) covers inf and NaN; 1e-30 is 0x0DA24260. */
        uint32_t w[4] = {0, 0, 0, 0x3F800000u}, n = t->size < 4 ? t->size : 4;
        int bad = 0;
        for (uint32_t i = 0; i < n; i++) {
            w[i] = ld32(q + 4 * i);
            uint32_t m = w[i] & 0x7FFFFFFF;
            bad |= m > 0x7149F2CAu || (t->past && m && m < 0x0DA24260u);
        }
        memcpy(o, w, 16);
        if (bad) memcpy(o, last[t->a], 16); else memcpy(last[t->a], o, 16);
    }
}

static void assemble(uint32_t prim, uint32_t b, uint32_t n);

/* INLINE_ARRAY: vertices packed in the push buffer, enabled attributes in order */
static uint32_t *kinl;
static uint32_t kinl_n, kinl_cap;

static void inline_array_flush(void) {
    uint32_t vsize = 0;
    for (int a = 0; a < 16; a++) {
        uint32_t fmt = KREG(K_VTX_FORMAT + a * 4);
        if ((fmt >> 4) & 0xF) vsize += attr_bytes(fmt & 0xF, (fmt >> 4) & 0xF);
    }
    if (!vsize) { kinl_n = 0; return; }
    uint32_t n = kinl_n * 4 / vsize;
    kv = grow(kv, &kv_cap, kv_n + n, sizeof(KVertex));
    uint32_t b = kv_n;
    const uint8_t *q = (const uint8_t *)kinl;
    for (uint32_t v = 0; v < n; v++) {
        KVertex *out = &kv[kv_n++];
        for (int a = 0; a < 16; a++) {
            uint32_t fmt = KREG(K_VTX_FORMAT + a * 4), type = fmt & 0xF, size = (fmt >> 4) & 0xF;
            if (!size) { memcpy(out->a[a], k_cur[a], 16); continue; }
            decode_attr(a, type, size, q, out->a[a]);
            q += attr_bytes(type, size);
        }
    }
    assemble(k_prim, b, n);
    kinl_n = 0;
}

/* Append indices for a primitive segment of n vertices starting at vertex b. */
static void assemble(uint32_t prim, uint32_t b, uint32_t n) {
    ki = grow(ki, &ki_cap, ki_n + n * 6 + 8, 4);
    uint32_t *o = ki + ki_n;
    switch (prim) {
    case 1: case 2: case 5: for (uint32_t i = 0; i < n; i++) *o++ = b + i; break;          /* points, lines, tris */
    case 3: case 4:                                                                       /* line loop / strip */
        for (uint32_t i = 0; i + 1 < n; i++) { *o++ = b + i; *o++ = b + i + 1; }
        if (prim == 3 && n > 2) { *o++ = b + n - 1; *o++ = b; }
        break;
    case 6:                                                                               /* triangle strip */
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) { *o++ = b + i + 1; *o++ = b + i; } else { *o++ = b + i; *o++ = b + i + 1; }
            *o++ = b + i + 2;
        }
        break;
    case 7: case 10:                                                                      /* fan, polygon */
        for (uint32_t i = 1; i + 1 < n; i++) { *o++ = b; *o++ = b + i; *o++ = b + i + 1; }
        break;
    case 8:                                                                               /* quads */
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            *o++ = b + i; *o++ = b + i + 1; *o++ = b + i + 2;
            *o++ = b + i; *o++ = b + i + 2; *o++ = b + i + 3;
        }
        break;
    case 9:                                                                               /* quad strip */
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            *o++ = b + i; *o++ = b + i + 1; *o++ = b + i + 3;
            *o++ = b + i; *o++ = b + i + 3; *o++ = b + i + 2;
        }
        break;
    default: break;
    }
    ki_n = (uint32_t)(o - ki);
}

static void draw_arrays(uint32_t data) {
    uint32_t start = data & 0xFFFFFF, n = (data >> 24) + 1;
    kv = grow(kv, &kv_cap, kv_n + n, sizeof(KVertex));
    uint32_t b = kv_n;
    vertex_setup();
    for (uint32_t i = 0; i < n; i++) fetch_vertex(start + i, &kv[kv_n++]);
    assemble(k_prim, b, n);
}

static void end_primitive(void) {
    if (kinl_n) inline_array_flush();
    if (kelem_n) {
        kv = grow(kv, &kv_cap, kv_n + kelem_n, sizeof(KVertex));
        uint32_t b = kv_n;
        vertex_setup();
        for (uint32_t i = 0; i < kelem_n; i++) fetch_vertex(kelem[i], &kv[kv_n++]);
        assemble(k_prim, b, kelem_n);
        kelem_n = 0;
    }
    if (ki_n) {
        int topo = k_prim <= 1 ? KTOPO_POINTS : k_prim <= 4 ? KTOPO_LINES : KTOPO_TRIS;
        gpu_draw(topo, kv, kv_n, ki, ki_n);
    }
    kv_n = ki_n = 0;
}

static void add_element(uint32_t index) {
    kelem = grow(kelem, &kelem_cap, kelem_n + 1, 4);
    kelem[kelem_n++] = index;
}

static void set_cur(int a, float x, float y, float z, float w) {
    k_cur[a][0] = x; k_cur[a][1] = y; k_cur[a][2] = z; k_cur[a][3] = w;
}
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Inline vertex data methods that set "current" attribute values. */
static void inline_attr(uint32_t method, uint32_t data) {
    if (method >= 0x1A00 && method < 0x1B00) {                 /* SET_VERTEX_DATA4F_M[16][4] */
        uint32_t a = (method - 0x1A00) >> 4, c = (method >> 2) & 3;
        k_cur[a][c] = u2f(data);
    } else if (method >= 0x1880 && method < 0x1900) {          /* SET_VERTEX_DATA2F_M[16][2] */
        uint32_t a = (method - 0x1880) >> 3, c = (method >> 2) & 1;
        k_cur[a][c] = u2f(data);
        if (c == 1) { k_cur[a][2] = 0; k_cur[a][3] = 1; }
    } else if (method >= 0x1940 && method < 0x1980) {          /* SET_VERTEX_DATA4UB[16] */
        uint32_t a = (method - 0x1940) >> 2;
        set_cur(a, (data & 0xFF) / 255.f, ((data >> 8) & 0xFF) / 255.f, ((data >> 16) & 0xFF) / 255.f, (data >> 24) / 255.f);
    } else if (method >= 0x1900 && method < 0x1940) {          /* SET_VERTEX_DATA2S[16] */
        uint32_t a = (method - 0x1900) >> 2;
        set_cur(a, (float)(int16_t)data, (float)(int16_t)(data >> 16), 0, 1);
    } else if (method == 0x156C) {                             /* SET_DIFFUSE_COLOR4UB (D3DCOLOR) */
        set_cur(3, ((data >> 16) & 0xFF) / 255.f, ((data >> 8) & 0xFF) / 255.f, (data & 0xFF) / 255.f, (data >> 24) / 255.f);
    } else if (method == 0x158C) {                             /* SET_SPECULAR_COLOR4UB */
        set_cur(4, ((data >> 16) & 0xFF) / 255.f, ((data >> 8) & 0xFF) / 255.f, (data & 0xFF) / 255.f, (data >> 24) / 255.f);
    } else if (method >= 0x1550 && method < 0x1560) {          /* SET_DIFFUSE_COLOR4F */
        k_cur[3][(method >> 2) & 3] = u2f(data);
    } else if (method >= 0x1570 && method < 0x1580) {          /* SET_SPECULAR_COLOR4F */
        k_cur[4][(method >> 2) & 3] = u2f(data);
    }
}

static int pb_frame = -1, pb_clears;   /* UG_PBFRAME=N: log methods between clear N and N+1 */

/* UG_PBSTATS=<s>: every s seconds, a histogram of Kelvin methods and the distinct values of the state that sizes the
 * renderer (transform mode, lighting, combiners, texture/vertex/surface formats). Research only. */
static uint32_t st_hist[0x800];
#define ST_VALS 24
static const uint32_t st_watch[] = {0x1E94, 0x0314, 0x1E60, 0x1E70, 0x0208, 0x1B04, 0x1B44, 0x1B84, 0x1BC4, 0x0300,
                                    0x0304, 0x029C, 0x02A4, 0x17FC, 0x1E98, 0x1EA0};
static uint32_t st_val[sizeof st_watch / 4][ST_VALS], st_nval[sizeof st_watch / 4];
static uint64_t st_next;
static int st_period = -1;

static void stats_method(uint32_t method, uint32_t data) {
    st_hist[method >> 2]++;
    for (unsigned w = 0; w < sizeof st_watch / 4; w++) {
        if (st_watch[w] != method) continue;
        uint32_t i = 0;
        while (i < st_nval[w] && st_val[w][i] != data) i++;
        if (i == st_nval[w] && i < ST_VALS) st_val[w][st_nval[w]++] = data;
    }
}

static void stats_dump(void) {
    if (st_period < 0) { const char *e = getenv("UG_PBSTATS"); st_period = e ? atoi(e) : 0; }
    if (!st_period) return;
    uint64_t now = sched_now_100ns();
    if (now < st_next) return;
    st_next = now + (uint64_t)st_period * 10000000ull;
    LOG("[pbstats] t=%.1f s, clears %d\n", now / 1e7, pb_clears);
    for (int k = 0; k < 40; k++) {   /* top methods */
        uint32_t best = 0, bm = 0;
        for (uint32_t m = 0; m < 0x800; m++) if (st_hist[m] > best) { best = st_hist[m]; bm = m; }
        if (!best) break;
        LOG("[pbstats]   %04X x%u\n", bm << 2, best);
        st_hist[bm] = 0;
    }
    memset(st_hist, 0, sizeof st_hist);
    for (unsigned w = 0; w < sizeof st_watch / 4; w++) {
        char line[512]; int n = 0;
        for (uint32_t i = 0; i < st_nval[w] && n < 480; i++) n += snprintf(line + n, sizeof line - n, " %X", st_val[w][i]);
        line[n] = 0;
        if (st_nval[w]) LOG("[pbstats]   %04X:%s\n", st_watch[w], line);
    }
}

static void pgraph_method(uint32_t subch, uint32_t method, uint32_t data) {
    if (pb_frame < 0) { const char *e = getenv("UG_PBFRAME"); pb_frame = e ? atoi(e) : 0; }
    if (subch == 0 && method == 0x1D94) pb_clears++;
    if (pb_log || (pb_frame && (pb_clears == pb_frame || subch != 0)))
        LOG("[pb] sub%u %04X = %08X\n", subch, method, data);
    if (method == 0) { sub_class[subch] = data; return; }
    if (subch != 0) return;   /* subchannel 0 = Kelvin (3D) */
    if (st_period) stats_method(method, data);
    g_kreg[method >> 2] = data;
    if (method >= 0x1500 && method < 0x1B00 && method != 0x17FC && method != 0x1800 && method != 0x1808 &&
        method != 0x1810 && method != 0x1818 && (method < 0x1720 || method >= 0x17A0))
        inline_attr(method, data);
    if (method >= 0x0B00 && method < 0x0B80) {                          /* SET_TRANSFORM_PROGRAM[32] */
        if (vp_load < VP_SLOTS * 4) { ((uint32_t *)g_vp_prog)[vp_load++] = data; g_vp_serial++; }
        return;
    }
    if (method >= 0x0B80 && method < 0x0C00) {                          /* SET_TRANSFORM_CONSTANT[32] */
        if (vc_load < VP_CONSTS * 4) { memcpy(&((float *)g_vp_const)[vc_load++], &data, 4); g_vc_serial++; }
        return;
    }
    if ((method >= 0x0AF0 && method < 0x0B00) || (method >= 0x0A20 && method < 0x0A30)) {
        /* SET_VIEWPORT_SCALE / _OFFSET live in constants 58 / 59 (as xemu), where the D3D epilogue of vertex programs reads
         * them. THUG's bone uploads overwrite both; the game restores them with these methods.
         * UG_VPCONST=0: off (debug). */
        static int on = -1;
        if (on < 0) { const char *e = getenv("UG_VPCONST"); on = !(e && *e == '0'); }
        float *c = &g_vp_const[method >= 0x0AF0 ? 58 : 59][(method >> 2) & 3];
        if (on && memcmp(c, &data, 4)) { memcpy(c, &data, 4); g_vc_serial++; }
    }
    switch (method) {
    case 0x17FC:                                                        /* SET_BEGIN_END */
        if (data == 0) end_primitive();
        else { kv_n = ki_n = kelem_n = kinl_n = 0; }
        k_prim = data;
        break;
    case 0x1810: draw_arrays(data); break;                              /* DRAW_ARRAYS */
    case 0x1800: add_element(data & 0xFFFF); add_element(data >> 16); break;   /* ARRAY_ELEMENT16 pair */
    case 0x1808: add_element(data); break;                              /* ARRAY_ELEMENT32 */
    case 0x1818:                                                        /* INLINE_ARRAY */
        kinl = grow(kinl, &kinl_cap, kinl_n + 1, 4);
        kinl[kinl_n++] = data;
        break;
    case 0x1D94: gpu_clear(data); break;                                /* CLEAR_SURFACE */
    case 0x012C:                                                        /* FLIP_INCREMENT_WRITE */
        /* XDK Swap: 1D8C (scratch) holds the surface offset just completed */
        gpu_flip((nv2a_dma_base(KREG(K_DMA_COLOR)) + KREG(K_ZSTENCIL_CLEAR)) & 0x03FFFFFF);
        {   /* UG_TMRLOG=1: the game's own frame timer (swap callback 0x173ED0, vblank callback 0x173E90) */
            static int tl = -1;
            static uint32_t tn;
            if (tl < 0) tl = getenv("UG_TMRLOG") != NULL;
            if (tl && ++tn % 300 == 0)
                LOG("[tmr] flips %u vblanks %u swaps %u last SwapVBlank %u ring %d %d %d %d ratio %08X cbs %u %u\n", tn, RD32(0x35F0C0),
                    RD32(0x35F0D0), RD32(0x35F050), (int)RD32(0x35F040), (int)RD32(0x35F044), (int)RD32(0x35F048),
                    (int)RD32(0x35F04C), RD32(0x35F030), RD8(0x35F0E0), RD8(0x35F0E1));
        }
        {   /* in-game menu (menu.c): a modal loop here pauses every guest thread */
            extern volatile int g_menu_req;
            extern void menu_run(void);
            if (g_menu_req) menu_run();
        }
        {   /* The NV2A flips at vblank: pace flips to UG_FPS (default 60; 0 = unlimited; menu.c changes it). Only this
             * guest thread waits (sched_sleep_until runs the others). */
            static uint64_t next;
            if (g_fps < 0) { const char *e = getenv("UG_FPS"); g_fps = e ? atoi(e) : 60; }
            /* the game never steps less than 1/60 s per frame (0x174030): faster flips would speed it up */
            int fps = g_fps;
            static int unlock = -1;   /* UG_FPSUNLOCK=1: experiments */
            if (unlock < 0) unlock = getenv("UG_FPSUNLOCK") != NULL;
            if (!unlock && (fps <= 0 || fps > 60)) fps = 60;
            if (fps > 0) {
                uint64_t period = 10000000ull / (uint64_t)fps, now = sched_now_100ns();
                if (!next || now > next + 4 * period) next = now;   /* first flip or a long stall: restart */
                next += period;
                if (next > now) sched_sleep_until(next);
            }
            /* Frame length. The game steps by the vblanks between its last 4 swaps (ring 0x35F040, filled by its D3D
             * swap callback 0x173ED0, averaged and clamped to 4 in 0x173F80). The flip interrupt that calls it does
             * not exist here, so the ring stayed at 1 and the game ran at half speed at 30 fps and slowed down on any drop:
             * fill it from the real time between flips, whole 60 Hz vblanks with the rest carried over.
             * UG_FRAMESTEP=0: off (every flip is one step). */
            static int fs = -1, ri;
            static uint64_t last;
            static double acc;
            if (fs < 0) { const char *e = getenv("UG_FRAMESTEP"); fs = !(e && *e == '0'); }
            uint64_t t = sched_now_100ns();
            if (fs && last && RD8(0x35F0E1)) {   /* the game has registered its callback */
                acc += (double)(t - last) * 60.0 / 1e7;
                int d = (int)(acc + 0.5);
                acc -= d;
                if (d > 4) { d = 4; acc = 0; }   /* a stall: the game's own limit */
                WR32(0x35F040u + (uint32_t)ri * 4, (uint32_t)d);
                ri = (ri + 1) & 3;
            }
            last = t;
            g_flip_exit = t;
        }
        break;
    case 0x01A4: k_dma_semaphore = data; break;                         /* SET_CONTEXT_DMA_SEMAPHORE */
    case 0x1E9C: vp_load = data * 4; break;                             /* SET_TRANSFORM_PROGRAM_LOAD */
    case 0x1EA0: g_vp_start = data; break;                              /* SET_TRANSFORM_PROGRAM_START */
    case 0x1EA4: vc_load = data * 4; break;                             /* SET_TRANSFORM_CONSTANT_LOAD */
    case 0x1D6C: k_semaphore_offset = data; break;                      /* SET_SEMAPHORE_OFFSET */
    case 0x1D70: {                                                      /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        uint32_t a = dma_base(k_dma_semaphore) + k_semaphore_offset;
        WR32(a, data);
        if (pb_log) LOG("[pb] semaphore %08X <- %X (dev %08X [34] %08X)\n", a, data, RD32(0xD0170), RD32(RD32(0xD0170) + 0x34));
        break;
    }
    default: break;
    }
}

/* ---- PVIDEO overlay ---- */
static int ov_buffer;
static uint32_t ov_frames;

/* ---- PFIFO pusher: run the push buffer from GET to PUT ("infinitely fast" GPU) ---- */
static uint32_t pb_get, pb_ret;

static void pfifo_kick(uint32_t put) {
    if (pb_log < 0) { const char *e = getenv("UG_PBLOG"); pb_log = e && *e && *e != '0'; }
    put &= 0x03FFFFFC;
    uint32_t get = pb_get;
    for (int guard = 0; get != put; guard++) {
        if (guard > 4000000) { LOG("[pb] runaway push buffer at %08X (put %08X)\n", get, put); break; }
        uint32_t cmd = RD32(get);
        get += 4;
        if ((cmd & 0xE0000003) == 0x20000000) { get = cmd & 0x03FFFFFC; continue; }     /* old jump */
        if ((cmd & 3) == 1) { get = cmd & 0x03FFFFFC; continue; }                      /* jump */
        if ((cmd & 3) == 2) { pb_ret = get; get = cmd & 0x03FFFFFC; continue; }         /* call */
        if (cmd == 0x00020000) { get = pb_ret; continue; }                             /* return */
        uint32_t kind = cmd & 0xE0030003;
        if (kind != 0 && kind != 0x40000000) {
            LOG("[pb] bad command %08X at %08X, skipping to put\n", cmd, get - 4);
            get = put;
            break;
        }
        uint32_t method = cmd & 0x1FFC, subch = (cmd >> 13) & 7, count = (cmd >> 18) & 0x7FF;
        for (uint32_t i = 0; i < count; i++) {
            pgraph_method(subch, method, RD32(get));
            get += 4;
            if (!kind) method += 4;
        }
    }
    pb_get = get;
    *reg(NV_USER_GET) = get;
    stats_dump();
}

uint32_t mmio_read(uint32_t a, int size) {
    uint32_t v;
    sched_maybe_yield();   /* register polling loops */
    if (a == 0xFE820010u) {
        /* APU: free-running sample counter (48 kHz), polled by DSOUND init */
        v = (uint32_t)(sched_now_100ns() * 48 / 10000) & ~3u;
    } else if (a < NV_BASE || a >= NV_BASE + NV_SIZE) {
        v = RD32(a);
        if (a < 0xFE800000u || a >= 0xFE880000u) LOG("[mmio] R%d %08X (non-NV2A)\n", size * 8, a);
    } else {
        uint32_t off = a - NV_BASE;
        switch (off & ~3u) {
        case NV_PMC_BOOT_0: v = 0x02A000A3; break;
        case 0x000100:                                    /* PMC_INTR_0: pending sources */
            v = (*reg(0x2100) & *reg(0x2140) ? 1u << 8 : 0) | (*reg(0x400100) & *reg(0x400140) ? 1u << 12 : 0) |
                (*reg(0x600100) & *reg(0x600140) ? 1u << 24 : 0);
            break;
        case NV_PTIMER_TIME0: v = (uint32_t)(sched_now_100ns() * 100) << 5; break;
        case NV_PTIMER_TIME1: v = (uint32_t)((sched_now_100ns() * 100) >> 27); break;
        case NV_PFB_WBC: v = *reg(off) &= ~0x10000u; break;
        /* the GPU is always idle: FIFO caches empty, pusher not busy */
        case 0x002400: v = *reg(off) | 0x10; break;       /* PFIFO_RUNOUT_STATUS: empty */
        case 0x003214: v = *reg(off) | 0x10; break;       /* CACHE1_STATUS: empty */
        case 0x003220: v = *reg(off) & ~0x10u; break;     /* CACHE1_DMA_PUSH: status idle */
        /* PVIDEO overlay (WMV playback): every queued buffer is consumed at once. The overlay
         * registers (0x8900+) keep their values for the host renderer. */
        case 0x008700: v = 0; break;                      /* PVIDEO_BUFFER: none in use */
        default: v = *reg(off); break;
        }
        touch(a, 0, v);
        v >>= (a & 3) * 8;
    }
    if (size == 1) return v & 0xFF;
    if (size == 2) return v & 0xFFFF;
    return v;
}

void mmio_write(uint32_t a, uint32_t v, int size) {
    if (a < NV_BASE || a >= NV_BASE + NV_SIZE) {
        if (a < 0xFE800000u || a >= 0xFE880000u) LOG("[mmio] W%d %08X = %08X (non-NV2A)\n", size * 8, a, v);
        if (size == 4) WR32(a, v); else if (size == 2) WR16(a, (uint16_t)v); else WR8(a, (uint8_t)v);
        return;
    }
    touch(a, 1, v);
    switch ((a - NV_BASE) & ~3u) {
    /* interrupt status registers: write 1 to clear */
    case 0x000100: return;  /* PMC_INTR_0 is derived */
    case 0x002100: case 0x009100: case 0x400100: case 0x600100:
        *reg(a - NV_BASE) &= ~(v << (a & 3) * 8);
        return;
    default: break;
    }
    if (size == 4) WR32(a, v); else if (size == 2) WR16(a, (uint16_t)v); else WR8(a, (uint8_t)v);
    switch ((a - NV_BASE) & ~3u) {
    case 0x600800: if (pb_frame > 0) LOG("[crtc] start %08X tid %lu\n", v, GetCurrentThreadId()); break;
    case NV_USER_PUT: pfifo_kick(v); break;
    case NV_USER_GET: pb_get = v & 0x03FFFFFC; break;
    case 0x008700:                                        /* PVIDEO_BUFFER: buffer queued for display */
        if (v & 0x01) ov_buffer = 0;
        if (v & 0x10) ov_buffer = 1;
        if (v & 0x11) ov_frames++;
        break;
    default: break;
    }
}

/* PVIDEO overlay state for the renderer (registers at 0x8900+, two buffers). */
int nv2a_overlay(NvOverlay *o) {
    if (!ov_frames || (*reg(0x008704) & 1)) return 0;     /* nothing queued yet, or PVIDEO_STOP */
    int b = ov_buffer;
    uint32_t size_in = *reg(0x008928 + b * 4), fmt = *reg(0x008958 + b * 4);
    uint32_t pt_out = *reg(0x008948 + b * 4), size_out = *reg(0x008950 + b * 4);
    o->addr = (*reg(0x008900 + b * 4) + *reg(0x008920 + b * 4)) & 0x03FFFFFF;
    o->w = size_in & 0x7FF; o->h = (size_in >> 16) & 0x7FF;
    o->pitch = fmt & 0x1FFF;
    o->ox = pt_out & 0xFFF; o->oy = (pt_out >> 16) & 0xFFF;
    o->ow = size_out & 0xFFF; o->oh = (size_out >> 16) & 0xFFF;
    o->frames = ov_frames;
    static int logged;
    if (!logged++) LOG("[ovl] buffer %d addr %08X %ux%u pitch %u fmt %08X out %u,%u %ux%u\n", b, o->addr, o->w, o->h,
                       o->pitch, fmt, o->ox, o->oy, o->ow, o->oh);
    return o->w && o->h && o->pitch;
}

/* ---- interrupts ---- */
#define NV_PMC_INTR     0x000100u
#define NV_PCRTC_INTR   0x600100u
#define NV_PCRTC_INTR_EN 0x600140u
#define NV_GPU_IRQ      3

void nv2a_vblank(void) {
    gpu_vblank();
    *reg(NV_PCRTC_INTR) |= 1;
    if (*reg(NV_PCRTC_INTR_EN) & 1) kernel_raise_irq(NV_GPU_IRQ);
}
