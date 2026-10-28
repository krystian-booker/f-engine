$input v_texcoord0, v_color0, v_worldPos, v_normal, v_shadowPos, v_viewDir
#include <bgfx_shader.sh>
#include "lighting.sh"
SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_palette, 1);
SAMPLER2D(s_shadow, 2);
uniform vec4 u_spriteFlash;   // rgb, amount
uniform vec4 u_spriteOutline; // rgba (alpha 0 = off)
uniform vec4 u_spriteParams;  // atlas texel w, h, palette row v, palette enabled
uniform vec4 u_materialParams; // alpha cutoff, unused, rim, unlit
float alphaAt(vec2 uv) { return texture2D(s_albedo, uv).a; }
void main() {
    vec4 texel = texture2D(s_albedo, v_texcoord0);
    if (u_spriteParams.w > 0.5) {
        // Palette swap: the red channel indexes a palette row.
        vec4 swapped = texture2D(s_palette, vec2(texel.r, u_spriteParams.z));
        texel.rgb = swapped.rgb;
    }
    vec4 albedo = texel * v_color0;
    if (u_spriteOutline.a > 0.0 && albedo.a < 0.5) {
        vec2 t = u_spriteParams.xy;
        float around = max(max(alphaAt(v_texcoord0 + vec2(t.x, 0.0)), alphaAt(v_texcoord0 - vec2(t.x, 0.0))),
                           max(alphaAt(v_texcoord0 + vec2(0.0, t.y)), alphaAt(v_texcoord0 - vec2(0.0, t.y))));
        if (around > 0.5) {
            gl_FragColor = vec4(u_spriteOutline.rgb, u_spriteOutline.a * v_color0.a);
            return;
        }
    }
    if (albedo.a < u_materialParams.x) discard;
    vec3 normal = normalize(v_normal);
    float sun = max(0.0, dot(normal, -u_sunDirection.xyz)) * 0.5 + 0.5;
    sun *= shadowVisibility(s_shadow, v_shadowPos);
    vec3 light = u_ambient.rgb + u_sunColor.rgb * sun + pointLights(v_worldPos, normal);
    vec3 color = mix(albedo.rgb * light, albedo.rgb, u_materialParams.w);
    color = mix(color, u_spriteFlash.rgb, u_spriteFlash.a);
    gl_FragColor = vec4(applyFog(color, v_worldPos), albedo.a);
}
