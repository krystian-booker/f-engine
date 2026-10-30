#pragma once
#include "engine/world/Camera.h"
#include <glm/glm.hpp>

namespace fe {
// The Scene viewport's own orbit camera. The game's camera rig keeps writing
// the world camera every tick; the editor never touches it and renders the
// focus world with this one instead (WorldDraw::camera), so editing never
// moves the game's view and the game never moves the editor's.
struct EditorCamera {
    Camera camera;

    // Starts from the game's view (entering Edit mode).
    void adopt(const Camera& game);
    // Mouse drags in pixels: yaw and pitch (pitch stays between the limits).
    void orbit(glm::vec2 deltaPixels);
    // Moves the target across the view plane by as many world units as the
    // pixels cover at the target's distance.
    void pan(glm::vec2 deltaPixels, float viewportHeightPixels);
    // Wheel notches: each one scales the distance (and orthographic height) by 0.9.
    void zoom(float notches);
    // Frames a world-space box: target at its centre, distance so it fits.
    void frame(glm::vec3 min, glm::vec3 max);
    void lookAt(glm::vec3 target);
    void setOrthographic(bool on);

    static constexpr float MinDistance = 0.5f, MaxDistance = 500;
    static constexpr float MinPitch = -89, MaxPitch = 89.9f;
    static constexpr float DegreesPerPixel = 0.25f;
};
} // namespace fe
