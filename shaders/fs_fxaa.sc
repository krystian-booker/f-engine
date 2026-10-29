$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_input, 0);
uniform vec4 u_info; // 1/width, 1/height, time, texture origin bottom-left (0/1)

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// FXAA (the compact console variant) on the tone-mapped, graded image.
void main() {
    vec2 t = u_info.xy;
    vec2 uv = v_texcoord0;
    vec4 centre = texture2D(s_input, uv);
    float nw = luma(texture2D(s_input, uv + vec2(-t.x, -t.y)).rgb);
    float ne = luma(texture2D(s_input, uv + vec2(t.x, -t.y)).rgb);
    float sw = luma(texture2D(s_input, uv + vec2(-t.x, t.y)).rgb);
    float se = luma(texture2D(s_input, uv + vec2(t.x, t.y)).rgb);
    float m = luma(centre.rgb);
    float lumaMin = min(m, min(min(nw, ne), min(sw, se)));
    float lumaMax = max(m, max(max(nw, ne), max(sw, se)));
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2_splat(-8.0), vec2_splat(8.0)) * t;
    vec3 a = 0.5 * (texture2D(s_input, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                    texture2D(s_input, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture2D(s_input, uv - dir * 0.5).rgb + texture2D(s_input, uv + dir * 0.5).rgb);
    float lumaB = luma(b);
    gl_FragColor = vec4(lumaB < lumaMin || lumaB > lumaMax ? a : b, centre.a);
}
