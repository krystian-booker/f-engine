#pragma once
#include "engine/nav/Grid.h"
#include "engine/world/MapData.h"
#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace fe {
struct Model;

// The automatic first pass over a map's walkability: the geometry is sampled
// with rays from above, cells are judged by how much of them is level ground
// with head room, partially covered cells get collider boxes for the covered
// part, and the result is applied as ordinary grid and collider data the
// brush can clean up afterwards. UI-free; runs headless.
struct BakeOptions {
    int layer = 0;                  // the grid layer written
    float minHeight = -1e9f;        // only surfaces inside this band count (a bridge deck for layer 1)
    float maxHeight = 1e9f;
    int samples = 4;                // rays per cell side (4 = 16 per cell, one per quarter cell)
    float maxSlopeDegrees = 35;     // steeper surfaces are not ground (ramps qualify)
    float clearance = 1.6f;         // head room a body needs above the ground
    float step = 0.6f;              // a sample this far from the cell's level is not the same floor
    float coverage = 0.75f;         // walkable samples needed for a walkable cell
    float partialCoverage = 0.25f;  // below this the cell is blocked; between, walkable with boxes
    float heightStep = 0.25f;       // cell heights are rounded to this
    bool writeWalkable = true, writeHeights = true, writeRamps = true, writeWater = true, writeColliders = true;
    bool keepBlocked = false;       // never make a cell that is unwalkable now walkable
    // Walkable cells no step of `step` connects to the largest walkable region
    // (roof tops, ledges without a ramp) are blocked.
    bool pruneIslands = true;
    // The outer ring of cells is blocked (a margin that keeps bodies off the
    // map's edge; the grid's bounds stop them anyway).
    bool blockEdges = true;
    std::optional<glm::ivec4> region; // cells {x, z, width, depth}; whole grid when unset
    [[nodiscard]] bool inRegion(int x, int z) const {
        return !region || (x >= region->x && z >= region->y && x < region->x + region->z && z < region->y + region->w);
    }
};

struct BakeCell {
    bool hit = false;       // any sample found a surface in the band
    bool walkable = false;  // the verdict
    bool partial = false;   // walkable with boxes over the covered quarters
    bool water = false;     // the surface is water
    bool island = false;    // walkable ground nothing connects to the main floor (pruned)
    float coverage = 0;     // share of samples that are walkable ground
    float height = 0;       // the floor height (median of the ground samples)
    Ramp ramp = Ramp::None;
    float rise = 0;
    // 0 = the vote was at a threshold, 1 = unanimous; the preview colours by it.
    float confidence = 0;
};

struct BakeResult {
    int width = 0, depth = 0;
    std::vector<BakeCell> cells;
    std::vector<Box> colliders; // for the covered quarters of partial cells
    int walkable = 0, blocked = 0, partial = 0, unsampled = 0, islands = 0;
    [[nodiscard]] const BakeCell& at(int x, int z) const {
        return cells[static_cast<std::size_t>(z * width + x)];
    }
    [[nodiscard]] BakeCell& at(int x, int z) { return cells[static_cast<std::size_t>(z * width + x)]; }
};

// Triangle soup of a model in world space, bucketed over the ground plane
// for cheap vertical rays.
class MeshRaycaster {
public:
    struct Hit {
        float y = 0;
        glm::vec3 normal{0, 1, 0};
        bool water = false;
    };
    explicit MeshRaycaster(const Model& model);
    // The highest surface under (x, z) with a top between minY and maxY.
    [[nodiscard]] std::optional<Hit> highest(float x, float z, float minY, float maxY) const;
    // Whether any triangle lies above (x, y, z) within `distance`.
    [[nodiscard]] bool blockedAbove(float x, float y, float z, float distance) const;
    [[nodiscard]] std::size_t triangleCount() const { return triangles_.size(); }
    [[nodiscard]] glm::vec3 min() const { return min_; }
    [[nodiscard]] glm::vec3 max() const { return max_; }

private:
    struct Triangle {
        glm::vec3 a, b, c, normal;
        bool water = false;
    };
    [[nodiscard]] const std::vector<int>& bucket(float x, float z) const;
    std::vector<Triangle> triangles_;
    std::vector<std::vector<int>> buckets_;
    glm::vec3 min_{0}, max_{0};
    int bucketsX_ = 0, bucketsZ_ = 0;
    static const std::vector<int> None;
};

[[nodiscard]] BakeResult bakeWalkability(const Model& model, const Grid& grid, const BakeOptions& options);
// Writes a bake into a map (grid cells of the options' layer, the water tag,
// the partial-cell colliders); returns the number of cells changed.
int applyBake(MapData& map, const BakeResult& result, const BakeOptions& options);
} // namespace fe
