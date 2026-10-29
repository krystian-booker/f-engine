$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_source, 0);
// [0] source texel w, h; [1] y: weight of the coarser level
uniform vec4 u_bloom[2];

vec3 tap(vec2 uv, vec2 offset) { return texture2D(s_source, uv + offset * u_bloom[0].xy).rgb; }

// 9-tap tent filter of the coarser level, added onto the finer one.
void main() {
    vec3 sum = tap(v_texcoord0, vec2(0.0, 0.0)) * 4.0;
    sum += (tap(v_texcoord0, vec2(0.0, -1.0)) + tap(v_texcoord0, vec2(-1.0, 0.0)) + tap(v_texcoord0, vec2(1.0, 0.0)) + tap(v_texcoord0, vec2(0.0, 1.0))) * 2.0;
    sum += tap(v_texcoord0, vec2(-1.0, -1.0)) + tap(v_texcoord0, vec2(1.0, -1.0)) + tap(v_texcoord0, vec2(-1.0, 1.0)) + tap(v_texcoord0, vec2(1.0, 1.0));
    gl_FragColor = vec4(sum * (u_bloom[1].y / 16.0), 1.0);
}
