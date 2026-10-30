#pragma once
#include "engine/core/GameConfig.h"
#include "engine/platform/Display.h"
#include "engine/services/Clipboard.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <bgfx/bgfx.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fe {
class Window {
public:
    explicit Window(const GameConfig& config);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    [[nodiscard]] SDL_Window* native() const { return window_; }
    [[nodiscard]] bgfx::SwapChain swapChain() const;
    // What swapChain() handles are: Wayland surfaces or the platform default
    // (X11 on Linux). bgfx::Init::platformData.type must match.
    [[nodiscard]] bgfx::NativeWindowHandleType::Enum handleType() const;
    [[nodiscard]] std::pair<int, int> pixelSize() const;
    [[nodiscard]] bool minimized() const;
    // Connected monitors in SDL's order, with their exclusive fullscreen modes.
    [[nodiscard]] static std::vector<Monitor> monitors();
    // Fills the window's fullscreen mode, size and monitor into `state`.
    void describe(DisplayState& state) const;
    // Moves, resizes and switches the window to `target` (vsync is the
    // renderer's business); waits until the window system has done it.
    void apply(const DisplayState& target);
    // Refresh rate of the window's monitor in Hz (60 when unknown).
    [[nodiscard]] float refreshRate() const;

private:
    SDL_Window* window_ = nullptr;
    SDL_MetalView metalView_ = nullptr;
};

// The player's preferred locale as a BCP 47 tag ("en-US"), "en-US" when the
// system has no preference.
std::string preferredLocale();

// The system clipboard (SDL_GetClipboardText / SDL_SetClipboardText); needs
// SDL's video subsystem, so a Window must exist. The runtime supplies it as
// EngineOptions::clipboard, except to replays and recordings.
class SdlClipboard final : public Clipboard {
public:
    [[nodiscard]] std::optional<std::string> read() override;
    void write(const std::string& text) override;
};

// Whether the window is in the foreground (focused and not minimised), from
// SDL window events.
class WindowActivity {
public:
    void handle(const SDL_Event& event);
    [[nodiscard]] bool foreground() const { return focused_ && !minimized_; }

private:
    bool focused_ = true, minimized_ = false;
};
} // namespace fe
