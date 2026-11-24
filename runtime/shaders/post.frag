#version 450
// World post-processing (gpu_vulkan.c fx_apply), run between the 3D scene and the HUD: edge anti-aliasing (FXAA: luma
// edge search along the edge, then a sub-pixel shift across it) and the blurred ambient occlusion of ao.frag.
layout(std140, set = 0, binding = 0) uniform P {
    vec4 proj;   // see ao.frag
    vec4 zp;
    vec4 rt;
    vec4 opt;
    vec4 area;
} p;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D color_tex;   // copy of the scene, linear filter
layout(set = 0, binding = 3) uniform sampler2D ao_tex;
layout(location = 0) out vec4 o_color;

vec3 rgb(vec2 px) { return textureLod(color_tex, clamp(px, p.area.xy + 0.5, p.area.zw - 0.5) * p.rt.zw, 0.0).rgb; }
float luma(vec2 px) { return dot(rgb(px), vec3(0.299, 0.587, 0.114)); }

vec3 fxaa(vec2 px) {
    vec3 mid = rgb(px);
    float m = dot(mid, vec3(0.299, 0.587, 0.114));
    float n = luma(px + vec2(0, -1)), s = luma(px + vec2(0, 1)), w = luma(px + vec2(-1, 0)), e = luma(px + vec2(1, 0));
    float lo = min(m, min(min(n, s), min(w, e))), hi = max(m, max(max(n, s), max(w, e))), range = hi - lo;
    if (range < max(0.0312, hi * 0.125)) return mid;
    float nw = luma(px + vec2(-1, -1)), ne = luma(px + vec2(1, -1)), sw = luma(px + vec2(-1, 1)), se = luma(px + vec2(1, 1));
    float eh = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);
    float ev = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);
    bool horz = eh >= ev;
    float l1 = horz ? n : w, l2 = horz ? s : e;
    float g1 = abs(l1 - m), g2 = abs(l2 - m);
    float side = g1 >= g2 ? -1.0 : 1.0;                 // the steeper neighbour
    float avg = 0.5 * ((side < 0.0 ? l1 : l2) + m), gs = 0.25 * max(g1, g2);
    vec2 across = horz ? vec2(0, side) : vec2(side, 0), along = horz ? vec2(1, 0) : vec2(0, 1);
    vec2 p0 = px + across * 0.5;
    const float steps[10] = float[10](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);
    float da = 0.0, db = 0.0, ea = 0.0, eb = 0.0;
    bool fa = false, fb = false;
    for (int i = 0; i < 10 && !(fa && fb); i++) {
        if (!fa) { da += steps[i]; ea = luma(p0 - along * da) - avg; fa = abs(ea) >= gs; }
        if (!fb) { db += steps[i]; eb = luma(p0 + along * db) - avg; fb = abs(eb) >= gs; }
    }
    float dist = min(da, db), end = da < db ? ea : eb;
    float off = ((m - avg) < 0.0) != (end < 0.0) ? 0.5 - dist / (da + db) : 0.0;
    float blur = clamp(abs((2.0 * (n + s + e + w) + nw + ne + sw + se) / 12.0 - m) / range, 0.0, 1.0);
    blur = smoothstep(0.0, 1.0, blur);
    return rgb(px + across * max(off, blur * blur * 0.4));
}

float lin(ivec2 c) {
    float z = texelFetch(depth_tex, clamp(c, ivec2(p.area.xy), ivec2(p.area.zw) - 1), 0).x;
    return z >= 1.0 ? 1e6 : p.zp.y / min(z - p.zp.x, -1e-7);
}

// 4x4 box (one period of ao.frag's pattern), samples across a depth step left out
float ao(ivec2 c) {
    int d = int(p.opt.x);
    float w = lin(c), sum = 0.0, wt = 0.0;
    ivec2 a = c / d, first = ivec2(p.area.xy) / d, last = (ivec2(p.area.zw) + d - 1) / d - 1;
    for (int y = -2; y < 2; y++)
        for (int x = -2; x < 2; x++) {
            ivec2 q = clamp(a + ivec2(x, y), first, last);
            float k = max(1.0 - abs(lin(q * d + d / 2) - w) / (2.0 + 0.03 * w), 0.0);
            sum += k * texelFetch(ao_tex, q, 0).x;
            wt += k;
        }
    return wt > 0.0 ? sum / wt : 1.0;
}

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec4 src = texelFetch(color_tex, c, 0);
    vec3 col = p.opt.y != 0.0 ? fxaa(gl_FragCoord.xy) : src.rgb;
    float occ = p.opt.z != 0.0 ? ao(c) : 1.0;
    o_color = vec4(p.opt.w != 0.0 ? vec3(occ) : col * occ, src.a);   // alpha kept: the HUD may blend with it
}
