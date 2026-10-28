#pragma once
#include "engine/render/ImGuiRenderer.h"
#include <filesystem>
#include <memory>
union SDL_Event;

namespace fe {
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
    void endFrame();
    void reloadShaders();
    // Writes the next frame to a PNG file.
    void capture(const std::filesystem::path& file);
    [[nodiscard]] const char* name() const;
    [[nodiscard]] const std::filesystem::path& shaderRoot() const { return shaderRoot_; }

private:
    struct Callback;
    Window& window_;
    std::filesystem::path shaderRoot_;
    int width_ = 0, height_ = 0;
    std::unique_ptr<Callback> callback_;
    std::unique_ptr<ImGuiRenderer> imgui_;
};
} // namespace fe
