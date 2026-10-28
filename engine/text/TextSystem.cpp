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
    TextStyle style;
    style.font = current_.get();
    style.size = options.size > 0 ? options.size : current_->lineHeight();
    style.lineGap = options.lineGap;
    style.wrapWidth = options.width;
    style.align = options.align;
    return style;
}

TextLayout TextSystem::layout(const std::string& markup, const TextOptions& options) {
    return layoutText(parse(markup, options.color), style(options));
}

std::vector<std::string> TextSystem::paginate(const std::string& markup, const TextOptions& options,
                                              int lines) {
    std::vector<std::string> pages;
    for (const auto& page : paginateText(parse(markup, options.color), style(options), lines)) {
        pages.push_back(toMarkup(page, options.color));
    }
    return pages;
}
} // namespace fe
