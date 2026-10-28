#pragma once
#include "engine/ui/BitmapFont.h"
#include "engine/ui/Text.h"
#include "engine/core/Json.h"
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <map>
#include <optional>

namespace fe {
struct Rect { float x=0,y=0,w=0,h=0; };
// Game UI uses a virtual pixel canvas; ImGui supplies only the final draw list.
// No ImGui widgets or layouts are required by the game-facing menus.
class Canvas {
public:
    Canvas(std::filesystem::path gameRoot,int virtualWidth,int virtualHeight);
    ~Canvas();
    Canvas(const Canvas&)=delete;
    Canvas& operator=(const Canvas&)=delete;
    void begin();
    void window(Rect rectangle);
    void rectangle(Rect rectangle,std::uint32_t rgba);
    void fillScreen(std::uint32_t rgba);
    [[nodiscard]] glm::vec2 fromViewport(glm::vec2 normalized) const;
    void text(const std::string& text,float x,float y,std::uint32_t rgba=0xffffffff,float size=8);
    void rich(const std::vector<TextGlyph>& text,std::size_t visible,Rect rectangle,float size=8);
    [[nodiscard]] std::vector<std::vector<TextGlyph>> pages(const std::vector<TextGlyph>& text,Rect rectangle,float size=8) const;
    void image(const std::string& file,Rect rectangle);
    void gauge(Rect rectangle,float value,std::uint32_t rgba);
    void list(const std::vector<std::string>& labels,MenuCursor& cursor,Rect rectangle,int visibleRows=7);
    [[nodiscard]] Rect layout(const std::string& name,Rect fallback) const;
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    void reload();
private:
    bgfx::TextureHandle texture(const std::string& file);
    float glyph(const std::string& utf8,float x,float y,std::uint32_t rgba,float size);
    std::filesystem::path root_;
    int width_,height_;
    float scale_=1;
    glm::vec2 offset_{0};
    std::map<std::string,bgfx::TextureHandle> textures_;
    std::optional<BitmapFont> font_;
    std::filesystem::path fontDirectory_;
    Json layouts_,icons_;
    std::string skin_;
    int skinWidth_=24,skinHeight_=24,border_=8;
};
}
