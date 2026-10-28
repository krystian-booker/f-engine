#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
// One unit of parsed markup. Text glyphs carry a codepoint; icon glyphs carry
// an icon id; pure timing glyphs ({wait:...}) carry neither. Typewriter
// effects count these units, so waits are part of the reveal sequence.
struct Glyph {
    char32_t codepoint = 0;
    std::uint32_t color = 0xffffffff;
    std::string icon;
    float wait = 0;  // seconds to pause before revealing the next unit
    float speed = 0; // characters per second from here on (0 = unchanged)
    [[nodiscard]] bool newline() const { return codepoint == U'\n'; }
    [[nodiscard]] bool space() const { return codepoint == U' ' || codepoint == U'\t'; }
};

// Resolves tags the parser does not know ({player}, {var:chapter}...).
// Returning nullopt keeps the tag text literally.
using MarkupResolver = std::function<std::optional<std::string>(std::string_view tag)>;

class ColorPalette {
public:
    ColorPalette();
    void define(const std::string& name, std::uint32_t rgba) { colors_[name] = rgba; }
    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view name) const;

private:
    std::map<std::string, std::uint32_t, std::less<>> colors_;
};

// Tags: {color:name|#rrggbb|#rrggbbaa|reset} {icon:id} {wait:seconds}
// {speed:cps}. "{{" is a literal brace. Unknown tags go to the resolver.
std::vector<Glyph> parseMarkup(std::string_view source, std::uint32_t baseColor, const ColorPalette& palette,
                               const MarkupResolver& resolver = {});
// Rebuilds markup from glyphs (used to hand pages back to scripts).
std::string toMarkup(const std::vector<Glyph>& glyphs, std::uint32_t baseColor);
std::string utf8(char32_t codepoint);
std::u32string decodeUtf8(std::string_view text);
} // namespace fe
