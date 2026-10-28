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

// Movement rules shared by path finding and reachability.
struct StepRules {
    bool diagonal = false;
    float maxUp = 0.6f;                                    // largest climb between adjacent cells
    float maxDown = 0.6f;                                  // largest drop
    std::uint32_t blockedTags = 0;                         // cells with any of these tags cannot be entered
    std::vector<std::pair<std::uint32_t, float>> tagCosts; // extra entry cost per tag
    std::unordered_set<std::int64_t> blocked;              // occupied cells that cannot be entered
    std::unordered_set<std::int64_t> noStop;               // can be crossed but not ended on (allies)
};

std::vector<GridPos> findPath(const Grid& grid, GridPos from, GridPos to, const StepRules& rules,
                              int maxVisited = 200000);

struct ReachCell {
    GridPos pos;
    float cost = 0;
    int parent = -1; // index into the result, -1 for the start
    bool stoppable = true;
};
// Every cell reachable within `budget` movement cost (Dijkstra).
std::vector<ReachCell> reachable(const Grid& grid, GridPos start, float budget, const StepRules& rules);
std::vector<GridPos> reachPath(const std::vector<ReachCell>& cells, GridPos target);

// True when nothing in the grid rises above the segment between two points.
bool lineOfSight(const Grid& grid, glm::vec3 from, glm::vec3 to, int layer, std::uint32_t blockingTags = 0);

enum class AreaShape { Diamond, Square, Cross, Line, Cone, Ring };
std::optional<AreaShape> parseAreaShape(const std::string& name);
// Cell offsets for a shape. `direction` matters for Line and Cone.
std::vector<glm::ivec2> areaOffsets(AreaShape shape, int minRange, int maxRange,
                                    glm::ivec2 direction = {0, 1});

struct GridHit {
    GridPos cell;
    glm::vec3 point{0};
};
std::optional<GridHit> raycastGrid(const Grid& grid, glm::vec3 origin, glm::vec3 direction,
                                   float maxDistance = 200);
} // namespace fe
