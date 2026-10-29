#include "engine/services/InputState.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace fe {
namespace {
// The gamepad number of "pad3:a" (3), 0 for "pad:a", -1 for other inputs.
int padNumber(std::string_view input) {
    if (!input.starts_with("pad")) {
        return -1;
    }
    const auto colon = input.find(':');
    if (colon == std::string_view::npos) {
        return -1;
    }
    int number = 0;
    for (std::size_t i = 3; i < colon; ++i) {
        if (input[i] < '0' || input[i] > '9' || number > 1000) {
            return -1;
        }
        number = number * 10 + (input[i] - '0');
    }
    return colon == 3 ? 0 : number;
}

// Stick axes rest at a position rather than being pressed.
bool stickAxis(std::string_view input) {
    if (padNumber(input) < 0) {
        return false;
    }
    const auto control = input.substr(input.find(':') + 1);
    return control == "leftx" || control == "lefty" || control == "rightx" || control == "righty";
}
} // namespace

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
    // Consecutive values on the same side of the threshold collapse into one;
    // a flood of up/down changes keeps only the oldest and newest few.
    auto& queue = pending_[input];
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    if (!queue.empty() && (std::abs(queue.back()) >= 0.5f) == (std::abs(clamped) >= 0.5f)) {
        queue.back() = clamped;
    } else {
        queue.push_back(clamped);
        if (queue.size() > 8) {
            queue.erase(queue.end() - 3, queue.end() - 1);
        }
    }
    const int pad = padNumber(input);
    if (input.starts_with("key:")) {
        setDevice("keyboard");
    } else if (pad > 0 && std::abs(value) > 0.3f) {
        usePad(pad);
    } else if (pad == 0 && std::abs(value) > 0.3f) {
        setDevice("gamepad");
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

void InputState::usePad(int number) {
    lastDevice_ = "gamepad";
    if (const auto found = pads_.find(number); found != pads_.end()) {
        deviceName_ = found->second.name;
        deviceType_ = found->second.type;
    }
}

void InputState::connectPad(Pad pad) {
    pads_[pad.number] = std::move(pad);
}

void InputState::disconnectPad(int number) {
    pads_.erase(number);
}

std::string InputState::deviceType() const {
    return lastDevice_ == "gamepad" ? deviceType_ : "keyboard";
}

void InputState::feedText(std::string_view utf8) {
    if (pendingText_.size() + utf8.size() <= 4096) {
        pendingText_ += utf8;
    }
}

void InputState::feedComposition(std::string text, int cursor) {
    pendingComposition_.emplace(std::move(text), cursor);
}

void InputState::beginText(std::optional<glm::vec4> rect, std::string kind) {
    textEntry_ = {true, rect, std::move(kind)};
    textEntryChanged_ = true;
}

void InputState::endText() {
    if (!textEntry_.active) {
        return;
    }
    textEntry_ = {};
    textEntryChanged_ = true;
    pendingComposition_.reset();
    composition_.clear();
    compositionCursor_ = 0;
}

std::optional<InputState::TextEntry> InputState::takeTextEntryChange() {
    if (!textEntryChanged_) {
        return std::nullopt;
    }
    textEntryChanged_ = false;
    return textEntry_;
}

void InputState::rumble(Rumble request) {
    request.low = std::clamp(request.low, 0.0f, 1.0f);
    request.high = std::clamp(request.high, 0.0f, 1.0f);
    request.seconds = std::isfinite(request.seconds) ? std::clamp(request.seconds, 0.0, 60.0) : 0.0;
    rumble_.push_back(request);
}

std::vector<InputState::Rumble> InputState::takeRumble() {
    return std::exchange(rumble_, {});
}

void InputState::focusLost() {
    pendingComposition_.emplace(std::string(), 0);
    for (auto& [input, queue] : pending_) {
        queue.assign(1, 0.0f);
    }
    for (const auto& [input, value] : raw_) {
        if (value != 0) {
            pending_[input].assign(1, 0.0f);
        }
    }
}

void InputState::applyPending() {
    pressedInputs_.clear();
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto& [input, queue] = *it;
        float& state = raw_[input];
        bool changed = false;
        while (!queue.empty()) {
            const float value = queue.front();
            const bool down = std::abs(value) >= 0.5f;
            if (down != (std::abs(state) >= 0.5f)) {
                if (changed) {
                    break;
                }
                changed = true;
                if (down && !stickAxis(input)) {
                    pressedInputs_.push_back(input);
                }
            }
            state = value;
            queue.pop_front();
        }
        it = queue.empty() ? pending_.erase(it) : std::next(it);
    }
    text_ = std::exchange(pendingText_, {});
    if (pendingComposition_) {
        composition_ = std::move(pendingComposition_->first);
        compositionCursor_ = pendingComposition_->second;
        pendingComposition_.reset();
    }
}

void InputState::tick(double dt) {
    applyPending();
    for (auto& [name, action] : actions_) {
        bool held = false;
        for (const auto& input : action.inputs) {
            const float value = raw(input);
            const bool negative = input.ends_with("-");
            const auto base = negative || input.ends_with("+") ? input.substr(0, input.size() - 1) : input;
            const float directed = input == base ? std::abs(value) : (negative ? -raw(base) : raw(base));
            held = held || directed >= action.threshold;
        }
        action.pressed = held && !action.held;
        action.released = !held && action.held;
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
