#include "engine/world/Movement.h"
#include "engine/nav/Query.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fe {
namespace {
constexpr float SubStep = 0.05f;      // longest collision-checked move
constexpr float DefaultRadius = 0.2f; // entities without a mover
constexpr float DefaultHeight = 1.6f; // entities without a collider
} // namespace

MovementSystem::MovementSystem(entt::registry& registry, Grid& grid, CollisionSystem& collision,
                               EventQueue& events)
    : registry_(registry), grid_(grid), collision_(collision), events_(events) {
}

int MovementSystem::targetLayer(glm::vec3 target, int current) const {
    const int x = floorCell(target.x);
    const int z = floorCell(target.z);
    if (grid_.contains(x, z, current) && grid_.at(x, z, current).walkable) {
        return current;
    }
    std::optional<int> best;
    float distance = 0;
    for (int l = 0; l < grid_.layerCount(); ++l) {
        if (!grid_.at(x, z, l).walkable) {
            continue;
        }
        const float d = std::abs(grid_.heightAt({target.x, target.z}, l) - target.y);
        if (!best || d < distance) {
            best = l;
            distance = d;
        }
    }
    return best.value_or(current);
}

GridPos MovementSystem::cellOf(entt::entity entity) const {
    const auto& transform = registry_.get<Transform>(entity);
    return {floorCell(transform.position.x), floorCell(transform.position.z), transform.layer};
}

void MovementSystem::moveTo(entt::entity entity, glm::vec3 target, bool findPath, std::optional<int> layer) {
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    std::vector<Waypoint> route;
    if (!findPath) {
        route.push_back({target, layer.value_or(-1), StepKind::Walk});
        follow(entity, std::move(route));
        return;
    }
    finishClimb(entity);
    const auto& transform = registry_.get<Transform>(entity);
    const auto from = cellOf(entity);
    GridPos to{floorCell(target.x), floorCell(target.z), layer.value_or(transform.layer)};
    if (!grid_.contains(to)) {
        throw std::runtime_error("Movement target is outside the map grid");
    }
    if (!layer) {
        to.layer = targetLayer(target, transform.layer);
    }
    StepRules rules;
    rules.maxUp = rules.maxDown = mover.maxStep;
    rules.diagonal = true;
    rules.jump = mover.jump;
    const auto planned = findRoute(grid_, from, to, rules, routeVisitLimit_);
    const auto& path = planned.steps;
    // A search stopped by its visit cap may hold a detour (or nothing): the
    // mover does not follow it, and says so the way it does for no route.
    if (path.empty() || planned.truncated) {
        stop(entity);
        mover.jumping = false;
        events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
        return;
    }
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const auto& cell = path[i].cell;
        const glm::vec2 centre(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f);
        route.push_back({{centre.x, grid_.heightAt(centre, cell.layer), centre.y}, cell.layer, path[i].kind});
    }
    if (path.front().kind != StepKind::Walk) {
        // Jumps and climbs start from the middle of the start cell.
        const glm::vec2 centre(static_cast<float>(from.x) + 0.5f, static_cast<float>(from.z) + 0.5f);
        route.insert(route.begin(), {{centre.x, grid_.heightAt(centre, from.layer), centre.y}, from.layer});
    }
    route.push_back({target, path.back().cell.layer, path.back().kind});
    follow(entity, std::move(route));
}

void MovementSystem::follow(entt::entity entity, std::vector<Waypoint> route) {
    finishClimb(entity);
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    mover.waypoints = std::move(route);
    mover.stuckTime = 0;
    mover.jumping = false;
    mover.active = !mover.waypoints.empty();
}

void MovementSystem::stop(entt::entity entity) {
    if (auto* mover = registry_.try_get<Mover>(entity)) {
        mover->active = false;
        mover->waypoints.clear();
        mover->drive = glm::vec2(0);
    }
}

void MovementSystem::jump(entt::entity entity, glm::vec3 target, float height, float duration, int layer) {
    finishClimb(entity);
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    mover.active = false;
    mover.waypoints.clear();
    startJump(entity, target, height, duration, layer);
}

void MovementSystem::startJump(entt::entity entity, glm::vec3 target, float height, float duration,
                               int layer) {
    auto& mover = registry_.get<Mover>(entity);
    mover.jumping = true;
    mover.jumpFrom = registry_.get<Transform>(entity).position;
    mover.jumpTo = target;
    mover.jumpHeight = height;
    mover.jumpDuration = std::max(0.05f, duration);
    mover.jumpTime = 0;
    mover.jumpLayer = std::clamp(layer, 0, grid_.layerCount() - 1);
    events_.push({WorldEvent::Kind::JumpStart, entity, entt::null, {}});
}

bool MovementSystem::startClimb(entt::entity entity, GridPos to, glm::vec3 end) {
    auto& mover = registry_.get<Mover>(entity);
    const auto& transform = registry_.get<Transform>(entity);
    const auto from = cellOf(entity);
    const glm::ivec2 step(to.x - from.x, to.z - from.z);
    const auto* ladder = grid_.ladderStep(from, step);
    if (!ladder || !(to == (from == ladder->foot() ? ladder->top() : ladder->foot()))) {
        return false;
    }
    // The climb runs up the face between the two cells, on the foot's side.
    const auto foot = ladder->foot();
    const glm::vec2 wall(ladder->direction);
    const glm::vec2 edge = glm::vec2(static_cast<float>(foot.x) + 0.5f, static_cast<float>(foot.z) + 0.5f) +
                           wall * (0.5f - std::max(mover.radius, 0.05f));
    const float start = transform.position.y;
    const float finish =
        to == ladder->top() ? grid_.edgeHeight(to, -step.x, -step.y) : grid_.heightIn(foot, edge);
    mover.climbPoints = {{{edge.x, start, edge.y}, mover.speed},
                         {{edge.x, finish, edge.y}, mover.climbSpeed},
                         {end, mover.speed}};
    mover.climbFacing = wall;
    mover.climbLayer = to.layer;
    mover.climbing = true;
    events_.push({WorldEvent::Kind::ClimbStart, entity, entt::null, {}});
    return true;
}

void MovementSystem::finishClimb(entt::entity entity) {
    auto* mover = registry_.try_get<Mover>(entity);
    if (!mover || !mover->climbing) {
        return;
    }
    auto& transform = registry_.get<Transform>(entity);
    transform.position = mover->climbPoints.back().first;
    transform.layer = mover->climbLayer;
    mover->climbing = false;
    mover->climbPoints.clear();
    collision_.moved(entity);
    events_.push({WorldEvent::Kind::ClimbEnd, entity, entt::null, {}});
}

void MovementSystem::place(entt::entity entity, glm::vec3 position, int layer, bool keepHeight) {
    auto& transform = registry_.get<Transform>(entity);
    transform.layer = std::clamp(layer, 0, grid_.layerCount() - 1);
    if (!keepHeight) {
        position.y = grid_.heightAt({position.x, position.z}, transform.layer);
    }
    transform.position = transform.previous = position;
    stop(entity);
    if (auto* mover = registry_.try_get<Mover>(entity)) {
        mover->jumping = mover->climbing = false;
        mover->climbPoints.clear();
    }
    collision_.moved(entity);
}

bool MovementSystem::moving(entt::entity entity) const {
    const auto* mover = registry_.try_get<Mover>(entity);
    return mover && (mover->active || mover->jumping || mover->climbing || glm::length(mover->drive) > 0);
}

float MovementSystem::bodyHeight(entt::entity entity) const {
    const auto* collider = registry_.try_get<Collider>(entity);
    return collider ? collider->height : DefaultHeight;
}

bool MovementSystem::canOccupy(entt::entity entity, glm::vec3 position, int layer) const {
    const auto* mover = registry_.try_get<Mover>(entity);
    const float radius = mover ? mover->radius : DefaultRadius;
    return supported({position.x, position.z}, radius, layer) &&
           !collision_.blockedByBox(position, radius, bodyHeight(entity)) &&
           !collision_.blockedByEntity(entity, position, radius);
}

bool MovementSystem::supported(glm::vec2 p, float radius, int layer) const {
    if (grid_.canStand(p, radius, layer)) {
        return true;
    }
    // Any link of the cell whose other surface meets this one and holds the body.
    return grid_.anyLinkAt(floorCell(p.x), floorCell(p.y), layer, [&](const GridLink& link) {
        const int other = link.other(layer);
        return std::abs(grid_.heightAt(p, other) - grid_.heightAt(p, layer)) <= Grid::LinkGap + 1e-4f &&
               grid_.canStand(p, radius, other);
    });
}

bool MovementSystem::stepTowards(entt::entity entity, glm::vec2 delta) {
    auto& transform = registry_.get<Transform>(entity);
    const auto& mover = registry_.get<Mover>(entity);
    const float length = glm::length(delta);
    if (length > 1e-5f) {
        transform.facing = delta / length;
    }
    if (!mover.collide) {
        transform.position.x += delta.x;
        transform.position.z += delta.y;
        collision_.moved(entity);
        return length > 1e-5f;
    }
    const float height = bodyHeight(entity);
    glm::vec2 p(transform.position.x, transform.position.z);
    int layer = transform.layer;
    auto attempt = [&](glm::vec2 move) {
        const float distance = glm::length(move);
        if (distance < 1e-7f) {
            return false;
        }
        const glm::vec2 to = p + move;
        const glm::vec2 ahead = to + move / distance * std::max(mover.radius, SubStep);
        const int next = grid_.traverseLink(to, layer, move, mover.maxStep, mover.maxStep,
                                            !grid_.walkableAt(ahead, layer));
        const float here = grid_.heightAt(p, layer);
        const float there = grid_.heightAt(to, next);
        const glm::vec3 probe(to.x, there, to.y);
        if (!supported(to, mover.radius, next) || std::abs(there - here) > mover.maxStep ||
            collision_.blockedByBox(probe, mover.radius, height) ||
            collision_.blockedByEntity(entity, probe, mover.radius)) {
            return false;
        }
        p = to;
        layer = next;
        return true;
    };
    const glm::vec2 start = p;
    // Script speeds are bounded, so a tick moves far less than MaxSubSteps
    // sub-steps; the cap only keeps a corrupt length from looping for ever.
    constexpr float MaxSubSteps = 4096;
    const float wanted = std::ceil(length / SubStep);
    const int steps = wanted >= 1 && wanted <= MaxSubSteps ? static_cast<int>(wanted)
                      : wanted > MaxSubSteps               ? static_cast<int>(MaxSubSteps)
                                                           : 1;
    const auto increment = delta / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        if (!attempt(increment) && !attempt({increment.x, 0})) {
            attempt({0, increment.y});
        }
    }
    transform.layer = layer;
    transform.position = {p.x, grid_.heightAt(p, layer), p.y};
    collision_.moved(entity);
    return glm::length(p - start) > 1e-5f;
}

void MovementSystem::advanceJump(entt::entity entity, float dt) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    mover.jumpTime += dt;
    const float t = std::min(1.0f, mover.jumpTime / mover.jumpDuration);
    transform.position =
        glm::mix(mover.jumpFrom, mover.jumpTo, t) + glm::vec3(0, 4 * mover.jumpHeight * t * (1 - t), 0);
    const glm::vec2 heading(mover.jumpTo.x - mover.jumpFrom.x, mover.jumpTo.z - mover.jumpFrom.z);
    if (glm::length(heading) > 1e-4f) {
        transform.facing = glm::normalize(heading);
    }
    collision_.moved(entity);
    if (t < 1) {
        return;
    }
    mover.jumping = false;
    transform.layer = mover.jumpLayer;
    events_.push({WorldEvent::Kind::Landed, entity, entt::null, {}});
    if (mover.active && mover.waypoints.empty()) {
        mover.active = false;
        events_.push({WorldEvent::Kind::Arrived, entity, entt::null, {}});
    }
}

void MovementSystem::advanceClimb(entt::entity entity, float dt) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    float budget = dt;
    while (!mover.climbPoints.empty() && budget > 0) {
        const auto [point, speed] = mover.climbPoints.front();
        const auto offset = point - transform.position;
        const float distance = glm::length(offset);
        const float reach = std::max(speed, 0.01f) * budget;
        const glm::vec2 flat(offset.x, offset.z);
        transform.facing = glm::length(flat) > 1e-4f && std::abs(offset.y) < 1e-4f ? glm::normalize(flat)
                                                                                   : mover.climbFacing;
        if (distance > reach) {
            transform.position += offset / distance * reach;
            break;
        }
        transform.position = point;
        budget -= distance / std::max(speed, 0.01f);
        mover.climbPoints.erase(mover.climbPoints.begin());
    }
    collision_.moved(entity);
    if (!mover.climbPoints.empty()) {
        return;
    }
    mover.climbing = false;
    transform.layer = mover.climbLayer;
    events_.push({WorldEvent::Kind::ClimbEnd, entity, entt::null, {}});
    if (mover.active && mover.waypoints.empty()) {
        mover.active = false;
        events_.push({WorldEvent::Kind::Arrived, entity, entt::null, {}});
    }
}

void MovementSystem::advanceRoute(entt::entity entity, float dt) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    const float step = mover.speed * dt;
    float budget = step;
    const auto before = transform.position;
    while (mover.active && !mover.waypoints.empty() && !mover.jumping && !mover.climbing && budget > 0) {
        const auto waypoint = mover.waypoints.front();
        const int layer = waypoint.layer >= 0 ? waypoint.layer : transform.layer;
        if (waypoint.kind == StepKind::Jump) {
            mover.waypoints.erase(mover.waypoints.begin());
            const glm::vec2 across(waypoint.position.x - transform.position.x,
                                   waypoint.position.z - transform.position.z);
            const float length = glm::length(across);
            const float rise = std::abs(waypoint.position.y - transform.position.y);
            startJump(entity, waypoint.position, 0.4f + 0.2f * length + 0.5f * rise,
                      std::max(0.3f, length / std::max(mover.speed, 0.1f)), layer);
            break;
        }
        if (waypoint.kind == StepKind::Climb) {
            mover.waypoints.erase(mover.waypoints.begin());
            const GridPos to{floorCell(waypoint.position.x), floorCell(waypoint.position.z), layer};
            if (!startClimb(entity, to, waypoint.position)) {
                mover.active = false;
                mover.waypoints.clear();
                events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
                return;
            }
            break;
        }
        const glm::vec2 to(waypoint.position.x - transform.position.x,
                           waypoint.position.z - transform.position.z);
        const float distance = glm::length(to);
        if (distance > std::max(budget, 0.02f)) {
            stepTowards(entity, to / distance * budget);
            if (!mover.collide) {
                transform.position.y = glm::mix(transform.position.y, waypoint.position.y, budget / distance);
            }
            break;
        }
        stepTowards(entity, to);
        if (!mover.collide) {
            transform.position.y = waypoint.position.y;
        }
        if (glm::length(glm::vec2(transform.position.x - waypoint.position.x,
                                  transform.position.z - waypoint.position.z)) >= 0.05f) {
            break; // blocked short of the waypoint
        }
        if (waypoint.layer >= 0) {
            transform.layer = waypoint.layer;
            if (!mover.collide) {
                collision_.moved(entity);
            }
        }
        budget -= distance;
        mover.waypoints.erase(mover.waypoints.begin());
    }
    if (mover.jumping || mover.climbing) {
        mover.stuckTime = 0;
        return;
    }
    const float moved = glm::length(transform.position - before);
    mover.stuckTime = moved < step * 0.1f ? mover.stuckTime + dt : 0;
    if (mover.waypoints.empty()) {
        mover.active = false;
        events_.push({WorldEvent::Kind::Arrived, entity, entt::null, {}});
    } else if (mover.stuckTime > 1.0) {
        mover.active = false;
        mover.waypoints.clear();
        events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
    }
}

void MovementSystem::update(double dt) {
    const auto delta = static_cast<float>(dt);
    for (const auto entity : registry_.view<Transform, Mover>()) {
        auto& mover = registry_.get<Mover>(entity);
        if (mover.jumping || mover.climbing) {
            mover.drive = glm::vec2(0); // steering waits until the body is back on its feet
            if (mover.jumping) {
                advanceJump(entity, delta);
            } else {
                advanceClimb(entity, delta);
            }
            continue;
        }
        if (glm::length(mover.drive) > 0) {
            const auto drive = mover.drive * delta;
            mover.drive = glm::vec2(0);
            const auto before = registry_.get<Transform>(entity).position;
            stepTowards(entity, drive);
            const auto after = registry_.get<Transform>(entity).position;
            if (mover.collide &&
                glm::length(glm::vec2(after.x - before.x, after.z - before.z)) < 0.1f * glm::length(drive)) {
                // Walking into a ladder climbs it.
                const bool alongX = std::abs(drive.x) >= std::abs(drive.y);
                const glm::ivec2 axis =
                    alongX ? glm::ivec2(drive.x > 0 ? 1 : -1, 0) : glm::ivec2(0, drive.y > 0 ? 1 : -1);
                const float share = std::abs(alongX ? drive.x : drive.y) / glm::length(drive);
                const auto from = cellOf(entity);
                if (const auto* ladder = share > 0.7f ? grid_.ladderStep(from, axis) : nullptr) {
                    const auto to = from == ladder->foot() ? ladder->top() : ladder->foot();
                    const glm::vec2 centre(static_cast<float>(to.x) + 0.5f, static_cast<float>(to.z) + 0.5f);
                    if (grid_.at(to).walkable) {
                        startClimb(entity, to, {centre.x, grid_.heightAt(centre, to.layer), centre.y});
                    }
                }
            }
            continue;
        }
        if (mover.active && !mover.waypoints.empty()) {
            advanceRoute(entity, delta);
        }
    }
}

void MovementSystem::updateAttachments() {
    for (const auto entity : registry_.view<Transform>()) {
        auto& transform = registry_.get<Transform>(entity);
        if (transform.parent == entt::null) {
            continue;
        }
        const auto* parent =
            registry_.valid(transform.parent) ? registry_.try_get<Transform>(transform.parent) : nullptr;
        if (!parent) {
            transform.parent = entt::null; // the parent was destroyed
            continue;
        }
        transform.position = parent->position + transform.offset;
        transform.previous = parent->previous + transform.offset;
        transform.layer = parent->layer;
        collision_.moved(entity);
    }
}

void MovementSystem::collectRiders() {
    for (const auto [platform, transform, surface] : registry_.view<Transform, Platform>().each()) {
        auto [found, added] = rides_.try_emplace(platform);
        auto& ride = found->second;
        if (added) {
            ride.last = transform.position;
        }
        ride.riders.clear();
        const glm::vec2 half = surface.size * 0.5f;
        for (const auto [entity, rider] : registry_.view<Transform>().each()) {
            if (entity == platform || rider.parent != entt::null || rider.layer != transform.layer ||
                registry_.all_of<Platform>(entity) || !registry_.any_of<Mover, Collider>(entity)) {
                continue;
            }
            if (const auto* mover = registry_.try_get<Mover>(entity);
                mover && (mover->jumping || mover->climbing)) {
                continue;
            }
            const auto offset = rider.position - ride.last;
            if (std::abs(offset.x) <= half.x && std::abs(offset.z) <= half.y && std::abs(offset.y) < 0.2f) {
                ride.riders.push_back(entity);
            }
        }
    }
}

void MovementSystem::restoreCells() {
    for (auto it = overrides_.rbegin(); it != overrides_.rend(); ++it) {
        if (grid_.contains(it->cell) && grid_.at(it->cell) == it->applied) {
            grid_.at(it->cell) = it->original; // unless a script changed the cell since
        }
    }
    overrides_.clear();
}

// Riders that collide when walking keep colliding while carried: a solid
// entity (not on the same platform) stops the horizontal part of the carry,
// sliding along it where one axis is free; the vertical part always applies.
void MovementSystem::carry(entt::entity rider, glm::vec3 delta, std::span<const entt::entity> aboard) {
    auto& carried = registry_.get<Transform>(rider);
    const auto* mover = registry_.try_get<Mover>(rider);
    const auto* collider = registry_.try_get<Collider>(rider);
    const bool collides = mover ? mover->collide : collider && collider->solid;
    const float radius = mover ? mover->radius : collider ? collider->radius : 0.0f;
    glm::vec3 applied(0, delta.y, 0);
    if (collides) {
        for (const glm::vec3 move : {delta, glm::vec3(delta.x, delta.y, 0), glm::vec3(0, delta.y, delta.z)}) {
            if (!collision_.blockedByEntity(rider, carried.position + move, radius, aboard)) {
                applied = move;
                break;
            }
        }
    } else {
        applied = delta;
    }
    carried.position += applied;
    collision_.moved(rider);
}

void MovementSystem::carryRiders() {
    std::erase_if(rides_, [&](const auto& entry) {
        return !registry_.valid(entry.first) || !registry_.all_of<Transform, Platform>(entry.first);
    });
    restoreCells();
    for (auto& [platform, ride] : rides_) {
        const auto& transform = registry_.get<Transform>(platform);
        const auto delta = transform.position - ride.last;
        ride.last = transform.position;
        aboard_.assign(ride.riders.begin(), ride.riders.end());
        aboard_.push_back(platform);
        for (const auto rider : ride.riders) {
            if (registry_.valid(rider)) {
                carry(rider, delta, aboard_);
            }
        }
        const glm::vec2 half = registry_.get<Platform>(platform).size * 0.5f;
        // Cells whose centre the platform covers, within the grid.
        const int x0 = std::max(0, ceilCell(transform.position.x - half.x - 0.5f));
        const int x1 = std::min(grid_.width() - 1, floorCell(transform.position.x + half.x - 0.5f));
        const int z0 = std::max(0, ceilCell(transform.position.z - half.y - 0.5f));
        const int z1 = std::min(grid_.depth() - 1, floorCell(transform.position.z + half.y - 0.5f));
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const GridPos cell{x, z, transform.layer};
                if (!grid_.contains(cell)) {
                    continue;
                }
                auto& current = grid_.at(cell);
                CellOverride entry{cell, current, current};
                entry.applied.walkable = true;
                entry.applied.height = transform.position.y;
                entry.applied.ramp = Ramp::None;
                entry.applied.rise = 1;
                current = entry.applied;
                overrides_.push_back(entry);
            }
        }
    }
}
} // namespace fe
