#include "engine/render/RenderCommon.h"
#include <algorithm>
#include <cstring>
#include <glm/gtc/packing.hpp>

namespace fe {
namespace {
std::uint32_t byte(float value) {
    return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

struct ScreenVertex {
    float x, y, u, v;
};

const bgfx::VertexLayout& screenLayout() {
    static const bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        return l;
    }();
    return layout;
}
} // namespace

std::uint32_t packRgba8(glm::vec4 color) {
    return byte(color.r) | (byte(color.g) << 8u) | (byte(color.b) << 16u) | (byte(color.a) << 24u);
}

std::uint32_t packDirection(glm::vec3 direction, float sign) {
    return packRgba8(glm::vec4(direction * 0.5f + 0.5f, sign < 0 ? 0.0f : 1.0f));
}

WorldVertex WorldVertex::pack(glm::vec3 position, glm::vec3 normal, glm::vec2 uv, glm::vec4 color,
                              glm::vec4 tangent) {
    WorldVertex v{position, packDirection(normal), packDirection(glm::vec3(tangent), tangent.w), uv, {}};
    const auto half = glm::packHalf4x16(color);
    std::memcpy(v.color.data(), &half, sizeof(half));
    return v;
}

std::uint64_t blendFunction(BlendMode mode) {
    switch (mode) {
    case BlendMode::Additive:
        return BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE,
                                              BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE);
    case BlendMode::Multiply:
        return BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO,
                                              BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE);
    case BlendMode::Premultiplied:
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    case BlendMode::Opaque:
        return 0;
    case BlendMode::Alpha:
        break;
    }
    return AlphaBlend;
}

float blendOutput(BlendMode mode) {
    switch (mode) {
    case BlendMode::Additive:
        return 1;
    case BlendMode::Multiply:
        return 2;
    case BlendMode::Premultiplied:
        return 3;
    case BlendMode::Opaque:
    case BlendMode::Alpha:
        break;
    }
    return 0;
}

const bgfx::VertexLayout& worldLayout() {
    static const bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Half)
            .end();
        return l;
    }();
    return layout;
}

const bgfx::VertexLayout& spriteLayout() {
    static const bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color1, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::Color2, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord1, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord2, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Uint8, true)
            .end();
        return l;
    }();
    return layout;
}

Frustum::Frustum(const glm::mat4& m) {
    const auto row = [&](int i) {
        return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
    };
    planes = {row(3) + row(0), row(3) - row(0), row(3) + row(1),
              row(3) - row(1), row(3) + row(2), row(3) - row(2)};
}

bool Frustum::visible(glm::vec3 min, glm::vec3 max) const {
    for (const auto& p : planes) {
        const glm::vec3 positive(p.x >= 0 ? max.x : min.x, p.y >= 0 ? max.y : min.y,
                                 p.z >= 0 ? max.z : min.z);
        if (glm::dot(glm::vec3(p), positive) + p.w < 0) {
            return false;
        }
    }
    return true;
}

SceneViews::SceneViews(std::uint16_t first)
    : sky(static_cast<std::uint16_t>(first + ShadowCascadeCount + LightShadowTiles)), opaque(sky + 1),
      overlay(sky + 2), transparent(sky + 3), distortion(sky + 4), spriteMask(sky + 5) {
    for (std::uint16_t c = 0; c < ShadowCascadeCount; ++c) {
        shadow[c] = static_cast<std::uint16_t>(first + c);
    }
    for (std::uint16_t t = 0; t < LightShadowTiles; ++t) {
        lightShadow[t] = static_cast<std::uint16_t>(first + ShadowCascadeCount + t);
    }
}

SceneTarget::SceneTarget(glm::ivec2 targetSize) : size(targetSize) {
    const auto w = static_cast<std::uint16_t>(size.x), h = static_cast<std::uint16_t>(size.y);
    color = Texture(
        bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | LinearClamp));
    depth = Texture(
        bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT | PointClamp));
    depthCopy = Texture(bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::D32F,
                                              BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST | PointClamp));
    colorCopy = Texture(bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA16F,
                                              BGFX_TEXTURE_BLIT_DST | LinearClamp));
    const bgfx::TextureHandle attachments[] = {color.get(), depth.get()};
    scene = Framebuffer(bgfx::createFrameBuffer(2, attachments, false));
}

bgfx::FrameBufferHandle SceneTarget::distortionTarget() {
    if (!distortionBuffer.valid()) {
        distortion = Texture(
            bgfx::createTexture2D(static_cast<std::uint16_t>(size.x), static_cast<std::uint16_t>(size.y),
                                  false, 1, bgfx::TextureFormat::RG16F, BGFX_TEXTURE_RT | LinearClamp));
        const bgfx::TextureHandle attachments[] = {distortion.get()};
        distortionBuffer = Framebuffer(bgfx::createFrameBuffer(1, attachments, false));
    }
    return distortionBuffer.get();
}

bgfx::FrameBufferHandle SceneTarget::spriteMaskTarget() {
    if (!spriteMaskBuffer.valid()) {
        spriteMask = Texture(bgfx::createTexture2D(static_cast<std::uint16_t>(size.x),
            static_cast<std::uint16_t>(size.y), false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | PointClamp));
        const bgfx::TextureHandle attachments[] = {spriteMask.get(), depth.get()};
        spriteMaskBuffer = Framebuffer(bgfx::createFrameBuffer(2, attachments, false));
    }
    return spriteMaskBuffer.get();
}

void SceneTarget::copyOpaque(std::uint16_t view) const {
    bgfx::blit(view, {.handle = colorCopy.get()}, {.handle = color.get()});
    bgfx::blit(view, {.handle = depthCopy.get()}, {.handle = depth.get()});
}

Uniforms::Uniforms() {
    auto vec4 = [](const char* name, std::uint16_t count = 1) {
        return Uniform(bgfx::createUniform(name, bgfx::UniformType::Vec4, count));
    };
    auto sampler = [](const char* name) {
        return Uniform(bgfx::createUniform(name, bgfx::UniformType::Sampler));
    };
    env = vec4("u_env", EnvCount);
    cascades = Uniform(bgfx::createUniform("u_cascades", bgfx::UniformType::Mat4, CascadeCount));
    lightShadows = Uniform(bgfx::createUniform("u_lightShadows", bgfx::UniformType::Mat4, LightShadowTiles));
    material = vec4("u_material", MaterialCount);
    normalMatrix = Uniform(bgfx::createUniform("u_normalMatrix", bgfx::UniformType::Mat4));
    water = vec4("u_water", WaterCount);
    sprite = vec4("u_sprite", SpriteCount);
    overlay = vec4("u_overlay");
    sky = vec4("u_sky", SkyCount);
    post = vec4("u_post", PostCount);
    present = vec4("u_present", PresentCount);
    info = vec4("u_info");
    params = vec4("u_params", ParamsCount);
    albedo = sampler("s_albedo");
    normal = sampler("s_normal");
    shadow = sampler("s_shadow");
    palette = sampler("s_palette");
    lights = sampler("s_lights");
    lightGrid = sampler("s_lightGrid");
    lightShadow = sampler("s_lightShadow");
    emissive = sampler("s_emissive");
    spriteNormal = sampler("s_spriteNormal");
    spriteEmissive = sampler("s_spriteEmissive");
    sceneDepth = sampler("s_sceneDepth");
    sceneColor = sampler("s_sceneColor");
    panorama = sampler("s_panorama");
    scene = sampler("s_scene");
    bloom = sampler("s_bloom");
    lut = sampler("s_lut");
    lut2 = sampler("s_lut2");
    depth = sampler("s_depth");
    frame = sampler("s_frame");
}

void fullscreen(std::uint16_t view, bgfx::ProgramHandle program, std::uint64_t state, std::uint32_t& calls) {
    if (bgfx::getAvailTransientVertexBuffer(3, screenLayout()) < 3) {
        return;
    }
    // Texture v runs down the screen except on bottom-left-origin backends.
    const bool flip = bgfx::getCaps()->originBottomLeft;
    const ScreenVertex vertices[] = {
        {-1, -1, 0, flip ? 0.0f : 1.0f}, {3, -1, 2, flip ? 0.0f : 1.0f}, {-1, 3, 0, flip ? 2.0f : -1.0f}};
    bgfx::TransientVertexBuffer vb;
    bgfx::allocTransientVertexBuffer(&vb, 3, screenLayout());
    std::memcpy(vb.data, vertices, sizeof(vertices));
    bgfx::setVertexBuffer(0, &vb);
    bgfx::setState(state);
    submitDraw(view, program, calls);
}
} // namespace fe
