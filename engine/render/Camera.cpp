#include "engine/render/Camera.h"
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <cmath>

namespace fe {
void Camera::update(glm::vec3 target, const CameraSettings& settings, double dt, bool snap) {
    previousTarget_=target_;
    settings_ = settings;
    target.x = glm::clamp(target.x,settings.min.x,settings.max.x);
    target.z = glm::clamp(target.z,settings.min.y,settings.max.y);
    target.y += 0.65f;
    if (snap || !initialized_) target_ = previousTarget_ = target;
    else target_ = glm::mix(target_,target,static_cast<float>(1-std::exp(-7*dt)));
    initialized_ = true;
}
glm::vec3 Camera::position(double alpha) const {
    const float pitch = glm::radians(settings_.pitch), yaw = glm::radians(settings_.yaw);
    return glm::mix(previousTarget_,target_,static_cast<float>(glm::clamp(alpha,0.0,1.0)))+visualOffset_ + glm::vec3(std::sin(yaw)*std::cos(pitch),std::sin(pitch),std::cos(yaw)*std::cos(pitch))*settings_.distance;
}
glm::mat4 Camera::view(double alpha) const {return glm::lookAtRH(position(alpha),glm::mix(previousTarget_,target_,static_cast<float>(glm::clamp(alpha,0.0,1.0)))+visualOffset_,glm::vec3(0,1,0));}
glm::mat4 Camera::projection(float aspect, bool homogeneous) const {
    return homogeneous ? glm::perspectiveRH_NO(glm::radians(settings_.fov),aspect,0.1f,150.0f)
        : glm::perspectiveRH_ZO(glm::radians(settings_.fov),aspect,0.1f,150.0f);
}
glm::vec3 Camera::right() const { return {std::cos(glm::radians(settings_.yaw)),0,-std::sin(glm::radians(settings_.yaw))}; }
}
