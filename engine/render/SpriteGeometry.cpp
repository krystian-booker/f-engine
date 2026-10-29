#include "engine/render/SpriteGeometry.h"
#include <cmath>

namespace fe {
void projectDecal(const Grid& grid, int layer, glm::vec2 min, glm::vec2 max, glm::vec2 uv0, glm::vec2 uv1,
                  float lift, std::vector<DecalQuad>& out) {
    if (max.x <= min.x || max.y <= min.y) {
        return;
    }
    const glm::vec2 size = max - min;
    const auto uvAt = [&](glm::vec2 p) {
        return glm::mix(uv0, uv1, (p - min) / size);
    };
    // Only cells of the grid get a quad, so the loops stay within it.
    const int x0 = std::max(0, floorCell(min.x)), x1 = std::min(grid.width(), ceilCell(max.x));
    const int z0 = std::max(0, floorCell(min.y)), z1 = std::min(grid.depth(), ceilCell(max.y));
    for (int z = z0; z < z1; ++z) {
        for (int x = x0; x < x1; ++x) {
            GridPos cell{x, z, layer};
            if (!grid.contains(cell) || (layer > 0 && !grid.at(cell).walkable)) {
                cell.layer = 0;
                if (!grid.contains(cell)) {
                    continue;
                }
            }
            const glm::vec2 lo = glm::max(min, glm::vec2(x, z));
            const glm::vec2 hi = glm::min(max, glm::vec2(x + 1, z + 1));
            if (hi.x <= lo.x || hi.y <= lo.y) {
                continue;
            }
            const auto at = [&](glm::vec2 p) {
                return glm::vec3(p.x, grid.heightIn(cell, p) + lift, p.y);
            };
            const glm::vec2 corners[] = {{lo.x, hi.y}, {hi.x, hi.y}, {hi.x, lo.y}, {lo.x, lo.y}};
            DecalQuad quad;
            for (std::size_t k = 0; k < 4; ++k) {
                quad.positions[k] = at(corners[k]);
                quad.uv[k] = uvAt(corners[k]);
            }
            quad.normal = glm::normalize(
                glm::cross(quad.positions[1] - quad.positions[0], quad.positions[3] - quad.positions[0]));
            out.push_back(quad);
        }
    }
}

glm::vec3 snapToPixel(glm::vec3 position, const glm::mat4& viewProjection, glm::vec3 cameraRight,
                      glm::vec3 cameraUp, glm::ivec2 target) {
    const glm::vec4 clip = viewProjection * glm::vec4(position, 1);
    if (clip.w <= 1e-4f || target.x < 1 || target.y < 1) {
        return position;
    }
    const glm::vec2 half = glm::vec2(target) * 0.5f;
    const glm::vec2 pixel = glm::vec2(clip) / clip.w * half + half;
    const glm::vec2 offset = (glm::floor(pixel) + 0.5f - pixel) / half; // in NDC
    // How far one NDC unit moves along each screen axis at this depth.
    const auto ndcPerUnit = [&](glm::vec3 axis) {
        const glm::vec4 moved = viewProjection * glm::vec4(position + axis, 1);
        return glm::vec2(moved) / moved.w - glm::vec2(clip) / clip.w;
    };
    const glm::vec2 alongRight = ndcPerUnit(cameraRight), alongUp = ndcPerUnit(cameraUp);
    const float det = alongRight.x * alongUp.y - alongUp.x * alongRight.y;
    if (std::abs(det) < 1e-12f) {
        return position;
    }
    const float a = (offset.x * alongUp.y - alongUp.x * offset.y) / det;
    const float b = (alongRight.x * offset.y - offset.x * alongRight.y) / det;
    return position + cameraRight * a + cameraUp * b;
}
} // namespace fe
