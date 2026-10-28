#include "engine/input/SdlInput.h"
#include "engine/core/Log.h"
#include <algorithm>

namespace fe {
SdlInput::SdlInput(InputState& state) : state_(state) {
    int count = 0;
    if (auto* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            open(ids[i]);
        }
        SDL_free(ids);
    }
}

SdlInput::~SdlInput() {
    for (const auto& [id, pad] : pads_) {
        (void)id;
        SDL_CloseGamepad(pad);
    }
}

void SdlInput::open(SDL_JoystickID id) {
    if (pads_.contains(id)) {
        return;
    }
    if (auto* pad = SDL_OpenGamepad(id)) {
        pads_[id] = pad;
        const char* name = SDL_GetGamepadName(pad);
        state_.setDevice("gamepad", name ? name : "Gamepad");
        log(LogLevel::Info, std::string("Gamepad connected: ") + (name ? name : "unnamed"));
    }
}

void SdlInput::process(const SDL_Event& event, const Canvas& canvas, glm::ivec2 viewport, float density,
                       bool uiCaptured) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        state_.focusLost();
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (!uiCaptured && !event.key.repeat) {
            state_.feed(std::string("key:") + SDL_GetScancodeName(event.key.scancode),
                        event.key.down ? 1.0f : 0.0f);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (!uiCaptured) {
            state_.feedMouse(
                canvas.fromWindow({event.motion.x * density, event.motion.y * density}, viewport));
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (uiCaptured) {
            break;
        }
        const char* name = event.button.button == SDL_BUTTON_LEFT    ? "mouse:left"
                           : event.button.button == SDL_BUTTON_RIGHT ? "mouse:right"
                                                                     : "mouse:middle";
        state_.feed(name, event.button.down ? 1.0f : 0.0f);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!uiCaptured) {
            state_.feedWheel(event.wheel.y);
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        open(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (const auto found = pads_.find(event.gdevice.which); found != pads_.end()) {
            SDL_CloseGamepad(found->second);
            pads_.erase(found);
            state_.focusLost();
            log(LogLevel::Info, "Gamepad disconnected");
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        state_.feed(std::string("pad:") +
                        SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event.gbutton.button)),
                    event.gbutton.down ? 1.0f : 0.0f);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const auto axis = static_cast<SDL_GamepadAxis>(event.gaxis.axis);
        const float value = std::clamp(static_cast<float>(event.gaxis.value) / 32767.0f, -1.0f, 1.0f);
        state_.feed(std::string("pad:") + SDL_GetGamepadStringForAxis(axis), value);
        break;
    }
    default:
        break;
    }
}
} // namespace fe
