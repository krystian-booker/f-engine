#pragma once
#include <QWindow>
#include <bgfx/bgfx.h>
#include <functional>
#include <glm/glm.hpp>

namespace fe::editor {
// A native window the renderer presents into through a swap chain of its
// own: the Scene view, the Game view. Embedded in the widget tree with
// QWidget::createWindowContainer.
//
// The swap chain follows the native window: it is rebuilt when the window is
// resized and dropped before Qt destroys the native surface (hiding the
// view, floating or re-docking its panel), then built again on the next
// sync() once the window is back.
class ViewportWindow final : public QWindow {
public:
    ViewportWindow();
    ~ViewportWindow() override;

    // Brings the swap chain in line with the native window. Runs before the
    // frame's first submission: replacing a swap chain flushes bgfx frames.
    void sync();
    // Drops the swap chain; the owner calls it before the renderer goes.
    void release();
    // Whether there is a swap chain to draw into this frame.
    [[nodiscard]] bool ready() const { return bgfx::isValid(target_); }
    [[nodiscard]] bgfx::FrameBufferHandle target() const { return target_; }
    [[nodiscard]] glm::ivec2 pixels() const { return pixels_; }
    // How many swap chains this view has built (diagnostics and tests).
    [[nodiscard]] int chainsBuilt() const { return chainsBuilt_; }

    // Pointer input, positions in swap chain pixels.
    struct Pointer {
        glm::vec2 position{0};
        Qt::MouseButtons buttons;
        Qt::KeyboardModifiers modifiers;
    };
    std::function<void(const Pointer&)> onPress, onMove, onRelease;
    std::function<void(float notches)> onWheel;
    std::function<void()> onLeave;
    // Keys pressed while the view has focus; the window's shortcuts do not
    // reach a native child window by themselves.
    std::function<void(QKeyEvent* event)> onKey;
    std::function<void(QKeyEvent* event)> onKeyRelease;
    // Files or project assets dropped on the view (paths as dragged).
    std::function<void(const QStringList& paths, glm::vec2 position)> onDrop;

protected:
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    void watchSurface();
    [[nodiscard]] Pointer pointer(const QMouseEvent& event) const;

    bgfx::FrameBufferHandle target_ = BGFX_INVALID_HANDLE;
    glm::ivec2 pixels_{0};
    void* window_ = nullptr;
    int chainsBuilt_ = 0;
};
} // namespace fe::editor
