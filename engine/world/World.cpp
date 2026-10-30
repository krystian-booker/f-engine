#include "engine/world/World.h"
#include "engine/core/Log.h"
#include "engine/nav/Query.h"
#include "engine/world/Animation.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace fe {
namespace {
std::vector<Box> staticBoxes(const MapData& map, AssetCache& assets, const std::string& geometry) {
    auto boxes = map.colliders;
    if (const auto model = assets.tryModel(geometry)) {
        for (const auto& marker : model->markers) {
            if (marker.hasBounds && marker.properties.value("collider", false)) {
                boxes.push_back({marker.min, marker.max});
            }
        }
    }
    return boxes;
}

// Marks the emitter entities World makes from the map.
struct MapEmitterEntity {};
} // namespace

void EventQueue::push(WorldEvent event) {
    if (events_.size() >= Capacity) {
        if (dropped_++ == 0) {
            log(LogLevel::Warning, "World events are not being read (world:events()); dropping the oldest");
        }
        events_.pop_front();
    }
    events_.push_back(std::move(event));
}

std::vector<WorldEvent> EventQueue::take() {
    std::vector<WorldEvent> taken(std::make_move_iterator(events_.begin()),
                                  std::make_move_iterator(events_.end()));
    events_.clear();
    dropped_ = 0;
    return taken;
}

World::World(std::string name, MapData map, AssetCache& assets, std::uint64_t seed)
    : name_(std::move(name)), map_(std::move(map)), authored_(map_), assets_(assets), random_(seed),
      collision_(registry_, staticBoxes(map_, assets_, geometryPath())),
      movement_(registry_, map_.grid, collision_, events_), particles_(registry_, assets_, random_) {
    rebuildStatic();
    movement_.setBlockedCells(&boxBlockedCells());
    // The camera starts on the `start` point, else the first point, else the middle.
    const auto start = map_.points.find("start");
    camera_.target = start != map_.points.end() ? start->second.position
                     : map_.points.empty()      ? glm::vec3(map_.grid.width() * 0.5f, 0, map_.grid.depth() * 0.5f)
                                                : map_.points.begin()->second.position;
    camera_.snap();
}

const std::unordered_set<std::int64_t>& World::boxBlockedCells() const {
    if (boxBlockedGrid_ != map_.grid.revision() || boxBlockedBoxes_ != boxesRevision_) {
        boxBlockedGrid_ = map_.grid.revision();
        boxBlockedBoxes_ = boxesRevision_;
        boxBlocked_ = fe::boxBlockedCells(map_.grid, collision_.boxes());
    }
    return boxBlocked_;
}

void World::rebuildStatic() {
    collision_.setBoxes(staticBoxes(map_, assets_, geometryPath()));
    ++boxesRevision_;
    collision_.setWrap(map_.grid.wraps() ? glm::vec2(static_cast<float>(map_.grid.width()),
                                                     static_cast<float>(map_.grid.depth()))
                                         : glm::vec2(0));
    collision_.refresh();
    Json emitters = Json::array();
    for (const auto& emitter : map_.emitters) {
        emitters.push_back({toJson(emitter.position), emitter.settings.toJson()});
    }
    if (emitters == builtEmitters_) {
        return; // running emitters keep their state
    }
    builtEmitters_ = std::move(emitters);
    const auto old = registry_.view<MapEmitterEntity>();
    registry_.destroy(old.begin(), old.end());
    for (const auto& emitter : map_.emitters) {
        const auto e = create();
        registry_.emplace<Transform>(e, Transform{emitter.position, emitter.position});
        registry_.emplace<Emitter>(e, Emitter{emitter.settings});
        registry_.emplace<MapEmitterEntity>(e);
    }
}

std::optional<WorldHit> World::raycast(glm::vec3 origin, glm::vec3 direction,
                                       const RayOptions& options) const {
    if (glm::length(direction) < 1e-6f || !std::isfinite(options.maxDistance) || options.maxDistance <= 0) {
        return std::nullopt;
    }
    direction = glm::normalize(direction);
    std::optional<WorldHit> best;
    if (const auto ground = raycastGrid(map_.grid, origin, direction, options.maxDistance, options.layers)) {
        best =
            WorldHit{WorldHit::Kind::Grid, glm::length(ground->point - origin), ground->point, ground->cell};
    }
    const auto accept = [&](entt::entity entity) {
        if (!options.entities) {
            return false;
        }
        if (const auto* collider = registry_.try_get<Collider>(entity);
            options.solidOnly && collider && !collider->solid) {
            return false;
        }
        if (options.tags.empty()) {
            return true;
        }
        const auto* tags = registry_.try_get<Tags>(entity);
        return tags && std::any_of(options.tags.begin(), options.tags.end(),
                                   [&](const std::string& tag) { return tags->has(tag); });
    };
    const float reach = best ? best->distance : options.maxDistance;
    if (const auto hit = collision_.raycast(origin, direction, reach, accept)) {
        best = WorldHit{.kind = hit->entity == entt::null ? WorldHit::Kind::Collider : WorldHit::Kind::Entity,
                        .distance = hit->distance,
                        .point = origin + direction * hit->distance,
                        .cell = {},
                        .collider = hit->box,
                        .entity = hit->entity};
    }
    if (options.entities) {
        for (const auto [entity, transform, sprite] : registry_.view<Transform, Sprite>().each()) {
            if (!sprite.visible || registry_.all_of<Collider>(entity) || !accept(entity)) {
                continue;
            }
            const auto animation = assets_.trySprite(sprite.animation);
            if (!animation) {
                continue;
            }
            const auto quad = spriteQuad(sprite, *animation, interpolated(entity, options.alpha), transform.facing,
                                         options.camera ? *options.camera : camera_, options.alpha,
                                         options.pixelsPerUnit);
            const auto t = rayQuad(origin, direction, quad);
            if (t && *t <= (best ? best->distance : options.maxDistance)) {
                best = WorldHit{.kind = WorldHit::Kind::Entity,
                                .distance = *t,
                                .point = origin + direction * *t,
                                .cell = {},
                                .collider = -1,
                                .entity = entity};
            }
        }
    }
    return best;
}

entt::entity World::pickSprite(glm::vec3 origin, glm::vec3 direction, double alpha, int pixelsPerUnit,
                               const Camera* camera) const {
    if (glm::length(direction) < 1e-6f) {
        return entt::null;
    }
    direction = glm::normalize(direction);
    entt::entity picked = entt::null;
    float nearest = std::numeric_limits<float>::infinity();
    for (const auto [entity, transform, sprite] : registry_.view<Transform, Sprite>().each()) {
        if (!sprite.visible || sprite.animation.empty()) {
            continue;
        }
        const auto animation = assets_.trySprite(sprite.animation);
        if (!animation) {
            continue;
        }
        const auto quad = spriteQuad(sprite, *animation, interpolated(entity, alpha), transform.facing,
                                     camera ? *camera : camera_, alpha, pixelsPerUnit);
        if (const auto t = rayQuad(origin, direction, quad); t && *t < nearest) {
            nearest = *t;
            picked = entity;
        }
    }
    return picked;
}

World::~World() {
    if (destroyHook_) {
        for (const auto entity : registry_.view<ScriptRef>()) {
            destroyHook_(entity);
        }
    }
}

entt::entity World::create() {
    const auto entity = registry_.create();
    registry_.emplace<EntitySerial>(entity, EntitySerial{nextSerial_++});
    return entity;
}

entt::entity World::spawn(const std::string& id, glm::vec3 position, int layer) {
    if (!id.empty() && names_.contains(id)) {
        throw std::runtime_error("Entity id already in use: " + id);
    }
    const auto entity = create();
    Transform transform;
    // Canonical on a wrapping map: one representative per position.
    const auto flat = map_.grid.wrapPoint({position.x, position.z});
    transform.position = transform.previous = {flat.x, position.y, flat.y};
    transform.layer = std::clamp(layer, 0, map_.grid.layerCount() - 1);
    registry_.emplace<Transform>(entity, transform);
    if (!id.empty()) {
        registry_.emplace<Name>(entity, Name{id});
        names_[id] = entity;
    }
    return entity;
}

void World::destroy(entt::entity entity) {
    if (!registry_.valid(entity)) {
        return;
    }
    if (const auto* name = registry_.try_get<Name>(entity)) {
        names_.erase(name->id);
    }
    if (destroyHook_ && registry_.all_of<ScriptRef>(entity)) {
        destroyHook_(entity);
    }
    // Children notice the stale parent handle on the next tick and detach.
    registry_.destroy(entity);
}

std::vector<WorldEvent> World::takeEvents() {
    auto events = events_.take();
    const auto gone = [&](entt::entity e) {
        return e != entt::null && !registry_.valid(e);
    };
    std::erase_if(events, [&](const WorldEvent& event) { return gone(event.a) || gone(event.b); });
    return events;
}

entt::entity World::find(const std::string& id) const {
    const auto found = names_.find(id);
    return found == names_.end() ? entt::null : found->second;
}

void World::rename(entt::entity entity, const std::string& id) {
    if (!registry_.valid(entity)) {
        return;
    }
    if (const auto* name = registry_.try_get<Name>(entity)) {
        names_.erase(name->id);
    }
    if (id.empty()) {
        registry_.remove<Name>(entity);
        return;
    }
    if (names_.contains(id)) {
        throw std::runtime_error("Entity id already in use: " + id);
    }
    registry_.emplace_or_replace<Name>(entity, Name{id});
    names_[id] = entity;
}

std::vector<entt::entity> World::query(const std::string& tag, std::optional<glm::vec3> centre,
                                       float radius) const {
    std::vector<entt::entity> result;
    const auto near = [&](const Transform& transform) {
        const auto& p = transform.position;
        return !centre || glm::length(map_.grid.delta({centre->x, centre->z}, {p.x, p.z})) <= radius;
    };
    if (tag.empty()) {
        for (const auto [entity, transform] : registry_.view<Transform>().each()) {
            if (near(transform)) {
                result.push_back(entity);
            }
        }
        return result;
    }
    for (const auto [entity, tags, transform] : registry_.view<Tags, Transform>().each()) {
        if (tags.has(tag) && near(transform)) {
            result.push_back(entity);
        }
    }
    return result;
}

void World::setClip(entt::entity entity, const std::string& clip, bool restart) {
    auto& sprite = registry_.get<Sprite>(entity);
    if (sprite.clip == clip && !restart) {
        return;
    }
    restartClip(sprite, clip);
}

void World::playMesh(entt::entity entity, const std::string& animation, bool loop) {
    auto& mesh = registry_.get<MeshInstance>(entity);
    mesh.animation = animation;
    mesh.loop = loop;
    mesh.time = 0;
    mesh.playing = true;
}

void World::beginTick() {
    movement_.setBlockedCells(&boxBlockedCells()); // recomputed when the grid or boxes changed
    if (map_.grid.wraps()) {
        // Scripts may steer the target past a seam; it stays canonical like
        // every position, and the frame already drawn interpolated across it.
        const auto flat = map_.grid.wrapPoint({camera_.target.x, camera_.target.z});
        camera_.target.x = flat.x;
        camera_.target.z = flat.y;
    }
    camera_.beginTick();
    for (const auto [entity, transform] : registry_.view<Transform>().each()) {
        transform.previous = transform.position;
        transform.previousYaw = transform.yaw;
    }
    for (const auto [entity, sprite] : registry_.view<Sprite>().each()) {
        sprite.previousTime = sprite.time;
    }
}

void World::tick(double dt) {
    if (!std::isfinite(dt) || dt <= 0) {
        return;
    }
    time_ += dt;
    movement_.update(dt);
    movement_.carryRiders();
    movement_.updateAttachments();
    movement_.reportCells();
    advanceAnimations(registry_, assets_, camera_.yaw, dt, events_);
    particles_.update(dt, map_.environment.weather, camera_.target);
    collision_.updateContacts(events_);
    updateZones(registry_, map_.zones, events_);
}

float flickerSeed(const LightSettings& mapLight) {
    // FNV-1a over the position's bits (+0.0f folds -0 into 0), mapped to a
    // phase in [0, 100): non-negative, so never an entity light's seed.
    std::uint32_t hash = 2166136261u;
    for (int i = 0; i < 3; ++i) {
        const auto bits = std::bit_cast<std::uint32_t>(mapLight.position[i] + 0.0f);
        for (int byte = 0; byte < 4; ++byte) {
            hash = (hash ^ ((bits >> (8 * byte)) & 0xffu)) * 16777619u;
        }
    }
    return static_cast<float>(hash % 100000u) * 0.001f;
}

float flickerSeed(entt::entity light) {
    return -1.3f - static_cast<float>(entt::to_entity(light)) * 3.7f;
}

std::vector<LightSettings> World::activeLights() const {
    std::vector<LightSettings> lights = map_.lights;
    for (auto& light : lights) {
        light.intensity *= light.flickerScale(time_, flickerSeed(light));
    }
    for (const auto [entity, transform, source] : registry_.view<Transform, LightSource>().each()) {
        if (!source.enabled) {
            continue;
        }
        auto light = source.light;
        light.position += transform.position;
        light.intensity *= light.flickerScale(time_, flickerSeed(entity));
        lights.push_back(light);
    }
    return lights;
}

glm::vec3 World::interpolated(entt::entity entity, double alpha) const {
    const auto& transform = registry_.get<Transform>(entity);
    return glm::mix(transform.previous, transform.position, static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
}
} // namespace fe
