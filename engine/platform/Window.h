#pragma once
#include "engine/core/GameConfig.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <bgfx/bgfx.h>
#include <utility>

namespace fe {
class Window {
public:
    explicit Window(const GameConfig& config);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    [[nodiscard]] SDL_Window* native() const { return window_; }
    [[nodiscard]] bgfx::SwapChain swapChain() const;
    [[nodiscard]] std::pair<int, int> pixelSize() const;
    [[nodiscard]] bool minimized() const;

private:
    SDL_Window* window_ = nullptr;
    SDL_MetalView metalView_ = nullptr;
};
} // namespace fe
