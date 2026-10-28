#pragma once
#include "engine/core/Json.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace fe {
enum TileFlags : std::uint32_t { Water = 1, Counter = 2, DamageFloor = 4 };
enum class Ramp { None, North, East, South, West };
struct NavCell {
    float height = 0;
    bool walkable = true;
    Ramp ramp = Ramp::None;
    std::uint32_t flags = 0;
    int region = 0;
};
class NavGrid {
public:
    NavGrid() = default;
    NavGrid(int width, int depth);
    static NavGrid fromJson(const Json& data);
    [[nodiscard]] Json toJson() const;
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int depth() const { return depth_; }
    [[nodiscard]] bool contains(int x, int z) const;
    NavCell& at(int x, int z);
    [[nodiscard]] const NavCell& at(int x, int z) const;
    [[nodiscard]] float heightAt(glm::vec2 position) const;
    [[nodiscard]] bool canStand(glm::vec2 position, float radius = 0.2f) const;
    [[nodiscard]] glm::vec3 move(glm::vec3 position, glm::vec2 delta,
        float radius = 0.2f, float maxStep = 0.3f) const;
    [[nodiscard]] std::vector<glm::ivec2> findPath(glm::ivec2 start, glm::ivec2 goal) const;
private:
    int width_ = 0, depth_ = 0;
    std::vector<NavCell> cells_;
};
}
