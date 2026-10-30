#pragma once
#include "engine/world/MapData.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fe {
struct Model;
struct ModelPart;
struct GridBrush;
class AssetCache;

// Derived walkability. The map's terrain model and every placed model (as
// placed: position, yaw, scale) are sampled with rays from above, cells are
// judged by how much of them is level ground with head room, and every
// quarter of a walkable cell that is not ground (a wall, a post, a drop)
// becomes a collider box, so bodies stop at the geometry itself and routes
// avoid the cells it covers. The result is ordinary grid data plus boxes in
// MapData::bake, which authors correct with overrides instead of painting.
// UI-free; runs headless (fengine_headless --bake).
struct BakeOptions {
    int layer = 0;                  // the grid layer written
    float minHeight = -1e9f;        // only surfaces inside this band count (a bridge deck for layer 1)
    float maxHeight = 1e9f;
    int samples = 4;                // rays per cell side (4 = 16 per cell, one per quarter cell)
    float maxSlopeDegrees = 35;     // steeper surfaces are not ground (ramps qualify)
    float clearance = 1.6f;         // head room a body needs above the ground
    float step = 0.6f;              // a sample this far from the cell's level is not the same floor
    float minCoverage = 0.25f;      // ground share a walkable cell needs; the rest of it gets boxes
    float heightStep = 0.25f;       // cell heights are rounded to this
    // Walkable cells no step of `step` connects to the largest walkable region
    // (roof tops, ledges without a ramp) are blocked.
    bool pruneIslands = true;
    // The outer ring of cells is blocked (a margin that keeps bodies off the
    // map's edge; the grid's bounds stop them anyway).
    bool blockEdges = true;
    // A quarter that drops more than `step` below the cell's floor (a cliff
    // edge inside the cell) gets a box, so bodies do not walk off it.
    bool boxDrops = true;
    std::optional<glm::ivec4> region; // cells {x, z, width, depth}; whole grid when unset
    [[nodiscard]] bool inRegion(int x, int z) const {
        return !region || (x >= region->x && z >= region->y && x < region->x + region->z && z < region->y + region->w);
    }
    // The settings a map stores (MapData::bake.options): everything but the
    // height band and the region, which belong to one bake. Missing keys
    // keep their defaults.
    static BakeOptions fromJson(const Json& json);
    [[nodiscard]] Json toJson() const;
};

struct BakeCell {
    bool hit = false;       // any sample found a surface in the band (false: left as it was)
    bool walkable = false;  // the verdict
    bool partial = false;   // walkable with boxes over the quarters that are not ground
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

// One model of the scene a bake reads, in world space through `transform`
// (the terrain with the identity, a placed model through placementMatrix).
struct BakePiece {
    const Model* model = nullptr;
    glm::mat4 transform{1};
};

// Triangle soup of the scene in world space, bucketed over the ground plane
// for cheap vertical rays.
class MeshRaycaster {
public:
    struct Hit {
        float y = 0;
        glm::vec3 normal{0, 1, 0};
        bool water = false;
    };
    explicit MeshRaycaster(const Model& model);
    explicit MeshRaycaster(std::span<const BakePiece> scene);
    // The highest surface under (x, z) with a top between minY and maxY.
    [[nodiscard]] std::optional<Hit> highest(float x, float z, float minY, float maxY) const;
    // Whether any triangle lies above (x, y, z) within `distance`.
    [[nodiscard]] bool blockedAbove(float x, float y, float z, float distance) const;
    // Whether a triangle that is not level ground (its normal's y below
    // `levelCos`) overlaps the xz rectangle lo..hi with heights inside
    // minY..maxY: a wall or post standing in that square, however thin, which
    // the rays of `highest` could pass beside.
    [[nodiscard]] bool obstacleIn(glm::vec2 lo, glm::vec2 hi, float minY, float maxY, float levelCos) const;
    [[nodiscard]] std::size_t triangleCount() const { return triangles_.size(); }
    [[nodiscard]] glm::vec3 min() const { return min_; }
    [[nodiscard]] glm::vec3 max() const { return max_; }

private:
    struct Triangle {
        glm::vec3 a, b, c, normal;
        bool water = false;
    };
    void add(const Model& model, const glm::mat4& transform);
    void finish();
    [[nodiscard]] const std::vector<int>& bucket(float x, float z) const;
    template<class Visit> bool anyBucket(glm::vec2 lo, glm::vec2 hi, Visit&& visit) const;
    std::vector<Triangle> triangles_;
    std::vector<std::vector<int>> buckets_;
    glm::vec3 min_{0}, max_{0};
    int bucketsX_ = 0, bucketsZ_ = 0;
    static const std::vector<int> None;
};

// The parts of a model the bake reads: when any node carries a truthy
// `collision` custom property, the parts under those nodes (shown or hidden:
// a simplified collider mesh is usually hidden with `visible: false`);
// otherwise every shown part.
[[nodiscard]] std::vector<const ModelPart*> collisionParts(const Model& model);
[[nodiscard]] bool hasCollisionNodes(const Model& model);
// A placement's world matrix: translate(position) * rotate(yaw about +y) *
// scale, as the renderer poses its mesh.
[[nodiscard]] glm::mat4 placementMatrix(const Placement& placement);
// The model file a placement contributes to the bake, or null: a string
// `model` prop, `solid` not false (decoration bodies walk through) and no
// event `pages` (a scripted prop shows and hides itself; it keeps a round
// collider instead).
[[nodiscard]] const std::string* bakedModelOf(const Placement& placement);

// The scene a map bakes: its terrain and every baked placement.
struct BakeScene {
    std::vector<BakePiece> pieces;
    std::vector<std::shared_ptr<const Model>> models; // keeps the pieces' models alive
    std::vector<std::string> problems;                 // placements whose model could not be loaded
};
[[nodiscard]] BakeScene sceneForMap(const MapData& map, AssetCache& assets, const std::string& geometryPath);

[[nodiscard]] BakeResult bakeWalkability(std::span<const BakePiece> scene, const Grid& grid, const BakeOptions& options);
[[nodiscard]] BakeResult bakeWalkability(const Model& model, const Grid& grid, const BakeOptions& options);
// Writes a bake into a map: grid cells of the options' layer (in the
// region), the water tag, the derived boxes (MapData::bake.colliders; a
// region bake replaces only the boxes inside the region), then the map's
// overrides. Returns the number of cells changed.
int applyBake(MapData& map, const BakeResult& result, const BakeOptions& options);
// Applies the map's overrides (those inside `region` when given) to the grid;
// returns the number of cells changed.
int applyOverrides(MapData& map, std::optional<glm::ivec4> region = std::nullopt);
// The brush as overrides. `setOverride` records what the brush paints on one
// cell (walkable, height, ramp, a tag added or removed) in a 1 x 1 override,
// merged into the latest one at that cell when nothing later covers it, and
// applies it to the grid; `setOverrideRect` records one rectangle override
// for the cells between `a` and `b` (inclusive). `clearOverride` forgets the
// overrides of a cell (rectangles covering it are split around it); the
// caller re-bakes the cell to restore the derived value. Each returns
// whether the map changed.
bool setOverride(MapData& map, GridPos cell, const GridBrush& brush);
bool setOverrideRect(MapData& map, GridPos a, GridPos b, const GridBrush& brush);
bool clearOverride(MapData& map, GridPos cell);
// Every cell on `layer` that an override covers.
[[nodiscard]] std::vector<GridPos> overriddenCells(const MapData& map, int layer);
// A hash of everything a bake reads: the geometry file, the baked placements
// (id, model, position, yaw, scale, layer), their model files and the stored
// options, as 16 hex digits. `readFile` gives a file's bytes by its game
// path, or nothing when it is missing. The same inputs give the same
// fingerprint on every platform.
[[nodiscard]] std::string bakeFingerprint(
    const MapData& map, const std::string& geometryPath,
    const std::function<std::optional<std::string>(const std::string&)>& readFile);
// A `readFile` for bakeFingerprint over a game folder: the bytes of a loose
// file by its game path, nothing when it is missing or outside the folder.
[[nodiscard]] std::function<std::optional<std::string>(const std::string&)> gameFileReader(
    const std::filesystem::path& gameRoot);
// The map's geometry as an asset path ("maps/<id>/<geometry>").
[[nodiscard]] std::string geometryPathOf(const MapData& map);
// The cells a placed model's bake can change: those under its box padded by
// one cell (a body's route-planning radius reaches that far), as a region
// {x, z, width, depth} clamped to the grid.
[[nodiscard]] glm::ivec4 bakeRegionFor(const Model& model, const Placement& placement, const Grid& grid);
[[nodiscard]] glm::ivec4 unionRegion(glm::ivec4 a, glm::ivec4 b);
} // namespace fe
