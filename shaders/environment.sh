// Per-world constants shared by the scene vertex and fragment shaders.
uniform vec4 u_env[10];
#define u_ambient       u_env[0]
#define u_sunDirection  u_env[1]
#define u_sunColor      u_env[2]
#define u_cameraPos     u_env[3]
#define u_fogColor      u_env[4] // rgb, density
#define u_fogParams     u_env[5] // height, height falloff, strongest blend
#define u_shadowParams  u_env[6] // strength (0 = off), 1 / atlas width, 1 / atlas height
#define u_lightGrid     u_env[7] // origin x, origin z, 1 / cell size, cells across
#define u_lightGridSize u_env[8] // cells deep, 1 / grid texture width, 1 / grid texture height, 1 / light texture width
#define u_lightShadowParams u_env[9] // any light shadows (0/1), 1 / atlas width, 1 / atlas height
