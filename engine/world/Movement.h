#pragma once
#include "engine/nav/Query.h"
#include "engine/world/Collision.h"
#include <map>
#include <optional>
#include <span>

namespace fe {
// Movers: path following, direct steering, jumps, ladder climbs and
// attachments. Movement collides against the grid, static boxes and solid
// entities, changes layers through Grid::traverseLink and climbs ladders
// through Grid::ladderStep, the rules path finding uses.
class MovementSystem {
public:
    MovementSystem(entt::registry& registry, Grid& grid, CollisionSystem& collision, EventQueue& events);

    // Walks to `target`. With `findPath`, plans an A* path to the target's
    // cell on `layer` (default: the mover's layer if walkable there, else the
    // walkable layer whose surface is nearest target.y), using the mover's
    // gap jumps and any ladders. No path, or a search stopped by its visit cap
    // before settling the target (Route::truncated, even when it found a
    // possibly longer route), stops the mover and emits `blocked` at once.
    void moveTo(entt::entity entity, glm::vec3 target, bool findPath, std::optional<int> layer);
    // Cells an A* search of moveTo may expand (default DefaultRouteVisits).
    void setRouteVisitLimit(int limit) { routeVisitLimit_ = limit; }
    // Follows a route as given (no planning). Each waypoint is reached by
    // walking, a jump arc or a ladder climb; `arrived` is emitted once, at the
    // end. Jumps emit `jump_start` and `landed`, climbs `climb_start` and
    // `climb_end`.
    void follow(entt::entity entity, std::vector<Waypoint> route);
    void stop(entt::entity entity);
    void jump(entt::entity entity, glm::vec3 target, float height, float duration, int layer);
    void place(entt::entity entity, glm::vec3 position, int layer, bool keepHeight);
    [[nodiscard]] bool canOccupy(entt::entity entity, glm::vec3 position, int layer) const;
    [[nodiscard]] bool moving(entt::entity entity) const;
    void update(double dt);
    void updateAttachments();
    // Moving platforms: call collectRiders() before update() and
    // carryRiders() after it. Riders are entities with a mover or a collider
    // (not jumping, climbing or attached) whose feet were on a platform's top
    // and inside its footprint; they move with it, and those that collide
    // when walking are still stopped by solid entities that are not aboard.
    // carryRiders() also moves the platforms' grid overrides to the cells
    // they now cover.
    void collectRiders();
    void carryRiders();

private:
    bool stepTowards(entt::entity entity, glm::vec2 delta);
    void startJump(entt::entity entity, glm::vec3 target, float height, float duration, int layer);
    // Starts climbing the ladder that joins the mover's cell to `to`, ending at
    // `end`; false when no ladder joins them.
    bool startClimb(entt::entity entity, GridPos to, glm::vec3 end);
    void finishClimb(entt::entity entity);
    void advanceJump(entt::entity entity, float dt);
    void advanceClimb(entt::entity entity, float dt);
    void advanceRoute(entt::entity entity, float dt);
    [[nodiscard]] GridPos cellOf(entt::entity entity) const;
    // Standing room on a layer; inside a link cell where both surfaces meet,
    // either layer's footprint counts.
    [[nodiscard]] bool supported(glm::vec2 position, float radius, int layer) const;
    [[nodiscard]] float bodyHeight(entt::entity entity) const;
    [[nodiscard]] int targetLayer(glm::vec3 target, int current) const;

    void restoreCells();
    void carry(entt::entity rider, glm::vec3 delta, std::span<const entt::entity> aboard);

    struct Ride {
        glm::vec3 last{0}; // platform position at the end of the last tick
        std::vector<entt::entity> riders;
    };
    // A grid cell a platform covers: the cell before and the cell as written.
    struct CellOverride {
        GridPos cell;
        GridCell original, applied;
    };

    entt::registry& registry_;
    Grid& grid_;
    CollisionSystem& collision_;
    EventQueue& events_;
    std::map<entt::entity, Ride> rides_; // ordered, so overlapping platforms resolve the same way every run
    std::vector<CellOverride> overrides_;
    std::vector<entt::entity> aboard_; // a platform and its riders, which never block each other
    int routeVisitLimit_ = DefaultRouteVisits;
};
} // namespace fe
