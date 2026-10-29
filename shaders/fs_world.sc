$input v_texcoord0, v_color0, v_worldPos, v_normal, v_viewDir, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_normal, 1);
#include "lighting.sh"
SAMPLER2D(s_emissive, 5);
// colour; alpha cutoff, has normal map, unlit, normal scale; emissive rgb, blend output
uniform vec4 u_material[3];
void main() {
    vec4 albedo = texture2D(s_albedo, v_texcoord0) * v_color0 * u_material[0];
    if (albedo.a < u_material[1].x) discard;
    vec3 normal = normalize(v_normal);
    if (u_material[1].y > 0.5) {
        vec3 tangent = normalize(v_tangent.xyz - normal * dot(normal, v_tangent.xyz));
        vec3 bitangent = cross(normal, tangent) * v_tangent.w;
        vec3 detail = texture2D(s_normal, v_texcoord0).xyz * 2.0 - 1.0;
        detail.xy *= u_material[1].w;
        normal = normalize(tangent * detail.x + bitangent * detail.y + normal * detail.z);
    }
    float sun = max(0.0, dot(normal, -u_sunDirection.xyz)) * shadowVisibility(v_worldPos);
    vec3 light = u_ambient.rgb + u_sunColor.rgb * sun + pointLights(v_worldPos, normal, v_worldPos);
    vec3 color = mix(albedo.rgb * light, albedo.rgb, u_material[1].z);
    color += u_material[2].rgb * texture2D(s_emissive, v_texcoord0).rgb;
    gl_FragColor = blendOutput(color, albedo.a, v_worldPos, u_material[2].w);
}
