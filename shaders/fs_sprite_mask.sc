$input v_texcoord0, v_color0, v_worldPos, v_normal, v_flash, v_outline, v_probe, v_spriteParams, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
uniform vec4 u_sprite[2];
void main() {
    if (texture2D(s_albedo, v_texcoord0).a * v_color0.a < u_sprite[0].w) discard;
    gl_FragColor = vec4_splat(1.0);
}
