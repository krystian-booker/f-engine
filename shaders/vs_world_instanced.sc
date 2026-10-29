$input a_position, a_normal, a_tangent, a_texcoord0, a_color0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0, v_color0, v_worldPos, v_normal, v_viewDir, v_tangent
#include <bgfx_shader.sh>
#include "environment.sh"
// vs_world for instanced mesh parts: i_data0..3 are the model matrix columns
// and i_data4 the tint, which multiplies the vertex colour. Normals use the
// matrix's cofactor over its determinant, the inverse transpose
// (instanceNormal() in FrameMath.h mirrors it).
void main() {
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    vec4 world = mul(model, vec4(a_position, 1.0));
    gl_Position = mul(u_viewProj, world);
    v_worldPos = world.xyz;
    vec3 c0 = i_data0.xyz;
    vec3 c1 = i_data1.xyz;
    vec3 c2 = i_data2.xyz;
    vec3 n = a_normal.xyz * 2.0 - 1.0;
    float determinant = dot(c0, cross(c1, c2));
    determinant = abs(determinant) > 1e-12 ? determinant : 1.0;
    v_normal = (cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z) / determinant;
    v_tangent = vec4(mul(model, vec4(a_tangent.xyz * 2.0 - 1.0, 0.0)).xyz, a_tangent.w * 2.0 - 1.0);
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0 * i_data4;
    v_viewDir = u_cameraPos.xyz - world.xyz;
}
