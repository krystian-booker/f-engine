#include "engine/text/Markup.h"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

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
        if (first < 0x80) {
            out.push_back(first);
            ++i;
            continue;
        }
        // Well-formed sequences (Unicode table 3-7): the lead byte fixes the
        // length and the range of the second byte, which rules out overlong
        // forms, surrogates and values above U+10FFFF.
        std::size_t size = 0;
        char32_t value = 0;
        unsigned char low = 0x80, high = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            size = 2;
            value = first & 0x1fu;
        } else if (first >= 0xe0 && first <= 0xef) {
            size = 3;
            value = first & 0x0fu;
            low = first == 0xe0 ? 0xa0 : 0x80;
            high = first == 0xed ? 0x9f : 0xbf;
        } else if (first >= 0xf0 && first <= 0xf4) {
            size = 4;
            value = first & 0x07u;
            low = first == 0xf0 ? 0x90 : 0x80;
            high = first == 0xf4 ? 0x8f : 0xbf;
        }
        // An invalid sequence becomes one U+FFFD per maximal invalid prefix.
        std::size_t used = 1;
        for (; size && used < size && i + used < text.size(); ++used) {
            const auto next = static_cast<unsigned char>(text[i + used]);
            if (next < (used == 1 ? low : 0x80) || next > (used == 1 ? high : 0xbf)) {
                break;
            }
            value = (value << 6u) | (next & 0x3fu);
        }
        out.push_back(size && used == size ? value : ReplacementCharacter);
        i += used;
    }
    return out;
}

std::string utf8(char32_t c) {
    if ((c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff) {
        c = ReplacementCharacter;
    }
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
    Glyph style; // color, scale, bold and effect for the next glyphs
    std::vector<Glyph> out;
    std::size_t resolved = 0; // bytes the resolver returned so far

    [[nodiscard]] Glyph styled() const {
        Glyph glyph;
        glyph.color = style.color;
        glyph.scale = style.scale;
        glyph.bold = style.bold;
        glyph.effect = style.effect;
        return glyph;
    }
    void text(std::string_view value) {
        for (const auto c : decodeUtf8(value)) {
            auto glyph = styled();
            glyph.codepoint = c;
            out.push_back(std::move(glyph));
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
        const bool bare = colon == std::string_view::npos;
        if (name == "color") {
            if (argument == "reset") {
                style.color = baseColor;
                return true;
            }
            if (const auto value = palette.find(argument)) {
                style.color = *value;
                return true;
            }
            return false;
        }
        if (name == "icon" && !argument.empty()) {
            auto glyph = styled();
            glyph.icon = std::string(argument);
            out.push_back(std::move(glyph));
            return true;
        }
        if (name == "wait" || name == "speed") {
            const auto value = number(argument);
            if (!value || *value > 600) {
                return false;
            }
            auto glyph = styled();
            (name == "wait" ? glyph.wait : glyph.speed) = *value;
            out.push_back(std::move(glyph));
            return true;
        }
        if (name == "size") {
            const auto value = number(argument);
            if (!value || *value < 0.25f || *value > 8) {
                return false;
            }
            style.scale = *value;
            return true;
        }
        if (bare && (tag == "/size" || tag == "b" || tag == "/b")) {
            if (tag == "/size") {
                style.scale = 1;
            } else {
                style.bold = tag == "b";
            }
            return true;
        }
        if (bare && (tag == "shake" || tag == "wave")) {
            style.effect = tag == "shake" ? TextEffect::Shake : TextEffect::Wave;
            return true;
        }
        if (bare && (tag == "/shake" || tag == "/wave")) {
            style.effect = TextEffect::None;
            return true;
        }
        if (name == "ruby") {
            const auto bar = argument.find('|');
            if (bar == std::string_view::npos) {
                return false;
            }
            const auto base = decodeUtf8(argument.substr(0, bar));
            const auto reading = argument.substr(bar + 1);
            if (base.empty() || base.size() > 0xffff || reading.empty()) {
                return false;
            }
            for (std::size_t i = 0; i < base.size(); ++i) {
                auto glyph = styled();
                glyph.codepoint = base[i];
                if (i == 0) {
                    glyph.ruby = std::string(reading);
                    glyph.rubySpan = static_cast<std::uint16_t>(base.size());
                } else {
                    glyph.rubyContinues = true;
                }
                out.push_back(std::move(glyph));
            }
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
                    // Each level of resolved tags may name more tags, so a
                    // few short replacements could otherwise expand into
                    // millions of glyphs.
                    resolved += replacement->size();
                    if (resolved > MaxResolvedBytes) {
                        throw std::runtime_error(
                            "Text tags expand too much: the resolver returned more than 1 MiB for one text");
                    }
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
    Parser parser{palette, resolver, baseColor, {}, {}};
    parser.style.color = baseColor;
    parser.parse(source, 0);
    return std::move(parser.out);
}

std::string toMarkup(const std::vector<Glyph>& glyphs, std::uint32_t baseColor) {
    std::string out;
    Glyph style;
    style.color = baseColor;
    char buffer[32];
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto& glyph = glyphs[i];
        if (glyph.color != style.color) {
            std::snprintf(buffer, sizeof buffer, "{color:#%08x}", glyph.color);
            out += buffer;
        }
        if (glyph.scale != style.scale) {
            std::snprintf(buffer, sizeof buffer, "{size:%g}", glyph.scale);
            out += glyph.scale == 1 ? std::string("{/size}") : std::string(buffer);
        }
        if (glyph.bold != style.bold) {
            out += glyph.bold ? "{b}" : "{/b}";
        }
        if (glyph.effect != style.effect) {
            out += glyph.effect == TextEffect::Shake   ? "{shake}"
                   : glyph.effect == TextEffect::Wave  ? "{wave}"
                   : style.effect == TextEffect::Shake ? "{/shake}"
                                                       : "{/wave}";
        }
        style = glyph;
        if (glyph.rubySpan > 0) {
            out += "{ruby:";
            const auto end = std::min(glyphs.size(), i + glyph.rubySpan);
            for (auto j = i; j < end; ++j) {
                out += utf8(glyphs[j].codepoint);
            }
            out += "|" + glyph.ruby + "}";
            i = end - 1;
        } else if (!glyph.icon.empty()) {
            out += "{icon:" + glyph.icon + "}";
        } else if (glyph.wait > 0) {
            std::snprintf(buffer, sizeof buffer, "{wait:%g}", glyph.wait);
            out += buffer;
        } else if (glyph.speed >= 0) {
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
