#pragma once
#include "engine/core/Json.h"
#include <deque>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
// Device-agnostic input. Platform code feeds raw inputs by name ("key:Z",
// "pad:a", "pad1:a", "mouse:left", axes "pad:leftx"); scripts declare actions
// and axes and bind raw inputs to them. The engine ships no default bindings.
// Text entry, gamepad descriptions and rumble also pass through here: scripts
// make requests the platform layer carries out, and the platform reports text
// and devices back.
class InputState {
public:
    struct Action {
        std::vector<std::string> inputs; // raw input names
        float threshold = 0.5f;          // analog inputs count as held past this
        bool held = false, pressed = false, released = false;
        double heldTime = 0, previousHeldTime = 0;
    };
    // A connected gamepad. `type` names its button layout: "xbox",
    // "playstation", "switch", "steamdeck" or "generic".
    struct Pad {
        int number = 0; // 1-based, as in "pad1:"
        std::string name, type = "generic";
        bool rumble = false, triggerRumble = false;
    };
    // A rumble request; pad 0 means every gamepad. Strengths are 0..1.
    struct Rumble {
        int pad = 0;
        float low = 0, high = 0;
        double seconds = 0;
        bool triggers = false; // the trigger motors instead of the handles
    };
    // What the platform should do about text entry: show the IME or on-screen
    // keyboard near `rect` (canvas pixels: x, y, width, height).
    struct TextEntry {
        bool active = false;
        std::optional<glm::vec4> rect;
        std::string kind = "text"; // text | name | number
    };
    struct Axis {
        std::vector<std::string> negative, positive; // digital raw inputs
        std::vector<std::string> analog;             // analog raw inputs (-1..1)
        float deadzone = 0.2f;
        float value = 0;
    };

    void define(const std::string& action, std::vector<std::string> inputs, float threshold = 0.5f);
    void defineAxis(const std::string& axis, Axis definition);
    void clearDefinitions();
    [[nodiscard]] std::vector<std::string> bindings(const std::string& action) const;
    void bind(const std::string& action, std::vector<std::string> inputs);

    // digital: 1 down, 0 up; analog: -1..1. Values are queued until tick().
    void feed(const std::string& input, float value);
    void feedMouse(glm::vec2 canvasPosition);
    void feedWheel(float delta) { wheel_ += delta; }
    void setDevice(const std::string& device, const std::string& name = {});
    // The gamepad with this number was used (last_device becomes "gamepad").
    void usePad(int number);
    void connectPad(Pad pad);
    void disconnectPad(int number);
    [[nodiscard]] const std::map<int, Pad>& pads() const { return pads_; }
    // Committed UTF-8 text and the IME's pre-edit string (cursor in
    // characters); both are queued until tick().
    void feedText(std::string_view utf8);
    void feedComposition(std::string text, int cursor);
    void focusLost();

    // Script requests carried out by the platform layer.
    void beginText(std::optional<glm::vec4> rect, std::string kind);
    void endText();
    [[nodiscard]] const TextEntry& textEntry() const { return textEntry_; }
    // The text entry request when it changed since the last call.
    std::optional<TextEntry> takeTextEntryChange();
    void rumble(Rumble request);
    std::vector<Rumble> takeRumble();

    // Applies queued input and recomputes action and axis state. Each raw
    // input changes between up and down at most once per tick; further
    // changes wait for the next tick, so a press and release inside one tick
    // read as pressed this tick and released the next, and a release and
    // re-press as released then pressed.
    void tick(double dt);
    [[nodiscard]] bool held(const std::string& action) const;
    [[nodiscard]] bool pressed(const std::string& action) const;
    [[nodiscard]] bool released(const std::string& action) const;
    // True on press, then every `interval` seconds after `delay`.
    [[nodiscard]] bool repeat(const std::string& action, double delay, double interval) const;
    [[nodiscard]] double heldTime(const std::string& action) const;
    [[nodiscard]] float axis(const std::string& axis) const;
    [[nodiscard]] bool hasAxis(const std::string& axis) const { return axes_.contains(axis); }
    [[nodiscard]] float raw(const std::string& input) const;
    // Raw inputs that went down this tick (for rebinding screens).
    [[nodiscard]] const std::vector<std::string>& pressedInputs() const { return pressedInputs_; }
    [[nodiscard]] const std::string& lastDevice() const { return lastDevice_; }
    [[nodiscard]] const std::string& deviceName() const { return deviceName_; }
    // "keyboard" while the keyboard or mouse was used last, otherwise the
    // layout of the last gamepad used.
    [[nodiscard]] std::string deviceType() const;
    // Text committed this tick, and the current pre-edit string.
    [[nodiscard]] const std::string& text() const { return text_; }
    [[nodiscard]] const std::string& composition() const { return composition_; }
    [[nodiscard]] int compositionCursor() const { return compositionCursor_; }
    [[nodiscard]] glm::vec2 mouse() const { return mouse_; }
    [[nodiscard]] bool mouseMoved() const { return mouseMoved_; }
    [[nodiscard]] float wheel() const { return wheelTick_; }
    [[nodiscard]] const std::map<std::string, Action>& actions() const { return actions_; }

private:
    void applyPending();
    std::map<std::string, Action> actions_;
    std::map<std::string, Axis> axes_;
    std::map<std::string, float> raw_;
    std::map<std::string, std::deque<float>> pending_;
    std::vector<std::string> pressedInputs_;
    std::string lastDevice_ = "keyboard", deviceName_, deviceType_ = "generic";
    std::map<int, Pad> pads_;
    std::string pendingText_, text_;
    std::optional<std::pair<std::string, int>> pendingComposition_;
    std::string composition_;
    int compositionCursor_ = 0;
    TextEntry textEntry_;
    bool textEntryChanged_ = false;
    std::vector<Rumble> rumble_;
    glm::vec2 mouse_{0}, previousMouse_{0};
    bool mouseMoved_ = false;
    float wheel_ = 0, wheelTick_ = 0;
};
} // namespace fe
