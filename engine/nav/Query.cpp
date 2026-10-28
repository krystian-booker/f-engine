#include "engine/nav/Query.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace fe {
namespace {
struct Step {
    GridPos to;
    float cost = 0;
};

// Valid moves out of a cell, including layer changes through links.
std::vector<Step> neighbours(const Grid& grid, GridPos from, const StepRules& rules) {
    static constexpr int Directions[8][2] = {{0, -1}, {1, 0}, {0, 1},  {-1, 0},
                                             {1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
    std::vector<Step> steps;
    const int count = rules.diagonal ? 8 : 4;
    auto enterable = [&](GridPos p) {
        if (!grid.contains(p)) {
            return false;
        }
        const auto& cell = grid.at(p);
        return cell.walkable && !(cell.tags & rules.blockedTags) && !rules.blocked.contains(cellKey(p));
    };
    for (int d = 0; d < count; ++d) {
        const int dx = Directions[d][0];
        const int dz = Directions[d][1];
        GridPos next{from.x + dx, from.z + dz, from.layer};
        if (!enterable(next)) {
            continue;
        }
        if (dx && dz) {
            // No corner cutting past walls.
            if (!enterable({from.x + dx, from.z, from.layer}) ||
                !enterable({from.x, from.z + dz, from.layer})) {
                continue;
            }
        }
        const float rise = grid.edgeHeight(next, -dx, -dz) - grid.edgeHeight(from, dx, dz);
        if (rise > rules.maxUp + 1e-4f || -rise > rules.maxDown + 1e-4f) {
            continue;
        }
        float cost = (dx && dz) ? 1.41421356f : 1.0f;
        for (const auto& [mask, extra] : rules.tagCosts) {
            if (grid.at(next).tags & mask) {
                cost += extra;
            }
        }
        steps.push_back({next, cost});
    }
    if (const auto other = grid.linkedLayer(from.x, from.z, from.layer)) {
        const GridPos next{from.x, from.z, *other};
        if (enterable(next)) {
            steps.push_back({next, 0.5f});
        }
    }
    return steps;
}
} // namespace

std::vector<GridPos> findPath(const Grid& grid, GridPos from, GridPos to, const StepRules& rules,
                              int maxVisited) {
    if (!grid.contains(from) || !grid.contains(to) || !grid.at(to).walkable) {
        return {};
    }
    if (from == to) {
        return {to};
    }
    struct Node {
        float priority;
        std::int64_t key;
        bool operator>(const Node& other) const { return priority > other.priority; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<>> open;
    std::unordered_map<std::int64_t, float> best;
    std::unordered_map<std::int64_t, GridPos> parent;
    std::unordered_map<std::int64_t, GridPos> position;
    const auto heuristic = [&](GridPos p) {
        const float dx = static_cast<float>(std::abs(p.x - to.x));
        const float dz = static_cast<float>(std::abs(p.z - to.z));
        return rules.diagonal ? std::max(dx, dz) + 0.41421356f * std::min(dx, dz) : dx + dz;
    };
    const auto start = cellKey(from);
    best[start] = 0;
    position[start] = from;
    open.push({heuristic(from), start});
    int visited = 0;
    const auto goal = cellKey(to);
    while (!open.empty() && visited++ < maxVisited) {
        const auto current = open.top();
        open.pop();
        if (current.key == goal) {
            break;
        }
        const auto here = position[current.key];
        const float cost = best[current.key];
        if (current.priority - heuristic(here) > cost + 1e-4f) {
            continue; // stale queue entry
        }
        for (const auto& step : neighbours(grid, here, rules)) {
            const auto key = cellKey(step.to);
            const float next = cost + step.cost;
            const auto found = best.find(key);
            if (found != best.end() && found->second <= next) {
                continue;
            }
            best[key] = next;
            parent[key] = here;
            position[key] = step.to;
            open.push({next + heuristic(step.to), key});
        }
    }
    if (!best.contains(goal)) {
        return {};
    }
    std::vector<GridPos> path{to};
    for (auto key = goal; key != start;) {
        const auto previous = parent.at(key);
        if (previous == from) {
            break;
        }
        path.push_back(previous);
        key = cellKey(previous);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<ReachCell> reachable(const Grid& grid, GridPos start, float budget, const StepRules& rules) {
    std::vector<ReachCell> cells;
    if (!grid.contains(start) || !std::isfinite(budget) || budget < 0) {
        return cells;
    }
    std::unordered_map<std::int64_t, int> indexOf;
    using Entry = std::pair<float, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    cells.push_back({start, 0, -1, true});
    indexOf[cellKey(start)] = 0;
    open.push({0, 0});
    while (!open.empty()) {
        const auto [cost, index] = open.top();
        open.pop();
        if (cost > cells[static_cast<std::size_t>(index)].cost + 1e-5f) {
            continue;
        }
        const auto here = cells[static_cast<std::size_t>(index)].pos;
        for (const auto& step : neighbours(grid, here, rules)) {
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
                open.push({next, found->second});
                continue;
            }
            const int added = static_cast<int>(cells.size());
            cells.push_back({step.to, next, index, !rules.noStop.contains(key)});
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
    const auto delta = to - from;
    const float distance = glm::length(glm::vec2(delta.x, delta.z));
    const int samples = std::max(1, static_cast<int>(std::ceil(distance / 0.2f)));
    for (int i = 1; i < samples; ++i) {
        const auto p = from + delta * (static_cast<float>(i) / static_cast<float>(samples));
        const int x = static_cast<int>(std::floor(p.x));
        const int z = static_cast<int>(std::floor(p.z));
        if (!grid.contains(x, z, layer)) {
            continue;
        }
        const auto& cell = grid.at(x, z, layer);
        if ((cell.tags & blockingTags) || grid.heightAt({p.x, p.z}, layer) > p.y + 1e-3f) {
            return false;
        }
    }
    return true;
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

std::vector<glm::ivec2> areaOffsets(AreaShape shape, int minRange, int maxRange, glm::ivec2 direction) {
    std::vector<glm::ivec2> offsets;
    minRange = std::max(0, minRange);
    maxRange = std::clamp(maxRange, minRange, 64);
    if (direction == glm::ivec2(0)) {
        direction = {0, 1};
    }
    const glm::ivec2 side(-direction.y, direction.x);
    for (int z = -maxRange; z <= maxRange; ++z) {
        for (int x = -maxRange; x <= maxRange; ++x) {
            const int manhattan = std::abs(x) + std::abs(z);
            const int chebyshev = std::max(std::abs(x), std::abs(z));
            const int forward = x * direction.x + z * direction.y;
            const int lateral = x * side.x + z * side.y;
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
                inside =
                    forward >= std::max(minRange, 1) && forward <= maxRange && std::abs(lateral) < forward;
                break;
            }
            if (inside) {
                offsets.emplace_back(x, z);
            }
        }
    }
    return offsets;
}

std::optional<GridHit> raycastGrid(const Grid& grid, glm::vec3 origin, glm::vec3 direction,
                                   float maxDistance) {
    if (glm::length(direction) < 1e-6f) {
        return std::nullopt;
    }
    direction = glm::normalize(direction);
    constexpr float StepSize = 0.04f;
    for (float t = 0; t <= maxDistance; t += StepSize) {
        const auto p = origin + direction * t;
        const int x = static_cast<int>(std::floor(p.x));
        const int z = static_cast<int>(std::floor(p.z));
        for (int layer = grid.layerCount() - 1; layer >= 0; --layer) {
            if (!grid.contains(x, z, layer)) {
                continue;
            }
            const auto& cell = grid.at(x, z, layer);
            if (layer > 0 && !cell.walkable) {
                continue; // empty upper-layer cells are open air
            }
            const float surface = grid.heightAt({p.x, p.z}, layer);
            if (p.y <= surface && p.y >= surface - 0.6f) {
                return GridHit{{x, z, layer}, {p.x, surface, p.z}};
            }
        }
    }
    return std::nullopt;
}
} // namespace fe
