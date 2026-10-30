#pragma once
#include "engine/core/Json.h"
#include "engine/nav/Query.h"
#include "engine/world/MapData.h"
#include <array>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace fe {
struct Name {
    std::string id;
};

// A number unique to one entity of a world, never reused: entt recycles
// entity slots and wraps their versions (4096 for a slot), so a script handle
// checks the serial to know it still names the entity it was made for.
struct EntitySerial {
    std::uint64_t value = 0;
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

// Y-axis billboard, lying flat at the feet's height, full billboard, or
// projected onto the grid surface of its layer (following ramps).
enum class SpriteMode { Upright, Flat, Facing, Decal };

struct Sprite {
    std::string animation; // path of a .anim.json file
    std::string clip = "idle";
    std::string moveClip, idleClip; // optional automatic clip switching
    double time = 0, previousTime = 0;
    float speed = 1;
    int direction = -1; // fixed direction row, -1 = from facing and camera
    bool visible = true, unlit = false, castShadow = true;
    BlendMode blend = BlendMode::Opaque; // opaque sprites with a tint alpha below 1 draw as alpha
    SpriteMode mode = SpriteMode::Upright;
    float scale = 1, depthBias = 0;
    glm::vec2 pixelOffset{0};
    glm::vec4 tint{1};
    glm::vec4 flash{1, 1, 1, 0};   // rgb + strength
    glm::vec4 outline{0, 0, 0, 0}; // rgba, alpha 0 = off
    float outlineWidth = 1;        // atlas texels the outline reaches out, 0.5 to 4
    glm::vec4 silhouette{0};       // rgba drawn where map geometry or meshes hide the sprite; alpha 0 = off
    std::optional<bool> pixelSnap; // unset: the environment's pixel_snap
    std::string palette;           // palette image (rows of colors)
    int paletteRow = 0;
    float distort = 0;     // >0: bends the scene behind it (shock waves, heat) instead of drawing
    std::string shader;    // game fragment shader (shaders/fs_<name>.sc) for its colour; empty = the engine's
    bool finished = false; // non-looping clip reached its end
};

struct MeshInstance {
    std::string model;
    std::string animation;
    float time = 0, speed = 1;
    bool loop = true, playing = false, visible = true, castShadow = true;
    glm::vec4 tint{1};
    std::optional<BlendMode> blend; // unset: each glTF material's alpha mode
    std::string shader;             // game fragment shader for every part; empty: each material's own
};

struct LightSource {
    LightSettings light; // position is an offset from the entity
    bool enabled = true;
};

// The particle system counts each emitter's live particles itself, so
// replacing the component (new settings) keeps the cap of what is still alive.
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

// A point on a mover's route. `layer` is the grid layer on arrival (-1 =
// wherever links lead); `kind` says how the mover gets there: walking, a jump
// arc or a ladder climb.
struct Waypoint {
    glm::vec3 position{0};
    int layer = -1;
    StepKind kind = StepKind::Walk;
};

// What a mover is doing. A followed route may pass through a jump or a
// climb: the motion is Jump or Climb while the arc or the ladder lasts and
// goes back to the route (or ends with `arrived`) afterwards. Launch is a
// ballistic flight under gravity, never part of a route.
enum class Motion : std::uint8_t { None, Route, Jump, Climb, Launch };

// A jump in flight: a parabola from `from` to `to` over `duration` seconds,
// landing on layer `layer`.
struct JumpArc {
    glm::vec3 from{0}, to{0};
    float height = 1, duration = 0.5f, time = 0;
    int fromLayer = 0, layer = 0;
};

// A ladder climb: straight segments through `points` at their speeds, from
// `start` (on `fromLayer`) to the last point (on `layer`).
struct LadderClimb {
    std::vector<std::pair<glm::vec3, float>> points;
    glm::vec3 start{0};
    glm::vec2 facing{0, -1};
    int fromLayer = 0, layer = 0;
};

// A ballistic flight (MovementSystem::launch): `velocity` in units/second,
// falling under MovementTuning::gravity, from `fromLayer`.
struct Ballistic {
    glm::vec3 velocity{0};
    int fromLayer = 0;
};

struct Mover {
    // Settings
    float speed = 2.5f, maxStep = 0.6f, radius = 0.2f;
    float climbSpeed = 1.5f; // ladder climbing, units/second
    JumpRules jump;          // gap jumps when planning paths (distance 0 = none)
    bool collide = true;
    // Cells per side the body covers, anchored at its cell and extending
    // towards +x and +z (StepRules::footprint); 1 = one cell.
    int footprint = 1;
    // Flying: stands on and paths over unwalkable cells and height steps;
    // grid bounds and blocked tags still hold (StepRules::fly).
    bool fly = false;
    // Passability class, flying or not: cells carrying one of blockedTags,
    // or (when requiredTags is not zero) none of requiredTags, can be neither
    // stood on nor pathed through (StepRules::blockedTags/requiredTags). A
    // boat is {fly = true, requiredTags = water}.
    std::uint32_t blockedTags = 0, requiredTags = 0;
    glm::vec2 drive{0}; // direct steering for this tick (units/second)

    // State, owned by MovementSystem
    Motion motion = Motion::None;
    bool onRoute = false; // the jump or climb is a step of `waypoints`: `arrived` follows it
    std::vector<Waypoint> waypoints;
    double stuckTime = 0;
    JumpArc arc;
    LadderClimb climb;
    Ballistic flight;
    bool moved = false; // displaced by its own walking, jumping or climbing this tick
    GridPos cell{0, 0, -1}; // last cell reported with cell_enter (layer -1 = none yet)
};

// A moving surface (rafts, lifts). Its top is the entity's position; the
// footprint is `size` (x by z) centred on it. Movers and colliders standing on
// it ride along, and grid cells whose centre it covers on its layer become
// walkable at its height while it is there.
struct Platform {
    glm::vec2 size{1, 1};
};

struct AudioEmitter {
    std::string sound;
    float radius = 8, volume = 1;
    int voice = -1;
};

struct Props {
    Json value = Json::object();
};

// Numbers a game shader reads as `u_params[4]` (sprites and meshes with a
// `shader`): 16 values, zero until a script sets them. Simulation data like
// every other component, so saves, hot reload and tests see them; the
// renderer only reads them.
struct MaterialParams {
    std::array<glm::vec4, 4> values{};
};

// Lua registry reference for a script-side data table (managed by bindings).
struct ScriptRef {
    int reference = -2;
};
} // namespace fe
