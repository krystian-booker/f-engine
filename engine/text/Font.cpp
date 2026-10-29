#include "engine/text/Font.h"
#include "engine/core/Paths.h"
#include <cstring>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace fe {
namespace {
class BitmapFont final : public Font {
public:
    explicit BitmapFont(const std::filesystem::path& file) {
        std::istringstream stream(readFile(file));
        const std::regex field(R"re(([A-Za-z]+)=("[^"]*"|[^\s]+))re");
        std::map<int, std::string> pageFiles;
        std::string line;
        while (std::getline(stream, line)) {
            std::istringstream words(line);
            std::string type;
            words >> type;
            std::map<std::string, std::string> fields;
            for (auto it = std::sregex_iterator(line.begin(), line.end(), field);
                 it != std::sregex_iterator(); ++it) {
                std::string value = (*it)[2];
                if (value.starts_with('"')) {
                    value = value.substr(1, value.size() - 2);
                }
                fields[(*it)[1]] = value;
            }
            auto integer = [&](const char* key) {
                return std::stoi(fields.at(key));
            };
            if (type == "common") {
                lineHeight_ = static_cast<float>(integer("lineHeight"));
                baseline_ = fields.contains("base") ? static_cast<float>(integer("base")) : lineHeight_;
            } else if (type == "page") {
                pageFiles[integer("id")] = fields.at("file");
            } else if (type == "char") {
                FontGlyph g;
                g.x = static_cast<float>(integer("x"));
                g.y = static_cast<float>(integer("y"));
                g.width = static_cast<float>(integer("width"));
                g.height = static_cast<float>(integer("height"));
                g.offsetX = static_cast<float>(integer("xoffset"));
                g.offsetY = static_cast<float>(integer("yoffset"));
                g.advance = static_cast<float>(integer("xadvance"));
                g.page = integer("page");
                glyphs_[static_cast<char32_t>(integer("id"))] = g;
            } else if (type == "kerning") {
                kernings_[{static_cast<char32_t>(integer("first")),
                           static_cast<char32_t>(integer("second"))}] = static_cast<float>(integer("amount"));
            }
        }
        if (lineHeight_ <= 0 || pageFiles.empty() || glyphs_.empty()) {
            throw std::runtime_error("Invalid BMFont text file: " + file.string());
        }
        for (const auto& [id, name] : pageFiles) {
            if (id != static_cast<int>(pages_.size())) {
                throw std::runtime_error("BMFont pages must be numbered from zero: " + file.string());
            }
            pages_.push_back(Image::load(resolveInside(file.parent_path(), name)));
        }
        for (const auto& [codepoint, g] : glyphs_) {
            const bool inside = g.page >= 0 && g.page < static_cast<int>(pages_.size()) && g.x >= 0 &&
                                g.y >= 0 && g.x + g.width <= static_cast<float>(pages_[g.page].width) &&
                                g.y + g.height <= static_cast<float>(pages_[g.page].height);
            if (!inside) {
                throw std::runtime_error("Bitmap font glyph outside its page: " + file.string());
            }
        }
    }
    [[nodiscard]] float lineHeight() const override { return lineHeight_; }
    [[nodiscard]] float baseline() const override { return baseline_; }
    [[nodiscard]] const FontGlyph* glyph(char32_t codepoint) override {
        const auto found = glyphs_.find(codepoint);
        return found == glyphs_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] float kerning(char32_t a, char32_t b) const override {
        const auto found = kernings_.find({a, b});
        return found == kernings_.end() ? 0 : found->second;
    }
    [[nodiscard]] std::size_t pageCount() const override { return pages_.size(); }
    [[nodiscard]] const Image& page(std::size_t index) const override { return pages_.at(index); }
    [[nodiscard]] bool pixel() const override { return true; }

private:
    float lineHeight_ = 0, baseline_ = 0;
    std::map<char32_t, FontGlyph> glyphs_;
    std::map<std::pair<char32_t, char32_t>, float> kernings_;
    std::vector<Image> pages_;
};

class TrueTypeFont final : public Font {
public:
    TrueTypeFont(const std::filesystem::path& file, float size, bool pixel) : pixel_(pixel) {
        const auto bytes = readFile(file);
        bytes_.assign(bytes.begin(), bytes.end());
        if (size < 4 || size > 256 ||
            !stbtt_InitFont(&info_, bytes_.data(), stbtt_GetFontOffsetForIndex(bytes_.data(), 0))) {
            throw std::runtime_error("Invalid TrueType font or size: " + file.string());
        }
        scale_ = stbtt_ScaleForPixelHeight(&info_, size);
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&info_, &ascent, &descent, &gap);
        ascent_ = static_cast<float>(ascent) * scale_;
        lineHeight_ = static_cast<float>(ascent - descent + gap) * scale_;
        addPage();
    }
    [[nodiscard]] float lineHeight() const override { return lineHeight_; }
    [[nodiscard]] float baseline() const override { return ascent_; }
    [[nodiscard]] const FontGlyph* glyph(char32_t codepoint) override {
        if (const auto found = glyphs_.find(codepoint); found != glyphs_.end()) {
            return found->second ? &*found->second : nullptr;
        }
        const int index = stbtt_FindGlyphIndex(&info_, static_cast<int>(codepoint));
        if (index == 0 && codepoint != U' ') {
            glyphs_[codepoint] = std::nullopt;
            return nullptr;
        }
        glyphs_[codepoint] = rasterize(index);
        return &*glyphs_[codepoint];
    }
    [[nodiscard]] float kerning(char32_t a, char32_t b) const override {
        return static_cast<float>(
                   stbtt_GetCodepointKernAdvance(&info_, static_cast<int>(a), static_cast<int>(b))) *
               scale_;
    }
    [[nodiscard]] std::size_t pageCount() const override { return pages_.size(); }
    [[nodiscard]] const Image& page(std::size_t index) const override { return pages_.at(index); }
    [[nodiscard]] std::uint32_t pageVersion(std::size_t index) const override { return versions_.at(index); }
    [[nodiscard]] bool pixel() const override { return pixel_; }

private:
    static constexpr int PageSize = 512;
    void addPage() {
        Image image;
        image.width = image.height = PageSize;
        image.rgba.assign(static_cast<std::size_t>(PageSize) * PageSize * 4, 0);
        pages_.push_back(std::move(image));
        versions_.push_back(1);
        penX_ = penY_ = 1;
        rowHeight_ = 0;
    }
    FontGlyph rasterize(int index) {
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&info_, index, &advance, &bearing);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&info_, index, scale_, scale_, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (penX_ + w + 1 >= PageSize) {
            penX_ = 1;
            penY_ += rowHeight_ + 1;
            rowHeight_ = 0;
        }
        if (penY_ + h + 1 >= PageSize) {
            addPage();
        }
        auto& image = pages_.back();
        std::vector<unsigned char> coverage(static_cast<std::size_t>(std::max(1, w * h)));
        if (w > 0 && h > 0) {
            stbtt_MakeGlyphBitmap(&info_, coverage.data(), w, h, w, scale_, scale_, index);
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                auto alpha = coverage[static_cast<std::size_t>(y) * w + x];
                if (pixel_) {
                    alpha = alpha >= 128 ? 255 : 0;
                }
                const auto offset = (static_cast<std::size_t>(penY_ + y) * PageSize + penX_ + x) * 4;
                image.rgba[offset] = image.rgba[offset + 1] = image.rgba[offset + 2] = 255;
                image.rgba[offset + 3] = alpha;
            }
        }
        FontGlyph glyph;
        glyph.page = static_cast<int>(pages_.size() - 1);
        glyph.x = static_cast<float>(penX_);
        glyph.y = static_cast<float>(penY_);
        glyph.width = static_cast<float>(w);
        glyph.height = static_cast<float>(h);
        glyph.offsetX = static_cast<float>(x0);
        glyph.offsetY = ascent_ + static_cast<float>(y0);
        glyph.advance = static_cast<float>(advance) * scale_;
        penX_ += w + 1;
        rowHeight_ = std::max(rowHeight_, h);
        ++versions_.back();
        return glyph;
    }
    std::vector<unsigned char> bytes_;
    stbtt_fontinfo info_{};
    float scale_ = 1, ascent_ = 0, lineHeight_ = 0;
    bool pixel_;
    std::map<char32_t, std::optional<FontGlyph>> glyphs_;
    std::vector<Image> pages_;
    std::vector<std::uint32_t> versions_;
    int penX_ = 1, penY_ = 1, rowHeight_ = 0;
};
} // namespace

std::shared_ptr<Font> loadBitmapFont(const std::filesystem::path& file) {
    return std::make_shared<BitmapFont>(file);
}

std::shared_ptr<Font> loadTrueTypeFont(const std::filesystem::path& file, float size, bool pixel) {
    return std::make_shared<TrueTypeFont>(file, size, pixel);
}

FontSet::FontSet(std::vector<std::shared_ptr<Font>> faces) : faces_(std::move(faces)) {
    if (faces_.empty()) {
        throw std::invalid_argument("A font set needs at least one face");
    }
}

FontSet::Hit FontSet::find(char32_t codepoint) const {
    const auto& primary = faces_.front();
    for (const auto& face : faces_) {
        if (const auto* glyph = face->glyph(codepoint)) {
            const float scale = primary->lineHeight() / face->lineHeight();
            return {face.get(), glyph, scale, primary->baseline() - face->baseline() * scale};
        }
    }
    return {primary.get(), primary->glyph(U'?'), 1, 0};
}
} // namespace fe
