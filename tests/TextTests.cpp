#include "engine/core/AssetCache.h"
#include "engine/text/TextSystem.h"
#include <doctest/doctest.h>

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
    REQUIRE(pages.size() == 3); // "one two" | "three four" | "five six" | "seven" | "eight nine"
    CHECK(pages[0].find("one") == 0);
    CHECK(pages[1].find("five") == 0);
    CHECK(pages[2].find("eight") == 0);
}

TEST_CASE("UTF-8 decoding and encoding") {
    const auto decoded = decodeUtf8("é→€");
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[1] == U'→');
    CHECK(utf8(decoded[2]) == "€");
}
