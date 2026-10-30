$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_depth, 0);
#include "post.sh"
// [0] tan(half fov) x, y (an orthographic view's half extents), orthographic (0/1);
// [1] radius (world units), strength, bias
uniform vec4 u_ssao[2];

vec3 viewPosition(vec2 uv) {
    float d = linearDepth(texture2D(s_depth, uv).r);
    float spread = u_ssao[0].z > 0.5 ? 1.0 : d;
    return vec3((uv * 2.0 - 1.0) * u_ssao[0].xy * spread, d);
}

// Half resolution ambient occlusion: twelve samples spiralling through a
// view-space disc around the reconstructed surface, rotated per pixel.
void main() {
    vec2 uv = v_texcoord0;
    vec3 p = viewPosition(uv);
    if (p.z > u_dof[0].y * 0.99) {
        gl_FragColor = vec4_splat(1.0);
        return;
    }
    vec2 t = u_dof[2].xy;
    vec3 right = viewPosition(uv + vec2(t.x, 0.0)), left = viewPosition(uv - vec2(t.x, 0.0));
    vec3 down = viewPosition(uv + vec2(0.0, t.y)), up = viewPosition(uv - vec2(0.0, t.y));
    vec3 dx = abs(right.z - p.z) < abs(p.z - left.z) ? right - p : p - left;
    vec3 dy = abs(down.z - p.z) < abs(p.z - up.z) ? down - p : p - up;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;
    float radius = u_ssao[1].x;
    float noise = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float occlusion = 0.0;
    for (int i = 0; i < 12; ++i) {
        float angle = noise + float(i) * 2.39996323;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius * sqrt((float(i) + 0.5) / 12.0);
        float spread = u_ssao[0].z > 0.5 ? 1.0 : p.z;
        vec3 s = viewPosition(uv + offset / (2.0 * spread * u_ssao[0].xy));
        vec3 v = s - p;
        float distance = length(v);
        float range = smoothstep(0.0, 1.0, radius / max(abs(p.z - s.z), 0.0001));
        occlusion += max(dot(n, v) / max(distance, 0.0001) - u_ssao[1].z, 0.0) * range;
    }
    float ao = clamp(1.0 - u_ssao[1].y * occlusion / 12.0, 0.0, 1.0);
    gl_FragColor = vec4(ao, ao, ao, 1.0);
}
