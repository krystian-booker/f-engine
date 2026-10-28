#pragma once
#include "engine/render/GpuAssets.h"
#include "engine/services/Canvas.h"
#include "engine/services/ScreenFx.h"

namespace fe {
// Draws the script-built 2D canvas and full-screen fades/flashes/letterbox.
class CanvasRenderer {
public:
    CanvasRenderer(GpuAssets& gpu, const std::filesystem::path& shaderRoot);
    void reload(const std::filesystem::path& shaderRoot);
    void render(const Canvas& canvas, const ScreenFx& screen, glm::ivec2 viewport);

private:
    struct Vertex {
        float x, y, u, v;
        std::uint32_t abgr;
    };
    void flush(std::vector<Vertex>& vertices, bgfx::TextureHandle texture, const Rect& clip, bool additive,
               const Canvas::Presentation& presentation, glm::ivec2 viewport);
    void fill(glm::vec4 rect, glm::vec4 color, bool additive, glm::ivec2 viewport);
    GpuAssets& gpu_;
    Program program_;
    Uniform sampler_;
    bgfx::VertexLayout layout_;
    std::uint16_t view_ = 200;
};
} // namespace fe
