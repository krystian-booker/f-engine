$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_half, 0);
#include "post.sh"

// Bokeh gather over a golden-angle disc at half resolution, kept apart for
// the two fields: the far field only takes samples that are themselves far
// and wide enough to reach this pixel, so sharp foreground never smears into
// the blurred background; the near field spreads over anything behind it and
// carries a coverage alpha. Bright samples weigh more (bokeh highlights).
void main() {
    const int Taps = 32;
    float radius = u_dof[2].z;
    vec4 centre = texture2D(s_half, v_texcoord0);
    vec3 farSum = vec3_splat(0.0), nearSum = vec3_splat(0.0);
    float farWeight = 0.0, nearWeight = 0.0, nearest = 0.0;
    if (centre.a > 0.0) {
        farSum = centre.rgb;
        farWeight = 1.0;
    }
    for (int k = 0; k < Taps; ++k) {
        float fraction = (float(k) + 0.5) / float(Taps);
        float r = sqrt(fraction) * radius;
        float angle = float(k) * 2.39996323;
        vec4 s = texture2D(s_half, v_texcoord0 + vec2(cos(angle), sin(angle)) * r * u_dof[2].xy);
        float reach = abs(s.a) * radius;
        float weight = clamp(reach - r + 1.0, 0.0, 1.0) * (1.0 + min(max(luminance(s.rgb) - 1.0, 0.0), 3.0));
        if (s.a > 0.0 && centre.a > 0.0) {
            farSum += s.rgb * weight;
            farWeight += weight;
        } else if (s.a < 0.0) {
            nearSum += s.rgb * weight;
            nearWeight += weight;
            nearest = max(nearest, reach);
        }
    }
    float expected = float(Taps) * pow(clamp((nearest + 1.0) / max(radius, 1.0), 0.0, 1.0), 2.0);
    float coverage = clamp(2.0 * nearWeight / max(expected, 1.0), 0.0, 1.0) * smoothstep(0.5, 1.5, nearest);
    gl_FragData[0] = vec4(nearSum / max(nearWeight, 0.0001), coverage);
    gl_FragData[1] = vec4(farSum / max(farWeight, 0.0001), farWeight > 0.0 ? 1.0 : 0.0);
}
