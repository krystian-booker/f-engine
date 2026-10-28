#pragma once
#include "engine/text/Font.h"
#include "engine/text/Markup.h"
#include <string>
#include <vector>

namespace fe {
enum class TextAlign { Left, Center, Right };

struct TextStyle {
    const FontSet* font = nullptr;
    float size = 8;      // line height of the font, in canvas pixels
    float lineGap = 3;   // extra space between lines
    float wrapWidth = 0; // 0 disables wrapping
    TextAlign align = TextAlign::Left;
};

struct PlacedGlyph {
    std::size_t unit = 0; // index into the parsed glyph sequence
    float x = 0, y = 0, width = 0, height = 0;
    Font* font = nullptr;
    const FontGlyph* source = nullptr;
    std::uint32_t color = 0xffffffff;
    std::string icon;
};

struct TextLayout {
    std::vector<PlacedGlyph> glyphs;
    std::vector<int> lineOfUnit; // line index for every parsed unit
    float width = 0, height = 0;
    int lines = 0;
};

TextLayout layoutText(const std::vector<Glyph>& glyphs, const TextStyle& style);
// Splits text into pages of at most `linesPerPage` wrapped lines.
std::vector<std::vector<Glyph>> paginateText(const std::vector<Glyph>& glyphs, const TextStyle& style,
                                             int linesPerPage);
} // namespace fe
