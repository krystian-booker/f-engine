#include "engine/ui/BitmapFont.h"
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fe {
BitmapFont BitmapFont::load(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) throw std::runtime_error("Cannot open bitmap font " + file.string());
    BitmapFont font;
    const std::regex field(R"re(([A-Za-z]+)=("[^"]*"|[^\s]+))re");
    std::string line;
    while (std::getline(stream,line)) {
        std::istringstream words(line); std::string type; words >> type;
        std::map<std::string,std::string> fields;
        for (auto it = std::sregex_iterator(line.begin(),line.end(),field); it != std::sregex_iterator(); ++it) {
            std::string value = (*it)[2];
            if (value.starts_with('"')) value = value.substr(1,value.size()-2);
            fields[(*it)[1]] = value;
        }
        auto integer = [&](const char* key) { return std::stoi(fields.at(key)); };
        if (type == "common") { font.lineHeight = integer("lineHeight"); font.atlasWidth = integer("scaleW"); font.atlasHeight = integer("scaleH"); }
        if (type == "page") font.pages[integer("id")] = fields.at("file");
        if (type == "char") font.glyphs[integer("id")] = {integer("x"),integer("y"),integer("width"),integer("height"),integer("xoffset"),integer("yoffset"),integer("xadvance"),integer("page")};
        if (type == "kerning") font.kernings[{integer("first"),integer("second")}] = integer("amount");
    }
    if (font.lineHeight <= 0 || font.atlasWidth <= 0 || font.atlasHeight <= 0 || font.pages.empty() || font.glyphs.empty())
        throw std::runtime_error("Invalid BMFont text file: " + file.string());
    for (const auto& [id,g] : font.glyphs) {
        (void)id;
        if (!font.pages.contains(g.page) || g.x < 0 || g.y < 0 || g.width < 0 || g.height < 0 ||
            g.x+g.width > font.atlasWidth || g.y+g.height > font.atlasHeight) throw std::runtime_error("Invalid bitmap font glyph");
    }
    return font;
}
const FontGlyph* BitmapFont::glyph(std::uint32_t codepoint) const {
    auto found = glyphs.find(codepoint); if (found == glyphs.end()) found = glyphs.find('?');
    return found == glyphs.end() ? nullptr : &found->second;
}
int BitmapFont::kerning(std::uint32_t first, std::uint32_t second) const {
    auto found = kernings.find({first,second}); return found == kernings.end() ? 0 : found->second;
}
}
