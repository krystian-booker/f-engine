$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_ao, 0);
SAMPLER2D(s_depth, 1);
#include "post.sh"

// Depth-aware blur along one axis: samples across a depth edge don't count.
void main() {
    float centre = linearDepth(texture2D(s_depth, v_texcoord0).r);
    float sum = texture2D(s_ao, v_texcoord0).r;
    float total = 1.0;
    for (int i = -4; i <= 4; ++i) {
        if (i == 0) continue;
        vec2 uv = v_texcoord0 + u_dof[2].xy * float(i);
        float depth = linearDepth(texture2D(s_depth, uv).r);
        float weight = exp(-float(i * i) / 8.0) * max(0.0, 1.0 - abs(depth - centre) / (centre * 0.05));
        sum += texture2D(s_ao, uv).r * weight;
        total += weight;
    }
    float ao = sum / total;
    gl_FragColor = vec4(ao, ao, ao, 1.0);
}
