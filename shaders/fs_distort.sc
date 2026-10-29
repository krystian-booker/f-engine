$input v_texcoord0, v_color0, v_worldPos, v_normal, v_flash, v_outline, v_probe, v_spriteParams, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_sceneDepth, 1);
uniform vec4 u_distort[2]; // [0] time, atlas texel w, h; [1] 1 / scene width, 1 / scene height

float alphaAt(vec2 uv) { return texture2D(s_albedo, uv).a; }

// Screen offsets (fractions of the screen) for distort sprites and
// particles, summed into the distortion buffer: the alpha gradient bends
// like a lens (rings make shock waves) and a wobble makes heat haze.
void main() {
    float a = alphaAt(v_texcoord0) * v_color0.a;
    if (a < 0.004) discard;
    if (gl_FragCoord.z >= texture2D(s_sceneDepth, gl_FragCoord.xy * u_distort[1].xy).r) discard;
    vec2 t = u_distort[0].yz;
    vec2 lens = vec2(alphaAt(v_texcoord0 + vec2(t.x, 0.0)) - alphaAt(v_texcoord0 - vec2(t.x, 0.0)),
                     alphaAt(v_texcoord0 + vec2(0.0, t.y)) - alphaAt(v_texcoord0 - vec2(0.0, t.y))) * v_color0.a;
    float time = u_distort[0].x;
    vec2 wobble = vec2(sin(v_worldPos.y * 9.0 + v_worldPos.x * 3.0 + time * 7.0),
                       cos(v_worldPos.x * 7.0 + v_worldPos.z * 5.0 - time * 6.0)) * 0.5 * a;
    gl_FragColor = vec4((lens * 4.0 + wobble) * v_spriteParams.y * 0.01, 0.0, 1.0);
}
