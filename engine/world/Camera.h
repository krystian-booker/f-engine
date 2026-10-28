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
    glm::vec3 shake{0}; // visual offset, not interpolated

    void beginTick();
    void snap(); // drop interpolation after a cut
    [[nodiscard]] glm::vec3 position(double alpha = 1) const;
    [[nodiscard]] glm::vec3 focus(double alpha = 1) const;
    [[nodiscard]] glm::mat4 view(double alpha = 1) const;
    [[nodiscard]] glm::mat4 projection(float aspect, bool homogeneousDepth) const;
    [[nodiscard]] glm::vec3 right(double alpha = 1) const;
    [[nodiscard]] glm::vec3 forward(double alpha = 1) const;
    [[nodiscard]] float interpolatedYaw(double alpha = 1) const;
    // Normalized device coords (-1..1, y up) for a world point; z > 1 means behind.
    [[nodiscard]] glm::vec3 project(glm::vec3 point, float aspect) const;
    // World-space ray through normalized device coords.
    void ray(glm::vec2 ndc, float aspect, glm::vec3& origin, glm::vec3& direction) const;

private:
    struct State {
        glm::vec3 target{0};
        float yaw = 0, pitch = 42, distance = 17, fov = 30;
    };
    [[nodiscard]] State blend(double alpha) const;
    State previous_;
    bool initialized_ = false;
};
} // namespace fe
