#pragma once
#include "engine/core/AssetCache.h"
#include "engine/text/TextLayout.h"
#include <glm/glm.hpp>
#include <map>
#include <optional>

namespace fe {
struct IconDef {
    std::string image;
    glm::vec4 rect{0}; // x, y, w, h in pixels; zero size = whole image
};

struct TextOptions {
    std::string font = "default";
    float size = 0; // 0 = the font's native line height
    std::uint32_t color = 0xffffffff;
    float width = 0, lineGap = 3;
    TextAlign align = TextAlign::Left;
    std::size_t visible = static_cast<std::size_t>(-1); // typewriter: units shown
    std::uint32_t shadow = 0;                           // rgba, 0 = none
    std::uint32_t outline = 0;                          // rgba, 0 = none
};

// Shared text services: colors, icons and the script tag resolver.
class TextSystem {
public:
    explicit TextSystem(AssetCache& assets) : assets_(assets) {}
    ColorPalette& palette() { return palette_; }
    void defineIcon(const std::string& id, IconDef icon) { icons_[id] = std::move(icon); }
    [[nodiscard]] const IconDef* icon(const std::string& id) const;
    void setResolver(MarkupResolver resolver) { resolver_ = std::move(resolver); }
    [[nodiscard]] std::vector<Glyph> parse(const std::string& markup, std::uint32_t color) const;
    [[nodiscard]] TextStyle style(const TextOptions& options);
    [[nodiscard]] TextLayout layout(const std::string& markup, const TextOptions& options);
    [[nodiscard]] std::vector<std::string> paginate(const std::string& markup, const TextOptions& options,
                                                    int lines);
    [[nodiscard]] AssetCache& assets() { return assets_; }

private:
    AssetCache& assets_;
    ColorPalette palette_;
    std::map<std::string, IconDef> icons_;
    MarkupResolver resolver_;
    std::shared_ptr<FontSet> current_;
};
} // namespace fe
