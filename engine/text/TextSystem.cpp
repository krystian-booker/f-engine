#include "engine/text/TextSystem.h"

namespace fe {
const IconDef* TextSystem::icon(const std::string& id) const {
    const auto found = icons_.find(id);
    return found == icons_.end() ? nullptr : &found->second;
}

std::vector<Glyph> TextSystem::parse(const std::string& markup, std::uint32_t color) const {
    return parseMarkup(markup, color, palette_, resolver_);
}

TextStyle TextSystem::style(const TextOptions& options) {
    current_ = assets_.font(options.font);
    const auto bold = options.font + ".bold";
    currentBold_ = assets_.hasFont(bold) ? assets_.font(bold) : nullptr;
    TextStyle style;
    style.font = current_.get();
    style.bold = currentBold_.get();
    style.size = options.size > 0 ? options.size : current_->lineHeight();
    style.lineGap = options.lineGap;
    style.wrapWidth = options.width;
    style.align = options.align;
    return style;
}

// The markup is parsed (running the script's resolver, which may lay out
// other text) before style() takes the fonts this layout uses.
TextLayout TextSystem::layout(const std::string& markup, const TextOptions& options) {
    const auto glyphs = parse(markup, options.color);
    return layoutText(glyphs, style(options));
}

std::vector<std::string> TextSystem::paginate(const std::string& markup, const TextOptions& options,
                                              int lines) {
    std::vector<std::string> pages;
    const auto glyphs = parse(markup, options.color);
    for (const auto& page : paginateText(glyphs, style(options), lines)) {
        pages.push_back(toMarkup(page, options.color));
    }
    return pages;
}
} // namespace fe
