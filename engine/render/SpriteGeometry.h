#pragma once
#include "engine/nav/Grid.h"
#include <array>
#include <glm/glm.hpp>
#include <vector>

namespace fe {
// One piece of a decal lying on one grid cell's surface plane. Corners are
// bottom-left, bottom-right, top-right, top-left as seen from above with
// north (-z) at the top, like a flat sprite.
struct DecalQuad {
    std::array<glm::vec3, 4> positions{};
    std::array<glm::vec2, 4> uv{};
    glm::vec3 normal{0, 1, 0};
};

// Projects the XZ rectangle `min`..`max` (min.y is its north edge, which
// shows the top of the image) onto the surface of `layer`, following heights
// and ramps: the rectangle is cut at cell borders and each piece lies on its
// cell's plane, `lift` above it. `uv0` maps to the north-west corner and
// `uv1` to the south-east one. Cells the layer lacks (or that are not walkable on an upper
// layer) fall back to layer 0;
// pieces outside the grid are dropped.
void projectDecal(const Grid& grid, int layer, glm::vec2 min, glm::vec2 max, glm::vec2 uv0, glm::vec2 uv1,
                  float lift, std::vector<DecalQuad>& out);

// Moves `position` the least distance parallel to the screen so it projects
// onto the centre of a whole pixel of a `target`-sized image. `cameraRight`
// and `cameraUp` span the screen plane; `viewProjection` maps to clip space.
glm::vec3 snapToPixel(glm::vec3 position, const glm::mat4& viewProjection, glm::vec3 cameraRight,
                      glm::vec3 cameraUp, glm::ivec2 target);
} // namespace fe
