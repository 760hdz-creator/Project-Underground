#version 450
// NV2A vertex stage for Project Underground.
// Mode 2: interprets the guest's NV2A vertex program (uploaded with the draw). Mode 0: fixed-function transform
// through the composite matrix (as Project Gauntlet's D3D11 renderer). Field layout of the 128-bit NV2A vertex
// instruction as documented by the community (xemu's vsh field table; no code copied).

layout(std430, set = 0, binding = 1) readonly buffer Verts { vec4 v_in[]; };   // 16 float4 attributes per vertex

layout(std140, set = 0, binding = 0) uniform Draw {
    uvec4 prog[136];       // instruction slots (dword 0 unused)
    vec4 cst[192];         // vertex program constants
    vec4 comp[4];          // composite matrix rows (fixed function)
    vec4 texm[16];         // texture matrices, 4 rows each
    vec4 vpo;              // viewport offset (fixed function)
    vec4 surf;             // guest surface w, h, 1/w, 1/h
    vec4 zr;               // clip min, clip max, 1/zmax (fixed function), unused
    uvec4 mode;            // x: transform mode, y: program start, z: texmat enables (bit per stage), w: unused
    // fragment stage data follows (see nv2a.frag); same block layout
    uvec4 comb[9];         // [0] = control, final cw0, final cw1, -; [1+st] = color icw, color ocw, alpha icw, alpha ocw
    vec4 cfac[16];
    vec4 fogc;
    vec4 alpha;
    vec4 tsz[4];
    uvec4 tmode;
} D;

layout(location = 0) out vec4 o_d0;
layout(location = 1) out vec4 o_d1;
layout(location = 2) out vec4 o_t0;
layout(location = 3) out vec4 o_t1;
layout(location = 4) out vec4 o_t2;
layout(location = 5) out vec4 o_t3;
layout(location = 6) out float o_fog;

vec4 V(int i) { return v_in[gl_VertexIndex * 16 + i]; }

uint fld(uvec4 ins, int w, int lo, int n) { return (ins[w] >> uint(lo)) & ((1u << uint(n)) - 1u); }

vec4 R[13];
vec4 O[16];
int A0;

vec4 swz(vec4 x, uint s) { return vec4(x[(s >> 6) & 3u], x[(s >> 4) & 3u], x[(s >> 2) & 3u], x[s & 3u]); }

// mux: 1 temp, 2 input, 3 constant
vec4 src(uvec4 ins, uint mux, uint r, uint s, uint neg) {
    vec4 x = vec4(0.0);
    if (mux == 1u) x = r == 12u ? O[0] : R[r];
    else if (mux == 2u) x = V(int(fld(ins, 1, 9, 4)));
    else if (mux == 3u) {
        int ci = int(fld(ins, 1, 13, 8));
        if (fld(ins, 3, 1, 1) != 0u) ci += A0;
        x = (ci >= 0 && ci < 192) ? D.cst[ci] : vec4(0.0);
    }
    x = swz(x, s);
    return neg != 0u ? -x : x;
}

void wmask(inout vec4 d, vec4 v, uint m) {
    if ((m & 8u) != 0u) d.x = v.x;
    if ((m & 4u) != 0u) d.y = v.y;
    if ((m & 2u) != 0u) d.z = v.z;
    if ((m & 1u) != 0u) d.w = v.w;
}

void run_program() {
    for (int i = 0; i < 13; i++) R[i] = vec4(0.0);
    A0 = 0;
    for (int pc = int(D.mode.y); pc < 136; pc++) {
        uvec4 ins = D.prog[pc];
        uint ilu = fld(ins, 1, 25, 3), mac = fld(ins, 1, 21, 4);
        vec4 a = src(ins, fld(ins, 2, 26, 2), fld(ins, 2, 28, 4),
                     fld(ins, 1, 6, 2) << 6 | fld(ins, 1, 4, 2) << 4 | fld(ins, 1, 2, 2) << 2 | fld(ins, 1, 0, 2),
                     fld(ins, 1, 8, 1));
        vec4 b = src(ins, fld(ins, 2, 11, 2), fld(ins, 2, 13, 4),
                     fld(ins, 2, 23, 2) << 6 | fld(ins, 2, 21, 2) << 4 | fld(ins, 2, 19, 2) << 2 | fld(ins, 2, 17, 2),
                     fld(ins, 2, 25, 1));
        vec4 c = src(ins, fld(ins, 3, 28, 2), fld(ins, 2, 0, 2) << 2 | fld(ins, 3, 30, 2),
                     fld(ins, 2, 8, 2) << 6 | fld(ins, 2, 6, 2) << 4 | fld(ins, 2, 4, 2) << 2 | fld(ins, 2, 2, 2),
                     fld(ins, 2, 10, 1));
        vec4 m = vec4(0.0), l = vec4(0.0);
        switch (mac) {
        case 1u: m = a; break;                                                  // MOV
        case 2u: m = a * b; break;                                              // MUL
        case 3u: m = a + c; break;                                              // ADD
        case 4u: m = a * b + c; break;                                          // MAD
        case 5u: m = vec4(dot(a.xyz, b.xyz)); break;                            // DP3
        case 6u: m = vec4(dot(a.xyz, b.xyz) + b.w); break;                      // DPH
        case 7u: m = vec4(dot(a, b)); break;                                    // DP4
        case 8u: m = vec4(1.0, a.y * b.y, a.z, b.w); break;                     // DST
        case 9u: m = min(a, b); break;                                          // MIN
        case 10u: m = max(a, b); break;                                         // MAX
        case 11u: m = vec4(lessThan(a, b)); break;                              // SLT
        case 12u: m = vec4(greaterThanEqual(a, b)); break;                      // SGE
        case 13u: A0 = int(floor(a.x + 0.001)); break;                          // ARL
        default: break;
        }
        float s = c.x;
        switch (ilu) {
        case 1u: l = c; break;                                                  // MOV
        case 2u: l = vec4(s == 0.0 ? 3.4e38 : 1.0 / s); break;                  // RCP
        case 3u: {                                                              // RCC
            float r = 1.0 / s;
            float ar = clamp(abs(r), 5.42101e-20, 1.884467e19);
            l = vec4(r < 0.0 ? -ar : ar);
            break;
        }
        case 4u: l = vec4(inversesqrt(max(abs(s), 1e-38))); break;              // RSQ
        case 5u: { float f = floor(s); l = vec4(exp2(f), s - f, exp2(s), 1.0); break; }   // EXP
        case 6u: {                                                              // LOG
            float x = abs(s);
            if (x == 0.0) { l = vec4(-3.4e38, 1.0, -3.4e38, 1.0); break; }
            float e = floor(log2(x));
            l = vec4(e, x / exp2(e), log2(x), 1.0);
            break;
        }
        case 7u: {                                                              // LIT
            float d = max(c.x, 0.0), sp = max(c.y, 0.0), pw = clamp(c.w, -127.9961, 127.9961);
            l = vec4(1.0, d, c.x > 0.0 ? pow(sp, pw) : 0.0, 1.0);
            break;
        }
        default: break;
        }
        uint outr = fld(ins, 3, 20, 4);
        if (mac != 0u && mac != 13u) {
            uint mm = fld(ins, 3, 24, 4);
            if (outr == 12u) wmask(O[0], m, mm); else if (outr < 12u) wmask(R[outr], m, mm);
        }
        if (ilu != 0u) {
            uint lm = fld(ins, 3, 16, 4);
            uint lr = (mac != 0u) ? 1u : outr;   // paired with a MAC op, the ILU result goes to r1
            if (lr == 12u) wmask(O[0], l, lm); else if (lr < 12u) wmask(R[lr], l, lm);
        }
        uint om = fld(ins, 3, 12, 4);
        if (om != 0u) {
            vec4 val = fld(ins, 3, 2, 1) != 0u ? l : m;
            uint addr = fld(ins, 3, 3, 8);
            if (fld(ins, 3, 11, 1) != 0u) wmask(O[addr & 15u], val, om);
            // constant writes (ORB = 0) are not modeled
        }
        if (fld(ins, 3, 0, 1) != 0u) break;   // FINAL
    }
}

vec4 texmat(int s, vec4 t) {
    if ((D.mode.z & (1u << uint(s))) == 0u) return t;
    return vec4(dot(D.texm[s * 4 + 0], t), dot(D.texm[s * 4 + 1], t), dot(D.texm[s * 4 + 2], t), dot(D.texm[s * 4 + 3], t));
}

void main() {
    vec4 pos;
    if ((D.mode.x & 3u) == 2u) {
        for (int i = 0; i < 16; i++) O[i] = vec4(0.0, 0.0, 0.0, 1.0);
        O[3] = vec4(1.0); O[4] = vec4(0.0);
        run_program();
        // oPos is in screen space (x, y pixels; z in Z-buffer units; already divided by w): back to clip space
        vec4 p = O[0];
        float w = p.w;
        // pre-transformed vertices often carry rhw = 0, so the program's 1/rhw is infinite: treat as w = 1
        if (w == 0.0 || isinf(w) || isnan(w) || abs(w) > 1e30) w = 1.0;
        else if (w < 0.0) w = min(w, -1.175494e-38); else w = max(w, 1.175494e-38);
        float zn = D.zr.y != D.zr.x ? (p.z - D.zr.x) / (D.zr.y - D.zr.x) : p.z;
        pos = vec4((p.x * D.surf.z * 2.0 - 1.0) * w, (p.y * D.surf.w * 2.0 - 1.0) * w, zn * w, w);
        o_d0 = clamp(O[3], 0.0, 1.0); o_d1 = clamp(O[4], 0.0, 1.0);
        o_t0 = O[9]; o_t1 = O[10]; o_t2 = O[11]; o_t3 = O[12];
        o_fog = O[5].x;
    } else {
        vec4 p = V(0);
        vec4 c = vec4(dot(D.comp[0], p), dot(D.comp[1], p), dot(D.comp[2], p), dot(D.comp[3], p));
        float sx = c.x + D.vpo.x * c.w, sy = c.y + D.vpo.y * c.w;
        pos = vec4(sx * D.surf.z * 2.0 - c.w, sy * D.surf.w * 2.0 - c.w, c.z * D.zr.z, c.w);
        o_d0 = V(3); o_d1 = V(4);
        o_t0 = texmat(0, V(9)); o_t1 = texmat(1, V(10)); o_t2 = texmat(2, V(11)); o_t3 = texmat(3, V(12));
        o_fog = V(5).x;
    }
    gl_Position = pos;
    gl_PointSize = 1.0;
}
