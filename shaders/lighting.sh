// Shared lighting for world geometry, sprites and water.
uniform vec4 u_ambient;
uniform vec4 u_sunDirection;
uniform vec4 u_sunColor;
uniform vec4 u_cameraPos;
uniform vec4 u_fogColor;     // rgb, density
uniform vec4 u_fogParams;    // height, height falloff
uniform vec4 u_shadowParams; // texel, depth scale, y flip, enabled
uniform vec4 u_lightPosRadius[8];
uniform vec4 u_lightColor[8]; // rgb * intensity, w = cos(inner cone) or -2 for point lights
uniform vec4 u_lightDir[8];   // direction, w = cos(outer cone)

float shadowVisibility(sampler2D shadowMap, vec4 shadowPosition) {
    vec3 p = shadowPosition.xyz / shadowPosition.w;
    vec2 uv = vec2(p.x * 0.5 + 0.5, p.y * u_shadowParams.z * 0.5 + 0.5);
    float depth = p.z * u_shadowParams.y + (1.0 - u_shadowParams.y);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth > 1.0 || depth < 0.0) return 1.0;
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture2D(shadowMap, uv + vec2(float(x), float(y)) * u_shadowParams.x).r;
            visibility += depth - 0.0025 <= stored ? 1.0 : 0.3;
        }
    }
    return mix(1.0, visibility / 9.0, u_shadowParams.w);
}

vec3 pointLights(vec3 position, vec3 normal) {
    vec3 light = vec3_splat(0.0);
    for (int i = 0; i < 8; ++i) {
        vec3 delta = u_lightPosRadius[i].xyz - position;
        float distance = length(delta);
        vec3 direction = delta / max(distance, 0.001);
        float attenuation = max(0.0, 1.0 - distance / max(u_lightPosRadius[i].w, 0.001));
        attenuation *= attenuation;
        if (u_lightColor[i].w > -1.5) {
            float cosAngle = dot(-direction, normalize(u_lightDir[i].xyz));
            attenuation *= smoothstep(u_lightDir[i].w, u_lightColor[i].w, cosAngle);
        }
        light += u_lightColor[i].rgb * attenuation * max(0.0, dot(normal, direction) * 0.8 + 0.2);
    }
    return light;
}

vec3 applyFog(vec3 color, vec3 position) {
    float distanceFog = 1.0 - exp(-length(u_cameraPos.xyz - position) * u_fogColor.w);
    float heightFactor = clamp(exp(-(position.y - u_fogParams.x) * u_fogParams.y), 0.0, 1.0);
    return mix(color, u_fogColor.rgb, clamp(distanceFog * heightFactor, 0.0, 0.92));
}
