#pragma once
#include <bgfx/bgfx.h>
#include <utility>

namespace fe {
// Where the renderer's main swap chain presents: the game's SDL window, or
// nothing at all for a host (the editor application) whose views each bring
// their own swap chain.
class RenderSurface {
public:
    virtual ~RenderSurface() = default;
    // The native handles and formats of the swap chain; a null `nwh` starts
    // the renderer headless, with no main swap chain.
    [[nodiscard]] virtual bgfx::SwapChain swapChain() const = 0;
    // What swapChain() handles are: Wayland surfaces or the platform default
    // (X11 on Linux). bgfx::Init::platformData.type must match.
    [[nodiscard]] virtual bgfx::NativeWindowHandleType::Enum handleType() const = 0;
    [[nodiscard]] virtual std::pair<int, int> pixelSize() const = 0;
    [[nodiscard]] virtual bool minimized() const = 0;
};
} // namespace fe
