$input a_position, a_normal, a_tangent, a_texcoord0, a_color0, a_color1, a_color2, a_texcoord1, a_texcoord2
$output v_texcoord0, v_color0, v_worldPos, v_normal, v_flash, v_outline, v_probe, v_spriteParams, v_tangent
#include <bgfx_shader.sh>
void main() {
    gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
    v_worldPos = a_position;
    v_normal = a_normal.xyz * 2.0 - 1.0;
    v_tangent = vec4(a_tangent.xyz * 2.0 - 1.0, 1.0);
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
    v_flash = a_color1;
    v_outline = a_color2;
    v_probe = a_texcoord1;
    v_spriteParams = a_texcoord2;
}
