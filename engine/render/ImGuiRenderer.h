#pragma once
#include <bgfx/bgfx.h>
#include <filesystem>
struct ImDrawData;

namespace fe {
class ImGuiRenderer {
public:
    explicit ImGuiRenderer(const std::filesystem::path& shaderRoot);
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    void render(ImDrawData* data, std::uint16_t view);
    void reload(const std::filesystem::path& shaderRoot);

private:
    bgfx::VertexLayout layout_;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle font_ = BGFX_INVALID_HANDLE;
};
} // namespace fe
