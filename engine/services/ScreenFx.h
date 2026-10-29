#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace fe {
// Full-screen effects. Scripts animate the values; renderers apply them.
// Letterbox, transitions, radial blur, chromatic aberration, fade and flash
// cover the worlds and the canvas UI alike; tint only multiplies the 3D scenes.
struct ScreenFx {
    glm::vec4 fade{0, 0, 0, 0};    // rgb + amount
    glm::vec4 flash{1, 1, 1, 0};   // rgb + amount, additive
    glm::vec4 tint{1, 1, 1, 0};    // rgb + amount, multiplies the 3D scene
    float letterbox = 0;           // 0..1 fraction of the maximum bar height
    float letterboxHeight = 0.12f; // maximum bar height as a fraction of the screen height
    float radialBlur = 0;          // 0..1, streaks away from radialCentre
    glm::vec2 radialCentre{0.5f};  // screen coordinates, 0..1 with y down
    float chromatic = 0;           // chromatic aberration, 0..1
    // Freeze-frame: while frozen the screen shows the frame captured when
    // freeze() was last called; transitions reveal the live frame underneath.
    bool frozen = false;
    std::uint32_t freezeSerial = 0; // bumped by every freeze() so each call recaptures
    struct Transition {
        std::string kind = "none"; // none | fade | swirl | mosaic | wipe | iris | shatter
        float progress = 0;
        glm::vec2 centre{0.5f};
        glm::vec4 color{0, 0, 0, 1};
    } transition;
};
} // namespace fe
