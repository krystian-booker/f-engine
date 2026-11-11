#pragma once
#include "engine/core/AssetCache.h"
#include <array>
#include <bgfx/bgfx.h>
#include <filesystem>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fe {
// Owns a bgfx handle and destroys it.
template<class H> class GpuHandle {
public:
    GpuHandle() = default;
    explicit GpuHandle(H handle) : value_(handle) {}
    ~GpuHandle() { reset(); }
    GpuHandle(const GpuHandle&) = delete;
    GpuHandle& operator=(const GpuHandle&) = delete;
    GpuHandle(GpuHandle&& other) noexcept : value_(other.value_) { other.value_ = BGFX_INVALID_HANDLE; }
    GpuHandle& operator=(GpuHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = BGFX_INVALID_HANDLE;
        }
        return *this;
    }
    void reset() {
        if (bgfx::isValid(value_)) {
            bgfx::destroy(value_);
        }
        value_ = BGFX_INVALID_HANDLE;
    }
    [[nodiscard]] H get() const { return value_; }
    [[nodiscard]] bool valid() const { return bgfx::isValid(value_); }

private:
    H value_ = BGFX_INVALID_HANDLE;
};

using Texture = GpuHandle<bgfx::TextureHandle>;
using Program = GpuHandle<bgfx::ProgramHandle>;
using Uniform = GpuHandle<bgfx::UniformHandle>;
using Framebuffer = GpuHandle<bgfx::FrameBufferHandle>;
using VertexBuffer = GpuHandle<bgfx::VertexBufferHandle>;
using IndexBuffer = GpuHandle<bgfx::IndexBufferHandle>;

// How an image file is sampled. Colour that is lit or blended in the linear
// 3D scene is decoded from sRGB; UI images and palette indices keep their
// stored values (the canvas composes in sRGB space).
enum class TextureUse {
    Ui,          // canvas, point filtered
    UiSmooth,    // canvas, bilinear
    Sprite,      // sRGB colour, point filtered, no mips
    SpriteIndex, // raw values (palette swap indices), point filtered
    Scenery,     // sRGB colour, trilinear mips, clamped (backdrops, soft particles)
    Panorama,    // sRGB colour, bilinear, wraps horizontally
};
// Scenery and panoramas load a cooked `.ktx` next to the image when there is
// one in a format the GPU samples (see `fengine_assetc cook`).
constexpr std::size_t TextureUseCount = 6;

// A cooked texture used in place of an image file, and when it was written.
struct CookedTexture {
    std::filesystem::path file;
    std::filesystem::file_time_type modified;
    bool operator==(const CookedTexture&) const = default;
};

// A model texture, shared by every part of every model that names the same
// image file with the same colour space and mip setting (a tileset used by
// ten props uploads once). Parts hold it, so it lives while any does.
struct ModelTexture {
    Texture texture;
    CookedTexture cooked;
    std::filesystem::path original;  // the image file a cooked texture may replace
    std::shared_ptr<Image> embedded; // a .glb's image, held so its address stays unique
    bool stale = false;              // its cooked file changed: users re-upload
};

struct GpuPart {
    VertexBuffer vertices;
    IndexBuffer indices;
    bgfx::TextureHandle albedo = BGFX_INVALID_HANDLE, normal = BGFX_INVALID_HANDLE,
                        emission = BGFX_INVALID_HANDLE;
    std::array<std::shared_ptr<const ModelTexture>, 3> textures; // what the handles above come from
    Material material;
    int node = 0;
    glm::vec3 min{0}, max{0}; // bounds: world space for baked map chunks, node space otherwise
    // A skinned part of a mesh entity's model: the skin and the ModelPart
    // (index into the source's parts) its vertices are skinned from each
    // frame, and those vertices packed at rest (the bytes of WorldVertex,
    // RenderCommon.h), of which a frame rewrites only position, normal and
    // tangent. -1 and empty otherwise.
    int skin = -1, sourcePart = -1;
    std::vector<std::uint8_t> packed;
};

// GPU copy of a model. Static map geometry is baked into world space and
// split into chunks so each chunk can be culled on its own.
struct GpuModel {
    std::shared_ptr<const Model> source;
    std::vector<GpuPart> parts;
};

// A sprite animation with its atlas (and optional normal and emissive
// images laid out like it) resolved.
struct SpriteSheet {
    std::shared_ptr<const SpriteAnimation> animation;
    std::shared_ptr<const Image> atlas, normalImage, emissiveImage;
    std::string atlasPath, normalPath, emissivePath;
    glm::ivec2 size{0};
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE; // TextureUse::Sprite
    // Invalid when the sheet has none.
    bgfx::TextureHandle normal = BGFX_INVALID_HANDLE, emissive = BGFX_INVALID_HANDLE;
};

std::uint32_t samplerFlags(const TextureSampler& sampler, bool anisotropic);
// Compiled shaders live in <root>/<shaderBackend()>/<name>.bin.
bgfx::ProgramHandle loadProgram(const std::filesystem::path& root, const char* vertex, const char* fragment);
bgfx::ProgramHandle loadProgram(const std::filesystem::path& vertex, const std::filesystem::path& fragment);
// A compiled shader's bytes, read through fe::readFile, so a game's shaders
// may be entries of its .pak; throws when it is missing or too short.
[[nodiscard]] std::string readShaderBinary(const std::filesystem::path& file);
// Whether compiled vertex and fragment shader binaries pair: the fragment
// shader's inputs are exactly the vertex shader's outputs.
[[nodiscard]] bool shaderInterfacesMatch(const std::string& vertex, const std::string& fragment);
// "metal", "dx11", "spirv" or "glsl" for the running renderer.
const char* shaderBackend();

class GpuAssets {
public:
    explicit GpuAssets(AssetCache& assets);
    [[nodiscard]] AssetCache& cpu() { return assets_; }
    // Drops the GPU copies of assets that were reloaded or evicted from the
    // asset cache (the CPU asset is gone) or whose LUT file or cooked texture
    // changed; the rest stay, and a model keeps its textures unless one of
    // its own cooked files changed. It does nothing (and touches no file)
    // unless the asset cache's generation or file changes moved since the
    // last call: files only change through the development file watcher.
    void sync();
    // Changes whenever sync() may have dropped or replaced something, so
    // handles cached elsewhere (on components) are valid while it is unchanged.
    // Values are unique across GpuAssets objects: a cache filled by another
    // (destroyed) one never matches.
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    bgfx::TextureHandle image(const std::string& path, TextureUse use);
    [[nodiscard]] glm::ivec2 imageSize(const std::string& path);
    // Null when the animation or its atlas is missing.
    const SpriteSheet* spriteSheet(const std::string& animationPath);
    bgfx::TextureHandle fontPage(const Font* font, int page);
    const GpuModel* model(const std::string& path, bool bakeChunks);
    // 16^3 colour grading texture from a .cube file ("" = identity).
    bgfx::TextureHandle lut(const std::string& path);
    [[nodiscard]] bgfx::TextureHandle white() const { return white_.get(); }
    [[nodiscard]] bgfx::TextureHandle flatNormal() const { return flatNormal_.get(); }

private:
    struct ImageEntry {
        std::shared_ptr<const Image> source;
        CookedTexture cooked;
        std::filesystem::path original; // the image file a cooked texture may replace
        Texture texture;
    };
    struct LutEntry {
        std::filesystem::path file;
        std::filesystem::file_time_type modified;
        Texture texture;
    };
    // Null when the material has no such image. `mips` false uploads the
    // image without a mip chain (and ignores a cooked file, which has one).
    std::shared_ptr<const ModelTexture> modelTexture(const ModelImage& image, bool srgb, bool mips);
    // Creates a texture from the cooked file next to `original` when it exists
    // and the GPU samples its format; records what was used in `cooked`.
    bool uploadCooked(const std::filesystem::path& original, bool srgb, std::uint64_t flags, Texture& texture,
                      CookedTexture& cooked);
    [[nodiscard]] static CookedTexture cookedState(const std::filesystem::path& original);
    AssetCache& assets_;
    std::uint64_t generation_ = 0, fileChanges_ = 0, revision_ = 0;
    Texture white_, flatNormal_;
    std::map<std::string, std::array<ImageEntry, TextureUseCount>, std::less<>> images_;
    std::unordered_map<std::string, SpriteSheet> sheets_;
    std::map<std::pair<const Font*, int>, std::pair<std::uint32_t, Texture>> fontPages_;
    std::map<std::pair<std::string, bool>, std::unique_ptr<GpuModel>> models_;
    // Keyed by image file (or an embedded image's identity), sRGB, mips.
    std::map<std::tuple<std::string, bool, bool>, std::shared_ptr<ModelTexture>> modelTextures_;
    std::map<std::string, LutEntry> luts_;
    std::set<std::string> failed_;
};
} // namespace fe
