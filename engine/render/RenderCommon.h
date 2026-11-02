#pragma once
#include "engine/render/DrawOrder.h"
#include "engine/render/GpuAssets.h"
#include "engine/render/ShadowCascades.h"
#include <array>
#include <glm/glm.hpp>

namespace fe {
constexpr std::uint32_t PointClamp = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                     BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
constexpr std::uint32_t LinearClamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
// Colour blends with straight alpha; alpha accumulates coverage, so a world's
// output composites correctly over the worlds drawn before it.
constexpr std::uint64_t AlphaBlend =
    BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                   BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
// Colour already multiplied by its alpha: light added past the coverage
// (bloom, a blurred foreground) lands on what is underneath instead of being
// scaled away. The post pass composites a world this way.
constexpr std::uint64_t PremultipliedBlend =
    BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
constexpr std::uint64_t OpaqueState =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
// Blend function of a blended draw. Scene alpha is coverage: additive and
// multiply leave it as it is.
std::uint64_t blendFunction(BlendMode mode);
// What the scene shaders' blendOutput() does with the colour (u_material[2].w, u_sprite[1].x).
float blendOutput(BlendMode mode);

// Map, mesh, backdrop and overlay vertex: 36 bytes. The normal and tangent
// (w = bitangent sign) are packed to bytes; the colour to half floats,
// because vertex colours above 1 make glowing geometry.
struct WorldVertex {
    glm::vec3 position{0};
    std::uint32_t normal = 0, tangent = 0;
    glm::vec2 uv{0};
    std::array<std::uint16_t, 4> color{}; // half floats
    static WorldVertex pack(glm::vec3 position, glm::vec3 normal, glm::vec2 uv, glm::vec4 color,
                            glm::vec4 tangent = glm::vec4(1, 0, 0, 1));
};
static_assert(sizeof(WorldVertex) == 36);
const bgfx::VertexLayout& worldLayout();

// Packs a unit vector to bytes (x, y, z * 0.5 + 0.5) and a sign to w (0 or 1).
std::uint32_t packDirection(glm::vec3 direction, float sign = 1);

// Sprite and particle vertex, positions already in world space.
struct SpriteVertex {
    glm::vec3 position{0};
    glm::vec2 uv{0};
    glm::vec4 tint{1};
    std::uint32_t flash = 0, outline = 0;  // rgba8; a silhouette quad carries its colour as the outline
    glm::vec4 probe{0};                    // light probe position, unlit flag
    glm::vec2 params{0};                   // palette row v, distortion strength (distort quads)
    std::uint32_t normal = 0, tangent = 0; // packDirection: shading normal; image +x in world space
};
const bgfx::VertexLayout& spriteLayout();

std::uint32_t packRgba8(glm::vec4 color);

struct Frustum {
    std::array<glm::vec4, 6> planes{}; // all zero: everything is visible
    Frustum() = default;
    explicit Frustum(const glm::mat4& viewProjection);
    [[nodiscard]] bool visible(glm::vec3 min, glm::vec3 max) const;
};

// Per-draw parameters of a game sprite or mesh shader (u_params, vec4[4]),
// set per entity from a script; all zero unless set.
using ShaderParams = std::array<glm::vec4, 4>;

// Every uniform handle the scene, post and screen passes use, created once.
struct Uniforms {
    Uniforms();
    Uniform env, cascades, lightShadows, material, normalMatrix, water, sprite, overlay, sky, post, present,
        info, params;
    Uniform albedo, normal, shadow, palette, lights, lightGrid, lightShadow, emissive, spriteNormal,
        spriteEmissive, sceneDepth, sceneColor, panorama, scene, bloom, lut, lut2, depth, frame;
    static constexpr std::uint16_t EnvCount = 10, CascadeCount = 3, MaterialCount = 3, WaterCount = 5,
                                   SpriteCount = 2, SkyCount = 5, PostCount = 4, PresentCount = 7,
                                   ParamsCount = 4;
};

// The bgfx views one world's scene uses, in execution order (PostPass takes its own).
struct SceneViews {
    static constexpr std::uint16_t Count = ShadowCascadeCount + LightShadowTiles + 6;
    explicit SceneViews(std::uint16_t first);
    std::array<std::uint16_t, ShadowCascadeCount> shadow{};
    std::array<std::uint16_t, LightShadowTiles> lightShadow{};
    std::uint16_t sky, opaque, overlay, transparent, distortion, spriteMask;
};

// Off-screen targets of one scene resolution, reused by every world drawn at
// that resolution (worlds render one after another).
struct SceneTarget {
    explicit SceneTarget(glm::ivec2 size);
    // Copies the scene's colour and depth into colorCopy and depthCopy at the
    // start of `view`. Passes that need to see the opaque scene behind them
    // (water, distortion) call it once per world before drawing in `view`.
    void copyOpaque(std::uint16_t view) const;
    // The screen-offset buffer of distort sprites and particles, created on
    // first use. Their shader tests against the scene depth texture, so walls
    // hide distortion behind them.
    bgfx::FrameBufferHandle distortionTarget();
    bgfx::FrameBufferHandle spriteMaskTarget();
    glm::ivec2 size;
    Texture color, depth;
    Texture colorCopy, depthCopy; // the opaque scene, before transparent draws
    Texture distortion;           // RG16F screen offsets, valid when `distorted`
    Texture spriteMask;
    Framebuffer spriteMaskBuffer;
    Framebuffer scene, distortionBuffer;
    bool distorted = false; // the world drew distortion this frame
};

// Fills `view` with one triangle covering the screen.
void fullscreen(std::uint16_t view, bgfx::ProgramHandle program, std::uint64_t state, std::uint32_t& calls);

// Counts submitted draws.
inline void submitDraw(std::uint16_t view, bgfx::ProgramHandle program, std::uint32_t& calls) {
    bgfx::submit(view, program);
    ++calls;
}
} // namespace fe
