#include "engine/world/Movement.h"
#include "engine/nav/Query.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace fe {
namespace {
constexpr float SubStep = 0.05f;             // longest collision-checked move
constexpr float MinimumJumpSeconds = 0.05f;  // shortest arc jump() accepts
constexpr float LandingSearchStep = 0.25f;   // ring spacing when a landing is blocked
constexpr int LandingSearchAngles = 16;      // spots per ring
constexpr float PlatformBoardHeight = 0.2f;  // how far above or below a platform's top a rider's feet may be
constexpr int MaxAttachmentDepth = 64;       // longest parent chain followed before giving up

glm::vec2 cellCentre(int x, int z) {
    return {static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f};
}

// `v` turned about the y axis by `yaw` radians, as the renderer turns meshes.
glm::vec3 rotateYaw(glm::vec3 v, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

glm::vec2 flat(glm::vec3 p) {
    return {p.x, p.z};
}
} // namespace

MovementSystem::MovementSystem(entt::registry& registry, Grid& grid, CollisionSystem& collision,
                               EventQueue& events)
    : registry_(registry), grid_(grid), collision_(collision), events_(events) {
}

MovementSystem::Body MovementSystem::bodyOf(entt::entity entity) const {
    Body body;
    const auto* mover = registry_.try_get<Mover>(entity);
    const auto* collider = registry_.try_get<Collider>(entity);
    if (collider) {
        body.height = collider->height;
        body.radius = collider->radius;
    }
    if (mover) {
        body.radius = mover->radius;
        body.footprint = std::clamp(mover->footprint, 1, MaxFootprint);
        body.fly = mover->fly;
        body.collide = mover->collide;
        body.blockedTags = mover->blockedTags;
        body.requiredTags = mover->requiredTags;
    }
    return body;
}

int MovementSystem::targetLayer(glm::vec3 target, int current, const Body& body) const {
    const int x = floorCell(target.x);
    const int z = floorCell(target.z);
    const auto open = [&](int layer) {
        if (!grid_.contains(x, z, layer)) {
            return false;
        }
        const auto& cell = std::as_const(grid_).at(x, z, layer);
        return (body.fly || cell.walkable) && !(cell.tags & body.blockedTags) &&
               (!body.requiredTags || (cell.tags & body.requiredTags));
    };
    if (open(current)) {
        return current;
    }
    std::optional<int> best;
    float distance = 0;
    for (int l = 0; l < grid_.layerCount(); ++l) {
        if (!open(l)) {
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

void MovementSystem::moveTo(entt::entity entity, glm::vec3 target, bool findPath, std::optional<int> layer,
                            const StepRules* rules) {
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    std::vector<Waypoint> route;
    if (!findPath) {
        route.push_back({target, layer.value_or(-1), StepKind::Walk});
        follow(entity, std::move(route));
        return;
    }
    const auto& transform = registry_.get<Transform>(entity);
    const GridPos to =
        grid_.wrapCell({floorCell(target.x), floorCell(target.z), layer.value_or(transform.layer)});
    if (!grid_.contains(to)) {
        throw std::runtime_error("Movement target is outside the map grid");
    }
    settle(entity);
    const auto body = bodyOf(entity);
    const auto from = cellOf(entity);
    const int goalLayer = layer ? to.layer : targetLayer(target, transform.layer, body);
    StepRules planning;
    if (rules) {
        planning = *rules;
    } else {
        planning.maxUp = planning.maxDown = mover.maxStep;
        planning.diagonal = true;
        planning.jump = mover.jump;
        planning.blockedTags = mover.blockedTags;
        planning.requiredTags = mover.requiredTags;
    }
    planning.footprint = body.footprint;
    planning.fly = body.fly;
    if (blockedCells_ && !blockedCells_->empty()) {
        // Static boxes: the route goes around them instead of into them.
        planning.blocked.insert(blockedCells_->begin(), blockedCells_->end());
        planning.blocked.erase(cellKey(from)); // a body already beside a box may leave
    }
    const auto planned = findRoute(grid_, from, {to.x, to.z, goalLayer}, planning, routeVisitLimit_);
    // The search may have run a script's cost function, and a script can
    // destroy the entity it is planning for: nothing is left to move then,
    // and `mover` above no longer refers to anything.
    if (!registry_.valid(entity) || !registry_.all_of<Mover, Transform>(entity)) {
        return;
    }
    auto& planner = registry_.get<Mover>(entity);
    const auto& path = planned.steps;
    // A search stopped by its visit cap may hold a detour (or nothing): the
    // mover does not follow it, and says so the way it does for no route.
    if (path.empty() || planned.truncated) {
        reset(planner);
        events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
        return;
    }
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const auto& cell = path[i].cell;
        const auto centre = cellCentre(cell.x, cell.z);
        route.push_back({{centre.x, grid_.heightAt(centre, cell.layer), centre.y}, cell.layer, path[i].kind});
    }
    if (path.front().kind != StepKind::Walk) {
        // Jumps and climbs start from the middle of the start cell.
        const auto centre = cellCentre(from.x, from.z);
        route.insert(route.begin(), {{centre.x, grid_.heightAt(centre, from.layer), centre.y}, from.layer});
    }
    route.push_back({target, path.back().cell.layer, path.back().kind});
    follow(entity, std::move(route));
}

void MovementSystem::follow(entt::entity entity, std::vector<Waypoint> route) {
    settle(entity);
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    if (route.empty()) {
        reset(mover);
        return;
    }
    mover.state = Mover::Route{std::move(route)};
}

void MovementSystem::stop(entt::entity entity) {
    settle(entity);
    if (auto* mover = registry_.try_get<Mover>(entity)) {
        reset(*mover);
        mover->drive = glm::vec2(0);
    }
}

void MovementSystem::jump(entt::entity entity, glm::vec3 target, float height, float duration, int layer) {
    settle(entity);
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    reset(mover);
    if (auto jump = jumpTo(entity, target, height, duration, layer)) {
        begin(entity, std::move(*jump), std::nullopt);
    } else {
        events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
    }
}

bool MovementSystem::freeOfSolids(entt::entity entity, glm::vec3 position, const Body& body) const {
    return !collision_.blockedByBox(position, body.radius, body.height) &&
           !collision_.blockedByEntity(entity, position, body.radius, body.height);
}

float MovementSystem::restY(const Mover& mover, glm::vec2 p, int layer) const {
    return grid_.heightAt(p, layer) + mover.lift;
}

float MovementSystem::liftOf(entt::entity entity) const {
    const auto* mover = registry_.try_get<Mover>(entity);
    return mover ? mover->lift : 0.0f;
}

void MovementSystem::canonicalise(Transform& transform) const {
    if (!grid_.wraps()) {
        return;
    }
    const auto here = flat(transform.position);
    const auto wrapped = grid_.wrapPoint(here);
    const auto back = grid_.delta(here, flat(transform.previous));
    transform.position.x = wrapped.x;
    transform.position.z = wrapped.y;
    transform.previous.x = wrapped.x + back.x;
    transform.previous.z = wrapped.y + back.y;
}

void MovementSystem::easeLift(Mover& mover, float dt) {
    if (mover.altitudeSpeed <= 0) {
        mover.lift = mover.altitude;
        return;
    }
    const float step = mover.altitudeSpeed * dt;
    if (std::abs(mover.altitude - mover.lift) <= step) {
        mover.lift = mover.altitude;
    } else {
        mover.lift += mover.altitude > mover.lift ? step : -step;
    }
}

// `target` and the spots searched around it are surface points; a lifted body
// is tested that far above them, where its feet would be.
std::optional<glm::vec3> MovementSystem::landingFor(entt::entity entity, glm::vec3 target, int layer) const {
    const auto body = bodyOf(entity);
    const glm::vec3 up(0, liftOf(entity), 0);
    if (!body.collide || freeOfSolids(entity, target + up, body)) {
        return target;
    }
    const auto centre = flat(target);
    for (float ring = LandingSearchStep; ring <= tuning.landingSearch + 1e-4f; ring += LandingSearchStep) {
        for (int i = 0; i < LandingSearchAngles; ++i) {
            const float angle = 2 * std::numbers::pi_v<float> * static_cast<float>(i) /
                                static_cast<float>(LandingSearchAngles);
            const glm::vec2 spot = centre + glm::vec2(std::cos(angle), std::sin(angle)) * ring;
            const glm::vec3 probe(spot.x, grid_.heightAt(spot, layer), spot.y);
            if (supported(spot, body, layer) && freeOfSolids(entity, probe + up, body)) {
                return probe;
            }
        }
    }
    return std::nullopt;
}

std::optional<Mover::Jump> MovementSystem::jumpTo(entt::entity entity, glm::vec3 target, float height,
                                                  float duration, int layer) const {
    const auto& transform = registry_.get<Transform>(entity);
    layer = std::clamp(layer, 0, grid_.layerCount() - 1);
    const auto landing = landingFor(entity, target, layer);
    if (!landing) {
        return std::nullopt;
    }
    Mover::Jump jump;
    jump.from = transform.position;
    // The landing in the body's sheet of a wrapping grid, so the arc is the
    // short way round; a lifted body lands lifted.
    const auto reach = grid_.delta(flat(transform.position), flat(*landing));
    jump.to = glm::vec3(transform.position.x + reach.x, landing->y + liftOf(entity),
                        transform.position.z + reach.y);
    jump.height = height;
    jump.duration = std::max(MinimumJumpSeconds, duration);
    jump.fromLayer = transform.layer;
    jump.layer = layer;
    return jump;
}

std::optional<Mover::Climb> MovementSystem::climbTo(entt::entity entity, GridPos to, glm::vec3 end) const {
    const auto& mover = registry_.get<Mover>(entity);
    const auto& transform = registry_.get<Transform>(entity);
    const auto from = cellOf(entity);
    const glm::ivec2 step(to.x - from.x, to.z - from.z);
    const auto* ladder = grid_.ladderStep(from, step);
    if (!ladder || !(to == (from == ladder->foot() ? ladder->top() : ladder->foot()))) {
        return std::nullopt;
    }
    // The climb runs up the face between the two cells, on the foot's side.
    const auto foot = ladder->foot();
    const glm::vec2 wall(ladder->direction);
    const glm::vec2 edge = cellCentre(foot.x, foot.z) + wall * (0.5f - std::max(mover.radius, 0.05f));
    const float start = transform.position.y;
    const float finish =
        to == ladder->top() ? grid_.edgeHeight(to, -step.x, -step.y) : grid_.heightIn(foot, edge);
    Mover::Climb climb;
    climb.points = {{{edge.x, start, edge.y}, mover.speed},
                    {{edge.x, finish, edge.y}, mover.climbSpeed},
                    {end, mover.speed}};
    climb.start = transform.position;
    climb.facing = wall;
    climb.fromLayer = transform.layer;
    climb.layer = to.layer;
    return climb;
}

void MovementSystem::begin(entt::entity entity, Mover::Jump jump, std::optional<Mover::Route> route) {
    jump.route = std::move(route);
    registry_.get<Mover>(entity).state = std::move(jump);
    events_.push({WorldEvent::Kind::JumpStart, entity, entt::null, {}});
}

void MovementSystem::begin(entt::entity entity, Mover::Climb climb, std::optional<Mover::Route> route) {
    climb.route = std::move(route);
    registry_.get<Mover>(entity).state = std::move(climb);
    events_.push({WorldEvent::Kind::ClimbStart, entity, entt::null, {}});
}

void MovementSystem::settle(entt::entity entity) {
    const auto* mover = registry_.try_get<Mover>(entity);
    if (!mover) {
        return;
    }
    if (mover->as<Mover::Jump>()) {
        finishJump(entity, true);
    } else if (mover->as<Mover::Climb>()) {
        finishClimb(entity, true);
    } else if (mover->as<Mover::Launch>()) {
        const auto& transform = registry_.get<Transform>(entity);
        finishLaunch(entity, groundUnder(flat(transform.position), bodyOf(entity), transform.position.y));
    }
}

void MovementSystem::launch(entt::entity entity, glm::vec3 velocity) {
    auto& mover = registry_.get_or_emplace<Mover>(entity);
    if (mover.as<Mover::Climb>()) {
        throw std::runtime_error("Cannot launch an entity that is climbing a ladder");
    }
    auto& transform = registry_.get<Transform>(entity);
    // A jump or launch in flight goes on as this one, from where the body is.
    mover.state = Mover::Launch{velocity, transform.layer};
    mover.drive = glm::vec2(0);
    const auto heading = flat(velocity);
    if (glm::length(heading) > 1e-4f) {
        transform.facing = glm::normalize(heading);
    }
    events_.push({WorldEvent::Kind::JumpStart, entity, entt::null, {}});
}

MovementSystem::Surface MovementSystem::groundUnder(glm::vec2 position, const Body& body, float height) const {
    std::optional<Surface> best;
    for (int l = 0; l < grid_.layerCount(); ++l) {
        const float surface = grid_.heightAt(position, l);
        if (surface <= height + 1e-4f && (!best || surface > best->height) && supported(position, body, l)) {
            best = Surface{surface, l};
        }
    }
    return best.value_or(Surface{grid_.heightAt(position, 0), 0});
}

void MovementSystem::finishLaunch(entt::entity entity, Surface surface) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    mover.lift = 0; // a ballistic fall lands on the surface; the lift eases back up afterwards
    const glm::vec3 point(transform.position.x, surface.height, transform.position.z);
    const auto spot = landingFor(entity, point, surface.layer);
    transform.layer = surface.layer;
    transform.position = spot.value_or(point);
    reset(mover);
    canonicalise(transform);
    collision_.moved(entity);
    events_.push({WorldEvent::Kind::Landed, entity, entt::null, {}});
    if (!spot) {
        events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
    }
}

// Integrates the flight in sub-steps no longer horizontally than SubStep. A
// sub-step that would carry the body into a solid, a box, ground more than a
// step above the body or out of the grid ends the horizontal motion instead
// (a collide = false body minds the grid's edge only). Landing is
// tested after each sub-step: the highest standable surface between the
// heights the step started and ended at, or the floor (the lowest layer's
// surface) once the body is at or below it.
void MovementSystem::advanceLaunch(entt::entity entity, float dt) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    auto& flight = *mover.as<Mover::Launch>();
    const auto body = bodyOf(entity);
    const float gravity = tuning.gravity;
    glm::vec2 horizontal = flat(flight.velocity);
    const float wanted = std::ceil(glm::length(horizontal) * dt / SubStep);
    constexpr float MaxSubSteps = 4096;
    const int steps = wanted >= 1 && wanted <= MaxSubSteps ? static_cast<int>(wanted)
                      : wanted > MaxSubSteps               ? static_cast<int>(MaxSubSteps)
                                                           : 1;
    const float h = dt / static_cast<float>(steps);
    mover.moved = true;
    for (int i = 0; i < steps; ++i) {
        const glm::vec3 start = transform.position;
        const float y0 = start.y;
        const float y1 = y0 + flight.velocity.y * h - 0.5f * gravity * h * h;
        flight.velocity.y -= gravity * h;
        glm::vec2 to = flat(start) + horizontal * h;
        if (glm::length(horizontal) > 0) {
            const glm::vec3 probe(to.x, y1, to.y);
            const bool blocked =
                !grid_.contains(floorCell(to.x), floorCell(to.y)) ||
                (body.collide &&
                 (grid_.heightAt(to, 0) > std::max(y0, y1) + mover.maxStep + 1e-4f ||
                  collision_.blockedByBox(probe, body.radius, body.height) ||
                  collision_.blocksMove(entity, start, probe, body.radius, body.height)));
            if (blocked) {
                flight.velocity.x = flight.velocity.z = 0;
                horizontal = glm::vec2(0);
                to = flat(start);
            }
        }
        transform.position = {to.x, y1, to.y};
        canonicalise(transform);
        collision_.moved(entity);
        std::optional<Surface> landing;
        for (int l = 0; l < grid_.layerCount(); ++l) {
            const float surface = grid_.heightAt(to, l);
            if (surface > y0 + 1e-4f || surface < y1 || (landing && surface <= landing->height) ||
                !supported(to, body, l)) {
                continue;
            }
            landing = Surface{surface, l};
        }
        if (!landing && y1 <= grid_.heightAt(to, 0)) {
            landing = Surface{grid_.heightAt(to, 0), 0};
        }
        if (landing) {
            finishLaunch(entity, *landing);
            return;
        }
    }
}

void MovementSystem::resume(entt::entity entity, std::optional<Mover::Route> route) {
    auto& mover = registry_.get<Mover>(entity);
    if (route && !route->waypoints.empty()) {
        route->stuckTime = 0; // the walk starts afresh after the jump or climb
        mover.state = std::move(*route);
        return;
    }
    reset(mover);
    if (route) {
        events_.push({WorldEvent::Kind::Arrived, entity, entt::null, {}});
    }
}

void MovementSystem::finishJump(entt::entity entity, bool interrupted) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    auto arc = std::move(*mover.as<Mover::Jump>()); // the state is replaced below
    if (interrupted) {
        // Land on the ground under the body: the standable layer whose
        // surface is nearest the body's height; over a void, the nearer end
        // of the arc.
        const auto body = bodyOf(entity);
        const auto here = flat(transform.position);
        std::optional<int> ground;
        float nearest = 0;
        for (int l = 0; l < grid_.layerCount(); ++l) {
            const float rest = restY(mover, here, l);
            const float gap = std::abs(rest - transform.position.y);
            if ((!ground || gap < nearest) && supported(here, body, l) &&
                (!body.collide || !collision_.blockedByBox({here.x, rest, here.y}, body.radius, body.height))) {
                ground = l;
                nearest = gap;
            }
        }
        if (ground) {
            transform.layer = *ground;
            transform.position = {here.x, restY(mover, here, *ground), here.y};
        } else if (glm::length(grid_.delta(here, flat(arc.to))) <=
                   glm::length(grid_.delta(here, flat(arc.from)))) {
            transform.layer = arc.layer;
            transform.position = arc.to;
        } else {
            transform.layer = arc.fromLayer;
            transform.position = arc.from;
        }
        reset(mover);
    } else {
        transform.layer = arc.layer;
        transform.position = arc.to;
    }
    canonicalise(transform);
    collision_.moved(entity);
    events_.push({WorldEvent::Kind::Landed, entity, entt::null, {}});
    if (!interrupted) {
        resume(entity, std::move(arc.route));
    }
}

void MovementSystem::finishClimb(entt::entity entity, bool interrupted) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    auto climb = std::move(*mover.as<Mover::Climb>()); // the state is replaced below
    if (interrupted) {
        if (!climb.points.empty()) {
            // Step off at the nearer end of the ladder.
            const auto top = climb.points.back().first;
            if (glm::length(transform.position - top) <= glm::length(transform.position - climb.start)) {
                transform.position = top;
                transform.layer = climb.layer;
            } else {
                transform.position = climb.start;
                transform.layer = climb.fromLayer;
            }
        }
        reset(mover);
    } else {
        transform.layer = climb.layer;
    }
    canonicalise(transform);
    collision_.moved(entity);
    events_.push({WorldEvent::Kind::ClimbEnd, entity, entt::null, {}});
    if (!interrupted) {
        resume(entity, std::move(climb.route));
    }
}

void MovementSystem::place(entt::entity entity, glm::vec3 position, int layer, bool keepHeight) {
    settle(entity);
    auto& transform = registry_.get<Transform>(entity);
    transform.layer = std::clamp(layer, 0, grid_.layerCount() - 1);
    if (!keepHeight) {
        position.y = grid_.heightAt({position.x, position.z}, transform.layer) + liftOf(entity);
    }
    transform.position = transform.previous = position;
    canonicalise(transform);
    if (auto* mover = registry_.try_get<Mover>(entity)) {
        reset(*mover);
        mover->drive = glm::vec2(0);
    }
    collision_.moved(entity);
}

bool MovementSystem::moving(entt::entity entity) const {
    const auto* mover = registry_.try_get<Mover>(entity);
    return mover && (!mover->idle() || glm::length(mover->drive) > 0);
}

bool MovementSystem::canOccupy(entt::entity entity, glm::vec3 position, int layer) const {
    const auto body = bodyOf(entity);
    return supported({position.x, position.z}, body, layer) && freeOfSolids(entity, position, body);
}

bool MovementSystem::supported(glm::vec2 p, const Body& body, int layer) const {
    const auto stands = [&](int on) {
        return grid_.canStand(p, body.radius, on, body.footprint, body.fly, body.blockedTags, body.requiredTags);
    };
    if (stands(layer)) {
        return true;
    }
    // Any link of the cell whose other surface meets this one and holds the
    // body. The surfaces are compared where the link is crossed: at the
    // junction edge of a directed link (the edge walkers enter from, as path
    // finding tests it), under the body for an undirected one.
    return grid_.anyLinkAt(floorCell(p.x), floorCell(p.y), layer, [&](const GridLink& link) {
        const int other = link.other(layer);
        if (!grid_.contains(link.x, link.z, other) || !grid_.contains(link.x, link.z, layer)) {
            return false;
        }
        const glm::vec2 junction = link.direction == glm::ivec2(0)
                                       ? p
                                       : cellCentre(link.x, link.z) - glm::vec2(link.direction) * 0.5f;
        const float gap = grid_.heightIn({link.x, link.z, other}, junction) -
                          grid_.heightIn({link.x, link.z, layer}, junction);
        return std::abs(gap) <= Grid::LinkGap + 1e-4f && stands(other);
    });
}

bool MovementSystem::stepTowards(entt::entity entity, glm::vec2 delta) {
    auto& transform = registry_.get<Transform>(entity);
    auto& mover = registry_.get<Mover>(entity);
    const float length = glm::length(delta);
    if (length > 1e-5f) {
        transform.facing = delta / length;
    }
    if (!mover.collide) {
        transform.position.x += delta.x;
        transform.position.z += delta.y;
        canonicalise(transform);
        collision_.moved(entity);
        mover.moved = mover.moved || length > 1e-5f;
        return length > 1e-5f;
    }
    const auto body = bodyOf(entity);
    glm::vec2 p(transform.position.x, transform.position.z);
    int layer = transform.layer;
    auto attempt = [&](glm::vec2 move) {
        const float distance = glm::length(move);
        if (distance < 1e-7f) {
            return false;
        }
        const glm::vec2 to = p + move;
        const glm::vec2 ahead = to + move / distance * std::max(body.radius, SubStep);
        const int next = grid_.traverseLink(to, layer, move, mover.maxStep, mover.maxStep,
                                            !grid_.walkableAt(ahead, layer));
        // Surfaces decide the step; the body itself is tested where its feet are.
        const float here = grid_.heightAt(p, layer);
        const float there = grid_.heightAt(to, next);
        const glm::vec3 start(p.x, here + mover.lift, p.y);
        const glm::vec3 probe(to.x, there + mover.lift, to.y);
        if (!supported(to, body, next) || (!body.fly && std::abs(there - here) > mover.maxStep) ||
            collision_.blockedByBox(probe, body.radius, body.height) ||
            collision_.blocksMove(entity, start, probe, body.radius, body.height)) {
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
    transform.position = {p.x, restY(mover, p, layer), p.y};
    canonicalise(transform);
    collision_.moved(entity);
    const bool moved = glm::length(p - start) > 1e-5f;
    mover.moved = mover.moved || moved;
    return moved;
}

void MovementSystem::advanceJump(entt::entity entity, float dt) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    auto& arc = *mover.as<Mover::Jump>();
    arc.time += dt;
    const float t = std::min(1.0f, arc.time / arc.duration);
    transform.position = glm::mix(arc.from, arc.to, t) + glm::vec3(0, 4 * arc.height * t * (1 - t), 0);
    const glm::vec2 heading = flat(arc.to) - flat(arc.from); // one sheet: see jumpTo
    if (glm::length(heading) > 1e-4f) {
        transform.facing = glm::normalize(heading);
    }
    mover.moved = true;
    canonicalise(transform);
    collision_.moved(entity);
    if (t >= 1) {
        finishJump(entity, false);
    }
}

void MovementSystem::advanceClimb(entt::entity entity, float dt) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    auto& climb = *mover.as<Mover::Climb>();
    auto& points = climb.points;
    float budget = dt;
    while (!points.empty() && budget > 0) {
        const auto [point, speed] = points.front();
        const auto offset = point - transform.position;
        const float distance = glm::length(offset);
        const float reach = std::max(speed, 0.01f) * budget;
        const glm::vec2 level(offset.x, offset.z);
        transform.facing = glm::length(level) > 1e-4f && std::abs(offset.y) < 1e-4f ? glm::normalize(level)
                                                                                    : climb.facing;
        if (distance > reach) {
            transform.position += offset / distance * reach;
            break;
        }
        transform.position = point;
        budget -= distance / std::max(speed, 0.01f);
        points.erase(points.begin());
    }
    mover.moved = true;
    canonicalise(transform);
    collision_.moved(entity);
    if (points.empty()) {
        finishClimb(entity, false);
    }
}

void MovementSystem::advanceRoute(entt::entity entity, float dt) {
    auto& mover = registry_.get<Mover>(entity);
    auto& transform = registry_.get<Transform>(entity);
    auto& route = *mover.as<Mover::Route>();
    auto& waypoints = route.waypoints;
    const float step = mover.speed * dt;
    float budget = step;
    const auto before = transform.position;
    while (!waypoints.empty() && budget > 0) {
        const auto waypoint = waypoints.front();
        const int layer = waypoint.layer >= 0 ? waypoint.layer : transform.layer;
        if (waypoint.kind == StepKind::Jump) {
            // The jump takes over, carrying the rest of the route (the
            // reference `route` is moved from and then replaced).
            waypoints.erase(waypoints.begin());
            const float length = glm::length(grid_.delta(flat(transform.position), flat(waypoint.position)));
            // Surface to surface: a lifted body's arc is shaped by the ground, not its height.
            const float rise = std::abs(waypoint.position.y - (transform.position.y - mover.lift));
            const float apex =
                tuning.jumpArcBase + tuning.jumpArcPerUnit * length + tuning.jumpArcPerRise * rise;
            const float seconds = std::max(tuning.jumpMinimumSeconds, length / std::max(mover.speed, 0.1f));
            if (auto jump = jumpTo(entity, waypoint.position, apex, seconds, layer)) {
                begin(entity, std::move(*jump), std::move(route));
            } else {
                reset(mover);
                events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
            }
            return;
        }
        if (waypoint.kind == StepKind::Climb) {
            waypoints.erase(waypoints.begin());
            const GridPos to{floorCell(waypoint.position.x), floorCell(waypoint.position.z), layer};
            if (auto climb = climbTo(entity, to, waypoint.position)) {
                begin(entity, std::move(*climb), std::move(route));
            } else {
                reset(mover);
                events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
            }
            return;
        }
        const glm::vec2 to = grid_.delta(flat(transform.position), flat(waypoint.position));
        const float distance = glm::length(to);
        if (distance > std::max(budget, tuning.snapDistance)) {
            stepTowards(entity, to / distance * budget);
            if (!mover.collide) {
                transform.position.y =
                    glm::mix(transform.position.y, waypoint.position.y + mover.lift, budget / distance);
            }
            break;
        }
        stepTowards(entity, to);
        if (!mover.collide) {
            transform.position.y = waypoint.position.y + mover.lift;
        }
        if (glm::length(grid_.delta(flat(transform.position), flat(waypoint.position))) >=
            tuning.arriveDistance) {
            break; // blocked short of the waypoint
        }
        if (waypoint.layer >= 0) {
            transform.layer = waypoint.layer;
            if (!mover.collide) {
                collision_.moved(entity);
            }
        }
        budget -= distance;
        waypoints.erase(waypoints.begin());
    }
    const auto travelled = grid_.delta(flat(before), flat(transform.position));
    const float moved = glm::length(glm::vec3(travelled.x, transform.position.y - before.y, travelled.y));
    route.stuckTime = moved < step * tuning.stuckShare ? route.stuckTime + dt : 0;
    if (waypoints.empty()) {
        reset(mover);
        events_.push({WorldEvent::Kind::Arrived, entity, entt::null, {}});
    } else if (route.stuckTime > tuning.stuckSeconds) {
        reset(mover);
        events_.push({WorldEvent::Kind::Blocked, entity, entt::null, {}});
    }
}

void MovementSystem::update(double dt) {
    const auto delta = static_cast<float>(dt);
    for (const auto entity : registry_.view<Transform, Mover>()) {
        auto& mover = registry_.get<Mover>(entity);
        mover.moved = false;
        const float lift = mover.lift;
        easeLift(mover, delta);
        if (mover.lift != lift && mover.idle()) {
            // Rising or sinking in place is not a motion (the idle clip plays),
            // but the body is where its feet now rest.
            auto& transform = registry_.get<Transform>(entity);
            transform.position.y = restY(mover, flat(transform.position), transform.layer);
            collision_.moved(entity);
        }
        if (mover.airborne()) {
            mover.drive = glm::vec2(0); // steering waits until the body is back on its feet
            if (mover.as<Mover::Jump>()) {
                advanceJump(entity, delta);
            } else if (mover.as<Mover::Climb>()) {
                advanceClimb(entity, delta);
            } else {
                advanceLaunch(entity, delta);
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
                glm::length(grid_.delta(flat(before), flat(after))) < 0.1f * glm::length(drive)) {
                // Walking into a ladder climbs it (a lifted body flies past it).
                const bool alongX = std::abs(drive.x) >= std::abs(drive.y);
                const glm::ivec2 axis =
                    alongX ? glm::ivec2(drive.x > 0 ? 1 : -1, 0) : glm::ivec2(0, drive.y > 0 ? 1 : -1);
                const float share = std::abs(alongX ? drive.x : drive.y) / glm::length(drive);
                const auto from = cellOf(entity);
                const auto* ladder = share > 0.7f && mover.lift <= 0 ? grid_.ladderStep(from, axis) : nullptr;
                if (ladder) {
                    const auto to = from == ladder->foot() ? ladder->top() : ladder->foot();
                    const auto centre = cellCentre(to.x, to.z);
                    const glm::vec3 end{centre.x, grid_.heightAt(centre, to.layer), centre.y};
                    auto climb = mover.fly || std::as_const(grid_).at(to).walkable ? climbTo(entity, to, end)
                                                                                    : std::nullopt;
                    if (climb) {
                        // A route being followed waits on the ladder and goes on from its end.
                        std::optional<Mover::Route> route;
                        if (auto* walking = mover.as<Mover::Route>()) {
                            route = std::move(*walking);
                        }
                        begin(entity, std::move(*climb), std::move(route));
                    }
                }
            }
            continue;
        }
        if (mover.as<Mover::Route>()) {
            advanceRoute(entity, delta);
        }
    }
}

void MovementSystem::reportCells() {
    for (const auto entity : registry_.view<Transform, Mover>()) {
        auto& mover = registry_.get<Mover>(entity);
        const auto cell = cellOf(entity);
        if (mover.cell.layer < 0) {
            mover.cell = cell; // first sight: nothing was entered
            continue;
        }
        if (cell == mover.cell) {
            continue;
        }
        mover.cell = cell;
        events_.push({WorldEvent::Kind::CellEnter, entity, entt::null, {}, cell});
    }
}

void MovementSystem::resolveAttachment(entt::entity entity, int depth) {
    if (!resolved_.insert(entity).second) {
        return;
    }
    auto& transform = registry_.get<Transform>(entity);
    if (transform.parent == entt::null) {
        return;
    }
    if (!registry_.valid(transform.parent) || !registry_.all_of<Transform>(transform.parent)) {
        transform.parent = entt::null; // the parent was destroyed
        return;
    }
    if (depth < MaxAttachmentDepth) {
        resolveAttachment(transform.parent, depth + 1); // parents first
    }
    const auto& parent = registry_.get<Transform>(transform.parent);
    glm::vec3 base = parent.position, before = parent.previous;
    if (transform.ground) {
        // A blob shadow: on the surface under the parent, however high it flies.
        base.y = grid_.heightAt(flat(base), parent.layer);
        before.y = grid_.heightAt(flat(before), parent.layer);
    }
    transform.position = base + rotateYaw(transform.offset, parent.yaw);
    transform.previous = before + rotateYaw(transform.offset, parent.previousYaw);
    transform.layer = parent.layer;
    canonicalise(transform);
    collision_.moved(entity);
}

void MovementSystem::updateAttachments() {
    resolved_.clear();
    for (const auto entity : registry_.view<Transform>()) {
        if (registry_.get<Transform>(entity).parent != entt::null) {
            resolveAttachment(entity, 0);
        }
    }
}

void MovementSystem::restoreCells() {
    for (auto it = overrides_.rbegin(); it != overrides_.rend(); ++it) {
        if (grid_.contains(it->cell) && std::as_const(grid_).at(it->cell) == it->applied) {
            grid_.setCell(it->cell, it->original); // unless a script changed the cell since
        }
    }
    overrides_.clear();
}

// Riders that collide when walking keep colliding while carried: a solid
// entity (not on the same platform), a static box or terrain the rider
// cannot stand on stops the horizontal part of the carry, sliding along it
// where one axis is free; the vertical part always applies. `deck` and
// `half` are the platform's new top and half size: a rider still on the deck
// is supported by it whatever the grid says. Platforms and their decks are
// measured in plain coordinates: on a wrapping map a platform stays inside
// the canonical range and does not straddle the seam.
void MovementSystem::carry(entt::entity rider, glm::vec3 delta, std::span<const entt::entity> aboard,
                           glm::vec3 deck, glm::vec2 half) {
    auto& carried = registry_.get<Transform>(rider);
    const auto* mover = registry_.try_get<Mover>(rider);
    const auto* collider = registry_.try_get<Collider>(rider);
    const bool collides = mover ? mover->collide : collider && collider->solid;
    const auto body = bodyOf(rider);
    glm::vec3 applied(0, delta.y, 0);
    if (collides) {
        const auto allowed = [&](glm::vec3 move) {
            const auto to = carried.position + move;
            const bool onDeck = std::abs(to.x - deck.x) <= half.x && std::abs(to.z - deck.z) <= half.y;
            return !collision_.blocksMove(rider, carried.position, to, body.radius, body.height, aboard) &&
                   !collision_.blockedByBox(to, body.radius, body.height) &&
                   (onDeck || supported(flat(to), body, carried.layer));
        };
        for (const glm::vec3 move : {delta, glm::vec3(delta.x, delta.y, 0), glm::vec3(0, delta.y, delta.z)}) {
            if (allowed(move)) {
                applied = move;
                break;
            }
        }
    } else {
        applied = delta;
    }
    carried.position += applied;
    canonicalise(carried);
    collision_.moved(rider);
}

void MovementSystem::carryRiders() {
    std::erase_if(rides_, [&](const auto& entry) {
        return !registry_.valid(entry.first) || !registry_.all_of<Transform, Platform>(entry.first);
    });
    for (const auto platform : registry_.view<Transform, Platform>()) {
        auto [found, added] = rides_.try_emplace(platform);
        if (added) {
            found->second.last = registry_.get<Transform>(platform).position;
        }
    }
    restoreCells();
    for (auto& [platform, ride] : rides_) {
        const auto& transform = registry_.get<Transform>(platform);
        const glm::vec2 half = registry_.get<Platform>(platform).size * 0.5f;
        const auto delta = transform.position - ride.last;
        // This tick's riders: on the top and inside the footprint the platform
        // had when the tick began, measured where they stand now, so a mover
        // that stepped off during the tick is not displaced. Colliders come
        // from the collision hash around that footprint; movers without a
        // collider (not in the hash, and rare) from their own view.
        ride.riders.clear();
        const auto consider = [&](entt::entity entity) {
            const auto& rider = registry_.get<Transform>(entity);
            if (entity == platform || rider.parent != entt::null || rider.layer != transform.layer ||
                registry_.all_of<Platform>(entity)) {
                return;
            }
            if (const auto* mover = registry_.try_get<Mover>(entity); mover && mover->airborne()) {
                return;
            }
            const auto offset = rider.position - ride.last;
            if (std::abs(offset.x) <= half.x && std::abs(offset.z) <= half.y &&
                std::abs(offset.y) < PlatformBoardHeight) {
                ride.riders.push_back(entity);
            }
        };
        nearScratch_.clear();
        collision_.nearby(ride.last, std::max(half.x, half.y), nearScratch_);
        for (const auto entity : nearScratch_) {
            consider(entity);
        }
        for (const auto entity : registry_.view<Transform, Mover>(entt::exclude<Collider>)) {
            consider(entity);
        }
        ride.last = transform.position;
        // Cells whose centre the platform covers, within the grid, written
        // before its riders move so their footing is known.
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
                const auto& current = std::as_const(grid_).at(cell);
                CellOverride entry{cell, current, current};
                entry.applied.walkable = true;
                entry.applied.height = transform.position.y;
                entry.applied.ramp = Ramp::None;
                entry.applied.rise = 1;
                grid_.setCell(cell, entry.applied);
                overrides_.push_back(entry);
            }
        }
        aboard_.assign(ride.riders.begin(), ride.riders.end());
        aboard_.push_back(platform);
        for (const auto rider : ride.riders) {
            if (registry_.valid(rider)) {
                carry(rider, delta, aboard_, transform.position, half);
            }
        }
    }
}
} // namespace fe
