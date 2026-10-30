#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
// Animated per-glyph effects; renderers move glyphs by effect and phase.
enum class TextEffect : std::uint8_t { None, Shake, Wave };

// One unit of parsed markup. Text glyphs carry a codepoint; icon glyphs carry
// an icon id; pure timing glyphs ({wait:...}, {speed:...}) carry neither.
// Typewriter effects count these units, so waits are part of the reveal
// sequence. Ruby readings are not units: they belong to their base glyphs.
struct Glyph {
    char32_t codepoint = 0;
    std::uint32_t color = 0xffffffff;
    std::string icon;
    float wait = 0;   // seconds to pause before revealing the next unit
    float speed = -1; // characters per second from here on (0 = instant, < 0 = unchanged)
    float scale = 1;  // {size:n}
    bool bold = false;
    TextEffect effect = TextEffect::None;
    std::string ruby; // reading set above this unit and the next rubySpan - 1
    std::uint16_t rubySpan = 0;
    bool rubyContinues = false; // a later unit of a ruby base
    [[nodiscard]] bool newline() const { return codepoint == U'\n'; }
    [[nodiscard]] bool space() const { return codepoint == U' ' || codepoint == U'\t'; }
    [[nodiscard]] bool timing() const { return !codepoint && icon.empty(); }
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
// {speed:cps} ({speed:0} shows the rest at once) {size:scale} ... {/size}
// {b} ... {/b} {shake} ... {/shake} {wave} ... {/wave} and
// {ruby:base|reading} (furigana: `reading` set small above `base`). {size}
// and {b} nest: {/size} restores the scale in force before its {size}, and
// {b}{b}x{/b}y{/b} keeps y bold. "{{" is a literal brace. Unknown tags go to
// the resolver; tags in its replacements resolve too, 4 levels deep, and more
// than MaxResolvedBytes of replacements in one parse throw std::runtime_error.
inline constexpr std::size_t MaxResolvedBytes = std::size_t{1} << 20;
std::vector<Glyph> parseMarkup(std::string_view source, std::uint32_t baseColor, const ColorPalette& palette,
                               const MarkupResolver& resolver = {});
// Rebuilds markup from glyphs (used to hand pages back to scripts).
std::string toMarkup(const std::vector<Glyph>& glyphs, std::uint32_t baseColor);
constexpr char32_t ReplacementCharacter = 0xfffd;
// Encodes a codepoint; surrogates and values above U+10FFFF become U+FFFD.
std::string utf8(char32_t codepoint);
// Strict decoding: overlong forms, surrogates, stray continuation bytes and
// truncated sequences become U+FFFD.
std::u32string decodeUtf8(std::string_view text);
} // namespace fe
