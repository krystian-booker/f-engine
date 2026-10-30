#pragma once
#include <glm/glm.hpp>
#include <string>
#include <string_view>

namespace fe {
// Receives everything fed to InputState (raw inputs, mouse position, wheel,
// typed text and IME pre-edit strings) in the order it arrives, so a played
// session can be written down and replayed. ReplayRecorder (engine/runtime)
// implements it; InputState::setRecorder attaches one.
class InputRecorder {
public:
    virtual ~InputRecorder() = default;
    virtual void input(const std::string& name, float value) = 0;
    virtual void mouse(glm::vec2 canvasPosition) = 0;
    virtual void wheel(float delta) = 0;
    virtual void text(std::string_view utf8) = 0;
    virtual void composition(const std::string& text, int cursor) = 0;
};
} // namespace fe
