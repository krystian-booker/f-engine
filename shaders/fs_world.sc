$input v_texcoord0, v_color0, v_worldPos, v_normal, v_shadowPos, v_viewDir
#include <bgfx_shader.sh>
#include "lighting.sh"
SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_normal, 1);
SAMPLER2D(s_shadow, 2);
uniform vec4 u_materialColor;
uniform vec4 u_materialParams; // alpha cutoff, has normal map, rim, unlit
uniform vec4 u_emissive;
void main() {
    vec4 albedo = texture2D(s_albedo, v_texcoord0) * v_color0 * u_materialColor;
    if (albedo.a < u_materialParams.x) discard;
    vec3 normal = normalize(v_normal);
    if (u_materialParams.y > 0.5) {
        vec3 dp1 = dFdx(v_worldPos), dp2 = dFdy(v_worldPos);
        vec2 duv1 = dFdx(v_texcoord0), duv2 = dFdy(v_texcoord0);
        vec3 tangent = normalize(dp1 * duv2.y - dp2 * duv1.y);
        vec3 bitangent = normalize(-dp1 * duv2.x + dp2 * duv1.x);
        vec3 detail = texture2D(s_normal, v_texcoord0).xyz * 2.0 - 1.0;
        normal = normalize(tangent * detail.x + bitangent * detail.y + normal * detail.z);
    }
    float sun = max(0.0, dot(normal, -u_sunDirection.xyz)) * shadowVisibility(s_shadow, v_shadowPos);
    vec3 light = u_ambient.rgb + u_sunColor.rgb * sun + pointLights(v_worldPos, normal);
    float rim = pow(1.0 - max(0.0, dot(normal, normalize(v_viewDir))), 3.0) * u_materialParams.z;
    vec3 color = mix(albedo.rgb * light + u_sunColor.rgb * rim, albedo.rgb, u_materialParams.w) + u_emissive.rgb;
    gl_FragColor = vec4(applyFog(color, v_worldPos), albedo.a);
}
