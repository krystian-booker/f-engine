#pragma once
#include "engine/text/TextSystem.h"
#include <array>
#include <glm/glm.hpp>
#include <map>
#include <string_view>
#include <vector>

namespace fe {
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct CanvasTexture {
    enum class Kind { White, Image, FontPage };
    Kind kind = Kind::White;
    std::string image;
    const Font* font = nullptr;
    int page = 0;
    bool linear = false;
    bool operator==(const CanvasTexture&) const = default;
};

struct CanvasQuad {
    CanvasTexture texture;
    std::array<glm::vec2, 4> corners{}; // clockwise from top-left, canvas pixels
    std::array<glm::vec2, 4> uv{};
    std::uint32_t color = 0xffffffff; // rgba
    Rect clip{0, 0, -1, -1};          // w < 0 = no clip
    int layer = 0;
    bool additive = false;
    // Text glyphs only: the markup effect (a TextEffect: 0 none, 1 shake,
    // 2 wave) and the glyph's phase (its unit index), so the renderer can
    // offset each glyph out of step with its neighbours.
    std::uint8_t effect = 0;
    float phase = 0;
};

struct ImageOptions {
    Rect source{}; // pixels, zero size = whole image
    std::uint32_t color = 0xffffffff;
    bool flipX = false, flipY = false, linear = false;
};

struct Transform2D {
    glm::vec2 translate{0}, scale{1}, origin{0};
    float rotation = 0; // radians, about origin
    float alpha = 1;
};

// Records 2D draw calls in virtual-pixel space each frame. Scripts build all
// game UI with it; the runtime batches the quads on the GPU and headless runs
// simply discard them.
class Canvas {
public:
    Canvas(TextSystem& text, int width, int height);
    void begin();
    void resize(int width, int height);
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

    void rect(Rect area, std::uint32_t color);
    void line(glm::vec2 from, glm::vec2 to, std::uint32_t color, float thickness = 1);
    void image(const std::string& path, Rect area, const ImageOptions& options = {});
    // Stretches the centre and edges of `path`, keeping `border` pixels of corners.
    void nineSlice(const std::string& path, Rect area, glm::vec4 border, const ImageOptions& options = {});
    glm::vec2 text(const std::string& markup, glm::vec2 position, const TextOptions& options);
    [[nodiscard]] TextLayout measure(const std::string& markup, const TextOptions& options);
    [[nodiscard]] glm::ivec2 imageSize(const std::string& path);
    // Render targets ("rt:<name>" images): world:draw{target} records each
    // one's size in canvas pixels; fe.canvas.image shows what was drawn there.
    static constexpr std::string_view TargetPrefix = "rt:";
    void defineTarget(const std::string& name, glm::ivec2 size) { targets_[name] = size; }

    void pushClip(Rect area);
    void popClip();
    void pushTransform(const Transform2D& transform);
    void popTransform();
    // Enlarge by exactly what fits instead of by whole factors (game.json canvas_scale "smooth").
    void setSmoothScale(bool smooth) { smoothScale_ = smooth; }
    void setLayer(int layer) { layer_ = layer; }
    void setAdditive(bool additive) { additive_ = additive; }

    // How the virtual canvas maps onto a window: integer scaling when it fits
    // (or the exact fit with setSmoothScale), centred. Shared by the renderer and by picking.
    struct Presentation {
        float scale = 1;
        glm::vec2 offset{0};
    };
    [[nodiscard]] Presentation present(glm::ivec2 viewport) const;
    [[nodiscard]] glm::vec2 toWindow(glm::vec2 canvas, glm::ivec2 viewport) const;
    [[nodiscard]] glm::vec2 fromWindow(glm::vec2 window, glm::ivec2 viewport) const;

    [[nodiscard]] const std::vector<CanvasQuad>& quads() const { return quads_; }
    [[nodiscard]] TextSystem& textSystem() { return text_; }

private:
    void quad(const CanvasTexture& texture, Rect area, glm::vec4 uv, std::uint32_t color);
    void glyphs(const TextLayout& layout, glm::vec2 origin, const TextOptions& options, std::uint32_t tint,
                glm::vec2 offset);
    TextSystem& text_;
    int width_, height_;
    bool smoothScale_ = false;
    int layer_ = 0;
    bool additive_ = false;
    std::vector<CanvasQuad> quads_;
    std::vector<Rect> clips_;
    std::vector<std::pair<glm::mat3, float>> transforms_;
    std::map<std::string, glm::ivec2, std::less<>> targets_;
};

std::uint32_t multiplyAlpha(std::uint32_t rgba, float alpha);
} // namespace fe
