#pragma once
#include "engine/core/Json.h"
#include <glm/glm.hpp>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fe {
// Device-agnostic input. Platform code feeds raw inputs by name ("key:Z",
// "pad:a", "mouse:left", axes "pad:leftx"); scripts declare actions and axes
// and bind raw inputs to them. The engine ships no default bindings.
class InputState {
public:
    struct Action {
        std::vector<std::string> inputs; // raw input names
        float threshold = 0.5f;          // analog inputs count as held past this
        bool held = false, pressed = false, released = false;
        double heldTime = 0, previousHeldTime = 0;
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

    void feed(const std::string& input, float value); // digital: 1 down, 0 up; analog: -1..1
    void feedMouse(glm::vec2 canvasPosition);
    void feedWheel(float delta) { wheel_ += delta; }
    void setDevice(const std::string& device, const std::string& name = {});
    void focusLost();

    // Recomputes action and axis state for this tick.
    void tick(double dt);
    [[nodiscard]] bool held(const std::string& action) const;
    [[nodiscard]] bool pressed(const std::string& action) const;
    [[nodiscard]] bool released(const std::string& action) const;
    // True on press, then every `interval` seconds after `delay`.
    [[nodiscard]] bool repeat(const std::string& action, double delay, double interval) const;
    [[nodiscard]] double heldTime(const std::string& action) const;
    [[nodiscard]] float axis(const std::string& axis) const;
    [[nodiscard]] float raw(const std::string& input) const;
    // Raw inputs that went down this tick (for rebinding screens).
    [[nodiscard]] const std::vector<std::string>& pressedInputs() const { return pressedInputs_; }
    [[nodiscard]] const std::string& lastDevice() const { return lastDevice_; }
    [[nodiscard]] const std::string& deviceName() const { return deviceName_; }
    [[nodiscard]] glm::vec2 mouse() const { return mouse_; }
    [[nodiscard]] bool mouseMoved() const { return mouseMoved_; }
    [[nodiscard]] float wheel() const { return wheelTick_; }
    [[nodiscard]] const std::map<std::string, Action>& actions() const { return actions_; }

private:
    std::map<std::string, Action> actions_;
    std::map<std::string, Axis> axes_;
    std::map<std::string, float> raw_;
    std::set<std::string> downEdges_, upEdges_;
    std::vector<std::string> pressedInputs_, pendingInputs_;
    std::string lastDevice_ = "keyboard", deviceName_;
    glm::vec2 mouse_{0}, previousMouse_{0};
    bool mouseMoved_ = false;
    float wheel_ = 0, wheelTick_ = 0;
};
} // namespace fe
