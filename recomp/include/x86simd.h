/* MMX / SSE lane helpers for lifted code. Included by x86rt.h. */
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>

typedef union { uint64_t q; uint32_t d[2]; int32_t sd[2]; uint16_t w[4]; int16_t sw[4]; uint8_t b[8]; int8_t sb[8]; } M64;
typedef union { float f[4]; uint32_t u[4]; int32_t s[4]; uint64_t q[2]; } X128;

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }

#define M64_LANES(name, field, n, expr) \
    static inline uint64_t mmx_##name(uint64_t x, uint64_t y) { \
        M64 a = {x}, b = {y}, r; for (int i = 0; i < n; i++) r.field[i] = (expr); return r.q; }

M64_LANES(paddb, b, 8, (uint8_t)(a.b[i] + b.b[i]))
M64_LANES(paddw, w, 4, (uint16_t)(a.w[i] + b.w[i]))
M64_LANES(paddd, d, 2, a.d[i] + b.d[i])
M64_LANES(psubb, b, 8, (uint8_t)(a.b[i] - b.b[i]))
M64_LANES(psubw, w, 4, (uint16_t)(a.w[i] - b.w[i]))
M64_LANES(psubd, d, 2, a.d[i] - b.d[i])
M64_LANES(paddsw, sw, 4, sat16(a.sw[i] + b.sw[i]))
M64_LANES(psubsw, sw, 4, sat16(a.sw[i] - b.sw[i]))
M64_LANES(paddusw, w, 4, (uint16_t)((a.w[i] + b.w[i]) > 0xFFFF ? 0xFFFF : a.w[i] + b.w[i]))
M64_LANES(pmullw, w, 4, (uint16_t)(a.sw[i] * b.sw[i]))
M64_LANES(pcmpeqb, b, 8, a.b[i] == b.b[i] ? 0xFF : 0)
M64_LANES(pcmpeqw, w, 4, a.w[i] == b.w[i] ? 0xFFFF : 0)
M64_LANES(pcmpgtb, b, 8, a.sb[i] > b.sb[i] ? 0xFF : 0)
M64_LANES(pcmpgtw, w, 4, a.sw[i] > b.sw[i] ? 0xFFFF : 0)
M64_LANES(pcmpgtd, d, 2, a.sd[i] > b.sd[i] ? 0xFFFFFFFFu : 0)
M64_LANES(pavgb, b, 8, (uint8_t)((a.b[i] + b.b[i] + 1) >> 1))

M64_LANES(psubusw, w, 4, (uint16_t)(a.w[i] > b.w[i] ? a.w[i] - b.w[i] : 0))
M64_LANES(psubusb, b, 8, (uint8_t)(a.b[i] > b.b[i] ? a.b[i] - b.b[i] : 0))
M64_LANES(paddusb, b, 8, (uint8_t)((a.b[i] + b.b[i]) > 0xFF ? 0xFF : a.b[i] + b.b[i]))
M64_LANES(paddsb, sb, 8, (int8_t)(a.sb[i] + b.sb[i] > 127 ? 127 : a.sb[i] + b.sb[i] < -128 ? -128 : a.sb[i] + b.sb[i]))
M64_LANES(psubsb, sb, 8, (int8_t)(a.sb[i] - b.sb[i] > 127 ? 127 : a.sb[i] - b.sb[i] < -128 ? -128 : a.sb[i] - b.sb[i]))
M64_LANES(pmulhw, w, 4, (uint16_t)(((int32_t)a.sw[i] * b.sw[i]) >> 16))
M64_LANES(pmulhuw, w, 4, (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16))
M64_LANES(pminub, b, 8, a.b[i] < b.b[i] ? a.b[i] : b.b[i])
M64_LANES(pmaxub, b, 8, a.b[i] > b.b[i] ? a.b[i] : b.b[i])
M64_LANES(pminsw, sw, 4, a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i])
M64_LANES(pmaxsw, sw, 4, a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i])
M64_LANES(pavgw, w, 4, (uint16_t)((a.w[i] + b.w[i] + 1) >> 1))
static inline uint64_t mmx_packsswb(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r;
    for (int i = 0; i < 4; i++) {
        int v = a.sw[i], w = b.sw[i];
        r.sb[i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
        r.sb[i + 4] = (int8_t)(w > 127 ? 127 : w < -128 ? -128 : w);
    }
    return r.q;
}
static inline uint64_t mmx_psadbw(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}; uint32_t s = 0;
    for (int i = 0; i < 8; i++) s += a.b[i] > b.b[i] ? a.b[i] - b.b[i] : b.b[i] - a.b[i];
    return s;
}
/* SSE scalar min/max: the second operand wins on NaN and on equal (incl. +0/-0), like minss/maxss. */
static inline float x86_minf(float a, float b) { return a < b ? a : b; }
static inline float x86_maxf(float a, float b) { return a > b ? a : b; }

static inline uint64_t mmx_pand(uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mmx_pandn(uint64_t a, uint64_t b) { return ~a & b; }
static inline uint64_t mmx_por(uint64_t a, uint64_t b) { return a | b; }
static inline uint64_t mmx_pxor(uint64_t a, uint64_t b) { return a ^ b; }

static inline uint64_t mmx_pmaddwd(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r;
    r.sd[0] = a.sw[0] * b.sw[0] + a.sw[1] * b.sw[1];
    r.sd[1] = a.sw[2] * b.sw[2] + a.sw[3] * b.sw[3];
    return r.q;
}
static inline uint64_t mmx_packssdw(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r;
    r.sw[0] = sat16(a.sd[0]); r.sw[1] = sat16(a.sd[1]); r.sw[2] = sat16(b.sd[0]); r.sw[3] = sat16(b.sd[1]);
    return r.q;
}
static inline uint64_t mmx_packuswb(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r;
    for (int i = 0; i < 4; i++) { r.b[i] = satu8(a.sw[i]); r.b[i + 4] = satu8(b.sw[i]); }
    return r.q;
}
static inline uint64_t mmx_punpcklbw(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r;
    for (int i = 0; i < 4; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; }
    return r.q;
}
static inline uint64_t mmx_punpckhbw(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r;
    for (int i = 0; i < 4; i++) { r.b[2 * i] = a.b[i + 4]; r.b[2 * i + 1] = b.b[i + 4]; }
    return r.q;
}
static inline uint64_t mmx_punpcklwd(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r; r.w[0] = a.w[0]; r.w[1] = b.w[0]; r.w[2] = a.w[1]; r.w[3] = b.w[1]; return r.q;
}
static inline uint64_t mmx_punpckhwd(uint64_t x, uint64_t y) {
    M64 a = {x}, b = {y}, r; r.w[0] = a.w[2]; r.w[1] = b.w[2]; r.w[2] = a.w[3]; r.w[3] = b.w[3]; return r.q;
}
static inline uint64_t mmx_punpckldq(uint64_t x, uint64_t y) { return (x & 0xFFFFFFFFu) | (y << 32); }
static inline uint64_t mmx_punpckhdq(uint64_t x, uint64_t y) { return (x >> 32) | (y & 0xFFFFFFFF00000000ull); }

/* shifts: count is the low 64 bits of the source */
#define M64_SHIFT(name, field, n, bits, expr) \
    static inline uint64_t mmx_##name(uint64_t x, uint64_t cnt) { \
        M64 a = {x}, r; unsigned k = cnt > (bits) ? (bits) : (unsigned)cnt; \
        for (int i = 0; i < n; i++) r.field[i] = (expr); return r.q; }
M64_SHIFT(psllw, w, 4, 16, k >= 16 ? 0 : (uint16_t)(a.w[i] << k))
M64_SHIFT(pslld, d, 2, 32, k >= 32 ? 0 : a.d[i] << k)
M64_SHIFT(psrlw, w, 4, 16, k >= 16 ? 0 : (uint16_t)(a.w[i] >> k))
M64_SHIFT(psrld, d, 2, 32, k >= 32 ? 0 : a.d[i] >> k)
M64_SHIFT(psraw, sw, 4, 16, (int16_t)(a.sw[i] >> (k > 15 ? 15 : k)))
M64_SHIFT(psrad, sd, 2, 32, a.sd[i] >> (k > 31 ? 31 : k))
static inline uint64_t mmx_psllq(uint64_t x, uint64_t k) { return k > 63 ? 0 : x << k; }
static inline uint64_t mmx_psrlq(uint64_t x, uint64_t k) { return k > 63 ? 0 : x >> k; }

/* ---- SSE (packed single) ---- */
#define X128_LANES(name, expr) \
    static inline void sse_##name(X128 *d, X128 s) { X128 a = *d; for (int i = 0; i < 4; i++) d->f[i] = (expr); }
X128_LANES(addps, a.f[i] + s.f[i])
X128_LANES(subps, a.f[i] - s.f[i])
X128_LANES(mulps, a.f[i] * s.f[i])
X128_LANES(divps, a.f[i] / s.f[i])
X128_LANES(minps, a.f[i] < s.f[i] ? a.f[i] : s.f[i])
X128_LANES(maxps, a.f[i] > s.f[i] ? a.f[i] : s.f[i])
static inline void sse_andps(X128 *d, X128 s) { d->q[0] &= s.q[0]; d->q[1] &= s.q[1]; }
static inline void sse_orps(X128 *d, X128 s) { d->q[0] |= s.q[0]; d->q[1] |= s.q[1]; }
static inline void sse_xorps(X128 *d, X128 s) { d->q[0] ^= s.q[0]; d->q[1] ^= s.q[1]; }
static inline void sse_andnps(X128 *d, X128 s) { d->q[0] = ~d->q[0] & s.q[0]; d->q[1] = ~d->q[1] & s.q[1]; }
static inline void sse_unpcklps(X128 *d, X128 s) { X128 a = *d; d->u[0] = a.u[0]; d->u[1] = s.u[0]; d->u[2] = a.u[1]; d->u[3] = s.u[1]; }
static inline void sse_unpckhps(X128 *d, X128 s) { X128 a = *d; d->u[0] = a.u[2]; d->u[1] = s.u[2]; d->u[2] = a.u[3]; d->u[3] = s.u[3]; }
static inline void sse_movlhps(X128 *d, X128 s) { d->q[1] = s.q[0]; }
static inline void sse_movhlps(X128 *d, X128 s) { d->q[0] = s.q[1]; }
static inline void sse_shufps(X128 *d, X128 s, unsigned imm) {
    X128 a = *d;
    d->u[0] = a.u[imm & 3]; d->u[1] = a.u[(imm >> 2) & 3]; d->u[2] = s.u[(imm >> 4) & 3]; d->u[3] = s.u[(imm >> 6) & 3];
}
X128_LANES(sqrtps, sqrtf(s.f[i]))
X128_LANES(rcpps, 1.0f / s.f[i])
X128_LANES(rsqrtps, 1.0f / sqrtf(s.f[i]))

/* float -> int32 as SSE does it: out of range or NaN gives the "integer indefinite" 0x80000000. */
static inline uint32_t x86_cvtf2i_trunc(float f) {
    return (f >= -2147483648.0f && f < 2147483648.0f) ? (uint32_t)(int32_t)f : 0x80000000u;
}
static inline uint32_t x86_cvtf2i_near(float f) {   /* MXCSR default: round to nearest even */
    f = nearbyintf(f);
    return (f >= -2147483648.0f && f < 2147483648.0f) ? (uint32_t)(int32_t)f : 0x80000000u;
}
