#include "engine/text/TextLayout.h"
#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace fe {
namespace {
constexpr float RubyScale = 0.5f;

bool isCjk(char32_t c) {
    return (c >= 0x2e80 && c <= 0x2fdf) || (c >= 0x3000 && c <= 0x30ff) || (c >= 0x3100 && c <= 0x31ff) ||
           (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xffef) || (c >= 0x20000 && c <= 0x3ffff);
}

// Characters a line may not start with: closing brackets and punctuation,
// small kana, the prolonged sound mark and iteration marks.
bool noLineStart(char32_t c) {
    static constexpr std::u32string_view Closers = U")]}>.,!?:;%'\"»’”…‥、。々〉》」』】〕"
                                                   U"〗〙〛〟〻゠・ーヽヾゝゞ！），．"
                                                   U"：；？］｝｠｡｣､･ｰ";
    static constexpr std::u32string_view Small = U"ぁぃぅぇぉっゃゅょゎゕゖ"
                                                 U"ァィゥェォッャュョヮヵヶ"
                                                 U"ｧｨｩｪｫｬｭｮｯ";
    return Closers.find(c) != std::u32string_view::npos || Small.find(c) != std::u32string_view::npos ||
           (c >= 0x31f0 && c <= 0x31ff);
}

// Characters a line may not end with: opening brackets and quotes.
bool noLineEnd(char32_t c) {
    static constexpr std::u32string_view Openers = U"([{<«‘“〈《「『【〔〖〘〚〝（［"
                                                   U"｛｟｢";
    return Openers.find(c) != std::u32string_view::npos;
}

struct Measurer {
    const TextStyle& style;
    [[nodiscard]] const FontSet& face(const Glyph& glyph) const {
        return glyph.bold && style.bold ? *style.bold : *style.font;
    }
    // Converts a face's pixels to canvas pixels for this glyph.
    [[nodiscard]] float scale(const Glyph& glyph) const {
        return style.size * glyph.scale / face(glyph).lineHeight();
    }
    [[nodiscard]] bool syntheticBold(const Glyph& glyph) const { return glyph.bold && !style.bold; }
    [[nodiscard]] float advance(const Glyph& glyph) const {
        if (!glyph.icon.empty()) {
            return style.size * glyph.scale + 1;
        }
        if (!glyph.codepoint || glyph.newline()) {
            return 0;
        }
        const auto hit = face(glyph).find(glyph.codepoint);
        const float bold = syntheticBold(glyph) && !glyph.space() ? 1.0f : 0.0f;
        return hit.glyph ? hit.glyph->advance * hit.scale * scale(glyph) + bold
                         : style.size * glyph.scale * 0.5f;
    }
    // Pair kerning between two characters drawn from the same face and size.
    [[nodiscard]] float kerning(const Glyph* previous, const Glyph& glyph) const {
        if (!previous || !previous->codepoint || !glyph.icon.empty() || !glyph.codepoint || glyph.newline() ||
            previous->scale != glyph.scale || previous->bold != glyph.bold) {
            return 0;
        }
        const auto& set = face(glyph);
        const auto a = set.find(previous->codepoint);
        const auto b = set.find(glyph.codepoint);
        if (!a.glyph || !b.glyph || a.font != b.font) {
            return 0;
        }
        return a.font->kerning(previous->codepoint, glyph.codepoint) * b.scale * scale(glyph);
    }
};

// The characters of a ruby reading, styled after its first base unit.
std::vector<Glyph> readingOf(const Glyph& base) {
    std::vector<Glyph> reading;
    for (const auto c : decodeUtf8(base.ruby)) {
        Glyph r;
        r.codepoint = c;
        r.color = base.color;
        r.scale = base.scale * RubyScale;
        r.bold = base.bold;
        r.effect = base.effect;
        reading.push_back(r);
    }
    return reading;
}

// Pen positions of a run of glyphs set one after another (with kerning),
// and its total advance.
float setRun(const std::vector<Glyph>& run, const Measurer& measure, std::vector<float>* pens = nullptr) {
    float pen = 0;
    for (std::size_t j = 0; j < run.size(); ++j) {
        pen += measure.kerning(j ? &run[j - 1] : nullptr, run[j]);
        if (pens) {
            pens->push_back(pen);
        }
        pen += measure.advance(run[j]);
    }
    return pen;
}

// Space added on each side of every base unit whose ruby reading is wider
// than the base: the excess is spread 1:2:1 (half a share before the first
// unit, a share between units, half after the last), so the base is
// respaced and centred under its reading instead of the reading overhanging.
std::vector<float> rubyPadding(const std::vector<Glyph>& glyphs, const Measurer& measure) {
    std::vector<float> padding(glyphs.size(), 0);
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        if (glyphs[i].rubySpan == 0) {
            continue;
        }
        const auto end = std::min(glyphs.size(), i + glyphs[i].rubySpan);
        const std::vector<Glyph> base(glyphs.begin() + static_cast<std::ptrdiff_t>(i),
                                      glyphs.begin() + static_cast<std::ptrdiff_t>(end));
        const float excess = setRun(readingOf(glyphs[i]), measure) - setRun(base, measure);
        if (excess > 0) {
            const float share = excess / static_cast<float>(2 * (end - i));
            std::fill(padding.begin() + static_cast<std::ptrdiff_t>(i),
                      padding.begin() + static_cast<std::ptrdiff_t>(end), share);
        }
    }
    return padding;
}

// Whether a line may break just before unit `i`.
bool breakBefore(const std::vector<Glyph>& glyphs, std::size_t i) {
    const auto& glyph = glyphs[i];
    if (glyph.space() || glyph.newline() || glyph.rubyContinues || glyph.timing()) {
        return false;
    }
    std::size_t p = i;
    while (p > 0 && glyphs[p - 1].timing()) {
        --p;
    }
    if (p == 0) {
        return false;
    }
    const auto& previous = glyphs[p - 1];
    if (previous.space()) {
        return true;
    }
    if (previous.newline()) {
        return false;
    }
    const bool ideographic =
        !previous.icon.empty() || !glyph.icon.empty() || isCjk(previous.codepoint) || isCjk(glyph.codepoint);
    return ideographic && !noLineStart(glyph.codepoint) && !noLineEnd(previous.codepoint);
}

// Computes, for every unit, the line it lands on and its x offset.
struct LineBreaker {
    const std::vector<Glyph>& glyphs;
    const Measurer& measure;
    float width;
    std::vector<float> padding; // added on both sides of a unit (ruby bases under wide readings)
    std::vector<int> line{};
    std::vector<float> x{};
    std::vector<float> widthBefore{}; // the line's width before each unit
    std::vector<float> lineWidths{0};
    std::vector<bool> wrapped{false};

    void run() {
        constexpr auto None = static_cast<std::size_t>(-1);
        line.assign(glyphs.size(), 0);
        x.assign(glyphs.size(), 0);
        widthBefore.assign(glyphs.size(), 0);
        int current = 0;
        float pen = 0;
        const Glyph* previous = nullptr;
        std::size_t lineStart = 0, breakAt = None;
        float widthAtBreak = 0;
        auto newLine = [&](bool wrap, std::size_t start) {
            lineWidths.push_back(0);
            wrapped.push_back(wrap);
            ++current;
            pen = 0;
            previous = nullptr;
            lineStart = start;
            breakAt = None;
        };
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            const auto& g = glyphs[i];
            if (g.newline()) {
                line[i] = current;
                newLine(false, i + 1);
                continue;
            }
            if (width > 0 && i > lineStart && breakBefore(glyphs, i)) {
                breakAt = i;
                widthAtBreak = lineWidths.back();
            }
            const float advance = measure.advance(g) + 2 * padding[i];
            float kern = measure.kerning(previous, g);
            if (width > 0 && pen > 0 && !g.space() && pen + kern + advance > width) {
                if (g.rubyContinues) {
                    // Keep a ruby base together when it fits on a line of its own.
                    auto start = i;
                    while (start > lineStart && glyphs[start].rubyContinues) {
                        --start;
                    }
                    if (!glyphs[start].rubyContinues && start > lineStart) {
                        breakAt = start;
                        widthAtBreak = widthBefore[start];
                    }
                }
                if (breakAt != None) {
                    // Move everything from the last opportunity onto the next line.
                    lineWidths.back() = widthAtBreak;
                    const auto restart = breakAt;
                    newLine(true, restart);
                    i = restart - 1;
                    continue;
                }
                newLine(true, i);
                kern = 0;
            }
            line[i] = current;
            widthBefore[i] = lineWidths.back();
            pen += kern;
            x[i] = pen + padding[i];
            pen += advance;
            if (!g.space()) {
                lineWidths.back() = std::max(lineWidths.back(), pen);
            }
            if (g.codepoint) {
                previous = &g;
            } else if (!g.icon.empty()) {
                previous = nullptr;
            }
        }
    }
};

struct Line {
    float top = 0, ruby = 0, scale = 1, baseline = 0;
};
} // namespace

TextLayout layoutText(const std::vector<Glyph>& glyphs, const TextStyle& style) {
    if (!style.font) {
        throw std::invalid_argument("Text layout needs a font");
    }
    const Measurer measure{style};
    LineBreaker breaker{glyphs, measure, style.wrapWidth, rubyPadding(glyphs, measure)};
    breaker.run();
    TextLayout result;
    result.lineOfUnit = breaker.line;
    result.wrappedLine = breaker.wrapped;
    result.lines = static_cast<int>(breaker.lineWidths.size());
    for (const float w : breaker.lineWidths) {
        result.width = std::max(result.width, w);
    }
    // Line heights: the largest glyph on the line plus room for ruby above it.
    std::vector<Line> lines(static_cast<std::size_t>(result.lines), Line{0, 0, 0, 0});
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto& g = glyphs[i];
        if (g.timing() || g.newline()) {
            continue;
        }
        auto& l = lines[static_cast<std::size_t>(breaker.line[i])];
        l.scale = std::max(l.scale, g.scale);
        if (g.rubySpan > 0) {
            l.ruby = std::max(l.ruby, style.size * g.scale * RubyScale);
        }
    }
    const float primary = style.size / style.font->lineHeight();
    float top = 0;
    for (auto& l : lines) {
        if (l.scale == 0) {
            l.scale = 1; // an empty line is as tall as plain text
        }
        l.top = top;
        l.baseline = top + l.ruby + style.font->baseline() * primary * l.scale;
        top += l.ruby + style.size * l.scale + style.lineGap;
    }
    result.height = lines.empty() ? 0 : top - style.lineGap;
    const float box = style.wrapWidth > 0 ? style.wrapWidth : result.width;
    auto offsetOf = [&](std::size_t lineIndex) {
        if (style.align == TextAlign::Center) {
            return (box - breaker.lineWidths[lineIndex]) * 0.5f;
        }
        if (style.align == TextAlign::Right) {
            return box - breaker.lineWidths[lineIndex];
        }
        return 0.0f;
    };
    // Places one character whose top-of-line (for its size) is `lineTop`.
    auto place = [&](const Glyph& g, std::size_t unit, float x, float lineTop) {
        const auto& set = measure.face(g);
        const auto hit = set.find(g.codepoint);
        if (!hit.glyph) {
            return;
        }
        const float faceScale = measure.scale(g);
        PlacedGlyph placed;
        placed.unit = unit;
        placed.color = g.color;
        placed.font = hit.font;
        placed.source = hit.glyph;
        placed.x = x + hit.glyph->offsetX * hit.scale * faceScale;
        placed.y = lineTop + (style.font->baseline() * primary * g.scale - set.baseline() * faceScale) +
                   (hit.baseline + hit.glyph->offsetY * hit.scale) * faceScale;
        placed.width = hit.glyph->width * hit.scale * faceScale;
        placed.height = hit.glyph->height * hit.scale * faceScale;
        placed.syntheticBold = measure.syntheticBold(g);
        placed.effect = g.effect;
        placed.phase = static_cast<float>(unit);
        result.glyphs.push_back(std::move(placed));
    };
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto& g = glyphs[i];
        if (g.timing() || g.newline() || g.space()) {
            continue;
        }
        const auto lineIndex = static_cast<std::size_t>(breaker.line[i]);
        const auto& l = lines[lineIndex];
        const float offset = offsetOf(lineIndex);
        const float glyphTop = l.baseline - style.font->baseline() * primary * g.scale;
        if (!g.icon.empty()) {
            PlacedGlyph placed;
            placed.unit = i;
            placed.color = g.color;
            placed.icon = g.icon;
            placed.x = breaker.x[i] + offset;
            placed.y = glyphTop;
            placed.width = placed.height = style.size * g.scale;
            placed.effect = g.effect;
            placed.phase = static_cast<float>(i);
            result.glyphs.push_back(std::move(placed));
        } else {
            place(g, i, breaker.x[i] + offset, glyphTop);
        }
        if (g.rubySpan == 0) {
            continue;
        }
        // The reading, centred over its base and revealed along with it.
        const auto last = std::min(glyphs.size(), i + g.rubySpan) - 1;
        const float left = breaker.x[i] - breaker.padding[i];
        const float right = breaker.x[last] + measure.advance(glyphs[last]) + breaker.padding[last];
        const auto reading = readingOf(g);
        std::vector<float> pens;
        const float width = setRun(reading, measure, &pens);
        const float start = (left + right - width) * 0.5f + offset;
        const float rubyTop = glyphTop - style.size * g.scale * RubyScale;
        for (std::size_t j = 0; j < reading.size(); ++j) {
            const auto unit = i + j * (last - i + 1) / reading.size();
            place(reading[j], unit, start + pens[j], rubyTop);
        }
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
    bool lineStart = true;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const int line = layout.lineOfUnit[i];
        const int page = line / linesPerPage;
        if (line != previousLine) {
            lineStart = true;
        }
        if (page != pageOf) {
            pages.resize(static_cast<std::size_t>(page) + 1);
            pageOf = page;
            previousLine = line;
        }
        auto& current = pages.back();
        const auto& g = glyphs[i];
        if (g.space() && lineStart && layout.wrappedLine[static_cast<std::size_t>(line)]) {
            continue; // the layout dropped this space at the start of a wrapped line
        }
        if (current.empty() && g.newline()) {
            continue;
        }
        if (line != previousLine && !g.newline() && !current.empty() && !current.back().newline()) {
            Glyph wrap;
            wrap.codepoint = U'\n';
            wrap.color = g.color;
            wrap.scale = g.scale;
            wrap.bold = g.bold;
            wrap.effect = g.effect;
            current.push_back(wrap);
        }
        previousLine = line;
        if (!g.space() && !g.timing()) {
            lineStart = false;
        }
        if (g.newline() && layout.lineOfUnit.size() > i + 1 &&
            layout.lineOfUnit[i + 1] / linesPerPage != page) {
            continue; // the page break replaces this newline
        }
        current.push_back(g);
    }
    std::erase_if(pages, [](const auto& page) { return page.empty(); });
    if (pages.empty()) {
        pages.emplace_back();
    }
    return pages;
}
} // namespace fe
