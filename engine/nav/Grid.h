#pragma once
#include "engine/core/Json.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace fe {
// Also the four compass directions (ladder links, entity facing); the one
// table of names, map codes and grid vectors for them is in Grid.cpp.
enum class Ramp : std::uint8_t { None, North, East, South, West };
inline constexpr int RampCount = 5;
// Ramp names used by scripts ("none", "north", ...) and one-letter map codes (". n e s w").
[[nodiscard]] const char* rampName(Ramp ramp);
[[nodiscard]] std::optional<Ramp> rampFromName(std::string_view name);
[[nodiscard]] char rampCode(Ramp ramp);
[[nodiscard]] std::optional<Ramp> rampFromCode(char code);
// The grid step towards a direction (north is -z); {0, 0} for None.
[[nodiscard]] glm::ivec2 rampDirection(Ramp ramp);
// The direction of a grid step, None unless it is one of the four.
[[nodiscard]] Ramp rampFromDirection(glm::ivec2 step);

struct GridCell {
    float height = 0;
    bool walkable = true;
    Ramp ramp = Ramp::None;
    float rise = 1;         // how far a ramp climbs across the cell
    std::uint32_t tags = 0; // bit i = grid tag palette entry i
    bool operator==(const GridCell&) const = default;
};

// floor(value) as an integer coordinate (grid cells, hash buckets), saturated
// to +-2^30: positions far outside any grid, infinities and NaN (the low end)
// map to cells outside the grid instead of overflowing an int.
inline constexpr int CellLimit = 1 << 30;
[[nodiscard]] inline int floorCell(float value) {
    if (!(value > -static_cast<float>(CellLimit))) {
        return -CellLimit;
    }
    if (!(value < static_cast<float>(CellLimit))) {
        return CellLimit;
    }
    return static_cast<int>(std::floor(value));
}
[[nodiscard]] inline int ceilCell(float value) {
    return -floorCell(-value);
}

struct GridPos {
    int x = 0, z = 0, layer = 0;
    bool operator==(const GridPos&) const = default;
};

enum class LinkKind : std::uint8_t { Step, Ladder };

// A step link joins two layers at a cell (the foot of a bridge); see
// Grid::traverseLink. A ladder link joins the cell (x, z) on layer `from`
// (its foot) to the neighbouring cell in `direction` on layer `to` (its top),
// whatever their height difference; see Grid::ladderStep. Path finding and
// movers share both rules.
struct GridLink {
    int x = 0, z = 0, from = 0, to = 0;
    glm::ivec2 direction{0}; // zero = undirected (step links only)
    LinkKind kind = LinkKind::Step;
    [[nodiscard]] int other(int layer) const { return layer == from ? to : from; }
    [[nodiscard]] GridPos foot() const { return {x, z, from}; }
    [[nodiscard]] GridPos top() const { return {x + direction.x, z + direction.y, to}; }
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
    GridCell& at(GridPos p) { return at(p.x, p.z, p.layer); }
    [[nodiscard]] const GridCell& at(int x, int z, int layer = 0) const;
    [[nodiscard]] const GridCell& at(GridPos p) const { return at(p.x, p.z, p.layer); }

    // Surface height at a world position (x, z) on a layer, following ramps.
    [[nodiscard]] float heightAt(glm::vec2 position, int layer = 0) const;
    // Height of a cell's surface plane at `position`, which may lie on the cell's border.
    [[nodiscard]] float heightIn(GridPos cell, glm::vec2 position) const;
    // Height where a cell meets its neighbour in direction (dx, dz).
    [[nodiscard]] float edgeHeight(GridPos cell, int dx, int dz) const;
    [[nodiscard]] bool canStand(glm::vec2 position, float radius = 0.2f, int layer = 0) const;
    [[nodiscard]] bool walkableAt(glm::vec2 position, int layer) const;
    // First step link at a cell that touches `layer`, or null.
    [[nodiscard]] const GridLink* linkAt(int x, int z, int layer) const;
    // Calls `visit` with every step link at a cell that touches `layer`, in
    // link order, until it returns true; returns whether one did.
    template<class Visit> bool anyLinkAt(int x, int z, int layer, Visit&& visit) const {
        if (!contains(x, z)) {
            return false;
        }
        const auto cell = index(x, z);
        for (auto it =
                 std::lower_bound(linkIndex_.begin(), linkIndex_.end(), std::make_pair(cell, std::size_t{0}));
             it != linkIndex_.end() && it->first == cell; ++it) {
            const auto& link = links_[it->second];
            if ((link.from == layer || link.to == layer) && visit(link)) {
                return true;
            }
        }
        return false;
    }

    // Largest height difference between two linked surfaces that still counts as joined.
    static constexpr float LinkGap = 0.25f;
    // The layer-change rule shared by path finding and movers. An agent at
    // `position` on `layer`, travelling along `heading`, moves to the other
    // layer of a link in that cell when
    //  - the link is directed and the heading points along `direction`
    //    (from -> to) or against it (to -> from), or the link is undirected
    //    and `layerEnds` (the current layer cannot be walked any further),
    //  - the other layer's cell is walkable, and
    //  - the other surface is at most min(LinkGap, maxUp) above and
    //    min(LinkGap, maxDown) below the current one at `position`.
    // Returns the layer the agent ends up on (`layer` when nothing changes).
    [[nodiscard]] int traverseLink(glm::vec2 position, int layer, glm::vec2 heading, float maxUp,
                                   float maxDown, bool layerEnds) const;
    // The ladder rule shared by path finding and movers: from a ladder's foot
    // heading along its direction, or from its top heading against it, the
    // agent climbs to the other end (any height). `step` is one of the four
    // axis directions. Returns the ladder, or null.
    [[nodiscard]] const GridLink* ladderStep(GridPos from, glm::ivec2 step) const;

    [[nodiscard]] const std::vector<std::string>& tagNames() const { return tagNames_; }
    // Bit for a tag name, adding it to the palette when `create` is set.
    std::uint32_t tagBit(const std::string& name, bool create);
    [[nodiscard]] std::uint32_t tagMask(const std::vector<std::string>& names) const;
    [[nodiscard]] std::vector<std::string> tagsOf(std::uint32_t mask) const;

    [[nodiscard]] const std::vector<GridLink>& links() const { return links_; }
    void setLinks(std::vector<GridLink> links);
    // Free-form per-cell data, kept for scripts (treasure ids, terrain notes).
    [[nodiscard]] const Json* cellProps(GridPos p) const;
    void setCellProps(GridPos p, Json props);

private:
    [[nodiscard]] std::size_t index(int x, int z) const {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    }
    void indexLinks();
    int width_ = 0, depth_ = 0;
    std::vector<Layer> layers_;
    std::vector<std::string> tagNames_;
    std::vector<GridLink> links_;
    std::vector<std::pair<std::size_t, std::size_t>> linkIndex_;   // (cell index, link index), sorted
    std::vector<std::pair<std::size_t, std::size_t>> ladderIndex_; // both ends of every ladder, sorted
    std::map<std::tuple<int, int, int>, Json> cellProps_;
};
} // namespace fe
