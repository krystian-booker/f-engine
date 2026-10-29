#include "engine/services/Canvas.h"
#include <algorithm>
#include <cmath>

namespace fe {
std::uint32_t multiplyAlpha(std::uint32_t rgba, float alpha) {
    const auto a =
        static_cast<std::uint32_t>(std::clamp(static_cast<float>(rgba & 0xffu) * alpha, 0.0f, 255.0f));
    return (rgba & 0xffffff00u) | a;
}

Canvas::Canvas(TextSystem& text, int width, int height) : text_(text), width_(width), height_(height) {
    begin();
}

void Canvas::begin() {
    quads_.clear();
    clips_.clear();
    transforms_.assign(1, {glm::mat3(1), 1.0f});
    layer_ = 0;
    additive_ = false;
}

void Canvas::resize(int width, int height) {
    width_ = std::max(16, width);
    height_ = std::max(16, height);
}

void Canvas::quad(const CanvasTexture& texture, Rect area, glm::vec4 uv, std::uint32_t color) {
    const auto& [matrix, alpha] = transforms_.back();
    CanvasQuad q;
    q.texture = texture;
    const glm::vec2 points[4] = {{area.x, area.y},
                                 {area.x + area.w, area.y},
                                 {area.x + area.w, area.y + area.h},
                                 {area.x, area.y + area.h}};
    for (int i = 0; i < 4; ++i) {
        q.corners[static_cast<std::size_t>(i)] = glm::vec2(matrix * glm::vec3(points[i], 1));
    }
    q.uv = {glm::vec2(uv.x, uv.y), glm::vec2(uv.z, uv.y), glm::vec2(uv.z, uv.w), glm::vec2(uv.x, uv.w)};
    q.color = multiplyAlpha(color, alpha);
    if (!clips_.empty()) {
        q.clip = clips_.back();
    }
    q.layer = layer_;
    q.additive = additive_;
    if ((q.color & 0xffu) != 0) {
        quads_.push_back(std::move(q));
    }
}

void Canvas::rect(Rect area, std::uint32_t color) {
    quad({}, area, {0, 0, 1, 1}, color);
}

void Canvas::line(glm::vec2 from, glm::vec2 to, std::uint32_t color, float thickness) {
    const auto delta = to - from;
    const float length = glm::length(delta);
    if (length < 1e-4f) {
        return;
    }
    pushTransform({from, glm::vec2(1), glm::vec2(0), std::atan2(delta.y, delta.x), 1});
    rect({0, -thickness * 0.5f, length, thickness}, color);
    popTransform();
}

glm::ivec2 Canvas::imageSize(const std::string& path) {
    if (path.starts_with(TargetPrefix)) {
        const auto found = targets_.find(std::string_view(path).substr(TargetPrefix.size()));
        return found == targets_.end() ? glm::ivec2(0) : found->second;
    }
    const auto image = text_.assets().tryImage(path);
    return image ? glm::ivec2(image->width, image->height) : glm::ivec2(0);
}

void Canvas::image(const std::string& path, Rect area, const ImageOptions& options) {
    const auto size = imageSize(path);
    if (size.x == 0) {
        rect(area, 0xff00ffff);
        return;
    }
    Rect source = options.source;
    if (source.w <= 0 || source.h <= 0) {
        source = {0, 0, static_cast<float>(size.x), static_cast<float>(size.y)};
    }
    if (area.w <= 0 || area.h <= 0) {
        area.w = source.w;
        area.h = source.h;
    }
    glm::vec4 uv(source.x / static_cast<float>(size.x), source.y / static_cast<float>(size.y),
                 (source.x + source.w) / static_cast<float>(size.x),
                 (source.y + source.h) / static_cast<float>(size.y));
    if (options.flipX) {
        std::swap(uv.x, uv.z);
    }
    if (options.flipY) {
        std::swap(uv.y, uv.w);
    }
    quad({CanvasTexture::Kind::Image, path, nullptr, 0, options.linear}, area, uv, options.color);
}

void Canvas::nineSlice(const std::string& path, Rect area, glm::vec4 border, const ImageOptions& options) {
    const auto size = imageSize(path);
    if (size.x == 0) {
        rect(area, 0xff00ffff);
        return;
    }
    Rect source = options.source;
    if (source.w <= 0 || source.h <= 0) {
        source = {0, 0, static_cast<float>(size.x), static_cast<float>(size.y)};
    }
    // border = left, top, right, bottom (source pixels; drawn 1:1)
    const float left = std::min(border.x, area.w * 0.5f), right = std::min(border.z, area.w * 0.5f);
    const float top = std::min(border.y, area.h * 0.5f), bottom = std::min(border.w, area.h * 0.5f);
    const float xs[4] = {area.x, area.x + left, area.x + area.w - right, area.x + area.w};
    const float ys[4] = {area.y, area.y + top, area.y + area.h - bottom, area.y + area.h};
    const float us[4] = {source.x, source.x + border.x, source.x + source.w - border.z, source.x + source.w};
    const float vs[4] = {source.y, source.y + border.y, source.y + source.h - border.w, source.y + source.h};
    const CanvasTexture texture{CanvasTexture::Kind::Image, path, nullptr, 0, options.linear};
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            const Rect cell{xs[x], ys[y], xs[x + 1] - xs[x], ys[y + 1] - ys[y]};
            if (cell.w <= 0 || cell.h <= 0) {
                continue;
            }
            quad(texture, cell,
                 {us[x] / static_cast<float>(size.x), vs[y] / static_cast<float>(size.y),
                  us[x + 1] / static_cast<float>(size.x), vs[y + 1] / static_cast<float>(size.y)},
                 options.color);
        }
    }
}

TextLayout Canvas::measure(const std::string& markup, const TextOptions& options) {
    return text_.layout(markup, options);
}

void Canvas::glyphs(const TextLayout& layout, glm::vec2 origin, const TextOptions& options,
                    std::uint32_t tint, glm::vec2 offset) {
    for (const auto& glyph : layout.glyphs) {
        if (glyph.unit >= options.visible) {
            continue;
        }
        const auto color = tint ? tint : glyph.color;
        const Rect area{std::round(origin.x + glyph.x + offset.x), std::round(origin.y + glyph.y + offset.y),
                        glyph.width, glyph.height};
        if (!glyph.icon.empty()) {
            const auto* icon = text_.icon(glyph.icon);
            if (!icon) {
                rect(area, multiplyAlpha(0xffd880ff, static_cast<float>(color & 0xffu) / 255.0f));
                continue;
            }
            ImageOptions image;
            image.source = {icon->rect.x, icon->rect.y, icon->rect.z, icon->rect.w};
            image.color = tint ? tint : 0xffffffff;
            this->image(icon->image, area, image);
            continue;
        }
        const auto& page = glyph.font->page(static_cast<std::size_t>(glyph.source->page));
        const glm::vec4 uv(glyph.source->x / static_cast<float>(page.width),
                           glyph.source->y / static_cast<float>(page.height),
                           (glyph.source->x + glyph.source->width) / static_cast<float>(page.width),
                           (glyph.source->y + glyph.source->height) / static_cast<float>(page.height));
        const CanvasTexture texture{
            CanvasTexture::Kind::FontPage, {}, glyph.font, glyph.source->page, !glyph.font->pixel()};
        const auto first = quads_.size();
        for (int copy = 0; copy < (glyph.syntheticBold ? 2 : 1); ++copy) {
            quad(texture, {area.x + static_cast<float>(copy), area.y, area.w, area.h}, uv, color);
        }
        for (auto i = first; i < quads_.size(); ++i) {
            quads_[i].effect = static_cast<std::uint8_t>(glyph.effect);
            quads_[i].phase = glyph.phase;
        }
    }
}

glm::vec2 Canvas::text(const std::string& markup, glm::vec2 position, const TextOptions& options) {
    const auto layout = text_.layout(markup, options);
    if (options.outline) {
        for (const glm::vec2 offset :
             {glm::vec2(-1, 0), glm::vec2(1, 0), glm::vec2(0, -1), glm::vec2(0, 1)}) {
            glyphs(layout, position, options, options.outline, offset);
        }
    }
    if (options.shadow) {
        glyphs(layout, position, options, options.shadow, {1, 1});
    }
    glyphs(layout, position, options, 0, {0, 0});
    return {options.width > 0 ? options.width : layout.width, layout.height};
}

Canvas::Presentation Canvas::present(glm::ivec2 viewport) const {
    const float fit = std::min(static_cast<float>(viewport.x) / static_cast<float>(width_),
                               static_cast<float>(viewport.y) / static_cast<float>(height_));
    Presentation p;
    p.scale = fit >= 1 ? std::max(1.0f, std::floor(fit)) : fit;
    p.offset = {(static_cast<float>(viewport.x) - static_cast<float>(width_) * p.scale) * 0.5f,
                (static_cast<float>(viewport.y) - static_cast<float>(height_) * p.scale) * 0.5f};
    return p;
}

glm::vec2 Canvas::toWindow(glm::vec2 canvas, glm::ivec2 viewport) const {
    const auto p = present(viewport);
    return p.offset + canvas * p.scale;
}

glm::vec2 Canvas::fromWindow(glm::vec2 window, glm::ivec2 viewport) const {
    const auto p = present(viewport);
    return (window - p.offset) / p.scale;
}

void Canvas::pushClip(Rect area) {
    const auto& matrix = transforms_.back().first;
    const auto a = glm::vec2(matrix * glm::vec3(area.x, area.y, 1));
    const auto b = glm::vec2(matrix * glm::vec3(area.x + area.w, area.y + area.h, 1));
    Rect clip{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
    if (!clips_.empty()) {
        const auto& outer = clips_.back();
        const float x0 = std::max(clip.x, outer.x), y0 = std::max(clip.y, outer.y);
        const float x1 = std::min(clip.x + clip.w, outer.x + outer.w),
                    y1 = std::min(clip.y + clip.h, outer.y + outer.h);
        clip = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    clips_.push_back(clip);
}

void Canvas::popClip() {
    if (!clips_.empty()) {
        clips_.pop_back();
    }
}

void Canvas::pushTransform(const Transform2D& t) {
    const float c = std::cos(t.rotation), s = std::sin(t.rotation);
    // translate * (origin) * rotate * scale * (-origin)
    glm::mat3 local(1, 0, 0, 0, 1, 0, -t.origin.x, -t.origin.y, 1);
    local = glm::mat3(t.scale.x, 0, 0, 0, t.scale.y, 0, 0, 0, 1) * local;
    local = glm::mat3(c, s, 0, -s, c, 0, 0, 0, 1) * local;
    local = glm::mat3(1, 0, 0, 0, 1, 0, t.origin.x + t.translate.x, t.origin.y + t.translate.y, 1) * local;
    const auto& [parent, alpha] = transforms_.back();
    transforms_.emplace_back(parent * local, alpha * std::clamp(t.alpha, 0.0f, 1.0f));
}

void Canvas::popTransform() {
    if (transforms_.size() > 1) {
        transforms_.pop_back();
    }
}
} // namespace fe
