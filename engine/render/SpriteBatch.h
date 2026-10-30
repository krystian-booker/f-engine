#pragma once
#include "engine/render/RenderCommon.h"
#include <unordered_map>
#include <vector>

namespace fe {
class SceneLighting;

// Sprite and particle quads of one world, uploaded once per frame into a
// single transient vertex buffer and drawn in as few calls as possible:
// consecutive quads sharing textures, shader and blend mode go out in one draw.
class SpriteBatch {
public:
    struct Material {
        bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE, palette = BGFX_INVALID_HANDLE;
        // Images laid out like the atlas; invalid when the sheet has none.
        bgfx::TextureHandle normal = BGFX_INVALID_HANDLE, emissive = BGFX_INVALID_HANDLE;
        glm::vec2 texel{0};       // 1 / atlas size, for outlines
        float outlineWidth = 1;   // atlas texels the outline reaches out
        bool paletteSwap = false;
        // A game sprite shader for the opaque and transparent passes; invalid = the engine's.
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        // Its shadow variant, for the shadow and silhouette passes; invalid = the engine's.
        bgfx::ProgramHandle shadowProgram = BGFX_INVALID_HANDLE;
        // The entity's u_params for those shaders; null = all zero. Quads
        // with different parameters cannot share a draw.
        const ShaderParams* params = nullptr;
        bool operator==(const Material& other) const {
            return atlas.idx == other.atlas.idx && palette.idx == other.palette.idx &&
                   normal.idx == other.normal.idx && emissive.idx == other.emissive.idx &&
                   texel == other.texel && outlineWidth == other.outlineWidth &&
                   paletteSwap == other.paletteSwap &&
                   program.idx == other.program.idx && shadowProgram.idx == other.shadowProgram.idx &&
                   params == other.params;
        }
    };
    struct MaterialHash {
        std::size_t operator()(const Material& m) const;
    };
    struct Quad {
        int material = 0;
        glm::vec3 min{0}, max{0};
        glm::vec3 centre{0};
        BlendMode blend = BlendMode::Opaque;
        bool visible = true, castShadow = false;
        bool silhouette = false; // drawn only where scene geometry hides it
    };
    enum class Pass {
        Shadow,      // depth only, alpha cutout
        Opaque,      // depth tested and written, alpha cutout
        Transparent, // each quad's blend mode, depth tested, not written
        Silhouette,  // the outline colour where something nearer was drawn
    };

    void clear();
    // Adds a quad; `vertices` are bottom-left, bottom-right, top-right, top-left.
    void add(const Material& material, const std::array<SpriteVertex, 4>& vertices, const Quad& settings);
    [[nodiscard]] const std::vector<Quad>& quads() const { return quads_; }
    // Copies every quad into this frame's transient vertex buffer; false when it is full.
    bool upload();
    struct Context {
        const Uniforms& uniforms;
        const SceneLighting* lighting = nullptr; // null for shadow passes
        bgfx::TextureHandle white = BGFX_INVALID_HANDLE, flatNormal = BGFX_INVALID_HANDLE;
        glm::vec4 info{0}; // u_info for game sprite shaders
    };
    // Draws `quads` (indices into quads()) in order, one call per run of
    // quads with the same material and blend mode. The opaque and transparent
    // passes use a material's own program when it has one, `program` otherwise.
    void draw(std::uint16_t view, const std::vector<int>& quads, bgfx::ProgramHandle program, Pass pass,
              const Context& context, std::uint32_t& calls) const;

private:
    std::vector<Material> materials_;
    std::unordered_map<Material, int, MaterialHash> materialIndex_;
    std::vector<Quad> quads_;
    std::vector<SpriteVertex> vertices_;
    mutable std::vector<std::uint32_t> indices_;
    bgfx::TransientVertexBuffer buffer_{};
    bool uploaded_ = false;
};
} // namespace fe
