#include "engine/platform/Window.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace fe {
namespace {
#if defined(__linux__)
bool wayland() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && std::string_view(driver) == "wayland";
}
#endif
} // namespace

Window::Window(const GameConfig& config) {
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
#if defined(__APPLE__)
    data.nwh = SDL_Metal_GetLayer(metalView_);
#elif defined(_WIN32)
    data.nwh =
        SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__linux__)
    const auto properties = SDL_GetWindowProperties(window_);
    if (wayland()) {
        data.ndt = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        data.nwh = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        data.ndt = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        data.nwh = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
            SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
    }
#else
#error Unsupported platform
#endif
    if (!data.nwh)
        throw std::runtime_error("No native window handle for bgfx");
    return data;
}
bgfx::NativeWindowHandleType::Enum Window::handleType() const {
#if defined(__linux__)
    if (wayland()) {
        return bgfx::NativeWindowHandleType::Wayland;
    }
#endif
    return bgfx::NativeWindowHandleType::Default;
}

std::pair<int, int> Window::pixelSize() const {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return {width, height};
}
bool Window::minimized() const {
    return SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED;
}

std::vector<Monitor> Window::monitors() {
    std::vector<Monitor> out;
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    for (int i = 0; ids && i < count; ++i) {
        Monitor monitor;
        const char* name = SDL_GetDisplayName(ids[i]);
        monitor.name = name ? name : "Display " + std::to_string(i + 1);
        if (const auto* desktop = SDL_GetDesktopDisplayMode(ids[i])) {
            monitor.desktop = {desktop->w, desktop->h, desktop->refresh_rate};
        }
        int modeCount = 0;
        if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(ids[i], &modeCount)) {
            for (int m = 0; m < modeCount; ++m) {
                const DisplayMode mode{modes[m]->w, modes[m]->h, modes[m]->refresh_rate};
                const bool known =
                    std::any_of(monitor.modes.begin(), monitor.modes.end(), [&](const DisplayMode& other) {
                        return other.width == mode.width && other.height == mode.height &&
                               other.refresh == mode.refresh;
                    });
                if (!known) {
                    monitor.modes.push_back(mode);
                }
            }
            SDL_free(modes);
        }
        out.push_back(std::move(monitor));
    }
    SDL_free(ids);
    return out;
}

void Window::describe(DisplayState& state) const {
    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    const SDL_DisplayMode* exclusive = fullscreen ? SDL_GetWindowFullscreenMode(window_) : nullptr;
    if (exclusive) {
        state.fullscreen = FullscreenMode::Exclusive;
        state.width = exclusive->w;
        state.height = exclusive->h;
    } else {
        state.fullscreen = fullscreen ? FullscreenMode::Borderless : FullscreenMode::Window;
        SDL_GetWindowSize(window_, &state.width, &state.height);
    }
    const SDL_DisplayID current = SDL_GetDisplayForWindow(window_);
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    for (int i = 0; ids && i < count; ++i) {
        if (ids[i] == current) {
            state.monitor = i + 1;
        }
    }
    SDL_free(ids);
}

void Window::apply(const DisplayState& target) {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    const SDL_DisplayID current = SDL_GetDisplayForWindow(window_);
    const SDL_DisplayID display =
        ids && target.monitor >= 1 && target.monitor <= count ? ids[target.monitor - 1] : current;
    SDL_free(ids);
    const bool move = display != 0 && display != current;
    if (target.fullscreen == FullscreenMode::Window || move) {
        SDL_SetWindowFullscreen(window_, false);
        SDL_SyncWindow(window_);
    }
    SDL_SetWindowSize(window_, target.width, target.height);
    if (move) {
        SDL_SetWindowPosition(window_, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)),
                              static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)));
        SDL_SyncWindow(window_);
    }
    if (target.fullscreen == FullscreenMode::Exclusive) {
        SDL_DisplayMode mode{};
        if (SDL_GetClosestFullscreenDisplayMode(display, target.width, target.height, 0.0f, false, &mode)) {
            SDL_SetWindowFullscreenMode(window_, &mode);
        } else {
            SDL_SetWindowFullscreenMode(window_, nullptr);
        }
        SDL_SetWindowFullscreen(window_, true);
    } else if (target.fullscreen == FullscreenMode::Borderless) {
        SDL_SetWindowFullscreenMode(window_, nullptr);
        SDL_SetWindowFullscreen(window_, true);
    }
    SDL_SyncWindow(window_);
}

float Window::refreshRate() const {
    const auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0f;
}

std::string preferredLocale() {
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    std::string tag;
    if (locales && count > 0 && locales[0]->language) {
        tag = locales[0]->language;
        if (locales[0]->country) {
            tag += std::string("-") + locales[0]->country;
        }
    }
    SDL_free(locales);
    return tag.empty() ? "en-US" : tag;
}

void WindowActivity::handle(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        focused_ = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focused_ = false;
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        minimized_ = true;
        break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        minimized_ = false;
        break;
    default:
        break;
    }
}
} // namespace fe
