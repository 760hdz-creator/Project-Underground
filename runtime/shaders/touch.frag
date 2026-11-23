#version 450
// On-screen touch controls: up to 24 shapes from linux/touch.c touch_shapes(). shape = x, y, radius, ring width
// (0 = filled disc) in swapchain pixels; color = r, g, b, alpha. Output is premultiplied (blend ONE, 1 - src alpha).
// The pixel position comes from uv * screen size, not gl_FragCoord (Gauntlet saw it read back wrong on Adreno).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform Touch {
    vec4 shape[24];
    vec4 color[24];
    vec4 info;   // count, unused, screen width, screen height
} t;
void main() {
    vec2 p = uv * t.info.zw;
    vec4 acc = vec4(0.0);
    int n = int(t.info.x);
    for (int i = 0; i < n; i++) {
        vec4 s = t.shape[i];
        float d = length(p - s.xy);
        float cov = s.w > 0.0 ? 1.0 - smoothstep(s.w * 0.5 - 1.0, s.w * 0.5 + 1.0, abs(d - s.z))
                              : 1.0 - smoothstep(s.z - 1.5, s.z + 1.5, d);
        float a = t.color[i].a * cov;
        acc = vec4(t.color[i].rgb * a, a) + acc * (1.0 - a);
    }
    o = acc;
}
