// Sun with cascaded shadows, clustered point and spot lights with shadow
// tiles, fog and blend-mode output. Samplers 2 to 4 and 7 are reserved for
// these in every scene fragment shader.
#include "environment.sh"
uniform mat4 u_cascades[3]; // world -> cascade tile uv and biased depth
uniform mat4 u_lightShadows[16]; // world -> light shadow atlas uv and depth, times w
SAMPLER2DSHADOW(s_shadow, 2);
SAMPLER2D(s_lights, 3);
SAMPLER2D(s_lightGrid, 4);
SAMPLER2DSHADOW(s_lightShadow, 7);

float shadowVisibility(vec3 position) {
    if (u_shadowParams.x <= 0.0) return 1.0;
    float margin = 2.0 * u_shadowParams.z;
    for (int c = 0; c < 3; ++c) {
        vec4 p = mul(u_cascades[c], vec4(position, 1.0));
        if (p.x > margin && p.x < 1.0 - margin && p.y > margin && p.y < 1.0 - margin && p.z > 0.0 && p.z < 1.0) {
            vec2 uv = vec2((p.x + float(c)) / 3.0, p.y);
            float lit = 0.0;
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    lit += shadow2D(s_shadow, vec3(uv + vec2(float(x), float(y)) * u_shadowParams.yz, p.z));
                }
            }
            return 1.0 - u_shadowParams.x * (1.0 - lit / 9.0);
        }
    }
    return 1.0;
}

// Cube face of a direction from a point light: +x, -x, +y, -y, +z, -z.
float cubeFace(vec3 d) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x >= 0.0 ? 0.0 : 1.0;
    if (a.y >= a.z) return d.y >= 0.0 ? 2.0 : 3.0;
    return d.z >= 0.0 ? 4.0 : 5.0;
}

// `shadow`: first tile (-1 = none), tile count (1 spot, 6 point), texel size per unit of distance.
float lightShadow(vec4 shadow, vec3 light, vec3 position, vec3 normal) {
    if (shadow.x < 0.0 || u_lightShadowParams.x <= 0.0) return 1.0;
    vec3 toLight = light - position;
    float distance = length(toLight);
    float texel = distance * shadow.z;
    // Offset along the normal and towards the light by about a texel, so
    // surfaces don't shadow themselves.
    vec3 p = position + normal * (texel * 1.5) + toLight / max(distance, 0.001) * (texel * 1.5);
    float tile = shadow.x + (shadow.y > 1.5 ? cubeFace(p - light) : 0.0);
    vec4 clip = mul(u_lightShadows[int(tile)], vec4(p, 1.0));
    if (clip.w <= 0.0) return 1.0;
    vec3 s = clip.xyz / clip.w;
    float margin = u_lightShadowParams.y * 1.5;
    if (s.x < tile / 16.0 + margin || s.x > (tile + 1.0) / 16.0 - margin || s.y < 0.0 || s.y > 1.0 ||
        s.z > 1.0) {
        return 1.0;
    }
    vec2 spread = u_lightShadowParams.yz * 0.5;
    float lit = shadow2D(s_lightShadow, vec3(s.xy + vec2(-spread.x, -spread.y), s.z));
    lit += shadow2D(s_lightShadow, vec3(s.xy + vec2(spread.x, -spread.y), s.z));
    lit += shadow2D(s_lightShadow, vec3(s.xy + vec2(-spread.x, spread.y), s.z));
    lit += shadow2D(s_lightShadow, vec3(s.xy + vec2(spread.x, spread.y), s.z));
    return lit * 0.25;
}

// `shadowPosition` is where shadows are looked up (sprites shade at a probe
// but are shadowed per pixel).
vec3 shadeLight(float index, vec3 position, vec3 normal, vec3 shadowPosition) {
    float u = (index + 0.5) * u_lightGridSize.w;
    vec4 posRadius = texture2DLod(s_lights, vec2(u, 0.5 / 4.0), 0.0);
    vec4 color = texture2DLod(s_lights, vec2(u, 1.5 / 4.0), 0.0);
    vec3 delta = posRadius.xyz - position;
    float distance = length(delta);
    vec3 direction = delta / max(distance, 0.001);
    float attenuation = max(0.0, 1.0 - distance / max(posRadius.w, 0.001));
    attenuation *= attenuation;
    if (color.w > -1.5) {
        vec4 spot = texture2DLod(s_lights, vec2(u, 2.5 / 4.0), 0.0);
        // Cosines of the outer and inner cone; a cone edge never has zero width.
        attenuation *= smoothstep(spot.w, max(color.w, spot.w + 0.0001), dot(-direction, spot.xyz));
    }
    if (attenuation <= 0.0) return vec3_splat(0.0);
    vec4 shadow = texture2DLod(s_lights, vec2(u, 3.5 / 4.0), 0.0);
    attenuation *= lightShadow(shadow, posRadius.xyz, shadowPosition, normal);
    return color.rgb * attenuation * max(0.0, dot(normal, direction) * 0.8 + 0.2);
}

// Lights binned into the world-space cell under `position`, best first,
// terminated by a negative index.
vec3 pointLights(vec3 position, vec3 normal, vec3 shadowPosition) {
    vec2 cell = clamp(floor((position.xz - u_lightGrid.xy) * u_lightGrid.z), vec2_splat(0.0),
                      vec2(u_lightGrid.w - 1.0, u_lightGridSize.x - 1.0));
    vec3 light = vec3_splat(0.0);
    for (int k = 0; k < 4; ++k) {
        vec4 ids = texture2DLod(s_lightGrid, vec2((cell.x * 4.0 + float(k) + 0.5) * u_lightGridSize.y,
                                                  (cell.y + 0.5) * u_lightGridSize.z), 0.0);
        if (ids.x < 0.0) return light;
        light += shadeLight(ids.x, position, normal, shadowPosition);
        if (ids.y < 0.0) return light;
        light += shadeLight(ids.y, position, normal, shadowPosition);
        if (ids.z < 0.0) return light;
        light += shadeLight(ids.z, position, normal, shadowPosition);
        if (ids.w < 0.0) return light;
        light += shadeLight(ids.w, position, normal, shadowPosition);
    }
    return light;
}

float fogAmount(vec3 position) {
    float distanceFog = 1.0 - exp(-length(u_cameraPos.xyz - position) * u_fogColor.w);
    float heightFactor = clamp(exp(-(position.y - u_fogParams.x) * u_fogParams.y), 0.0, 1.0);
    return clamp(distanceFog * heightFactor, 0.0, u_fogParams.z);
}

vec3 applyFog(vec3 color, vec3 position) {
    return mix(color, u_fogColor.rgb, fogAmount(position));
}

// Fogged colour for the draw's blend function (blendOutput() in RenderCommon):
// 0 alpha or opaque, 1 additive (fog fades the glow), 2 multiply (fog fades
// the darkening), 3 premultiplied alpha.
vec4 blendOutput(vec3 color, float alpha, vec3 position, float mode) {
    float fog = fogAmount(position);
    if (mode < 0.5) return vec4(mix(color, u_fogColor.rgb, fog), alpha);
    if (mode < 1.5) return vec4(color * (alpha * (1.0 - fog)), alpha);
    if (mode < 2.5) return vec4(mix(vec3_splat(1.0), color, alpha * (1.0 - fog)), alpha);
    return vec4(color * (1.0 - fog) + u_fogColor.rgb * (fog * alpha), alpha);
}
