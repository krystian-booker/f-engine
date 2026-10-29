#pragma once
#include "engine/render/GpuAssets.h"
#include "engine/services/Canvas.h"
#include <functional>

namespace fe {
// Draws the script-built 2D canvas over the composed worlds. The canvas is
// composed in sRGB space: images, fonts and colours keep their stored values.
// "rt:<name>" images show render targets worlds drew this frame; worlds
// leave premultiplied colour there, so those quads blend premultiplied (their
// colour premultiplied too). Glyph quads with an effect (shake, wave) move by
// their offset at `time`.
class CanvasRenderer {
public:
    // A render target's colour, or an invalid handle when none was drawn this frame.
    using TargetLookup = std::function<bgfx::TextureHandle(std::string_view name)>;
    CanvasRenderer(GpuAssets& gpu, const std::filesystem::path& shaderRoot);
    void reload(const std::filesystem::path& shaderRoot);
    void render(const Canvas& canvas, glm::ivec2 viewport, std::uint16_t view, bgfx::FrameBufferHandle target,
                double time, const TargetLookup& targets, std::uint32_t& calls);

private:
    struct Vertex {
        float x, y, u, v;
        std::uint32_t abgr;
    };
    void flush(bgfx::TextureHandle texture, const Rect& clip, bool additive, bool premultiplied,
               const Canvas::Presentation& presentation, glm::ivec2 viewport, std::uint32_t& calls);
    GpuAssets& gpu_;
    Program program_;
    Uniform sampler_;
    bgfx::VertexLayout layout_;
    std::uint16_t view_ = 0;
    std::vector<Vertex> batch_;
    std::vector<std::size_t> order_;
};
} // namespace fe
