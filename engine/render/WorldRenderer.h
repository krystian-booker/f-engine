#pragma once
#include "engine/render/CustomShaders.h"
#include "engine/render/OverlayGeometry.h"
#include "engine/render/SceneLighting.h"
#include "engine/render/SpriteBatch.h"
#include "engine/render/SpriteGeometry.h"
#include <filesystem>
#include <unordered_map>

namespace fe {
// Draws one world's 3D scene into a SceneTarget: cascaded sun shadows and
// point/spot light shadow tiles, sky and backdrop cards, map chunks and mesh
// entities (opaque mesh parts drawn more than once, in the main pass and in
// each shadow view, go out as one instanced draw), sprite silhouettes, opaque
// sprites and particles in batches, grid overlays, then blended geometry,
// sprites and particles by blend pass and back to front. Sprites, particle
// sheets and meshes keep their resolved GPU assets and shaders in render-side
// caches (components and a registry context of the world) checked against
// GpuAssets::revision(), so a frame looks nothing up by name per entity or
// particle. Post-processing is PostPass's job.
class WorldRenderer {
public:
    WorldRenderer(GpuAssets& gpu, const Uniforms& uniforms, CustomShaders& custom,
                  const std::filesystem::path& shaderRoot, int pixelsPerUnit);
    // Loads every program first, so a failure keeps the current ones.
    void reload(const std::filesystem::path& shaderRoot);
    // `slot` is the world's position in this frame's draw list.
    void render(World& world, std::size_t slot, double alpha, double time, float aspect,
                const SceneViews& views, SceneTarget& target, std::uint32_t& calls);

private:
    struct PartDraw {
        const GpuPart* part = nullptr;
        glm::mat4 transform{1}, normal{1};
        glm::vec4 tint{1};
        BlendMode blend = BlendMode::Opaque;
        int shader = -1; // CustomShaders slot; -1 = the engine's
    };
    struct Caster {
        const GpuPart* part = nullptr;
        glm::mat4 transform{1};
        glm::vec3 min{0}, max{0};
    };
    struct Transparent {
        TransparentKey key;
        bool sprite = false;
        int index = 0; // into parts_ or the sprite batch's quads
    };

    void setupViews(World& world, const SceneViews& views, const SceneTarget& target, const glm::mat4& view,
                    const glm::mat4& projection) const;
    void drawSky(World& world, glm::vec3 focus);
    // `shader`: a CustomShaders slot for every part (-1 = each material's own).
    void addPart(const GpuPart& part, const glm::mat4& transform, const glm::vec4& tint, bool castShadow,
                 std::optional<BlendMode> blend, int shader);
    // Binds a part's buffers, textures and uniforms (u_material without the tint when `instanced`).
    void bindPart(const PartDraw& draw, std::uint64_t state, bool instanced);
    [[nodiscard]] bgfx::ProgramHandle partProgram(const PartDraw& draw, bool instanced);
    void submitPart(std::uint16_t view, const PartDraw& draw, std::uint64_t state);
    // Opaque parts, one instanced draw per run of draws of the same part and shader.
    void drawOpaqueParts();
    // Shadow casters, culled and instanced per shadow view.
    void drawCasters();
    void castShadow(std::uint16_t view, const std::vector<int>& casters);
    void drawMeshes(World& world, double alpha);
    void collectSprites(World& world, double alpha);
    void addDecal(World& world, const SpriteBatch::Material& material, const SpriteVertex& base, int layer,
                  glm::vec3 bottomLeft, glm::vec2 size, glm::vec2 uv0, glm::vec2 uv1, BlendMode blend);
    void collectParticles(World& world, double alpha);
    void drawSprites();
    void drawOverlays(World& world);
    void submitOverlay(float pulse, bool border);
    void drawTransparent();
    // Distort sprites and particles: screen offsets for PostPass, not colour.
    void addDistortion(bgfx::TextureHandle atlas, glm::vec2 texel, std::array<SpriteVertex, 4> quad,
                       float strength);
    void drawDistortion();

    GpuAssets& gpu_;
    const Uniforms& uniforms_;
    CustomShaders& custom_;
    int pixelsPerUnit_;
    Program world_, worldInstanced_, sprite_, water_, shadow_, shadowInstanced_, sky_, overlay_, distort_;
    Uniform distortUniform_;
    ShadowAtlas atlas_;
    std::vector<std::unique_ptr<SceneLighting>> lightingSlots_;
    SpriteBatch sprites_;
    OverlayMesh overlayMesh_;
    std::vector<WorldVertex> overlayVertices_;
    std::vector<PartDraw> parts_; // blended parts, drawn back to front
    std::vector<PartDraw> opaque_;
    std::vector<Caster> casters_;
    std::vector<int> visible_, order_;
    std::vector<std::pair<const GpuPart*, int>> keys_;
    std::unordered_map<const GpuPart*, int> partShaders_; // material shader slots, for GpuAssets::revision()
    std::uint64_t partShadersRevision_ = 0;
    std::vector<Transparent> transparent_;
    std::vector<int> quadList_;
    std::vector<glm::mat4> pose_;
    struct DistortQuad {
        bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
        glm::vec2 texel{0};
        std::array<SpriteVertex, 4> vertices;
    };
    std::vector<DistortQuad> distortQuads_;
    std::vector<SpriteVertex> distortVertices_;
    std::vector<DecalQuad> decal_;

    // Per-render state
    const SceneLighting* lighting_ = nullptr;
    const Environment* environment_ = nullptr;
    const SceneViews* views_ = nullptr;
    SceneTarget* target_ = nullptr;
    Frustum frustum_;
    glm::mat4 viewProjection_{1};
    glm::vec3 cameraPosition_{0}, cameraRight_{1, 0, 0}, cameraUp_{0, 1, 0}, spriteNormal_{0, 1, 0};
    float nearPlane_ = 0.1f, farPlane_ = 100;
    double time_ = 0;
    glm::vec4 info_{0}; // u_info of game shaders: 1 / target size, seconds, origin bottom-left
    bool opaqueCopied_ = false;
    std::uint32_t* calls_ = nullptr;
};
} // namespace fe
