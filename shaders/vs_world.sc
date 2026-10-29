$input a_position, a_normal, a_tangent, a_texcoord0, a_color0
$output v_texcoord0, v_color0, v_worldPos, v_normal, v_viewDir, v_tangent
#include <bgfx_shader.sh>
#include "environment.sh"
uniform mat4 u_normalMatrix; // inverse transpose of the model matrix
void main() {
    vec4 world = mul(u_model[0], vec4(a_position, 1.0));
    gl_Position = mul(u_viewProj, world);
    v_worldPos = world.xyz;
    v_normal = mul(u_normalMatrix, vec4(a_normal.xyz * 2.0 - 1.0, 0.0)).xyz;
    v_tangent = vec4(mul(u_model[0], vec4(a_tangent.xyz * 2.0 - 1.0, 0.0)).xyz, a_tangent.w * 2.0 - 1.0);
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
    v_viewDir = u_cameraPos.xyz - world.xyz;
}
