#pragma once
#include <cstddef>
#include <optional>
#include <string>

namespace fe {
// The text clipboard behind fe.app.clipboard and fe.app.set_clipboard. The
// windowed runtime hands the engine the system clipboard (SdlClipboard in
// engine/platform/Window.h) through EngineOptions::clipboard; without one the
// engine keeps a string of its own, which is also what headless runs, tests,
// replays and recordings use, so a replay never reads the machine's clipboard.
class Clipboard {
public:
    virtual ~Clipboard() = default;
    // The clipboard's text; nullopt when it holds none.
    [[nodiscard]] virtual std::optional<std::string> read() = 0;
    virtual void write(const std::string& text) = 0;
};

// The most text fe.app.clipboard returns (longer text is cut at a UTF-8
// boundary) and fe.app.set_clipboard accepts.
inline constexpr std::size_t MaxClipboardBytes = 64 * 1024;

class MemoryClipboard final : public Clipboard {
public:
    [[nodiscard]] std::optional<std::string> read() override { return text_; }
    void write(const std::string& text) override { text_ = text; }

private:
    std::optional<std::string> text_;
};
} // namespace fe
