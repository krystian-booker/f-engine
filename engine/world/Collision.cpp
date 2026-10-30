#include "engine/world/Collision.h"
#include "engine/world/Geometry.h"
#include "engine/nav/Query.h"
#include "engine/nav/Grid.h"
#include <algorithm>
#include <cmath>

namespace fe {
namespace {
constexpr float HashCell = 2.0f;

// Saturated (floorCell), so a far-away or non-finite position still has a bucket.
int bucketCoord(float value) {
    return floorCell(value / HashCell);
}

// A collider radius as counted for the queries' reach: NaN (which would break
// the ordered set) counts as 0 and anything negative as 0.
float reachOf(float radius) {
    return radius > 0 ? radius : 0.0f;
}

std::int64_t bucketKey(int x, int z) {
    return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(z));
}

std::int64_t bucketKey(glm::vec3 p) {
    return bucketKey(bucketCoord(p.x), bucketCoord(p.z));
}


// Whether two upright bodies share any height (feet at `ay` and `by`).
// Heights below a hair count as a hair, so flat colliders still touch.
bool overlapVertically(float ay, float ah, float by, float bh) {
    constexpr float Hair = 1e-3f;
    return ay < by + std::max(bh, Hair) && by < ay + std::max(ah, Hair);
}

// Zone ids an entity is currently inside (internal to zone tracking).
struct ZonePresence {
    std::vector<std::string> inside;
};
} // namespace

CollisionSystem::CollisionSystem(entt::registry& registry, std::vector<Box> boxes)
    : registry_(registry), boxes_(std::move(boxes)) {
    indexBoxes();
    registry_.on_construct<Collider>().connect<&CollisionSystem::track>(*this);
    registry_.on_construct<Transform>().connect<&CollisionSystem::track>(*this);
    registry_.on_update<Collider>().connect<&CollisionSystem::resized>(*this);
    registry_.on_destroy<Collider>().connect<&CollisionSystem::untrack>(*this);
    registry_.on_destroy<Transform>().connect<&CollisionSystem::untrack>(*this);
    for (const auto entity : registry_.view<Transform, Collider>()) {
        track(registry_, entity);
    }
}

CollisionSystem::~CollisionSystem() {
    registry_.on_construct<Collider>().disconnect(this);
    registry_.on_construct<Transform>().disconnect(this);
    registry_.on_update<Collider>().disconnect(this);
    registry_.on_destroy<Collider>().disconnect(this);
    registry_.on_destroy<Transform>().disconnect(this);
}

void CollisionSystem::track(entt::registry& registry, entt::entity entity) {
    if (!registry.all_of<Transform, Collider>(entity) || tracked_.contains(entity)) {
        return;
    }
    insert(entity, bucketKey(registry.get<Transform>(entity).position));
}

void CollisionSystem::resized(entt::registry& registry, entt::entity entity) {
    const auto found = tracked_.find(entity);
    if (found == tracked_.end()) {
        return;
    }
    radii_.erase(radii_.find(found->second.radius));
    found->second.radius = reachOf(registry.get<Collider>(entity).radius);
    radii_.insert(found->second.radius);
}

void CollisionSystem::untrack(entt::registry&, entt::entity entity) {
    erase(entity);
}

void CollisionSystem::insert(entt::entity entity, std::int64_t key) {
    buckets_[key].push_back(entity);
    const float radius = reachOf(registry_.get<Collider>(entity).radius);
    tracked_[entity] = {key, radius};
    radii_.insert(radius);
}

void CollisionSystem::erase(entt::entity entity) {
    const auto found = tracked_.find(entity);
    if (found == tracked_.end()) {
        return;
    }
    if (const auto bucket = buckets_.find(found->second.key); bucket != buckets_.end()) {
        std::erase(bucket->second, entity);
        if (bucket->second.empty()) {
            buckets_.erase(bucket);
        }
    }
    radii_.erase(radii_.find(found->second.radius));
    tracked_.erase(found);
}

void CollisionSystem::moved(entt::entity entity) {
    const auto found = tracked_.find(entity);
    if (found == tracked_.end()) {
        return;
    }
    const auto key = bucketKey(registry_.get<Transform>(entity).position);
    if (key != found->second.key) {
        erase(entity);
        insert(entity, key);
    }
}

void CollisionSystem::refresh() {
    buckets_.clear();
    tracked_.clear();
    radii_.clear();
    for (const auto entity : registry_.view<Transform, Collider>()) {
        insert(entity, bucketKey(registry_.get<Transform>(entity).position));
    }
}

void CollisionSystem::setBoxes(std::vector<Box> boxes) {
    boxes_ = std::move(boxes);
    indexBoxes();
}

void CollisionSystem::indexBoxes() {
    // A box wider than this many buckets per side is tested by every query
    // instead of being listed in each bucket it covers.
    constexpr std::int64_t WideSpan = 64;
    boxBuckets_.clear();
    wideBoxes_.clear();
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        const auto& box = boxes_[i];
        const std::int64_t x0 = bucketCoord(box.min.x), x1 = bucketCoord(box.max.x);
        const std::int64_t z0 = bucketCoord(box.min.z), z1 = bucketCoord(box.max.z);
        if (x1 < x0 || z1 < z0 || (x1 - x0 + 1) * (z1 - z0 + 1) > WideSpan) {
            wideBoxes_.push_back(static_cast<int>(i)); // also non-finite bounds
            continue;
        }
        for (std::int64_t z = z0; z <= z1; ++z) {
            for (std::int64_t x = x0; x <= x1; ++x) {
                boxBuckets_[bucketKey(static_cast<int>(x), static_cast<int>(z))].push_back(static_cast<int>(i));
            }
        }
    }
}

template<class Visit> void CollisionSystem::forEachBoxNear(glm::vec3 p, float reach, Visit&& visit) const {
    for (const int index : wideBoxes_) {
        visit(boxes_[static_cast<std::size_t>(index)]);
    }
    if (boxBuckets_.empty()) {
        return;
    }
    const std::int64_t x0 = bucketCoord(p.x - reach), x1 = bucketCoord(p.x + reach);
    const std::int64_t z0 = bucketCoord(p.z - reach), z1 = bucketCoord(p.z + reach);
    if (x1 < x0 || z1 < z0) {
        return;
    }
    const std::int64_t span = (x1 - x0 + 1) * (z1 - z0 + 1);
    if (span > static_cast<std::int64_t>(boxBuckets_.size())) {
        for (const auto& [key, indices] : boxBuckets_) {
            const std::int64_t x = key >> 32;
            const std::int64_t z = static_cast<std::int32_t>(static_cast<std::uint32_t>(key));
            if (x >= x0 && x <= x1 && z >= z0 && z <= z1) {
                for (const int index : indices) {
                    visit(boxes_[static_cast<std::size_t>(index)]);
                }
            }
        }
        return;
    }
    for (std::int64_t z = z0; z <= z1; ++z) {
        for (std::int64_t x = x0; x <= x1; ++x) {
            const auto found = boxBuckets_.find(bucketKey(static_cast<int>(x), static_cast<int>(z)));
            if (found == boxBuckets_.end()) {
                continue;
            }
            for (const int index : found->second) {
                visit(boxes_[static_cast<std::size_t>(index)]);
            }
        }
    }
}

template<class Visit> void CollisionSystem::forEachMirror(glm::vec3 p, float reach, Visit&& visit) const {
    visit(p);
    if (wrap_.x <= 0) {
        return;
    }
    // One shift per axis whose seam the square crosses. A reach past half
    // the map needs no second shift: the plain square already covers it.
    const float dx = p.x - reach < 0 ? wrap_.x : p.x + reach > wrap_.x ? -wrap_.x : 0.0f;
    const float dz = p.z - reach < 0 ? wrap_.y : p.z + reach > wrap_.y ? -wrap_.y : 0.0f;
    if (dx != 0) {
        visit(p + glm::vec3(dx, 0, 0));
    }
    if (dz != 0) {
        visit(p + glm::vec3(0, 0, dz));
    }
    if (dx != 0 && dz != 0) {
        visit(p + glm::vec3(dx, 0, dz));
    }
}

glm::vec2 CollisionSystem::separation(glm::vec3 a, glm::vec3 b) const {
    const glm::vec2 d(b.x - a.x, b.z - a.z);
    if (wrap_.x <= 0) {
        return d;
    }
    return {wrapDifference(d.x, wrap_.x), wrapDifference(d.y, wrap_.y)};
}

template<class Visit> void CollisionSystem::forEachNear(glm::vec3 p, float reach, Visit&& visit) const {
    if (wrap_.x <= 0) {
        forEachNearAt(p, reach, visit);
        return;
    }
    // Mirrors can share buckets near a corner: collect, then visit each once.
    auto& found = mirrorScratch_;
    found.clear();
    forEachMirror(p, reach, [&](glm::vec3 q) {
        forEachNearAt(q, reach, [&](entt::entity other) { found.push_back(other); });
    });
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    for (const auto other : found) {
        visit(other);
    }
}

template<class Visit> void CollisionSystem::forEachNearAt(glm::vec3 p, float reach, Visit&& visit) const {
    const std::int64_t x0 = bucketCoord(p.x - reach), x1 = bucketCoord(p.x + reach);
    const std::int64_t z0 = bucketCoord(p.z - reach), z1 = bucketCoord(p.z + reach);
    if (x1 < x0 || z1 < z0) {
        return;
    }
    // Coordinates are saturated to +-2^30, so the product fits.
    const std::int64_t span = (x1 - x0 + 1) * (z1 - z0 + 1);
    if (span > static_cast<std::int64_t>(buckets_.size())) {
        // Fewer occupied buckets than buckets in reach: look at those.
        for (const auto& [key, bucket] : buckets_) {
            const std::int64_t x = key >> 32;
            const std::int64_t z = static_cast<std::int32_t>(static_cast<std::uint32_t>(key));
            if (x >= x0 && x <= x1 && z >= z0 && z <= z1) {
                for (const auto other : bucket) {
                    visit(other);
                }
            }
        }
        return;
    }
    for (std::int64_t z = z0; z <= z1; ++z) {
        for (std::int64_t x = x0; x <= x1; ++x) {
            const auto found = buckets_.find(bucketKey(static_cast<int>(x), static_cast<int>(z)));
            if (found == buckets_.end()) {
                continue;
            }
            for (const auto other : found->second) {
                visit(other);
            }
        }
    }
}

void CollisionSystem::nearby(glm::vec3 position, float reach, std::vector<entt::entity>& out) const {
    forEachNear(position, reach, [&](entt::entity entity) { out.push_back(entity); });
}

bool CollisionSystem::blockedByBox(glm::vec3 position, float radius, float height) const {
    bool blocked = false;
    forEachMirror(position, radius, [&](glm::vec3 p) {
        if (blocked) {
            return;
        }
        forEachBoxNear(p, radius, [&](const Box& box) {
            if (blocked || p.y + height < box.min.y || p.y > box.max.y) {
                return;
            }
            const glm::vec2 nearest = glm::clamp(glm::vec2(p.x, p.z), glm::vec2(box.min.x, box.min.z),
                                                 glm::vec2(box.max.x, box.max.z));
            const auto offset = nearest - glm::vec2(p.x, p.z);
            blocked = glm::dot(offset, offset) < radius * radius;
        });
    });
    return blocked;
}

bool CollisionSystem::blockedByEntity(entt::entity self, glm::vec3 p, float radius, float height,
                                      std::span<const entt::entity> ignore) const {
    return blocksMove(self, p, p, radius, height, ignore);
}

bool CollisionSystem::blocksMove(entt::entity self, glm::vec3 from, glm::vec3 to, float radius, float height,
                                 std::span<const entt::entity> ignore) const {
    bool blocked = false;
    forEachNear(to, radius + maxRadius(), [&](entt::entity other) {
        if (blocked || other == self || std::find(ignore.begin(), ignore.end(), other) != ignore.end()) {
            return;
        }
        const auto& collider = registry_.get<Collider>(other);
        const auto q = registry_.get<Transform>(other).position;
        if (!collider.solid || !overlapVertically(to.y, height, q.y, collider.height)) {
            return;
        }
        const float reach = radius + collider.radius;
        const float after = glm::length(separation(to, q));
        if (after >= reach) {
            return;
        }
        // Overlapping at the destination: allowed only as a way out of an
        // overlap that already exists.
        const float before = glm::length(separation(from, q));
        blocked = !(before < reach && after > before + 1e-6f);
    });
    return blocked;
}

void CollisionSystem::updateContacts(EventQueue& events) {
    constexpr float Margin = 0.05f;
    current_.clear();
    for (const auto a : registry_.view<Transform, Collider>()) {
        const auto pa = registry_.get<Transform>(a).position;
        const auto& ca = registry_.get<Collider>(a);
        forEachNear(pa, ca.radius + maxRadius() + Margin, [&](entt::entity b) {
            if (!(a < b)) {
                return;
            }
            const auto pb = registry_.get<Transform>(b).position;
            const auto& cb = registry_.get<Collider>(b);
            const float reach = ca.radius + cb.radius + Margin;
            const glm::vec2 d = separation(pa, pb);
            if (overlapVertically(pa.y, ca.height + Margin, pb.y, cb.height + Margin) &&
                glm::dot(d, d) <= reach * reach) {
                current_.emplace_back(a, b);
            }
        });
    }
    std::sort(current_.begin(), current_.end());
    for (const auto& pair : current_) {
        if (!std::binary_search(contacts_.begin(), contacts_.end(), pair)) {
            events.push({WorldEvent::Kind::ContactBegin, pair.first, pair.second, {}});
        }
    }
    for (const auto& pair : contacts_) {
        if (!std::binary_search(current_.begin(), current_.end(), pair) && registry_.valid(pair.first) &&
            registry_.valid(pair.second)) {
            events.push({WorldEvent::Kind::ContactEnd, pair.first, pair.second, {}});
        }
    }
    std::swap(contacts_, current_);
}

std::optional<CollisionSystem::Hit>
CollisionSystem::raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance,
                         const std::function<bool(entt::entity)>& accept) const {
    std::optional<Hit> best;
    const auto consider = [&](std::optional<float> t, entt::entity entity, int box) {
        if (t && (!best || *t < best->distance)) {
            best = Hit{*t, entity, box};
        }
    };
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        consider(rayBox(origin, direction, boxes_[i], maxDistance), entt::null, static_cast<int>(i));
    }
    for (const auto [entity, transform, collider] : registry_.view<Transform, Collider>().each()) {
        if (accept(entity)) {
            consider(rayCylinder(origin, direction, transform.position, collider.radius, collider.height,
                                 best ? best->distance : maxDistance),
                     entity, -1);
        }
    }
    return best;
}

void updateZones(entt::registry& registry, const std::vector<Zone>& zones, EventQueue& events) {
    for (const auto entity : registry.view<Transform>()) {
        if (!registry.any_of<Mover, Collider>(entity)) {
            continue;
        }
        const auto& transform = registry.get<Transform>(entity);
        auto* presence = registry.try_get<ZonePresence>(entity);
        for (const auto& zone : zones) {
            const bool now = zone.contains(transform.position, transform.layer);
            const bool before = presence && std::find(presence->inside.begin(), presence->inside.end(),
                                                      zone.id) != presence->inside.end();
            if (now && !before) {
                if (!presence) {
                    presence = &registry.emplace<ZonePresence>(entity);
                }
                presence->inside.push_back(zone.id);
                events.push({WorldEvent::Kind::ZoneEnter, entity, entt::null, zone.id});
            } else if (!now && before) {
                std::erase(presence->inside, zone.id);
                events.push({WorldEvent::Kind::ZoneExit, entity, entt::null, zone.id});
            }
        }
        if (!presence) {
            continue;
        }
        // Zones removed from the map at run time count as left.
        std::erase_if(presence->inside, [&](const std::string& id) {
            const bool exists =
                std::any_of(zones.begin(), zones.end(), [&](const Zone& z) { return z.id == id; });
            if (!exists) {
                events.push({WorldEvent::Kind::ZoneExit, entity, entt::null, id});
            }
            return !exists;
        });
    }
}

std::unordered_set<std::int64_t> boxBlockedCells(const Grid& grid, const std::vector<Box>& boxes, float radius,
                                                 float stepUp, float bodyHeight) {
    std::unordered_set<std::int64_t> blocked;
    for (const auto& box : boxes) {
        const auto lo = glm::min(box.min, box.max), hi = glm::max(box.min, box.max);
        // Cells whose centre disc can reach the box horizontally.
        const int x0 = std::max(0, floorCell(lo.x - radius - 0.5f)), x1 = std::min(grid.width() - 1, floorCell(hi.x + radius + 0.5f));
        const int z0 = std::max(0, floorCell(lo.z - radius - 0.5f)), z1 = std::min(grid.depth() - 1, floorCell(hi.z + radius + 0.5f));
        for (int layer = 0; layer < grid.layerCount(); ++layer) {
            for (int z = z0; z <= z1; ++z) {
                for (int x = x0; x <= x1; ++x) {
                    const glm::vec2 centre(static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f);
                    // Nearest point of the box's footprint to the centre.
                    const glm::vec2 nearest(std::clamp(centre.x, lo.x, hi.x), std::clamp(centre.y, lo.z, hi.z));
                    if (glm::length(nearest - centre) >= radius) {
                        continue;
                    }
                    const auto& cell = grid.at(x, z, layer);
                    if (!cell.walkable) {
                        continue; // already unwalkable: nothing to add
                    }
                    const float surface = grid.heightAt(centre, layer);
                    if (lo.y < surface + bodyHeight && hi.y > surface + stepUp + 1e-3f) {
                        blocked.insert(cellKey({x, z, layer}));
                    }
                }
            }
        }
    }
    return blocked;
}
} // namespace fe
