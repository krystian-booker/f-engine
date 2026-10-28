#include "engine/render/WorldRenderer.h"
#include "engine/render/ImGuiRenderer.h"
#include "engine/assets/Image.h"
#include "engine/assets/Mesh.h"
#include "engine/assets/SpriteAnimation.h"
#include "engine/core/AssetRegistry.h"
#include "engine/core/Log.h"
#include "engine/world/World.h"
#include <bgfx/bgfx.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <set>

namespace fe {
namespace {
template<class H> struct GpuHandle {
    H value = BGFX_INVALID_HANDLE;
    GpuHandle() = default;
    explicit GpuHandle(H handle) : value(handle) {}
    ~GpuHandle() { if (bgfx::isValid(value)) bgfx::destroy(value); }
    GpuHandle(const GpuHandle&) = delete;
    GpuHandle& operator=(const GpuHandle&) = delete;
    GpuHandle(GpuHandle&& other) noexcept : value(other.value) { other.value = BGFX_INVALID_HANDLE; }
    GpuHandle& operator=(GpuHandle&& other) noexcept {
        if (this != &other) { if (bgfx::isValid(value)) bgfx::destroy(value); value = other.value; other.value = BGFX_INVALID_HANDLE; }
        return *this;
    }
};
using Texture = GpuHandle<bgfx::TextureHandle>;
using Framebuffer = GpuHandle<bgfx::FrameBufferHandle>;
using Program = GpuHandle<bgfx::ProgramHandle>;
using Uniform = GpuHandle<bgfx::UniformHandle>;
constexpr std::uint16_t ShadowView=1, WorldView=10, SpriteView=20, TransparentView=30, BloomView=40, BlurView=41, PostView=50;
constexpr std::uint16_t ShadowSize=1024;
constexpr std::uint64_t PointSampler = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
constexpr std::uint64_t LinearSampler = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
std::shared_ptr<Texture> texture(const Image& image, bool point = true,bool srgb = false) {
    const auto handle = bgfx::createTexture2D(static_cast<std::uint16_t>(image.width),static_cast<std::uint16_t>(image.height),
        false,1,bgfx::TextureFormat::RGBA8,(point ? PointSampler : LinearSampler)|(srgb?BGFX_TEXTURE_SRGB:0),
        bgfx::copy(image.rgba.data(),static_cast<std::uint32_t>(image.rgba.size())));
    if (!bgfx::isValid(handle)) throw std::runtime_error("Could not create GPU texture");
    return std::make_shared<Texture>(handle);
}
struct GpuPart {
    GpuHandle<bgfx::VertexBufferHandle> vertices;
    GpuHandle<bgfx::IndexBufferHandle> indices;
    std::shared_ptr<Texture> albedo, normal;
    glm::vec4 color{1};
    float cutoff = 0.01f;
    bool transparent = false, doubleSided = false;
    glm::vec3 center{0};
};
struct GpuMesh { std::vector<GpuPart> parts; };
struct GpuSprite { SpriteAnimation animation; std::shared_ptr<Texture> atlas; int width=0,height=0; };
struct SpriteBatch {
    std::shared_ptr<Texture> atlas;std::vector<MeshVertex> vertices;std::vector<std::uint32_t> indices;
    glm::vec3 center{0};bool blend=false,unlit=false,castShadow=true;
};
struct ScreenVertex { float x,y,z,u,v; };
}
struct WorldRenderer::Impl {
    std::filesystem::path root, shaderRoot;
    Camera camera;
    bgfx::VertexLayout meshLayout, screenLayout;
    Program world, shadow, bloom, post;
    std::map<std::string,Uniform> uniforms;
    std::shared_ptr<Texture> white, flatNormal;
    Texture lut;
    std::string lutPath;
    std::string failedLut;
    std::set<std::string> failedSprites;
    Framebuffer shadowBuffer, sceneBuffer, bloomBuffer, blurBuffer;
    bgfx::TextureHandle shadowColor = BGFX_INVALID_HANDLE, sceneColor = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle sceneDepth=BGFX_INVALID_HANDLE;
    bgfx::TextureHandle bloomColor = BGFX_INVALID_HANDLE, blurColor = BGFX_INVALID_HANDLE;
    int width=0,height=0;
    int pixelsPerUnit=32;
    AssetRegistry<GpuMesh> meshes;
    AssetRegistry<GpuSprite> sprites;
    std::map<std::string,Handle<GpuMesh>> meshPaths;
    std::map<std::string,Handle<GpuSprite>> spritePaths;
    Impl(std::filesystem::path folder,std::filesystem::path shaders,int ppu)
        : root(std::move(folder)), shaderRoot(std::move(shaders)),pixelsPerUnit(ppu),
          meshes([this](const auto& p) { return loadMesh(p); }), sprites([this](const auto& p) { return loadSprite(p); }) {
        meshLayout.begin().add(bgfx::Attrib::Position,3,bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal,3,bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0,2,bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0,4,bgfx::AttribType::Float).end();
        screenLayout.begin().add(bgfx::Attrib::Position,3,bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0,2,bgfx::AttribType::Float).end();
        white = texture(Image::solid(255,255,255)); flatNormal = texture(Image::solid(128,128,255));
        reloadPrograms();
        for (const auto* name : {"u_materialColor","u_materialParams","u_ambient","u_sunDirection","u_sunColor","u_cameraPos","u_fogColor","u_shadowParams","u_blur","u_post","u_postFlags","u_screen","u_dof"})
            uniforms.emplace(name,Uniform(bgfx::createUniform(name,bgfx::UniformType::Vec4)));
        uniforms.emplace("u_lightViewProj",Uniform(bgfx::createUniform("u_lightViewProj",bgfx::UniformType::Mat4)));
        for (const auto* name : {"u_lightPosRadius","u_lightColor"}) uniforms.emplace(name,Uniform(bgfx::createUniform(name,bgfx::UniformType::Vec4,8)));
        for (const auto* name : {"s_albedo","s_normal","s_shadow","s_scene","s_bloom","s_lut","s_depth"}) uniforms.emplace(name,Uniform(bgfx::createUniform(name,bgfx::UniformType::Sampler)));
        auto depth = bgfx::createTexture2D(ShadowSize,ShadowSize,false,1,bgfx::TextureFormat::D16,BGFX_TEXTURE_RT);
        shadowColor = bgfx::createTexture2D(ShadowSize,ShadowSize,false,1,bgfx::TextureFormat::R32F,BGFX_TEXTURE_RT | PointSampler);
        const bgfx::TextureHandle attachments[]{shadowColor,depth};
        shadowBuffer = Framebuffer(bgfx::createFrameBuffer(2,attachments,true));
        loadLut("");
    }
    void reloadPrograms() {
        Program nextWorld(loadProgram(shaderRoot,"vs_world","fs_world"));
        Program nextShadow(loadProgram(shaderRoot,"vs_shadow","fs_shadow"));
        Program nextBloom(loadProgram(shaderRoot,"vs_screen","fs_bloom"));
        Program nextPost(loadProgram(shaderRoot,"vs_screen","fs_post"));
        world=std::move(nextWorld); shadow=std::move(nextShadow); bloom=std::move(nextBloom); post=std::move(nextPost);
        log(LogLevel::Info,"World shaders reloaded");
    }
    void set(const char* name, const glm::vec4& value) { bgfx::setUniform(uniforms.at(name).value,glm::value_ptr(value)); }
    void bind(std::uint8_t slot,const char* name,bgfx::TextureHandle handle,std::uint32_t flags=UINT32_MAX) {
        bgfx::setTexture(slot,uniforms.at(name).value,handle,flags);
    }
    std::shared_ptr<GpuMesh> loadMesh(const std::filesystem::path& path) {
        const auto source = Mesh::load(path);
        auto result = std::make_shared<GpuMesh>();
        std::map<std::pair<const Image*,bool>,std::shared_ptr<Texture>> textures;
        auto upload = [&](const std::shared_ptr<Image>& image,const std::shared_ptr<Texture>& fallback,bool srgb) {
            if (!image) return fallback;
            auto& gpu = textures[{image.get(),srgb}]; if (!gpu) gpu = texture(*image,true,srgb); return gpu;
        };
        for (const auto& part : source.parts) {
            GpuPart gpu;
            gpu.vertices = GpuHandle(bgfx::createVertexBuffer(bgfx::copy(part.vertices.data(),static_cast<std::uint32_t>(part.vertices.size()*sizeof(MeshVertex))),meshLayout));
            gpu.indices = GpuHandle(bgfx::createIndexBuffer(bgfx::copy(part.indices.data(),static_cast<std::uint32_t>(part.indices.size()*4)),BGFX_BUFFER_INDEX32));
            gpu.albedo=upload(part.material.albedo,white,true); gpu.normal=upload(part.material.normal,flatNormal,false);
            gpu.color=part.material.color; gpu.cutoff=part.material.alphaCutoff;
            gpu.transparent=part.material.transparent; gpu.doubleSided=part.material.doubleSided;
            for (const auto& v : part.vertices) gpu.center += v.position;
            gpu.center /= static_cast<float>(part.vertices.size());
            if (!bgfx::isValid(gpu.vertices.value) || !bgfx::isValid(gpu.indices.value)) throw std::runtime_error("GPU mesh allocation failed");
            result->parts.push_back(std::move(gpu));
        }
        return result;
    }
    std::shared_ptr<GpuSprite> loadSprite(const std::filesystem::path& path) {
        auto result = std::make_shared<GpuSprite>(); result->animation=SpriteAnimation::load(path);
        const auto image=Image::load(path.parent_path()/result->animation.atlas);
        result->width=image.width; result->height=image.height; result->atlas=texture(image,true,true);
        for (const auto& [name,clip] : result->animation.clips) for (const auto& direction : clip.directions) for (const auto& frame : direction)
            if (frame.rect.x+frame.rect.z > image.width || frame.rect.y+frame.rect.w > image.height) throw std::runtime_error("Sprite frame outside atlas: " + name);
        return result;
    }
    void loadLut(const std::string& path) {
        constexpr int size=16;
        std::vector<std::uint8_t> bytes(size*size*size*4);
        std::vector<glm::vec3> source;
        int inputSize=size;
        if (!path.empty()) {
            std::ifstream stream(root/path); if (!stream) throw std::runtime_error("Cannot open LUT " + path);
            std::string line;
            while (std::getline(stream,line)) {
                std::istringstream values(line); std::string token; values>>token;
                if (token.empty() || token[0]=='#' || token=="TITLE" || token=="DOMAIN_MIN" || token=="DOMAIN_MAX") continue;
                if (token=="LUT_3D_SIZE") { values>>inputSize; if (inputSize<2 || inputSize>64) throw std::runtime_error("LUT size must be 2..64"); continue; }
                try { glm::vec3 color; color.r=std::stof(token); if (values>>color.g>>color.b) source.push_back(color); }
                catch (...) { throw std::runtime_error("Invalid .cube LUT line"); }
            }
            if (source.size()!=static_cast<std::size_t>(inputSize*inputSize*inputSize)) throw std::runtime_error("Incomplete .cube LUT");
        }
        for (int b=0;b<size;++b) for (int g=0;g<size;++g) for (int r=0;r<size;++r) {
            glm::vec3 color=glm::vec3(r,g,b)/float(size-1);
            if (!source.empty()) {
                const glm::vec3 coordinate=color*float(inputSize-1); const glm::ivec3 lo=glm::ivec3(coordinate), hi=glm::min(lo+1,glm::ivec3(inputSize-1));
                const glm::vec3 f=glm::fract(coordinate);
                auto at=[&](int x,int y,int z) { return source[(z*inputSize+y)*inputSize+x]; };
                color=glm::mix(glm::mix(glm::mix(at(lo.x,lo.y,lo.z),at(hi.x,lo.y,lo.z),f.x),glm::mix(at(lo.x,hi.y,lo.z),at(hi.x,hi.y,lo.z),f.x),f.y),
                    glm::mix(glm::mix(at(lo.x,lo.y,hi.z),at(hi.x,lo.y,hi.z),f.x),glm::mix(at(lo.x,hi.y,hi.z),at(hi.x,hi.y,hi.z),f.x),f.y),f.z);
            }
            const int i=((b*size+g)*size+r)*4;
            for (int c=0;c<3;++c) bytes[i+c]=static_cast<std::uint8_t>(glm::clamp(color[c],0.0f,1.0f)*255); bytes[i+3]=255;
        }
        Texture next(bgfx::createTexture3D(size,size,size,false,bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP|BGFX_SAMPLER_W_CLAMP,bgfx::copy(bytes.data(),static_cast<std::uint32_t>(bytes.size()))));
        if (!bgfx::isValid(next.value)) throw std::runtime_error("Could not create color LUT");
        lut=std::move(next); lutPath=path;
    }
    void resize(int w,int h) {
        if (w==width && h==height) return;
        width=w; height=h;
        auto color=bgfx::createTexture2D(static_cast<std::uint16_t>(w),static_cast<std::uint16_t>(h),false,1,bgfx::TextureFormat::RGBA16F,BGFX_TEXTURE_RT|LinearSampler);
        auto depth=bgfx::createTexture2D(static_cast<std::uint16_t>(w),static_cast<std::uint16_t>(h),false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT|PointSampler);
        const bgfx::TextureHandle attachments[]{color,depth};
        sceneBuffer=Framebuffer(bgfx::createFrameBuffer(2,attachments,true)); sceneColor=color;sceneDepth=depth;
        const auto bw=static_cast<std::uint16_t>(std::max(1,w/4)), bh=static_cast<std::uint16_t>(std::max(1,h/4));
        bloomBuffer=Framebuffer(bgfx::createFrameBuffer(bw,bh,bgfx::TextureFormat::RGBA16F,BGFX_TEXTURE_RT|LinearSampler));
        blurBuffer=Framebuffer(bgfx::createFrameBuffer(bw,bh,bgfx::TextureFormat::RGBA16F,BGFX_TEXTURE_RT|LinearSampler));
        bloomColor=bgfx::getTexture(bloomBuffer.value); blurColor=bgfx::getTexture(blurBuffer.value);
    }
    void screen(std::uint16_t view,bgfx::ProgramHandle program) {
        const bool flip=bgfx::getCaps()->originBottomLeft;
        const ScreenVertex vertices[]{{-1,-1,0,0,flip?0.0f:1.0f},{3,-1,0,2,flip?0.0f:1.0f},{-1,3,0,0,flip?2.0f:-1.0f}};
        bgfx::TransientVertexBuffer vb;
        if (bgfx::getAvailTransientVertexBuffer(3,screenLayout)<3) return;
        bgfx::allocTransientVertexBuffer(&vb,3,screenLayout); std::memcpy(vb.data,vertices,sizeof(vertices));
        bgfx::setVertexBuffer(0,&vb); bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A); bgfx::submit(view,program);
    }
    void material(const std::shared_ptr<Texture>& albedo,const std::shared_ptr<Texture>& normal,glm::vec4 color,float cutoff,float rim,bool unlit=false) {
        bind(0,"s_albedo",albedo->value); bind(1,"s_normal",normal->value); bind(2,"s_shadow",shadowColor);
        set("u_materialColor",color); set("u_materialParams",{cutoff,normal!=flatNormal?1.0f:0.0f,rim,unlit?1.0f:0.0f});
    }
    void lighting(const MapData& map,const glm::mat4& lightVP,bool shadows) {
        bgfx::setUniform(uniforms.at("u_lightViewProj").value,glm::value_ptr(lightVP));
        set("u_ambient",{map.ambient,1}); set("u_sunDirection",{glm::normalize(map.sunDirection),0});
        set("u_sunColor",{map.sunColor,1}); set("u_cameraPos",{camera.position(),1}); set("u_fogColor",{map.fogColor,map.fogDensity});
        set("u_shadowParams",{1.0f/ShadowSize,bgfx::getCaps()->homogeneousDepth?0.5f:1.0f,bgfx::getCaps()->originBottomLeft?1.0f:-1.0f,shadows?1.0f:0.0f});
        std::array<glm::vec4,8> positions{},colors{};
        for (std::size_t i=0;i<std::min<std::size_t>(8,map.lights.size());++i) {
            positions[i]={map.lights[i].position,map.lights[i].radius}; colors[i]={map.lights[i].color*map.lights[i].intensity,1};
        }
        bgfx::setUniform(uniforms.at("u_lightPosRadius").value,positions.data(),8);
        bgfx::setUniform(uniforms.at("u_lightColor").value,colors.data(),8);
    }
};
WorldRenderer::WorldRenderer(std::filesystem::path root,std::filesystem::path shaders,int ppu) : impl_(std::make_unique<Impl>(std::move(root),std::move(shaders),ppu)) {}
WorldRenderer::~WorldRenderer()=default;
Camera& WorldRenderer::camera() { return impl_->camera; }
void WorldRenderer::updateCamera(const World& world,double dt,bool snap) { impl_->camera.update(world.playerPosition(),world.map().cameraAt(world.playerPosition()),dt,snap); }
void WorldRenderer::render(World& field,double alpha,int width,int height) {
    auto& p=*impl_; auto& map=field.map(); p.resize(width,height);
    if (p.lutPath!=map.post.lut && p.failedLut!=map.post.lut) {
        try {p.loadLut(map.post.lut);p.failedLut.clear();}
        catch (const std::exception& error) {p.failedLut=map.post.lut;log(LogLevel::Error,error.what());}
    }
    const auto path=p.root/"maps"/map.id/map.geometry;
    const auto key=path.generic_string();
    if (!p.meshPaths.contains(key)) p.meshPaths[key]=p.meshes.load(path);
    const auto mesh=p.meshes.get(p.meshPaths.at(key));
    const auto view=p.camera.view(alpha), projection=p.camera.projection(float(width)/height,bgfx::getCaps()->homogeneousDepth);
    const auto target=p.camera.target();
    const auto sun=glm::normalize(map.sunDirection);
    const auto lightView=glm::lookAtRH(target-sun*30.0f,target,std::abs(sun.y)>.99f?glm::vec3(0,0,1):glm::vec3(0,1,0));
    const auto lightProjection=bgfx::getCaps()->homogeneousDepth ? glm::orthoRH_NO(-16.0f,16.0f,-16.0f,16.0f,0.1f,70.0f) : glm::orthoRH_ZO(-16.0f,16.0f,-16.0f,16.0f,0.1f,70.0f);
    const auto lightVP=lightProjection*lightView;
    bgfx::setViewName(ShadowView,"Sun shadow"); bgfx::setViewFrameBuffer(ShadowView,p.shadowBuffer.value);
    bgfx::setViewRect(ShadowView,0,0,ShadowSize,ShadowSize); bgfx::setViewTransform(ShadowView,glm::value_ptr(lightView),glm::value_ptr(lightProjection));
    bgfx::setViewClear(ShadowView,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,0xffffffff,1); bgfx::touch(ShadowView);
    for (auto id : {WorldView,SpriteView,TransparentView}) {
        bgfx::setViewFrameBuffer(id,p.sceneBuffer.value); bgfx::setViewRect(id,0,0,static_cast<std::uint16_t>(width),static_cast<std::uint16_t>(height));
        bgfx::setViewTransform(id,glm::value_ptr(view),glm::value_ptr(projection));
    }
    bgfx::setViewName(WorldView,"World opaque"); bgfx::setViewName(SpriteView,"Billboard sprites"); bgfx::setViewName(TransparentView,"Transparent effects");
    bgfx::setViewMode(TransparentView,bgfx::ViewMode::Sequential);
    bgfx::setViewClear(WorldView,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,UINT32_C(0x202d40ff),1.0f,0); bgfx::touch(WorldView);
    const glm::mat4 identity(1);
    std::vector<const GpuPart*> transparent;
    for (const auto& part : mesh->parts) {
        if (part.transparent) { transparent.push_back(&part); continue; }
        auto geometry=[&] { bgfx::setTransform(glm::value_ptr(identity)); bgfx::setVertexBuffer(0,part.vertices.value); bgfx::setIndexBuffer(part.indices.value); };
        if (shadows) {
            geometry(); p.bind(0,"s_albedo",part.albedo->value); p.set("u_materialParams",{part.cutoff,0,0,0});
            bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS); bgfx::submit(ShadowView,p.shadow.value);
        }
        geometry(); p.lighting(map,lightVP,shadows); p.material(part.albedo,part.normal,part.color,part.cutoff,0);
        bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS|BGFX_STATE_MSAA); bgfx::submit(WorldView,p.world.value);
    }
    std::map<std::uint32_t,SpriteBatch> batches;std::vector<SpriteBatch> alphaBatches;
    for (auto e : field.registry().view<Transform,SpriteRenderer>()) {
        const auto& sprite=field.registry().get<SpriteRenderer>(e); const auto& t=field.registry().get<Transform>(e);
        if (!sprite.visible || sprite.animation.empty()) continue;
        const auto spritePath=p.root/sprite.animation; const auto spriteKey=spritePath.generic_string();
        if (p.failedSprites.contains(spriteKey)) continue;
        if (!p.spritePaths.contains(spriteKey)) {
            try {p.spritePaths[spriteKey]=p.sprites.load(spritePath);}
            catch (const std::exception& error) {p.failedSprites.insert(spriteKey);log(LogLevel::Error,error.what());continue;}
        }
        const auto data=p.sprites.get(p.spritePaths.at(spriteKey));
        const auto& frame=data->animation.frame(sprite.clip,t.facing,p.camera.yaw(),sprite.elapsed);
        SpriteBatch* selected=nullptr;
        if (sprite.blend) {alphaBatches.emplace_back();selected=&alphaBatches.back();}
        else selected=&batches[(std::uint32_t(data->atlas->value.idx)<<2)|(sprite.unlit?1u:0u)|(sprite.castShadow?2u:0u)];
        auto& batch=*selected;batch.atlas=data->atlas;batch.blend=sprite.blend;batch.unlit=sprite.unlit;batch.castShadow=sprite.castShadow;
        const auto position=glm::mix(t.previous,t.position,static_cast<float>(alpha)); const auto right=p.camera.right();
        batch.center=position;
        const float w=float(frame.rect.z)/p.pixelsPerUnit*sprite.scale,h=float(frame.rect.w)/p.pixelsPerUnit*sprite.scale;
        const auto left=position-right*(w*data->animation.pivot.x)+glm::vec3(0,h*(data->animation.pivot.y-1),0);
        const float u0=float(frame.rect.x)/data->width,v0=float(frame.rect.y)/data->height,u1=float(frame.rect.x+frame.rect.z)/data->width,v1=float(frame.rect.y+frame.rect.w)/data->height;
        const auto base=static_cast<std::uint32_t>(batch.vertices.size()); const auto normal=glm::cross(right,glm::vec3(0,1,0));
        batch.vertices.push_back({left,normal,{u0,v1},sprite.tint}); batch.vertices.push_back({left+right*w,normal,{u1,v1},sprite.tint});
        batch.vertices.push_back({left+right*w+glm::vec3(0,h,0),normal,{u1,v0},sprite.tint}); batch.vertices.push_back({left+glm::vec3(0,h,0),normal,{u0,v0},sprite.tint});
        for (auto index : {0u,1u,2u,0u,2u,3u}) batch.indices.push_back(base+index);
    }
    auto drawBatch=[&](const SpriteBatch& batch) {
        const auto vc=static_cast<std::uint32_t>(batch.vertices.size()),ic=static_cast<std::uint32_t>(batch.indices.size());
        if (bgfx::getAvailTransientVertexBuffer(vc,p.meshLayout)<vc || bgfx::getAvailTransientIndexBuffer(ic,true)<ic) return;
        bgfx::TransientVertexBuffer vb; bgfx::TransientIndexBuffer ib;
        bgfx::allocTransientVertexBuffer(&vb,vc,p.meshLayout); bgfx::allocTransientIndexBuffer(&ib,ic,true);
        std::memcpy(vb.data,batch.vertices.data(),vc*sizeof(MeshVertex)); std::memcpy(ib.data,batch.indices.data(),ic*4);
        auto geometry=[&] { bgfx::setTransform(glm::value_ptr(identity)); bgfx::setVertexBuffer(0,&vb); bgfx::setIndexBuffer(&ib); };
        if (shadows && batch.castShadow && !batch.blend) {
            geometry(); p.bind(0,"s_albedo",batch.atlas->value); p.set("u_materialParams",{0.5f,0,0,0});
            bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS); bgfx::submit(ShadowView,p.shadow.value);
        }
        geometry(); p.lighting(map,lightVP,shadows); p.material(batch.atlas,p.flatNormal,{1,1,1,1},batch.blend?0.001f:0.5f,batch.unlit?0:0.08f,batch.unlit);
        bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_DEPTH_TEST_LESS|BGFX_STATE_MSAA|(batch.blend?BGFX_STATE_BLEND_ALPHA:BGFX_STATE_WRITE_Z));
        bgfx::submit(batch.blend?TransparentView:SpriteView,p.world.value);
    };
    for (const auto& [atlas,batch]:batches) {(void)atlas;drawBatch(batch);}
    struct AlphaDraw {glm::vec3 center;const GpuPart* mesh=nullptr;const SpriteBatch* sprite=nullptr;};
    std::vector<AlphaDraw> alphaDraws;
    for (const auto* part:transparent) alphaDraws.push_back({part->center,part,nullptr});
    for (const auto& batch:alphaBatches) alphaDraws.push_back({batch.center,nullptr,&batch});
    std::sort(alphaDraws.begin(),alphaDraws.end(),[&](const auto& a,const auto& b) {return glm::length(a.center-p.camera.position())>glm::length(b.center-p.camera.position());});
    for (const auto& draw:alphaDraws) {
        if (draw.sprite) drawBatch(*draw.sprite);
        else {
            const auto* part=draw.mesh;
            bgfx::setTransform(glm::value_ptr(identity));bgfx::setVertexBuffer(0,part->vertices.value);bgfx::setIndexBuffer(part->indices.value);
            p.lighting(map,lightVP,shadows);p.material(part->albedo,part->normal,part->color,part->cutoff,0);
            bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_DEPTH_TEST_LESS|BGFX_STATE_BLEND_ALPHA);bgfx::submit(TransparentView,p.world.value);
        }
    }
    const auto bw=static_cast<std::uint16_t>(std::max(1,width/4)),bh=static_cast<std::uint16_t>(std::max(1,height/4));
    bgfx::setViewName(BloomView,"Bloom extraction"); bgfx::setViewRect(BloomView,0,0,bw,bh); bgfx::setViewFrameBuffer(BloomView,p.bloomBuffer.value);
    p.bind(0,"s_scene",p.sceneColor); p.set("u_blur",{1.0f/width,0,1,0}); p.screen(BloomView,p.bloom.value);
    bgfx::setViewName(BlurView,"Bloom blur"); bgfx::setViewRect(BlurView,0,0,bw,bh); bgfx::setViewFrameBuffer(BlurView,p.blurBuffer.value);
    p.bind(0,"s_scene",p.bloomColor); p.set("u_blur",{0,1.0f/bh,0,0}); p.screen(BlurView,p.bloom.value);
    bgfx::setViewName(PostView,"Tilt-shift, LUT and composite"); bgfx::setViewRect(PostView,0,0,static_cast<std::uint16_t>(width),static_cast<std::uint16_t>(height));
    p.bind(0,"s_scene",p.sceneColor); p.bind(1,"s_bloom",p.blurColor); p.bind(2,"s_lut",p.lut.value);
    p.bind(3,"s_depth",p.sceneDepth);
    p.set("u_dof",{.1f,150.0f,map.post.focusDistance>0?map.post.focusDistance:p.camera.settings().distance,map.post.aperture});
    p.set("u_post",{map.post.bloomStrength,map.post.focus,map.post.focusRange,map.post.exposure});
    p.set("u_postFlags",{map.post.bloom?1.0f:0.0f,map.post.depthOfField?1.0f:0.0f,map.post.vignette?1.0f:0.0f,map.post.colorGrade?1.0f:0.0f});
    p.set("u_screen",{1.0f/width,1.0f/height,0,0}); p.screen(PostView,p.post.value);
}
void WorldRenderer::reload(const std::filesystem::path& path) {
    auto& p=*impl_;
    p.failedSprites.erase(path.generic_string());p.failedLut.clear();
    const auto extension=path.extension().string();
    if (extension==".sc" || (extension==".bin" && path.parent_path().parent_path()==p.shaderRoot)) { p.reloadPrograms(); return; }
    if (extension==".cube") { p.loadLut(p.lutPath); return; }
    if (extension==".json") p.sprites.reloadPath(path);
    if (extension==".png" || extension==".jpg") for (const auto& [key,handle] : p.spritePaths) { (void)key; p.sprites.reload(handle); }
    if (extension==".glb" || extension==".gltf" || extension==".bin" || extension==".png" || extension==".jpg")
        for (const auto& [key,handle] : p.meshPaths) { (void)key; p.meshes.reload(handle); }
}
}
