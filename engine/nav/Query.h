#pragma once
#include "engine/nav/Grid.h"
#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

namespace fe {
[[nodiscard]] inline std::int64_t cellKey(GridPos p) {
    return (static_cast<std::int64_t>(p.layer) << 40) | (static_cast<std::int64_t>(p.z) << 20) |
           static_cast<std::int64_t>(p.x);
}

// How a step enters its cell: walking (including ramps and step links), a
// jump across a gap, or a climb along a ladder link.
enum class StepKind : std::uint8_t { Walk, Jump, Climb };

// Horizontal gap jumps. Along one of the four axes (and with `diagonal` the
// four diagonals too) an agent may jump over 1..`distance` gap cells to an
// enterable cell on the same layer. A gap cell is unwalkable, carries a
// blocked tag, or is walkable but lower than both ends by more than the
// walking step limits allow; it must not rise above the lower end, nor may any
// lower layer's surface there, and it must not be occupied (`blocked`). A
// diagonal jump also passes the corners of the cells beside its line, which
// must not rise above the higher end. The landing may be at most maxUp
// above and maxDown below the take-off edge. A jump over n cells costs n + 1
// (times sqrt(2) diagonally) plus the landing's tag costs.
struct JumpRules {
    int distance = 0; // 0 = no gap jumps
    float maxUp = 0, maxDown = 0;
    bool diagonal = false;
};

// Largest body footprint (cells per side) the planner and movers accept.
inline constexpr int MaxFootprint = 8;

// Movement rules shared by path finding and reachability. Layer changes
// follow Grid::traverseLink and ladders Grid::ladderStep, the rules movers use.
struct StepRules {
    bool diagonal = false;
    float maxUp = 0.6f;                                    // largest climb between adjacent cells
    float maxDown = 0.6f;                                  // largest drop
    std::uint32_t blockedTags = 0;                         // cells with any of these tags cannot be entered
    std::uint32_t requiredTags = 0;                        // when not zero: cells with none of them cannot be entered
    std::vector<std::pair<std::uint32_t, float>> tagCosts; // extra entry cost per tag (finite, >= 0)
    std::unordered_set<std::int64_t> blocked;              // occupied cells that cannot be entered
    std::unordered_set<std::int64_t> noStop;               // can be crossed but not ended on (allies)
    // Zones of control: these cells can be entered (at their normal cost) but
    // no step leads out of them, except out of the start cell.
    std::unordered_set<std::int64_t> zoc;
    JumpRules jump;
    bool ladders = true; // false: ladder links are not climbed
    // The body's size in cells per side: a square anchored at the unit's cell
    // and extending towards +x and +z. Every cell of it must be enterable
    // (walkable unless `fly`, free of blocked tags and blocked cells); height
    // steps are measured at the anchor cell. Units wider than one cell do not
    // climb ladders.
    int footprint = 1;
    // Flying: walkability and height steps are ignored (chasms and water can
    // be crossed and ended on); grid bounds, blocked tags and blocked cells
    // still hold.
    bool fly = false;
    // Called for every candidate step with its base cost (distance plus tag
    // costs); returns the cost to use, or nullopt when the step is impossible.
    // Results must be finite and non-negative (std::invalid_argument otherwise).
    // With a cost function findPath searches without a distance estimate, so
    // its result stays the cheapest route whatever the costs are.
    std::function<std::optional<float>(GridPos from, GridPos to, float base)> cost;
};

// Throws std::invalid_argument for negative or non-finite step limits and tag costs.
void validateRules(const StepRules& rules);

struct PathStep {
    GridPos cell;
    StepKind kind = StepKind::Walk;
};
// A planned route. `truncated`: the search expanded `maxVisited` cells before
// settling the goal, so `steps` is empty or a route that may not be the cheapest.
struct Route {
    std::vector<PathStep> steps;
    bool truncated = false;
};
// A* from one cell to another, excluding the start. Ties between equal-cost
// cells break on the cell key, so results do not depend on the standard library.
// A goal that cannot be entered under `rules` (unwalkable, a blocked or
// missing tag, a blocked cell, outside the grid) returns an empty route
// without searching, and so does a goal in another connected component of the
// grid (Grid::component) when the rules neither fly nor jump gaps, the two
// ways a route can leave its component. `maxVisited` counts expanded cells;
// stale queue entries do not count.
inline constexpr int DefaultRouteVisits = 200000;
Route findRoute(const Grid& grid, GridPos from, GridPos to, const StepRules& rules,
                int maxVisited = DefaultRouteVisits);
// findRoute without the step kinds.
std::vector<GridPos> findPath(const Grid& grid, GridPos from, GridPos to, const StepRules& rules,
                              int maxVisited = DefaultRouteVisits);

struct ReachCell {
    GridPos pos;
    float cost = 0;
    int parent = -1; // index into the result, -1 for the start
    bool stoppable = true;
    StepKind kind = StepKind::Walk; // how the step from `parent` enters this cell
};
// Every cell reachable within `budget` movement cost (Dijkstra; ties break on
// discovery order, which is deterministic). The search expands at most
// `maxVisited` cells (a zero-cost step function would otherwise flood the
// grid); `truncated` says it stopped there, so `cells` may be incomplete.
struct Reach {
    std::vector<ReachCell> cells;
    bool truncated = false;
};
Reach findReachable(const Grid& grid, GridPos start, float budget, const StepRules& rules,
                    int maxVisited = DefaultRouteVisits);
// findReachable's cells without the truncation flag.
std::vector<ReachCell> reachable(const Grid& grid, GridPos start, float budget, const StepRules& rules,
                                 int maxVisited = DefaultRouteVisits);
std::vector<GridPos> reachPath(const std::vector<ReachCell>& cells, GridPos target);

// True when no terrain blocks the segment between two points. The segment is
// traced cell by cell: ground (layer 0) is solid below its surface, walkable
// upper-layer cells (bridge decks) block a segment that passes through their
// surface, and cells carrying `blockingTags` on any of those layers block.
// The shooter's own cell (its xz cell on `layer`) is ignored.
bool lineOfSight(const Grid& grid, glm::vec3 from, glm::vec3 to, int layer, std::uint32_t blockingTags = 0);

enum class AreaShape { Diamond, Square, Cross, Line, Cone, Ring };
std::optional<AreaShape> parseAreaShape(const std::string& name);
// Cell offsets for a shape. `direction` matters for Line and Cone and snaps
// to the nearest of the eight grid directions; diagonal lines run along the
// diagonal and diagonal cones fill the quadrant between the two axes.
std::vector<glm::ivec2> areaOffsets(AreaShape shape, int minRange, int maxRange,
                                    glm::vec2 direction = {0, 1});

struct GridHit {
    GridPos cell;
    glm::vec3 point{0};
};
// First grid surface a ray hits: cell tops, the side faces of raised ground
// cells (cliffs block the ray) and walkable upper-layer surfaces. Only layers
// whose bit is set in `layers` are hit.
std::optional<GridHit> raycastGrid(const Grid& grid, glm::vec3 origin, glm::vec3 direction,
                                   float maxDistance = 200, std::uint32_t layers = ~0u);
} // namespace fe
