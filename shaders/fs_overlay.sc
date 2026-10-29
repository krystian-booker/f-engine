$input v_texcoord0, v_color0, v_worldPos, v_normal, v_viewDir, v_tangent
#include <bgfx_shader.sh>
uniform vec4 u_overlay; // time, pulse speed, border enabled
void main() {
    vec2 edge = min(v_texcoord0, 1.0 - v_texcoord0);
    float border = u_overlay.z > 0.5 ? step(min(edge.x, edge.y), 0.08) : 0.0;
    float pulse = u_overlay.y > 0.0 ? 0.75 + 0.25 * sin(u_overlay.x * u_overlay.y * 3.1415) : 1.0;
    vec4 color = v_color0;
    color.a *= pulse * mix(0.75, 1.0, border);
    color.rgb = mix(color.rgb, vec3_splat(1.0), border * 0.35);
    gl_FragColor = color;
}
