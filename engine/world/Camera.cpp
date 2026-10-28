#include "engine/world/Camera.h"
#include <algorithm>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace fe {
namespace {
float lerpAngle(float a, float b, float t) {
    float delta = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return a + delta * t;
}
} // namespace

void Camera::beginTick() {
    if (!initialized_) {
        snap();
        return;
    }
    previous_ = {target, yaw, pitch, distance, fov};
}

void Camera::snap() {
    previous_ = {target, yaw, pitch, distance, fov};
    initialized_ = true;
}

Camera::State Camera::blend(double alpha) const {
    if (!initialized_) {
        return {target, yaw, pitch, distance, fov};
    }
    const auto t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    return {glm::mix(previous_.target, target, t), lerpAngle(previous_.yaw, yaw, t),
            glm::mix(previous_.pitch, pitch, t), glm::mix(previous_.distance, distance, t),
            glm::mix(previous_.fov, fov, t)};
}

float Camera::interpolatedYaw(double alpha) const {
    return blend(alpha).yaw;
}

glm::vec3 Camera::focus(double alpha) const {
    return blend(alpha).target + shake;
}

glm::vec3 Camera::position(double alpha) const {
    const auto s = blend(alpha);
    const float p = glm::radians(std::clamp(s.pitch, -89.0f, 89.9f));
    const float y = glm::radians(s.yaw);
    return s.target + shake +
           glm::vec3(std::sin(y) * std::cos(p), std::sin(p), std::cos(y) * std::cos(p)) * s.distance;
}

glm::mat4 Camera::view(double alpha) const {
    const auto eye = position(alpha);
    const auto at = focus(alpha);
    const auto up = std::abs(blend(alpha).pitch) > 89.5f ? glm::vec3(0, 0, -1) : glm::vec3(0, 1, 0);
    return glm::lookAtRH(eye, at, up);
}

glm::mat4 Camera::projection(float aspect, bool homogeneousDepth) const {
    const float f = glm::radians(std::clamp(fov, 1.0f, 170.0f));
    return homogeneousDepth ? glm::perspectiveRH_NO(f, aspect, nearPlane, farPlane)
                            : glm::perspectiveRH_ZO(f, aspect, nearPlane, farPlane);
}

glm::vec3 Camera::right(double alpha) const {
    const float y = glm::radians(blend(alpha).yaw);
    return {std::cos(y), 0, -std::sin(y)};
}

glm::vec3 Camera::forward(double alpha) const {
    return glm::normalize(focus(alpha) - position(alpha));
}

glm::vec3 Camera::project(glm::vec3 point, float aspect) const {
    const auto clip = projection(aspect, true) * view() * glm::vec4(point, 1);
    if (clip.w <= 1e-6f) {
        return {0, 0, 2};
    }
    return glm::vec3(clip) / clip.w;
}

void Camera::ray(glm::vec2 ndc, float aspect, glm::vec3& origin, glm::vec3& direction) const {
    const auto inverse = glm::inverse(projection(aspect, true) * view());
    auto nearPoint = inverse * glm::vec4(ndc, -1, 1);
    auto farPoint = inverse * glm::vec4(ndc, 1, 1);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    origin = glm::vec3(nearPoint);
    direction = glm::normalize(glm::vec3(farPoint - nearPoint));
}
} // namespace fe
