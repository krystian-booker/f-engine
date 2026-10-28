#include "engine/world/NavGrid.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace fe {
namespace {
constexpr const char* rampNames[] = {"none", "north", "east", "south", "west"};
}
NavGrid::NavGrid(int width, int depth) : width_(width), depth_(depth) {
    if (width < 1 || depth < 1 || width > 4096 || depth > 4096)
        throw std::runtime_error("Nav grid dimensions must be 1..4096");
    cells_.resize(static_cast<std::size_t>(width) * depth);
}
bool NavGrid::contains(int x, int z) const { return x >= 0 && z >= 0 && x < width_ && z < depth_; }
NavCell& NavGrid::at(int x, int z) {
    if (!contains(x, z)) throw std::out_of_range("Nav grid cell");
    return cells_[static_cast<std::size_t>(z) * width_ + x];
}
const NavCell& NavGrid::at(int x, int z) const {
    if (!contains(x, z)) throw std::out_of_range("Nav grid cell");
    return cells_[static_cast<std::size_t>(z) * width_ + x];
}
NavGrid NavGrid::fromJson(const Json& data) {
    NavGrid grid(data.at("width").get<int>(), data.at("depth").get<int>());
    if (data.contains("cells")) {
        const auto& cells = data.at("cells");
        if (!cells.is_array() || cells.size() != grid.cells_.size())
            throw std::runtime_error("Nav grid cells must contain width × depth entries");
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto& cell = cells[i];
            auto& out = grid.cells_[i];
            out.height = cell.value("height", 0.0f);
            out.walkable = cell.value("walkable", true);
            out.flags = cell.value("flags", 0u);
            out.region = cell.value("region", 0);
            const auto ramp = cell.value("ramp", std::string("none"));
            const auto found = std::find(std::begin(rampNames), std::end(rampNames), ramp);
            if (found == std::end(rampNames) || !std::isfinite(out.height) || out.region < 0)
                throw std::runtime_error("Invalid nav cell at index " + std::to_string(i));
            out.ramp = static_cast<Ramp>(found - std::begin(rampNames));
        }
    }
    return grid;
}
Json NavGrid::toJson() const {
    Json cells = Json::array();
    for (const auto& cell : cells_) cells.push_back({{"height", cell.height}, {"walkable", cell.walkable},
        {"ramp", rampNames[static_cast<int>(cell.ramp)]}, {"flags", cell.flags}, {"region", cell.region}});
    return {{"width", width_}, {"depth", depth_}, {"cells", cells}};
}
float NavGrid::heightAt(glm::vec2 p) const {
    const int x = static_cast<int>(std::floor(p.x)), z = static_cast<int>(std::floor(p.y));
    if (!contains(x, z)) return 0;
    const auto& cell = at(x, z);
    const float fx = p.x - x, fz = p.y - z;
    switch (cell.ramp) {
    case Ramp::North: return cell.height + 1 - fz;
    case Ramp::East: return cell.height + fx;
    case Ramp::South: return cell.height + fz;
    case Ramp::West: return cell.height + 1 - fx;
    default: return cell.height;
    }
}
bool NavGrid::canStand(glm::vec2 p, float radius) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || radius < 0 || radius > 0.49f) return false;
    // Circle vs each nearby blocked cell, rather than a square footprint.
    const int x0 = static_cast<int>(std::floor(p.x - radius));
    const int x1 = static_cast<int>(std::floor(p.x + radius));
    const int z0 = static_cast<int>(std::floor(p.y - radius));
    const int z1 = static_cast<int>(std::floor(p.y + radius));
    for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) {
        if (contains(x, z) && at(x, z).walkable) continue;
        const glm::vec2 nearest = glm::clamp(p, glm::vec2(x, z), glm::vec2(x + 1, z + 1));
        const auto offset = nearest - p;
        if (glm::dot(offset, offset) <= radius * radius + 1e-7f) return false;
    }
    return true;
}
glm::vec3 NavGrid::move(glm::vec3 position, glm::vec2 delta, float radius, float maxStep) const {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || maxStep < 0) return position;
    glm::vec2 p(position.x, position.z);
    // Substeps prevent tunnelling through a wall during scripted/fast movement.
    const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(delta) / 0.1f)));
    if (steps > 10000) throw std::runtime_error("Movement delta too large");
    const auto increment = delta / static_cast<float>(steps);
    auto valid = [&](glm::vec2 candidate) {
        return canStand(candidate, radius) && std::abs(heightAt(candidate) - heightAt(p)) <= maxStep;
    };
    for (int i = 0; i < steps; ++i) {
        if (valid(p + increment)) p += increment;
        else {
            if (valid(p + glm::vec2(increment.x, 0))) p.x += increment.x;
            if (valid(p + glm::vec2(0, increment.y))) p.y += increment.y;
        }
    }
    return {p.x, heightAt(p), p.y};
}
std::vector<glm::ivec2> NavGrid::findPath(glm::ivec2 start, glm::ivec2 goal) const {
    if (!contains(start.x, start.y) || !contains(goal.x, goal.y) ||
        !at(start.x, start.y).walkable || !at(goal.x, goal.y).walkable) return {};
    const auto index = [&](glm::ivec2 p) { return p.y * width_ + p.x; };
    std::vector<int> parent(cells_.size(), -1);
    std::queue<glm::ivec2> frontier;
    frontier.push(start); parent[index(start)] = index(start);
    constexpr std::array<glm::ivec2, 4> directions{{{0,-1},{1,0},{0,1},{-1,0}}};
    while (!frontier.empty()) {
        const auto p = frontier.front(); frontier.pop();
        if (p == goal) break;
        for (auto d : directions) {
            const auto q = p + d;
            if (!contains(q.x, q.y) || parent[index(q)] >= 0 || !at(q.x, q.y).walkable) continue;
            const auto from = glm::vec2(p) + 0.5f, to = glm::vec2(q) + 0.5f;
            const auto moved = move({from.x, heightAt(from), from.y}, to - from);
            if (glm::length(glm::vec2(moved.x, moved.z) - to) > 0.05f) continue;
            parent[index(q)] = index(p); frontier.push(q);
        }
    }
    if (parent[index(goal)] < 0) return {};
    std::vector<glm::ivec2> path;
    for (int i = index(goal); i != index(start); i = parent[i]) path.emplace_back(i % width_, i / width_);
    std::reverse(path.begin(), path.end());
    return path;
}
}
