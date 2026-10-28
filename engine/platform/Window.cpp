#include "engine/platform/Window.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace fe {
Window::Window(const GameConfig& config) {
#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
    window_ = SDL_CreateWindow(config.title.c_str(), config.windowWidth, config.windowHeight, flags);
    if (!window_) {
        const std::string message = std::string("SDL_CreateWindow: ") + SDL_GetError();
        SDL_Quit();
        throw std::runtime_error(message);
    }
#if defined(__APPLE__)
    metalView_ = SDL_Metal_CreateView(window_);
    if (!metalView_) {
        const std::string message = std::string("SDL_Metal_CreateView: ") + SDL_GetError();
        SDL_DestroyWindow(window_);
        SDL_Quit();
        throw std::runtime_error(message);
    }
#endif
}
Window::~Window() {
    if (metalView_)
        SDL_Metal_DestroyView(metalView_);
    if (window_)
        SDL_DestroyWindow(window_);
    SDL_Quit();
}
bgfx::SwapChain Window::swapChain() const {
    bgfx::SwapChain data{};
    // SwapChain's neutral defaults mean "keep existing" during reset; the
    // initial chain must specify actual formats before Metal creates its layer.
    data.formatColor = bgfx::TextureFormat::BGRA8;
    data.formatDepthStencil = bgfx::TextureFormat::D24S8;
    data.numBackBuffers = 2;
    const auto properties = SDL_GetWindowProperties(window_);
    (void)properties;
#if defined(__APPLE__)
    data.nwh = SDL_Metal_GetLayer(metalView_);
#elif defined(_WIN32)
    data.nwh = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__linux__)
    data.ndt = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    data.nwh = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
#else
#error Unsupported platform
#endif
    if (!data.nwh)
        throw std::runtime_error("No native window handle for bgfx");
    return data;
}
std::pair<int, int> Window::pixelSize() const {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return {width, height};
}
bool Window::minimized() const {
    return SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED;
}
} // namespace fe
