#include "engine/render/CanvasRenderer.h"
#include "engine/render/FrameMath.h"
#include <algorithm>
#include <bx/math.h>
#include <cstring>
#include <numeric>

namespace fe {
namespace {
std::uint32_t toAbgr(std::uint32_t rgba) {
    return ((rgba & 0xffu) << 24u) | (((rgba >> 8u) & 0xffu) << 16u) | (((rgba >> 16u) & 0xffu) << 8u) |
           (rgba >> 24u);
}
} // namespace

CanvasRenderer::CanvasRenderer(GpuAssets& gpu, const std::filesystem::path& shaderRoot) : gpu_(gpu) {
    layout_.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    sampler_ = Uniform(bgfx::createUniform("s_texture", bgfx::UniformType::Sampler));
    reload(shaderRoot);
}

void CanvasRenderer::reload(const std::filesystem::path& shaderRoot) {
    program_ = Program(loadProgram(shaderRoot, "vs_ui", "fs_ui"));
}

void CanvasRenderer::flush(bgfx::TextureHandle texture, const Rect& clip, bool additive, bool premultiplied,
                           const Canvas::Presentation& presentation, glm::ivec2 viewport,
                           std::uint32_t& calls) {
    if (batch_.empty()) {
        return;
    }
    const auto count = static_cast<std::uint32_t>(batch_.size());
    if (bgfx::getAvailTransientVertexBuffer(count, layout_) < count) {
        batch_.clear();
        return;
    }
    bgfx::TransientVertexBuffer vb;
    bgfx::allocTransientVertexBuffer(&vb, count, layout_);
    std::memcpy(vb.data, batch_.data(), count * sizeof(Vertex));
    bgfx::setVertexBuffer(0, &vb);
    if (clip.w >= 0) {
        const float x0 = std::max(0.0f, presentation.offset.x + clip.x * presentation.scale);
        const float y0 = std::max(0.0f, presentation.offset.y + clip.y * presentation.scale);
        const float x1 = std::min(static_cast<float>(viewport.x),
                                  presentation.offset.x + (clip.x + clip.w) * presentation.scale);
        const float y1 = std::min(static_cast<float>(viewport.y),
                                  presentation.offset.y + (clip.y + clip.h) * presentation.scale);
        bgfx::setScissor(static_cast<std::uint16_t>(x0), static_cast<std::uint16_t>(y0),
                         static_cast<std::uint16_t>(std::max(0.0f, x1 - x0)),
                         static_cast<std::uint16_t>(std::max(0.0f, y1 - y0)));
    }
    bgfx::setTexture(0, sampler_.get(), texture);
    const std::uint64_t blend =
        additive        ? BGFX_STATE_BLEND_ADD
        : premultiplied ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA)
                        : BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | blend);
    bgfx::submit(view_, program_.get());
    ++calls;
    batch_.clear();
}

void CanvasRenderer::render(const Canvas& canvas, glm::ivec2 viewport, std::uint16_t view,
                            bgfx::FrameBufferHandle target, double time, const TargetLookup& targets,
                            std::uint32_t& calls) {
    view_ = view;
    float projection[16];
    bx::mtxOrtho(projection, 0, static_cast<float>(viewport.x), static_cast<float>(viewport.y), 0, 0, 1000, 0,
                 bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewName(view_, "Game UI");
    bgfx::setViewMode(view_, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(view_, target);
    bgfx::setViewRect(view_, 0, 0, static_cast<std::uint16_t>(viewport.x),
                      static_cast<std::uint16_t>(viewport.y));
    bgfx::setViewTransform(view_, nullptr, projection);
    bgfx::touch(view_);
    const auto presentation = canvas.present(viewport);
    const auto& quads = canvas.quads();
    order_.resize(quads.size());
    std::iota(order_.begin(), order_.end(), 0);
    std::stable_sort(order_.begin(), order_.end(),
                     [&](std::size_t a, std::size_t b) { return quads[a].layer < quads[b].layer; });
    bgfx::TextureHandle current = BGFX_INVALID_HANDLE;
    Rect clip{0, 0, -1, -1};
    bool additive = false, premultiplied = false;
    for (const auto index : order_) {
        const auto& q = quads[index];
        bgfx::TextureHandle texture = gpu_.white();
        bool flipV = false, target = false;
        if (q.texture.kind == CanvasTexture::Kind::Image &&
            q.texture.image.starts_with(Canvas::TargetPrefix)) {
            texture = targets(std::string_view(q.texture.image).substr(Canvas::TargetPrefix.size()));
            if (!bgfx::isValid(texture)) {
                continue;
            }
            flipV = bgfx::getCaps()->originBottomLeft;
            target = true;
        } else if (q.texture.kind == CanvasTexture::Kind::Image) {
            texture = gpu_.image(q.texture.image, q.texture.linear ? TextureUse::UiSmooth : TextureUse::Ui);
        } else if (q.texture.kind == CanvasTexture::Kind::FontPage) {
            texture = gpu_.fontPage(q.texture.font, q.texture.page);
        }
        const bool sameClip =
            q.clip.x == clip.x && q.clip.y == clip.y && q.clip.w == clip.w && q.clip.h == clip.h;
        if (texture.idx != current.idx || !sameClip || q.additive != additive || target != premultiplied) {
            flush(current, clip, additive, premultiplied, presentation, viewport, calls);
            current = texture;
            clip = q.clip;
            additive = q.additive;
            premultiplied = target;
        }
        const auto abgr = toAbgr(target ? premultiplyRgba(q.color) : q.color);
        const auto shift = glyphEffectOffset(q.effect, q.phase, time);
        Vertex corners[4];
        for (std::size_t i = 0; i < 4; ++i) {
            const auto p = presentation.offset + (q.corners[i] + shift) * presentation.scale;
            corners[i] = {p.x, p.y, q.uv[i].x, flipV ? 1.0f - q.uv[i].y : q.uv[i].y, abgr};
        }
        for (const int i : {0, 1, 2, 0, 2, 3}) {
            batch_.push_back(corners[i]);
        }
    }
    flush(current, clip, additive, premultiplied, presentation, viewport, calls);
}
} // namespace fe
