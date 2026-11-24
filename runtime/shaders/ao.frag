#version 450
// Ambient occlusion from the depth buffer (gpu_vulkan.c fx_apply): cosine-weighted occlusion over a 12-tap spiral
// (bounded: faces and cloth seen close up stay natural), with normals rebuilt from depth. The spiral angle follows a
// 4x4 pattern that post.frag averages away. Drawn with touch.vert (one full-screen triangle).
layout(std140, set = 0, binding = 0) uniform P {
    vec4 proj;   // focal length x, y and centre x, y, in host pixels
    vec4 zp;     // depth = x + y / w (w = view distance); z: radius in world units; w: strength
    vec4 rt;     // host size of the target; 1 / that size
    vec4 opt;    // x: AO pixel size in host pixels; y: FXAA on; z: AO on; w: show AO only (debug)
    vec4 area;   // this view's rectangle in host pixels (x0, y0, x1, y1): split screen has two
} p;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(location = 0) out float o_ao;

const int N = 12;
const float FADE0 = 2500.0, FADE1 = 5000.0;   // depth precision runs out beyond (near plane about 2)

vec3 pos(ivec2 c) {
    c = clamp(c, ivec2(p.area.xy), ivec2(p.area.zw) - 1);
    float z = texelFetch(depth_tex, c, 0).x;
    float w = z >= 1.0 ? 1e6 : p.zp.y / min(z - p.zp.x, -1e-7);
    return vec3((vec2(c) + 0.5 - p.proj.zw) / p.proj.xy * w, w);
}

void main() {
    int step_px = int(p.opt.x);
    ivec2 a = ivec2(gl_FragCoord.xy), c = a * step_px + step_px / 2;
    vec3 P = pos(c);
    o_ao = 1.0;
    if (P.z >= FADE1) return;
    float R = p.zp.z, rpx = R * p.proj.y / P.z;
    rpx = min(rpx, (p.area.w - p.area.y) * 0.12);
    if (rpx < 2.0 * float(step_px)) return;

    ivec2 ex = ivec2(step_px, 0), ey = ivec2(0, step_px);
    vec3 l = P - pos(c - ex), r = pos(c + ex) - P, u = P - pos(c - ey), d = pos(c + ey) - P;
    vec3 n = cross(abs(l.z) < abs(r.z) ? l : r, abs(u.z) < abs(d.z) ? u : d);
    if (dot(n, n) <= 0.0) return;
    n = normalize(n);
    if (dot(n, P) > 0.0) n = -n;

    const int bayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    float phi = float(bayer[(a.y & 3) * 4 + (a.x & 3)]) / 16.0 * 6.2831853;
    float bias = 0.5 + 0.002 * P.z, R2 = R * R, sum = 0.0;
    vec2 aspect = vec2(p.proj.x / p.proj.y, 1.0);   // anamorphic widescreen: pixels are not square
    for (int i = 0; i < N; i++) {
        float t = (float(i) + 0.5) / float(N), ang = t * 6.2831853 * 5.0 + phi;
        vec3 v = pos(c + ivec2(vec2(cos(ang), sin(ang)) * aspect * rpx * t)) - P;
        float vv = dot(v, v), vn = dot(v, n) - bias, f = max(R2 - vv, 0.0) / R2;
        sum += f * f * max(vn / sqrt(vv + 1.0) - 0.1, 0.0);
    }
    float ao = max(1.0 - p.zp.w * sum / float(N), 0.4);
    o_ao = mix(ao, 1.0, smoothstep(FADE0, FADE1, P.z));
}
