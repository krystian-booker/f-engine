#pragma once
#include "engine/render/RenderSurface.h"
class QWindow;

namespace fe::editor {
// The window system's handles in the form bgfx takes them: the display
// connection (X11 Display*, wl_display*; null elsewhere) and a window (X11
// window id, wl_surface*, HWND, NSView*).
[[nodiscard]] void* nativeDisplay();
[[nodiscard]] void* nativeWindow(QWindow* window);
[[nodiscard]] bgfx::NativeWindowHandleType::Enum nativeHandleType();

// The editor's main render surface: none. bgfx starts without a swap chain
// and every viewport brings its own (ViewportWindow), so views can come and
// go with the dock layout.
class HeadlessSurface final : public RenderSurface {
public:
    [[nodiscard]] bgfx::SwapChain swapChain() const override;
    [[nodiscard]] bgfx::NativeWindowHandleType::Enum handleType() const override {
        return nativeHandleType();
    }
    [[nodiscard]] std::pair<int, int> pixelSize() const override { return {16, 16}; }
    [[nodiscard]] bool minimized() const override { return false; }
};
} // namespace fe::editor
