#include "engine/render/PostPass.h"
#include <glm/gtc/type_ptr.hpp>

namespace fe {
namespace {
constexpr std::uint64_t WriteAll = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;

Framebuffer colorTarget(glm::ivec2 size, bgfx::TextureFormat::Enum format) {
    return Framebuffer(bgfx::createFrameBuffer(static_cast<std::uint16_t>(size.x),
                                               static_cast<std::uint16_t>(size.y), format,
                                               BGFX_TEXTURE_RT | LinearClamp));
}

void target(std::uint16_t view, const char* name, bgfx::FrameBufferHandle framebuffer, glm::ivec4 rect) {
    bgfx::setViewName(view, name);
    bgfx::setViewFrameBuffer(view, framebuffer);
    bgfx::setViewRect(view, static_cast<std::uint16_t>(rect.x), static_cast<std::uint16_t>(rect.y),
                      static_cast<std::uint16_t>(rect.z), static_cast<std::uint16_t>(rect.w));
}

void target(std::uint16_t view, const char* name, bgfx::FrameBufferHandle framebuffer, glm::ivec2 size) {
    target(view, name, framebuffer, glm::ivec4(0, 0, size));
}

float flag(bool on) {
    return on ? 1.0f : 0.0f;
}
} // namespace

PostPass::PostPass(GpuAssets& gpu, const Uniforms& uniforms, CustomShaders& custom,
                   const std::filesystem::path& shaderRoot)
    : gpu_(gpu), uniforms_(uniforms),
      bloomUniform_(bgfx::createUniform("u_bloom", bgfx::UniformType::Vec4, 2)),
      dofUniform_(bgfx::createUniform("u_dof", bgfx::UniformType::Vec4, 3)),
      ssaoUniform_(bgfx::createUniform("u_ssao", bgfx::UniformType::Vec4, 2)),
      source_(bgfx::createUniform("s_source", bgfx::UniformType::Sampler)),
      half_(bgfx::createUniform("s_half", bgfx::UniformType::Sampler)),
      near_(bgfx::createUniform("s_near", bgfx::UniformType::Sampler)),
      far_(bgfx::createUniform("s_far", bgfx::UniformType::Sampler)),
      ao_(bgfx::createUniform("s_ao", bgfx::UniformType::Sampler)),
      distortion_(bgfx::createUniform("s_distortion", bgfx::UniformType::Sampler)),
      input_(bgfx::createUniform("s_input", bgfx::UniformType::Sampler)), custom_(custom) {
    reload(shaderRoot);
}

void PostPass::reload(const std::filesystem::path& shaderRoot) {
    const auto screen = [&](const char* fragment) {
        return Program(loadProgram(shaderRoot, "vs_screen", fragment));
    };
    Program next[] = {screen("fs_bloom_down"), screen("fs_bloom_up"), screen("fs_dof_prefilter"),
                      screen("fs_dof_gather"), screen("fs_ssao"),     screen("fs_ssao_blur"),
                      screen("fs_post"),       screen("fs_fxaa")};
    bloomDown_ = std::move(next[0]);
    bloomUp_ = std::move(next[1]);
    dofPrefilter_ = std::move(next[2]);
    dofGather_ = std::move(next[3]);
    ssao_ = std::move(next[4]);
    ssaoBlur_ = std::move(next[5]);
    post_ = std::move(next[6]);
    fxaa_ = std::move(next[7]);
}

std::uint16_t PostPass::viewsNeeded(const PostSettings& s) {
    const auto bloom = s.bloom ? 2 * s.bloomLevels - 1 : 0;
    return static_cast<std::uint16_t>((s.ssao ? 3 : 0) + (s.depthOfField ? 2 : 0) + bloom + 1 +
                                      static_cast<int>(s.custom.size()) + (s.fxaa() ? 1 : 0));
}

void PostPass::endFrame() {
    targets_.endFrame();
    chains_.endFrame();
}

void PostPass::render(World& world, const ScreenFx& screen, SceneTarget& scene, const PostOutput& output,
                      double alpha, double time, const std::function<std::uint16_t()>& takeView,
                      std::uint32_t& calls) {
    const auto& s = world.environment().post;
    const auto& camera = world.camera();
    const auto* caps = bgfx::getCaps();
    const float bottomLeft = flag(caps->originBottomLeft);
    auto& targets = targets_.acquire({scene.size.x, scene.size.y}, [] { return Targets(); });
    const glm::ivec2 halfSize = glm::max(glm::ivec2(1), scene.size / 2);
    const glm::vec2 size(scene.size), half(halfSize);
    const float pixelScale = std::max(1.0f, s.pixelScale);
    const float blurRadius = s.dofRadius / pixelScale; // scene pixels
    // The scene was drawn with the interpolated camera; the focus plane and
    // field of view follow it per frame, not per tick.
    const float cameraDistance = camera.interpolatedDistance(alpha);
    glm::vec4 dof[3] = {glm::vec4(camera.nearPlane, camera.farPlane,
                                  s.focusDistance > 0 ? s.focusDistance : cameraDistance, s.aperture),
                        glm::vec4(s.focus, s.focusRange,
                                  s.dofMode == "depth"  ? 0.0f
                                  : s.dofMode == "tilt" ? 1.0f
                                                        : 2.0f,
                                  flag(caps->homogeneousDepth) + (camera.orthographic ? 2.0f : 0.0f)),
                        glm::vec4(1.0f / size, 0, bottomLeft)};

    auto ao = gpu_.white();
    if (s.ssao) {
        if (!targets.ao.valid()) {
            targets.ao = colorTarget(halfSize, bgfx::TextureFormat::RGBA8);
            targets.aoBlur = colorTarget(halfSize, bgfx::TextureFormat::RGBA8);
        }
        // Half the view's extent per unit of depth, or in world units when orthographic.
        const float tanY = camera.orthographic ? camera.viewHeight(alpha) * 0.5f
                                               : std::tan(glm::radians(camera.interpolatedFov(alpha)) * 0.5f);
        const glm::vec4 ssao[2] = {glm::vec4(tanY * size.x / size.y, tanY, flag(camera.orthographic), 0),
                                   glm::vec4(s.ssaoRadius, s.ssaoStrength, s.ssaoBias, 0)};
        auto view = takeView();
        target(view, "Ambient occlusion", targets.ao.get(), halfSize);
        bgfx::setTexture(0, uniforms_.depth.get(), scene.depth.get(), PointClamp);
        bgfx::setUniform(dofUniform_.get(), dof, 3);
        bgfx::setUniform(ssaoUniform_.get(), ssao, 2);
        fullscreen(view, ssao_.get(), WriteAll, calls);
        const std::pair<glm::vec2, const Framebuffer*> passes[] = {{{1.0f / half.x, 0}, &targets.aoBlur},
                                                                   {{0, 1.0f / half.y}, &targets.ao}};
        for (const auto& [direction, destination] : passes) {
            view = takeView();
            target(view, "Occlusion blur", destination->get(), halfSize);
            const auto& source = destination == &targets.ao ? targets.aoBlur : targets.ao;
            bgfx::setTexture(0, ao_.get(), bgfx::getTexture(source.get()));
            bgfx::setTexture(1, uniforms_.depth.get(), scene.depth.get(), PointClamp);
            glm::vec4 blur[3] = {dof[0], dof[1], glm::vec4(direction, 0, bottomLeft)};
            bgfx::setUniform(dofUniform_.get(), blur, 3);
            fullscreen(view, ssaoBlur_.get(), WriteAll, calls);
        }
        ao = bgfx::getTexture(targets.ao.get());
    }

    auto nearField = gpu_.white(), farField = gpu_.white();
    const bool depthOfField = s.depthOfField && blurRadius > 0;
    if (depthOfField) {
        if (!targets.dofHalf.valid()) {
            targets.dofHalf = colorTarget(halfSize, bgfx::TextureFormat::RGBA16F);
            const auto w = static_cast<std::uint16_t>(halfSize.x), h = static_cast<std::uint16_t>(halfSize.y);
            targets.dofNear = Texture(bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA16F,
                                                            BGFX_TEXTURE_RT | LinearClamp));
            targets.dofFar = Texture(bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA16F,
                                                           BGFX_TEXTURE_RT | LinearClamp));
            const bgfx::TextureHandle fields[] = {targets.dofNear.get(), targets.dofFar.get()};
            targets.dofFields = Framebuffer(bgfx::createFrameBuffer(2, fields, false));
        }
        auto view = takeView();
        target(view, "Depth of field prefilter", targets.dofHalf.get(), halfSize);
        bgfx::setTexture(0, uniforms_.scene.get(), scene.color.get());
        bgfx::setTexture(1, uniforms_.depth.get(), scene.depth.get(), PointClamp);
        bgfx::setUniform(dofUniform_.get(), dof, 3);
        fullscreen(view, dofPrefilter_.get(), WriteAll, calls);
        view = takeView();
        target(view, "Depth of field gather", targets.dofFields.get(), halfSize);
        bgfx::setTexture(0, half_.get(), bgfx::getTexture(targets.dofHalf.get()), PointClamp);
        glm::vec4 gather[3] = {dof[0], dof[1], glm::vec4(1.0f / half, blurRadius * 0.5f, bottomLeft)};
        bgfx::setUniform(dofUniform_.get(), gather, 3);
        fullscreen(view, dofGather_.get(), WriteAll, calls);
        nearField = targets.dofNear.get();
        farField = targets.dofFar.get();
    }

    auto bloom = gpu_.white();
    float bloomStrength = 0;
    if (s.bloom && s.bloomStrength > 0) {
        const auto sizes = bloomMipSizes(scene.size, s.bloomLevels);
        if (sizes != targets.bloomSizes) {
            targets.bloomSizes = sizes;
            targets.bloom.clear();
            for (const auto mip : sizes) {
                targets.bloom.push_back(colorTarget(mip, bgfx::TextureFormat::RGBA16F));
            }
        }
        const auto levels = sizes.size();
        for (std::size_t level = 0; level < levels; ++level) {
            const auto view = takeView();
            target(view, "Bloom down", targets.bloom[level].get(), sizes[level]);
            const glm::vec2 source = level == 0 ? size : glm::vec2(sizes[level - 1]);
            bgfx::setTexture(0, source_.get(),
                             level == 0 ? scene.color.get()
                                        : bgfx::getTexture(targets.bloom[level - 1].get()));
            const glm::vec4 settings[2] = {glm::vec4(1.0f / source, flag(level == 0), s.bloomThreshold),
                                           glm::vec4(bloomKnee(s), 0, 0, 0)};
            bgfx::setUniform(bloomUniform_.get(), settings, 2);
            fullscreen(view, bloomDown_.get(), WriteAll, calls);
        }
        for (std::size_t level = levels - 1; level-- > 0;) {
            const auto view = takeView();
            target(view, "Bloom up", targets.bloom[level].get(), sizes[level]);
            bgfx::setTexture(0, source_.get(), bgfx::getTexture(targets.bloom[level + 1].get()));
            const glm::vec4 settings[2] = {glm::vec4(1.0f / glm::vec2(sizes[level + 1]), 0, 0),
                                           glm::vec4(0, s.bloomRadius, 0, 0)};
            bgfx::setUniform(bloomUniform_.get(), settings, 2);
            fullscreen(view, bloomUp_.get(), BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ADD, calls);
        }
        bloom = bgfx::getTexture(targets.bloom[0].get());
        bloomStrength = s.bloomStrength / bloomNormalisation(static_cast<int>(levels), s.bloomRadius);
    }

    std::vector<std::pair<bgfx::ProgramHandle, const CustomPost*>> passes;
    for (const auto& pass : s.custom) {
        const auto program = custom_.program(pass.shader, ShaderVertex::Screen);
        if (bgfx::isValid(program)) {
            passes.emplace_back(program, &pass);
        }
    }
    const bool fxaa = s.fxaa();
    const glm::ivec2 outSize(output.rect.z, output.rect.w);
    std::array<Framebuffer, 2>* chain = nullptr;
    if (!passes.empty() || fxaa) {
        chain = &chains_.acquire({outSize.x, outSize.y}, [&] {
            return std::array<Framebuffer, 2>{colorTarget(outSize, bgfx::TextureFormat::RGBA8),
                                              colorTarget(outSize, bgfx::TextureFormat::RGBA8)};
        });
    }
    // fs_post.sc writes premultiplied colour (see the class comment).
    const auto finalState = WriteAll | PremultipliedBlend;

    auto view = takeView();
    if (chain) {
        target(view, "Post", (*chain)[0].get(), outSize);
    } else {
        target(view, "Post", output.framebuffer, output.rect);
    }
    // A whole pixel scale keeps every scene pixel a crisp block; a fraction
    // (dynamic resolution) is filtered up.
    bgfx::setTexture(0, uniforms_.scene.get(), scene.color.get(), s.pixelated() ? PointClamp : LinearClamp);
    bgfx::setTexture(1, uniforms_.bloom.get(), bloom);
    bgfx::setTexture(2, uniforms_.lut.get(), gpu_.lut(s.lut));
    bgfx::setTexture(3, uniforms_.depth.get(), scene.depth.get(), PointClamp);
    bgfx::setTexture(4, uniforms_.lut2.get(), gpu_.lut(s.lutBlendTarget));
    bgfx::setTexture(5, near_.get(), nearField);
    bgfx::setTexture(6, far_.get(), farField);
    bgfx::setTexture(7, ao_.get(), ao);
    bgfx::setTexture(8, distortion_.get(), scene.distorted ? scene.distortion.get() : gpu_.white());
    bgfx::setTexture(9, protectedSprites_.get(), s.protectSprites ? scene.spriteMask.get() : gpu_.white(),
                     PointClamp);
    const glm::vec4 protection(s.protectSprites ? 1.0f : 0.0f, 0, 0, 0);
    bgfx::setUniform(protection_.get(), &protection);
    PostFrame frame;
    frame.bloomStrength = bloomStrength;
    frame.depthOfField = depthOfField;
    frame.distorted = scene.distorted;
    frame.blurRadius = blurRadius;
    frame.tint = screen.tint;
    const auto post = packPost(s, frame);
    bgfx::setUniform(uniforms_.post.get(), post.data(), Uniforms::PostCount);
    bgfx::setUniform(dofUniform_.get(), dof, 3);
    fullscreen(view, post_.get(), chain ? WriteAll : finalState, calls);
    if (!chain) {
        return;
    }

    const glm::vec4 info(1.0f / glm::vec2(outSize), static_cast<float>(time), bottomLeft);
    std::size_t current = 0;
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const bool last = i + 1 == passes.size() && !fxaa;
        view = takeView();
        if (last) {
            target(view, "Game post", output.framebuffer, output.rect);
        } else {
            target(view, "Game post", (*chain)[1 - current].get(), outSize);
        }
        bgfx::setTexture(0, input_.get(), bgfx::getTexture((*chain)[current].get()));
        bgfx::setTexture(1, uniforms_.depth.get(), scene.depth.get(), PointClamp);
        const auto params = packCustomParams(passes[i].second->params);
        bgfx::setUniform(uniforms_.params.get(), params.data(), Uniforms::ParamsCount);
        bgfx::setUniform(uniforms_.info.get(), glm::value_ptr(info));
        fullscreen(view, passes[i].first, last ? finalState : WriteAll, calls);
        current = 1 - current;
    }
    if (fxaa) {
        view = takeView();
        target(view, "FXAA", output.framebuffer, output.rect);
        bgfx::setTexture(1, protectedSprites_.get(), s.protectSprites ? scene.spriteMask.get() : gpu_.white(),
                         PointClamp);
        bgfx::setUniform(protection_.get(), &protection);
        bgfx::setTexture(0, input_.get(), bgfx::getTexture((*chain)[current].get()));
        bgfx::setUniform(uniforms_.info.get(), glm::value_ptr(info));
        fullscreen(view, fxaa_.get(), finalState, calls);
    }
}
} // namespace fe
