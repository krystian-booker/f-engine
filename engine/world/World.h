#pragma once
#include "engine/core/AssetCache.h"
#include "engine/core/Random.h"
#include "engine/world/Camera.h"
#include "engine/world/Collision.h"
#include "engine/world/Components.h"
#include "engine/world/Events.h"
#include "engine/world/Movement.h"
#include "engine/world/Overlays.h"
#include "engine/world/Particles.h"
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

namespace fe {
// What World::raycast hit.
struct WorldHit {
    enum class Kind { Grid, Collider, Entity };
    Kind kind = Kind::Grid;
    float distance = 0;
    glm::vec3 point{0};
    GridPos cell;                     // Grid hits
    int collider = -1;                // Collider hits: index into colliders()
    entt::entity entity = entt::null; // Entity hits
};

struct RayOptions {
    float maxDistance = 200;
    std::uint32_t layers = ~0u; // grid layers that can be hit (bit per layer)
    bool entities = true;
    bool solidOnly = true;         // entities with a non-solid collider (triggers) are not hit
    std::vector<std::string> tags; // when not empty, only entities with one of these tags are hit
    int pixelsPerUnit = 32;        // for sprites whose animation does not set one
    double alpha = 1;              // camera interpolation that orients sprites
};

// Flicker noise seeds, fixed for each light whatever else is lit: map lights
// by their authored position (lights at the same position flicker together;
// moving one changes its phase), entity lights by their entity.
[[nodiscard]] float flickerSeed(const LightSettings& mapLight);
[[nodiscard]] float flickerSeed(entt::entity light);

// A loaded map with its entities. Several worlds can exist at once (a field
// map kept alive under a battle stage). Purely simulation data: renderers
// and scripts read and change it; nothing here knows any game rules. The
// world owns the entity registry and the systems that update it: movement
// and collision, animation, particles and weather, overlays and the camera.
class World {
public:
    World(std::string name, MapData map, AssetCache& assets, std::uint64_t seed);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // The engine skips tick() while paused (the world still renders and
    // scripts may still change it) and otherwise advances it by dt * timeScale.
    bool paused = false;
    float timeScale = 1;

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] MapData& map() { return map_; }
    [[nodiscard]] const MapData& map() const { return map_; }
    // The map as loaded from disk plus editor edits; scripts never change it.
    [[nodiscard]] MapData& authored() { return authored_; }
    [[nodiscard]] const MapData& authored() const { return authored_; }
    [[nodiscard]] Grid& grid() { return map_.grid; }
    [[nodiscard]] Environment& environment() { return map_.environment; }
    [[nodiscard]] Camera& camera() { return camera_; }
    [[nodiscard]] const Camera& camera() const { return camera_; }
    [[nodiscard]] entt::registry& registry() { return registry_; }
    [[nodiscard]] const entt::registry& registry() const { return registry_; }
    [[nodiscard]] AssetCache& assets() { return assets_; }
    [[nodiscard]] double time() const { return time_; }

    entt::entity spawn(const std::string& id, glm::vec3 position, int layer = 0);
    void destroy(entt::entity entity);
    [[nodiscard]] bool valid(entt::entity entity) const { return registry_.valid(entity); }
    // The entity's EntitySerial (spawned entities and map emitters have one);
    // NoSerial for an invalid entity or one without.
    static constexpr std::uint64_t NoSerial = ~std::uint64_t{0};
    [[nodiscard]] std::uint64_t serial(entt::entity entity) const {
        const auto* serial = registry_.valid(entity) ? registry_.try_get<EntitySerial>(entity) : nullptr;
        return serial ? serial->value : NoSerial;
    }
    // Whether `entity` is still the entity that had `serial`.
    [[nodiscard]] bool alive(entt::entity entity, std::uint64_t serial) const {
        return serial != NoSerial && this->serial(entity) == serial;
    }
    [[nodiscard]] entt::entity find(const std::string& id) const;
    [[nodiscard]] std::vector<entt::entity> query(const std::string& tag, std::optional<glm::vec3> centre,
                                                  float radius) const;
    void rename(entt::entity entity, const std::string& id);
    void setDestroyHook(std::function<void(entt::entity)> hook) { destroyHook_ = std::move(hook); }

    // Movement (see MovementSystem). `rules`, when given, plan the route
    // instead of the mover's defaults.
    void moveTo(entt::entity entity, glm::vec3 target, bool findPath, std::optional<int> layer = std::nullopt,
                const StepRules* rules = nullptr) {
        movement_.moveTo(entity, target, findPath, layer, rules);
    }
    [[nodiscard]] MovementTuning& movementTuning() { return movement_.tuning; }
    [[nodiscard]] const MovementTuning& movementTuning() const { return movement_.tuning; }
    void follow(entt::entity entity, std::vector<Waypoint> route) {
        movement_.follow(entity, std::move(route));
    }
    void stop(entt::entity entity) { movement_.stop(entity); }
    void jump(entt::entity entity, glm::vec3 target, float height, float duration, int layer) {
        movement_.jump(entity, target, height, duration, layer);
    }
    // A ballistic flight under MovementTuning::gravity (see MovementSystem::launch).
    void launch(entt::entity entity, glm::vec3 velocity) { movement_.launch(entity, velocity); }
    // Instantly places an entity, snapping height to the grid unless `keepHeight`.
    void place(entt::entity entity, glm::vec3 position, int layer, bool keepHeight = false) {
        movement_.place(entity, position, layer, keepHeight);
    }
    [[nodiscard]] bool canOccupy(entt::entity entity, glm::vec3 position, int layer) const {
        return movement_.canOccupy(entity, position, layer);
    }
    [[nodiscard]] bool moving(entt::entity entity) const { return movement_.moving(entity); }
    void setRouteVisitLimit(int limit) { movement_.setRouteVisitLimit(limit); }

    void setClip(entt::entity entity, const std::string& clip, bool restart = true);
    void playMesh(entt::entity entity, const std::string& animation, bool loop);
    void burst(glm::vec3 position, const EmitterSettings& settings, int count) {
        particles_.burst(position, settings, count);
    }

    // Stores interpolation state; call before scripts change anything this tick.
    void beginTick();
    void tick(double dt);
    [[nodiscard]] std::string geometryPath() const { return "maps/" + map_.id + "/" + map_.geometry; }
    // Events since the last call. Events naming an entity destroyed since they
    // were queued are dropped (a contact with something destroyed in the same
    // tick is never delivered, so no event names an entity that is gone).
    std::vector<WorldEvent> takeEvents();

    [[nodiscard]] const std::vector<Particle>& particles() const { return particles_.particles(); }
    // Particles alive from an emitter entity.
    [[nodiscard]] int liveParticles(entt::entity emitter) const { return particles_.live(emitter); }
    [[nodiscard]] const std::vector<std::string>& particleSprites() const { return particles_.sprites(); }
    std::map<std::string, OverlayLayer>& overlays() { return overlays_.layers; }
    std::map<std::string, OverlayPath>& overlayPaths() { return overlays_.paths; }
    OverlayCursor& cursor() { return overlays_.cursor; }
    [[nodiscard]] const std::vector<Box>& colliders() const { return collision_.boxes(); }
    // Applies the live map's colliders (plus geometry marker boxes) and, when
    // they changed, its emitters, replacing the emitter entities made from the
    // map; lights are read from the map every frame. The editor calls this
    // after edits; it also refreshes the spatial hash (refreshCollision()).
    void rebuildStatic();
    // Re-buckets every collider. Movement, place() and spawn keep the spatial
    // hash current; code that writes Transform::position directly (the
    // editor) calls this afterwards.
    void refreshCollision() { collision_.refresh(); }

    // First thing a ray hits: grid surfaces and cliff faces, static colliders,
    // and entities (their collider, or their sprite's quad when they have no
    // collider). `direction` need not be normalized.
    [[nodiscard]] std::optional<WorldHit> raycast(glm::vec3 origin, glm::vec3 direction,
                                                  const RayOptions& options) const;
    // The visible sprite nearest along a ray (the one drawn on top), or null.
    [[nodiscard]] entt::entity pickSprite(glm::vec3 origin, glm::vec3 direction, double alpha,
                                          int pixelsPerUnit) const;
    // World-space light list for this frame (static + entity lights, flicker applied).
    [[nodiscard]] std::vector<LightSettings> activeLights() const;
    [[nodiscard]] glm::vec3 interpolated(entt::entity entity, double alpha) const;

private:
    std::string name_;
    MapData map_;
    MapData authored_;
    AssetCache& assets_;
    entt::registry registry_;
    Camera camera_;
    Random random_;
    double time_ = 0;
    EventQueue events_;
    CollisionSystem collision_;
    MovementSystem movement_;
    ParticleSystem particles_;
    Overlays overlays_;
    std::unordered_map<std::string, entt::entity> names_;
    std::function<void(entt::entity)> destroyHook_;
    Json builtEmitters_; // map emitters the emitter entities were made from
    std::uint64_t nextSerial_ = 0;
    // A new entity with its EntitySerial.
    entt::entity create();
};
} // namespace fe
