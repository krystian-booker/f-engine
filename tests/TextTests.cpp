#include "engine/core/AssetCache.h"
#include "engine/core/NumberFormat.h"
#include "engine/services/Canvas.h"
#include "engine/text/TextSystem.h"
#include <doctest/doctest.h>
#include <fstream>
#include <limits>

using namespace fe;

TEST_CASE("markup parses colors, icons, waits, escapes and custom tags") {
    ColorPalette palette;
    const auto glyphs = parseMarkup("A{color:red}B{icon:sword}{wait:0.5}{{{who}", 0xffffffff, palette,
                                    [](std::string_view tag) -> std::optional<std::string> {
                                        if (tag == "who") {
                                            return "Io";
                                        }
                                        return std::nullopt;
                                    });
    REQUIRE(glyphs.size() == 7);
    CHECK(glyphs[0].codepoint == U'A');
    CHECK(glyphs[1].color == 0xff7070ff);
    CHECK(glyphs[2].icon == "sword");
    CHECK(glyphs[3].wait == doctest::Approx(0.5));
    CHECK(glyphs[4].codepoint == U'{');
    CHECK(glyphs[5].codepoint == U'I');
    CHECK(glyphs[6].codepoint == U'o');
    const auto unknown = parseMarkup("{nope}", 0xffffffff, palette);
    CHECK(unknown.size() == 6);
}

TEST_CASE("markup round trips through toMarkup") {
    ColorPalette palette;
    const auto glyphs = parseMarkup("Hi {color:gold}there{icon:x}", 0xffffffff, palette);
    const auto again = parseMarkup(toMarkup(glyphs, 0xffffffff), 0xffffffff, palette);
    REQUIRE(again.size() == glyphs.size());
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        CHECK(again[i].codepoint == glyphs[i].codepoint);
        CHECK(again[i].color == glyphs[i].color);
        CHECK(again[i].icon == glyphs[i].icon);
    }
}

TEST_CASE("layout wraps words and pagination splits pages") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_LUALIB);
    assets.defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    TextSystem text(assets);
    TextOptions options;
    options.width = 60; // ten 6-pixel characters
    const auto layout = text.layout("one two three four five six", options);
    CHECK(layout.lines == 3);
    CHECK(layout.width <= 60);
    const auto pages = text.paginate("one two three four five six seven eight nine", options, 2);
    // Five lines, two per page: "one two/three four", "five six/seven", "eight nine".
    REQUIRE(pages.size() == 3);
    CHECK(pages[0].find("one") == 0);
    CHECK(pages[1].find("five") == 0);
    CHECK(pages[2].find("eight") == 0);
}

TEST_CASE("UTF-8 decoding and encoding") {
    const auto decoded = decodeUtf8("é→€");
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[1] == U'→');
    CHECK(utf8(decoded[2]) == "€");
    CHECK(decodeUtf8("\xF0\x9F\x98\x80") == U"\U0001F600");
}

TEST_CASE("invalid UTF-8 becomes U+FFFD per maximal invalid sequence") {
    constexpr char32_t R = ReplacementCharacter;
    CHECK(decodeUtf8("\xC0\xAF") == std::u32string{R, R});               // overlong '/'
    CHECK(decodeUtf8("\xE0\x80\xAF") == std::u32string{R, R, R});        // overlong 3-byte
    CHECK(decodeUtf8("\xED\xA0\x80") == std::u32string{R, R, R});        // UTF-16 surrogate
    CHECK(decodeUtf8("\xF4\x90\x80\x80") == std::u32string{R, R, R, R}); // above U+10FFFF
    CHECK(decodeUtf8("a\x80z") == std::u32string{U'a', R, U'z'});        // stray continuation
    CHECK(decodeUtf8("a\xE2\x82") == std::u32string{U'a', R});           // truncated
    CHECK(decodeUtf8("\xE2\x82"
                     "b") == std::u32string{R, U'b'});
    CHECK(decodeUtf8("\xFF") == std::u32string{R});
    CHECK(utf8(0xd800) == utf8(R));
    CHECK(utf8(0x110000) == utf8(R));
}

namespace {
// A font where every glyph is a box `advance` wide; "AV" kerns by -2.
class BoxFont final : public Font {
public:
    BoxFont(float line, float base, float advance, bool wide) : line_(line), base_(base), wide_(wide) {
        glyph_.advance = advance;
        glyph_.width = glyph_.height = advance;
        glyph_.offsetY = 1;
        image_.width = image_.height = 1;
        image_.rgba.assign(4, 255);
    }
    [[nodiscard]] float lineHeight() const override { return line_; }
    [[nodiscard]] float baseline() const override { return base_; }
    [[nodiscard]] const FontGlyph* glyph(char32_t codepoint) override {
        return (codepoint < 0x80) != wide_ ? &glyph_ : nullptr;
    }
    [[nodiscard]] float kerning(char32_t a, char32_t b) const override {
        return a == U'A' && b == U'V' ? -2 : 0;
    }
    [[nodiscard]] std::size_t pageCount() const override { return 1; }
    [[nodiscard]] const Image& page(std::size_t) const override { return image_; }
    [[nodiscard]] bool pixel() const override { return true; }

private:
    float line_, base_;
    bool wide_;
    FontGlyph glyph_;
    Image image_;
};

std::vector<Glyph> plain(std::string_view text) {
    return parseMarkup(text, 0xffffffff, ColorPalette{});
}
} // namespace

TEST_CASE("layout applies kerning pairs") {
    const FontSet font({std::make_shared<BoxFont>(10, 8, 6, false)});
    TextStyle style;
    style.font = &font;
    style.size = 10;
    const auto kerned = layoutText(plain("AV"), style);
    REQUIRE(kerned.glyphs.size() == 2);
    CHECK(kerned.glyphs[1].x == doctest::Approx(4));
    CHECK(kerned.width == doctest::Approx(10));
    CHECK(layoutText(plain("VA"), style).glyphs[1].x == doctest::Approx(6));
}

TEST_CASE("fallback faces sit on the primary baseline") {
    const FontSet font(
        {std::make_shared<BoxFont>(10, 8, 6, false), std::make_shared<BoxFont>(20, 10, 12, true)});
    TextStyle style;
    style.font = &font;
    style.size = 10;
    const auto layout = layoutText(plain("A\xE5\xAD\x97"), style);
    REQUIRE(layout.glyphs.size() == 2);
    CHECK(layout.glyphs[0].y == doctest::Approx(1));
    // Scaled by 10/20, shifted so its baseline (10 * 0.5) lands on 8.
    CHECK(layout.glyphs[1].y == doctest::Approx(3.5));
    CHECK(layout.glyphs[1].height == doctest::Approx(6));
}

TEST_CASE("leading spaces are dropped only on wrapped lines") {
    const FontSet font({std::make_shared<BoxFont>(10, 8, 6, false)});
    TextStyle style;
    style.font = &font;
    style.size = 10;
    const auto indented = layoutText(plain("a\n  b"), style);
    REQUIRE(indented.glyphs.size() == 2);
    CHECK(indented.glyphs[1].x == doctest::Approx(12));
    style.wrapWidth = 18;
    const auto wrapped = layoutText(plain("aaa bbb"), style);
    REQUIRE(wrapped.glyphs.size() == 6);
    CHECK(wrapped.glyphs[3].x == doctest::Approx(0));
    style.wrapWidth = 30;
    const auto pages = paginateText(plain("aaa bbb\n  ccc"), style, 1);
    REQUIRE(pages.size() == 3);
    CHECK(pages[1].front().codepoint == U'b');
    CHECK(pages[2].front().codepoint == U' ');
}

TEST_CASE("bitmap font pages cannot escape the font's folder") {
    const auto root = std::filesystem::temp_directory_path() / "fengine-font-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "fonts");
    std::ofstream(root / "fonts" / "bad.fnt")
        << "common lineHeight=10 base=8\npage id=0 file=\"../outside.png\"\n"
           "char id=65 x=0 y=0 width=1 height=1 xoffset=0 yoffset=0 xadvance=1 page=0\n";
    CHECK_THROWS_WITH(loadBitmapFont(root / "fonts" / "bad.fnt"), doctest::Contains("escapes"));
}

TEST_CASE("markup sizes, bold, effects, ruby and instant speed parse and round trip") {
    ColorPalette palette;
    const auto glyphs =
        parseMarkup("{size:2}A{/size}{b}B{/b}{shake}C{/shake}{wave}D{/wave}{ruby:漢字|かんじ}{speed:0}E",
                    0xffffffff, palette);
    REQUIRE(glyphs.size() == 8);
    CHECK(glyphs[0].scale == doctest::Approx(2));
    CHECK(glyphs[1].scale == doctest::Approx(1));
    CHECK(glyphs[1].bold);
    CHECK_FALSE(glyphs[2].bold);
    CHECK(glyphs[2].effect == TextEffect::Shake);
    CHECK(glyphs[3].effect == TextEffect::Wave);
    CHECK(glyphs[4].codepoint == U'漢');
    CHECK(glyphs[4].ruby == "かんじ");
    CHECK(glyphs[4].rubySpan == 2);
    CHECK(glyphs[5].rubyContinues);
    CHECK(glyphs[6].timing());
    CHECK(glyphs[6].speed == doctest::Approx(0));
    CHECK(glyphs[7].speed < 0);
    const auto again = parseMarkup(toMarkup(glyphs, 0xffffffff), 0xffffffff, palette);
    REQUIRE(again.size() == glyphs.size());
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        CHECK(again[i].codepoint == glyphs[i].codepoint);
        CHECK(again[i].scale == glyphs[i].scale);
        CHECK(again[i].bold == glyphs[i].bold);
        CHECK(again[i].effect == glyphs[i].effect);
        CHECK(again[i].ruby == glyphs[i].ruby);
        CHECK(again[i].speed == glyphs[i].speed);
    }
    CHECK(parseMarkup("{size:99}{ruby:x}", 0xffffffff, palette).size() == 17); // invalid tags stay literal
}

TEST_CASE("sized glyphs share a baseline and grow their line") {
    const FontSet font({std::make_shared<BoxFont>(10, 8, 6, false)});
    TextStyle style;
    style.font = &font;
    style.size = 10;
    style.lineGap = 2;
    const auto layout = layoutText(parseMarkup("a{size:2}b{/size}\nc", 0xffffffff, ColorPalette{}), style);
    REQUIRE(layout.glyphs.size() == 3);
    CHECK(layout.glyphs[1].x == doctest::Approx(6));
    CHECK(layout.glyphs[1].height == doctest::Approx(12));
    // Baseline at 16 for the first line: the small glyph's top is 8 above it, the big one's 16.
    CHECK(layout.glyphs[0].y == doctest::Approx(8 + 1));
    CHECK(layout.glyphs[1].y == doctest::Approx(0 + 2));
    CHECK(layout.glyphs[2].y == doctest::Approx(22 + 1));
    CHECK(layout.height == doctest::Approx(32));
}

TEST_CASE("ruby readings sit centred above their base and reveal with it") {
    const FontSet font({std::make_shared<BoxFont>(10, 8, 6, false)});
    TextStyle style;
    style.font = &font;
    style.size = 10;
    const auto layout = layoutText(parseMarkup("x{ruby:ab|cdef}", 0xffffffff, ColorPalette{}), style);
    REQUIRE(layout.glyphs.size() == 7); // x, a, the reading, b
    CHECK(layout.height == doctest::Approx(15));
    const auto& base = layout.glyphs[1];
    CHECK(base.x == doctest::Approx(6));
    CHECK(base.y == doctest::Approx(5 + 1));
    // Four half-size characters (3 px each) centred over the 12 px base.
    CHECK(layout.glyphs[2].x == doctest::Approx(6));
    CHECK(layout.glyphs[5].x == doctest::Approx(15));
    CHECK(layout.glyphs[2].y == doctest::Approx(0.5));
    CHECK(layout.glyphs[2].unit == 1);
    CHECK(layout.glyphs[4].unit == 2);
    // A reading wider than its base spreads the base 1:2:1 and the text after it.
    const auto wide = layoutText(parseMarkup("x{ruby:ab|cdefghij}y", 0xffffffff, ColorPalette{}), style);
    REQUIRE(wide.glyphs.size() == 12);                          // x, a, eight reading characters, b, y
    CHECK(wide.glyphs[1].x == doctest::Approx(6 + 3));          // 12 px excess over two characters
    CHECK(wide.glyphs[10].x == doctest::Approx(6 + 3 + 6 + 6)); // b: a share after a
    CHECK(wide.glyphs[11].x == doctest::Approx(6 + 24));        // y after the 24 px base
    CHECK(wide.glyphs[2].x == doctest::Approx(6));              // the reading spans the base exactly
    CHECK(wide.glyphs[9].x == doctest::Approx(6 + 21));
    CHECK(wide.width == doctest::Approx(36));
    // A ruby base never breaks across lines.
    style.wrapWidth = 12;
    const auto wrapped = layoutText(parseMarkup("x{ruby:ab|c}", 0xffffffff, ColorPalette{}), style);
    CHECK(wrapped.lineOfUnit[1] == 1);
    CHECK(wrapped.lineOfUnit[2] == 1);
}

TEST_CASE("bold uses the bold face or draws twice; effects reach canvas quads") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_LUALIB);
    assets.defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    TextSystem text(assets);
    Canvas canvas(text, 320, 180);
    canvas.begin();
    const auto plainWidth = canvas.measure("AB", {}).width;
    CHECK(canvas.measure("{b}AB{/b}", {}).width == doctest::Approx(plainWidth + 2));
    canvas.text("A{b}B{/b}{wave}C{/wave}{shake}D", {0, 0}, {});
    const auto& quads = canvas.quads();
    REQUIRE(quads.size() == 5);
    CHECK(quads[1].corners[0].x + 1 == doctest::Approx(quads[2].corners[0].x));
    CHECK(quads[0].effect == 0);
    CHECK(quads[3].effect == static_cast<std::uint8_t>(TextEffect::Wave));
    CHECK(quads[4].effect == static_cast<std::uint8_t>(TextEffect::Shake));
    CHECK(quads[4].phase == doctest::Approx(3));
    assets.defineFont("default.bold", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    const auto bold = text.layout("{b}AB{/b}", {});
    REQUIRE(bold.glyphs.size() == 2);
    CHECK_FALSE(bold.glyphs[0].syntheticBold);
    CHECK(bold.width == doctest::Approx(plainWidth));
}

TEST_CASE("CJK text breaks between characters following kinsoku rules") {
    const FontSet font(
        {std::make_shared<BoxFont>(10, 8, 6, false), std::make_shared<BoxFont>(10, 8, 10, true)});
    TextStyle style;
    style.font = &font;
    style.size = 10;
    style.wrapWidth = 30; // three CJK characters
    const auto lines = [&](std::string_view text) {
        const auto glyphs = parseMarkup(text, 0xffffffff, ColorPalette{});
        const auto layout = layoutText(glyphs, style);
        std::vector<std::u32string> out(static_cast<std::size_t>(layout.lines));
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            out[static_cast<std::size_t>(layout.lineOfUnit[i])] += glyphs[i].codepoint;
        }
        return out;
    };
    CHECK(lines("あいうえおか") == std::vector<std::u32string>{U"あいう", U"えおか"});
    // A closing mark or small kana cannot start a line: the character before it moves down too.
    CHECK(lines("あいう。えお") == std::vector<std::u32string>{U"あい", U"う。え", U"お"});
    CHECK(lines("あいうっていく") == std::vector<std::u32string>{U"あい", U"うって", U"いく"});
    // An opening bracket cannot end a line.
    CHECK(lines("あい「うえ」") == std::vector<std::u32string>{U"あい", U"「う", U"え」"});
    // Latin words break at spaces; next to CJK characters they may break too.
    CHECK(lines("abc def") == std::vector<std::u32string>{U"abc ", U"def"});
    CHECK(lines("ab漢字") == std::vector<std::u32string>{U"ab漢", U"字"});
}

TEST_CASE("pagination keeps sizes, ruby and instant speed") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_LUALIB);
    assets.defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    TextSystem text(assets);
    TextOptions options;
    options.width = 60;
    const auto pages =
        text.paginate("{size:2}one{/size} {ruby:two|x} {speed:0}three four five six", options, 2);
    REQUIRE(pages.size() >= 2);
    CHECK(pages[0].find("{size:2}one{/size}") != std::string::npos);
    CHECK(pages[0].find("{ruby:two|x}") != std::string::npos);
    CHECK(pages[0].find("{speed:0}") != std::string::npos);
}

TEST_CASE("resolved markup is bounded per parse") {
    // The reviewer's probe: each tag resolving to 25 more tags expanded two
    // tags into 25^4 resolver calls and tens of thousands of pages.
    ColorPalette palette;
    int calls = 0;
    const MarkupResolver amplifying = [&calls](std::string_view) -> std::optional<std::string> {
        ++calls;
        std::string out;
        for (int i = 0; i < 25; ++i) {
            out += "{x}";
        }
        return out;
    };
    CHECK_THROWS_WITH(parseMarkup("{a}{a}", 0xffffffff, palette, amplifying),
                      doctest::Contains("expand too much"));
    CHECK(calls <= static_cast<int>(MaxResolvedBytes / 75) + 2);
    // Replacements up to the limit resolve, nested 4 levels deep.
    const MarkupResolver nested = [](std::string_view tag) -> std::optional<std::string> {
        if (tag.size() < 4) {
            return "<{" + std::string(tag) + "x}>";
        }
        return std::string(MaxResolvedBytes / 2, 'z');
    };
    const auto glyphs = parseMarkup("{a}", 0xffffffff, palette, nested);
    CHECK(glyphs.size() > MaxResolvedBytes / 2);
}

TEST_CASE("{size} and {b} nest: closing tags restore the enclosing style") {
    ColorPalette palette;
    const auto glyphs = parseMarkup("{size:2}a{size:3}b{/size}c{/size}d{b}e{b}f{/b}g{/b}h{/b}i", 0xffffffff, palette);
    REQUIRE(glyphs.size() == 9);
    CHECK(glyphs[0].scale == doctest::Approx(2));
    CHECK(glyphs[1].scale == doctest::Approx(3));
    CHECK(glyphs[2].scale == doctest::Approx(2)); // back to the outer size, not 1
    CHECK(glyphs[3].scale == doctest::Approx(1));
    CHECK(glyphs[4].bold);
    CHECK(glyphs[5].bold);
    CHECK(glyphs[6].bold); // one {/b} of two
    CHECK_FALSE(glyphs[7].bold);
    CHECK_FALSE(glyphs[8].bold); // a stray {/b} is harmless
    // Pages written back as markup keep the nested sizes.
    const auto again = parseMarkup(toMarkup(glyphs, 0xffffffff), 0xffffffff, palette);
    REQUIRE(again.size() == glyphs.size());
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        CHECK(again[i].scale == glyphs[i].scale);
        CHECK(again[i].bold == glyphs[i].bold);
    }
    CHECK(parseMarkup("{/size}x", 0xffffffff, palette)[0].scale == doctest::Approx(1));
}

TEST_CASE("a ruby base broken by the layout is kept whole across a page break") {
    const FontSet font({std::make_shared<BoxFont>(10, 8, 6, false)});
    TextStyle style;
    style.font = &font;
    style.size = 10;
    style.wrapWidth = 18; // three characters: the five-character base cannot fit a line
    const auto glyphs = parseMarkup("ab {ruby:cdefg|x} h", 0xffffffff, ColorPalette{});
    const auto layout = layoutText(glyphs, style);
    REQUIRE(layout.lines >= 3);
    CHECK(layout.lineOfUnit[3] != layout.lineOfUnit[7]); // the base spans lines
    const auto pages = paginateText(glyphs, style, 2);
    REQUIRE(pages.size() >= 2);
    for (const auto& page : pages) {
        // Every page's markup holds its ruby whole (or not at all), without a newline inside the base.
        const auto markup = toMarkup(page, 0xffffffff);
        const auto ruby = markup.find("{ruby:");
        if (ruby != std::string::npos) {
            CHECK(markup.substr(ruby, 14) == "{ruby:cdefg|x}");
        }
        for (const auto& g : page) {
            CHECK_FALSE((g.rubyContinues && &g != &page.front() && (&g)[-1].newline()));
        }
    }
    CHECK(toMarkup(pages[1], 0xffffffff).find("{ruby:cdefg|x}") != std::string::npos);
    CHECK(toMarkup(pages[0], 0xffffffff).find("ruby") == std::string::npos);
}

TEST_CASE("numbers format with a language's separators") {
    CHECK(formatNumber(1234567.891, 2, "en-US") == "1,234,567.89");
    CHECK(formatNumber(1234567.891, 2, "de") == "1.234.567,89");
    CHECK(formatNumber(1234567.891, 2, "fr-FR") == "1 234 567,89");
    CHECK(formatNumber(1234567.891, 2, "pt_BR") == "1.234.567,89");
    CHECK(formatNumber(1234567.891, 0, "ja") == "1,234,568");
    CHECK(formatNumber(-1234.5, 1, "ru") == "-1 234,5");
    CHECK(formatNumber(999, 0, "sv") == "999");
    CHECK(formatNumber(1000, 0, "tr") == "1.000");
    CHECK(formatNumber(0.125, 2, "nl") == "0,13");
    CHECK(formatNumber(-0.001, 2, "en") == "0.00"); // no negative zero
    CHECK(formatNumber(42, 3, "xx-YY") == "42.000");  // unknown languages format as English
    CHECK_THROWS((void)formatNumber(1, 21, "en"));
    CHECK_THROWS((void)formatNumber(std::numeric_limits<double>::infinity(), 0, "en"));
    CHECK(numberSeparators("ES").decimal == ",");
    CHECK(numberSeparators("zh-Hant-TW").group == ",");
}
