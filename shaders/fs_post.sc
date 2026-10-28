$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_scene, 0);
SAMPLER2D(s_bloom, 1);
SAMPLER3D(s_lut, 2);
SAMPLER2D(s_depth, 3);
SAMPLER3D(s_lut2, 4);
uniform vec4 u_post;        // bloom strength, focus, focus range, exposure
uniform vec4 u_postFlags;   // bloom, depth of field, vignette, colour grade
uniform vec4 u_screen;      // 1/width, 1/height, vignette strength, LUT blend
uniform vec4 u_dof;         // near, far, focus distance, aperture
uniform vec4 u_tint;        // rgb, amount
uniform vec4 u_transition;  // kind (1 swirl, 2 mosaic, 3 wipe, 4 iris), progress, centre x, centre y
uniform vec4 u_transitionColor;
vec3 toneMap(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec2 transitionUv(vec2 uv) {
    float kind = u_transition.x;
    float t = u_transition.y;
    if (kind > 0.5 && kind < 1.5) {
        vec2 c = uv - u_transition.zw;
        float r = length(c);
        float angle = t * 9.0 * (1.0 - r);
        float s = sin(angle), co = cos(angle);
        return u_transition.zw + vec2(c.x * co - c.y * s, c.x * s + c.y * co) * (1.0 - t * 0.6);
    }
    if (kind > 1.5 && kind < 2.5) {
        float cells = mix(400.0, 12.0, t);
        return (floor(uv * cells) + 0.5) / cells;
    }
    return uv;
}
float transitionCover(vec2 uv) {
    float kind = u_transition.x;
    float t = u_transition.y;
    if (kind < 0.5) return 0.0;
    if (kind < 2.5) return smoothstep(0.6, 1.0, t);
    if (kind < 3.5) return step(uv.x, t * 1.1 - 0.05);
    float radius = (1.0 - t) * 1.25;
    return step(radius, length((uv - u_transition.zw) * vec2(1.7, 1.0)));
}
void main() {
    vec2 uv = transitionUv(v_texcoord0);
    vec3 sharp = texture2D(s_scene, uv).rgb;
    float deviceDepth = texture2D(s_depth, uv).r;
    float viewDepth = u_dof.x * u_dof.y / max(u_dof.y - deviceDepth * (u_dof.y - u_dof.x), 0.001);
    float coc = abs(viewDepth - u_dof.z) / max(viewDepth, 0.1) * u_dof.w;
    float tilt = max(0.0, (abs(uv.y - u_post.y) - u_post.z) * 5.0);
    float blur = clamp(max(tilt, coc), 0.0, 1.0) * u_postFlags.y;
    vec3 softened = sharp * 0.2;
    for (int i = 0; i < 8; ++i) {
        float angle = float(i) * 0.785398;
        vec2 offset = vec2(cos(angle), sin(angle)) * u_screen.xy * blur * 5.0;
        softened += texture2D(s_scene, uv + offset).rgb * 0.1;
    }
    vec3 color = mix(sharp, softened, blur);
    color += texture2D(s_bloom, uv).rgb * u_post.x * u_postFlags.x;
    color = toneMap(color * u_post.w);
    color = pow(color, vec3_splat(1.0 / 2.2));
    vec3 lutCoord = clamp(color, 0.0, 1.0) * (15.0 / 16.0) + vec3_splat(0.5 / 16.0);
    vec3 graded = mix(texture3D(s_lut, lutCoord).rgb, texture3D(s_lut2, lutCoord).rgb, u_screen.w);
    color = mix(color, graded, u_postFlags.w);
    color = mix(color, color * u_tint.rgb, u_tint.a);
    vec2 centred = v_texcoord0 - 0.5;
    color *= 1.0 - dot(centred, centred) * u_screen.z * u_postFlags.z;
    color = mix(color, u_transitionColor.rgb, transitionCover(v_texcoord0));
    gl_FragColor = vec4(color, 1.0);
}
