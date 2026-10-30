#pragma once
#include "engine/services/Canvas.h"
#include "engine/services/InputState.h"
#include <SDL3/SDL.h>
#include <map>
#include <string>

namespace fe {
class InputRecorder;

// Translates SDL events into named raw inputs for InputState:
//   key:<scancode name>          keyboard ("key:Return", "key:Left Shift")
//   mouse:left|right|middle|x1|x2
//   pad:<button|axis>            any connected gamepad ("pad:a", "pad:leftx")
//   pad1:<button|axis>, pad2:... one gamepad, numbered from 1 in connection
//                                order; a number is reused after a disconnect
// "pad:" reports the strongest value across gamepads, so two pads holding and
// releasing the same button don't cancel each other out. Text input and IME
// pre-edit events become InputState text (pre-edit only while the game's text
// entry is active, so late events after text_end don't revive a composition;
// focus loss ends one), and connected gamepads are described to InputState.
// apply() carries out InputState's text entry and rumble requests.
class SdlInput {
public:
    explicit SdlInput(InputState& state);
    ~SdlInput();
    SdlInput(const SdlInput&) = delete;
    SdlInput& operator=(const SdlInput&) = delete;
    // `uiCaptured` (a developer window has focus) drops key and button presses,
    // mouse motion and wheel. Releases always pass through, so nothing sticks.
    void process(const SDL_Event& event, const Canvas& canvas, glm::ivec2 viewport, float pixelDensity,
                 bool uiCaptured);
    // Starts or stops text input (IME, and the on-screen keyboard on Steam
    // Deck and touch devices) and plays queued rumble.
    void apply(SDL_Window* window, const Canvas& canvas, glm::ivec2 viewport, float pixelDensity);
    // Also send everything fed to InputState (inputs, mouse, wheel, typed text
    // and pre-edit strings) to a recorder (or stop: null); the same as
    // InputState::setRecorder.
    void record(InputRecorder* recorder);
    // Releases every input that is down (InputState::focusLost) and stops
    // rumble (focus loss).
    void releaseAll();

    // Button layout of a gamepad for prompts: "xbox", "playstation", "switch",
    // "steamdeck" or "generic". `steamDeck` is true when running on a Steam
    // Deck, where Valve devices are the built-in controls.
    [[nodiscard]] static std::string padType(SDL_GamepadType type, Uint16 vendor, Uint16 product,
                                             bool steamDeck);

private:
    struct Pad {
        SDL_Gamepad* handle = nullptr;
        int number = 0;
        std::string name;
        std::map<std::string, float> values;
    };
    Pad& pad(SDL_JoystickID id);
    void disconnect(SDL_JoystickID id);
    void feedPad(SDL_JoystickID id, const std::string& control, float value);
    void playRumble(const InputState::Rumble& request);
    InputState& state_;
    std::map<SDL_JoystickID, Pad> pads_;
};
} // namespace fe
