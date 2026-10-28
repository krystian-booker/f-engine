#include "engine/render/Renderer.h"
#include "engine/platform/Window.h"
#include "engine/core/Log.h"
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>

namespace fe {
Renderer::Renderer(Window& window, const std::filesystem::path& root) : window_(window),shaderRoot_(root) {
    // Keep submission and rendering on the SDL/main thread (especially Metal).
#if BGFX_CONFIG_MULTITHREADED
    bgfx::renderFrame();
#endif
    bgfx::Init init;
#if defined(__APPLE__)
    init.type = bgfx::RendererType::Metal;
#elif defined(_WIN32)
    init.type = bgfx::RendererType::Direct3D11;
#else
    init.type = bgfx::RendererType::Vulkan;
#endif
    init.swapChain = window.swapChain();
    init.fallback = false;
    auto [width, height] = window.pixelSize();
    width_ = width; height_ = height;
    init.swapChain.width = static_cast<std::uint32_t>(width);
    init.swapChain.height = static_cast<std::uint32_t>(height);
    init.reset = BGFX_RESET_VSYNC;
    if (!bgfx::init(init)) throw std::runtime_error("bgfx initialization failed");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForOther(window.native())) {
        ImGui::DestroyContext(); bgfx::shutdown();
        throw std::runtime_error("ImGui SDL3 initialization failed");
    }
    try { imgui_ = std::make_unique<ImGuiRenderer>(root); }
    catch (...) { ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext(); bgfx::shutdown(); throw; }
    log(LogLevel::Info, std::string("Renderer: ") + name());
}
Renderer::~Renderer() {
    imgui_.reset();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    bgfx::shutdown();
}
void Renderer::processEvent(const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); }
bool Renderer::beginFrame() {
    const auto [width, height] = window_.pixelSize();
    if (width <= 0 || height <= 0 || window_.minimized()) return false;
    if (width != width_ || height != height_) {
        width_ = width; height_ = height;
        auto swapChain = window_.swapChain();
        swapChain.width = static_cast<std::uint32_t>(width);
        swapChain.height = static_cast<std::uint32_t>(height);
        bgfx::reset(BGFX_RESET_VSYNC, &swapChain);
    }
    bgfx::setViewName(0, "World");
    bgfx::setViewRect(0, 0, 0, static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clearColor_, 1.0f, 0);
    bgfx::touch(0);
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return true;
}
void Renderer::endFrame() {
    ImGui::Render();
    imgui_->render(ImGui::GetDrawData(), 255);
    bgfx::frame();
}
const char* Renderer::name() const { return bgfx::getRendererName(bgfx::getRendererType()); }
void Renderer::reloadShaders() {imgui_->reload(shaderRoot_);log(LogLevel::Info,"UI shaders reloaded");}
}
