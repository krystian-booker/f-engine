#include "engine/text/TextLayout.h"
#include <algorithm>
#include <stdexcept>

namespace fe {
namespace {
struct Measurer {
    const TextStyle& style;
    float scale;
    [[nodiscard]] float advance(const Glyph& glyph) const {
        if (!glyph.icon.empty()) {
            return style.size + 1;
        }
        if (!glyph.codepoint || glyph.newline()) {
            return 0;
        }
        const auto hit = style.font->find(glyph.codepoint);
        return hit.glyph ? hit.glyph->advance * hit.scale * scale : style.size * 0.5f;
    }
};

// Computes, for every unit, the line it lands on and its x offset.
struct LineBreaker {
    const std::vector<Glyph>& glyphs;
    const Measurer& measure;
    float width;
    std::vector<int> line;
    std::vector<float> x;
    std::vector<float> lineWidths{0};

    void run() {
        line.assign(glyphs.size(), 0);
        x.assign(glyphs.size(), 0);
        int current = 0;
        float pen = 0;
        auto breakLine = [&] {
            lineWidths.back() = std::max(lineWidths.back(), 0.0f);
            lineWidths.push_back(0);
            ++current;
            pen = 0;
        };
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            const auto& g = glyphs[i];
            if (g.newline()) {
                line[i] = current;
                breakLine();
                continue;
            }
            const bool wordStart = !g.space() && (i == 0 || glyphs[i - 1].space() || glyphs[i - 1].newline());
            if (width > 0 && wordStart && pen > 0) {
                float word = 0;
                for (auto j = i; j < glyphs.size() && !glyphs[j].space() && !glyphs[j].newline(); ++j) {
                    word += measure.advance(glyphs[j]);
                }
                if (word <= width && pen + word > width) {
                    breakLine();
                }
            }
            const float advance = measure.advance(g);
            if (width > 0 && pen > 0 && pen + advance > width && !g.space()) {
                breakLine();
            }
            if (g.space() && pen == 0 && current > 0) {
                line[i] = current; // swallow leading spaces of wrapped lines
                continue;
            }
            line[i] = current;
            x[i] = pen;
            pen += advance;
            if (!g.space()) {
                lineWidths.back() = std::max(lineWidths.back(), pen);
            }
        }
    }
};
} // namespace

TextLayout layoutText(const std::vector<Glyph>& glyphs, const TextStyle& style) {
    if (!style.font) {
        throw std::invalid_argument("Text layout needs a font");
    }
    const Measurer measure{style, style.size / style.font->lineHeight()};
    LineBreaker breaker{glyphs, measure, style.wrapWidth, {}, {}};
    breaker.run();
    TextLayout result;
    result.lineOfUnit = breaker.line;
    result.lines = static_cast<int>(breaker.lineWidths.size());
    for (const float w : breaker.lineWidths) {
        result.width = std::max(result.width, w);
    }
    const float lineHeight = style.size + style.lineGap;
    result.height = static_cast<float>(result.lines) * lineHeight - style.lineGap;
    const float box = style.wrapWidth > 0 ? style.wrapWidth : result.width;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto& g = glyphs[i];
        if ((!g.codepoint && g.icon.empty()) || g.newline() || g.space()) {
            continue;
        }
        const auto lineIndex = static_cast<std::size_t>(breaker.line[i]);
        float offset = 0;
        if (style.align == TextAlign::Center) {
            offset = (box - breaker.lineWidths[lineIndex]) * 0.5f;
        } else if (style.align == TextAlign::Right) {
            offset = box - breaker.lineWidths[lineIndex];
        }
        PlacedGlyph placed;
        placed.unit = i;
        placed.color = g.color;
        const float top = static_cast<float>(lineIndex) * lineHeight;
        if (!g.icon.empty()) {
            placed.icon = g.icon;
            placed.x = breaker.x[i] + offset;
            placed.y = top;
            placed.width = placed.height = style.size;
            result.glyphs.push_back(std::move(placed));
            continue;
        }
        const auto hit = style.font->find(g.codepoint);
        if (!hit.glyph) {
            continue;
        }
        const float scale = hit.scale * measure.scale;
        placed.font = hit.font;
        placed.source = hit.glyph;
        placed.x = breaker.x[i] + offset + hit.glyph->offsetX * scale;
        placed.y = top + hit.glyph->offsetY * scale;
        placed.width = hit.glyph->width * scale;
        placed.height = hit.glyph->height * scale;
        result.glyphs.push_back(std::move(placed));
    }
    return result;
}

std::vector<std::vector<Glyph>> paginateText(const std::vector<Glyph>& glyphs, const TextStyle& style,
                                             int linesPerPage) {
    linesPerPage = std::max(1, linesPerPage);
    const auto layout = layoutText(glyphs, style);
    std::vector<std::vector<Glyph>> pages(1);
    int pageOf = 0;
    int previousLine = 0;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const int line = layout.lineOfUnit[i];
        const int page = line / linesPerPage;
        if (page != pageOf) {
            pages.resize(static_cast<std::size_t>(page) + 1);
            pageOf = page;
            previousLine = line;
        }
        auto& current = pages.back();
        if (current.empty() && (glyphs[i].newline() || glyphs[i].space())) {
            continue;
        }
        if (line != previousLine && !glyphs[i].newline() && !current.empty() && !current.back().newline()) {
            Glyph wrap = glyphs[i];
            wrap.codepoint = U'\n';
            wrap.icon.clear();
            wrap.wait = wrap.speed = 0;
            current.push_back(wrap);
        }
        previousLine = line;
        if (glyphs[i].newline() && layout.lineOfUnit.size() > i + 1 &&
            layout.lineOfUnit[i + 1] / linesPerPage != page) {
            continue; // the page break replaces this newline
        }
        current.push_back(glyphs[i]);
    }
    std::erase_if(pages, [](const auto& page) { return page.empty(); });
    if (pages.empty()) {
        pages.emplace_back();
    }
    return pages;
}
} // namespace fe
