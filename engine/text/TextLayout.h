#pragma once
#include "engine/text/Font.h"
#include "engine/text/Markup.h"
#include <string>
#include <vector>

namespace fe {
enum class TextAlign { Left, Center, Right };

struct TextStyle {
    const FontSet* font = nullptr;
    const FontSet* bold = nullptr; // face for {b}; without one bold glyphs are drawn twice, 1px apart
    float size = 8;                // line height of the font, in canvas pixels
    float lineGap = 3;             // extra space between lines
    float wrapWidth = 0;           // 0 disables wrapping
    TextAlign align = TextAlign::Left;
};

struct PlacedGlyph {
    std::size_t unit = 0; // index into the parsed glyph sequence (a ruby reading: its base's unit)
    float x = 0, y = 0, width = 0, height = 0;
    Font* font = nullptr;
    const FontGlyph* source = nullptr;
    std::uint32_t color = 0xffffffff;
    std::string icon;
    bool syntheticBold = false; // draw again one pixel to the right
    TextEffect effect = TextEffect::None;
    float phase = 0; // the unit index, so neighbouring glyphs animate out of step
};

struct TextLayout {
    std::vector<PlacedGlyph> glyphs;
    std::vector<int> lineOfUnit;   // line index for every parsed unit
    std::vector<bool> wrappedLine; // per line: begun by word wrap (not a newline)
    float width = 0, height = 0;
    int lines = 0;
};

// Lays out glyphs in lines. Lines break at hard newlines and, with a wrap
// width, at the last break opportunity that fits: before a word that follows
// a space, and between CJK characters (kinsoku shori: no line starts with
// closing punctuation or small kana, none ends with an opening bracket).
// Ruby bases never break. A run with no opportunity breaks where it
// overflows. Each line is as tall as its largest {size:} glyph plus room for
// its ruby readings, which sit centred above their bases at half size. A
// reading wider than its base respaces the base instead of overhanging: the
// excess goes 1:2:1 around and between the base characters (JIS X 4051).
// Right-to-left scripts are not supported (no bidirectional reordering).
TextLayout layoutText(const std::vector<Glyph>& glyphs, const TextStyle& style);
// Splits text into pages of at most `linesPerPage` wrapped lines.
std::vector<std::vector<Glyph>> paginateText(const std::vector<Glyph>& glyphs, const TextStyle& style,
                                             int linesPerPage);
} // namespace fe
