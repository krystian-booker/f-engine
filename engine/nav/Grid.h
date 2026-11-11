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

// Toroidal arithmetic (Grid::wraps()). Floor division, never fmod: the same
// result for negative values on every platform, so wrapped simulations stay
// deterministic. wrapCoordinate folds a value into [0, size); wrapDifference
// folds a difference into [-size/2, size/2), the shortest way round.
[[nodiscard]] inline float wrapCoordinate(float value, float size) {
    const float wrapped = value - std::floor(value / size) * size;
    return wrapped < size ? wrapped : 0.0f; // a hair below zero rounds up to `size`
}
[[nodiscard]] inline float wrapDifference(float difference, float size) {
    return difference - std::floor(difference / size + 0.5f) * size;
}
[[nodiscard]] inline int wrapIndex(int value, int size) {
    const int wrapped = value % size;
    return wrapped < 0 ? wrapped + size : wrapped;
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
    // A toroidal grid ("wrap": true): the east edge joins the west and the
    // south the north. Every cell lookup folds its x and z into the grid, so
    // heights, walkability, links, components and path finding continue
    // across the seam, and contains() refuses only a bad layer. Positions
    // are still kept canonical, in [0, width) x [0, depth): wrapPoint and
    // wrapCell give that representative and delta the shortest difference.
    // All of them are the identity on a grid that does not wrap.
    [[nodiscard]] bool wraps() const { return wrap_; }
    void setWrap(bool wrap);
    [[nodiscard]] glm::vec2 wrapPoint(glm::vec2 p) const {
        return wrap_ ? glm::vec2(wrapCoordinate(p.x, static_cast<float>(width_)),
                                 wrapCoordinate(p.y, static_cast<float>(depth_)))
                     : p;
    }
    [[nodiscard]] GridPos wrapCell(GridPos p) const {
        return wrap_ ? GridPos{wrapIndex(p.x, width_), wrapIndex(p.z, depth_), p.layer} : p;
    }
    // `to - from` on the x/z plane, the short way round when the grid wraps.
    [[nodiscard]] glm::vec2 delta(glm::vec2 from, glm::vec2 to) const {
        const auto d = to - from;
        return wrap_ ? glm::vec2(wrapDifference(d.x, static_cast<float>(width_)),
                                 wrapDifference(d.y, static_cast<float>(depth_)))
                     : d;
    }
    // Whether a cell lies inside the grid's rectangle and layer, wrap or no
    // wrap: the strict test, for validating authored data.
    [[nodiscard]] bool inRange(int x, int z, int layer = 0) const {
        return x >= 0 && z >= 0 && x < width_ && z < depth_ && layer >= 0 && layer < layerCount();
    }
    [[nodiscard]] bool inRange(GridPos p) const { return inRange(p.x, p.z, p.layer); }
    // Whether a cell can be looked up: inRange, or any x and z of a valid
    // layer when the grid wraps.
    [[nodiscard]] bool contains(int x, int z, int layer = 0) const;
    [[nodiscard]] bool contains(GridPos p) const { return contains(p.x, p.z, p.layer); }
    // Writable access counts as a change (see revision()): read through a
    // const Grid where nothing is written.
    GridCell& at(int x, int z, int layer = 0);
    GridCell& at(GridPos p) { return at(p.x, p.z, p.layer); }
    [[nodiscard]] const GridCell& at(int x, int z, int layer = 0) const;
    [[nodiscard]] const GridCell& at(GridPos p) const { return at(p.x, p.z, p.layer); }
    // Replaces a cell; a change counts towards revision(), writing the same
    // cell again does not.
    void setCell(GridPos p, const GridCell& cell);
    // Counts the changes that may alter connectivity: every write through the
    // non-const at() (which cannot tell a read from a write), setCell with a
    // different cell, setLinks and addLayer. Derived data cached on the grid
    // (component()) is keyed on it.
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    // Changes only when walkability, layers, links or wrapping change: what
    // component() is cached against (a cell's height or tags leave it alone).
    [[nodiscard]] std::uint64_t topologyRevision() const { return topology_; }
    // The connected component of a cell under the most permissive walking
    // rules: 4-neighbour walkable cells of a layer, joined through every step
    // and ladder link whose two ends are walkable, ignoring heights, tags and
    // blocked sets. Cells in different components cannot be joined by any
    // route that neither flies nor jumps gaps; the same component promises
    // nothing. -1 for an unwalkable cell or one outside the grid. Computed on
    // first use and cached until the grid changes.
    [[nodiscard]] int component(GridPos p) const;

    // Surface height at a world position (x, z) on a layer, following ramps.
    [[nodiscard]] float heightAt(glm::vec2 position, int layer = 0) const;
    // Height of a cell's surface plane at `position`, which may lie on the cell's border.
    [[nodiscard]] float heightIn(GridPos cell, glm::vec2 position) const;
    // Height where a cell meets its neighbour in direction (dx, dz).
    [[nodiscard]] float edgeHeight(GridPos cell, int dx, int dz) const;
    // Standing room for a body of `radius` at `position`: every cell its disc
    // touches is inside the grid and walkable. With a `footprint` above 1 the
    // body also covers that many cells per side from its anchor cell (the
    // cell of `position`) towards +x and +z, which must be inside the grid and
    // walkable too. A flying body (`fly`) needs the cells inside the grid only.
    // Whether it flies or not, a cell carrying one of `blockedTags`, or (when
    // `requiredTags` is not zero) none of `requiredTags`, refuses the body:
    // its passability class.
    [[nodiscard]] bool canStand(glm::vec2 position, float radius = 0.2f, int layer = 0, int footprint = 1,
                                bool fly = false, std::uint32_t blockedTags = 0,
                                std::uint32_t requiredTags = 0) const;
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
    // Cell slot of (x, z), which the caller has checked with contains(); a
    // wrapping grid folds the coordinates here, so every lookup wraps.
    [[nodiscard]] std::size_t index(int x, int z) const {
        if (wrap_) {
            x = wrapIndex(x, width_);
            z = wrapIndex(z, depth_);
        }
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    }
    void indexLinks();
    void buildComponents() const;
    int width_ = 0, depth_ = 0;
    bool wrap_ = false;
    std::vector<Layer> layers_;
    std::vector<std::string> tagNames_;
    std::vector<GridLink> links_;
    std::vector<std::pair<std::size_t, std::size_t>> linkIndex_;   // (cell index, link index), sorted
    std::vector<std::pair<std::size_t, std::size_t>> ladderIndex_; // both ends of every ladder, sorted
    std::map<std::tuple<int, int, int>, Json> cellProps_;
    std::uint64_t revision_ = 0;
    // Moves with what connectivity depends on: walkability, layers, links and
    // wrapping (every revision_ bump except a cell keeping its walkability).
    std::uint64_t topology_ = 0;
    // component() cache: one label per cell of every layer, valid for componentsRevision_ (a topology_).
    mutable std::vector<std::int32_t> components_;
    mutable std::uint64_t componentsRevision_ = 0;
    mutable bool componentsBuilt_ = false;
};
} // namespace fe
