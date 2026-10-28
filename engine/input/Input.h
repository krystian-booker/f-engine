#pragma once
#include "engine/core/Json.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <array>
#include <bitset>
#include <map>

namespace fe {
enum class Action { Up, Down, Left, Right, Confirm, Cancel, Menu, Previous, Next, Count };
constexpr std::size_t ActionCount=static_cast<std::size_t>(Action::Count);
struct ActionSnapshot {
    std::bitset<ActionCount> held, pressed, released;
    glm::vec2 movement{0};
    [[nodiscard]] bool down(Action action) const { return held[static_cast<std::size_t>(action)]; }
    [[nodiscard]] bool hit(Action action) const { return pressed[static_cast<std::size_t>(action)]; }
};
class Input {
public:
    Input();
    void processEvent(const SDL_Event& event);
    ActionSnapshot tick();
    void rebind(Action action, SDL_Scancode key);
    [[nodiscard]] SDL_Scancode binding(Action action) const;
    [[nodiscard]] Json toJson() const;
    void fromJson(const Json& data);
    [[nodiscard]] SDL_Scancode lastKey() const { return lastKey_; }
    void clearLastKey() { lastKey_=SDL_SCANCODE_UNKNOWN; }
    static const char* name(Action action);
private:
    std::array<SDL_Scancode,ActionCount> bindings_;
    std::array<SDL_GamepadButton,ActionCount> buttons_;
    std::bitset<SDL_SCANCODE_COUNT> keys_;
    std::bitset<SDL_GAMEPAD_BUTTON_COUNT> padButtons_;
    std::bitset<ActionCount> pendingPress_,pendingRelease_,previous_;
    glm::vec2 stick_{0};
    SDL_JoystickID activePad_=0;
    SDL_Scancode lastKey_=SDL_SCANCODE_UNKNOWN;
};
class InputRepeat {
public:
    bool hit(const ActionSnapshot& input,Action action,double dt);
    void clear();
private:
    std::array<double,ActionCount> elapsed_{},next_{};
};
}
