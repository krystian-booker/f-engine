#pragma once
#include "engine/core/AssetCache.h"
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
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

struct GpuPart {
    VertexBuffer vertices;
    IndexBuffer indices;
    bgfx::TextureHandle albedo = BGFX_INVALID_HANDLE, normal = BGFX_INVALID_HANDLE;
    Material material;
    int node = 0;
    glm::vec3 min{0}, max{0}; // bounds (world space for baked chunks, node space otherwise)
};

// GPU copy of a model. Static map geometry is baked into world space and
// split into chunks so each chunk can cull and pick its nearest lights.
struct GpuModel {
    std::shared_ptr<const Model> source;
    std::vector<GpuPart> parts;
    bool baked = false;
};

const bgfx::VertexLayout& meshLayout();
bgfx::ProgramHandle loadProgram(const std::filesystem::path& root, const char* vertex, const char* fragment);

class GpuAssets {
public:
    explicit GpuAssets(AssetCache& assets);
    [[nodiscard]] AssetCache& cpu() { return assets_; }
    // Drops GPU copies when CPU assets were reloaded.
    void sync();
    bgfx::TextureHandle image(const std::string& path, bool linear = false);
    bgfx::TextureHandle fontPage(const Font* font, int page);
    const GpuModel* model(const std::string& path, bool bakeChunks);
    // 16^3 colour grading texture from a .cube file ("" = identity).
    bgfx::TextureHandle lut(const std::string& path);
    [[nodiscard]] bgfx::TextureHandle white() const { return white_.get(); }
    [[nodiscard]] bgfx::TextureHandle flatNormal() const { return flatNormal_.get(); }
    [[nodiscard]] glm::ivec2 imageSize(const std::string& path);

private:
    bgfx::TextureHandle upload(const Image& image, bool linear, bool srgb);
    AssetCache& assets_;
    std::uint64_t generation_ = 0;
    Texture white_, flatNormal_;
    std::map<std::pair<std::string, bool>, Texture> images_;
    std::map<std::pair<const Font*, int>, std::pair<std::uint32_t, Texture>> fontPages_;
    std::map<std::pair<std::string, bool>, std::unique_ptr<GpuModel>> models_;
    std::map<const Image*, Texture> modelTextures_;
    std::map<std::string, Texture> luts_;
    std::set<std::string> failed_;
};
} // namespace fe
