#pragma once
#include "engine/editor/Selection.h"
#include "engine/nav/Query.h"
#include "engine/world/Camera.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace fe {
class World;

// The Scene viewport in window pixels, and the mapping between pixels and
// normalized device coordinates the camera uses.
struct ViewRect {
    glm::ivec4 pixels{0, 0, 1, 1}; // x, y, width, height
    [[nodiscard]] float aspect() const {
        return static_cast<float>(std::max(1, pixels.z)) / static_cast<float>(std::max(1, pixels.w));
    }
    [[nodiscard]] bool contains(glm::vec2 px) const {
        return px.x >= static_cast<float>(pixels.x) && px.y >= static_cast<float>(pixels.y) &&
               px.x < static_cast<float>(pixels.x + pixels.z) && px.y < static_cast<float>(pixels.y + pixels.w);
    }
    [[nodiscard]] glm::vec2 ndc(glm::vec2 px) const {
        return {(px.x - static_cast<float>(pixels.x)) / static_cast<float>(std::max(1, pixels.z)) * 2 - 1,
                1 - (px.y - static_cast<float>(pixels.y)) / static_cast<float>(std::max(1, pixels.w)) * 2};
    }
    [[nodiscard]] glm::vec2 toPixels(glm::vec2 ndc) const {
        return {static_cast<float>(pixels.x) + (ndc.x + 1) * 0.5f * static_cast<float>(pixels.z),
                static_cast<float>(pixels.y) + (1 - ndc.y) * 0.5f * static_cast<float>(pixels.w)};
    }
};

struct PickOptions {
    const Camera* camera = nullptr; // the editor camera; null = the world's
    double alpha = 1;
    int pixelsPerUnit = 32;
    std::uint32_t layers = ~0u; // grid layers hit by ground rays (bit per layer)
    bool placements = true, zones = true, points = true, lights = true, emitters = true, colliders = true,
         entities = true;
    float handleRadiusPixels = 10; // how close to a handle's icon counts as hitting it
};

struct ViewRay {
    glm::vec3 origin{0}, direction{0, 0, -1};
};
// The world ray under a viewport pixel.
[[nodiscard]] ViewRay viewRay(const Camera& camera, const ViewRect& view, glm::vec2 mousePx, double alpha);
// The grid surface under a viewport pixel.
[[nodiscard]] std::optional<GridHit> pickGround(const World& world, const Camera& camera, const ViewRect& view,
                                                glm::vec2 mousePx, double alpha, std::uint32_t layers = ~0u);
// A world point's viewport pixel, or nullopt when it is behind the camera.
[[nodiscard]] std::optional<glm::vec2> projectToView(const Camera& camera, const ViewRect& view, glm::vec3 point,
                                                     double alpha);

// What is under a viewport pixel: handles (points, lights, emitters, and
// placements that have no entity) by screen distance first, then the world
// ray (entities, their placements, colliders, meshes by their bounds), then
// the zone under the ground hit. Empty ground gives nullopt.
[[nodiscard]] std::optional<Selection> pickScene(World& world, const ViewRect& view, glm::vec2 mousePx,
                                                 const PickOptions& options);
} // namespace fe
