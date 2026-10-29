#include "engine/render/Renderer.h"
#include "engine/assets/Image.h"
#include "engine/core/Log.h"
#include "engine/platform/Window.h"
#include "engine/render/FrameRenderer.h"
#include <bx/allocator.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>

namespace fe {
namespace {
constexpr std::uint32_t ResetFlags = BGFX_RESET_MAXANISOTROPY;
}

// bgfx callbacks: screenshots become PNG files; debug builds log bgfx traces
// (including its warnings).
struct Renderer::Callback final : bgfx::CallbackI {
    void fatal(const char* file, std::uint16_t line, bgfx::Fatal::Enum, const char* message) override {
        log(LogLevel::Error,
            std::string("bgfx fatal at ") + file + ":" + std::to_string(line) + ": " + message);
        std::abort();
    }
#ifdef NDEBUG
    void traceVargs(const char*, std::uint16_t, const char*, va_list) override {}
#else
    void traceVargs(const char* file, std::uint16_t line, const char* format, va_list arguments) override {
        char message[1024];
        std::vsnprintf(message, sizeof(message), format, arguments);
        std::string text = message;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        log(LogLevel::Debug, std::string("bgfx ") + file + ":" + std::to_string(line) + ": " + text);
    }
#endif
    void profilerBegin(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerBeginLiteral(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerEnd() override {}
    std::uint32_t cacheReadSize(std::uint64_t) override { return 0; }
    bool cacheRead(std::uint64_t, void*, std::uint32_t) override { return false; }
    void cacheWrite(std::uint64_t, const void*, std::uint32_t) override {}
    void screenShot(const char* path, std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                    bgfx::TextureFormat::Enum format, const void* data, std::uint32_t, bool flip) override {
        if (format != bgfx::TextureFormat::RGBA8 && format != bgfx::TextureFormat::BGRA8) {
            log(LogLevel::Error, "Screenshot skipped: unsupported back buffer format " +
                                     std::to_string(static_cast<int>(format)));
            return;
        }
        const bool bgra = format == bgfx::TextureFormat::BGRA8;
        Image image;
        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
        const auto* source = static_cast<const std::uint8_t*>(data);
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* row = source + static_cast<std::size_t>(flip ? height - 1 - y : y) * pitch;
            for (std::uint32_t x = 0; x < width; ++x) {
                auto* out = &image.rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                out[0] = row[x * 4 + (bgra ? 2 : 0)];
                out[1] = row[x * 4 + 1];
                out[2] = row[x * 4 + (bgra ? 0 : 2)];
                out[3] = 255;
            }
        }
        try {
            image.savePng(path);
            log(LogLevel::Info, std::string("Wrote screenshot ") + path);
        } catch (const std::exception& error) {
            log(LogLevel::Error, error.what());
        }
    }
    void captureBegin(std::uint32_t, std::uint32_t, std::uint32_t, bgfx::TextureFormat::Enum, bool) override {
    }
    void captureEnd() override {}
    void captureFrame(const void*, std::uint32_t) override {}
};

Renderer::Renderer(Window& window, const std::filesystem::path& root)
    : window_(window), shaderRoot_(root), callback_(std::make_unique<Callback>()) {
    // Submission and rendering stay on the SDL thread (required on macOS).
#if BGFX_CONFIG_MULTITHREADED
    bgfx::renderFrame();
#endif
    auto [width, height] = window.pixelSize();
    width_ = width;
    height_ = height;
    bgfx::Init init;
    init.platformData.type = window.handleType();
    init.swapChain = window.swapChain();
    init.swapChain.width = static_cast<std::uint32_t>(width);
    init.swapChain.height = static_cast<std::uint32_t>(height);
    init.reset = ResetFlags | BGFX_RESET_VSYNC;
    init.callback = callback_.get();
    // Sprites, particles and overlays stream through transient buffers.
    init.limits.maxTransientVbSize = 32u << 20u;
    init.limits.maxTransientIbSize = 8u << 20u;
#if defined(__APPLE__)
    const bgfx::RendererType::Enum preferred[] = {bgfx::RendererType::Metal};
#elif defined(_WIN32)
    const bgfx::RendererType::Enum preferred[] = {bgfx::RendererType::Direct3D11};
#else
    // Vulkan first; OpenGL when no Vulkan driver is available.
    const bgfx::RendererType::Enum preferred[] = {bgfx::RendererType::Vulkan, bgfx::RendererType::OpenGL};
#endif
    bool started = false;
    for (const auto type : preferred) {
        init.type = type;
        if (bgfx::init(init)) {
            started = true;
            break;
        }
        log(LogLevel::Warning, std::string("Renderer backend unavailable: ") + bgfx::getRendererName(type));
    }
    if (!started) {
        throw std::runtime_error("bgfx could not start any rendering backend");
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForOther(window.native())) {
        ImGui::DestroyContext();
        bgfx::shutdown();
        throw std::runtime_error("ImGui SDL3 initialisation failed");
    }
    try {
        imgui_ = std::make_unique<ImGuiRenderer>(root);
    } catch (...) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        bgfx::shutdown();
        throw;
    }
    log(LogLevel::Info, std::string("Renderer: ") + name());
}

Renderer::~Renderer() {
    imgui_.reset();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    bgfx::shutdown();
}

void Renderer::setVsync(bool on) {
    resetPending_ = resetPending_ || on != vsync_;
    vsync_ = on;
}

void Renderer::processEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

bool Renderer::beginFrame() {
    const auto [width, height] = window_.pixelSize();
    if (width <= 0 || height <= 0 || window_.minimized()) {
        return false;
    }
    if (width != width_ || height != height_ || resetPending_) {
        width_ = width;
        height_ = height;
        resetPending_ = false;
        auto swapChain = window_.swapChain();
        swapChain.width = static_cast<std::uint32_t>(width);
        swapChain.height = static_cast<std::uint32_t>(height);
        bgfx::reset(ResetFlags | (vsync_ ? BGFX_RESET_VSYNC : 0u), &swapChain);
    }
    bgfx::setViewRect(0, 0, 0, static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
    bgfx::touch(0);
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return true;
}

std::uint32_t Renderer::endFrame() {
    ImGui::Render();
    imgui_->render(ImGui::GetDrawData(), static_cast<std::uint16_t>(bgfx::getCaps()->limits.maxViews - 1));
    return bgfx::frame();
}

void Renderer::capture(const std::filesystem::path& file) {
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, file.string().c_str());
}

const char* Renderer::name() const {
    return bgfx::getRendererName(bgfx::getRendererType());
}

void Renderer::reloadShaders() {
    imgui_->reload(shaderRoot_);
    if (frame_) {
        frame_->reload();
    }
    log(LogLevel::Info, "Shaders reloaded");
}
} // namespace fe
