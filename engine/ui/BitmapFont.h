#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <cstdint>

namespace fe {
struct FontGlyph { int x=0,y=0,width=0,height=0,offsetX=0,offsetY=0,advance=0,page=0; };
class BitmapFont {
public:
    static BitmapFont load(const std::filesystem::path& file);
    std::map<std::uint32_t,FontGlyph> glyphs;
    std::map<int,std::string> pages;
    std::map<std::pair<std::uint32_t,std::uint32_t>,int> kernings;
    int lineHeight = 0, atlasWidth = 0, atlasHeight = 0;
    [[nodiscard]] const FontGlyph* glyph(std::uint32_t codepoint) const;
    [[nodiscard]] int kerning(std::uint32_t first, std::uint32_t second) const;
};
}
