#include "engine/render/CanvasRenderer.h"
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

std::uint32_t toAbgr(glm::vec4 c) {
    const auto byte = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (byte(c.a) << 24u) | (byte(c.b) << 16u) | (byte(c.g) << 8u) | byte(c.r);
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

void CanvasRenderer::flush(std::vector<Vertex>& vertices, bgfx::TextureHandle texture, const Rect& clip,
                           bool additive, const Canvas::Presentation& presentation, glm::ivec2 viewport) {
    if (vertices.empty()) {
        return;
    }
    const auto count = static_cast<std::uint32_t>(vertices.size());
    if (bgfx::getAvailTransientVertexBuffer(count, layout_) < count) {
        vertices.clear();
        return;
    }
    bgfx::TransientVertexBuffer vb;
    bgfx::allocTransientVertexBuffer(&vb, count, layout_);
    std::memcpy(vb.data, vertices.data(), count * sizeof(Vertex));
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
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   (additive
                        ? BGFX_STATE_BLEND_ADD
                        : BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)));
    bgfx::submit(view_, program_.get());
    vertices.clear();
}

void CanvasRenderer::fill(glm::vec4 rect, glm::vec4 color, bool additive, glm::ivec2 viewport) {
    if (color.a <= 0.001f) {
        return;
    }
    const auto abgr = toAbgr(color);
    std::vector<Vertex> vertices = {{rect.x, rect.y, 0, 0, abgr}, {rect.z, rect.y, 1, 0, abgr},
                                    {rect.z, rect.w, 1, 1, abgr}, {rect.x, rect.y, 0, 0, abgr},
                                    {rect.z, rect.w, 1, 1, abgr}, {rect.x, rect.w, 0, 1, abgr}};
    flush(vertices, gpu_.white(), Rect{0, 0, -1, -1}, additive, {}, viewport);
}

void CanvasRenderer::render(const Canvas& canvas, const ScreenFx& screen, glm::ivec2 viewport) {
    float projection[16];
    bx::mtxOrtho(projection, 0, static_cast<float>(viewport.x), static_cast<float>(viewport.y), 0, 0, 1000, 0,
                 bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewName(view_, "Game UI");
    bgfx::setViewMode(view_, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(view_, 0, 0, static_cast<std::uint16_t>(viewport.x),
                      static_cast<std::uint16_t>(viewport.y));
    bgfx::setViewTransform(view_, nullptr, projection);
    bgfx::touch(view_);
    const auto presentation = canvas.present(viewport);
    const auto& quads = canvas.quads();
    std::vector<std::size_t> order(quads.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return quads[a].layer < quads[b].layer; });
    std::vector<Vertex> batch;
    bgfx::TextureHandle current = BGFX_INVALID_HANDLE;
    Rect clip{0, 0, -1, -1};
    bool additive = false;
    for (const auto index : order) {
        const auto& q = quads[index];
        bgfx::TextureHandle texture = gpu_.white();
        if (q.texture.kind == CanvasTexture::Kind::Image) {
            texture = gpu_.image(q.texture.image, q.texture.linear);
        } else if (q.texture.kind == CanvasTexture::Kind::FontPage) {
            texture = gpu_.fontPage(q.texture.font, q.texture.page);
        }
        const bool sameClip =
            q.clip.x == clip.x && q.clip.y == clip.y && q.clip.w == clip.w && q.clip.h == clip.h;
        if (texture.idx != current.idx || !sameClip || q.additive != additive) {
            flush(batch, current, clip, additive, presentation, viewport);
            current = texture;
            clip = q.clip;
            additive = q.additive;
        }
        const auto abgr = toAbgr(q.color);
        Vertex corners[4];
        for (int i = 0; i < 4; ++i) {
            const auto p = presentation.offset + q.corners[static_cast<std::size_t>(i)] * presentation.scale;
            corners[i] = {p.x, p.y, q.uv[static_cast<std::size_t>(i)].x, q.uv[static_cast<std::size_t>(i)].y,
                          abgr};
        }
        for (const int i : {0, 1, 2, 0, 2, 3}) {
            batch.push_back(corners[i]);
        }
    }
    flush(batch, current, clip, additive, presentation, viewport);
    const auto w = static_cast<float>(viewport.x), h = static_cast<float>(viewport.y);
    if (screen.letterbox > 0) {
        const float bar = h * 0.12f * std::clamp(screen.letterbox, 0.0f, 1.0f);
        fill({0, 0, w, bar}, {0, 0, 0, 1}, false, viewport);
        fill({0, h - bar, w, h}, {0, 0, 0, 1}, false, viewport);
    }
    fill({0, 0, w, h}, {glm::vec3(screen.flash), screen.flash.a}, true, viewport);
    fill({0, 0, w, h}, {glm::vec3(screen.fade), screen.fade.a}, false, viewport);
}
} // namespace fe
