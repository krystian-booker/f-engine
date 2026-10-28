$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo,0);
uniform vec4 u_materialParams;
void main() {
    if (texture2D(s_albedo,v_texcoord0).a < u_materialParams.x) discard;
    gl_FragColor = vec4(gl_FragCoord.zzz,1.0);
}
