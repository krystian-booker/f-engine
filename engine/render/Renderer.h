#pragma once
#include "engine/render/ImGuiRenderer.h"
#include <filesystem>
#include <memory>
union SDL_Event;

namespace fe {
class FrameRenderer;
class Window;
// bgfx and ImGui lifetime, frame boundaries and screenshots.
class Renderer {
public:
    Renderer(Window& window, const std::filesystem::path& shaderRoot);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void processEvent(const SDL_Event& event);
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

private:
    struct Callback;
    Window& window_;
    std::filesystem::path shaderRoot_;
    int width_ = 0, height_ = 0;
    bool vsync_ = true, resetPending_ = false;
    std::unique_ptr<Callback> callback_;
    std::unique_ptr<ImGuiRenderer> imgui_;
    FrameRenderer* frame_ = nullptr;
};
} // namespace fe
