#pragma once
#include "engine/render/GpuAssets.h"
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
    Program program_;
    Uniform sampler_;
    Texture font_;
};
} // namespace fe
