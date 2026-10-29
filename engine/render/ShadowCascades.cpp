#include "engine/render/ShadowCascades.h"
#include <algorithm>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <numeric>

namespace fe {
std::array<float, ShadowCascadeCount + 1> cascadeSplits(const ShadowCamera& camera) {
    const float start = std::clamp(camera.splitStart, camera.nearPlane, camera.farPlane);
    std::array<float, ShadowCascadeCount + 1> splits{};
    splits.front() = camera.nearPlane;
    splits.back() = camera.farPlane;
    for (int i = 1; i < ShadowCascadeCount; ++i) {
        const float t = static_cast<float>(i) / ShadowCascadeCount;
        const float logarithmic = start * std::pow(camera.farPlane / start, t);
        const float linear = start + (camera.farPlane - start) * t;
        splits[static_cast<std::size_t>(i)] = glm::mix(linear, logarithmic, 0.5f);
    }
    return splits;
}

ShadowCascades fitShadowCascades(const ShadowCamera& camera, const ShadowSettings& settings) {
    const auto splits = cascadeSplits(camera);
    const float tanY = std::tan(camera.fovY * 0.5f), tanX = tanY * camera.aspect;
    const auto sun = glm::normalize(settings.sunDirection);
    const auto lightView =
        glm::lookAtRH(glm::vec3(0), sun, std::abs(sun.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0));
    // Casters closest to the sun bound the near plane of every cascade.
    float casterTop = -1e30f;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? settings.sceneMax.x : settings.sceneMin.x,
                               (i & 2) ? settings.sceneMax.y : settings.sceneMin.y,
                               (i & 4) ? settings.sceneMax.z : settings.sceneMin.z);
        casterTop = std::max(casterTop, (lightView * glm::vec4(corner, 1)).z);
    }
    ShadowCascades cascades{};
    for (int c = 0; c < ShadowCascadeCount; ++c) {
        auto& cascade = cascades[static_cast<std::size_t>(c)];
        const float d0 = splits[static_cast<std::size_t>(c)], d1 = splits[static_cast<std::size_t>(c) + 1];
        std::array<glm::vec3, 8> corners{};
        glm::vec3 centre(0);
        for (int i = 0; i < 8; ++i) {
            const float d = (i & 4) ? d1 : d0;
            corners[static_cast<std::size_t>(i)] = camera.position + camera.forward * d +
                                                   camera.right * (((i & 1) ? 1.0f : -1.0f) * d * tanX) +
                                                   camera.up * (((i & 2) ? 1.0f : -1.0f) * d * tanY);
            centre += corners[static_cast<std::size_t>(i)] / 8.0f;
        }
        float radius = 0;
        for (const auto& corner : corners) {
            radius = std::max(radius, glm::length(corner - centre));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;
        const float texel = radius * 2.0f / static_cast<float>(settings.resolution);
        glm::vec3 light = glm::vec3(lightView * glm::vec4(centre, 1));
        light.x = std::floor(light.x / texel) * texel;
        light.y = std::floor(light.y / texel) * texel;
        const float nearPlane = -std::max(light.z + radius, casterTop) - 1.0f;
        const float farPlane = -(light.z - radius) + 1.0f;
        const float l = light.x - radius, r = light.x + radius, b = light.y - radius, t = light.y + radius;
        cascade.view = lightView;
        cascade.projection = settings.homogeneousDepth ? glm::orthoRH_NO(l, r, b, t, nearPlane, farPlane)
                                                       : glm::orthoRH_ZO(l, r, b, t, nearPlane, farPlane);
        const float depthBias = settings.bias / (farPlane - nearPlane);
        glm::mat4 toTexture(1);
        toTexture[0][0] = 0.5f;
        toTexture[1][1] = settings.originBottomLeft ? 0.5f : -0.5f;
        toTexture[3] = glm::vec4(0.5f, 0.5f, -depthBias, 1);
        cascade.sample = toTexture * glm::orthoRH_ZO(l, r, b, t, nearPlane, farPlane) * lightView;
        cascade.centre = glm::vec3(glm::inverse(lightView) * glm::vec4(light, 1));
        cascade.radius = radius;
    }
    return cascades;
}

namespace {
constexpr float MaxSpotFov = glm::radians(170.0f);
// Texels of overlap around each cube face.
constexpr float FaceMargin = 4.0f;

float lightFov(const LightSettings& light) {
    if (light.type == LightType::Point) {
        return 2.0f * std::atan(1.0f + 2.0f * FaceMargin / LightShadowTileResolution);
    }
    return std::min(MaxSpotFov, glm::radians(std::max(light.outerCone, light.innerCone)) * 2.0f + 0.05f);
}

glm::mat4 perspective(float fov, float nearPlane, float farPlane, bool homogeneousDepth) {
    return homogeneousDepth ? glm::perspectiveRH_NO(fov, 1.0f, nearPlane, farPlane)
                            : glm::perspectiveRH_ZO(fov, 1.0f, nearPlane, farPlane);
}
} // namespace

float lightShadowPriority(const LightSettings& light, glm::vec3 focus) {
    if (!light.castShadows || light.intensity <= 0) {
        return 0;
    }
    const float brightness = std::max({light.color.r, light.color.g, light.color.b});
    const float beyond = std::max(0.0f, glm::length(light.position - focus) - light.radius);
    return light.intensity * brightness / (1.0f + beyond);
}

std::vector<LightShadowSlot> chooseShadowLights(const std::vector<LightShadowCandidate>& candidates,
                                                int tileBudget) {
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return candidates[a].priority > candidates[b].priority;
    });
    std::vector<LightShadowSlot> slots;
    int next = 0;
    for (const auto index : order) {
        const auto& candidate = candidates[index];
        const int tiles = candidate.point ? 6 : 1;
        if (candidate.priority <= 0 || next + tiles > tileBudget) {
            continue;
        }
        slots.push_back({index, next, tiles});
        next += tiles;
    }
    return slots;
}

int cubeFace(glm::vec3 d) {
    const glm::vec3 a = glm::abs(d);
    if (a.x >= a.y && a.x >= a.z) {
        return d.x >= 0 ? 0 : 1;
    }
    if (a.y >= a.z) {
        return d.y >= 0 ? 2 : 3;
    }
    return d.z >= 0 ? 4 : 5;
}

std::vector<LightShadowView> lightShadowViews(const LightSettings& light, int firstTile,
                                              bool homogeneousDepth, bool originBottomLeft) {
    static const std::array<std::pair<glm::vec3, glm::vec3>, 6> faces = {{{{1, 0, 0}, {0, -1, 0}},
                                                                          {{-1, 0, 0}, {0, -1, 0}},
                                                                          {{0, 1, 0}, {0, 0, 1}},
                                                                          {{0, -1, 0}, {0, 0, -1}},
                                                                          {{0, 0, 1}, {0, -1, 0}},
                                                                          {{0, 0, -1}, {0, -1, 0}}}};
    const float fov = lightFov(light);
    const float farPlane = std::max(light.radius, 0.1f);
    const float nearPlane = std::clamp(farPlane * 0.01f, 0.02f, 0.25f);
    std::vector<std::pair<glm::vec3, glm::vec3>> directions;
    if (light.type == LightType::Point) {
        directions.assign(faces.begin(), faces.end());
    } else {
        const auto forward = glm::normalize(light.direction);
        directions.emplace_back(forward,
                                std::abs(forward.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0));
    }
    std::vector<LightShadowView> views;
    for (std::size_t i = 0; i < directions.size(); ++i) {
        const auto& [forward, up] = directions[i];
        LightShadowView out;
        out.view = glm::lookAtRH(light.position, light.position + forward, up);
        out.projection = perspective(fov, nearPlane, farPlane, homogeneousDepth);
        const float tile = static_cast<float>(firstTile + static_cast<int>(i));
        glm::mat4 toTile(1);
        toTile[0][0] = 0.5f / LightShadowTiles;
        toTile[1][1] = originBottomLeft ? 0.5f : -0.5f;
        toTile[3] = glm::vec4((tile + 0.5f) / LightShadowTiles, 0.5f, 0, 1);
        out.sample = toTile * glm::perspectiveRH_ZO(fov, 1.0f, nearPlane, farPlane) * out.view;
        views.push_back(out);
    }
    return views;
}

float lightShadowTexelScale(const LightSettings& light) {
    return 2.0f * std::tan(lightFov(light) * 0.5f) / LightShadowTileResolution;
}
} // namespace fe
