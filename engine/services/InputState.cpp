#include "engine/services/InputState.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fe {
void InputState::define(const std::string& action, std::vector<std::string> inputs, float threshold) {
    if (action.empty()) {
        throw std::invalid_argument("Input actions need a name");
    }
    auto& entry = actions_[action];
    entry.inputs = std::move(inputs);
    entry.threshold = std::clamp(threshold, 0.05f, 1.0f);
}

void InputState::defineAxis(const std::string& axis, Axis definition) {
    if (axis.empty()) {
        throw std::invalid_argument("Input axes need a name");
    }
    definition.deadzone = std::clamp(definition.deadzone, 0.0f, 0.95f);
    axes_[axis] = std::move(definition);
}

void InputState::clearDefinitions() {
    actions_.clear();
    axes_.clear();
}

std::vector<std::string> InputState::bindings(const std::string& action) const {
    const auto found = actions_.find(action);
    return found == actions_.end() ? std::vector<std::string>{} : found->second.inputs;
}

void InputState::bind(const std::string& action, std::vector<std::string> inputs) {
    const auto found = actions_.find(action);
    if (found == actions_.end()) {
        throw std::invalid_argument("Unknown input action: " + action);
    }
    found->second.inputs = std::move(inputs);
}

void InputState::feed(const std::string& input, float value) {
    if (!std::isfinite(value)) {
        return;
    }
    const float previous = raw(input);
    raw_[input] = std::clamp(value, -1.0f, 1.0f);
    const bool analog =
        input == "pad:leftx" || input == "pad:lefty" || input == "pad:rightx" || input == "pad:righty";
    const bool wasDown = std::abs(previous) >= 0.5f;
    const bool isDown = std::abs(value) >= 0.5f;
    if (isDown && !wasDown) {
        downEdges_.insert(input);
        if (!analog) {
            pendingInputs_.push_back(input);
        }
    } else if (!isDown && wasDown) {
        upEdges_.insert(input);
    }
    if (input.starts_with("key:")) {
        setDevice("keyboard");
    } else if (input.starts_with("pad:") && std::abs(value) > 0.3f) {
        setDevice("gamepad", deviceName_);
    } else if (input.starts_with("mouse:")) {
        setDevice("mouse");
    }
}

void InputState::feedMouse(glm::vec2 position) {
    if (glm::length(position - mouse_) > 0.5f) {
        setDevice("mouse");
    }
    mouse_ = position;
}

void InputState::setDevice(const std::string& device, const std::string& name) {
    lastDevice_ = device;
    if (!name.empty()) {
        deviceName_ = name;
    }
}

void InputState::focusLost() {
    for (auto& [input, value] : raw_) {
        if (std::abs(value) >= 0.5f) {
            upEdges_.insert(input);
        }
        value = 0;
    }
}

void InputState::tick(double dt) {
    for (auto& [name, action] : actions_) {
        bool held = false, down = false, up = false;
        for (const auto& input : action.inputs) {
            const float value = raw(input);
            const bool negative = input.ends_with("-");
            const auto base = negative || input.ends_with("+") ? input.substr(0, input.size() - 1) : input;
            const float directed = input == base ? std::abs(value) : (negative ? -raw(base) : raw(base));
            held = held || directed >= action.threshold;
            down = down || downEdges_.contains(input);
            up = up || upEdges_.contains(input);
        }
        action.pressed = (held && !action.held) || (down && !action.held);
        action.released = (!held && action.held) || (up && !held && !action.pressed);
        action.held = held;
        action.previousHeldTime = action.heldTime;
        action.heldTime = held ? action.heldTime + dt : 0;
    }
    for (auto& [name, axis] : axes_) {
        float value = 0;
        for (const auto& input : axis.positive) {
            value += raw(input) >= 0.5f ? 1.0f : 0.0f;
        }
        for (const auto& input : axis.negative) {
            value -= raw(input) >= 0.5f ? 1.0f : 0.0f;
        }
        for (const auto& input : axis.analog) {
            const float analog = raw(input);
            if (std::abs(analog) > axis.deadzone && std::abs(analog) > std::abs(value)) {
                value = std::copysign((std::abs(analog) - axis.deadzone) / (1 - axis.deadzone), analog);
            }
        }
        axis.value = std::clamp(value, -1.0f, 1.0f);
    }
    pressedInputs_ = std::move(pendingInputs_);
    pendingInputs_.clear();
    downEdges_.clear();
    upEdges_.clear();
    mouseMoved_ = glm::length(mouse_ - previousMouse_) > 0.01f;
    previousMouse_ = mouse_;
    wheelTick_ = wheel_;
    wheel_ = 0;
}

bool InputState::held(const std::string& action) const {
    const auto found = actions_.find(action);
    return found != actions_.end() && found->second.held;
}

bool InputState::pressed(const std::string& action) const {
    const auto found = actions_.find(action);
    return found != actions_.end() && found->second.pressed;
}

bool InputState::released(const std::string& action) const {
    const auto found = actions_.find(action);
    return found != actions_.end() && found->second.released;
}

bool InputState::repeat(const std::string& action, double delay, double interval) const {
    const auto found = actions_.find(action);
    if (found == actions_.end()) {
        return false;
    }
    const auto& a = found->second;
    if (a.pressed) {
        return true;
    }
    if (!a.held || a.heldTime < delay || interval <= 0) {
        return false;
    }
    const auto before = std::floor((a.previousHeldTime - delay) / interval);
    const auto now = std::floor((a.heldTime - delay) / interval);
    return a.previousHeldTime < delay || now > before;
}

double InputState::heldTime(const std::string& action) const {
    const auto found = actions_.find(action);
    return found == actions_.end() ? 0 : found->second.heldTime;
}

float InputState::axis(const std::string& name) const {
    const auto found = axes_.find(name);
    return found == axes_.end() ? 0 : found->second.value;
}

float InputState::raw(const std::string& input) const {
    const auto found = raw_.find(input);
    return found == raw_.end() ? 0 : found->second;
}
} // namespace fe
