#pragma once
#include "engine/services/Canvas.h"
#include "engine/services/InputState.h"
#include <SDL3/SDL.h>
#include <map>

namespace fe {
// Translates SDL events into named raw inputs for InputState:
// "key:<scancode name>", "pad:<button|axis>", "mouse:left|right|middle".
class SdlInput {
public:
    explicit SdlInput(InputState& state);
    ~SdlInput();
    SdlInput(const SdlInput&) = delete;
    SdlInput& operator=(const SdlInput&) = delete;
    // `uiCaptured` skips keyboard/mouse while a developer window has focus.
    void process(const SDL_Event& event, const Canvas& canvas, glm::ivec2 viewport, float pixelDensity,
                 bool uiCaptured);

private:
    void open(SDL_JoystickID id);
    InputState& state_;
    std::map<SDL_JoystickID, SDL_Gamepad*> pads_;
};
} // namespace fe
