#include "engine/render/ImGuiRenderer.h"
#include "engine/render/GpuAssets.h"
#include <algorithm>
#include <array>
#include <bx/math.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <stdexcept>
#include <vector>

namespace fe {
ImGuiRenderer::ImGuiRenderer(const std::filesystem::path& root) {
    layout_.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    program_ = Program(loadProgram(root, "vs_ui", "fs_ui"));
    sampler_ = Uniform(bgfx::createUniform("s_texture", bgfx::UniformType::Sampler));
    unsigned char* pixels;
    int width, height;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    font_ = Texture(bgfx::createTexture2D(
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(pixels, static_cast<std::uint32_t>(width * height * 4))));
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->SetTexID(static_cast<ImTextureID>(font_.get().idx + 1));
    io.BackendRendererName = "fengine_bgfx";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
}
ImGuiRenderer::~ImGuiRenderer() {
    auto& io = ImGui::GetIO();
    io.Fonts->SetTexID(0);
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
}
void ImGuiRenderer::reload(const std::filesystem::path& root) {
    program_ = Program(loadProgram(root, "vs_ui", "fs_ui"));
}
void ImGuiRenderer::render(ImDrawData* data, std::uint16_t view) {
    const auto width = static_cast<int>(data->DisplaySize.x * data->FramebufferScale.x);
    const auto height = static_cast<int>(data->DisplaySize.y * data->FramebufferScale.y);
    if (width <= 0 || height <= 0)
        return;
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
            bgfx::getAvailTransientIndexBuffer(indices, sizeof(ImDrawIdx) == 4) < indices)
            break;
        bgfx::TransientVertexBuffer vb;
        bgfx::TransientIndexBuffer ib;
        bgfx::allocTransientVertexBuffer(&vb, vertices, layout_);
        bgfx::allocTransientIndexBuffer(&ib, indices, sizeof(ImDrawIdx) == 4);
        std::memcpy(vb.data, list->VtxBuffer.Data, vertices * sizeof(ImDrawVert));
        std::memcpy(ib.data, list->IdxBuffer.Data, indices * sizeof(ImDrawIdx));
        for (const auto& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState)
                    command.UserCallback(list, &command);
                continue;
            }
            const auto clip = command.ClipRect;
            const auto x0 =
                std::clamp((clip.x - data->DisplayPos.x) * data->FramebufferScale.x, 0.0f, float(width));
            const auto y0 =
                std::clamp((clip.y - data->DisplayPos.y) * data->FramebufferScale.y, 0.0f, float(height));
            const auto x1 =
                std::clamp((clip.z - data->DisplayPos.x) * data->FramebufferScale.x, 0.0f, float(width));
            const auto y1 =
                std::clamp((clip.w - data->DisplayPos.y) * data->FramebufferScale.y, 0.0f, float(height));
            if (x1 <= x0 || y1 <= y0)
                continue;
            bgfx::setScissor(static_cast<std::uint16_t>(x0), static_cast<std::uint16_t>(y0),
                             static_cast<std::uint16_t>(std::ceil(x1 - x0)),
                             static_cast<std::uint16_t>(std::ceil(y1 - y0)));
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                           BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
            const bgfx::TextureHandle texture{static_cast<std::uint16_t>(command.GetTexID() - 1)};
            bgfx::setTexture(0, sampler_.get(), texture);
            bgfx::setVertexBuffer(0, &vb, command.VtxOffset, vertices - command.VtxOffset);
            bgfx::setIndexBuffer(&ib, command.IdxOffset, command.ElemCount);
            bgfx::submit(view, program_.get());
        }
    }
}
} // namespace fe
