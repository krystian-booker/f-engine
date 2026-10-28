$input v_texcoord0, v_viewDir
#include <bgfx_shader.sh>
SAMPLER2D(s_panorama, 0);
uniform vec4 u_skyTop;
uniform vec4 u_skyHorizon;
uniform vec4 u_skyBottom;
uniform vec4 u_skyParams;  // mode (1 colour, 2 gradient, 3 panorama), cloud cover, cloud time, unused
uniform vec4 u_cloudColor;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
void main() {
    vec3 d = normalize(v_viewDir);
    vec3 color;
    if (u_skyParams.x > 2.5) {
        float u = atan2(d.x, -d.z) / 6.2831853 + 0.5;
        float v = acos(clamp(d.y, -1.0, 1.0)) / 3.1415927;
        color = texture2D(s_panorama, vec2(u, v)).rgb;
    } else if (u_skyParams.x > 1.5) {
        color = d.y > 0.0 ? mix(u_skyHorizon.rgb, u_skyTop.rgb, pow(d.y, 0.6))
                          : mix(u_skyHorizon.rgb, u_skyBottom.rgb, pow(-d.y, 0.4));
    } else {
        color = u_skyHorizon.rgb;
    }
    if (u_skyParams.y > 0.0 && d.y > 0.02) {
        vec2 p = d.xz / max(d.y, 0.05) * 1.5 + vec2(u_skyParams.z, u_skyParams.z * 0.4);
        float n = noise(p) * 0.6 + noise(p * 2.1) * 0.3 + noise(p * 4.3) * 0.1;
        float cover = smoothstep(1.0 - u_skyParams.y, 1.0, n) * smoothstep(0.02, 0.25, d.y);
        color = mix(color, u_cloudColor.rgb, cover);
    }
    gl_FragColor = vec4(color, 1.0);
}
