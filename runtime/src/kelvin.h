/* NV2A Kelvin (class 0x97) state shared between the push-buffer front end (nv2a.c)
 * and the host renderer backend (gpu_d3d11.c). Method offsets follow the public
 * NV097_* register map (names as in xemu's nv2a_regs.h / nouveau rnndb).
 */
#pragma once
#include <stdint.h>

/* Raw method register file: every Kelvin method write lands at g_kreg[method >> 2]. */
extern uint32_t g_kreg[0x800];
#define KREG(m) g_kreg[(m) >> 2]

/* method offsets used by the backend */
#define K_DMA_A            0x0184
#define K_DMA_B            0x0188
#define K_DMA_COLOR        0x0194
#define K_DMA_ZETA         0x0198
#define K_DMA_VERTEX_A     0x019C
#define K_DMA_VERTEX_B     0x01A0
#define K_CLIP_H           0x0200
#define K_CLIP_V           0x0204
#define K_SURF_FORMAT      0x0208
#define K_SURF_PITCH       0x020C
#define K_COLOR_OFFSET     0x0210
#define K_ZETA_OFFSET      0x0214
#define K_COMB_ALPHA_ICW   0x0260   /* [8] */
#define K_COMB_SPECFOG_CW0 0x0288
#define K_COMB_SPECFOG_CW1 0x028C
#define K_CONTROL0         0x0290
#define K_FOG_MODE         0x029C
#define K_FOG_GEN_MODE     0x02A0
#define K_FOG_ENABLE       0x02A4
#define K_FOG_COLOR        0x02A8
#define K_WINDOW_CLIP_H    0x02C0   /* [8] */
#define K_WINDOW_CLIP_V    0x02E0   /* [8] */
#define K_ALPHA_TEST_EN    0x0300
#define K_BLEND_EN         0x0304
#define K_CULL_EN          0x0308
#define K_DEPTH_TEST_EN    0x030C
#define K_LIGHTING_EN      0x0314
#define K_STENCIL_TEST_EN  0x032C
#define K_ALPHA_FUNC       0x033C
#define K_ALPHA_REF        0x0340
#define K_BLEND_SFACTOR    0x0344
#define K_BLEND_DFACTOR    0x0348
#define K_BLEND_COLOR      0x034C
#define K_BLEND_EQUATION   0x0350
#define K_DEPTH_FUNC       0x0354
#define K_COLOR_MASK       0x0358
#define K_DEPTH_MASK       0x035C
#define K_STENCIL_MASK     0x0360
#define K_STENCIL_FUNC     0x0364
#define K_STENCIL_REF      0x0368
#define K_STENCIL_FMASK    0x036C
#define K_STENCIL_OP_FAIL  0x0370
#define K_STENCIL_OP_ZFAIL 0x0374
#define K_STENCIL_OP_ZPASS 0x0378
#define K_CULL_FACE        0x039C
#define K_FRONT_FACE       0x03A0
#define K_TEXMAT_EN        0x0420   /* [4] */
#define K_COMPOSITE        0x0680   /* [16] floats */
#define K_TEXTURE_MATRIX   0x06C0   /* [4][16] floats */
#define K_SPECFOG_FACTOR0  0x1E20   /* final combiner constants c0, c1 */
#define K_SPECFOG_FACTOR1  0x1E24
#define K_FOG_PARAMS       0x09C0   /* [3] floats */
#define K_VIEWPORT_OFFSET  0x0A20   /* [4] floats */
#define K_COMB_FACTOR0     0x0A60   /* [8] */
#define K_COMB_FACTOR1     0x0A80   /* [8] */
#define K_COMB_ALPHA_OCW   0x0AA0   /* [8] */
#define K_COMB_COLOR_ICW   0x0AC0   /* [8] */
#define K_VIEWPORT_SCALE   0x0AF0   /* [4] floats */
#define K_VTX_FORMAT       0x1760   /* [16] */
#define K_TEX_OFFSET(i)    (0x1B00 + (i) * 0x40)
#define K_TEX_FORMAT(i)    (0x1B04 + (i) * 0x40)
#define K_TEX_ADDRESS(i)   (0x1B08 + (i) * 0x40)
#define K_TEX_CONTROL0(i)  (0x1B0C + (i) * 0x40)
#define K_TEX_CONTROL1(i)  (0x1B10 + (i) * 0x40)
#define K_TEX_FILTER(i)    (0x1B14 + (i) * 0x40)
#define K_TEX_RECT(i)      (0x1B1C + (i) * 0x40)
#define K_TEX_PALETTE(i)   (0x1B20 + (i) * 0x40)
#define K_TEX_BORDER(i)    (0x1B24 + (i) * 0x40)
#define K_ZSTENCIL_CLEAR   0x1D8C
#define K_COLOR_CLEAR      0x1D90
#define K_CLEAR_RECT_H     0x1D98
#define K_CLEAR_RECT_V     0x1D9C
#define K_COMB_COLOR_OCW   0x1E40   /* [8] */
#define K_COMB_CONTROL     0x1E60
#define K_SHADER_STAGE_PROG 0x1E70
#define K_SHADER_OTHER_IN  0x1E78
#define K_TRANSFORM_MODE   0x1E94   /* bits 1:0: 0 fixed function, 2 vertex program */
#define K_CLIP_MIN         0x0394   /* float: depth range min (Z-buffer units) */
#define K_CLIP_MAX         0x0398

static inline float kf(uint32_t m) { union { uint32_t u; float f; } x; x.u = KREG(m); return x.f; }

/* Decoded vertex: the 16 NV2A vertex attributes as float4.
 * 0 position, 1 weight, 2 normal, 3 diffuse, 4 specular, 5 fog, 6 point size,
 * 7 back diffuse, 8 back specular, 9-12 texcoord 0-3. */
typedef struct { float a[16][4]; } KVertex;

enum { KTOPO_POINTS, KTOPO_LINES, KTOPO_TRIS };

/* Vertex program state (nv2a.c): 136 instruction slots of 4 dwords, 192 float4 constants. */
#define VP_SLOTS 136
#define VP_CONSTS 192
extern uint32_t g_vp_prog[VP_SLOTS][4];
extern float g_vp_const[VP_CONSTS][4];
extern uint32_t g_vp_start;                  /* SET_TRANSFORM_PROGRAM_START */
extern uint32_t g_vp_serial, g_vc_serial;    /* bumped on every program / constant write (upload caching) */

/* Physical base of a DMA context object (nv2a.c). */
uint32_t nv2a_dma_base(uint32_t handle);

/* PVIDEO overlay (YUY2 video planes used by the WMV player) */
typedef struct { uint32_t addr, w, h, pitch, ox, oy, ow, oh, frames; } NvOverlay;
int nv2a_overlay(NvOverlay *o);              /* 1 if an overlay frame is shown */

/* ---- backend (gpu_d3d11.c) ---- */
void gpu_init(void);
void gpu_draw(int topo, const KVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni);
void gpu_clear(uint32_t flags);              /* NV097_CLEAR_SURFACE */
void gpu_flip(uint32_t surface_phys);        /* present the surface at this physical address */
void gpu_vblank(void);                       /* 60 Hz, DPC thread: presents the overlay when no 3D flips happen */
