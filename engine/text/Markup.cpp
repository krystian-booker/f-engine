#include "engine/text/Markup.h"
#include <charconv>
#include <cmath>
#include <cstdio>

namespace fe {
ColorPalette::ColorPalette()
    : colors_{{"white", 0xffffffff}, {"black", 0x000000ff}, {"red", 0xff7070ff},    {"green", 0x80ff90ff},
              {"blue", 0x80b0ffff},  {"gold", 0xffda80ff},  {"yellow", 0xffee80ff}, {"gray", 0xa0a8b8ff}} {
}

std::optional<std::uint32_t> ColorPalette::find(std::string_view name) const {
    if (const auto found = colors_.find(name); found != colors_.end()) {
        return found->second;
    }
    if ((name.size() == 7 || name.size() == 9) && name[0] == '#') {
        std::uint32_t value = 0;
        const auto [end, ec] = std::from_chars(name.data() + 1, name.data() + name.size(), value, 16);
        if (ec == std::errc{} && end == name.data() + name.size()) {
            return name.size() == 7 ? (value << 8u) | 0xffu : value;
        }
    }
    return std::nullopt;
}

std::u32string decodeUtf8(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t size = 1;
        char32_t value = first;
        if ((first & 0xe0u) == 0xc0u) {
            size = 2;
            value = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            size = 3;
            value = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            size = 4;
            value = first & 0x07u;
        }
        bool valid = i + size <= text.size();
        for (std::size_t j = 1; valid && j < size; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            valid = (next & 0xc0u) == 0x80u;
            value = (value << 6u) | (next & 0x3fu);
        }
        out.push_back(valid ? value : U'?');
        i += valid ? size : 1;
    }
    return out;
}

std::string utf8(char32_t c) {
    std::string out;
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
    return out;
}

namespace {
struct Parser {
    const ColorPalette& palette;
    const MarkupResolver& resolver;
    std::uint32_t baseColor;
    std::uint32_t color;
    std::vector<Glyph> out;

    void text(std::string_view value) {
        for (const auto c : decodeUtf8(value)) {
            out.push_back({c, color, {}, 0, 0});
        }
    }
    static std::optional<float> number(std::string_view value) {
        float result = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(result) || result < 0) {
            return std::nullopt;
        }
        return result;
    }
    bool builtin(std::string_view tag) {
        const auto colon = tag.find(':');
        const auto name = tag.substr(0, colon);
        const auto argument = colon == std::string_view::npos ? std::string_view{} : tag.substr(colon + 1);
        if (name == "color") {
            if (argument == "reset") {
                color = baseColor;
                return true;
            }
            if (const auto value = palette.find(argument)) {
                color = *value;
                return true;
            }
            return false;
        }
        if (name == "icon" && !argument.empty()) {
            out.push_back({0, color, std::string(argument), 0, 0});
            return true;
        }
        if (name == "wait" || name == "speed") {
            const auto value = number(argument);
            if (!value || *value > 600) {
                return false;
            }
            Glyph glyph{0, color, {}, 0, 0};
            (name == "wait" ? glyph.wait : glyph.speed) = *value;
            out.push_back(glyph);
            return true;
        }
        return false;
    }
    void parse(std::string_view source, int depth) {
        std::size_t i = 0;
        while (i < source.size()) {
            const auto brace = source.find('{', i);
            text(source.substr(i, brace - i));
            if (brace == std::string_view::npos) {
                return;
            }
            if (brace + 1 < source.size() && source[brace + 1] == '{') {
                text("{");
                i = brace + 2;
                continue;
            }
            const auto end = source.find('}', brace + 1);
            if (end == std::string_view::npos) {
                text(source.substr(brace));
                return;
            }
            const auto tag = source.substr(brace + 1, end - brace - 1);
            if (!builtin(tag)) {
                std::optional<std::string> replacement;
                if (resolver && depth < 4) {
                    replacement = resolver(tag);
                }
                if (replacement) {
                    parse(*replacement, depth + 1);
                } else {
                    text(source.substr(brace, end - brace + 1));
                }
            }
            i = end + 1;
        }
    }
};
} // namespace

std::vector<Glyph> parseMarkup(std::string_view source, std::uint32_t baseColor, const ColorPalette& palette,
                               const MarkupResolver& resolver) {
    Parser parser{palette, resolver, baseColor, baseColor, {}};
    parser.parse(source, 0);
    return std::move(parser.out);
}

std::string toMarkup(const std::vector<Glyph>& glyphs, std::uint32_t baseColor) {
    std::string out;
    std::uint32_t color = baseColor;
    char buffer[32];
    for (const auto& glyph : glyphs) {
        if (glyph.color != color) {
            std::snprintf(buffer, sizeof buffer, "{color:#%08x}", glyph.color);
            out += buffer;
            color = glyph.color;
        }
        if (!glyph.icon.empty()) {
            out += "{icon:" + glyph.icon + "}";
        } else if (glyph.wait > 0) {
            std::snprintf(buffer, sizeof buffer, "{wait:%g}", glyph.wait);
            out += buffer;
        } else if (glyph.speed > 0) {
            std::snprintf(buffer, sizeof buffer, "{speed:%g}", glyph.speed);
            out += buffer;
        } else if (glyph.codepoint == U'{') {
            out += "{{";
        } else if (glyph.codepoint) {
            out += utf8(glyph.codepoint);
        }
    }
    return out;
}
} // namespace fe
