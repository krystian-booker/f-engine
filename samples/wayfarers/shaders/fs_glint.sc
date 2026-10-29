$input v_texcoord0, v_color0, v_worldPos, v_normal, v_viewDir, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_normal, 1);
#include "lighting.sh"
SAMPLER2D(s_emissive, 5);
uniform vec4 u_material[3]; // colour; alpha cutoff, has normal map, unlit, normal scale; emissive rgb, blend output
uniform vec4 u_info;        // 1/width, 1/height, seconds, texture origin bottom-left (0/1)

// A mesh material shader: the engine's lighting with a band of light that
// sweeps up the surface every two seconds (enchanted crates, magic doors).
void main() {
    vec4 albedo = texture2D(s_albedo, v_texcoord0) * v_color0 * u_material[0];
    if (albedo.a < u_material[1].x) discard;
    vec3 normal = normalize(v_normal);
    float sun = max(0.0, dot(normal, -u_sunDirection.xyz)) * shadowVisibility(v_worldPos);
    vec3 light = u_ambient.rgb + u_sunColor.rgb * sun + pointLights(v_worldPos, normal, v_worldPos);
    float band = 1.0 - smoothstep(0.0, 0.08, abs(fract(v_worldPos.y * 0.8 - u_info.z * 0.5) - 0.5));
    vec3 color = albedo.rgb * light + vec3(0.4, 1.2, 2.0) * band;
    gl_FragColor = blendOutput(color, albedo.a, v_worldPos, u_material[2].w);
}
