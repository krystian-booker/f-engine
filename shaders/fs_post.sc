$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_scene,0);
SAMPLER2D(s_bloom,1);
SAMPLER3D(s_lut,2);
SAMPLER2D(s_depth,3);
uniform vec4 u_post;
uniform vec4 u_postFlags;
uniform vec4 u_screen;
uniform vec4 u_dof;
vec3 toneMap(vec3 x) { return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0); }
void main() {
    vec2 uv = v_texcoord0;
    vec3 sharp = texture2D(s_scene,uv).rgb;
    float deviceDepth=texture2D(s_depth,uv).r;
    float viewDepth=u_dof.x*u_dof.y/max(u_dof.y-deviceDepth*(u_dof.y-u_dof.x),0.001);
    float coc=abs(viewDepth-u_dof.z)/max(viewDepth,0.1)*u_dof.w;
    float tilt=max(0.0,(abs(uv.y-u_post.y)-u_post.z)*5.0);
    float blur=clamp(max(tilt,coc),0.0,1.0)*u_postFlags.y;
    vec3 softened = sharp*0.2;
    for (int i = 0; i < 8; ++i) {
        float angle = float(i)*0.785398;
        vec2 offset = vec2(cos(angle),sin(angle))*u_screen.xy*blur*5.0;
        softened += texture2D(s_scene,uv+offset).rgb*0.1;
    }
    vec3 color = mix(sharp,softened,blur);
    color += texture2D(s_bloom,uv).rgb*u_post.x*u_postFlags.x;
    color = toneMap(color*u_post.w);
    color = pow(color,vec3_splat(1.0/2.2));
    vec3 graded = texture3D(s_lut,clamp(color,0.0,1.0)*(15.0/16.0)+vec3_splat(0.5/16.0)).rgb;
    color = mix(color,graded,u_postFlags.w);
    vec2 centered = uv-0.5;
    color *= 1.0-dot(centered,centered)*0.65*u_postFlags.z;
    gl_FragColor = vec4(color,1.0);
}
