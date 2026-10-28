#pragma once
#include "engine/render/ImGuiRenderer.h"
#include <filesystem>
#include <memory>
union SDL_Event;
namespace fe {
class Window;
class Renderer {
public:
    Renderer(Window& window, const std::filesystem::path& shaderRoot);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void processEvent(const SDL_Event& event);
    bool beginFrame();
    void endFrame();
    void reloadShaders();
    [[nodiscard]] const char* name() const;
    void setClearColor(std::uint32_t rgba) { clearColor_ = rgba; }
private:
    Window& window_;
    std::filesystem::path shaderRoot_;
    int width_ = 0, height_ = 0;
    std::uint32_t clearColor_ = 0x202d40ff;
    std::unique_ptr<ImGuiRenderer> imgui_;
};
}
