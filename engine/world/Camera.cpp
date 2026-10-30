#include "engine/world/Camera.h"
#include <algorithm>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace fe {
namespace {
// Interpolates along the shorter arc, whatever the winding of a and b.
float lerpAngle(float a, float b, float t) {
    float delta = std::fmod(b - a, 360.0f);
    if (delta > 180) {
        delta -= 360;
    } else if (delta < -180) {
        delta += 360;
    }
    return a + delta * t;
}
} // namespace

float Camera::orthoHeightFor(float distance, float fov, float orthoHeight) {
    if (orthoHeight > 0) {
        return orthoHeight;
    }
    return 2.0f * distance * std::tan(glm::radians(std::clamp(fov, 1.0f, 170.0f)) * 0.5f);
}

Camera::State Camera::current() const {
    return {target, yaw, pitch, distance, fov, orthoHeightFor(distance, fov, orthoHeight)};
}

void Camera::beginTick() {
    if (!initialized_) {
        snap();
        return;
    }
    previous_ = current();
}

void Camera::snap() {
    previous_ = current();
    initialized_ = true;
}

Camera::State Camera::blend(double alpha) const {
    if (!initialized_) {
        return current();
    }
    const auto t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    const auto now = current();
    return {glm::mix(previous_.target, now.target, t),
            lerpAngle(previous_.yaw, now.yaw, t),
            glm::mix(previous_.pitch, now.pitch, t),
            glm::mix(previous_.distance, now.distance, t),
            glm::mix(previous_.fov, now.fov, t),
            glm::mix(previous_.orthoHeight, now.orthoHeight, t)};
}

float Camera::interpolatedYaw(double alpha) const {
    return blend(alpha).yaw;
}

float Camera::interpolatedFov(double alpha) const {
    return blend(alpha).fov;
}

float Camera::interpolatedDistance(double alpha) const {
    return blend(alpha).distance;
}

float Camera::viewHeight(double alpha) const {
    const auto s = blend(alpha);
    if (orthographic) {
        return s.orthoHeight;
    }
    return orthoHeightFor(s.distance, s.fov, 0);
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

glm::mat4 Camera::projection(float aspect, bool homogeneousDepth, double alpha) const {
    if (orthographic) {
        const float h = blend(alpha).orthoHeight * 0.5f, w = h * aspect;
        return homogeneousDepth ? glm::orthoRH_NO(-w, w, -h, h, nearPlane, farPlane)
                                : glm::orthoRH_ZO(-w, w, -h, h, nearPlane, farPlane);
    }
    const float f = glm::radians(std::clamp(blend(alpha).fov, 1.0f, 170.0f));
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

glm::vec3 Camera::project(glm::vec3 point, float aspect, double alpha) const {
    const auto viewMatrix = view(alpha);
    const auto clip = projection(aspect, true, alpha) * viewMatrix * glm::vec4(point, 1);
    if (clip.w <= 1e-6f) {
        return {0, 0, 2};
    }
    auto ndc = glm::vec3(clip) / clip.w;
    // An orthographic projection has no vanishing point: a point behind the
    // eye still lands on the screen, so it is flagged the way a perspective
    // one flags a point behind its near plane.
    if (orthographic && (viewMatrix * glm::vec4(point, 1)).z > 0) {
        ndc.z = 2;
    }
    return ndc;
}

void Camera::ray(glm::vec2 ndc, float aspect, glm::vec3& origin, glm::vec3& direction, double alpha) const {
    const auto inverse = glm::inverse(projection(aspect, true, alpha) * view(alpha));
    auto nearPoint = inverse * glm::vec4(ndc, -1, 1);
    auto farPoint = inverse * glm::vec4(ndc, 1, 1);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    origin = glm::vec3(nearPoint);
    direction = glm::normalize(glm::vec3(farPoint - nearPoint));
}
} // namespace fe
