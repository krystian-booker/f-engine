#pragma once
#include "engine/core/Json.h"
#include "engine/nav/Grid.h"
#include "engine/world/MapData.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace fe {
struct Name {
    std::string id;
};

struct Tags {
    std::vector<std::string> values;
    [[nodiscard]] bool has(const std::string& tag) const {
        return std::find(values.begin(), values.end(), tag) != values.end();
    }
};

struct Transform {
    glm::vec3 position{0}, previous{0};
    glm::vec2 facing{0, 1};
    float yaw = 0, previousYaw = 0; // radians, used by mesh entities
    glm::vec3 scale{1};
    int layer = 0;
    entt::entity parent = entt::null; // follows the parent with `offset`
    glm::vec3 offset{0};
};

enum class SpriteMode { Upright, Flat, Facing }; // Y-axis billboard, lying on the ground, full billboard

struct Sprite {
    std::string animation; // path of a .anim.json file
    std::string clip = "idle";
    std::string moveClip, idleClip; // optional automatic clip switching
    double time = 0, previousTime = 0;
    float speed = 1;
    int direction = -1; // fixed direction row, -1 = from facing and camera
    bool visible = true, blend = false, unlit = false, castShadow = true;
    SpriteMode mode = SpriteMode::Upright;
    float scale = 1, depthBias = 0;
    glm::vec2 pixelOffset{0};
    glm::vec4 tint{1};
    glm::vec4 flash{1, 1, 1, 0};   // rgb + strength
    glm::vec4 outline{0, 0, 0, 0}; // rgba, alpha 0 = off
    std::string palette;           // palette image (rows of colors)
    int paletteRow = 0;
    bool finished = false; // non-looping clip reached its end
};

struct MeshInstance {
    std::string model;
    std::string animation;
    float time = 0, speed = 1;
    bool loop = true, playing = false, visible = true, castShadow = true;
    glm::vec4 tint{1};
};

struct LightSource {
    LightSettings light; // position is an offset from the entity
    bool enabled = true;
};

struct Emitter {
    EmitterSettings settings;
    bool active = true;
    double accumulator = 0;
    bool burstDone = false;
};

struct Collider {
    float radius = 0.25f, height = 1.6f;
    bool solid = true;
};

struct Mover {
    float speed = 2.5f, maxStep = 0.6f, radius = 0.2f;
    bool collide = true;
    glm::vec2 drive{0}; // direct steering for this tick (units/second)
    std::vector<glm::vec3> waypoints;
    bool active = false;
    double stuckTime = 0;
    float progress = 0;
    // Jump arc
    bool jumping = false;
    glm::vec3 jumpFrom{0}, jumpTo{0};
    float jumpHeight = 1, jumpDuration = 0.5f, jumpTime = 0;
    int jumpLayer = 0;
};

struct AudioEmitter {
    std::string sound;
    float radius = 8, volume = 1;
    int voice = -1;
};

struct Props {
    Json value = Json::object();
};

// Lua registry reference for a script-side data table (managed by bindings).
struct ScriptRef {
    int reference = -2;
};
} // namespace fe
