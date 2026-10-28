$input v_texcoord0, v_color0, v_worldPos, v_normal, v_shadowPos
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo,0);
SAMPLER2D(s_normal,1);
SAMPLER2D(s_shadow,2);
uniform vec4 u_materialColor;
uniform vec4 u_materialParams;
uniform vec4 u_ambient;
uniform vec4 u_sunDirection;
uniform vec4 u_sunColor;
uniform vec4 u_cameraPos;
uniform vec4 u_fogColor;
uniform vec4 u_shadowParams;
uniform vec4 u_lightPosRadius[8];
uniform vec4 u_lightColor[8];
float shadowVisibility(vec4 shadowPosition) {
    vec3 p = shadowPosition.xyz / shadowPosition.w;
    vec2 uv = vec2(p.x*0.5+0.5,p.y*u_shadowParams.z*0.5+0.5);
    float depth = p.z*u_shadowParams.y + (1.0-u_shadowParams.y);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth > 1.0 || depth < 0.0) return 1.0;
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        float stored = texture2D(s_shadow,uv+vec2(float(x),float(y))*u_shadowParams.x).r;
        visibility += depth-0.0025 <= stored ? 1.0 : 0.30;
    }
    return mix(1.0,visibility/9.0,u_shadowParams.w);
}
void main() {
    vec4 albedo = texture2D(s_albedo,v_texcoord0)*v_color0*u_materialColor;
    if (albedo.a < u_materialParams.x) discard;
    vec3 normal = normalize(v_normal);
    if (u_materialParams.y > 0.5) {
        vec3 dp1 = dFdx(v_worldPos), dp2 = dFdy(v_worldPos);
        vec2 duv1 = dFdx(v_texcoord0), duv2 = dFdy(v_texcoord0);
        vec3 tangent = normalize(dp1*duv2.y-dp2*duv1.y);
        vec3 bitangent = normalize(-dp1*duv2.x+dp2*duv1.x);
        vec3 detail = texture2D(s_normal,v_texcoord0).xyz*2.0-1.0;
        normal = normalize(tangent*detail.x+bitangent*detail.y+normal*detail.z);
    }
    vec3 light = u_ambient.rgb + u_sunColor.rgb*max(0.0,dot(normal,-u_sunDirection.xyz))*shadowVisibility(v_shadowPos);
    for (int i = 0; i < 8; ++i) {
        vec3 delta = u_lightPosRadius[i].xyz-v_worldPos;
        float distance = length(delta);
        float attenuation = max(0.0,1.0-distance/max(u_lightPosRadius[i].w,0.001));
        light += u_lightColor[i].rgb*attenuation*attenuation*max(0.0,dot(normal,delta/max(distance,0.001)));
    }
    vec3 viewDir = normalize(u_cameraPos.xyz-v_worldPos);
    float rim = pow(1.0-max(0.0,dot(normal,viewDir)),3.0)*u_materialParams.z;
    vec3 color = mix(albedo.rgb*light + u_sunColor.rgb*rim,albedo.rgb,u_materialParams.w);
    float fog = 1.0-exp(-length(u_cameraPos.xyz-v_worldPos)*u_fogColor.w);
    fog *= clamp(1.25-v_worldPos.y*0.08,0.0,1.0);
    gl_FragColor = vec4(mix(color,u_fogColor.rgb,clamp(fog,0.0,0.9)),albedo.a);
}
