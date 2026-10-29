#pragma once
#include "engine/render/LightGrid.h"
#include "engine/render/RenderCommon.h"
#include "engine/world/World.h"

namespace fe {
// The cascaded sun shadow map and the point and spot light shadow tiles,
// shared by every world of a frame: worlds draw one after another, so each
// overwrites them before its own scene samples them.
class ShadowAtlas {
public:
    static constexpr std::uint16_t Resolution = 2048; // per cascade
    ShadowAtlas();
    [[nodiscard]] bgfx::TextureHandle texture() const { return depth_.get(); }
    [[nodiscard]] bgfx::FrameBufferHandle framebuffer() const { return buffer_.get(); }
    [[nodiscard]] bgfx::TextureHandle lightTexture() const { return lightDepth_.get(); }
    [[nodiscard]] bgfx::FrameBufferHandle lightFramebuffer() const { return lightBuffer_.get(); }

private:
    Texture depth_, lightDepth_;
    Framebuffer buffer_, lightBuffer_;
};

// A light shadow tile rendered this frame: its view and the volume its casters must touch.
struct LightShadowTile {
    std::uint16_t view = 0;
    Frustum frustum;
};

// Everything one world's scene shaders read about light: environment
// constants, the fitted shadow cascades and the clustered light textures.
// One instance per world drawn in a frame, because texture updates all land
// before the frame renders.
class SceneLighting {
public:
    SceneLighting();
    // Fits the cascades, sets up the shadow views and bins this frame's lights.
    // `viewProjection` is the camera's, for skipping shadows of lights out of view.
    void prepare(World& world, double alpha, float aspect, const glm::mat4& viewProjection,
                 const SceneViews& views, const ShadowAtlas& atlas);
    // Uniforms and textures for one draw (samplers 2..4 and 7).
    void bind(const Uniforms& uniforms) const;
    [[nodiscard]] bool shadows() const { return shadows_; }
    [[nodiscard]] const std::array<Frustum, ShadowCascadeCount>& cascadeFrusta() const { return frusta_; }
    [[nodiscard]] const std::vector<LightShadowTile>& lightTiles() const { return lightTiles_; }

private:
    void prepareLightShadows(glm::vec3 focus, const Frustum& view, const SceneViews& views,
                             const ShadowAtlas& atlas);
    std::array<glm::vec4, Uniforms::EnvCount> env_{};
    std::array<glm::mat4, ShadowCascadeCount> cascades_{};
    std::array<Frustum, ShadowCascadeCount> frusta_;
    std::array<glm::mat4, LightShadowTiles> lightShadows_{};
    std::vector<LightShadowTile> lightTiles_;
    std::vector<LightShadowCandidate> shadowCandidates_;
    bgfx::TextureHandle atlas_ = BGFX_INVALID_HANDLE, lightAtlas_ = BGFX_INVALID_HANDLE;
    bool shadows_ = false;
    std::vector<SceneLight> sceneLights_;
    LightGrid grid_;
    Texture lights_, cells_;
    glm::ivec2 cellTextureSize_{0};
};
} // namespace fe
