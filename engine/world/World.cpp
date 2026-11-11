#include "engine/world/World.h"
#include "engine/core/Log.h"
#include "engine/nav/Query.h"
#include <algorithm>
#include <cmath>

namespace fe {
namespace {
constexpr float HashCell = 2.0f;
constexpr std::size_t MaxParticles = 16384;

std::int64_t hashKey(int x, int z) {
    return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(z));
}

float noise(double t, float seed) {
    return static_cast<float>(std::sin(t * 1.7 + seed) * 0.5 + std::sin(t * 4.3 + seed * 2.1) * 0.3 +
                              std::sin(t * 9.1 + seed * 0.7) * 0.2);
}
} // namespace

World::World(std::string name, MapData map, AssetCache& assets, std::uint64_t seed)
    : name_(std::move(name)), map_(std::move(map)), assets_(assets), random_(seed) {
    colliders_ = map_.colliders;
    if (const auto model = assets_.tryModel(geometryPath())) {
        for (const auto& marker : model->markers) {
            if (marker.hasBounds && marker.properties.value("collider", false)) {
                colliders_.push_back({marker.min, marker.max});
            }
        }
    }
    for (const auto& emitter : map_.emitters) {
        const auto e = registry_.create();
        registry_.emplace<Transform>(e, Transform{emitter.position, emitter.position});
        registry_.emplace<Emitter>(e, Emitter{emitter.settings});
    }
    camera_.target = map_.points.empty() ? glm::vec3(map_.grid.width() * 0.5f, 0, map_.grid.depth() * 0.5f)
                                         : map_.points.begin()->second.position;
    camera_.snap();
}

World::~World() {
    if (destroyHook_) {
        for (const auto entity : registry_.view<ScriptRef>()) {
            destroyHook_(entity);
        }
    }
}

entt::entity World::spawn(const std::string& id, glm::vec3 position, int layer) {
    if (!id.empty() && names_.contains(id)) {
        throw std::runtime_error("Entity id already in use: " + id);
    }
    const auto entity = registry_.create();
    Transform transform;
    transform.position = transform.previous = position;
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
    std::erase_if(contacts_, [&](const auto& pair) { return pair.first == entity || pair.second == entity; });
    zonesInside_.erase(entity);
    for (const auto child : registry_.view<Transform>()) {
        auto& transform = registry_.get<Transform>(child);
        if (transform.parent == entity) {
            transform.parent = entt::null;
        }
    }
    registry_.destroy(entity);
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
    for (const auto entity : registry_.view<Transform>()) {
        if (!tag.empty()) {
            const auto* tags = registry_.try_get<Tags>(entity);
            if (!tags || !tags->has(tag)) {
                continue;
            }
        }
        if (centre) {
            const auto p = registry_.get<Transform>(entity).position;
            if (glm::length(glm::vec2(p.x - centre->x, p.z - centre->z)) > radius) {
                continue;
            }
        }
        result.push_back(entity);
    }
    return result;
}

void World::moveTo(entt::entity entity, glm::vec3 target, bool findPath) {
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    const auto& transform = registry_.get<Transform>(entity);
    mover.waypoints.clear();
    mover.stuckTime = 0;
    mover.jumping = false;
    if (findPath) {
        StepRules rules;
        rules.maxUp = rules.maxDown = mover.maxStep;
        rules.diagonal = true;
        const GridPos from{static_cast<int>(std::floor(transform.position.x)),
                           static_cast<int>(std::floor(transform.position.z)), transform.layer};
        GridPos to{static_cast<int>(std::floor(target.x)), static_cast<int>(std::floor(target.z)),
                   transform.layer};
        if (!map_.grid.contains(to)) {
            throw std::runtime_error("Movement target is outside the map grid");
        }
        if (!map_.grid.at(to).walkable) {
            for (int l = 0; l < map_.grid.layerCount(); ++l) {
                if (map_.grid.at(to.x, to.z, l).walkable) {
                    to.layer = l;
                    break;
                }
            }
        }
        const auto path = fe::findPath(map_.grid, from, to, rules);
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            const auto& cell = path[i];
            const glm::vec2 centre(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f);
            mover.waypoints.emplace_back(centre.x, map_.grid.heightAt(centre, cell.layer), centre.y);
        }
    }
    mover.waypoints.push_back(target);
    mover.active = true;
}

void World::stop(entt::entity entity) {
    if (auto* mover = registry_.try_get<Mover>(entity)) {
        mover->active = false;
        mover->waypoints.clear();
        mover->drive = glm::vec2(0);
    }
}

void World::jump(entt::entity entity, glm::vec3 target, float height, float duration, int layer) {
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    const auto& transform = registry_.get<Transform>(entity);
    mover.active = false;
    mover.waypoints.clear();
    mover.jumping = true;
    mover.jumpFrom = transform.position;
    mover.jumpTo = target;
    mover.jumpHeight = height;
    mover.jumpDuration = std::max(0.05f, duration);
    mover.jumpTime = 0;
    mover.jumpLayer = std::clamp(layer, 0, map_.grid.layerCount() - 1);
}

void World::place(entt::entity entity, glm::vec3 position, int layer, bool keepHeight) {
    auto& transform = registry_.get<Transform>(entity);
    transform.layer = std::clamp(layer, 0, map_.grid.layerCount() - 1);
    if (!keepHeight) {
        position.y = map_.grid.heightAt({position.x, position.z}, transform.layer);
    }
    transform.position = transform.previous = position;
    stop(entity);
}

bool World::moving(entt::entity entity) const {
    const auto* mover = registry_.try_get<Mover>(entity);
    return mover && (mover->active || mover->jumping || glm::length(mover->drive) > 0);
}

bool World::canOccupy(entt::entity entity, glm::vec3 position, int layer) const {
    const auto* mover = registry_.try_get<Mover>(entity);
    const float radius = mover ? mover->radius : 0.2f;
    return map_.grid.canStand({position.x, position.z}, radius, layer) &&
           !blockedByCollider(position, radius, 1.6f) && !blockedByEntity(entity, position, radius);
}

void World::setClip(entt::entity entity, const std::string& clip, bool restart) {
    auto& sprite = registry_.get<Sprite>(entity);
    if (sprite.clip == clip && !restart) {
        return;
    }
    sprite.clip = clip;
    sprite.time = sprite.previousTime = 0;
    sprite.finished = false;
}

void World::playMesh(entt::entity entity, const std::string& animation, bool loop) {
    auto& mesh = registry_.get<MeshInstance>(entity);
    mesh.animation = animation;
    mesh.loop = loop;
    mesh.time = 0;
    mesh.playing = true;
}

void World::beginTick() {
    camera_.beginTick();
    for (const auto entity : registry_.view<Transform>()) {
        auto& transform = registry_.get<Transform>(entity);
        transform.previous = transform.position;
        transform.previousYaw = transform.yaw;
    }
    for (const auto entity : registry_.view<Sprite>()) {
        auto& sprite = registry_.get<Sprite>(entity);
        sprite.previousTime = sprite.time;
    }
}

void World::tick(double dt) {
    if (!std::isfinite(dt) || dt <= 0) {
        return;
    }
    time_ += dt;
    rebuildHash();
    updateMovers(dt);
    updateAttachments();
    advanceAnimations(dt);
    updateEmitters(dt);
    updateWeather(dt);
    updateParticles(dt);
    rebuildHash();
    updateContacts();
    updateZones();
}

std::vector<WorldEvent> World::takeEvents() {
    auto events = std::move(events_);
    events_.clear();
    return events;
}

void World::advanceAnimations(double dt) {
    for (const auto entity : registry_.view<Sprite>()) {
        auto& sprite = registry_.get<Sprite>(entity);
        const auto* mover = registry_.try_get<Mover>(entity);
        if (!sprite.moveClip.empty() && mover) {
            const auto* transform = registry_.try_get<Transform>(entity);
            const bool moved = transform && glm::length(transform->position - transform->previous) > 1e-4f;
            const auto& wanted =
                moved ? sprite.moveClip : (sprite.idleClip.empty() ? "idle" : sprite.idleClip);
            if (sprite.clip != wanted &&
                (sprite.clip == sprite.moveClip || sprite.clip == sprite.idleClip || sprite.clip == "idle")) {
                setClip(entity, wanted, true);
            }
        }
        sprite.time += dt * sprite.speed;
        const auto animation = assets_.trySprite(sprite.animation);
        if (!animation) {
            continue;
        }
        const auto* transform = registry_.try_get<Transform>(entity);
        const int direction =
            sprite.direction >= 0
                ? sprite.direction
                : animation->direction(sprite.clip, transform ? transform->facing : glm::vec2(0, 1),
                                       camera_.yaw);
        for (const auto& name : animation->events(sprite.clip, direction, sprite.previousTime, sprite.time)) {
            events_.push_back({WorldEvent::Kind::AnimationEvent, entity, entt::null, name});
        }
        if (!sprite.finished) {
            const auto found = animation->clips.find(sprite.clip);
            if (found != animation->clips.end() && !found->second.loop) {
                const auto& frames = found->second.row(direction);
                double length = 0;
                for (const auto& frame : frames) {
                    length += frame.duration;
                }
                if (sprite.time >= length) {
                    sprite.finished = true;
                    events_.push_back({WorldEvent::Kind::AnimationFinished, entity, entt::null, sprite.clip});
                }
            }
        }
    }
    for (const auto entity : registry_.view<MeshInstance>()) {
        auto& mesh = registry_.get<MeshInstance>(entity);
        if (!mesh.playing) {
            continue;
        }
        mesh.time += static_cast<float>(dt) * mesh.speed;
        const auto model = assets_.tryModel(mesh.model);
        const int index = model ? model->animation(mesh.animation) : -1;
        if (index < 0) {
            continue;
        }
        const float duration = model->animations[static_cast<std::size_t>(index)].duration;
        if (duration <= 0) {
            continue;
        }
        if (mesh.loop) {
            mesh.time = std::fmod(mesh.time, duration);
        } else if (mesh.time >= duration) {
            mesh.time = duration;
            mesh.playing = false;
            events_.push_back({WorldEvent::Kind::AnimationFinished, entity, entt::null, mesh.animation});
        }
    }
}

void World::rebuildHash() {
    hash_.clear();
    for (const auto entity : registry_.view<Transform, Collider>()) {
        const auto& p = registry_.get<Transform>(entity).position;
        hash_[hashKey(static_cast<int>(std::floor(p.x / HashCell)),
                      static_cast<int>(std::floor(p.z / HashCell)))]
            .push_back(entity);
    }
}

bool World::blockedByCollider(glm::vec3 p, float radius, float height) const {
    for (const auto& box : colliders_) {
        if (p.y + height < box.min.y || p.y > box.max.y) {
            continue;
        }
        const glm::vec2 nearest =
            glm::clamp(glm::vec2(p.x, p.z), glm::vec2(box.min.x, box.min.z), glm::vec2(box.max.x, box.max.z));
        const auto offset = nearest - glm::vec2(p.x, p.z);
        if (glm::dot(offset, offset) < radius * radius) {
            return true;
        }
    }
    return false;
}

bool World::blockedByEntity(entt::entity self, glm::vec3 p, float radius) const {
    const int cx = static_cast<int>(std::floor(p.x / HashCell));
    const int cz = static_cast<int>(std::floor(p.z / HashCell));
    for (int z = cz - 1; z <= cz + 1; ++z) {
        for (int x = cx - 1; x <= cx + 1; ++x) {
            const auto found = hash_.find(hashKey(x, z));
            if (found == hash_.end()) {
                continue;
            }
            for (const auto other : found->second) {
                if (other == self || !registry_.valid(other)) {
                    continue;
                }
                const auto& collider = registry_.get<Collider>(other);
                if (!collider.solid) {
                    continue;
                }
                const auto q = registry_.get<Transform>(other).position;
                if (std::abs(q.y - p.y) > 0.8f) {
                    continue;
                }
                const float reach = radius + collider.radius;
                const glm::vec2 d(q.x - p.x, q.z - p.z);
                if (glm::dot(d, d) < reach * reach) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool World::stepTowards(entt::entity entity, glm::vec2 delta) {
    auto& transform = registry_.get<Transform>(entity);
    const auto& mover = registry_.get<Mover>(entity);
    if (glm::length(delta) > 1e-5f) {
        transform.facing = glm::normalize(delta);
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(delta) / 0.1f)));
    const auto increment = delta / static_cast<float>(steps);
    const auto& grid = map_.grid;
    auto valid = [&](glm::vec2 from, glm::vec2 to) {
        if (!mover.collide) {
            return true;
        }
        const float here = grid.heightAt(from, transform.layer);
        const float there = grid.heightAt(to, transform.layer);
        const glm::vec3 probe(to.x, there, to.y);
        return grid.canStand(to, mover.radius, transform.layer) && std::abs(there - here) <= mover.maxStep &&
               !blockedByCollider(probe, mover.radius, 1.6f) && !blockedByEntity(entity, probe, mover.radius);
    };
    glm::vec2 p(transform.position.x, transform.position.z);
    const glm::vec2 start = p;
    for (int i = 0; i < steps; ++i) {
        if (valid(p, p + increment)) {
            p += increment;
            continue;
        }
        if (valid(p, p + glm::vec2(increment.x, 0))) {
            p.x += increment.x;
        } else if (valid(p, p + glm::vec2(0, increment.y))) {
            p.y += increment.y;
        }
    }
    if (mover.collide) {
        updateLayer(transform, mover, p, p - start);
        transform.position = {p.x, grid.heightAt(p, transform.layer), p.y};
    } else {
        transform.position.x = p.x;
        transform.position.z = p.y;
    }
    return glm::length(p - start) > 1e-5f;
}

void World::updateLayer(Transform& transform, const Mover& mover, glm::vec2 p, glm::vec2 moved) const {
    const auto& grid = map_.grid;
    const int x = static_cast<int>(std::floor(p.x));
    const int z = static_cast<int>(std::floor(p.y));
    const auto* link = grid.linkAt(x, z, transform.layer);
    if (!link) {
        return;
    }
    const int other = link->from == transform.layer ? link->to : link->from;
    bool wanted = false;
    if (link->direction != glm::ivec2(0)) {
        const float along = glm::dot(moved, glm::vec2(link->direction));
        wanted = other == link->to ? along > 1e-5f : along < -1e-5f;
    } else {
        wanted = !grid.canStand(p, mover.radius, transform.layer);
    }
    const float gap = std::abs(grid.heightAt(p, other) - grid.heightAt(p, transform.layer));
    if (wanted && gap <= std::min(0.25f, mover.maxStep) && grid.canStand(p, mover.radius, other)) {
        transform.layer = other;
    }
}

void World::updateMovers(double dt) {
    const auto delta = static_cast<float>(dt);
    for (const auto entity : registry_.view<Transform, Mover>()) {
        auto& mover = registry_.get<Mover>(entity);
        auto& transform = registry_.get<Transform>(entity);
        if (mover.jumping) {
            mover.jumpTime += delta;
            const float t = std::min(1.0f, mover.jumpTime / mover.jumpDuration);
            transform.position = glm::mix(mover.jumpFrom, mover.jumpTo, t) +
                                 glm::vec3(0, 4 * mover.jumpHeight * t * (1 - t), 0);
            const glm::vec2 heading(mover.jumpTo.x - mover.jumpFrom.x, mover.jumpTo.z - mover.jumpFrom.z);
            if (glm::length(heading) > 1e-4f) {
                transform.facing = glm::normalize(heading);
            }
            if (t >= 1) {
                mover.jumping = false;
                transform.layer = mover.jumpLayer;
                events_.push_back({WorldEvent::Kind::Landed, entity, entt::null, {}});
            }
            continue;
        }
        if (glm::length(mover.drive) > 0) {
            stepTowards(entity, mover.drive * delta);
            mover.drive = glm::vec2(0);
            continue;
        }
        if (!mover.active || mover.waypoints.empty()) {
            continue;
        }
        const auto target = mover.waypoints.front();
        const glm::vec2 to(target.x - transform.position.x, target.z - transform.position.z);
        const float distance = glm::length(to);
        const float step = mover.speed * delta;
        const auto before = transform.position;
        if (distance <= std::max(step, 0.02f)) {
            stepTowards(entity, to);
            if (!mover.collide) {
                transform.position.y = target.y;
            }
            if (glm::length(glm::vec2(transform.position.x - target.x, transform.position.z - target.z)) <
                0.05f) {
                mover.waypoints.erase(mover.waypoints.begin());
            }
        } else {
            stepTowards(entity, to / distance * step);
            if (!mover.collide) {
                transform.position.y = glm::mix(transform.position.y, target.y, step / distance);
            }
        }
        const float moved = glm::length(transform.position - before);
        mover.stuckTime = moved < step * 0.1f ? mover.stuckTime + dt : 0;
        if (mover.waypoints.empty()) {
            mover.active = false;
            events_.push_back({WorldEvent::Kind::Arrived, entity, entt::null, {}});
        } else if (mover.stuckTime > 1.0) {
            mover.active = false;
            mover.waypoints.clear();
            events_.push_back({WorldEvent::Kind::Blocked, entity, entt::null, {}});
        }
    }
}

void World::updateAttachments() {
    for (const auto entity : registry_.view<Transform>()) {
        auto& transform = registry_.get<Transform>(entity);
        if (transform.parent == entt::null || !registry_.valid(transform.parent)) {
            continue;
        }
        const auto& parent = registry_.get<Transform>(transform.parent);
        transform.position = parent.position + transform.offset;
        transform.previous = parent.previous + transform.offset;
        transform.layer = parent.layer;
    }
}

std::uint16_t World::particleSprite(const std::string& path) {
    const auto found = std::find(particleSprites_.begin(), particleSprites_.end(), path);
    if (found != particleSprites_.end()) {
        return static_cast<std::uint16_t>(found - particleSprites_.begin());
    }
    particleSprites_.push_back(path);
    return static_cast<std::uint16_t>(particleSprites_.size() - 1);
}

void World::spawnParticle(const EmitterSettings& s, glm::vec3 origin) {
    if (particles_.size() >= MaxParticles) {
        return;
    }
    auto jitter = [&](float amount) {
        return static_cast<float>(random_.unit() * 2 - 1) * amount;
    };
    Particle particle;
    particle.position = origin + glm::vec3(jitter(s.area.x), jitter(s.area.y), jitter(s.area.z));
    particle.velocity = s.velocity + glm::vec3(jitter(s.velocityJitter.x), jitter(s.velocityJitter.y),
                                               jitter(s.velocityJitter.z));
    particle.lifetime = std::max(0.05f, s.lifetime + jitter(s.lifetimeJitter));
    particle.size = s.size;
    particle.sizeEnd = s.sizeEnd;
    particle.drag = s.drag;
    particle.stretch = s.stretch;
    particle.gravity = s.gravity;
    particle.color = s.color;
    particle.colorEnd = s.colorEnd;
    particle.sprite = particleSprite(s.sprite);
    particle.unlit = s.unlit;
    particles_.push_back(particle);
}

void World::burst(glm::vec3 position, const EmitterSettings& settings, int count) {
    for (int i = 0; i < std::clamp(count, 0, 4096); ++i) {
        spawnParticle(settings, position);
    }
}

void World::updateEmitters(double dt) {
    for (const auto entity : registry_.view<Transform, Emitter>()) {
        auto& emitter = registry_.get<Emitter>(entity);
        const auto origin = registry_.get<Transform>(entity).position;
        if (!emitter.burstDone) {
            burst(origin, emitter.settings, emitter.settings.burst);
            emitter.burstDone = true;
        }
        if (!emitter.active) {
            continue;
        }
        emitter.accumulator += dt * emitter.settings.rate;
        while (emitter.accumulator >= 1) {
            spawnParticle(emitter.settings, origin);
            emitter.accumulator -= 1;
        }
    }
}

void World::updateWeather(double dt) {
    const auto& weather = map_.environment.weather;
    if (weather.kind == "none" || weather.intensity <= 0) {
        return;
    }
    EmitterSettings s;
    s.sprite = weather.sprite;
    s.color = s.colorEnd = weather.color;
    s.area = {18, 1, 18};
    glm::vec3 origin = camera_.target + glm::vec3(0, 12, 0);
    float rate = 0;
    if (weather.kind == "rain") {
        rate = 700;
        s.velocity = weather.wind + glm::vec3(0, -16, 0);
        s.velocityJitter = {0.2f, 1, 0.2f};
        s.lifetime = 1.1f;
        s.size = s.sizeEnd = 0.025f;
        s.stretch = 0.05f;
        if (weather.color == glm::vec4(1)) {
            s.color = s.colorEnd = {0.75f, 0.82f, 1, 0.5f};
        }
    } else if (weather.kind == "snow") {
        rate = 220;
        s.velocity = weather.wind + glm::vec3(0, -1.2f, 0);
        s.velocityJitter = {0.4f, 0.2f, 0.4f};
        s.lifetime = 10;
        s.size = s.sizeEnd = 0.06f;
    } else if (weather.kind == "leaves") {
        rate = 18;
        s.velocity = weather.wind + glm::vec3(0.5f, -0.7f, 0.2f);
        s.velocityJitter = {0.5f, 0.2f, 0.5f};
        s.lifetime = 9;
        s.size = s.sizeEnd = 0.12f;
        if (weather.color == glm::vec4(1)) {
            s.color = s.colorEnd = {0.95f, 0.55f, 0.2f, 1};
        }
    } else {
        // embers, dust and custom drift up from the ground around the camera
        origin = camera_.target + glm::vec3(0, 0.3f, 0);
        s.area = {14, 0.3f, 14};
        rate = weather.kind == "embers" ? 40.0f : 25.0f;
        s.velocity = weather.wind + glm::vec3(0, weather.kind == "embers" ? 0.9f : 0.15f, 0);
        s.velocityJitter = {0.3f, 0.2f, 0.3f};
        s.lifetime = 4;
        s.size = s.sizeEnd = weather.kind == "embers" ? 0.05f : 0.04f;
        s.colorEnd.a = 0;
        if (weather.kind == "embers" && weather.color == glm::vec4(1)) {
            s.color = {2.2f, 0.9f, 0.3f, 1};
            s.colorEnd = {1.5f, 0.3f, 0.1f, 0};
        }
    }
    weatherAccumulator_ += dt * rate * weather.intensity;
    while (weatherAccumulator_ >= 1) {
        spawnParticle(s, origin);
        weatherAccumulator_ -= 1;
    }
}

void World::updateParticles(double dt) {
    const auto delta = static_cast<float>(dt);
    for (auto& p : particles_) {
        p.age += delta;
        p.velocity += p.gravity * delta;
        p.velocity *= std::exp(-p.drag * delta);
        p.position += p.velocity * delta;
    }
    std::erase_if(particles_, [](const Particle& p) { return p.age >= p.lifetime; });
}

void World::updateContacts() {
    std::set<std::pair<entt::entity, entt::entity>> current;
    for (const auto& [key, bucket] : hash_) {
        (void)key;
        for (const auto a : bucket) {
            const auto pa = registry_.get<Transform>(a).position;
            const auto ra = registry_.get<Collider>(a).radius;
            const int cx = static_cast<int>(std::floor(pa.x / HashCell));
            const int cz = static_cast<int>(std::floor(pa.z / HashCell));
            for (int z = cz - 1; z <= cz + 1; ++z) {
                for (int x = cx - 1; x <= cx + 1; ++x) {
                    const auto found = hash_.find(hashKey(x, z));
                    if (found == hash_.end()) {
                        continue;
                    }
                    for (const auto b : found->second) {
                        if (!(a < b)) {
                            continue;
                        }
                        const auto pb = registry_.get<Transform>(b).position;
                        const float reach = ra + registry_.get<Collider>(b).radius + 0.05f;
                        const glm::vec2 d(pb.x - pa.x, pb.z - pa.z);
                        if (std::abs(pb.y - pa.y) < 1.0f && glm::dot(d, d) <= reach * reach) {
                            current.emplace(a, b);
                        }
                    }
                }
            }
        }
    }
    for (const auto& pair : current) {
        if (!contacts_.contains(pair)) {
            events_.push_back({WorldEvent::Kind::ContactBegin, pair.first, pair.second, {}});
        }
    }
    for (const auto& pair : contacts_) {
        if (!current.contains(pair) && registry_.valid(pair.first) && registry_.valid(pair.second)) {
            events_.push_back({WorldEvent::Kind::ContactEnd, pair.first, pair.second, {}});
        }
    }
    contacts_ = std::move(current);
}

void World::updateZones() {
    if (map_.zones.empty()) {
        return;
    }
    for (const auto entity : registry_.view<Transform, Collider>()) {
        const auto& transform = registry_.get<Transform>(entity);
        auto& inside = zonesInside_[entity];
        for (const auto& zone : map_.zones) {
            const bool now = zone.contains(transform.position, transform.layer);
            const bool before = inside.contains(zone.id);
            if (now && !before) {
                inside.insert(zone.id);
                events_.push_back({WorldEvent::Kind::ZoneEnter, entity, entt::null, zone.id});
            } else if (!now && before) {
                inside.erase(zone.id);
                events_.push_back({WorldEvent::Kind::ZoneExit, entity, entt::null, zone.id});
            }
        }
    }
}

std::vector<LightSettings> World::activeLights() const {
    std::vector<LightSettings> lights = map_.lights;
    for (const auto entity : registry_.view<Transform, LightSource>()) {
        const auto& source = registry_.get<LightSource>(entity);
        if (!source.enabled) {
            continue;
        }
        auto light = source.light;
        light.position += registry_.get<Transform>(entity).position;
        lights.push_back(light);
    }
    for (std::size_t i = 0; i < lights.size(); ++i) {
        auto& light = lights[i];
        if (light.flicker > 0) {
            light.intensity *= std::max(
                0.0f, 1 + light.flicker * noise(time_ * light.flickerSpeed, static_cast<float>(i) * 3.7f));
        }
    }
    return lights;
}

glm::vec3 World::interpolated(entt::entity entity, double alpha) const {
    const auto& transform = registry_.get<Transform>(entity);
    return glm::mix(transform.previous, transform.position, static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
}
} // namespace fe
