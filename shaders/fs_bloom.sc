$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_scene,0);
uniform vec4 u_blur;
void main() {
    vec3 sum = texture2D(s_scene,v_texcoord0).rgb*0.227027;
    sum += texture2D(s_scene,v_texcoord0+u_blur.xy*1.384615).rgb*0.316216;
    sum += texture2D(s_scene,v_texcoord0-u_blur.xy*1.384615).rgb*0.316216;
    sum += texture2D(s_scene,v_texcoord0+u_blur.xy*3.230769).rgb*0.070270;
    sum += texture2D(s_scene,v_texcoord0-u_blur.xy*3.230769).rgb*0.070270;
    if (u_blur.z > 0.5) sum = max(sum-vec3_splat(0.85),vec3_splat(0.0));
    gl_FragColor = vec4(sum,1.0);
}
