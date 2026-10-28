#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fe {
struct TextGlyph {
    std::string text;
    std::uint32_t color = 0xffffffff;
    std::string icon;
    double wait = 0;
};
using TextSubstitution = std::function<std::string(const std::string&)>;
std::vector<TextGlyph> parseText(const std::string& source, const TextSubstitution& substitution = {});
// Word-wrap using the renderer's measured advances; split into complete pages.
using GlyphMeasure = std::function<float(const TextGlyph&)>;
std::vector<std::vector<TextGlyph>> paginateText(const std::vector<TextGlyph>& glyphs,
    float width, int linesPerPage, const GlyphMeasure& measure);
class Typewriter {
public:
    void set(std::vector<TextGlyph> glyphs, double glyphsPerSecond = 40);
    void update(double dt);
    void finish();
    [[nodiscard]] bool complete() const { return visible_ == glyphs_.size(); }
    [[nodiscard]] std::size_t visible() const { return visible_; }
    [[nodiscard]] const std::vector<TextGlyph>& glyphs() const { return glyphs_; }
private:
    std::vector<TextGlyph> glyphs_;
    std::size_t visible_ = 0;
    double speed_ = 40, elapsed_ = 0;
};
class MenuCursor {
public:
    void setCount(int count);
    bool move(int direction);
    void select(int index);
    [[nodiscard]] int selected() const { return selected_; }
    [[nodiscard]] int count() const { return count_; }
    [[nodiscard]] int scrollOffset(int visibleRows);
private:
    int selected_ = 0, count_ = 0, offset_ = 0;
};
class NumberTween {
public:
    explicit NumberTween(double initial = 0) : displayed_(initial), target_(initial) {}
    void target(double value) { target_ = value; }
    void update(double dt);
    [[nodiscard]] double value() const { return displayed_; }
private:
    double displayed_, target_;
};
}
