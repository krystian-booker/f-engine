#pragma once
#include "engine/assets/Image.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace fe {
struct FontGlyph {
    int page = 0;
    float x = 0, y = 0, width = 0, height = 0; // atlas rectangle in pixels
    float offsetX = 0, offsetY = 0;            // from the pen position to the top-left
    float advance = 0;
};

// A font face at its native pixel size. Atlas pages are CPU images; the
// renderer uploads them and re-uploads when pageVersion changes (TrueType
// faces rasterize glyphs on first use).
class Font {
public:
    virtual ~Font() = default;
    [[nodiscard]] virtual float lineHeight() const = 0;
    [[nodiscard]] virtual const FontGlyph* glyph(char32_t codepoint) = 0;
    [[nodiscard]] virtual float kerning(char32_t, char32_t) const { return 0; }
    [[nodiscard]] virtual std::size_t pageCount() const = 0;
    [[nodiscard]] virtual const Image& page(std::size_t index) const = 0;
    [[nodiscard]] virtual std::uint32_t pageVersion(std::size_t) const { return 0; }
    // Pixel fonts are sampled with nearest filtering and scaled by integers.
    [[nodiscard]] virtual bool pixel() const = 0;
};

std::shared_ptr<Font> loadBitmapFont(const std::filesystem::path& fntFile);
std::shared_ptr<Font> loadTrueTypeFont(const std::filesystem::path& ttfFile, float pixelSize, bool pixel);

// A primary face plus fallbacks (e.g. Latin pixel font, then a CJK TTF).
class FontSet {
public:
    struct Hit {
        Font* font = nullptr;
        const FontGlyph* glyph = nullptr;
        float scale = 1; // converts the face's pixels to the set's native size
    };
    explicit FontSet(std::vector<std::shared_ptr<Font>> faces);
    [[nodiscard]] Hit find(char32_t codepoint) const;
    [[nodiscard]] float lineHeight() const { return faces_.front()->lineHeight(); }
    [[nodiscard]] const std::vector<std::shared_ptr<Font>>& faces() const { return faces_; }

private:
    std::vector<std::shared_ptr<Font>> faces_;
};
} // namespace fe
