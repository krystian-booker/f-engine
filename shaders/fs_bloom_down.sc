$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_source, 0);
// [0] source texel w, h, first level (0/1: weight by coverage and apply the threshold), threshold
// [1] knee (absolute), upsample weight
uniform vec4 u_bloom[2];

vec3 tap(vec2 uv, vec2 offset) {
    vec4 c = texture2D(s_source, uv + offset * u_bloom[0].xy);
    return c.rgb * mix(1.0, c.a, u_bloom[0].z);
}

// 13-tap downsample: a 2x2 box in the middle and four overlapping corner
// boxes, which keeps moving highlights from flickering.
void main() {
    vec3 a = tap(v_texcoord0, vec2(-2.0, -2.0)), b = tap(v_texcoord0, vec2(0.0, -2.0)), c = tap(v_texcoord0, vec2(2.0, -2.0));
    vec3 d = tap(v_texcoord0, vec2(-1.0, -1.0)), e = tap(v_texcoord0, vec2(1.0, -1.0));
    vec3 f = tap(v_texcoord0, vec2(-2.0, 0.0)), g = tap(v_texcoord0, vec2(0.0, 0.0)), h = tap(v_texcoord0, vec2(2.0, 0.0));
    vec3 i = tap(v_texcoord0, vec2(-1.0, 1.0)), j = tap(v_texcoord0, vec2(1.0, 1.0));
    vec3 k = tap(v_texcoord0, vec2(-2.0, 2.0)), l = tap(v_texcoord0, vec2(0.0, 2.0)), m = tap(v_texcoord0, vec2(2.0, 2.0));
    vec3 sum = (d + e + i + j) * 0.125 + (a + b + f + g + b + c + g + h + f + g + k + l + g + h + l + m) * 0.03125;
    if (u_bloom[0].z > 0.5) {
        // Soft-knee threshold (bloomPass in FrameMath.h), clamped against fireflies.
        float brightness = max(sum.r, max(sum.g, sum.b));
        float knee = u_bloom[1].x;
        float soft = clamp(brightness - u_bloom[0].w + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 0.0001);
        sum *= max(soft, brightness - u_bloom[0].w) / max(brightness, 0.0001);
        sum = min(sum, vec3_splat(64.0));
    }
    gl_FragColor = vec4(sum, 1.0);
}
