$input a_position, a_texcoord0
$output v_texcoord0, v_viewDir
#include <bgfx_shader.sh>
void main() {
    gl_Position = vec4(a_position.xy, 1.0, 1.0);
    vec4 far = mul(u_invViewProj, vec4(a_position.xy, 1.0, 1.0));
    vec4 near = mul(u_invViewProj, vec4(a_position.xy, 0.0, 1.0));
    v_viewDir = far.xyz / far.w - near.xyz / near.w;
    v_texcoord0 = a_texcoord0;
}
