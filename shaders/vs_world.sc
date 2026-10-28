$input a_position, a_normal, a_texcoord0, a_color0
$output v_texcoord0, v_color0, v_worldPos, v_normal, v_shadowPos
#include <bgfx_shader.sh>
uniform mat4 u_lightViewProj;
void main() {
    vec4 world = mul(u_model[0],vec4(a_position,1.0));
    gl_Position = mul(u_viewProj,world);
    v_worldPos = world.xyz;
    v_normal = mul(u_model[0],vec4(a_normal,0.0)).xyz;
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
    v_shadowPos = mul(u_lightViewProj,world);
}
