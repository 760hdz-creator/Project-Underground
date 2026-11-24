#version 450
// NV2A vertex stage for Project Underground.
// Mode 2: interprets the guest's NV2A vertex program (uploaded with the draw). Mode 0: fixed-function transform
// through the composite matrix (as Project Gauntlet's D3D11 renderer). Field layout of the 128-bit NV2A vertex
// instruction as documented by the community (xemu's vsh field table; no code copied).

layout(std430, set = 0, binding = 1) readonly buffer Verts { vec4 v_in[]; };   // 16 float4 attributes per vertex

layout(std140, set = 0, binding = 0) uniform Draw {
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
    vec4 sfac[2];          // final combiner c0, c1 (SET_SPECULAR_FOG_FACTOR)
    vec4 fogp;             // fog params 0, 1; function (0 off, 1 linear, 2 exp, 3 exp2); abs
    vec4 mv[4];            // fixed function model-view 0 (rows = output components, as comp)
    vec4 imv[4];           // its inverse
    uvec4 tgen;            // texgen per stage, a byte per s/t/r/q: 0 off, 4 normal map, 5 reflection map
    uvec4 lctl;            // fixed-function lighting: x on, y color material (2 bits: emission, ambient, diffuse, specular
                           // source: 0 material, 1 vertex diffuse, 2 vertex specular), z light types (2 bits each), w normalize
    // only uploaded for lit draws
    vec4 lamb;             // scene ambient (times material), material alpha
    vec4 lemi;             // material emission
    vec4 lgt[32];          // per light: ambient; diffuse, range; direction (infinite) or eye-space position; attenuation
    // last: only uploaded for vertex-program draws (mode 2), garbage otherwise
    uvec4 prog[136];       // instruction slots (dword 0 unused)
    vec4 cst[192];         // vertex program constants
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

vec3 g_en, g_refl;

// NV2A fixed-function lighting (as xemu's vsh-ff: the driver has multiplied material colors into the lights unless the
// color material sources are vertex colors). Specular and spot cones are not modeled (gpu_vulkan.c logs them).
vec4 lighting(vec3 n, vec3 e, vec4 d, vec4 s) {
    uint es = D.lctl.y & 3u, as = (D.lctl.y >> 2) & 3u, ds = (D.lctl.y >> 4) & 3u;
    vec3 ev = es == 2u ? s.rgb : d.rgb, av = as == 2u ? s.rgb : d.rgb;
    vec3 c;
    if (es == 0u || as == 0u) {
        c = D.lamb.rgb;
        if (es != 0u) c += ev * D.lemi.rgb; else if (as != 0u) c += av * D.lemi.rgb;
    } else c = ev + av * D.lemi.rgb;
    for (int i = 0; i < 8; i++) {
        uint t = (D.lctl.z >> uint(i * 2)) & 3u;
        if (t == 0u) continue;
        vec3 l = D.lgt[i * 4 + 2].xyz;
        float att = 1.0;
        if (t != 1u) {
            l -= e;
            float r = length(l);
            if (r > D.lgt[i * 4 + 1].w) continue;
            vec3 k = D.lgt[i * 4 + 3].xyz;
            float q = k.x + k.y * r + k.z * r * r;
            att = q > 0.0 ? 1.0 / q : 1.0;
        }
        float nl = dot(l, l) > 0.0 ? max(0.0, dot(n, normalize(l))) : 0.0;
        c += (as == 0u ? vec3(1.0) : av) * D.lgt[i * 4].rgb * att;
        c += (ds == 0u ? vec3(1.0) : ds == 1u ? d.rgb : s.rgb) * D.lgt[i * 4 + 1].rgb * att * nl;
    }
    return vec4(clamp(c, 0.0, 1.0), ds == 0u ? D.lamb.a : ds == 1u ? d.a : s.a);
}

vec4 texgen(int s, vec4 t) {
    uint g = D.tgen[s];
    if (g == 0u) return t;
    // D3D feeds (generated xyz, 1) to the texture matrix; an absent texcoord array would leave q = 0 (k_cur)
    if ((g >> 24) == 0u) t.w = 1.0;
    for (int c = 0; c < 4; c++) {
        uint m = (g >> uint(c * 8)) & 255u;
        if (m == 5u && c < 3) t[c] = g_refl[c];
        else if (m == 4u && c < 3) t[c] = g_en[c];
    }
    return t;
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
        if ((D.tgen.x | D.tgen.y | D.tgen.z | D.tgen.w | D.lctl.x) != 0u) {
            // eye-space position and normal (normal through the inverse model-view, as D3D/xemu), GL reflection map
            vec4 ep = vec4(dot(D.mv[0], p), dot(D.mv[1], p), dot(D.mv[2], p), dot(D.mv[3], p));
            vec4 n4 = vec4(V(2).xyz, 0.0);
            vec3 en = vec3(dot(D.imv[0], n4), dot(D.imv[1], n4), dot(D.imv[2], n4));
            g_en = dot(en, en) > 0.0 ? normalize(en) : en;
            vec3 e = ep.xyz / (ep.w != 0.0 ? ep.w : 1.0);
            vec3 u = dot(e, e) > 0.0 ? normalize(e) : vec3(0.0, 0.0, 1.0);
            g_refl = u - 2.0 * dot(g_en, u) * g_en;
            if (D.lctl.x != 0u) { o_d0 = lighting(D.lctl.w != 0u ? g_en : en, e, o_d0, o_d1); o_d1 = vec4(0.0, 0.0, 0.0, o_d1.a); }
        }
        o_t0 = texmat(0, texgen(0, V(9))); o_t1 = texmat(1, texgen(1, V(10)));
        o_t2 = texmat(2, texgen(2, V(11))); o_t3 = texmat(3, texgen(3, V(12)));
        o_fog = V(5).x;
    }
    if (D.fogp.z != 0.0) {   // NV2A fog function (xemu's formulas), result clamped by the rasterizer
        float d = D.fogp.w != 0.0 ? abs(o_fog) : o_fog, f;
        if (D.fogp.z == 1.0) f = D.fogp.x + d * D.fogp.y - 1.0;
        else if (D.fogp.z == 2.0) f = D.fogp.x + exp2(d * D.fogp.y * 16.0) - 1.5;
        else f = D.fogp.x + exp2(-d * d * D.fogp.y * D.fogp.y * 32.0) - 1.5;
        o_fog = clamp(f, 0.0, 1.0);
    }
    gl_Position = pos;
    gl_PointSize = 1.0;
}
