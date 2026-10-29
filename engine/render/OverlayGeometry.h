#pragma once
#include "engine/nav/Grid.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace fe {
// Triangles for grid overlays, laid on the grid surface.
struct OverlayMesh {
    struct Vertex {
        glm::vec3 position{0};
        glm::vec2 uv{0}; // 0..1 across a cell; the shader draws borders near the edges
        glm::vec4 color{1};
    };
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    void clear() {
        vertices.clear();
        indices.clear();
    }
};

// A cell lifted `lift` above the ground and shrunk by `inset` on each side.
void addCellQuad(const Grid& grid, GridPos cell, float lift, float inset, glm::vec4 color, OverlayMesh& mesh);

// A strip through the centres of `cells` ending in an arrow head. Steps that
// stay on the same column (changing layer on a bridge) add nothing.
void addPathArrow(const Grid& grid, const std::vector<GridPos>& cells, glm::vec4 color, OverlayMesh& mesh);
} // namespace fe
