#include "engine/input/SdlInput.h"
#include "engine/core/Log.h"
#include "engine/runtime/ReplayHarness.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace fe {
SdlInput::SdlInput(InputState& state) : state_(state) {
    int count = 0;
    if (auto* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            pad(ids[i]);
        }
        SDL_free(ids);
    }
}

SdlInput::~SdlInput() {
    for (const auto& [id, pad] : pads_) {
        if (pad.handle) {
            SDL_CloseGamepad(pad.handle);
        }
    }
}

SdlInput::Pad& SdlInput::pad(SDL_JoystickID id) {
    const auto found = pads_.find(id);
    if (found != pads_.end()) {
        return found->second;
    }
    int number = 1;
    while (std::any_of(pads_.begin(), pads_.end(),
                       [&](const auto& entry) { return entry.second.number == number; })) {
        ++number;
    }
    Pad added;
    added.handle = SDL_OpenGamepad(id);
    added.number = number;
    const char* name = added.handle ? SDL_GetGamepadName(added.handle) : nullptr;
    added.name = name ? name : "Gamepad";
    InputState::Pad description;
    description.number = number;
    description.name = added.name;
    if (added.handle) {
        description.type =
            padType(SDL_GetGamepadType(added.handle), SDL_GetGamepadVendor(added.handle),
                    SDL_GetGamepadProduct(added.handle), SDL_GetHintBoolean("SteamDeck", false));
        const auto properties = SDL_GetGamepadProperties(added.handle);
        description.rumble = SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        description.triggerRumble =
            SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
    }
    state_.connectPad(description);
    log(LogLevel::Info,
        "Gamepad " + std::to_string(number) + " connected: " + added.name + " (" + description.type + ")");
    return pads_.emplace(id, std::move(added)).first->second;
}

void SdlInput::disconnect(SDL_JoystickID id) {
    const auto found = pads_.find(id);
    if (found == pads_.end()) {
        return;
    }
    std::vector<std::string> held;
    for (const auto& [control, value] : found->second.values) {
        if (value != 0) {
            held.push_back(control);
        }
    }
    for (const auto& control : held) {
        feedPad(id, control, 0);
    }
    if (found->second.handle) {
        SDL_CloseGamepad(found->second.handle);
    }
    state_.disconnectPad(found->second.number);
    log(LogLevel::Info, "Gamepad " + std::to_string(found->second.number) + " disconnected");
    pads_.erase(found);
}

void SdlInput::feedPad(SDL_JoystickID id, const std::string& control, float value) {
    auto& source = pad(id);
    source.values[control] = value;
    float strongest = 0;
    for (const auto& [other, entry] : pads_) {
        const auto found = entry.values.find(control);
        if (found != entry.values.end() && std::abs(found->second) > std::abs(strongest)) {
            strongest = found->second;
        }
    }
    // "pad:" first: rebinding screens take the first input pressed in a tick.
    feed("pad:" + control, strongest);
    feed("pad" + std::to_string(source.number) + ":" + control, value);
}

void SdlInput::compose(const std::string& text, int cursor) {
    composing_ = !text.empty();
    state_.feedComposition(text, cursor);
    if (recorder_) {
        recorder_->composition(text, cursor);
    }
}

void SdlInput::feed(const std::string& input, float value) {
    state_.feed(input, value);
    if (value != 0) {
        down_[input] = value;
    } else {
        down_.erase(input);
    }
    if (recorder_) {
        recorder_->input(input, value);
    }
}

std::string SdlInput::padType(SDL_GamepadType type, Uint16 vendor, Uint16 product, bool steamDeck) {
    constexpr Uint16 Valve = 0x28de, SteamDeckControls = 0x1205;
    if (vendor == Valve && (product == SteamDeckControls || steamDeck)) {
        return "steamdeck";
    }
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return "xbox";
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return "playstation";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return "switch";
    default:
        return "generic";
    }
}

void SdlInput::apply(SDL_Window* window, const Canvas& canvas, glm::ivec2 viewport, float density) {
    if (const auto entry = state_.takeTextEntryChange()) {
        if (!entry->active) {
            SDL_StopTextInput(window);
        } else {
            if (entry->rect) {
                const auto& r = *entry->rect;
                const auto a = canvas.toWindow({r.x, r.y}, viewport) / density;
                const auto b = canvas.toWindow({r.x + r.z, r.y + r.w}, viewport) / density;
                const SDL_Rect area{static_cast<int>(a.x), static_cast<int>(a.y),
                                    std::max(1, static_cast<int>(b.x - a.x)),
                                    std::max(1, static_cast<int>(b.y - a.y))};
                SDL_SetTextInputArea(window, &area, 0);
            } else {
                SDL_SetTextInputArea(window, nullptr, 0);
            }
            const auto properties = SDL_CreateProperties();
            const auto kind = entry->kind == "name"     ? SDL_TEXTINPUT_TYPE_TEXT_NAME
                              : entry->kind == "number" ? SDL_TEXTINPUT_TYPE_NUMBER
                                                        : SDL_TEXTINPUT_TYPE_TEXT;
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER, kind);
            SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
            SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
            if (!SDL_StartTextInputWithProperties(window, properties)) {
                log(LogLevel::Warning, std::string("Text input unavailable: ") + SDL_GetError());
            }
            SDL_DestroyProperties(properties);
        }
    }
    for (const auto& request : state_.takeRumble()) {
        playRumble(request);
    }
}

void SdlInput::playRumble(const InputState::Rumble& request) {
    const auto strength = [](float value) {
        return static_cast<Uint16>(std::lround(value * 0xffff));
    };
    const auto milliseconds = static_cast<Uint32>(request.seconds * 1000.0);
    for (const auto& [id, pad] : pads_) {
        if (!pad.handle || (request.pad != 0 && pad.number != request.pad)) {
            continue;
        }
        if (request.triggers) {
            SDL_RumbleGamepadTriggers(pad.handle, strength(request.low), strength(request.high),
                                      milliseconds);
        } else {
            SDL_RumbleGamepad(pad.handle, strength(request.low), strength(request.high), milliseconds);
        }
    }
}

void SdlInput::releaseAll() {
    for (auto& [id, pad] : pads_) {
        pad.values.clear();
        if (pad.handle) {
            SDL_RumbleGamepad(pad.handle, 0, 0, 0);
            SDL_RumbleGamepadTriggers(pad.handle, 0, 0, 0);
        }
    }
    std::vector<std::string> held;
    for (const auto& [input, value] : down_) {
        held.push_back(input);
    }
    for (const auto& input : held) {
        feed(input, 0);
    }
}

void SdlInput::process(const SDL_Event& event, const Canvas& canvas, glm::ivec2 viewport, float density,
                       bool uiCaptured) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        releaseAll();
        if (composing_) {
            compose("", 0);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (!event.key.repeat && !(uiCaptured && event.key.down)) {
            feed(std::string("key:") + SDL_GetScancodeName(event.key.scancode), event.key.down ? 1.0f : 0.0f);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (!uiCaptured) {
            const auto position =
                canvas.fromWindow({event.motion.x * density, event.motion.y * density}, viewport);
            state_.feedMouse(position);
            if (recorder_) {
                recorder_->mouse(position);
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (uiCaptured && event.button.down) {
            break;
        }
        const char* name = nullptr;
        switch (event.button.button) {
        case SDL_BUTTON_LEFT:
            name = "mouse:left";
            break;
        case SDL_BUTTON_RIGHT:
            name = "mouse:right";
            break;
        case SDL_BUTTON_MIDDLE:
            name = "mouse:middle";
            break;
        case SDL_BUTTON_X1:
            name = "mouse:x1";
            break;
        case SDL_BUTTON_X2:
            name = "mouse:x2";
            break;
        default:
            break;
        }
        if (name) {
            feed(name, event.button.down ? 1.0f : 0.0f);
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!uiCaptured) {
            state_.feedWheel(event.wheel.y);
            if (recorder_) {
                recorder_->wheel(event.wheel.y);
            }
        }
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (!uiCaptured && event.text.text) {
            state_.feedText(event.text.text);
            if (recorder_) {
                recorder_->text(event.text.text);
            }
        }
        break;
    case SDL_EVENT_TEXT_EDITING:
        if (!uiCaptured && state_.textEntry().active) {
            compose(event.edit.text ? event.edit.text : "", std::max(0, int(event.edit.start)));
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        pad(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        disconnect(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (const char* button =
                SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event.gbutton.button))) {
            feedPad(event.gbutton.which, button, event.gbutton.down ? 1.0f : 0.0f);
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (const char* axis = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(event.gaxis.axis))) {
            feedPad(event.gaxis.which, axis,
                    std::clamp(static_cast<float>(event.gaxis.value) / 32767.0f, -1.0f, 1.0f));
        }
        break;
    default:
        break;
    }
}
} // namespace fe
