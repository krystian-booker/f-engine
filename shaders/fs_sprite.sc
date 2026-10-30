$input v_texcoord0, v_color0, v_worldPos, v_normal, v_flash, v_outline, v_probe, v_spriteParams, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_palette, 1);
#include "lighting.sh"
SAMPLER2D(s_spriteNormal, 5);
SAMPLER2D(s_spriteEmissive, 6);
// [0] atlas texel w, h, palette enabled, alpha cutoff
// [1] output (0-3 blendOutput modes, 4 silhouette), has normal map, has emissive mask, outline width (texels)
uniform vec4 u_sprite[2];
float alphaAt(vec2 uv) { return texture2D(s_albedo, uv).a; }
void main() {
    vec4 texel = texture2D(s_albedo, v_texcoord0);
    if (u_sprite[0].z > 0.5) {
        // Palette swap: the stored red value indexes a palette row.
        texel.rgb = texture2D(s_palette, vec2(texel.r, v_spriteParams.x)).rgb;
    }
    vec4 albedo = texel * v_color0;
    if (u_sprite[1].x > 3.5) {
        // Silhouette: the part of the sprite hidden behind scene geometry.
        if (texel.a < u_sprite[0].w) discard;
        gl_FragColor = v_outline;
        return;
    }
    if (v_outline.a > 0.0 && albedo.a < 0.5) {
        vec2 t = u_sprite[0].xy * u_sprite[1].w;
        float around = max(max(alphaAt(v_texcoord0 + vec2(t.x, 0.0)), alphaAt(v_texcoord0 - vec2(t.x, 0.0))),
                           max(alphaAt(v_texcoord0 + vec2(0.0, t.y)), alphaAt(v_texcoord0 - vec2(0.0, t.y))));
        if (around > 0.5) {
            gl_FragColor = blendOutput(v_outline.rgb, v_outline.a * v_color0.a, v_worldPos, u_sprite[1].x);
            return;
        }
    }
    if (albedo.a < u_sprite[0].w) discard;
    vec3 normal = normalize(v_normal);
    vec3 shadePosition = v_probe.xyz;
    if (u_sprite[1].y > 0.5) {
        // Normal-mapped sprites are lit per pixel, so lights move across them.
        vec3 tangent = normalize(v_tangent.xyz - normal * dot(normal, v_tangent.xyz));
        vec3 bitangent = cross(normal, tangent);
        vec3 detail = texture2D(s_spriteNormal, v_texcoord0).xyz * 2.0 - 1.0;
        normal = normalize(tangent * detail.x + bitangent * detail.y + normal * detail.z);
        shadePosition = v_worldPos;
    }
    float sun = (max(0.0, dot(normal, -u_sunDirection.xyz)) * 0.5 + 0.5) * shadowVisibility(v_worldPos);
    vec3 light = u_ambient.rgb + u_sunColor.rgb * sun + pointLights(shadePosition, normal, v_worldPos);
    vec3 color = mix(albedo.rgb * light, albedo.rgb, v_probe.w);
    if (u_sprite[1].z > 0.5) {
        vec4 glow = texture2D(s_spriteEmissive, v_texcoord0);
        color += glow.rgb * glow.a;
    }
    color = mix(color, v_flash.rgb, v_flash.a);
    gl_FragColor = blendOutput(color, albedo.a, v_worldPos, u_sprite[1].x);
}
