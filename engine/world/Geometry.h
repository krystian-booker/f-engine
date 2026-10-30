#pragma once
#include "engine/world/MapData.h"
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <optional>

// Ray and bounds helpers shared by collision, rendering (culling) and the
// editor's picking. Header-only, no dependencies beyond glm and Box.
namespace fe {
// Narrows [near, far] to where a ray (start + t * step along one axis) lies
// inside the slab lo..hi; false when it misses the slab.
inline bool clipSlab(float start, float step, float lo, float hi, float& near, float& far) {
    if (std::abs(step) < 1e-9f) {
        return start >= lo && start <= hi;
    }
    float a = (lo - start) / step, b = (hi - start) / step;
    if (a > b) {
        std::swap(a, b);
    }
    near = std::max(near, a);
    far = std::min(far, b);
    return near <= far;
}

// Entry distance of a ray into an axis-aligned box (0 when it starts inside).
inline std::optional<float> rayBox(glm::vec3 o, glm::vec3 d, const Box& box, float maxDistance) {
    float near = 0, far = maxDistance;
    for (int axis = 0; axis < 3; ++axis) {
        if (!clipSlab(o[axis], d[axis], box.min[axis], box.max[axis], near, far)) {
            return std::nullopt;
        }
    }
    return near;
}

// Entry distance of a ray into an upright cylinder standing on `feet`.
inline std::optional<float> rayCylinder(glm::vec3 o, glm::vec3 d, glm::vec3 feet, float radius, float height,
                                        float maxDistance) {
    float near = 0, far = maxDistance;
    if (!clipSlab(o.y, d.y, feet.y, feet.y + height, near, far)) {
        return std::nullopt;
    }
    const glm::vec2 p(o.x - feet.x, o.z - feet.z), v(d.x, d.z);
    const float a = glm::dot(v, v), b = glm::dot(p, v), c = glm::dot(p, p) - radius * radius;
    if (a < 1e-12f) {
        return c <= 0 ? std::optional(near) : std::nullopt;
    }
    const float disc = b * b - a * c;
    if (disc < 0) {
        return std::nullopt;
    }
    const float root = std::sqrt(disc);
    const float t0 = (-b - root) / a, t1 = (-b + root) / a;
    near = std::max(near, t0);
    far = std::min(far, t1);
    return near <= far ? std::optional(near) : std::nullopt;
}

// Entry distance of a ray into a sphere (0 when it starts inside).
inline std::optional<float> raySphere(glm::vec3 o, glm::vec3 d, glm::vec3 centre, float radius,
                                      float maxDistance) {
    const auto p = o - centre;
    const float a = glm::dot(d, d), b = glm::dot(p, d), c = glm::dot(p, p) - radius * radius;
    if (a < 1e-12f) {
        return std::nullopt;
    }
    const float disc = b * b - a * c;
    if (disc < 0) {
        return std::nullopt;
    }
    const float root = std::sqrt(disc);
    const float t0 = std::max(0.0f, (-b - root) / a), t1 = (-b + root) / a;
    return t0 <= t1 && t0 <= maxDistance ? std::optional(t0) : std::nullopt;
}

// The axis-aligned bounds of a box's eight corners after a transform.
inline void transformBounds(const glm::mat4& m, glm::vec3 min, glm::vec3 max, glm::vec3& outMin,
                            glm::vec3& outMax) {
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = -outMin;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z);
        const auto p = glm::vec3(m * glm::vec4(corner, 1));
        outMin = glm::min(outMin, p);
        outMax = glm::max(outMax, p);
    }
}
} // namespace fe
