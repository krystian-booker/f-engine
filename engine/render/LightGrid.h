#pragma once
#include "engine/world/MapData.h"
#include <vector>

namespace fe {
// A light ready for shading: world position, unflickered settings and the
// intensity with flicker applied for this frame.
struct SceneLight {
    LightSettings settings;
    float intensity = 0;
};

// How much a light can contribute anywhere inside an XZ rectangle, from its
// unflickered intensity. 0 when the light does not reach the rectangle.
float lightScore(const LightSettings& light, glm::vec2 min, glm::vec2 max);

// Bins lights into a world-space XZ grid so every fragment shades only the
// lights that reach its cell. No light is picked per object, so neighbouring
// objects never disagree about which lights exist. A cell keeps its
// `MaxPerCell` highest-scoring lights; ranking ignores flicker, so a
// flickering light never strobes in and out of a cell.
class LightGrid {
public:
    static constexpr int MaxLights = 256, MaxPerCell = 16, MaxCells = 128, Rows = 4;
    static constexpr float MinCellSize = 2;

    // Lights beyond `MaxLights` are dropped furthest from `focus` first.
    void build(const std::vector<SceneLight>& lights, glm::vec2 boundsMin, glm::vec2 boundsMax,
               glm::vec3 focus);

    // Marks the light in texture slot `slot` as shadowed by `tiles` atlas tiles
    // from `firstTile`; `texelScale` is a shadow texel's size per unit of distance.
    void setShadow(int slot, int firstTile, int tiles, float texelScale);

    // Indices into the light texture of one cell, best first.
    [[nodiscard]] std::vector<int> cell(int x, int z) const;
    [[nodiscard]] glm::ivec2 cellOf(glm::vec2 position) const;

    glm::vec2 origin{0};
    float cellSize = MinCellSize;
    glm::ivec2 cells{1};
    int lightCount = 0;
    // The input light in each texture slot.
    std::vector<std::size_t> sources;
    // Rows of MaxLights texels: position and radius; colour times intensity
    // and cos(inner cone) (-2 for point lights); direction and cos(outer
    // cone); first shadow tile (-1 = none), tile count and texel scale.
    std::vector<glm::vec4> lightTexels;
    // cells.y rows of cells.x * MaxPerCell light indices, -1 after the last.
    std::vector<float> cellTexels;

private:
    struct Candidate {
        int cell;
        float score;
        int light;
    };
    // Scratch kept between builds, so a frame allocates nothing here.
    std::vector<std::size_t> chosen_;
    std::vector<Candidate> candidates_;
};
} // namespace fe
