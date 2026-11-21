#pragma once
#include "engine/render/FrameMath.h"
#include "engine/render/ImGuiRenderer.h"
#include <filesystem>
#include <functional>
#include <memory>

namespace fe {
class FrameRenderer;
class RenderSurface;
// bgfx and ImGui lifetime, frame boundaries and screenshots. ImGui gets its
// input and display size from a platform backend the host installs (SDL in
// the runtime); the renderer itself knows no window system.
class Renderer {
public:
    // `wanted` asks for an HDR back buffer (any encoding but Srgb, with the
    // paper white to present at): RGBA16F (scRGB) when the backend offers it,
    // else RGB10A2 (HDR10), else the 8-bit buffer; output() says which.
    Renderer(RenderSurface& surface, const std::filesystem::path& shaderRoot, const OutputColor& wanted = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    // Called in beginFrame() just before ImGui::NewFrame: the platform
    // backend's own new-frame step (display size, time, input).
    void setImGuiPlatformFrame(std::function<void()> newFrame) { imguiPlatformFrame_ = std::move(newFrame); }
    // Where endFrame() draws ImGui: a framebuffer (the editor application's
    // Scene view), or the back buffer when invalid.
    void setImGuiTarget(bgfx::FrameBufferHandle target) { imguiTarget_ = target; }
    // False while minimised; otherwise starts ImGui and resets on resize.
    bool beginFrame();
    // Submits the frame; returns bgfx's frame number (see bgfx::read).
    std::uint32_t endFrame();
    // Reloads the ImGui program and every program of the attached frame renderer.
    void reloadShaders();
    // The frame renderer whose programs reloadShaders() also reloads (null to detach).
    void attach(FrameRenderer* frame) { frame_ = frame; }
    // Waits for vertical blank when presenting (applied at the next frame).
    void setVsync(bool on);
    [[nodiscard]] bool vsync() const { return vsync_; }
    // Writes the next frame to a PNG file.
    void capture(const std::filesystem::path& file);
    [[nodiscard]] const char* name() const;
    [[nodiscard]] const std::filesystem::path& shaderRoot() const { return shaderRoot_; }
    // How the screen pass must encode the frame for the back buffer it got.
    [[nodiscard]] const OutputColor& output() const { return output_; }

private:
    struct Callback;
    // The surface's swap chain with the back buffer format output() needs.
    [[nodiscard]] bgfx::SwapChain swapChain() const;
    RenderSurface& surface_;
    std::filesystem::path shaderRoot_;
    OutputColor output_;
    int width_ = 0, height_ = 0;
    bool vsync_ = true, resetPending_ = false;
    std::unique_ptr<Callback> callback_;
    std::unique_ptr<ImGuiRenderer> imgui_;
    std::function<void()> imguiPlatformFrame_;
    bgfx::FrameBufferHandle imguiTarget_ = BGFX_INVALID_HANDLE;
    FrameRenderer* frame_ = nullptr;
};
} // namespace fe
