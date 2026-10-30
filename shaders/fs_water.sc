$input v_texcoord0, v_color0, v_worldPos, v_normal, v_viewDir, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_sceneDepth, 1);
#include "lighting.sh"
SAMPLER2D(s_sceneColor, 6);
// colour; alpha cutoff, has normal map, unlit, normal scale; emissive rgb, blend output (as fs_world.sc)
uniform vec4 u_material[3];
// time, wave scale, wave speed, fresnel; deep colour, depth falloff;
// sky horizon, specular; near, far, depth tint enabled, orthographic camera (0/1);
// reflection, refraction, opaque scene copy available, texture origin bottom-left
uniform vec4 u_water[5];
float wave(vec2 p, float t) {
    return sin(p.x * 1.7 + t) * 0.5 + sin(p.y * 2.3 - t * 1.3) * 0.35 + sin((p.x + p.y) * 3.1 + t * 0.7) * 0.15;
}
// View distance of a depth buffer value. Depth textures hold window depth
// (0..1) on every backend, and that maps to view distance the same way for
// OpenGL's -1..1 clip range (FrameMath.h, linearDepth()).
float viewDepth(float device) {
    if (u_water[3].w > 0.5) {
        return u_water[3].x + device * (u_water[3].y - u_water[3].x);
    }
    return u_water[3].x * u_water[3].y / max(u_water[3].y - device * (u_water[3].y - u_water[3].x), 0.0001);
}
vec2 screenOf(vec4 clip) {
    vec2 ndc = clip.xy / clip.w;
    return vec2(ndc.x * 0.5 + 0.5, u_water[4].w > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5);
}
// Marches the reflected ray through the opaque depth copy; rgb is the scene
// colour where it hits, a how much to trust it (fading near screen edges).
vec4 screenReflection(vec3 origin, vec3 direction) {
    float t = 0.2;
    for (int i = 0; i < 24; ++i) {
        vec3 p = origin + direction * t;
        vec4 clip = mul(u_viewProj, vec4(p, 1.0));
        if (clip.w <= 0.0) break;
        vec2 uv = screenOf(clip);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
        float behind = clip.w - viewDepth(texture2DLod(s_sceneDepth, uv, 0.0).r);
        if (behind > 0.0 && behind < max(0.5, t * 0.35)) {
            vec2 edge = min(uv, 1.0 - uv);
            float fade = clamp(min(edge.x, edge.y) * 10.0, 0.0, 1.0) * (1.0 - float(i) / 24.0);
            return vec4(texture2DLod(s_sceneColor, uv, 0.0).rgb, fade);
        }
        t *= 1.2;
    }
    return vec4_splat(0.0);
}
void main() {
    vec2 p = v_worldPos.xz * u_water[0].y;
    float t = u_water[0].x * u_water[0].z;
    float e = 0.05;
    float h = wave(p, t);
    vec3 normal = normalize(vec3(h - wave(p + vec2(e, 0.0), t), 4.0 * e, h - wave(p + vec2(0.0, e), t)));
    vec3 view = normalize(v_viewDir);
    float fresnel = pow(1.0 - max(0.0, dot(normal, view)), 4.0) * u_water[0].w;
    vec3 base = u_material[0].rgb * v_color0.rgb;
    float alpha = u_material[0].a * v_color0.a;
    bool copied = u_water[4].z > 0.5;
    vec2 screen = (gl_FragCoord.xy - u_viewRect.xy) / u_viewRect.zw;
    float surface = viewDepth(gl_FragCoord.z);
    vec2 behindUv = screen;
    if (copied) {
        // Refraction: look at the scene behind through the waves, unless what
        // is found there stands in front of the water.
        vec2 bent = screen + normal.xz * u_water[4].y;
        if (viewDepth(texture2D(s_sceneDepth, bent).r) > surface) behindUv = bent;
    }
    if (u_water[3].z > 0.5) {
        // Thicker water (scene depth behind the surface) tends to the deep colour.
        float depth = max(0.0, viewDepth(texture2D(s_sceneDepth, behindUv).r) - surface);
        float deep = 1.0 - exp(-depth * u_water[1].w);
        base = mix(base, u_water[1].rgb, deep);
        alpha = mix(alpha, 1.0, deep);
    }
    float sun = max(0.0, dot(normal, -u_sunDirection.xyz)) * shadowVisibility(v_worldPos);
    vec3 light = u_ambient.rgb + u_sunColor.rgb * sun + pointLights(v_worldPos, normal, v_worldPos);
    vec3 color = mix(base * light, base, u_material[1].z);
    color += u_material[2].rgb;
    float coverage = clamp(alpha + fresnel * 0.3, 0.0, 1.0);
    if (copied) {
        // Composite over the refracted scene here; where the scene behind is
        // transparent (no sky) keep blending with what is drawn below.
        vec4 behind = texture2D(s_sceneColor, behindUv);
        color = mix(color, mix(behind.rgb, color, alpha), behind.a);
        coverage = mix(coverage, 1.0, behind.a);
    }
    // The sky's horizon colour by fresnel, then the scene where the reflected
    // ray finds it (seen even from above, as stylised water is).
    color = mix(color, u_water[2].rgb, clamp(fresnel, 0.0, 0.8));
    if (copied && u_water[4].x > 0.0) {
        vec3 direction = reflect(-view, normal);
        direction.y = abs(direction.y);
        vec4 hit = screenReflection(v_worldPos, direction);
        color = mix(color, hit.rgb, hit.a * u_water[4].x * clamp(0.35 + fresnel, 0.0, 1.0));
    }
    vec3 halfway = normalize(view - u_sunDirection.xyz);
    float specular = pow(max(0.0, dot(normal, halfway)), 64.0) * u_water[2].w;
    color += u_sunColor.rgb * specular;
    gl_FragColor = vec4(applyFog(color, v_worldPos), coverage);
}
