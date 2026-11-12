$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_input, 0);
uniform vec4 u_params[4]; // [0] amount, tone r, g, b
uniform vec4 u_info;      // 1/width, 1/height, time, texture origin bottom-left (0/1)

// Flashback look: desaturates towards a warm tone with a little film grain.
void main() {
    vec4 color = texture2D(s_input, v_texcoord0);
    float grey = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    float grain = fract(sin(dot(gl_FragCoord.xy + u_info.z, vec2(12.9898, 78.233))) * 43758.5453) * 0.06 - 0.03;
    vec3 tone = grey * u_params[0].yzw + grain;
    gl_FragColor = vec4(mix(color.rgb, tone, u_params[0].x), color.a);
}
