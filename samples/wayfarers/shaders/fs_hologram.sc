$input v_texcoord0, v_color0, v_worldPos, v_normal, v_flash, v_outline, v_probe, v_spriteParams, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
#include "lighting.sh"
uniform vec4 u_sprite[2]; // [0] atlas texel w, h, palette enabled, alpha cutoff; [1] blend output
uniform vec4 u_info;      // 1/width, 1/height, seconds, texture origin bottom-left (0/1)

// A sprite shader: an unlit cyan projection with scrolling scan lines.
void main() {
    vec4 texel = texture2D(s_albedo, v_texcoord0) * v_color0;
    if (texel.a < u_sprite[0].w) discard;
    float grey = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
    float scan = 0.65 + 0.35 * step(0.5, fract(v_worldPos.y * 14.0 - u_info.z * 2.0));
    gl_FragColor = blendOutput(vec3(0.3, 1.4, 1.8) * (grey + 0.25) * scan, texel.a, v_worldPos, u_sprite[1].x);
}
