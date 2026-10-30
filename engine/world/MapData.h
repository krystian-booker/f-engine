#pragma once
#include "engine/nav/Grid.h"
#include <cmath>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fe {
enum class LightType { Point, Spot };

// How a sprite, particle or mesh part combines with what is behind it.
// Opaque writes depth (with an alpha cutout); the rest are drawn after the
// opaque scene without writing depth: alpha and premultiplied sorted back to
// front, then multiply, then additive.
enum class BlendMode { Opaque, Alpha, Additive, Multiply, Premultiplied };
[[nodiscard]] const char* blendModeName(BlendMode mode);
// Throws for unknown names.
[[nodiscard]] BlendMode blendModeFromName(const std::string& name);
// A JSON string, or a boolean (true = alpha, false = opaque).
[[nodiscard]] BlendMode blendModeFromJson(const Json& value);

// Light radius limit (world units), for maps and scripts alike.
inline constexpr float MaxLightRadius = 1000;

struct LightSettings {
    LightType type = LightType::Point;
    glm::vec3 position{0, 2, 0};
    glm::vec3 color{1, 0.75f, 0.45f};
    glm::vec3 direction{0, -1, 0};
    float radius = 5, intensity = 2;
    float innerCone = 20, outerCone = 35; // degrees from the axis, spot lights: 0 <= inner < outer <= 90
    float flicker = 0, flickerSpeed = 8;  // fractional intensity noise and its rate
    // Intensity multiplier keyframes {seconds, scale}, looping over the last key's time.
    std::vector<glm::vec2> flickerCurve;
    bool castShadows = false;
    // Intensity multiplier at `time`: the flicker curve times the noise wobble.
    // `seed` decorrelates the noise of different lights.
    [[nodiscard]] float flickerScale(double time, float seed) const;
    static LightSettings fromJson(const Json& data);
    [[nodiscard]] Json toJson() const;
};

struct EmitterSettings {
    std::string sprite;
    float rate = 10; // particles per second
    int burst = 0;   // particles on start
    float lifetime = 1, lifetimeJitter = 0.25f;
    glm::vec3 velocity{0, 1, 0}, velocityJitter{0.3f};
    glm::vec3 gravity{0}, area{0};
    float size = 0.2f, sizeEnd = 0.2f, drag = 0;
    float stretch = 0;                        // elongates particles along their velocity (rain)
    glm::vec4 color{1}, colorEnd{1, 1, 1, 0}; // linear; values above 1 glow through bloom
    BlendMode blend = BlendMode::Alpha;
    bool unlit = true, worldSpace = true;
    float distort = 0; // >0: particles only bend the scene behind them (heat haze), this strong
    int maxParticles = 512;
    static EmitterSettings fromJson(const Json& data);
    [[nodiscard]] Json toJson() const;
};

struct Backdrop {
    std::string image;
    float distance = 60, height = 20, width = 80, yaw = 0, elevation = 0, parallax = 0.2f;
    glm::vec4 tint{1};
};

struct SkySettings {
    std::string mode = "gradient"; // none | color | gradient | panorama
    glm::vec3 top{0.25f, 0.42f, 0.70f}, horizon{0.70f, 0.78f, 0.86f}, bottom{0.32f, 0.36f, 0.40f};
    std::string panorama;
    float cloudCover = 0, cloudSpeed = 0.01f;
    glm::vec3 cloudColor{1};
    std::vector<Backdrop> backdrops;
};

struct WeatherSettings {
    std::string kind = "none"; // none | rain | snow | leaves | embers | dust | custom
    float intensity = 0;
    glm::vec3 wind{0};
    glm::vec4 color{1};
    std::string sprite; // custom particle sprite
};

// A game post shader (<game>/shaders/<shader>.sc) and its numbers.
struct CustomPost {
    std::string shader;
    std::vector<float> params; // at most 16
};

struct PostSettings {
    bool bloom = true, depthOfField = true, vignette = true, colorGrade = true;
    float bloomStrength = 0.3f, bloomThreshold = 0.85f;
    float bloomKnee = 0.5f;       // soft threshold width, fraction of the threshold
    float bloomRadius = 0.75f;    // 0..1: weight of each wider bloom level
    int bloomLevels = 6;          // mip levels, 1..8
    std::string dofMode = "both"; // depth | tilt | both
    float focus = 0.55f, focusRange = 0.25f, focusDistance = 0, aperture = 1.5f;
    float dofRadius = 10; // widest blur, screen pixels
    bool ssao = false;
    float ssaoRadius = 0.6f, ssaoStrength = 1, ssaoBias = 0.03f;
    std::string antialias = "auto"; // auto (FXAA unless pixel_scale > 1) | fxaa | none
    float chromaticAberration = 0;
    float distortion = 1; // scales distort sprites and particles
    float exposure = 1, vignetteStrength = 0.65f;
    std::string tonemap = "aces"; // aces | none
    std::string lut, lutBlendTarget;
    float lutBlend = 0;
    // 1..8: the 3D scene renders at the output size over this and is scaled
    // up in the composite, with nearest filtering for a whole number (crisp
    // pixel art) and bilinear otherwise (dynamic resolution).
    float pixelScale = 1;
    std::vector<CustomPost> custom;
    [[nodiscard]] bool fxaa() const {
        return antialias == "fxaa" || (antialias == "auto" && pixelScale == 1);
    }
    // The scene is upsampled with nearest filtering: a whole scale above 1.
    [[nodiscard]] bool pixelated() const { return pixelScale > 1 && pixelScale == std::floor(pixelScale); }
};

// Look of `material: water` surfaces.
struct WaterSettings {
    float waveScale = 0.6f, waveSpeed = 1, fresnel = 1, specular = 0.6f;
    glm::vec3 deepColor{0.02f, 0.08f, 0.12f};
    float depthFalloff = 0.8f; // per unit of water depth; 0 disables the depth tint
    float reflection = 0.6f;   // 0..1: screen-space reflection of the scene (sky colour where it misses)
    float refraction = 0.04f;  // screen-space offset of the scene seen through the surface
};

// Colours here are linear values; lighting and the sky are computed in linear
// space and encoded to sRGB by the post pass.
struct Environment {
    glm::vec3 ambient{0.3f, 0.35f, 0.45f}, sunColor{1, 0.88f, 0.7f}, sunDirection{-0.6f, -1, -0.4f};
    glm::vec3 fogColor{0.12f, 0.17f, 0.23f};
    float fogDensity = 0.025f, fogHeight = 0, fogHeightFalloff = 0.08f;
    float fogMax = 0.92f; // strongest fog blend
    bool shadows = true;
    float shadowDistance = 24;      // how far past the camera focus sun shadows reach
    float shadowStrength = 0.7f;    // 1 = shadowed areas get no sunlight
    float shadowBias = 0.03f;       // world units
    float spriteLightHeight = 0.8f; // sprites take point light at this height above their feet
    bool anisotropic = true;        // anisotropic filtering of map and mesh textures
    bool pixelSnap = false;         // sprites move in whole scene pixels (per-sprite override)
    WaterSettings water;
    SkySettings sky;
    WeatherSettings weather;
    PostSettings post;
    static Environment fromJson(const Json& data);
    [[nodiscard]] Json toJson() const;
};

struct MapPoint {
    glm::vec3 position{0};
    glm::vec2 facing{0, 1};
    int layer = 0;
};

// An authored object. `type` and `props` are interpreted by scripts.
struct Placement {
    std::string id, type;
    glm::vec3 position{0};
    glm::vec2 facing{0, 1};
    int layer = 0;
    float yaw = 0;      // degrees about the vertical axis (meshes); written only when not 0
    glm::vec3 scale{1}; // written only when not 1 (a number when uniform)
    Json props = Json::object();
};

// Fires zone_enter/zone_exit for entities with a mover or a collider.
struct Zone {
    std::string id, type;
    glm::vec2 min{0}, max{0};
    int layer = -1; // -1 = every layer
    Json props = Json::object();
    [[nodiscard]] bool contains(glm::vec3 p, int l) const {
        return (layer < 0 || layer == l) && p.x >= min.x && p.x <= max.x && p.z >= min.y && p.z <= max.y;
    }
};

struct Box {
    glm::vec3 min{0}, max{0};
};

// An author's correction to derived walkability: a rectangle of `w` x `d`
// cells from (x, z) on `layer` whose listed fields replace what the bake
// wrote. Overrides apply in order after every bake, so they survive re-bakes.
struct CellOverride {
    int x = 0, z = 0, layer = 0, w = 1, d = 1;
    std::optional<bool> walkable;
    std::optional<float> height;
    std::optional<Ramp> ramp; // with `rise`
    float rise = 1;
    std::vector<std::string> tags, untags; // tag names added and removed
};

// Derived walkability: what the editor's bake (engine/editor/WalkBake.h)
// wrote from the terrain and the placed models, kept apart from authored
// data. `colliders` are the quarter-cell boxes of partly covered cells; the
// runtime adds them to `MapData::colliders`. `options` holds the bake's
// settings as JSON (the engine does not read them), `fingerprint` names the
// inputs they were baked from (empty: never baked) and `autoBake` says
// whether the editor re-bakes as placements change.
struct BakeData {
    bool autoBake = true;
    std::string fingerprint;
    Json options = Json::object();
    std::vector<Box> colliders;
    std::vector<CellOverride> overrides;
    // Nothing worth writing: a map that was never baked.
    [[nodiscard]] bool empty() const {
        return autoBake && fingerprint.empty() && options.empty() && colliders.empty() && overrides.empty();
    }
};

struct MapEmitter {
    glm::vec3 position{0};
    EmitterSettings settings;
};

// A map file (format 2). The engine understands geometry, the grid, lights,
// emitters, colliders, the bake and environment; everything game-specific is a
// placement, zone or prop that scripts interpret.
struct MapData {
    std::string id, name, geometry = "geometry.glb";
    Grid grid;
    std::map<std::string, MapPoint> points;
    std::vector<Placement> placements;
    std::vector<Zone> zones;
    std::vector<LightSettings> lights;
    std::vector<MapEmitter> emitters;
    std::vector<Box> colliders;
    BakeData bake;
    Environment environment;
    Json props = Json::object();

    static MapData load(const std::filesystem::path& path);
    static MapData fromJson(const Json& data);
    [[nodiscard]] Json toJson() const;
};

glm::vec2 jsonVec2(const Json& value);
glm::vec3 jsonVec3(const Json& value);
glm::vec4 jsonVec4(const Json& value);
Json toJson(glm::vec2 v);
Json toJson(glm::vec3 v);
Json toJson(glm::vec4 v);
} // namespace fe
