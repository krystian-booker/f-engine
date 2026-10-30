#pragma once
#include "engine/world/MapData.h"
#include <array>
#include <glm/glm.hpp>
#include <vector>

namespace fe {
// The camera a shadow map is fitted to (without screen shake, so shaking the
// camera never moves shadows).
struct ShadowCamera {
    glm::vec3 position{0}, forward{0, 0, -1}, right{1, 0, 0}, up{0, 1, 0};
    float fovY = 0.5f; // radians
    float aspect = 1, nearPlane = 0.1f, farPlane = 50;
    // Orthographic view: a box `orthoHeight` high (times aspect wide) instead
    // of a frustum; fovY is then ignored.
    bool orthographic = false;
    float orthoHeight = 10;
    // Where cascade splits start growing; the first cascade still reaches back to nearPlane.
    float splitStart = 1;
};

struct ShadowCascade {
    glm::mat4 view{1}, projection{1}; // for rendering the cascade (backend depth convention)
    // World position to (cascade texture u, v, compare depth): u and v in 0..1
    // of this cascade's tile, depth already biased.
    glm::mat4 sample{1};
    glm::vec3 centre{0};
    float radius = 0;
};

struct ShadowSettings {
    glm::vec3 sunDirection{0, -1, 0};
    glm::vec3 sceneMin{0}, sceneMax{0}; // everything that may cast into view
    int resolution = 2048;              // texels per cascade side
    float bias = 0.03f;                 // world units
    bool homogeneousDepth = false, originBottomLeft = false;
};

constexpr int ShadowCascadeCount = 3;
using ShadowCascades = std::array<ShadowCascade, ShadowCascadeCount>;

// View distances bounding each cascade: near, two splits, far.
std::array<float, ShadowCascadeCount + 1> cascadeSplits(const ShadowCamera& camera);

// Fits each cascade's orthographic box to a bounding sphere of its slice of
// the view frustum. The sphere radius depends only on the projection, so it
// does not change as the camera moves or turns, and the box centre is
// snapped to whole shadow texels in light space. Together these keep shadow
// edges from crawling.
ShadowCascades fitShadowCascades(const ShadowCamera& camera, const ShadowSettings& settings);

// Point and spot light shadows share one depth atlas: a row of square tiles.
// A spot light takes one tile, a point light six (a cube, one tile per face).
constexpr int LightShadowTiles = 16;
constexpr int LightShadowTileResolution = 512;

struct LightShadowCandidate {
    float priority = 0; // 0 = casts no shadow this frame
    bool point = true;
};

struct LightShadowSlot {
    std::size_t light = 0; // index into the candidates
    int firstTile = 0, tiles = 1;
};

// How much a light's shadow matters this frame: 0 unless it has `castShadows`;
// otherwise its brightness (flicker ignored, so shadows never strobe) falling
// off with its distance beyond its radius from `focus`.
float lightShadowPriority(const LightSettings& light, glm::vec3 focus);

// Gives tiles to the highest-priority lights first (ties keep candidate
// order). A light that does not fit in the tiles left is skipped, so a
// cheaper spot light further down may still get one. The first form writes
// into `slots` and uses `order` as scratch (nothing allocated once warm).
void chooseShadowLights(const std::vector<LightShadowCandidate>& candidates, std::vector<LightShadowSlot>& slots,
                        std::vector<std::size_t>& order, int tileBudget = LightShadowTiles);
std::vector<LightShadowSlot> chooseShadowLights(const std::vector<LightShadowCandidate>& candidates,
                                                int tileBudget = LightShadowTiles);

struct LightShadowView {
    glm::mat4 view{1}, projection{1}; // for rendering the tile (backend depth convention)
    // World position to (atlas u, v, device depth) times w: divide by w.
    glm::mat4 sample{1};
};

// The cube face a direction from a point light falls on: +x, -x, +y, -y, +z, -z.
int cubeFace(glm::vec3 direction);

// One view per tile: a spot light's cone, or a point light's cube faces in
// cubeFace order. Faces are a little wider than 90 degrees so filtering near
// their edges stays inside the rendered area. The first form appends to `views`.
void lightShadowViews(const LightSettings& light, int firstTile, bool homogeneousDepth, bool originBottomLeft,
                      std::vector<LightShadowView>& views);
std::vector<LightShadowView> lightShadowViews(const LightSettings& light, int firstTile,
                                              bool homogeneousDepth, bool originBottomLeft);

// World size of one shadow texel per unit of distance from the light.
float lightShadowTexelScale(const LightSettings& light);
} // namespace fe
