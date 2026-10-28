#include "engine/render/ImGuiRenderer.h"
#include <imgui.h>
#include <bx/math.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace fe {
namespace {
const char* backendDirectory() {
    switch (bgfx::getRendererType()) {
    case bgfx::RendererType::Metal: return "metal";
    case bgfx::RendererType::Direct3D11: return "dx11";
    case bgfx::RendererType::Direct3D12: return "dx11";
    case bgfx::RendererType::Vulkan: return "spirv";
    case bgfx::RendererType::OpenGL: return "glsl";
    default: throw std::runtime_error("Unsupported shader backend");
    }
}
bgfx::ShaderHandle loadShader(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open shader " + file.string());
    const auto length = stream.tellg();
    if (length <= 0 || length > 64 * 1024 * 1024) throw std::runtime_error("Invalid shader size");
    stream.seekg(0);
    std::vector<char> bytes(static_cast<std::size_t>(length));
    if (!stream.read(bytes.data(), length)) throw std::runtime_error("Cannot read shader " + file.string());
    auto shader = bgfx::createShader(bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size())));
    if (!bgfx::isValid(shader)) throw std::runtime_error("Invalid shader " + file.string());
    bgfx::setName(shader, file.filename().string().c_str());
    return shader;
}
}
bgfx::ProgramHandle loadProgram(const std::filesystem::path& root, const char* vertex, const char* fragment) {
    const auto directory = root / backendDirectory();
    auto header=[](const std::filesystem::path& path) {
        std::array<unsigned char,12> bytes{};std::ifstream stream(path,std::ios::binary);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("Invalid shader header: "+path.string());
        return bytes;
    };
    const auto vertexHeader=header(directory/(std::string(vertex)+".bin")),fragmentHeader=header(directory/(std::string(fragment)+".bin"));
    if (vertexHeader[0]!='V' || fragmentHeader[0]!='F' || vertexHeader[1]!='S' || fragmentHeader[1]!='S' || vertexHeader[2]!='H' || fragmentHeader[2]!='H' ||
        vertexHeader[3]!=fragmentHeader[3] || !std::equal(vertexHeader.begin()+8,vertexHeader.begin()+12,fragmentHeader.begin()+4))
        throw std::runtime_error("Shader interfaces do not match; keeping the previous program");
    auto vs = loadShader(directory / (std::string(vertex) + ".bin"));
    bgfx::ShaderHandle fs = BGFX_INVALID_HANDLE;
    try { fs = loadShader(directory / (std::string(fragment) + ".bin")); }
    catch (...) { bgfx::destroy(vs); throw; }
    auto program = bgfx::createProgram(vs, fs, true);
    if (!bgfx::isValid(program)) throw std::runtime_error("Cannot link GPU program");
    return program;
}
ImGuiRenderer::ImGuiRenderer(const std::filesystem::path& root) {
    layout_.begin().add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true).end();
    program_ = loadProgram(root, "vs_ui", "fs_ui");
    sampler_ = bgfx::createUniform("s_texture", bgfx::UniformType::Sampler);
    unsigned char* pixels; int width, height;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    font_ = bgfx::createTexture2D(static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
        false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(pixels, static_cast<std::uint32_t>(width * height * 4)));
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->SetTexID(static_cast<ImTextureID>(font_.idx + 1));
    io.BackendRendererName = "fengine_bgfx";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
}
ImGuiRenderer::~ImGuiRenderer() {
    auto& io = ImGui::GetIO();
    io.Fonts->SetTexID(0);
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    if (bgfx::isValid(font_)) bgfx::destroy(font_);
    if (bgfx::isValid(sampler_)) bgfx::destroy(sampler_);
    if (bgfx::isValid(program_)) bgfx::destroy(program_);
}
void ImGuiRenderer::reload(const std::filesystem::path& root) {
    auto replacement=loadProgram(root,"vs_ui","fs_ui");
    if (bgfx::isValid(program_)) bgfx::destroy(program_);program_=replacement;
}
void ImGuiRenderer::render(ImDrawData* data, std::uint16_t view) {
    const auto width = static_cast<int>(data->DisplaySize.x * data->FramebufferScale.x);
    const auto height = static_cast<int>(data->DisplaySize.y * data->FramebufferScale.y);
    if (width <= 0 || height <= 0) return;
    float projection[16];
    bx::mtxOrtho(projection, data->DisplayPos.x, data->DisplayPos.x + data->DisplaySize.x,
        data->DisplayPos.y + data->DisplaySize.y, data->DisplayPos.y, 0, 1000, 0,
        bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewName(view, "UI");
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(view, 0, 0, static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));
    bgfx::setViewTransform(view, nullptr, projection);
    for (const auto* list : data->CmdLists) {
        const auto vertices = static_cast<std::uint32_t>(list->VtxBuffer.Size);
        const auto indices = static_cast<std::uint32_t>(list->IdxBuffer.Size);
        if (bgfx::getAvailTransientVertexBuffer(vertices, layout_) < vertices ||
            bgfx::getAvailTransientIndexBuffer(indices, sizeof(ImDrawIdx) == 4) < indices) break;
        bgfx::TransientVertexBuffer vb;
        bgfx::TransientIndexBuffer ib;
        bgfx::allocTransientVertexBuffer(&vb, vertices, layout_);
        bgfx::allocTransientIndexBuffer(&ib, indices, sizeof(ImDrawIdx) == 4);
        std::memcpy(vb.data, list->VtxBuffer.Data, vertices * sizeof(ImDrawVert));
        std::memcpy(ib.data, list->IdxBuffer.Data, indices * sizeof(ImDrawIdx));
        for (const auto& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState) command.UserCallback(list, &command);
                continue;
            }
            const auto clip = command.ClipRect;
            const auto x0 = std::clamp((clip.x - data->DisplayPos.x) * data->FramebufferScale.x, 0.0f, float(width));
            const auto y0 = std::clamp((clip.y - data->DisplayPos.y) * data->FramebufferScale.y, 0.0f, float(height));
            const auto x1 = std::clamp((clip.z - data->DisplayPos.x) * data->FramebufferScale.x, 0.0f, float(width));
            const auto y1 = std::clamp((clip.w - data->DisplayPos.y) * data->FramebufferScale.y, 0.0f, float(height));
            if (x1 <= x0 || y1 <= y0) continue;
            bgfx::setScissor(static_cast<std::uint16_t>(x0), static_cast<std::uint16_t>(y0),
                static_cast<std::uint16_t>(std::ceil(x1 - x0)), static_cast<std::uint16_t>(std::ceil(y1 - y0)));
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
                BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
            const bgfx::TextureHandle texture{static_cast<std::uint16_t>(command.GetTexID() - 1)};
            bgfx::setTexture(0, sampler_, texture);
            bgfx::setVertexBuffer(0, &vb, command.VtxOffset, vertices - command.VtxOffset);
            bgfx::setIndexBuffer(&ib, command.IdxOffset, command.ElemCount);
            bgfx::submit(view, program_);
        }
    }
}
}
