$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
uniform vec4 u_material[3]; // [1].x = alpha cutoff
void main() {
    if (texture2D(s_albedo, v_texcoord0).a < u_material[1].x) discard;
    gl_FragColor = vec4_splat(0.0);
}
