#include "engine/editor/EditorCamera.h"
#include <algorithm>
#include <cmath>

namespace fe {
void EditorCamera::adopt(const Camera& game) {
    camera.target = game.focus();
    camera.yaw = game.yaw;
    camera.pitch = std::clamp(game.pitch, MinPitch, MaxPitch);
    camera.distance = std::clamp(game.interpolatedDistance(), MinDistance, MaxDistance);
    camera.fov = game.fov;
    camera.nearPlane = game.nearPlane;
    camera.farPlane = game.farPlane;
    camera.orthographic = game.orthographic;
    camera.orthoHeight = game.orthoHeight;
    camera.shake = glm::vec3(0);
    camera.snap();
}

void EditorCamera::orbit(glm::vec2 deltaPixels) {
    camera.yaw = std::fmod(camera.yaw + deltaPixels.x * DegreesPerPixel, 360.0f);
    camera.pitch = std::clamp(camera.pitch - deltaPixels.y * DegreesPerPixel, MinPitch, MaxPitch);
    camera.snap();
}

void EditorCamera::pan(glm::vec2 deltaPixels, float viewportHeightPixels) {
    if (viewportHeightPixels <= 0) {
        return;
    }
    const float unitsPerPixel = camera.viewHeight() / viewportHeightPixels;
    const auto right = camera.right();
    auto forward = camera.forward();
    forward.y = 0;
    if (glm::length(forward) < 1e-4f) {
        forward = glm::vec3(0, 0, -1);
    }
    forward = glm::normalize(forward);
    // Dragging right moves the view right (the world goes left under the mouse).
    camera.target -= right * (deltaPixels.x * unitsPerPixel);
    camera.target += forward * (deltaPixels.y * unitsPerPixel);
    camera.snap();
}

void EditorCamera::zoom(float notches) {
    const float factor = std::pow(0.9f, notches);
    camera.distance = std::clamp(camera.distance * factor, MinDistance, MaxDistance);
    if (camera.orthographic && camera.orthoHeight > 0) {
        camera.orthoHeight = std::clamp(camera.orthoHeight * factor, 0.1f, 2 * MaxDistance);
    }
    camera.snap();
}

void EditorCamera::frame(glm::vec3 min, glm::vec3 max) {
    const auto lo = glm::min(min, max), hi = glm::max(min, max);
    camera.target = (lo + hi) * 0.5f;
    const float radius = std::max(0.5f, glm::length(hi - lo) * 0.5f);
    const float halfFov = glm::radians(std::clamp(camera.fov, 1.0f, 170.0f)) * 0.5f;
    camera.distance = std::clamp(radius / std::sin(halfFov) * 1.1f, MinDistance, MaxDistance);
    if (camera.orthographic) {
        camera.orthoHeight = std::clamp(radius * 2.2f, 0.1f, 2 * MaxDistance);
    }
    camera.snap();
}

void EditorCamera::lookAt(glm::vec3 target) {
    camera.target = target;
    camera.snap();
}

void EditorCamera::setOrthographic(bool on) {
    if (on && !camera.orthographic) {
        camera.orthoHeight = camera.effectiveOrthoHeight(); // keep the framing
    }
    camera.orthographic = on;
    camera.snap();
}
} // namespace fe
