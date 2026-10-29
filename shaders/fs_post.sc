$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_scene, 0);
SAMPLER2D(s_bloom, 1);
SAMPLER3D(s_lut, 2);
SAMPLER2D(s_depth, 3);
SAMPLER3D(s_lut2, 4);
SAMPLER2D(s_near, 5);
SAMPLER2D(s_far, 6);
SAMPLER2D(s_ao, 7);
SAMPLER2D(s_distortion, 8);
#include "post.sh"
#include "chromatic.sh"
// Packed by packPost() in FrameMath.h:
// [0] bloom strength (normalised, 0 = off), exposure, chromatic aberration, distortion scale (0 = none drawn)
// [1] depth of field, vignette, colour grade, ambient occlusion (0/1)
// [2] vignette strength, LUT blend, ACES tone mapping (0/1), widest blur (scene pixels)
// [3] tint rgb, amount
uniform vec4 u_post[4];
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 encodeSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3_splat(1.0 / 2.4)) - 0.055, step(vec3_splat(0.0031308), c));
}
void main() {
    vec2 uv = v_texcoord0;
    if (u_post[0].w > 0.0) {
        // Overlapping distort particles add up; past 3% of the screen they would tear.
        vec2 offset = texture2D(s_distortion, uv).xy * u_post[0].w;
        uv += offset * min(1.0, 0.03 / max(length(offset), 0.0001));
    }
    vec4 scene = texture2D(s_scene, uv);
    vec3 color = scene.rgb;
    if (u_post[0].z > 0.0) {
        vec2 shift = chromaticShift(uv, u_post[0].z);
        color.r = texture2D(s_scene, uv + shift).r;
        color.b = texture2D(s_scene, uv - shift).b;
    }
    if (u_post[1].x > 0.5) {
        float coc = circleOfConfusion(linearDepth(texture2D(s_depth, uv).r), uv) * u_post[2].w;
        vec4 far = texture2D(s_far, uv);
        vec4 near = texture2D(s_near, uv);
        color = mix(color, far.rgb, far.a * smoothstep(0.5, 2.0, coc));
        color = mix(color, near.rgb, near.a);
    }
    if (u_post[1].w > 0.5) color *= texture2D(s_ao, uv).r;
    color += texture2D(s_bloom, uv).rgb * u_post[0].x;
    color *= u_post[0].y;
    color = u_post[2].z > 0.5 ? aces(color) : clamp(color, 0.0, 1.0);
    color = encodeSrgb(color);
    vec3 lutCoord = color * (15.0 / 16.0) + vec3_splat(0.5 / 16.0);
    vec3 graded = mix(texture3D(s_lut, lutCoord).rgb, texture3D(s_lut2, lutCoord).rgb, u_post[2].y);
    color = mix(color, graded, u_post[1].z);
    color = mix(color, color * u_post[3].rgb, u_post[3].a);
    vec2 centred = v_texcoord0 - 0.5;
    color *= 1.0 - dot(centred, centred) * u_post[2].x * u_post[1].y;
    gl_FragColor = vec4(color, scene.a);
}
