$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_input, 0);
SAMPLER2D(s_protectedSprites, 1);
uniform vec4 u_spriteProtection;
uniform vec4 u_info; // 1/width, 1/height, time, texture origin bottom-left (0/1)

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// FXAA (the compact console variant) on the tone-mapped, graded image. The
// image is premultiplied (fs_post.sc), so alpha is filtered along with the
// colour: an edge against a transparent background fades by coverage instead
// of taking on the clear colour of the empty pixels next to it.
void main() {
    vec2 t = u_info.xy;
    vec2 uv = v_texcoord0;
    vec4 centre = texture2D(s_input, uv);
    if (u_spriteProtection.x * texture2D(s_protectedSprites, uv).r > 0.5) {
        gl_FragColor = centre;
        return;
    }
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
    vec4 a = 0.5 * (texture2D(s_input, uv + dir * (1.0 / 3.0 - 0.5)) +
                    texture2D(s_input, uv + dir * (2.0 / 3.0 - 0.5)));
    vec4 b = a * 0.5 + 0.25 * (texture2D(s_input, uv - dir * 0.5) + texture2D(s_input, uv + dir * 0.5));
    float lumaB = luma(b.rgb);
    gl_FragColor = lumaB < lumaMin || lumaB > lumaMax ? a : b;
}
