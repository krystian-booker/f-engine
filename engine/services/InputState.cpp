#include "engine/services/InputState.h"
#include "engine/services/InputRecorder.h"
#include <algorithm>
#include <cmath>
#include <set>
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

constexpr float DefaultThreshold = 0.5f;
} // namespace

bool InputState::Edge::down(float value) const {
    switch (direction) {
    case 1:
        return value >= threshold;
    case -1:
        return -value >= threshold;
    default:
        return std::abs(value) >= threshold;
    }
}

bool InputState::analogInput(std::string_view input) const {
    if (padNumber(input) >= 0) {
        return true;
    }
    for (const auto& [name, axis] : axes_) {
        if (std::find(axis.analog.begin(), axis.analog.end(), input) != axis.analog.end()) {
            return true;
        }
    }
    return false;
}

InputState::Binding InputState::parseBinding(const std::string& bound) const {
    if (bound.size() > 1 && (bound.ends_with('+') || bound.ends_with('-'))) {
        auto base = bound.substr(0, bound.size() - 1);
        if (analogInput(base)) {
            return {std::move(base), bound.ends_with('-') ? -1 : 1};
        }
    }
    return {bound, 0};
}

std::vector<InputState::Edge> InputState::edgesOf(const std::string& input) const {
    std::vector<Edge> edges{{DefaultThreshold, 0}};
    for (const auto& [name, action] : actions_) {
        for (const auto& bound : action.inputs) {
            const auto binding = parseBinding(bound);
            if (binding.input != input) {
                continue;
            }
            const Edge edge{action.threshold, binding.direction};
            const bool known = std::any_of(edges.begin(), edges.end(), [&](const Edge& e) {
                return e.threshold == edge.threshold && e.direction == edge.direction;
            });
            if (!known) {
                edges.push_back(edge);
            }
        }
    }
    return edges;
}

bool InputState::sameSide(const std::vector<Edge>& edges, float a, float b) {
    return std::all_of(edges.begin(), edges.end(), [&](const Edge& e) { return e.down(a) == e.down(b); });
}

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
    if (recorder_) {
        recorder_->input(input, value);
    }
    // Consecutive values on the same side of every threshold an action applies
    // to this input collapse into one; a flood of up/down changes keeps only
    // the oldest and newest few.
    auto& queue = pending_[input];
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    if (!queue.empty() && sameSide(edgesOf(input), queue.back(), clamped)) {
        queue.back() = clamped;
    } else {
        queue.push_back(clamped);
        if (queue.size() > 8) {
            queue.erase(queue.end() - 3, queue.end() - 1);
        }
    }
    // The device in use follows what the player does on purpose: a key or
    // mouse button going down, or a gamepad control crossing well past rest.
    // Releases never switch (letting go of a key after picking up the pad),
    // and a control already past the threshold doesn't switch again until it
    // has come back to rest, so a drifting or held stick cannot fight the
    // keyboard for the prompts.
    const int pad = padNumber(input);
    if (pad >= 0) {
        const float magnitude = std::abs(value);
        if (magnitude >= DeviceSwitchOn) {
            if (engagedPadInputs_.insert(input).second) {
                if (pad > 0) {
                    usePad(pad);
                } else {
                    setDevice("gamepad");
                }
            }
        } else if (magnitude <= DeviceSwitchOff) {
            engagedPadInputs_.erase(input);
        }
    } else if (value != 0) {
        if (input.starts_with("key:")) {
            setDevice("keyboard");
        } else if (input.starts_with("mouse:")) {
            setDevice("mouse");
        }
    }
}

void InputState::feedMouse(glm::vec2 position) {
    if (recorder_) {
        recorder_->mouse(position);
    }
    if (lastDevice_ == "gamepad") {
        // A nudged mouse (a bumped desk, a Steam Deck trackpad at rest) is not
        // a change of device: the pointer has to travel from where it was when
        // the gamepad took over.
        if (glm::length(position - mouseAnchor_) > MouseSwitchDistance) {
            setDevice("mouse");
        }
    } else if (glm::length(position - mouse_) > 0.5f) {
        setDevice("mouse");
    }
    mouse_ = position;
}

void InputState::feedWheel(float delta) {
    if (recorder_) {
        recorder_->wheel(delta);
    }
    wheel_ += delta;
}

void InputState::setDevice(const std::string& device, const std::string& name) {
    if (device == "gamepad" && lastDevice_ != "gamepad") {
        mouseAnchor_ = mouse_;
    }
    if (device != "gamepad") {
        activePad_ = 0;
    }
    lastDevice_ = device;
    if (!name.empty()) {
        deviceName_ = name;
    }
}

void InputState::usePad(int number) {
    if (lastDevice_ != "gamepad") {
        mouseAnchor_ = mouse_;
    }
    lastDevice_ = "gamepad";
    activePad_ = number;
    if (const auto found = pads_.find(number); found != pads_.end()) {
        deviceName_ = found->second.name;
        deviceType_ = found->second.type;
    }
}

void InputState::connectPad(Pad pad) {
    const int number = pad.number;
    pads_[number] = pad;
    queuePadEvent({true, std::move(pad)});
    // Plugging a controller in is picking it up: prompts follow it at once.
    usePad(number);
}

void InputState::disconnectPad(int number) {
    const auto found = pads_.find(number);
    if (found == pads_.end()) {
        return;
    }
    PadEvent event{false, std::move(found->second)};
    pads_.erase(found);
    const auto prefix = "pad" + std::to_string(number) + ":";
    std::erase_if(engagedPadInputs_, [&](const std::string& input) { return input.starts_with(prefix); });
    queuePadEvent(std::move(event));
    // The pad in use is gone: fall back to another one, else to the keyboard,
    // so prompts never show the buttons of a controller that isn't there.
    if (lastDevice_ == "gamepad" && (activePad_ == number || activePad_ == 0)) {
        if (pads_.empty()) {
            setDevice("keyboard");
        } else {
            usePad(pads_.begin()->first);
        }
    }
}

void InputState::queuePadEvent(PadEvent event) {
    if (pendingPads_.size() >= MaxPendingPadEvents) {
        return;
    }
    if (recorder_) {
        recorder_->pad(event);
        recordedPads_ = pendingPads_.size() + 1;
    }
    pendingPads_.push_back(std::move(event));
}

void InputState::setRecorder(InputRecorder* recorder) {
    recorder_ = recorder;
    if (!recorder_) {
        return;
    }
    for (; recordedPads_ < pendingPads_.size(); ++recordedPads_) {
        recorder_->pad(pendingPads_[recordedPads_]);
    }
}

std::string InputState::deviceType() const {
    return lastDevice_ == "gamepad" ? deviceType_ : "keyboard";
}

std::string InputState::label(const std::string& input) const {
    if (labeler_) {
        if (auto text = labeler_(input); !text.empty()) {
            return text;
        }
    }
    const auto colon = input.find(':');
    return colon == std::string::npos ? input : input.substr(colon + 1);
}

void InputState::feedText(std::string_view utf8) {
    if (recorder_) {
        recorder_->text(utf8);
    }
    if (pendingText_.size() + utf8.size() <= 4096) {
        pendingText_ += utf8;
    }
}

void InputState::feedComposition(std::string text, int cursor) {
    if (recorder_) {
        recorder_->composition(text, cursor);
    }
    composing_ = !text.empty();
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
    composing_ = false;
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
    std::set<std::string> release;
    for (const auto& [input, queue] : pending_) {
        release.insert(input);
    }
    for (const auto& [input, value] : raw_) {
        if (value != 0) {
            release.insert(input);
        }
    }
    for (const auto& input : release) {
        // Queued presses are dropped, not delivered: nothing is held after focus loss.
        pending_[input].assign(1, 0.0f);
        if (recorder_) {
            recorder_->input(input, 0);
        }
    }
    if (composing_) {
        feedComposition(std::string(), 0);
    }
}

void InputState::applyPending() {
    pressedInputs_.clear();
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto& [input, queue] = *it;
        float& state = raw_[input];
        const auto edges = edgesOf(input);
        bool changed = false;
        while (!queue.empty()) {
            const float value = queue.front();
            if (!sameSide(edges, state, value)) {
                if (changed) {
                    break;
                }
                changed = true;
                const bool wasDown = std::abs(state) >= DefaultThreshold;
                if (std::abs(value) >= DefaultThreshold && !wasDown && !stickAxis(input)) {
                    pressedInputs_.push_back(input);
                }
            }
            state = value;
            queue.pop_front();
        }
        it = queue.empty() ? pending_.erase(it) : std::next(it);
    }
    // What prompts should show, compared tick to tick: a game redraws cached
    // glyphs when this turns true instead of comparing strings every frame.
    auto shown = deviceType();
    deviceChanged_ = shown != shownDevice_;
    shownDevice_ = std::move(shown);
    text_ = std::exchange(pendingText_, {});
    padEvents_ = std::exchange(pendingPads_, {});
    recordedPads_ = 0;
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
        for (const auto& bound : action.inputs) {
            const auto binding = parseBinding(bound);
            const float value = raw(binding.input);
            const float directed = binding.direction == 0  ? std::abs(value)
                                   : binding.direction < 0 ? -value
                                                           : value;
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
            value += raw(input) >= DefaultThreshold ? 1.0f : 0.0f;
        }
        for (const auto& input : axis.negative) {
            value -= raw(input) >= DefaultThreshold ? 1.0f : 0.0f;
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
