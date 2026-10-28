#pragma once
#include "engine/nav/Grid.h"
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace fe {
enum class LightType { Point, Spot };

struct LightSettings {
    LightType type = LightType::Point;
    glm::vec3 position{0, 2, 0};
    glm::vec3 color{1, 0.75f, 0.45f};
    glm::vec3 direction{0, -1, 0};
    float radius = 5, intensity = 2;
    float innerCone = 20, outerCone = 35; // degrees, spot lights
    float flicker = 0, flickerSpeed = 8;  // fractional intensity wobble
    bool castShadows = false;
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
    float stretch = 0; // elongates particles along their velocity (rain)
    glm::vec4 color{1}, colorEnd{1, 1, 1, 0};
    bool unlit = true, worldSpace = true;
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

struct PostSettings {
    bool bloom = true, depthOfField = true, vignette = true, colorGrade = true;
    float bloomStrength = 0.3f, bloomThreshold = 0.85f;
    float focus = 0.55f, focusRange = 0.25f, focusDistance = 0, aperture = 1.5f;
    float exposure = 1, vignetteStrength = 0.65f;
    std::string lut, lutBlendTarget;
    float lutBlend = 0;
    int pixelScale = 1; // >1 renders the 3D scene at reduced resolution
};

struct Environment {
    glm::vec3 ambient{0.3f, 0.35f, 0.45f}, sunColor{1, 0.88f, 0.7f}, sunDirection{-0.6f, -1, -0.4f};
    glm::vec3 fogColor{0.12f, 0.17f, 0.23f};
    float fogDensity = 0.025f, fogHeight = 0, fogHeightFalloff = 0.08f;
    bool shadows = true;
    float shadowDistance = 24;
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
    Json props = Json::object();
};

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

struct MapEmitter {
    glm::vec3 position{0};
    EmitterSettings settings;
};

// A map file (format 2). The engine understands geometry, the grid, lights,
// emitters, colliders and environment; everything game-specific is a
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
