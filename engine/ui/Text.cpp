#include "engine/ui/Text.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace fe {
namespace {
std::uint32_t color(const std::string& name, std::uint32_t fallback) {
    static const std::map<std::string,std::uint32_t> named{{"white",0xffffffff},{"red",0xff7070ff},
        {"green",0x80ff90ff},{"blue",0x80b0ffff},{"gold",0xffda80ff},{"yellow",0xffee80ff},{"reset",0xffffffff}};
    if (auto found = named.find(name); found != named.end()) return found->second;
    if (name.size() == 7 && name[0] == '#') {
        std::uint32_t rgb = 0; const auto [end,ec] = std::from_chars(name.data()+1,name.data()+7,rgb,16);
        if (ec == std::errc{} && end == name.data()+7) return (rgb<<8)|0xff;
    }
    return fallback;
}
void appendUtf8(std::vector<TextGlyph>& out, const std::string& text, std::uint32_t tint) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t size = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 1;
        if (i+size > text.size()) size = 1;
        for (std::size_t j = 1; j < size; ++j) if ((static_cast<unsigned char>(text[i+j]) & 0xc0) != 0x80) { size = 1; break; }
        out.push_back({text.substr(i,size),tint,"",0}); i += size;
    }
}
}
std::vector<TextGlyph> parseText(const std::string& source, const TextSubstitution& substitution) {
    std::vector<TextGlyph> out; std::uint32_t tint = 0xffffffff;
    for (std::size_t i = 0; i < source.size();) {
        if (source[i] == '{') {
            const auto end = source.find('}',i+1);
            if (end != std::string::npos) {
                const auto code = source.substr(i+1,end-i-1);
                if (code.starts_with("color:")) { tint = color(code.substr(6),tint); i = end+1; continue; }
                if (code.starts_with("icon:")) { out.push_back({"",tint,code.substr(5),0}); i = end+1; continue; }
                if (code.starts_with("wait:")) {
                    int frames = 0; const auto [last,ec] = std::from_chars(code.data()+5,code.data()+code.size(),frames);
                    if (ec == std::errc{} && last == code.data()+code.size() && frames >= 0 && frames <= 36000) {
                        out.push_back({"",tint,"",frames/60.0}); i = end+1; continue;
                    }
                }
                if (substitution) {
                    auto value = substitution(code);
                    if (!value.empty()) { appendUtf8(out,value,tint); i = end+1; continue; }
                }
            }
        }
        auto end = source.find('{',i+1);
        if (end == std::string::npos) end = source.size();
        appendUtf8(out,source.substr(i,end-i),tint); i = end;
    }
    return out;
}
std::vector<std::vector<TextGlyph>> paginateText(const std::vector<TextGlyph>& glyphs,float width,int linesPerPage,const GlyphMeasure& measure) {
    width=std::max(1.0f,width);linesPerPage=std::max(1,linesPerPage);
    std::vector<std::vector<TextGlyph>> pages(1);
    float x=0;int line=0;
    auto nextLine=[&] {
        x=0;
        if (++line>=linesPerPage) {pages.emplace_back();line=0;}
        else pages.back().push_back({"\n",0xffffffff,"",0});
    };
    for (std::size_t i=0;i<glyphs.size();++i) {
        const auto& g=glyphs[i];
        if (g.text=="\n") {nextLine();continue;}
        const bool space=g.text==" " || g.text=="\t";
        if (!space && (i==0 || glyphs[i-1].text==" " || glyphs[i-1].text=="\n")) {
            float word=0;
            for (auto j=i;j<glyphs.size() && glyphs[j].text!=" " && glyphs[j].text!="\t" && glyphs[j].text!="\n";++j) word+=measure(glyphs[j]);
            if (x>0 && word<=width && x+word>width) nextLine();
        }
        const float advance=measure(g);
        if (x>0 && x+advance>width) nextLine();
        if (space && x==0) continue;
        pages.back().push_back(g);x+=advance;
    }
    if (pages.size()>1 && pages.back().empty()) pages.pop_back();
    return pages;
}
void Typewriter::set(std::vector<TextGlyph> glyphs, double speed) {
    glyphs_ = std::move(glyphs); visible_ = 0; speed_ = std::max(1.0,speed); elapsed_ = 0;
}
void Typewriter::update(double dt) {
    if (!std::isfinite(dt) || dt <= 0 || complete()) return;
    elapsed_ += dt;
    while (!complete()) {
        const auto& glyph = glyphs_[visible_];
        const double delay = glyph.wait > 0 ? glyph.wait : 1/speed_;
        if (elapsed_+1e-12 < delay) break;
        elapsed_ = std::max(0.0,elapsed_ - delay); ++visible_;
    }
}
void Typewriter::finish() { visible_ = glyphs_.size(); elapsed_ = 0; }
void MenuCursor::setCount(int count) { count_ = std::max(0,count); select(selected_); }
void MenuCursor::select(int index) { selected_ = count_ ? std::clamp(index,0,count_-1) : 0; }
bool MenuCursor::move(int direction) {
    if (count_ < 1 || !direction) return false;
    const auto previous = selected_;
    selected_ = ((selected_ + direction) % count_ + count_) % count_;
    return previous != selected_;
}
int MenuCursor::scrollOffset(int visibleRows) {
    visibleRows = std::max(1,visibleRows);
    if (selected_ < offset_) offset_ = selected_;
    if (selected_ >= offset_+visibleRows) offset_ = selected_-visibleRows+1;
    offset_ = std::clamp(offset_,0,std::max(0,count_-visibleRows));
    return offset_;
}
void NumberTween::update(double dt) {
    if (!std::isfinite(dt) || dt <= 0) return;
    displayed_ += (target_-displayed_)*(1-std::exp(-12*dt));
    if (std::abs(target_-displayed_) < 0.01) displayed_ = target_;
}
}
