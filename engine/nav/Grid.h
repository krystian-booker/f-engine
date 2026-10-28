#pragma once
#include "engine/core/Json.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace fe {
enum class Ramp : std::uint8_t { None, North, East, South, West };

struct GridCell {
    float height = 0;
    bool walkable = true;
    Ramp ramp = Ramp::None;
    std::uint32_t tags = 0; // bit i = grid tag palette entry i
};

struct GridPos {
    int x = 0, z = 0, layer = 0;
    bool operator==(const GridPos&) const = default;
};

// A link lets movement change layers at a cell (the foot of a bridge).
// Moving along `direction` inside the cell climbs from `from` to `to`;
// moving against it climbs back down. Without a direction the switch only
// happens when the current layer ends.
struct GridLink {
    int x = 0, z = 0, from = 0, to = 0;
    glm::ivec2 direction{0};
};

// The logical map grid: one or more stacked layers of cells. Layer 0 is the
// ground; higher layers model bridges and overpasses. Cells carry heights,
// walkability, ramps and tag bits. Tags have no engine meaning; scripts give
// them meaning ("water", "region:2", "deploy").
class Grid {
public:
    struct Layer {
        std::string name;
        std::vector<GridCell> cells;
    };
    Grid() = default;
    Grid(int width, int depth, int layers = 1);
    static Grid fromJson(const Json& data);
    [[nodiscard]] Json toJson() const;

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int depth() const { return depth_; }
    [[nodiscard]] int layerCount() const { return static_cast<int>(layers_.size()); }
    [[nodiscard]] const Layer& layer(int index) const { return layers_.at(static_cast<std::size_t>(index)); }
    int addLayer(const std::string& name);
    [[nodiscard]] bool contains(int x, int z, int layer = 0) const;
    [[nodiscard]] bool contains(GridPos p) const { return contains(p.x, p.z, p.layer); }
    GridCell& at(int x, int z, int layer = 0);
    [[nodiscard]] const GridCell& at(int x, int z, int layer = 0) const;
    [[nodiscard]] const GridCell& at(GridPos p) const { return at(p.x, p.z, p.layer); }

    // Surface height at a world position (x, z) on a layer, following ramps.
    [[nodiscard]] float heightAt(glm::vec2 position, int layer = 0) const;
    // Height where a cell meets its neighbour in direction (dx, dz).
    [[nodiscard]] float edgeHeight(GridPos cell, int dx, int dz) const;
    [[nodiscard]] bool canStand(glm::vec2 position, float radius = 0.2f, int layer = 0) const;
    [[nodiscard]] std::optional<int> linkedLayer(int x, int z, int layer) const;
    [[nodiscard]] const GridLink* linkAt(int x, int z, int layer) const;

    [[nodiscard]] const std::vector<std::string>& tagNames() const { return tagNames_; }
    // Bit for a tag name, adding it to the palette when `create` is set.
    std::uint32_t tagBit(const std::string& name, bool create);
    [[nodiscard]] std::uint32_t tagMask(const std::vector<std::string>& names) const;
    [[nodiscard]] std::vector<std::string> tagsOf(std::uint32_t mask) const;

    [[nodiscard]] const std::vector<GridLink>& links() const { return links_; }
    void setLinks(std::vector<GridLink> links) { links_ = std::move(links); }
    // Free-form per-cell data, kept for scripts (treasure ids, terrain notes).
    [[nodiscard]] const Json* cellProps(GridPos p) const;
    void setCellProps(GridPos p, Json props);

private:
    [[nodiscard]] std::size_t index(int x, int z) const {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    }
    int width_ = 0, depth_ = 0;
    std::vector<Layer> layers_;
    std::vector<std::string> tagNames_;
    std::vector<GridLink> links_;
    std::map<std::tuple<int, int, int>, Json> cellProps_;
};
} // namespace fe
