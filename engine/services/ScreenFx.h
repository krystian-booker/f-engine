#pragma once
#include <glm/glm.hpp>
#include <string>

namespace fe {
// Full-screen effects. Scripts animate the values; renderers apply them.
struct ScreenFx {
    glm::vec4 fade{0, 0, 0, 0};  // rgb + amount, drawn over everything
    glm::vec4 flash{1, 1, 1, 0}; // rgb + amount, additive
    glm::vec4 tint{1, 1, 1, 0};  // rgb + amount, multiplies the 3D scene
    float letterbox = 0;         // 0..1 fraction of the maximum bar height
    struct Transition {
        std::string kind = "none"; // none | swirl | mosaic | wipe | iris
        float progress = 0;
        glm::vec2 centre{0.5f};
        glm::vec4 color{0, 0, 0, 1};
    } transition;
};
} // namespace fe
