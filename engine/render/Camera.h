#pragma once
#include "engine/world/Map.h"
#include <glm/glm.hpp>

namespace fe {
class Camera {
public:
    void update(glm::vec3 target, const CameraSettings& settings, double dt, bool snap = false);
    [[nodiscard]] glm::mat4 view(double alpha=1) const;
    [[nodiscard]] glm::mat4 projection(float aspect, bool homogeneousDepth) const;
    [[nodiscard]] glm::vec3 position(double alpha=1) const;
    [[nodiscard]] glm::vec3 target() const { return target_; }
    [[nodiscard]] glm::vec3 right() const;
    [[nodiscard]] float yaw() const { return settings_.yaw; }
    [[nodiscard]] const CameraSettings& settings() const { return settings_; }
    void setVisualOffset(glm::vec3 offset) {visualOffset_=offset;}
private:
    CameraSettings settings_;
    glm::vec3 target_{0};
    glm::vec3 previousTarget_{0};
    glm::vec3 visualOffset_{0};
    bool initialized_ = false;
};
}
