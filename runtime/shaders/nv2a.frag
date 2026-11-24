#version 450
// NV2A pixel stage for Project Underground: texture stages + register combiners, interpreted from the combiner
// registers uploaded with each draw (one precompiled shader, no runtime shader compiler). Combiner semantics as in
// Project Gauntlet's generated HLSL (gpu_d3d11.c gen_ps).

layout(std140, set = 0, binding = 0) uniform Draw {
    uvec4 prog[136];
    vec4 cst[192];
    vec4 comp[4];
    vec4 texm[16];
    vec4 vpo;
    vec4 surf;
    vec4 zr;
    uvec4 mode;
    uvec4 comb[9];         // [0] = control, final cw0, final cw1, -; [1+st] = color icw, color ocw, alpha icw, alpha ocw
    vec4 cfac[16];         // [st] = factor 0 of stage st, [8+st] = factor 1
    vec4 fogc;             // fog color rgb
    vec4 alpha;            // ref (0..255), func (0..7), test enable, output alpha scale
    vec4 tsz[4];           // 1/w, 1/h of linear (rect) textures
    uvec4 tmode;           // per stage: 0 none, 1 2D, 2 2D rect (unnormalized coords), 3 pass-through, 4 shadow map
    vec4 sfac[2];          // final combiner c0, c1 (SET_SPECULAR_FOG_FACTOR)
    vec4 fogp;             // fog params 0, 1; function (0 off, 1 linear, 2 exp, 3 exp2); abs
} D;

layout(set = 0, binding = 2) uniform sampler2D tex0;
layout(set = 0, binding = 3) uniform sampler2D tex1;
layout(set = 0, binding = 4) uniform sampler2D tex2;
layout(set = 0, binding = 5) uniform sampler2D tex3;

layout(location = 0) in vec4 i_d0;
layout(location = 1) in vec4 i_d1;
layout(location = 2) in vec4 i_t0;
layout(location = 3) in vec4 i_t1;
layout(location = 4) in vec4 i_t2;
layout(location = 5) in vec4 i_t3;
layout(location = 6) in float i_fog;

layout(location = 0) out vec4 o_color;

// combiner registers: 0 zero, 1 c0, 2 c1, 3 fog, 4 v0, 5 v1, 6/7 zero, 8-11 t0-t3, 12 r0, 13 r1, 14 sum, 15 ef
vec4 REG[16];

vec2 g_shadow_dbg = vec2(-1.0);
vec4 g_shadow_raw = vec4(-1.0);   // UG_DBGCOLOR=7: uv, raw ref / 4   // UG_DBGCOLOR=6: ref, stored depth of the last shadow stage

vec4 stage_tex(int s, vec4 tc) {
    uint m = D.tmode[s];
    if (m == 0u) return vec4(0.0);
    if (m == 3u) return clamp(tc, 0.0, 1.0);
    vec2 uv = tc.xy / (tc.w != 0.0 ? tc.w : 1.0);
    if (m == 2u || m == 4u) uv *= D.tsz[s].xy;
    vec4 t = s == 0 ? texture(tex0, uv) : s == 1 ? texture(tex1, uv) : s == 2 ? texture(tex2, uv) : texture(tex3, uv);
    if (m != 4u) return t;
    if (D.mode.w == 8u) return vec4(1.0);   // UG_DBGCOLOR=8: every shadow lookup lit (debug)
    // shadow map: compare the texcoord depth (Z-buffer units, projected) with the stored depth -> 1 lit / 0 shadowed
    float rr = tc.z / (tc.w != 0.0 ? tc.w : 1.0) * D.tsz[s].z;
    float r = clamp(rr, 0.0, 1.0), d = t.r;   // fixed-point ref saturates
    g_shadow_dbg = vec2(r, d);
    g_shadow_raw = vec4(uv, rr, tc.w);
    // Outside the shadow map (uv beyond [0,1]), past its far plane (ref >= 1) or behind the light (w <= 0): lit.
    // The saturated ref equals the cleared depth there, so THUG's "stored > ref" test failed and whole receiver
    // triangles at the edge of the light's frustum went dark (hard-edged wedges over walls, floors and the sky).
    if (tc.w <= 0.0 || rr >= 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(1.0);
    int f = int(D.tsz[s].w);
    bool p = f == 1 ? d < r : f == 2 ? d == r : f == 3 ? d <= r : f == 4 ? d > r : f == 5 ? d != r : f == 6 ? d >= r : f == 7;   // stored OP ref
    return vec4(p ? 1.0 : 0.0);
}

// input mapping (bits 7:5) of a combiner input byte; alpha_portion reads .b / .a instead of .rgb / .aaa
vec3 inp_rgb(uint b) {
    vec4 r = REG[b & 15u];
    vec3 x = (b & 16u) != 0u ? r.aaa : r.rgb;
    switch ((b >> 5) & 7u) {
    case 0u: return clamp(x, 0.0, 1.0);
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);
    case 2u: return 2.0 * clamp(x, 0.0, 1.0) - 1.0;
    case 3u: return 1.0 - 2.0 * clamp(x, 0.0, 1.0);
    case 4u: return clamp(x, 0.0, 1.0) - 0.5;
    case 5u: return 0.5 - clamp(x, 0.0, 1.0);
    case 6u: return x;
    default: return -x;
    }
}
float inp_a(uint b) {
    vec4 r = REG[b & 15u];
    float x = (b & 16u) != 0u ? r.a : r.b;
    switch ((b >> 5) & 7u) {
    case 0u: return clamp(x, 0.0, 1.0);
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);
    case 2u: return 2.0 * clamp(x, 0.0, 1.0) - 1.0;
    case 3u: return 1.0 - 2.0 * clamp(x, 0.0, 1.0);
    case 4u: return clamp(x, 0.0, 1.0) - 0.5;
    case 5u: return 0.5 - clamp(x, 0.0, 1.0);
    case 6u: return x;
    default: return -x;
    }
}

// output scale/bias: bit 15 bias, bits 17:16 scale (x1, x2, x4, x0.5); clamp to [-1, 1]
vec3 outop3(vec3 x, uint ocw) {
    if ((ocw & 0x8000u) != 0u) x -= 0.5;
    uint sc = (ocw >> 16) & 3u;
    x *= sc == 1u ? 2.0 : sc == 2u ? 4.0 : sc == 3u ? 0.5 : 1.0;
    return clamp(x, -1.0, 1.0);
}
float outop1(float x, uint ocw) {
    if ((ocw & 0x8000u) != 0u) x -= 0.5;
    uint sc = (ocw >> 16) & 3u;
    x *= sc == 1u ? 2.0 : sc == 2u ? 4.0 : sc == 3u ? 0.5 : 1.0;
    return clamp(x, -1.0, 1.0);
}

void main() {
    vec4 t0 = stage_tex(0, i_t0), t1 = stage_tex(1, i_t1), t2 = stage_tex(2, i_t2), t3 = stage_tex(3, i_t3);
    uint ctl = D.comb[0].x;
    REG[0] = vec4(0.0); REG[1] = D.cfac[0]; REG[2] = D.cfac[8]; REG[3] = vec4(D.fogc.rgb, D.fogp.z != 0.0 ? i_fog : 1.0);   // fog disabled: factor 1 (no fog)
    REG[4] = i_d0; REG[5] = i_d1; REG[6] = vec4(0.0); REG[7] = vec4(0.0);
    REG[8] = t0; REG[9] = t1; REG[10] = t2; REG[11] = t3;
    REG[12] = vec4(0.0, 0.0, 0.0, t0.a); REG[13] = vec4(0.0); REG[14] = vec4(0.0); REG[15] = vec4(0.0);
    int count = int(min(ctl & 0xFFu, 8u));
    for (int st = 0; st < count; st++) {
        uvec4 w = D.comb[1 + st];
        uint ci = w.x, co = w.y, ai = w.z, ao = w.w;
        if ((ctl & 0x1000u) != 0u) REG[1] = D.cfac[st];
        if ((ctl & 0x10000u) != 0u) REG[2] = D.cfac[8 + st];
        vec3 A = inp_rgb(ci >> 24), B = inp_rgb((ci >> 16) & 0xFFu), C = inp_rgb((ci >> 8) & 0xFFu), Dd = inp_rgb(ci & 0xFFu);
        vec3 cab = (co & 0x2000u) != 0u ? vec3(dot(A, B)) : A * B;
        vec3 ccd = (co & 0x1000u) != 0u ? vec3(dot(C, Dd)) : C * Dd;
        vec3 csum = (co & 0x4000u) != 0u ? (REG[12].a >= 0.5 ? ccd : cab) : cab + ccd;
        cab = outop3(cab, co); ccd = outop3(ccd, co); csum = outop3(csum, co);
        float a = inp_a(ai >> 24), b = inp_a((ai >> 16) & 0xFFu), c = inp_a((ai >> 8) & 0xFFu), d = inp_a(ai & 0xFFu);
        float aab = a * b, acd = c * d;
        float asum = (ao & 0x4000u) != 0u ? (REG[12].a >= 0.5 ? acd : aab) : aab + acd;
        aab = outop1(aab, ao); acd = outop1(acd, ao); asum = outop1(asum, ao);
        uint cd_dst = co & 15u, ab_dst = (co >> 4) & 15u, sum_dst = (co >> 8) & 15u;
        if (cd_dst != 0u) REG[cd_dst].rgb = ccd;
        if (ab_dst != 0u) REG[ab_dst].rgb = cab;
        if (sum_dst != 0u) REG[sum_dst].rgb = csum;
        uint acd_dst = ao & 15u, aab_dst = (ao >> 4) & 15u, asum_dst = (ao >> 8) & 15u;
        if (acd_dst != 0u) REG[acd_dst].a = acd;
        if (aab_dst != 0u) REG[aab_dst].a = aab;
        if (asum_dst != 0u) REG[asum_dst].a = asum;
        if ((co & 0x80000u) != 0u && ab_dst != 0u) REG[ab_dst].a = cab.b;   // AB blue to alpha
        if ((co & 0x40000u) != 0u && cd_dst != 0u) REG[cd_dst].a = ccd.b;   // CD blue to alpha
    }
    // final combiner: rgb = A*B + (1-A)*C + D, alpha = G; sum = r0 + v1, ef = E*F
    uint f0 = D.comb[0].y, f1 = D.comb[0].z;
    REG[1] = D.sfac[0]; REG[2] = D.sfac[1];   // the final combiner has its own constants
    REG[14].rgb = (f1 & 0x80u) != 0u ? clamp(REG[12].rgb + REG[5].rgb, 0.0, 1.0) : REG[12].rgb + REG[5].rgb;
    REG[15].rgb = inp_rgb(f1 >> 24) * inp_rgb((f1 >> 16) & 0xFFu);
    vec3 A = inp_rgb(f0 >> 24), B = inp_rgb((f0 >> 16) & 0xFFu), C = inp_rgb((f0 >> 8) & 0xFFu), Dd = inp_rgb(f0 & 0xFFu);
    vec4 o;
    o.rgb = clamp(A * B + (1.0 - A) * C + Dd, 0.0, 1.0);
    o.a = clamp(inp_a((f1 >> 8) & 0xFFu), 0.0, 1.0);
    if (D.alpha.z > 0.0) {
        float av = floor(o.a * 255.0 + 0.5), r = D.alpha.x;
        int f = int(D.alpha.y);
        bool p = true;
        if (f == 0) p = false; else if (f == 1) p = av < r; else if (f == 2) p = av == r; else if (f == 3) p = av <= r;
        else if (f == 4) p = av > r; else if (f == 5) p = av != r; else if (f == 6) p = av >= r;
        if (!p) discard;
    }
    o.a *= D.alpha.w;
    if (D.mode.w == 1u) o = vec4(1.0, 0.0, 1.0, 1.0);                 // UG_DBGCOLOR=1
    else if (D.mode.w == 2u) o = vec4(t0.rgb, 1.0);                     // UG_DBGCOLOR=2: stage 0 texel
    else if (D.mode.w == 3u) o = vec4(i_d0.rgb, 1.0);                   // UG_DBGCOLOR=3: diffuse
    else if (D.mode.w == 4u) o = vec4(REG[12].rgb, 1.0);                // UG_DBGCOLOR=4: r0 after the combiners
    else if (D.mode.w == 5u) o = vec4(o.rgb, 1.0);                      // UG_DBGCOLOR=5: final rgb, opaque
    else if (D.mode.w == 6u && g_shadow_dbg.x >= -0.5) o = vec4(fract(g_shadow_dbg.x * 16.0), g_shadow_dbg.y < 0.9999 ? 1.0 : 0.0, g_shadow_dbg.x > g_shadow_dbg.y ? 1.0 : 0.0, 1.0);
    else if (D.mode.w == 9u) o = g_shadow_raw.x > -0.5 || g_shadow_raw.y > -0.5 ?   // UG_DBGCOLOR=9: signed ref, stored, w>0
        vec4(clamp(g_shadow_raw.z * 0.5 + 0.5, 0.0, 1.0), g_shadow_dbg.y, g_shadow_raw.w > 0.0 ? 1.0 : 0.0, 1.0) : vec4(1.0);
    else if (D.mode.w == 7u) o =g_shadow_raw.x > -0.5 ? vec4(fract(g_shadow_raw.x), fract(g_shadow_raw.y), clamp(g_shadow_raw.z * 0.25, 0.0, 1.0), 1.0) : vec4(1.0);
    o_color = o;
}
