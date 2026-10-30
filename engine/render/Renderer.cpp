#include "engine/render/Renderer.h"
#include "engine/assets/Image.h"
#include "engine/core/Log.h"
#include "engine/platform/Window.h"
#include "engine/render/FrameRenderer.h"
#include <algorithm>
#include <array>
#include <bx/allocator.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>

namespace fe {
namespace {
constexpr std::uint32_t ResetFlags = BGFX_RESET_MAXANISOTROPY;

bgfx::TextureFormat::Enum backBufferFormat(OutputEncoding encoding) {
    switch (encoding) {
    case OutputEncoding::ScRgb:
        return bgfx::TextureFormat::RGBA16F;
    case OutputEncoding::Hdr10:
        return bgfx::TextureFormat::RGB10A2;
    case OutputEncoding::Srgb:
        break;
    }
    return bgfx::TextureFormat::BGRA8;
}

bool backBufferSupported(bgfx::TextureFormat::Enum format) {
    return (bgfx::getCaps()->formats[format] & BGFX_CAPS_FORMAT_TEXTURE_BACKBUFFER) != 0;
}

// The inverse of pqEncode, for screenshots of an HDR10 back buffer.
float pqDecode(float encoded) {
    constexpr float m1 = 2610.0f / 16384, m2 = 2523.0f / 4096 * 128, c1 = 3424.0f / 4096,
                    c2 = 2413.0f / 4096 * 32, c3 = 2392.0f / 4096 * 32;
    const float e = std::pow(std::clamp(encoded, 0.0f, 1.0f), 1.0f / m2);
    return std::pow(std::max(e - c1, 0.0f) / (c2 - c3 * e), 1.0f / m1) * 10000.0f;
}

glm::vec3 rec2020ToRec709(glm::vec3 c) {
    return {1.6605f * c.x - 0.5876f * c.y - 0.0728f * c.z, -0.1246f * c.x + 1.1329f * c.y - 0.0083f * c.z,
            -0.0182f * c.x - 0.1006f * c.y + 1.1187f * c.z};
}

std::uint8_t toByte(float srgb) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
}
} // namespace

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
    OutputColor output; // how an HDR back buffer's pixels were encoded

    // An HDR back buffer's pixel back in the sRGB the frame was composed in,
    // so a screenshot under --hdr still looks like the SDR one (what exceeds
    // the paper white clips).
    [[nodiscard]] std::array<std::uint8_t, 3> decode(const std::uint8_t* pixel, bgfx::TextureFormat::Enum format) const {
        glm::vec3 linear;
        if (format == bgfx::TextureFormat::RGBA16F) {
            std::uint16_t halves[3];
            std::memcpy(halves, pixel, sizeof(halves));
            linear = glm::vec3(halfToFloat(halves[0]), halfToFloat(halves[1]), halfToFloat(halves[2])) /
                     (output.paperWhite / std::max(output.referenceWhite, 1.0f));
        } else {
            std::uint32_t packed = 0;
            std::memcpy(&packed, pixel, sizeof(packed));
            const auto channel = [&](unsigned shift) {
                return pqDecode(static_cast<float>((packed >> shift) & 0x3ffu) / 1023.0f);
            };
            linear = rec2020ToRec709(glm::vec3(channel(0), channel(10), channel(20)) / output.paperWhite);
        }
        return {toByte(linearToSrgb(linear.x)), toByte(linearToSrgb(linear.y)), toByte(linearToSrgb(linear.z))};
    }

    void screenShot(const char* path, std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                    bgfx::TextureFormat::Enum format, const void* data, std::uint32_t, bool flip) override {
        const bool hdr = format == bgfx::TextureFormat::RGBA16F || format == bgfx::TextureFormat::RGB10A2;
        if (!hdr && format != bgfx::TextureFormat::RGBA8 && format != bgfx::TextureFormat::BGRA8) {
            log(LogLevel::Error, "Screenshot skipped: unsupported back buffer format " +
                                     std::to_string(static_cast<int>(format)));
            return;
        }
        const bool bgra = format == bgfx::TextureFormat::BGRA8;
        const std::size_t bytes = format == bgfx::TextureFormat::RGBA16F ? 8 : 4;
        Image image;
        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
        const auto* source = static_cast<const std::uint8_t*>(data);
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* row = source + static_cast<std::size_t>(flip ? height - 1 - y : y) * pitch;
            for (std::uint32_t x = 0; x < width; ++x) {
                auto* out = &image.rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                if (hdr) {
                    const auto rgb = decode(row + x * bytes, format);
                    out[0] = rgb[0];
                    out[1] = rgb[1];
                    out[2] = rgb[2];
                } else {
                    out[0] = row[x * 4 + (bgra ? 2 : 0)];
                    out[1] = row[x * 4 + 1];
                    out[2] = row[x * 4 + (bgra ? 0 : 2)];
                }
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

Renderer::Renderer(Window& window, const std::filesystem::path& root, const OutputColor& wanted)
    : window_(window), shaderRoot_(root), output_(wanted), callback_(std::make_unique<Callback>()) {
    // Submission and rendering stay on the SDL thread (required on macOS).
#if BGFX_CONFIG_MULTITHREADED
    bgfx::renderFrame();
#endif
    auto [width, height] = window.pixelSize();
    width_ = width;
    height_ = height;
    // Ask for the 16F swap chain first; bgfx falls back to 8 bits when the
    // backend cannot present it, and the caps say which it is afterwards.
    const bool wantHdr = wanted.encoding != OutputEncoding::Srgb;
    output_.encoding = wantHdr ? OutputEncoding::ScRgb : OutputEncoding::Srgb;
    bgfx::Init init;
    init.platformData.type = window.handleType();
    init.swapChain = swapChain();
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
    if (wantHdr) {
        if (backBufferSupported(bgfx::TextureFormat::RGBA16F)) {
            output_.encoding = OutputEncoding::ScRgb;
        } else if (backBufferSupported(bgfx::TextureFormat::RGB10A2)) {
            output_.encoding = OutputEncoding::Hdr10;
            resetPending_ = true; // the first frame resets the swap chain to it
        } else {
            output_.encoding = OutputEncoding::Srgb;
        }
#if defined(__APPLE__)
        // An extended-range Metal layer puts the display's SDR white at 1.0.
        output_.referenceWhite = output_.paperWhite;
#endif
        if (output_.encoding == OutputEncoding::Srgb) {
            log(LogLevel::Warning,
                std::string("HDR output unavailable on ") + name() + ": the back buffer stays 8-bit");
        } else {
            const bool headroom = (bgfx::getCaps()->supported & BGFX_CAPS_HDR10) != 0;
            log(LogLevel::Info, std::string("HDR output: ") +
                                    (output_.encoding == OutputEncoding::ScRgb ? "RGBA16F scRGB" : "RGB10A2 HDR10") +
                                    ", paper white " + std::to_string(static_cast<int>(output_.paperWhite)) +
                                    " nits" + (headroom ? "" : " (the display reports no HDR headroom)"));
        }
    }
    callback_->output = output_;
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

bgfx::SwapChain Renderer::swapChain() const {
    auto chain = window_.swapChain();
    chain.formatColor = backBufferFormat(output_.encoding);
    if (output_.encoding != OutputEncoding::Srgb) {
        chain.flags |= BGFX_SWAP_CHAIN_HDR10; // scRGB or PQ colour space, per the format
    }
    return chain;
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
        auto chain = swapChain();
        chain.width = static_cast<std::uint32_t>(width);
        chain.height = static_cast<std::uint32_t>(height);
        bgfx::reset(ResetFlags | (vsync_ ? BGFX_RESET_VSYNC : 0u), &chain);
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
