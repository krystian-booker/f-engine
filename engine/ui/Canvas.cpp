#include "engine/ui/Canvas.h"
#include "engine/assets/Image.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace fe {
namespace {
ImU32 tint(std::uint32_t rgba) { return IM_COL32((rgba>>24)&255,(rgba>>16)&255,(rgba>>8)&255,rgba&255); }
std::uint32_t codepoint(const std::string& text) {
    if (text.empty()) return 0;
    const auto first=static_cast<unsigned char>(text[0]);
    std::uint32_t result=first;
    if (text.size()==2) result=first&31;
    if (text.size()==3) result=first&15;
    if (text.size()==4) result=first&7;
    for (std::size_t i=1;i<text.size();++i) result=(result<<6)|(static_cast<unsigned char>(text[i])&63);
    return result;
}
}
Canvas::Canvas(std::filesystem::path root,int width,int height) : root_(std::move(root)),width_(width),height_(height) { reload(); }
Canvas::~Canvas() { for (auto [file,handle] : textures_) { (void)file; bgfx::destroy(handle); } }
void Canvas::reload() {
    for (auto [file,handle] : textures_) { (void)file; bgfx::destroy(handle); }
    textures_.clear(); font_.reset(); layouts_=Json::object(); icons_=Json::object(); skin_.clear();
    if (std::filesystem::exists(root_/"ui/layout.json")) layouts_=readJson(root_/"ui/layout.json");
    if (layouts_.contains("font")) {
        const auto path=std::filesystem::path(layouts_.at("font").get<std::string>());
        font_=BitmapFont::load(root_/path); fontDirectory_=path.parent_path();
    }
    if (std::filesystem::exists(root_/"ui/icons.json")) icons_=readJson(root_/"ui/icons.json");
    skin_=layouts_.value("skin",std::string("ui/skin.png"));
    if (std::filesystem::exists(root_/skin_)) {
        const auto image=Image::load(root_/skin_); skinWidth_=image.width; skinHeight_=image.height;
        border_=layouts_.value("skin_border",8); texture(skin_);
    } else skin_.clear();
}
void Canvas::begin() {
    const auto size=ImGui::GetIO().DisplaySize;
    const float fit=std::min(size.x/width_,size.y/height_);
    // Fractional downscaling is allowed on small windows. At normal sizes the
    // virtual grid uses an integer scale and is centered in the viewport.
    scale_=fit>=1 ? std::max(1.0f,std::floor(fit)) : fit;
    offset_={(size.x-width_*scale_)*.5f,(size.y-height_*scale_)*.5f};
}
bgfx::TextureHandle Canvas::texture(const std::string& file) {
    if (auto found=textures_.find(file);found!=textures_.end()) return found->second;
    const auto image=Image::load(root_/file);
    auto handle=bgfx::createTexture2D(static_cast<std::uint16_t>(image.width),static_cast<std::uint16_t>(image.height),false,1,bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_MIN_POINT|BGFX_SAMPLER_MAG_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(image.rgba.data(),static_cast<std::uint32_t>(image.rgba.size())));
    textures_[file]=handle; return handle;
}
void Canvas::rectangle(Rect r,std::uint32_t rgba) {
    auto* draw=ImGui::GetBackgroundDrawList();
    draw->AddRectFilled({offset_.x+r.x*scale_,offset_.y+r.y*scale_},{offset_.x+(r.x+r.w)*scale_,offset_.y+(r.y+r.h)*scale_},tint(rgba));
}
void Canvas::fillScreen(std::uint32_t rgba) {ImGui::GetBackgroundDrawList()->AddRectFilled({0,0},ImGui::GetIO().DisplaySize,tint(rgba));}
glm::vec2 Canvas::fromViewport(glm::vec2 normalized) const {
    const auto display=ImGui::GetIO().DisplaySize;
    return (normalized*glm::vec2(display.x,display.y)-offset_)/scale_;
}
void Canvas::window(Rect r) {
    if (skin_.empty()) {
        rectangle(r,0x101d39ed); rectangle({r.x,r.y,r.w,1},0xbac8e8ff); rectangle({r.x,r.y+r.h-1,r.w,1},0x7186acff);
        rectangle({r.x,r.y,1,r.h},0xbac8e8ff); rectangle({r.x+r.w-1,r.y,1,r.h},0x7186acff);
        rectangle({r.x+2,r.y+2,r.w-4,1},0x43577cff); return;
    }
    auto* draw=ImGui::GetBackgroundDrawList(); const float border=std::min({float(border_),r.w/2,r.h/2});
    const float xs[]{r.x,r.x+border,r.x+r.w-border,r.x+r.w},ys[]{r.y,r.y+border,r.y+r.h-border,r.y+r.h};
    const float us[]{0,float(border_)/skinWidth_,1-float(border_)/skinWidth_,1},vs[]{0,float(border_)/skinHeight_,1-float(border_)/skinHeight_,1};
    for (int y=0;y<3;++y) for (int x=0;x<3;++x)
        draw->AddImage(static_cast<ImTextureID>(texture(skin_).idx+1),{offset_.x+xs[x]*scale_,offset_.y+ys[y]*scale_},
            {offset_.x+xs[x+1]*scale_,offset_.y+ys[y+1]*scale_},{us[x],vs[y]},{us[x+1],vs[y+1]});
}
float Canvas::glyph(const std::string& utf8,float x,float y,std::uint32_t rgba,float size) {
    auto* draw=ImGui::GetBackgroundDrawList();
    if (font_) {
        const auto* g=font_->glyph(codepoint(utf8)); if (!g) return size*.5f;
        const float s=size/font_->lineHeight;
        const auto handle=texture((fontDirectory_/font_->pages.at(g->page)).generic_string());
        const ImVec2 a{offset_.x+(x+g->offsetX*s)*scale_,offset_.y+(y+g->offsetY*s)*scale_};
        const ImVec2 b{a.x+g->width*s*scale_,a.y+g->height*s*scale_};
        draw->AddImage(static_cast<ImTextureID>(handle.idx+1),a,b,{float(g->x)/font_->atlasWidth,float(g->y)/font_->atlasHeight},
            {float(g->x+g->width)/font_->atlasWidth,float(g->y+g->height)/font_->atlasHeight},tint(rgba));
        return g->advance*s;
    }
    auto* font=ImGui::GetFont();
    const ImVec2 position{std::floor(offset_.x+x*scale_),std::floor(offset_.y+y*scale_)};
    draw->AddText(font,size*scale_,{position.x+scale_*.5f,position.y+scale_*.5f},IM_COL32(0,0,0,180),utf8.c_str());
    draw->AddText(font,size*scale_,position,tint(rgba),utf8.c_str());
    return font->CalcTextSizeA(size,10000,0,utf8.c_str()).x;
}
void Canvas::text(const std::string& value,float x,float y,std::uint32_t rgba,float size) {
    const auto glyphs=parseText(value);
    float px=x;
    for (const auto& g:glyphs) {
        if (g.text=="\n") { y+=size+3; px=x; continue; }
        if (!g.text.empty()) px+=glyph(g.text,px,y,g.color==0xffffffff?rgba:g.color,size);
    }
}
std::vector<std::vector<TextGlyph>> Canvas::pages(const std::vector<TextGlyph>& text,Rect r,float size) const {
    return paginateText(text,r.w,std::max(1,int((r.h+3)/(size+3))),[&](const TextGlyph& g) {
        if (!g.icon.empty()) return size+2;
        if (g.text.empty()) return 0.0f;
        if (font_) {const auto* glyph=font_->glyph(codepoint(g.text));return glyph?glyph->advance*size/font_->lineHeight:size*.5f;}
        const auto& fonts=ImGui::GetIO().Fonts->Fonts;
        return fonts.empty()?size*.55f:fonts[0]->CalcTextSizeA(size,10000,0,g.text.c_str()).x;
    });
}
void Canvas::rich(const std::vector<TextGlyph>& glyphs,std::size_t visible,Rect r,float size) {
    float x=r.x,y=r.y;
    auto* draw=ImGui::GetBackgroundDrawList();
    draw->PushClipRect({offset_.x+r.x*scale_,offset_.y+r.y*scale_},{offset_.x+(r.x+r.w)*scale_,offset_.y+(r.y+r.h)*scale_},true);
    std::uint32_t previous=0;
    for (std::size_t i=0;i<std::min(visible,glyphs.size());++i) {
        const auto& g=glyphs[i];
        if (g.text=="\n") { x=r.x; y+=size+3; previous=0; continue; }
        if (!g.icon.empty()) {
            if (icons_.contains(g.icon)) image(icons_.at(g.icon).get<std::string>(),{x,y,size,size});
            else rectangle({x,y+1,size-1,size-1},0xffd880ff);
            x+=size+2;
        }
        if (!g.text.empty()) {
            const auto current=codepoint(g.text);
            if (font_) x+=font_->kerning(previous,current)*size/font_->lineHeight;
            x+=glyph(g.text,x,y,g.color,size); previous=current;
        }
    }
    draw->PopClipRect();
}
void Canvas::image(const std::string& file,Rect r) {
    ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(texture(file).idx+1),
        {offset_.x+r.x*scale_,offset_.y+r.y*scale_},{offset_.x+(r.x+r.w)*scale_,offset_.y+(r.y+r.h)*scale_});
}
void Canvas::gauge(Rect r,float value,std::uint32_t rgba) {
    rectangle(r,0x080e22ff); r.w=std::max(0.0f,(r.w-2)*std::clamp(value,0.0f,1.0f)); r.x+=1;r.y+=1;r.h=std::max(0.0f,r.h-2); rectangle(r,rgba);
}
void Canvas::list(const std::vector<std::string>& labels,MenuCursor& cursor,Rect r,int rows) {
    cursor.setCount(static_cast<int>(labels.size())); const int offset=cursor.scrollOffset(rows);
    for (int row=0;row<rows && row+offset<static_cast<int>(labels.size());++row) {
        const float y=r.y+row*13;
        if (row+offset==cursor.selected()) { rectangle({r.x-2,y-1,r.w,12},0x638ac255); text(">",r.x,y,0xffdb84ff); }
        text(labels[row+offset],r.x+9,y,row+offset==cursor.selected()?0xffe2a0ff:0xe4e9f4ff);
    }
}
Rect Canvas::layout(const std::string& name,Rect fallback) const {
    if (!layouts_.contains(name)) return fallback;
    const auto& r=layouts_.at(name); return {r.value("x",fallback.x),r.value("y",fallback.y),r.value("w",fallback.w),r.value("h",fallback.h)};
}
}
