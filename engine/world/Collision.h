#pragma once
#include "engine/world/Components.h"
#include "engine/world/Events.h"
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

namespace fe {
// Static boxes plus a spatial hash of every entity with a Transform and a
// Collider. Membership follows component construction and destruction, so
// the hash is exact while scripts run (spawn, collider add/remove, destroy);
// World reports position changes through moved(). Queries reach as far as the
// largest collider radius, which follows construction, updates and removal: a
// collider's radius changes through registry.patch<Collider> (or replace), so
// queries see a grown collider at once, and a shrunk or removed one no longer
// widens them. A query visits the buckets in its reach or, when those
// outnumber the occupied buckets, the occupied ones, so its cost never exceeds
// the number of colliders whatever the reach.
class CollisionSystem {
public:
    CollisionSystem(entt::registry& registry, std::vector<Box> boxes);
    ~CollisionSystem();
    CollisionSystem(const CollisionSystem&) = delete;
    CollisionSystem& operator=(const CollisionSystem&) = delete;

    [[nodiscard]] const std::vector<Box>& boxes() const { return boxes_; }
    void setBoxes(std::vector<Box> boxes) { boxes_ = std::move(boxes); }
    void moved(entt::entity entity);
    // Re-buckets every collider; picks up transforms edited outside World (editor).
    void refresh();
    [[nodiscard]] bool blockedByBox(glm::vec3 position, float radius, float height) const;
    // Whether a solid collider other than `self` (and those in `ignore`) overlaps a body there.
    [[nodiscard]] bool blockedByEntity(entt::entity self, glm::vec3 position, float radius,
                                       std::span<const entt::entity> ignore = {}) const;
    void updateContacts(EventQueue& events);

    // Nearest static box or entity collider (an upright cylinder from the
    // entity's feet) a ray crosses within `maxDistance`; `direction` is a
    // unit vector. Entities are tested only when `accept` returns true.
    struct Hit {
        float distance = 0;
        entt::entity entity = entt::null; // null for a static box
        int box = -1;
    };
    [[nodiscard]] std::optional<Hit> raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance,
                                             const std::function<bool(entt::entity)>& accept) const;

private:
    void track(entt::registry& registry, entt::entity entity);
    void resized(entt::registry& registry, entt::entity entity);
    void untrack(entt::registry& registry, entt::entity entity);
    template<class Visit> void forEachNear(glm::vec3 position, float reach, Visit&& visit) const;
    void insert(entt::entity entity, std::int64_t key);
    void erase(entt::entity entity);
    [[nodiscard]] float maxRadius() const { return radii_.empty() ? 0.0f : *radii_.rbegin(); }

    struct Tracked {
        std::int64_t key = 0; // bucket
        float radius = 0;     // as counted in radii_
    };
    entt::registry& registry_;
    std::vector<Box> boxes_;
    std::unordered_map<std::int64_t, std::vector<entt::entity>> buckets_; // occupied buckets only
    std::unordered_map<entt::entity, Tracked> tracked_;
    std::multiset<float> radii_; // every tracked collider's radius
    using Pair = std::pair<entt::entity, entt::entity>;
    std::vector<Pair> contacts_, current_; // sorted
};

// Emits zone_enter/zone_exit for every entity with a Transform and either a
// Mover or a Collider (anything that walks or can touch things). Zones
// removed at run time count as left.
void updateZones(entt::registry& registry, const std::vector<Zone>& zones, EventQueue& events);
} // namespace fe
