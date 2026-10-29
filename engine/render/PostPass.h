#pragma once
#include "engine/render/CustomShaders.h"
#include "engine/render/FrameMath.h"
#include "engine/render/RenderCommon.h"
#include "engine/services/ScreenFx.h"
#include "engine/world/World.h"
#include <filesystem>
#include <functional>

namespace fe {
// Where a world's post pass lands: a framebuffer and a pixel rect in it.
struct PostOutput {
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    glm::ivec4 rect{0}; // x, y, width, height
};

// One world's post-processing, from its HDR scene to the output:
// ambient occlusion and depth of field at half resolution, a bloom mip chain,
// then one composite pass (distortion, chromatic aberration, depth of field,
// occlusion, bloom, exposure, tone mapping, sRGB encoding, LUT grading, tint,
// vignette) followed by the game's post shaders and FXAA. The last pass
// blends over the output with the scene's alpha, so later worlds draw on top
// of earlier ones and transparent areas (sky "none") show what is underneath.
class PostPass {
public:
    PostPass(GpuAssets& gpu, const Uniforms& uniforms, CustomShaders& custom,
             const std::filesystem::path& shaderRoot);
    void reload(const std::filesystem::path& shaderRoot);
    // bgfx views render() takes for these settings.
    [[nodiscard]] static std::uint16_t viewsNeeded(const PostSettings& settings);
    void render(World& world, const ScreenFx& screen, SceneTarget& scene, const PostOutput& output,
                double time, const std::function<std::uint16_t()>& takeView, std::uint32_t& calls);
    // Releases targets no world used this frame.
    void endFrame();

private:
    struct Targets {
        std::vector<glm::ivec2> bloomSizes;
        std::vector<Framebuffer> bloom;
        Framebuffer dofHalf, dofFields, ao, aoBlur;
        Texture dofNear, dofFar;
    };
    GpuAssets& gpu_;
    const Uniforms& uniforms_;
    Program bloomDown_, bloomUp_, dofPrefilter_, dofGather_, ssao_, ssaoBlur_, post_, fxaa_;
    Uniform bloomUniform_, dofUniform_, ssaoUniform_, paramsUniform_;
    Uniform source_, half_, near_, far_, ao_, distortion_, input_;
    CustomShaders& custom_;
    FramePool<std::pair<int, int>, Targets> targets_;
    FramePool<std::pair<int, int>, std::array<Framebuffer, 2>> chains_;
};
} // namespace fe
