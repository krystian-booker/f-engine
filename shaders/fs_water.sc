$input v_texcoord0, v_color0, v_worldPos, v_normal, v_shadowPos, v_viewDir
#include <bgfx_shader.sh>
#include "lighting.sh"
SAMPLER2D(s_shadow, 2);
uniform vec4 u_materialColor;
uniform vec4 u_water;   // time, wave scale, wave speed, fresnel strength
uniform vec4 u_skyHorizon;
float wave(vec2 p, float t) {
    return sin(p.x * 1.7 + t) * 0.5 + sin(p.y * 2.3 - t * 1.3) * 0.35 + sin((p.x + p.y) * 3.1 + t * 0.7) * 0.15;
}
void main() {
    vec2 p = v_worldPos.xz * u_water.y;
    float t = u_water.x * u_water.z;
    float e = 0.05;
    float h = wave(p, t);
    vec3 normal = normalize(vec3(h - wave(p + vec2(e, 0.0), t), 4.0 * e, h - wave(p + vec2(0.0, e), t)));
    vec3 view = normalize(v_viewDir);
    float fresnel = pow(1.0 - max(0.0, dot(normal, view)), 4.0) * u_water.w;
    vec3 base = u_materialColor.rgb * v_color0.rgb;
    float sun = max(0.0, dot(normal, -u_sunDirection.xyz)) * shadowVisibility(s_shadow, v_shadowPos);
    vec3 light = u_ambient.rgb + u_sunColor.rgb * sun + pointLights(v_worldPos, normal);
    vec3 halfway = normalize(view - u_sunDirection.xyz);
    float specular = pow(max(0.0, dot(normal, halfway)), 64.0);
    vec3 color = mix(base * light, u_skyHorizon.rgb, clamp(fresnel, 0.0, 0.8)) + u_sunColor.rgb * specular * 0.6;
    gl_FragColor = vec4(applyFog(color, v_worldPos), clamp(u_materialColor.a * v_color0.a + fresnel * 0.3, 0.0, 1.0));
}
