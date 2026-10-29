#include "engine/nav/Query.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace fe {
namespace {
struct Step {
    GridPos to;
    float cost = 0;
    StepKind kind = StepKind::Walk;
};

constexpr int Axes[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

// Highest point of a cell's surface.
float cellTop(const Grid& grid, GridPos p) {
    const auto& cell = grid.at(p);
    return cell.height + (cell.ramp == Ramp::None ? 0 : std::max(0.0f, cell.rise));
}

// Valid moves out of a cell. Layer changes happen on the way out of a link
// cell or on the way into one, exactly where Grid::traverseLink lets a mover
// switch, so planned paths are walkable.
class Neighbours {
public:
    Neighbours(const Grid& grid, const StepRules& rules) : grid_(grid), rules_(rules) {}

    void collect(GridPos from, std::vector<Step>& out) const {
        static constexpr int Directions[8][2] = {{0, -1}, {1, 0}, {0, 1},  {-1, 0},
                                                 {1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
        out.clear();
        const glm::vec2 centre(static_cast<float>(from.x) + 0.5f, static_cast<float>(from.z) + 0.5f);
        for (int d = 0; d < (rules_.diagonal ? 8 : 4); ++d) {
            const int dx = Directions[d][0];
            const int dz = Directions[d][1];
            const glm::vec2 heading(static_cast<float>(dx), static_cast<float>(dz));
            if (dx && dz &&
                !(cornerOpen(from, {from.x + dx, from.z, from.layer}, dx, 0) &&
                  cornerOpen(from, {from.x, from.z + dz, from.layer}, 0, dz))) {
                continue;
            }
            GridPos source = from;
            GridPos next{from.x + dx, from.z + dz, from.layer};
            const bool layerEnds = !grid_.contains(next) || !grid_.at(next).walkable;
            const int exitLayer = grid_.traverseLink(centre + heading * EdgeOffset, from.layer, heading,
                                                     rules_.maxUp, rules_.maxDown, layerEnds);
            if (exitLayer != from.layer) {
                source.layer = next.layer = exitLayer;
            } else if (!layerEnds) {
                next.layer = grid_.traverseLink(centre + heading * (1 - EdgeOffset), from.layer, heading,
                                                rules_.maxUp, rules_.maxDown, false);
            }
            if (!enterable(next) || !climbable(source, next, dx, dz)) {
                continue;
            }
            add(from, next, (dx && dz) ? std::numbers::sqrt2_v<float> : 1.0f, StepKind::Walk, out);
        }
        for (const auto& axis : Axes) {
            const glm::ivec2 step(axis[0], axis[1]);
            if (const auto* ladder = rules_.ladders ? grid_.ladderStep(from, step) : nullptr) {
                const auto end = from == ladder->foot() ? ladder->top() : ladder->foot();
                if (enterable(end)) {
                    add(from, end, 1, StepKind::Climb, out);
                }
            }
            jumps(from, step, out);
        }
        if (rules_.jump.diagonal) {
            for (int d = 4; d < 8; ++d) {
                jumps(from, {Directions[d][0], Directions[d][1]}, out);
            }
        }
    }

private:
    static constexpr float EdgeOffset = 0.499f;

    void add(GridPos from, GridPos to, float cost, StepKind kind, std::vector<Step>& out) const {
        for (const auto& [mask, extra] : rules_.tagCosts) {
            if (grid_.at(to).tags & mask) {
                cost += extra;
            }
        }
        if (rules_.cost) {
            const auto custom = rules_.cost(from, to, cost);
            if (!custom) {
                return;
            }
            if (!std::isfinite(*custom) || *custom < 0) {
                throw std::invalid_argument("A path cost function must return a finite, non-negative number");
            }
            cost = *custom;
        }
        out.push_back({to, cost, kind});
    }
    void jumps(GridPos from, glm::ivec2 step, std::vector<Step>& out) const {
        const auto& jump = rules_.jump;
        if (jump.distance <= 0) {
            return;
        }
        const float takeoff = grid_.edgeHeight(from, step.x, step.y);
        for (int gaps = 1; gaps <= jump.distance; ++gaps) {
            const GridPos gap{from.x + step.x * gaps, from.z + step.y * gaps, from.layer};
            const GridPos landing{gap.x + step.x, gap.z + step.y, from.layer};
            if (!grid_.contains(landing)) {
                return;
            }
            if (!enterable(landing)) {
                continue;
            }
            const float arrival = grid_.edgeHeight(landing, -step.x, -step.y);
            const float rise = arrival - takeoff;
            if (rise > jump.maxUp + 1e-4f || -rise > jump.maxDown + 1e-4f) {
                continue;
            }
            const float floor = std::min(takeoff, arrival);
            const bool diagonal = step.x != 0 && step.y != 0;
            bool clear = true;
            for (int i = 1; i <= gaps && clear; ++i) {
                clear = isGap({from.x + step.x * i, from.z + step.y * i, from.layer}, floor);
            }
            const float ceiling = std::max(takeoff, arrival);
            for (int i = 1; i <= gaps + 1 && clear && diagonal; ++i) {
                clear = clearAbove({from.x + step.x * i, from.z + step.y * (i - 1), from.layer}, ceiling) &&
                        clearAbove({from.x + step.x * (i - 1), from.z + step.y * i, from.layer}, ceiling);
            }
            if (clear) {
                const float length =
                    static_cast<float>(gaps + 1) * (diagonal ? std::numbers::sqrt2_v<float> : 1);
                add(from, landing, length, StepKind::Jump, out);
                return; // the nearest landing in a direction is the only one
            }
        }
    }
    // No surface of this layer or one below rises above `floor` here.
    [[nodiscard]] bool clearAbove(GridPos p, float floor) const {
        if (!grid_.contains(p)) {
            return true;
        }
        for (int l = 0; l <= p.layer; ++l) {
            const GridPos below{p.x, p.z, l};
            if ((l == 0 || grid_.at(below).walkable) && cellTop(grid_, below) > floor + 1e-4f) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool isGap(GridPos p, float floor) const {
        if (!clearAbove(p, floor) || rules_.blocked.contains(cellKey(p))) {
            return false;
        }
        const auto& cell = grid_.at(p);
        return !cell.walkable || (cell.tags & rules_.blockedTags) ||
               cellTop(grid_, p) < floor - std::min(rules_.maxUp, rules_.maxDown) - 1e-4f;
    }
    [[nodiscard]] bool enterable(GridPos p) const {
        if (!grid_.contains(p)) {
            return false;
        }
        const auto& cell = grid_.at(p);
        return cell.walkable && !(cell.tags & rules_.blockedTags) && !rules_.blocked.contains(cellKey(p));
    }
    [[nodiscard]] bool climbable(GridPos from, GridPos to, int dx, int dz) const {
        const float rise = grid_.edgeHeight(to, -dx, -dz) - grid_.edgeHeight(from, dx, dz);
        return rise <= rules_.maxUp + 1e-4f && -rise <= rules_.maxDown + 1e-4f;
    }
    // A diagonal step may not slip past a wall, a pillar or a drop.
    [[nodiscard]] bool cornerOpen(GridPos from, GridPos side, int dx, int dz) const {
        return enterable(side) && climbable(from, side, dx, dz);
    }

    const Grid& grid_;
    const StepRules& rules_;
};

// Visits the grid cells a 2D segment crosses, in order, with the segment
// parameters (0..1) where it enters and leaves each. Stops when `visit`
// returns false; returns false in that case.
template<class Visit> bool traceCells(glm::vec2 a, glm::vec2 b, Visit&& visit) {
    const glm::vec2 d = b - a;
    int x = floorCell(a.x);
    int z = floorCell(a.y);
    const int stepX = d.x > 0 ? 1 : (d.x < 0 ? -1 : 0);
    const int stepZ = d.y > 0 ? 1 : (d.y < 0 ? -1 : 0);
    constexpr float Infinity = std::numeric_limits<float>::infinity();
    float nextX = stepX > 0   ? (static_cast<float>(x + 1) - a.x) / d.x
                  : stepX < 0 ? (static_cast<float>(x) - a.x) / d.x
                              : Infinity;
    float nextZ = stepZ > 0   ? (static_cast<float>(z + 1) - a.y) / d.y
                  : stepZ < 0 ? (static_cast<float>(z) - a.y) / d.y
                              : Infinity;
    const float deltaX = stepX ? 1 / std::abs(d.x) : Infinity;
    const float deltaZ = stepZ ? 1 / std::abs(d.y) : Infinity;
    // Callers clip the segment to the grid first, so this is at most its
    // width plus depth; 64-bit, as saturated coordinates span 2^31.
    const std::int64_t cells = std::abs(static_cast<std::int64_t>(floorCell(b.x)) - x) +
                               std::abs(static_cast<std::int64_t>(floorCell(b.y)) - z) + 1;
    float t = 0;
    for (std::int64_t i = 0; i < cells; ++i) {
        const float exit = std::min({nextX, nextZ, 1.0f});
        if (!visit(x, z, t, exit)) {
            return false;
        }
        if (exit >= 1) {
            break;
        }
        if (nextX < nextZ) {
            x += stepX;
            t = nextX;
            nextX += deltaX;
        } else {
            z += stepZ;
            t = nextZ;
            nextZ += deltaZ;
        }
    }
    return true;
}

// Narrows the segment a..b (parameters s0..s1 of 0..1) to the part over the
// grid's footprint; false when it misses the grid. In double precision, so
// the clipped part of a segment a million units long is still exact to a
// small fraction of a cell.
bool clipToGrid(const Grid& grid, glm::dvec2 a, glm::dvec2 b, double& s0, double& s1) {
    const glm::dvec2 d = b - a;
    const double highs[2] = {static_cast<double>(grid.width()), static_cast<double>(grid.depth())};
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(d[axis]) < 1e-12) {
            if (a[axis] < 0 || a[axis] > highs[axis]) {
                return false;
            }
            continue;
        }
        double t0 = (0 - a[axis]) / d[axis], t1 = (highs[axis] - a[axis]) / d[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        s0 = std::max(s0, t0);
        s1 = std::min(s1, t1);
    }
    return s0 <= s1;
}

glm::ivec2 snapDirection(glm::vec2 direction) {
    if (glm::length(direction) < 1e-6f || !std::isfinite(direction.x) || !std::isfinite(direction.y)) {
        return {0, 1};
    }
    const float octant = std::round(std::atan2(direction.y, direction.x) / (std::numbers::pi_v<float> / 4));
    const float angle = octant * std::numbers::pi_v<float> / 4;
    return {static_cast<int>(std::round(std::cos(angle))), static_cast<int>(std::round(std::sin(angle)))};
}
} // namespace

void validateRules(const StepRules& rules) {
    if (!std::isfinite(rules.maxUp) || !std::isfinite(rules.maxDown) || rules.maxUp < 0 ||
        rules.maxDown < 0) {
        throw std::invalid_argument("Step limits must be finite and not negative");
    }
    for (const auto& [mask, cost] : rules.tagCosts) {
        if (!std::isfinite(cost) || cost < 0) {
            throw std::invalid_argument("Tag costs must be finite and not negative");
        }
    }
    const auto& jump = rules.jump;
    if (jump.distance < 0 || jump.distance > 16 || !std::isfinite(jump.maxUp) ||
        !std::isfinite(jump.maxDown) || jump.maxUp < 0 || jump.maxDown < 0) {
        throw std::invalid_argument("Jump distance must be 0..16 and jump limits finite and not negative");
    }
}

Route findRoute(const Grid& grid, GridPos from, GridPos to, const StepRules& rules, int maxVisited) {
    validateRules(rules);
    Route route;
    if (!grid.contains(from) || !grid.contains(to) || !grid.at(to).walkable) {
        return route;
    }
    if (from == to) {
        route.steps.push_back({to, StepKind::Walk});
        return route;
    }
    struct Node {
        float priority, cost;
        std::int64_t key;
    };
    struct Later {
        bool operator()(const Node& a, const Node& b) const {
            return a.priority > b.priority || (a.priority == b.priority && a.key > b.key);
        }
    };
    struct Visit {
        float cost;
        GridPos position, parent;
        StepKind kind;
    };
    std::priority_queue<Node, std::vector<Node>, Later> open;
    std::unordered_map<std::int64_t, Visit> visits;
    const auto heuristic = [&](GridPos p) {
        if (rules.cost) {
            return 0.0f;
        }
        const float dx = static_cast<float>(std::abs(p.x - to.x));
        const float dz = static_cast<float>(std::abs(p.z - to.z));
        return rules.diagonal ? std::max(dx, dz) + (std::numbers::sqrt2_v<float> - 1) * std::min(dx, dz)
                              : dx + dz;
    };
    const Neighbours neighbours(grid, rules);
    std::vector<Step> steps;
    const auto start = cellKey(from);
    const auto goal = cellKey(to);
    visits[start] = {0, from, from, StepKind::Walk};
    open.push({heuristic(from), 0, start});
    int visited = 0;
    while (!open.empty()) {
        if (visited++ >= maxVisited) {
            route.truncated = true;
            break;
        }
        const auto current = open.top();
        open.pop();
        if (current.key == goal) {
            break;
        }
        const auto here = visits.at(current.key);
        if (current.cost > here.cost + 1e-4f) {
            continue; // stale queue entry
        }
        if (current.key != start && rules.zoc.contains(current.key)) {
            continue; // movement ends in a zone of control
        }
        neighbours.collect(here.position, steps);
        for (const auto& step : steps) {
            const auto key = cellKey(step.to);
            const float cost = here.cost + step.cost;
            const auto found = visits.find(key);
            if (found != visits.end() && found->second.cost <= cost) {
                continue;
            }
            visits[key] = {cost, step.to, here.position, step.kind};
            open.push({cost + heuristic(step.to), cost, key});
        }
    }
    if (!visits.contains(goal)) {
        return route;
    }
    for (auto key = goal; key != start;) {
        const auto& visit = visits.at(key);
        route.steps.push_back({visit.position, visit.kind});
        key = cellKey(visit.parent);
    }
    std::reverse(route.steps.begin(), route.steps.end());
    return route;
}

std::vector<GridPos> findPath(const Grid& grid, GridPos from, GridPos to, const StepRules& rules,
                              int maxVisited) {
    std::vector<GridPos> cells;
    for (const auto& step : findRoute(grid, from, to, rules, maxVisited).steps) {
        cells.push_back(step.cell);
    }
    return cells;
}

std::vector<ReachCell> reachable(const Grid& grid, GridPos start, float budget, const StepRules& rules) {
    validateRules(rules);
    std::vector<ReachCell> cells;
    if (!grid.contains(start) || !std::isfinite(budget) || budget < 0) {
        return cells;
    }
    std::unordered_map<std::int64_t, int> indexOf;
    using Entry = std::pair<float, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    const Neighbours neighbours(grid, rules);
    std::vector<Step> steps;
    cells.push_back({start, 0, -1, true});
    indexOf[cellKey(start)] = 0;
    open.push({0, 0});
    while (!open.empty()) {
        const auto [cost, index] = open.top();
        open.pop();
        if (cost > cells[static_cast<std::size_t>(index)].cost + 1e-5f) {
            continue;
        }
        if (index > 0 && rules.zoc.contains(cellKey(cells[static_cast<std::size_t>(index)].pos))) {
            continue; // movement ends in a zone of control
        }
        neighbours.collect(cells[static_cast<std::size_t>(index)].pos, steps);
        for (const auto& step : steps) {
            const float next = cost + step.cost;
            if (next > budget + 1e-5f) {
                continue;
            }
            const auto key = cellKey(step.to);
            const auto found = indexOf.find(key);
            if (found != indexOf.end()) {
                auto& existing = cells[static_cast<std::size_t>(found->second)];
                if (existing.cost <= next) {
                    continue;
                }
                existing.cost = next;
                existing.parent = index;
                existing.kind = step.kind;
                open.push({next, found->second});
                continue;
            }
            const int added = static_cast<int>(cells.size());
            cells.push_back({step.to, next, index, !rules.noStop.contains(key), step.kind});
            indexOf[key] = added;
            open.push({next, added});
        }
    }
    return cells;
}

std::vector<GridPos> reachPath(const std::vector<ReachCell>& cells, GridPos target) {
    const auto found =
        std::find_if(cells.begin(), cells.end(), [&](const ReachCell& c) { return c.pos == target; });
    if (found == cells.end()) {
        return {};
    }
    std::vector<GridPos> path;
    for (int index = static_cast<int>(found - cells.begin()); index > 0;
         index = cells[static_cast<std::size_t>(index)].parent) {
        path.push_back(cells[static_cast<std::size_t>(index)].pos);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

bool lineOfSight(const Grid& grid, glm::vec3 from, glm::vec3 to, int layer, std::uint32_t blockingTags) {
    constexpr float Epsilon = 1e-3f;
    if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z) || !std::isfinite(to.x) ||
        !std::isfinite(to.y) || !std::isfinite(to.z)) {
        return false;
    }
    const int startX = floorCell(from.x);
    const int startZ = floorCell(from.z);
    // Only the part of the segment over the grid can be blocked, so the trace
    // visits at most the grid's width plus depth cells however long it is.
    const glm::dvec3 origin(from), delta = glm::dvec3(to) - origin;
    double s0 = 0, s1 = 1;
    if (!clipToGrid(grid, {origin.x, origin.z}, {origin.x + delta.x, origin.z + delta.z}, s0, s1)) {
        return true;
    }
    const glm::vec3 start(origin + delta * s0);
    const glm::vec3 end(origin + delta * s1);
    return traceCells({start.x, start.z}, {end.x, end.z}, [&](int x, int z, float enter, float exit) {
        if (!grid.contains(x, z)) {
            return true;
        }
        const auto a = glm::mix(start, end, enter);
        const auto b = glm::mix(start, end, exit);
        for (int l = 0; l < grid.layerCount(); ++l) {
            const auto& cell = grid.at(x, z, l);
            if ((x == startX && z == startZ && l == layer) || (l > 0 && !cell.walkable)) {
                continue;
            }
            if (cell.tags & blockingTags) {
                return false;
            }
            const float above0 = a.y - grid.heightIn({x, z, l}, {a.x, a.z});
            const float above1 = b.y - grid.heightIn({x, z, l}, {b.x, b.z});
            const bool blocked =
                l == 0 ? (above0 < -Epsilon || above1 < -Epsilon)
                       : ((above0 > Epsilon && above1 < -Epsilon) || (above0 < -Epsilon && above1 > Epsilon));
            if (blocked) {
                return false;
            }
        }
        return true;
    });
}

std::optional<AreaShape> parseAreaShape(const std::string& name) {
    if (name == "diamond") {
        return AreaShape::Diamond;
    }
    if (name == "square") {
        return AreaShape::Square;
    }
    if (name == "cross") {
        return AreaShape::Cross;
    }
    if (name == "line") {
        return AreaShape::Line;
    }
    if (name == "cone") {
        return AreaShape::Cone;
    }
    if (name == "ring") {
        return AreaShape::Ring;
    }
    return std::nullopt;
}

std::vector<glm::ivec2> areaOffsets(AreaShape shape, int minRange, int maxRange, glm::vec2 heading) {
    std::vector<glm::ivec2> offsets;
    minRange = std::max(0, minRange);
    maxRange = std::clamp(maxRange, minRange, 64);
    const auto direction = snapDirection(heading);
    const bool diagonal = direction.x != 0 && direction.y != 0;
    const glm::ivec2 side(-direction.y, direction.x);
    for (int z = -maxRange; z <= maxRange; ++z) {
        for (int x = -maxRange; x <= maxRange; ++x) {
            const int manhattan = std::abs(x) + std::abs(z);
            const int chebyshev = std::max(std::abs(x), std::abs(z));
            // Steps along the direction and across it; for diagonals both
            // count whole diagonal steps.
            const int forward =
                diagonal ? std::min(x * direction.x, z * direction.y) : x * direction.x + z * direction.y;
            const int lateral = diagonal ? x * direction.x - z * direction.y : x * side.x + z * side.y;
            const int reach = diagonal ? chebyshev : forward;
            bool inside = false;
            switch (shape) {
            case AreaShape::Diamond:
                inside = manhattan >= minRange && manhattan <= maxRange;
                break;
            case AreaShape::Ring:
                inside = manhattan >= std::max(1, minRange) && manhattan <= maxRange;
                break;
            case AreaShape::Square:
                inside = chebyshev >= minRange && chebyshev <= maxRange;
                break;
            case AreaShape::Cross:
                inside = (x == 0 || z == 0) && manhattan >= minRange && manhattan <= maxRange;
                break;
            case AreaShape::Line:
                inside = lateral == 0 && forward >= std::max(minRange, 1) && forward <= maxRange;
                break;
            case AreaShape::Cone:
                inside = forward >= 1 && reach >= std::max(minRange, 1) && reach <= maxRange &&
                         (diagonal || std::abs(lateral) < forward);
                break;
            }
            if (inside) {
                offsets.emplace_back(x, z);
            }
        }
    }
    return offsets;
}

std::optional<GridHit> raycastGrid(const Grid& grid, glm::vec3 origin, glm::vec3 direction, float maxDistance,
                                   std::uint32_t layers) {
    if (glm::length(direction) < 1e-6f || !std::isfinite(maxDistance)) {
        return std::nullopt;
    }
    direction = glm::normalize(direction);
    // Clip the ray to the grid's footprint.
    float near = 0, far = maxDistance;
    const float lows[2] = {0, 0};
    const float highs[2] = {static_cast<float>(grid.width()), static_cast<float>(grid.depth())};
    const float starts[2] = {origin.x, origin.z};
    const float steps[2] = {direction.x, direction.z};
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(steps[axis]) < 1e-9f) {
            if (starts[axis] < lows[axis] || starts[axis] >= highs[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float a = (lows[axis] - starts[axis]) / steps[axis];
        float b = (highs[axis] - starts[axis]) / steps[axis];
        if (a > b) {
            std::swap(a, b);
        }
        near = std::max(near, a);
        far = std::min(far, b);
    }
    constexpr float Nudge = 1e-4f;
    if (far - near <= 2 * Nudge) {
        return std::nullopt;
    }
    const auto from = origin + direction * (near + Nudge);
    const auto to = origin + direction * (far - Nudge);
    std::optional<GridHit> hit;
    traceCells({from.x, from.z}, {to.x, to.z}, [&](int x, int z, float enter, float exit) {
        if (!grid.contains(x, z)) {
            return true;
        }
        const auto a = glm::mix(from, to, enter);
        const auto b = glm::mix(from, to, exit);
        float best = std::numeric_limits<float>::infinity();
        for (int l = 0; l < grid.layerCount(); ++l) {
            if ((l > 0 && !grid.at(x, z, l).walkable) || !(layers & (1u << static_cast<unsigned>(l)))) {
                continue; // empty upper-layer cells are open air
            }
            const float above0 = a.y - grid.heightIn({x, z, l}, {a.x, a.z});
            const float above1 = b.y - grid.heightIn({x, z, l}, {b.x, b.z});
            float at = 0;
            if (l == 0 && above0 < 0) {
                at = 0; // the side face of a raised cell (or the ray starts underground)
            } else if ((above0 >= 0) != (above1 >= 0) || above1 == 0) {
                at = above0 == above1 ? 0 : above0 / (above0 - above1);
            } else {
                continue;
            }
            if (at < best) {
                best = at;
                hit = GridHit{{x, z, l}, glm::mix(a, b, at)};
            }
        }
        return !hit;
    });
    return hit;
}
} // namespace fe
