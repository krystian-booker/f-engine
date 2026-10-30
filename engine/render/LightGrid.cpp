#include "engine/render/LightGrid.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace fe {
float lightScore(const LightSettings& light, glm::vec2 min, glm::vec2 max) {
    const glm::vec2 centre(light.position.x, light.position.z);
    const float distance = glm::length(centre - glm::clamp(centre, min, max));
    if (distance >= light.radius) {
        return 0;
    }
    const float falloff = 1 - distance / light.radius;
    const float brightness = std::max({light.color.r, light.color.g, light.color.b});
    return light.intensity * brightness * falloff * falloff;
}

void LightGrid::build(const std::vector<SceneLight>& lights, glm::vec2 boundsMin, glm::vec2 boundsMax,
                      glm::vec3 focus) {
    const glm::vec2 extent = glm::max(boundsMax - boundsMin, glm::vec2(MinCellSize));
    cellSize = std::max(MinCellSize, std::max(extent.x, extent.y) / static_cast<float>(MaxCells));
    origin = boundsMin;
    cells = glm::clamp(glm::ivec2(glm::ceil(extent / cellSize)), glm::ivec2(1), glm::ivec2(MaxCells));

    auto& chosen = chosen_;
    chosen.resize(lights.size());
    std::iota(chosen.begin(), chosen.end(), 0);
    if (chosen.size() > static_cast<std::size_t>(MaxLights)) {
        auto reach = [&](std::size_t i) {
            const auto& l = lights[i].settings;
            return glm::length(l.position - focus) - l.radius;
        };
        // Ties keep input order (the index breaks them), without stable_sort's buffer.
        std::sort(chosen.begin(), chosen.end(), [&](std::size_t a, std::size_t b) {
            const float ra = reach(a), rb = reach(b);
            return ra != rb ? ra < rb : a < b;
        });
        chosen.resize(MaxLights);
        std::sort(chosen.begin(), chosen.end());
    }
    lightCount = static_cast<int>(chosen.size());
    sources.assign(chosen.begin(), chosen.end());

    lightTexels.assign(static_cast<std::size_t>(MaxLights) * Rows, glm::vec4(0));
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        const auto& light = lights[chosen[i]];
        const auto& l = light.settings;
        const bool spot = l.type == LightType::Spot;
        const float direction = glm::length(l.direction);
        lightTexels[i] = glm::vec4(l.position, l.radius);
        lightTexels[MaxLights + i] =
            glm::vec4(l.color * light.intensity, spot ? std::cos(glm::radians(l.innerCone)) : -2.0f);
        lightTexels[MaxLights * 2 + i] =
            glm::vec4(direction > 0 ? l.direction / direction : glm::vec3(0, -1, 0),
                      std::cos(glm::radians(l.outerCone)));
        lightTexels[MaxLights * 3 + i] = glm::vec4(-1, 0, 0, 0);
    }

    cellTexels.assign(static_cast<std::size_t>(cells.x * cells.y * MaxPerCell), -1.0f);
    auto& candidates = candidates_;
    candidates.clear();
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        const auto& l = lights[chosen[i]].settings;
        const glm::vec2 centre(l.position.x, l.position.z);
        const auto lo = cellOf(centre - glm::vec2(l.radius));
        const auto hi = cellOf(centre + glm::vec2(l.radius));
        for (int z = lo.y; z <= hi.y; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                const glm::vec2 min = origin + glm::vec2(x, z) * cellSize;
                if (const float score = lightScore(l, min, min + glm::vec2(cellSize)); score > 0) {
                    candidates.push_back({z * cells.x + x, score, static_cast<int>(i)});
                }
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.cell != b.cell) {
            return a.cell < b.cell;
        }
        return a.score != b.score ? a.score > b.score : a.light < b.light;
    });
    int filled = 0, previous = -1;
    for (const auto& c : candidates) {
        filled = c.cell == previous ? filled + 1 : 0;
        previous = c.cell;
        if (filled < MaxPerCell) {
            cellTexels[static_cast<std::size_t>(c.cell * MaxPerCell + filled)] = static_cast<float>(c.light);
        }
    }
}

void LightGrid::setShadow(int slot, int firstTile, int tiles, float texelScale) {
    lightTexels[static_cast<std::size_t>(MaxLights * 3 + slot)] =
        glm::vec4(static_cast<float>(firstTile), static_cast<float>(tiles), texelScale, 0);
}

std::vector<int> LightGrid::cell(int x, int z) const {
    std::vector<int> out;
    const auto* in = &cellTexels[static_cast<std::size_t>((z * cells.x + x) * MaxPerCell)];
    for (int k = 0; k < MaxPerCell && in[k] >= 0; ++k) {
        out.push_back(static_cast<int>(in[k]));
    }
    return out;
}

glm::ivec2 LightGrid::cellOf(glm::vec2 position) const {
    return glm::clamp(glm::ivec2(glm::floor((position - origin) / cellSize)), glm::ivec2(0), cells - 1);
}
} // namespace fe
