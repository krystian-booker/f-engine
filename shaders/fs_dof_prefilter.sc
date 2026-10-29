$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_scene, 0);
SAMPLER2D(s_depth, 1);
#include "post.sh"

// Half resolution colour and signed circle of confusion. A near (negative)
// value wins over the others so foreground blur spreads past its edges.
// Colour is clamped like the bloom input: the gather favours bright samples,
// so a single extreme pixel would otherwise spread into a disc.
void main() {
    vec2 t = u_dof[2].xy * 0.5;
    vec2 uv0 = v_texcoord0 + vec2(-t.x, -t.y), uv1 = v_texcoord0 + vec2(t.x, -t.y);
    vec2 uv2 = v_texcoord0 + vec2(-t.x, t.y), uv3 = v_texcoord0 + vec2(t.x, t.y);
    vec3 color = (texture2D(s_scene, uv0).rgb + texture2D(s_scene, uv1).rgb + texture2D(s_scene, uv2).rgb +
                  texture2D(s_scene, uv3).rgb) * 0.25;
    float c0 = circleOfConfusion(linearDepth(texture2D(s_depth, uv0).r), uv0);
    float c1 = circleOfConfusion(linearDepth(texture2D(s_depth, uv1).r), uv1);
    float c2 = circleOfConfusion(linearDepth(texture2D(s_depth, uv2).r), uv2);
    float c3 = circleOfConfusion(linearDepth(texture2D(s_depth, uv3).r), uv3);
    float nearest = min(min(c0, c1), min(c2, c3));
    gl_FragColor = vec4(clamp(color, 0.0, 64.0), nearest < 0.0 ? nearest : (c0 + c1 + c2 + c3) * 0.25);
}
