#pragma once
#include "engine/core/AssetCache.h"
#include "engine/core/Random.h"
#include "engine/world/Camera.h"
#include "engine/world/Components.h"
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace fe {
struct WorldEvent {
    enum class Kind {
        ContactBegin,
        ContactEnd,
        Arrived,
        Blocked,
        Landed,
        AnimationEvent,
        AnimationFinished,
        ZoneEnter,
        ZoneExit
    };
    Kind kind;
    entt::entity a = entt::null, b = entt::null;
    std::string name; // animation event name, clip, or zone id
};

struct Particle {
    glm::vec3 position{0}, velocity{0};
    float age = 0, lifetime = 1, size = 0.2f, sizeEnd = 0.2f, drag = 0, stretch = 0;
    glm::vec3 gravity{0};
    glm::vec4 color{1}, colorEnd{1};
    std::uint16_t sprite = 0;
    bool unlit = true;
};

struct OverlayLayer {
    std::vector<GridPos> cells;
    glm::vec4 color{0.3f, 0.6f, 1, 0.45f};
    float pulse = 0;
    int order = 0;
    bool visible = true, border = true;
};

struct OverlayPath {
    std::vector<GridPos> cells;
    glm::vec4 color{1, 0.9f, 0.4f, 0.9f};
};

struct OverlayCursor {
    bool visible = false;
    GridPos cell;
    glm::vec4 color{1, 1, 1, 0.9f};
};

// A loaded map with its entities. Several worlds can exist at once (a field
// map kept alive under a battle stage). Purely simulation data: renderers
// and scripts read and change it; nothing here knows any game rules.
class World {
public:
    World(std::string name, MapData map, AssetCache& assets, std::uint64_t seed);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] MapData& map() { return map_; }
    [[nodiscard]] const MapData& map() const { return map_; }
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
    [[nodiscard]] entt::entity find(const std::string& id) const;
    [[nodiscard]] std::vector<entt::entity> query(const std::string& tag, std::optional<glm::vec3> centre,
                                                  float radius) const;
    void rename(entt::entity entity, const std::string& id);
    void setDestroyHook(std::function<void(entt::entity)> hook) { destroyHook_ = std::move(hook); }

    // Movement
    void moveTo(entt::entity entity, glm::vec3 target, bool findPath);
    void stop(entt::entity entity);
    void jump(entt::entity entity, glm::vec3 target, float height, float duration, int layer);
    // Instantly places an entity, snapping height to the grid unless `keepHeight`.
    void place(entt::entity entity, glm::vec3 position, int layer, bool keepHeight = false);
    [[nodiscard]] bool canOccupy(entt::entity entity, glm::vec3 position, int layer) const;
    [[nodiscard]] bool moving(entt::entity entity) const;

    void setClip(entt::entity entity, const std::string& clip, bool restart = true);
    void playMesh(entt::entity entity, const std::string& animation, bool loop);
    void burst(glm::vec3 position, const EmitterSettings& settings, int count);

    // Stores interpolation state; call before scripts change anything this tick.
    void beginTick();
    void tick(double dt);
    [[nodiscard]] std::string geometryPath() const { return "maps/" + map_.id + "/" + map_.geometry; }
    std::vector<WorldEvent> takeEvents();

    [[nodiscard]] const std::vector<Particle>& particles() const { return particles_; }
    [[nodiscard]] const std::vector<std::string>& particleSprites() const { return particleSprites_; }
    std::map<std::string, OverlayLayer>& overlays() { return overlays_; }
    std::map<std::string, OverlayPath>& overlayPaths() { return overlayPaths_; }
    OverlayCursor& cursor() { return cursor_; }
    [[nodiscard]] const std::vector<Box>& colliders() const { return colliders_; }
    // World-space light list for this frame (static + entity lights, flicker applied).
    [[nodiscard]] std::vector<LightSettings> activeLights() const;
    [[nodiscard]] glm::vec3 interpolated(entt::entity entity, double alpha) const;

private:
    void advanceAnimations(double dt);
    void updateMovers(double dt);
    void updateAttachments();
    void updateEmitters(double dt);
    void updateWeather(double dt);
    void updateParticles(double dt);
    void updateContacts();
    void updateZones();
    bool stepTowards(entt::entity entity, glm::vec2 delta);
    void updateLayer(Transform& transform, const Mover& mover, glm::vec2 position, glm::vec2 moved) const;
    [[nodiscard]] bool blockedByCollider(glm::vec3 position, float radius, float height) const;
    [[nodiscard]] bool blockedByEntity(entt::entity self, glm::vec3 position, float radius) const;
    void spawnParticle(const EmitterSettings& settings, glm::vec3 origin);
    std::uint16_t particleSprite(const std::string& path);
    void rebuildHash();

    std::string name_;
    MapData map_;
    AssetCache& assets_;
    entt::registry registry_;
    Camera camera_;
    Random random_;
    double time_ = 0;
    double weatherAccumulator_ = 0;
    std::unordered_map<std::string, entt::entity> names_;
    std::vector<Box> colliders_;
    std::vector<WorldEvent> events_;
    std::vector<Particle> particles_;
    std::vector<std::string> particleSprites_;
    std::map<std::string, OverlayLayer> overlays_;
    std::map<std::string, OverlayPath> overlayPaths_;
    OverlayCursor cursor_;
    std::unordered_map<std::int64_t, std::vector<entt::entity>> hash_;
    std::set<std::pair<entt::entity, entt::entity>> contacts_;
    std::map<entt::entity, std::set<std::string>> zonesInside_;
    std::function<void(entt::entity)> destroyHook_;
};
} // namespace fe
