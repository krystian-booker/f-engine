#include "apps/editor/ViewportWindow.h"
#include "apps/editor/NativeSurface.h"
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QWheelEvent>
#if defined(__linux__)
#include <qpa/qplatformwindow_p.h>
#endif

namespace fe::editor {
ViewportWindow::ViewportWindow() {
    // Not a raster surface: Qt must never paint into this window itself.
    setSurfaceType(QSurface::VulkanSurface);
}

ViewportWindow::~ViewportWindow() {
    release();
}

void ViewportWindow::sync() {
    if (!isExposed() || !handle()) {
        return;
    }
    const QSize size = this->size() * devicePixelRatio();
    const glm::ivec2 pixels(size.width(), size.height());
    void* window = nativeWindow(this);
    if (ready() && pixels == pixels_ && window == window_) {
        return;
    }
    release();
    if (!window || pixels.x <= 0 || pixels.y <= 0) {
        return;
    }
    bgfx::SwapChain chain;
    chain.nwh = window;
    chain.ndt = nativeDisplay();
    chain.width = static_cast<std::uint32_t>(pixels.x);
    chain.height = static_cast<std::uint32_t>(pixels.y);
    chain.formatColor = bgfx::TextureFormat::BGRA8;
    chain.formatDepthStencil = bgfx::TextureFormat::D24S8;
    chain.numBackBuffers = 2;
    target_ = bgfx::createFrameBuffer(chain);
    pixels_ = pixels;
    window_ = window;
    ++chainsBuilt_;
}

void ViewportWindow::release() {
    if (!ready()) {
        return;
    }
    bgfx::destroy(target_);
    target_ = BGFX_INVALID_HANDLE;
    window_ = nullptr;
    // The backend destroys the swap chain when it runs the frame's commands.
    // That has to happen now: the surface is about to go, and a surface can
    // carry only one swap chain at a time.
    bgfx::frame();
    bgfx::frame();
}

bool ViewportWindow::event(QEvent* event) {
    if (event->type() == QEvent::PlatformSurface) {
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            release();
        } else {
            watchSurface();
        }
    }
    if (event->type() == QEvent::Leave && onLeave) {
        onLeave();
    }
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        if (onDrop && (drag->mimeData()->hasUrls() || drag->mimeData()->hasText())) {
            drag->acceptProposedAction();
            return true;
        }
    }
    if (event->type() == QEvent::Drop && onDrop) {
        auto* drop = static_cast<QDropEvent*>(event);
        QStringList paths;
        for (const auto& url : drop->mimeData()->urls()) {
            paths << url.toLocalFile();
        }
        if (paths.isEmpty() && drop->mimeData()->hasText()) {
            paths << drop->mimeData()->text();
        }
        const QPointF at = drop->position() * devicePixelRatio();
        drop->acceptProposedAction();
        onDrop(paths, glm::vec2(at.x(), at.y()));
        return true;
    }
    return QWindow::event(event);
}

// Qt's Wayland windows drop their wl_surface whenever they are hidden or
// re-parented (floating a dock), without a platform surface event. The swap
// chain has to go first: a surface destroyed under a live Vulkan swap chain is
// a fatal protocol error. surfaceRoleDestroyed is the last signal before the
// surface goes; surfaceDestroyed comes too late.
void ViewportWindow::watchSurface() {
#if defined(__linux__)
    if (auto* native = nativeInterface<QNativeInterface::Private::QWaylandWindow>()) {
        QObject::connect(
            native, &QNativeInterface::Private::QWaylandWindow::surfaceRoleDestroyed, this,
            [this] { release(); }, Qt::DirectConnection);
    }
#endif
}

ViewportWindow::Pointer ViewportWindow::pointer(const QMouseEvent& event) const {
    const QPointF position = event.position() * devicePixelRatio();
    return {glm::vec2(position.x(), position.y()), event.buttons(), event.modifiers()};
}

void ViewportWindow::mousePressEvent(QMouseEvent* event) {
    if (onPress) {
        onPress(pointer(*event));
    }
}

void ViewportWindow::mouseMoveEvent(QMouseEvent* event) {
    if (onMove) {
        onMove(pointer(*event));
    }
}

void ViewportWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (onRelease) {
        onRelease(pointer(*event));
    }
}

void ViewportWindow::wheelEvent(QWheelEvent* event) {
    if (onWheel) {
        onWheel(static_cast<float>(event->angleDelta().y()) / 120.0f);
    }
}

void ViewportWindow::keyPressEvent(QKeyEvent* event) {
    if (onKey) {
        onKey(event);
    }
}

void ViewportWindow::keyReleaseEvent(QKeyEvent* event) {
    if (onKeyRelease) {
        onKeyRelease(event);
    }
}
} // namespace fe::editor
