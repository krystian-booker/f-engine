#include "apps/editor/NativeSurface.h"
#include <QGuiApplication>
#include <QWindow>
#include <cstdint>
#if defined(__linux__)
#include <qpa/qplatformnativeinterface.h>
#endif

namespace fe::editor {
namespace {
bool wayland() {
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}
} // namespace

void* nativeDisplay() {
#if defined(__linux__)
    return QGuiApplication::platformNativeInterface()->nativeResourceForIntegration(wayland() ? "wl_display"
                                                                                              : "display");
#else
    return nullptr;
#endif
}

void* nativeWindow(QWindow* window) {
#if defined(__linux__)
    if (wayland()) {
        return QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window);
    }
#endif
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(window->winId()));
}

bgfx::NativeWindowHandleType::Enum nativeHandleType() {
    return wayland() ? bgfx::NativeWindowHandleType::Wayland : bgfx::NativeWindowHandleType::Default;
}

bgfx::SwapChain HeadlessSurface::swapChain() const {
    bgfx::SwapChain chain;
    chain.nwh = nullptr;
    chain.ndt = nativeDisplay();
    chain.formatDepthStencil = bgfx::TextureFormat::D24S8;
    chain.numBackBuffers = 2;
    return chain;
}
} // namespace fe::editor
