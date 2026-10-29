#include "engine/render/OverlayGeometry.h"

namespace fe {
namespace {
constexpr float PathLift = 0.06f, PathHalfWidth = 0.09f, HeadHalfWidth = 0.22f, HeadLength = 0.38f;

void addIndices(OverlayMesh& mesh, std::uint32_t base, std::initializer_list<std::uint32_t> indices) {
    for (const auto index : indices) {
        mesh.indices.push_back(base + index);
    }
}
} // namespace

void addCellQuad(const Grid& grid, GridPos cell, float lift, float inset, glm::vec4 color,
                 OverlayMesh& mesh) {
    const float x0 = static_cast<float>(cell.x) + inset, x1 = static_cast<float>(cell.x + 1) - inset;
    const float z0 = static_cast<float>(cell.z) + inset, z1 = static_cast<float>(cell.z + 1) - inset;
    auto corner = [&](float x, float z, glm::vec2 uv) {
        return OverlayMesh::Vertex{{x, grid.heightAt({x, z}, cell.layer) + lift, z}, uv, color};
    };
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(corner(x0, z0, {0, 0}));
    mesh.vertices.push_back(corner(x1, z0, {1, 0}));
    mesh.vertices.push_back(corner(x1, z1, {1, 1}));
    mesh.vertices.push_back(corner(x0, z1, {0, 1}));
    addIndices(mesh, base, {0, 2, 1, 0, 3, 2});
}

void addPathArrow(const Grid& grid, const std::vector<GridPos>& cells, glm::vec4 color, OverlayMesh& mesh) {
    auto centre = [](GridPos p) {
        return glm::vec2(static_cast<float>(p.x) + 0.5f, static_cast<float>(p.z) + 0.5f);
    };
    auto at = [&](glm::vec2 p, int layer) {
        return OverlayMesh::Vertex{{p.x, grid.heightAt(p, layer) + PathLift, p.y}, {0.5f, 0.5f}, color};
    };
    std::size_t last = 0;
    for (std::size_t k = 0; k + 1 < cells.size(); ++k) {
        if (centre(cells[k]) != centre(cells[k + 1])) {
            last = k + 1;
        }
    }
    for (std::size_t k = 0; k + 1 < cells.size(); ++k) {
        const auto a = cells[k], b = cells[k + 1];
        const auto pa = centre(a);
        auto pb = centre(b);
        if (pa == pb) {
            continue;
        }
        const auto dir = glm::normalize(pb - pa);
        const glm::vec2 side(-dir.y, dir.x);
        const bool head = k + 1 == last;
        if (head) {
            pb -= dir * HeadLength;
        }
        auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(at(pa - side * PathHalfWidth, a.layer));
        mesh.vertices.push_back(at(pa + side * PathHalfWidth, a.layer));
        mesh.vertices.push_back(at(pb + side * PathHalfWidth, b.layer));
        mesh.vertices.push_back(at(pb - side * PathHalfWidth, b.layer));
        addIndices(mesh, base, {0, 2, 1, 0, 3, 2});
        if (head) {
            base = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(at(pb - side * HeadHalfWidth, b.layer));
            mesh.vertices.push_back(at(pb + side * HeadHalfWidth, b.layer));
            mesh.vertices.push_back(at(pb + dir * HeadLength, b.layer));
            addIndices(mesh, base, {0, 2, 1});
        }
    }
}
} // namespace fe
