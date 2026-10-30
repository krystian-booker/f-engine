#pragma once
#include "engine/nav/Query.h"
#include "engine/world/Collision.h"
#include <map>
#include <optional>
#include <span>
#include <unordered_set>

namespace fe {
// The movers' thresholds and the shape of route jumps, with the engine's
// defaults. Games tune them through World::movementTuning().
struct MovementTuning {
    float arriveDistance = 0.05f; // a waypoint counts as reached within this; stopping short of it is a block
    float snapDistance = 0.02f;   // a waypoint this close is reached even when the tick's step is shorter
    float stuckShare = 0.1f;      // a tick covering less than this share of its step counts as stuck
    double stuckSeconds = 1.0;    // stuck this long on a route: `blocked`
    // A route jump's apex: base + perUnit * horizontal length + perRise * |height difference|.
    float jumpArcBase = 0.4f, jumpArcPerUnit = 0.2f, jumpArcPerRise = 0.5f;
    float jumpMinimumSeconds = 0.3f; // shortest route jump
    float landingSearch = 1.0f;      // how far around a blocked landing the nearest free spot is looked for
    float gravity = 20.0f;           // units/second^2, for launch()
};

// Movers: path following, direct steering, jumps, ladder climbs, platforms
// and attachments. Movement collides against the grid, static boxes and
// solid entities, changes layers through Grid::traverseLink and climbs
// ladders through Grid::ladderStep, the rules path finding uses.
//
// A mover is in one motion at a time (Mover::motion): none, a route, a jump,
// a climb or a launch. Every command (moveTo, follow, stop, jump, launch,
// place) first ends a jump, launch or climb in progress: a jump lands at once
// at the ground under the body (or, over a void, at the nearer end of its
// arc), a launch on the highest standable surface under the body (else the
// lowest layer's), both emitting `landed`; a climb steps off at the nearer
// end of the ladder and emits `climb_end`. No command leaves a body in
// mid-air or on a ladder.
class MovementSystem {
public:
    MovementSystem(entt::registry& registry, Grid& grid, CollisionSystem& collision, EventQueue& events);

    MovementTuning tuning;

    // Walks to `target`. With `findPath`, plans an A* path to the target's
    // cell on `layer` (default: the mover's layer if walkable there, else the
    // walkable layer whose surface is nearest target.y). Without `rules` the
    // plan uses the mover's step limit and gap jumps, diagonal steps and any
    // ladders; with `rules` those rules plan the route as given. The mover's
    // footprint and fly always apply. No path, or a search stopped by its
    // visit cap before settling the target (Route::truncated, even when it
    // found a possibly longer route), stops the mover and emits `blocked` at
    // once. Throws std::runtime_error for a target outside the grid.
    void moveTo(entt::entity entity, glm::vec3 target, bool findPath, std::optional<int> layer,
                const StepRules* rules = nullptr);
    // Cells an A* search of moveTo may expand (default DefaultRouteVisits).
    void setRouteVisitLimit(int limit) { routeVisitLimit_ = limit; }
    // Follows a route as given (no planning). Each waypoint is reached by
    // walking, a jump arc or a ladder climb; `arrived` is emitted once, at the
    // end. Jumps emit `jump_start` and `landed`, climbs `climb_start` and
    // `climb_end`.
    void follow(entt::entity entity, std::vector<Waypoint> route);
    void stop(entt::entity entity);
    // A jump arc to `target`. A landing inside a static box or a solid entity
    // moves to the nearest free spot within tuning.landingSearch; with none,
    // the jump is refused with `blocked`.
    void jump(entt::entity entity, glm::vec3 target, float height, float duration, int layer);
    // A ballistic flight from where the body is: `velocity` (units/second)
    // under tuning.gravity. The horizontal part holds until a solid body, a
    // static box, a cliff face (ground higher than the body) or the grid's
    // edge stops it; the body then drops straight down. It lands on the first
    // surface it falls onto that it may stand on (any layer, by its tags, fly
    // and footprint), with a jump's landing validation at that moment: a
    // landing inside a box or a solid body moves to the nearest free spot
    // within tuning.landingSearch, and with none it lands there anyway and
    // `blocked` follows `landed`. With no standable surface under it, it
    // lands on the lowest layer's surface. Emits `jump_start` and `landed`.
    // A launch during a jump or launch replaces it from where the body is
    // (no event for the one replaced); std::runtime_error during a climb. A
    // zero velocity above the ground is a fall.
    void launch(entt::entity entity, glm::vec3 velocity);
    void place(entt::entity entity, glm::vec3 position, int layer, bool keepHeight);
    [[nodiscard]] bool canOccupy(entt::entity entity, glm::vec3 position, int layer) const;
    [[nodiscard]] bool moving(entt::entity entity) const;
    void update(double dt);
    // Moving platforms: call after update(). Riders are entities with a mover
    // or a collider (not jumping, climbing or attached) standing on a
    // platform's top and inside its footprint as this tick began; they move
    // with it. Those that collide when walking are still stopped by solid
    // entities that are not aboard, by static boxes and by terrain they
    // cannot stand on. Also moves the platforms' grid overrides to the cells
    // they now cover.
    void carryRiders();
    // Children follow their parents (offset turned by the parent's yaw);
    // parents are placed before their children whatever the entity order.
    void updateAttachments();
    // Emits cell_enter for every mover whose cell (x, z, layer) changed since
    // the last report; a mover's first report never emits.
    void reportCells();

private:
    // A mover's or collider's collision shape.
    struct Body {
        float radius = 0.2f, height = 1.6f;
        int footprint = 1;
        bool fly = false, collide = true;
        std::uint32_t blockedTags = 0, requiredTags = 0;
    };
    // A surface a falling body may come to rest on.
    struct Surface {
        float height = 0;
        int layer = 0;
    };
    [[nodiscard]] Body bodyOf(entt::entity entity) const;
    [[nodiscard]] GridPos cellOf(entt::entity entity) const;
    // Standing room on a layer; inside a link cell whose surfaces meet (at
    // the junction edge of a directed link, under the body otherwise), either
    // layer's footprint counts.
    [[nodiscard]] bool supported(glm::vec2 position, const Body& body, int layer) const;
    [[nodiscard]] bool freeOfSolids(entt::entity entity, glm::vec3 position, const Body& body) const;
    [[nodiscard]] int targetLayer(glm::vec3 target, int current, const Body& body) const;
    // The landing spot for a jump to `target`: `target` when free, else the
    // nearest free standing spot around it, else nothing.
    [[nodiscard]] std::optional<glm::vec3> landingFor(entt::entity entity, glm::vec3 target, int layer) const;

    bool stepTowards(entt::entity entity, glm::vec2 delta);
    bool startJump(entt::entity entity, glm::vec3 target, float height, float duration, int layer);
    // Starts climbing the ladder that joins the mover's cell to `to`, ending at
    // `end`; false when no ladder joins them.
    bool startClimb(entt::entity entity, GridPos to, glm::vec3 end);
    // Ends the jump, launch or climb in progress, if any (see the class comment).
    void settle(entt::entity entity);
    void finishJump(entt::entity entity, bool interrupted);
    void finishClimb(entt::entity entity, bool interrupted);
    // Ends a launch on `surface` under the body's position: the landing
    // validation, `landed` (and `blocked` when the spot is not free).
    void finishLaunch(entt::entity entity, Surface surface);
    // The highest standable surface at or below `height` at `position`, else
    // the lowest layer's surface (the floor, which is never fallen through).
    [[nodiscard]] Surface groundUnder(glm::vec2 position, const Body& body, float height) const;
    // What follows a finished jump or climb: the rest of the route, `arrived`, or nothing.
    void resume(entt::entity entity);
    static void reset(Mover& mover);
    void advanceJump(entt::entity entity, float dt);
    void advanceLaunch(entt::entity entity, float dt);
    void advanceClimb(entt::entity entity, float dt);
    void advanceRoute(entt::entity entity, float dt);
    void resolveAttachment(entt::entity entity, int depth);

    void restoreCells();
    void carry(entt::entity rider, glm::vec3 delta, std::span<const entt::entity> aboard, glm::vec3 deck,
               glm::vec2 half);

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
    std::vector<entt::entity> nearScratch_; // carryRiders() query results
    std::unordered_set<entt::entity> resolved_; // attachments placed this tick
    int routeVisitLimit_ = DefaultRouteVisits;
};
} // namespace fe
