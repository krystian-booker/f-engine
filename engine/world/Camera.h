#pragma once
#include <glm/glm.hpp>

namespace fe {
// Orbit camera math only. How the camera follows, zones, rotation input and
// cutscene paths are script behaviour that sets these values each tick.
class Camera {
public:
    glm::vec3 target{0};
    float yaw = 0, pitch = 42, distance = 17, fov = 30;
    float nearPlane = 0.1f, farPlane = 300;
    // Orthographic projection: parallel rays from the same orbit position, so
    // distance only moves the near and far planes. `orthoHeight` is the world
    // height the view covers; 0 derives it from distance and fov (the height
    // a perspective view covers at the target), so toggling keeps the framing.
    bool orthographic = false;
    float orthoHeight = 0;
    glm::vec3 shake{0}; // visual offset, not interpolated

    void beginTick();
    void snap(); // drop interpolation after a cut
    [[nodiscard]] glm::vec3 position(double alpha = 1) const;
    [[nodiscard]] glm::vec3 focus(double alpha = 1) const;
    [[nodiscard]] glm::mat4 view(double alpha = 1) const;
    [[nodiscard]] glm::mat4 projection(float aspect, bool homogeneousDepth, double alpha = 1) const;
    [[nodiscard]] glm::vec3 right(double alpha = 1) const;
    [[nodiscard]] glm::vec3 forward(double alpha = 1) const;
    [[nodiscard]] float interpolatedYaw(double alpha = 1) const;
    [[nodiscard]] float interpolatedFov(double alpha = 1) const;
    [[nodiscard]] float interpolatedDistance(double alpha = 1) const;
    // The world height the view covers at the target: orthoHeight (or its
    // derived default) when orthographic, 2 * distance * tan(fov / 2) otherwise.
    [[nodiscard]] float viewHeight(double alpha = 1) const;
    // The orthographic height in effect: orthoHeight, or the derived default.
    [[nodiscard]] float effectiveOrthoHeight() const { return orthoHeightFor(distance, fov, orthoHeight); }
    // Normalized device coords (-1..1, y up) for a world point; z > 1 means
    // behind. `alpha` selects the interpolated camera, so passing the frame's
    // draw alpha matches what the renderer shows.
    [[nodiscard]] glm::vec3 project(glm::vec3 point, float aspect, double alpha = 1) const;
    // World-space ray through normalized device coords (same `alpha` rule).
    // Orthographic rays are parallel: the origin moves with the coordinates
    // and the direction is the camera's forward.
    void ray(glm::vec2 ndc, float aspect, glm::vec3& origin, glm::vec3& direction, double alpha = 1) const;

private:
    struct State {
        glm::vec3 target{0};
        float yaw = 0, pitch = 42, distance = 17, fov = 30;
        float orthoHeight = 0; // always the effective height, so it interpolates
    };
    [[nodiscard]] static float orthoHeightFor(float distance, float fov, float orthoHeight);
    [[nodiscard]] State current() const;
    [[nodiscard]] State blend(double alpha) const;
    State previous_;
    bool initialized_ = false;
};
} // namespace fe
