#include "engine/render/WorldRenderer.h"
#include "engine/core/Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace fe {
namespace {
constexpr std::uint16_t ShadowSize = 2048;
constexpr std::uint64_t PointSampler = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                       BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
constexpr std::uint64_t LinearClamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
constexpr std::uint64_t OpaqueState =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
constexpr std::uint64_t BlendState =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA;

struct Views {
    std::uint16_t shadow, sky, opaque, overlay, transparent, bloom, blur, post;
    explicit Views(int slot) {
        const auto base = static_cast<std::uint16_t>(10 + slot * 12);
        shadow = base;
        sky = base + 1;
        opaque = base + 2;
        overlay = base + 3;
        transparent = base + 4;
        bloom = base + 5;
        blur = base + 6;
        post = base + 7;
    }
};

struct ScreenVertex {
    float x, y, z, u, v;
};

struct Frustum {
    std::array<glm::vec4, 6> planes{};
    explicit Frustum(const glm::mat4& m) {
        const auto row = [&](int i) {
            return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
        };
        planes = {row(3) + row(0), row(3) - row(0), row(3) + row(1),
                  row(3) - row(1), row(3) + row(2), row(3) - row(2)};
    }
    [[nodiscard]] bool visible(glm::vec3 min, glm::vec3 max) const {
        for (const auto& p : planes) {
            const glm::vec3 positive(p.x >= 0 ? max.x : min.x, p.y >= 0 ? max.y : min.y,
                                     p.z >= 0 ? max.z : min.z);
            if (glm::dot(glm::vec3(p), positive) + p.w < 0) {
                return false;
            }
        }
        return true;
    }
};

// A deferred transparent draw sorted back to front.
struct AlphaDraw {
    float distance = 0;
    std::function<void()> submit;
};
} // namespace

struct WorldRenderer::Impl {
    GpuAssets& gpu;
    std::filesystem::path shaderRoot;
    int pixelsPerUnit;
    Program world, sprite, water, shadow, sky, overlay, bloom, post;
    std::map<std::string, Uniform> uniforms;
    bgfx::VertexLayout screenLayout;
    Framebuffer shadowBuffer, sceneBuffer, bloomBuffer, blurBuffer;
    bgfx::TextureHandle shadowColor = BGFX_INVALID_HANDLE, sceneColor = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle sceneDepth = BGFX_INVALID_HANDLE, bloomColor = BGFX_INVALID_HANDLE,
                        blurColor = BGFX_INVALID_HANDLE;
    glm::ivec2 sceneSize{0};
    std::uint32_t calls = 0;
    // Per-render state
    std::vector<LightSettings> lights;
    glm::mat4 lightViewProj{1};
    glm::vec3 cameraPosition{0};
    const Environment* environment = nullptr;
    double time = 0;

    Impl(GpuAssets& g, std::filesystem::path shaders, int ppu)
        : gpu(g), shaderRoot(std::move(shaders)), pixelsPerUnit(ppu) {
        screenLayout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        for (const char* name :
             {"u_materialColor", "u_materialParams", "u_emissive",   "u_ambient",         "u_sunDirection",
              "u_sunColor",      "u_cameraPos",      "u_camera",     "u_fogColor",        "u_fogParams",
              "u_shadowParams",  "u_blur",           "u_post",       "u_postFlags",       "u_screen",
              "u_dof",           "u_tint",           "u_transition", "u_transitionColor", "u_spriteFlash",
              "u_spriteOutline", "u_spriteParams",   "u_water",      "u_skyTop",          "u_skyHorizon",
              "u_skyBottom",     "u_skyParams",      "u_cloudColor", "u_overlay"}) {
            uniforms.emplace(name, Uniform(bgfx::createUniform(name, bgfx::UniformType::Vec4)));
        }
        for (const char* name : {"u_lightViewProj"}) {
            uniforms.emplace(name, Uniform(bgfx::createUniform(name, bgfx::UniformType::Mat4)));
        }
        for (const char* name : {"u_lightPosRadius", "u_lightColor", "u_lightDir"}) {
            uniforms.emplace(name, Uniform(bgfx::createUniform(name, bgfx::UniformType::Vec4, 8)));
        }
        for (const char* name : {"s_albedo", "s_normal", "s_shadow", "s_palette", "s_panorama", "s_scene",
                                 "s_bloom", "s_lut", "s_lut2", "s_depth"}) {
            uniforms.emplace(name, Uniform(bgfx::createUniform(name, bgfx::UniformType::Sampler)));
        }
        const bgfx::TextureHandle shadowTargets[] = {
            shadowColor = bgfx::createTexture2D(ShadowSize, ShadowSize, false, 1, bgfx::TextureFormat::R32F,
                                                BGFX_TEXTURE_RT | PointSampler),
            bgfx::createTexture2D(ShadowSize, ShadowSize, false, 1, bgfx::TextureFormat::D16,
                                  BGFX_TEXTURE_RT)};
        shadowBuffer = Framebuffer(bgfx::createFrameBuffer(2, shadowTargets, true));
        reload();
    }

    void reload() {
        Program next[8] = {Program(loadProgram(shaderRoot, "vs_world", "fs_world")),
                           Program(loadProgram(shaderRoot, "vs_world", "fs_sprite")),
                           Program(loadProgram(shaderRoot, "vs_world", "fs_water")),
                           Program(loadProgram(shaderRoot, "vs_shadow", "fs_shadow")),
                           Program(loadProgram(shaderRoot, "vs_sky", "fs_sky")),
                           Program(loadProgram(shaderRoot, "vs_world", "fs_overlay")),
                           Program(loadProgram(shaderRoot, "vs_screen", "fs_bloom")),
                           Program(loadProgram(shaderRoot, "vs_screen", "fs_post"))};
        world = std::move(next[0]);
        sprite = std::move(next[1]);
        water = std::move(next[2]);
        shadow = std::move(next[3]);
        sky = std::move(next[4]);
        overlay = std::move(next[5]);
        bloom = std::move(next[6]);
        post = std::move(next[7]);
    }

    void set(const char* name, const glm::vec4& value) {
        bgfx::setUniform(uniforms.at(name).get(), glm::value_ptr(value));
    }
    void set(const char* name, const glm::mat4& value) {
        bgfx::setUniform(uniforms.at(name).get(), glm::value_ptr(value));
    }
    void bind(std::uint8_t stage, const char* name, bgfx::TextureHandle texture,
              std::uint32_t flags = UINT32_MAX) {
        bgfx::setTexture(stage, uniforms.at(name).get(), texture, flags);
    }

    void resize(glm::ivec2 viewport, int pixelScale) {
        const glm::ivec2 size = glm::max(glm::ivec2(1), viewport / std::max(1, pixelScale));
        if (size == sceneSize) {
            return;
        }
        sceneSize = size;
        const auto w = static_cast<std::uint16_t>(size.x), h = static_cast<std::uint16_t>(size.y);
        sceneColor = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA16F,
                                           BGFX_TEXTURE_RT | LinearClamp);
        sceneDepth =
            bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT | PointSampler);
        const bgfx::TextureHandle targets[] = {sceneColor, sceneDepth};
        sceneBuffer = Framebuffer(bgfx::createFrameBuffer(2, targets, true));
        const auto bw = static_cast<std::uint16_t>(std::max(1, size.x / 4)),
                   bh = static_cast<std::uint16_t>(std::max(1, size.y / 4));
        bloomBuffer = Framebuffer(
            bgfx::createFrameBuffer(bw, bh, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | LinearClamp));
        blurBuffer = Framebuffer(
            bgfx::createFrameBuffer(bw, bh, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | LinearClamp));
        bloomColor = bgfx::getTexture(bloomBuffer.get());
        blurColor = bgfx::getTexture(blurBuffer.get());
    }

    // Sun, fog and the 8 lights that matter most around a point.
    void lighting(glm::vec3 centre, float radius) {
        const auto& env = *environment;
        set("u_lightViewProj", lightViewProj);
        set("u_ambient", glm::vec4(env.ambient, 1));
        set("u_sunDirection", glm::vec4(glm::normalize(env.sunDirection), 0));
        set("u_sunColor", glm::vec4(env.sunColor, 1));
        set("u_cameraPos", glm::vec4(cameraPosition, 1));
        set("u_camera", glm::vec4(cameraPosition, 1));
        set("u_fogColor", glm::vec4(env.fogColor, env.fogDensity));
        set("u_fogParams", glm::vec4(env.fogHeight, env.fogHeightFalloff, 0, 0));
        set("u_shadowParams",
            glm::vec4(1.0f / ShadowSize, bgfx::getCaps()->homogeneousDepth ? 0.5f : 1.0f,
                      bgfx::getCaps()->originBottomLeft ? 1.0f : -1.0f, env.shadows ? 1.0f : 0.0f));
        std::array<std::pair<float, const LightSettings*>, 64> nearest{};
        std::size_t count = 0;
        for (const auto& light : lights) {
            const float reach = glm::length(light.position - centre) - light.radius - radius;
            if (reach < 0 && count < nearest.size()) {
                nearest[count++] = {reach / std::max(0.01f, light.intensity), &light};
            }
        }
        std::sort(nearest.begin(), nearest.begin() + static_cast<std::ptrdiff_t>(count));
        std::array<glm::vec4, 8> positions{}, colors{}, directions{};
        for (std::size_t i = 0; i < std::min<std::size_t>(8, count); ++i) {
            const auto& l = *nearest[i].second;
            positions[i] = {l.position, l.radius};
            const bool spot = l.type == LightType::Spot;
            colors[i] = {l.color * l.intensity, spot ? std::cos(glm::radians(l.innerCone)) : -2.0f};
            directions[i] = {glm::normalize(l.direction), std::cos(glm::radians(l.outerCone))};
        }
        bgfx::setUniform(uniforms.at("u_lightPosRadius").get(), positions.data(), 8);
        bgfx::setUniform(uniforms.at("u_lightColor").get(), colors.data(), 8);
        bgfx::setUniform(uniforms.at("u_lightDir").get(), directions.data(), 8);
    }

    void screen(std::uint16_t view, bgfx::ProgramHandle program,
                std::uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, float depth = 0) {
        const bool flip = bgfx::getCaps()->originBottomLeft;
        const ScreenVertex vertices[] = {{-1, -1, depth, 0, flip ? 0.0f : 1.0f},
                                         {3, -1, depth, 2, flip ? 0.0f : 1.0f},
                                         {-1, 3, depth, 0, flip ? 2.0f : -1.0f}};
        if (bgfx::getAvailTransientVertexBuffer(3, screenLayout) < 3) {
            return;
        }
        bgfx::TransientVertexBuffer vb;
        bgfx::allocTransientVertexBuffer(&vb, 3, screenLayout);
        std::memcpy(vb.data, vertices, sizeof(vertices));
        bgfx::setVertexBuffer(0, &vb);
        bgfx::setState(state);
        bgfx::submit(view, program);
        ++calls;
    }

    bool transient(const std::vector<MeshVertex>& vertices, const std::vector<std::uint32_t>& indices) {
        const auto vc = static_cast<std::uint32_t>(vertices.size()),
                   ic = static_cast<std::uint32_t>(indices.size());
        if (vc == 0 || bgfx::getAvailTransientVertexBuffer(vc, meshLayout()) < vc ||
            bgfx::getAvailTransientIndexBuffer(ic, true) < ic) {
            return false;
        }
        bgfx::TransientVertexBuffer vb;
        bgfx::TransientIndexBuffer ib;
        bgfx::allocTransientVertexBuffer(&vb, vc, meshLayout());
        bgfx::allocTransientIndexBuffer(&ib, ic, true);
        std::memcpy(vb.data, vertices.data(), vc * sizeof(MeshVertex));
        std::memcpy(ib.data, indices.data(), ic * sizeof(std::uint32_t));
        bgfx::setVertexBuffer(0, &vb);
        bgfx::setIndexBuffer(&ib);
        return true;
    }

    void material(const GpuPart& part, const glm::vec4& tint) {
        bind(0, "s_albedo", part.albedo);
        bind(1, "s_normal", part.normal);
        bind(2, "s_shadow", shadowColor);
        const auto& m = part.material;
        set("u_materialColor", m.color * tint);
        set("u_materialParams",
            glm::vec4(m.alphaCutoff, part.normal.idx != gpu.flatNormal().idx ? 1.0f : 0.0f, 0.0f,
                      m.kind == MaterialKind::Unlit ? 1.0f : 0.0f));
        set("u_emissive", glm::vec4(m.emissive, 0));
    }

    void drawPart(const Views& views, const GpuPart& part, const glm::mat4& transform, const glm::vec4& tint,
                  bool castShadow, std::vector<AlphaDraw>& alpha, const Frustum& frustum) {
        const auto wmin = glm::vec3(transform * glm::vec4(part.min, 1));
        const auto wmax = glm::vec3(transform * glm::vec4(part.max, 1));
        const auto lo = glm::min(wmin, wmax), hi = glm::max(wmin, wmax);
        const auto centre = (lo + hi) * 0.5f;
        const float radius = glm::length(hi - lo) * 0.5f;
        auto geometry = [&] {
            bgfx::setTransform(glm::value_ptr(transform));
            bgfx::setVertexBuffer(0, part.vertices.get());
            bgfx::setIndexBuffer(part.indices.get());
        };
        if (castShadow && environment->shadows && !part.material.transparent) {
            geometry();
            bind(0, "s_albedo", part.albedo);
            set("u_materialParams", glm::vec4(part.material.alphaCutoff, 0, 0, 0));
            bgfx::setState(OpaqueState | (part.material.doubleSided ? 0 : BGFX_STATE_CULL_CW));
            bgfx::submit(views.shadow, shadow.get());
            ++calls;
        }
        if (!frustum.visible(lo, hi)) {
            return;
        }
        if (part.material.transparent) {
            const bool isWater = part.material.kind == MaterialKind::Water;
            alpha.push_back({glm::length(centre - cameraPosition),
                             [this, &part, transform, tint, views, centre, radius, isWater] {
                                 bgfx::setTransform(glm::value_ptr(transform));
                                 bgfx::setVertexBuffer(0, part.vertices.get());
                                 bgfx::setIndexBuffer(part.indices.get());
                                 lighting(centre, radius);
                                 material(part, tint);
                                 if (isWater) {
                                     set("u_water", glm::vec4(static_cast<float>(time), 0.6f, 1.0f, 1.0f));
                                     set("u_skyHorizon", glm::vec4(environment->sky.horizon, 1));
                                 }
                                 bgfx::setState(BlendState |
                                                (part.material.doubleSided ? 0 : BGFX_STATE_CULL_CW));
                                 bgfx::submit(views.transparent, isWater ? water.get() : world.get());
                                 ++calls;
                             }});
            return;
        }
        geometry();
        lighting(centre, radius);
        material(part, tint);
        bgfx::setState(OpaqueState | (part.material.doubleSided ? 0 : BGFX_STATE_CULL_CW));
        bgfx::submit(views.opaque, world.get());
        ++calls;
    }

    void drawMap(World& w, const Views& views, std::vector<AlphaDraw>& alpha, const Frustum& frustum) {
        const auto* model = gpu.model(w.geometryPath(), true);
        if (!model) {
            return;
        }
        for (const auto& part : model->parts) {
            drawPart(views, part, glm::mat4(1), glm::vec4(1), true, alpha, frustum);
        }
    }

    void drawMeshes(World& w, double alphaT, const Views& views, std::vector<AlphaDraw>& alpha,
                    const Frustum& frustum) {
        auto& registry = w.registry();
        std::vector<glm::mat4> pose;
        for (const auto entity : registry.view<Transform, MeshInstance>()) {
            const auto& mesh = registry.get<MeshInstance>(entity);
            const auto* model = mesh.visible ? gpu.model(mesh.model, false) : nullptr;
            if (!model) {
                continue;
            }
            const auto& t = registry.get<Transform>(entity);
            const float yaw = glm::mix(t.previousYaw, t.yaw, static_cast<float>(alphaT));
            const auto base = glm::translate(glm::mat4(1), w.interpolated(entity, alphaT)) *
                              glm::rotate(glm::mat4(1), yaw, glm::vec3(0, 1, 0)) *
                              glm::scale(glm::mat4(1), t.scale);
            model->source->pose(model->source->animation(mesh.animation), mesh.time, pose);
            for (const auto& part : model->parts) {
                drawPart(views, part, base * pose[static_cast<std::size_t>(part.node)], mesh.tint,
                         mesh.castShadow, alpha, frustum);
            }
        }
    }

    // A camera-facing or ground-aligned quad for one sprite frame.
    void spriteQuad(const SpriteAnimation& animation, const SpriteFrame& frame, glm::ivec2 atlas,
                    glm::vec3 position, const Sprite& s, const Camera& camera, double alphaT,
                    std::vector<MeshVertex>& vertices) {
        const float ppu =
            static_cast<float>(animation.pixelsPerUnit > 0 ? animation.pixelsPerUnit : pixelsPerUnit);
        const float w = static_cast<float>(frame.rect.z) / ppu * s.scale,
                    h = static_cast<float>(frame.rect.w) / ppu * s.scale;
        glm::vec3 right = camera.right(alphaT), up(0, 1, 0);
        if (s.mode == SpriteMode::Facing) {
            up = glm::normalize(glm::cross(right, camera.forward(alphaT)));
        } else if (s.mode == SpriteMode::Flat) {
            right = glm::vec3(1, 0, 0);
            up = glm::vec3(0, 0, -1);
            position.y += 0.02f;
        }
        position += right * (s.pixelOffset.x / ppu) + up * (-s.pixelOffset.y / ppu);
        if (s.depthBias != 0) {
            position -= camera.forward(alphaT) * s.depthBias;
        }
        const auto bottomLeft =
            position - right * (w * animation.pivot.x) + up * (h * (animation.pivot.y - 1));
        const float u0 = static_cast<float>(frame.rect.x) / static_cast<float>(atlas.x);
        const float v0 = static_cast<float>(frame.rect.y) / static_cast<float>(atlas.y);
        const float u1 = static_cast<float>(frame.rect.x + frame.rect.z) / static_cast<float>(atlas.x);
        const float v1 = static_cast<float>(frame.rect.y + frame.rect.w) / static_cast<float>(atlas.y);
        const auto normal = glm::normalize(-camera.forward(alphaT) * 0.6f + glm::vec3(0, 0.4f, 0));
        vertices.push_back({bottomLeft, normal, {u0, v1}, s.tint});
        vertices.push_back({bottomLeft + right * w, normal, {u1, v1}, s.tint});
        vertices.push_back({bottomLeft + right * w + up * h, normal, {u1, v0}, s.tint});
        vertices.push_back({bottomLeft + up * h, normal, {u0, v0}, s.tint});
    }

    void drawSprites(World& w, double alphaT, const Views& views, std::vector<AlphaDraw>& alpha,
                     const Frustum& frustum) {
        auto& registry = w.registry();
        const auto& camera = w.camera();
        static const std::vector<std::uint32_t> quad = {0, 1, 2, 0, 2, 3};
        for (const auto entity : registry.view<Transform, Sprite>()) {
            const auto& s = registry.get<Sprite>(entity);
            if (!s.visible || s.animation.empty()) {
                continue;
            }
            const auto animation = gpu.cpu().trySprite(s.animation);
            if (!animation) {
                continue;
            }
            const auto atlasPath =
                (std::filesystem::path(s.animation).parent_path() / animation->atlas).generic_string();
            const auto atlasSize = gpu.imageSize(atlasPath);
            if (atlasSize.x == 0) {
                continue;
            }
            const auto& t = registry.get<Transform>(entity);
            const double spriteTime = glm::mix(s.previousTime, s.time, alphaT);
            const int direction =
                s.direction >= 0 ? s.direction
                                 : animation->direction(s.clip, t.facing, camera.interpolatedYaw(alphaT));
            const auto& frame = animation->frameFor(s.clip, direction, spriteTime);
            const auto position = w.interpolated(entity, alphaT);
            std::vector<MeshVertex> vertices;
            spriteQuad(*animation, frame, atlasSize, position, s, camera, alphaT, vertices);
            const auto lo = glm::min(vertices[0].position, vertices[2].position);
            const auto hi = glm::max(vertices[0].position, vertices[2].position);
            if (!frustum.visible(lo - glm::vec3(0.1f), hi + glm::vec3(0.1f))) {
                continue;
            }
            const auto texture = gpu.image(atlasPath);
            const bool palette = !s.palette.empty();
            auto submit = [this, vertices, texture, s, position, palette,
                           atlasSize](std::uint16_t view, std::uint64_t state, float cutoff) {
                if (!transient(vertices, quad)) {
                    return;
                }
                lighting(position + glm::vec3(0, 0.8f, 0), 1.0f);
                bind(0, "s_albedo", texture);
                bind(1, "s_palette", palette ? gpu.image(s.palette) : gpu.white());
                bind(2, "s_shadow", shadowColor);
                const auto paletteSize = palette ? gpu.imageSize(s.palette) : glm::ivec2(1);
                set("u_spriteFlash", s.flash);
                set("u_spriteOutline", s.outline);
                set("u_spriteParams",
                    glm::vec4(1.0f / static_cast<float>(atlasSize.x), 1.0f / static_cast<float>(atlasSize.y),
                              (static_cast<float>(s.paletteRow) + 0.5f) /
                                  static_cast<float>(std::max(1, paletteSize.y)),
                              palette ? 1.0f : 0.0f));
                set("u_materialParams", glm::vec4(cutoff, 0, 0, s.unlit ? 1.0f : 0.0f));
                bgfx::setState(state);
                bgfx::submit(view, sprite.get());
                ++calls;
            };
            if (s.castShadow && environment->shadows) {
                if (transient(vertices, quad)) {
                    bind(0, "s_albedo", texture);
                    set("u_materialParams", glm::vec4(0.5f, 0, 0, 0));
                    bgfx::setState(OpaqueState);
                    bgfx::submit(views.shadow, shadow.get());
                    ++calls;
                }
            }
            if (s.blend || s.tint.a < 0.999f) {
                alpha.push_back({glm::length(position - cameraPosition),
                                 [submit, views] { submit(views.transparent, BlendState, 0.01f); }});
            } else {
                submit(views.opaque, OpaqueState, 0.5f);
            }
        }
    }

    void drawParticles(World& w, double alphaT, const Views& views) {
        const auto& particles = w.particles();
        if (particles.empty()) {
            return;
        }
        const auto& camera = w.camera();
        const auto right = camera.right(alphaT);
        const auto up = glm::normalize(glm::cross(right, camera.forward(alphaT)));
        const auto& sprites = w.particleSprites();
        std::map<std::uint16_t, std::pair<std::vector<MeshVertex>, std::vector<std::uint32_t>>> batches;
        for (const auto& p : particles) {
            const float t = p.age / p.lifetime;
            const float size = glm::mix(p.size, p.sizeEnd, t);
            const auto color = glm::mix(p.color, p.colorEnd, t);
            glm::vec3 axisX = right * size, axisY = up * size;
            if (p.stretch > 0 && glm::length(p.velocity) > 0.01f) {
                axisY = glm::normalize(p.velocity) * (size + glm::length(p.velocity) * p.stretch);
                axisX = glm::normalize(glm::cross(axisY, camera.forward(alphaT))) * size;
            }
            auto& batch = batches[p.sprite];
            const auto base = static_cast<std::uint32_t>(batch.first.size());
            const glm::vec3 n(0, 1, 0);
            batch.first.push_back({p.position - axisX - axisY, n, {0, 1}, color});
            batch.first.push_back({p.position + axisX - axisY, n, {1, 1}, color});
            batch.first.push_back({p.position + axisX + axisY, n, {1, 0}, color});
            batch.first.push_back({p.position - axisX + axisY, n, {0, 0}, color});
            for (const auto index : {0u, 1u, 2u, 0u, 2u, 3u}) {
                batch.second.push_back(base + index);
            }
        }
        for (const auto& [spriteIndex, batch] : batches) {
            bgfx::TextureHandle texture = gpu.image("@fe/assets/ui/spark.png", true);
            const auto& path = sprites[spriteIndex];
            if (!path.empty()) {
                if (const auto animation = gpu.cpu().trySprite(path)) {
                    texture = gpu.image(
                        (std::filesystem::path(path).parent_path() / animation->atlas).generic_string(),
                        true);
                }
            }
            if (!transient(batch.first, batch.second)) {
                continue;
            }
            lighting(camera.focus(alphaT), 100);
            bind(0, "s_albedo", texture);
            bind(1, "s_palette", gpu.white());
            bind(2, "s_shadow", shadowColor);
            set("u_spriteFlash", glm::vec4(0));
            set("u_spriteOutline", glm::vec4(0));
            set("u_spriteParams", glm::vec4(0));
            set("u_materialParams", glm::vec4(0.01f, 0, 0, 1));
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA);
            bgfx::submit(views.transparent, sprite.get());
            ++calls;
        }
    }

    void cellQuad(const Grid& grid, GridPos cell, float lift, float inset, const glm::vec4& color,
                  std::vector<MeshVertex>& v, std::vector<std::uint32_t>& i) {
        const float x0 = static_cast<float>(cell.x) + inset, x1 = static_cast<float>(cell.x + 1) - inset;
        const float z0 = static_cast<float>(cell.z) + inset, z1 = static_cast<float>(cell.z + 1) - inset;
        auto corner = [&](float x, float z, glm::vec2 uv) {
            return MeshVertex{{x, grid.heightAt({x, z}, cell.layer) + lift, z}, {0, 1, 0}, uv, color};
        };
        const auto base = static_cast<std::uint32_t>(v.size());
        v.push_back(corner(x0, z0, {0, 0}));
        v.push_back(corner(x1, z0, {1, 0}));
        v.push_back(corner(x1, z1, {1, 1}));
        v.push_back(corner(x0, z1, {0, 1}));
        for (const auto index : {0u, 2u, 1u, 0u, 3u, 2u}) {
            i.push_back(base + index);
        }
    }

    void submitOverlay(const Views& views, const std::vector<MeshVertex>& v,
                       const std::vector<std::uint32_t>& i, float pulse, bool border) {
        if (!transient(v, i)) {
            return;
        }
        set("u_overlay", glm::vec4(static_cast<float>(time), pulse, border ? 1.0f : 0.0f, 0));
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(views.overlay, overlay.get());
        ++calls;
    }

    void drawOverlays(World& w, const Views& views) {
        const auto& grid = w.grid();
        std::vector<std::pair<int, const OverlayLayer*>> layers;
        for (const auto& [name, layer] : w.overlays()) {
            (void)name;
            if (layer.visible && !layer.cells.empty()) {
                layers.emplace_back(layer.order, &layer);
            }
        }
        std::sort(layers.begin(), layers.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [order, layer] : layers) {
            std::vector<MeshVertex> v;
            std::vector<std::uint32_t> i;
            for (const auto& cell : layer->cells) {
                if (grid.contains(cell)) {
                    cellQuad(grid, cell, 0.03f + static_cast<float>(order) * 0.004f, 0.02f, layer->color, v,
                             i);
                }
            }
            submitOverlay(views, v, i, layer->pulse, layer->border);
        }
        for (const auto& [name, path] : w.overlayPaths()) {
            (void)name;
            std::vector<MeshVertex> v;
            std::vector<std::uint32_t> i;
            for (std::size_t k = 0; k + 1 < path.cells.size(); ++k) {
                const auto a = path.cells[k], b = path.cells[k + 1];
                const glm::vec2 pa(static_cast<float>(a.x) + 0.5f, static_cast<float>(a.z) + 0.5f);
                const glm::vec2 pb(static_cast<float>(b.x) + 0.5f, static_cast<float>(b.z) + 0.5f);
                const auto dir = glm::normalize(pb - pa);
                const glm::vec2 side(-dir.y * 0.09f, dir.x * 0.09f);
                const auto base = static_cast<std::uint32_t>(v.size());
                auto at = [&](glm::vec2 p, int layer) {
                    return glm::vec3(p.x, grid.heightAt(p, layer) + 0.06f, p.y);
                };
                v.push_back({at(pa - side, a.layer), {0, 1, 0}, {0.5f, 0.5f}, path.color});
                v.push_back({at(pa + side, a.layer), {0, 1, 0}, {0.5f, 0.5f}, path.color});
                v.push_back({at(pb + side, b.layer), {0, 1, 0}, {0.5f, 0.5f}, path.color});
                v.push_back({at(pb - side, b.layer), {0, 1, 0}, {0.5f, 0.5f}, path.color});
                for (const auto index : {0u, 2u, 1u, 0u, 3u, 2u}) {
                    i.push_back(base + index);
                }
            }
            submitOverlay(views, v, i, 0, false);
        }
        const auto& cursor = w.cursor();
        if (cursor.visible && grid.contains(cursor.cell)) {
            std::vector<MeshVertex> v;
            std::vector<std::uint32_t> i;
            cellQuad(grid, cursor.cell, 0.08f, -0.04f, cursor.color, v, i);
            submitOverlay(views, v, i, 3, true);
        }
    }

    void drawSky(World& w, const Views& views, const glm::vec3& focus) {
        const auto& settings = environment->sky;
        const float mode = settings.mode == "panorama"   ? 3.0f
                           : settings.mode == "gradient" ? 2.0f
                           : settings.mode == "color"    ? 1.0f
                                                         : 0.0f;
        if (mode > 0) {
            set("u_skyTop", glm::vec4(settings.top, 1));
            set("u_skyHorizon", glm::vec4(settings.horizon, 1));
            set("u_skyBottom", glm::vec4(settings.bottom, 1));
            set("u_skyParams",
                glm::vec4(mode, settings.cloudCover, static_cast<float>(time) * settings.cloudSpeed, 0));
            set("u_cloudColor", glm::vec4(settings.cloudColor, 1));
            bind(0, "s_panorama", mode > 2.5f ? gpu.image(settings.panorama, true) : gpu.white());
            screen(views.sky, sky.get(),
                   BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS, 0.9999f);
        }
        // Backdrop cards: distant hills, cities, clouds.
        const glm::vec2 centre(static_cast<float>(w.grid().width()) * 0.5f,
                               static_cast<float>(w.grid().depth()) * 0.5f);
        static const std::vector<std::uint32_t> quad = {0, 1, 2, 0, 2, 3};
        for (const auto& b : settings.backdrops) {
            const float yaw = glm::radians(b.yaw);
            const glm::vec3 direction(std::sin(yaw), 0, -std::cos(yaw));
            const glm::vec3 anchor2 =
                glm::mix(focus, glm::vec3(centre.x, 0, centre.y), b.parallax) + direction * b.distance;
            const glm::vec3 anchor(anchor2.x, b.elevation, anchor2.z);
            const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), -direction));
            const glm::vec3 n = -direction;
            std::vector<MeshVertex> v = {
                {anchor - right * (b.width * 0.5f), n, {0, 1}, b.tint},
                {anchor + right * (b.width * 0.5f), n, {1, 1}, b.tint},
                {anchor + right * (b.width * 0.5f) + glm::vec3(0, b.height, 0), n, {1, 0}, b.tint},
                {anchor - right * (b.width * 0.5f) + glm::vec3(0, b.height, 0), n, {0, 0}, b.tint}};
            if (!transient(v, quad)) {
                continue;
            }
            lighting(anchor, 1);
            bind(0, "s_albedo", gpu.image(b.image, true));
            bind(1, "s_normal", gpu.flatNormal());
            bind(2, "s_shadow", shadowColor);
            set("u_materialColor", glm::vec4(1));
            set("u_materialParams", glm::vec4(0.01f, 0, 0, 1));
            set("u_emissive", glm::vec4(0));
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA);
            bgfx::submit(views.sky, world.get());
            ++calls;
        }
    }

    void postProcess(const Views& views, const World& w, glm::ivec2 viewport, const ScreenFx& fx) {
        const auto& settings = environment->post;
        const auto bw = static_cast<std::uint16_t>(std::max(1, sceneSize.x / 4)),
                   bh = static_cast<std::uint16_t>(std::max(1, sceneSize.y / 4));
        bgfx::setViewName(views.bloom, "Bloom extract");
        bgfx::setViewRect(views.bloom, 0, 0, bw, bh);
        bgfx::setViewFrameBuffer(views.bloom, bloomBuffer.get());
        bind(0, "s_scene", sceneColor);
        set("u_blur", glm::vec4(1.0f / static_cast<float>(sceneSize.x), 0, 1, settings.bloomThreshold));
        screen(views.bloom, bloom.get());
        bgfx::setViewName(views.blur, "Bloom blur");
        bgfx::setViewRect(views.blur, 0, 0, bw, bh);
        bgfx::setViewFrameBuffer(views.blur, blurBuffer.get());
        bind(0, "s_scene", bloomColor);
        set("u_blur", glm::vec4(0, 1.0f / static_cast<float>(bh), 0, 0));
        screen(views.blur, bloom.get());
        bgfx::setViewName(views.post, "Post");
        bgfx::setViewRect(views.post, 0, 0, static_cast<std::uint16_t>(viewport.x),
                          static_cast<std::uint16_t>(viewport.y));
        bgfx::setViewFrameBuffer(views.post, BGFX_INVALID_HANDLE);
        bind(0, "s_scene", sceneColor,
             settings.pixelScale > 1 ? static_cast<std::uint32_t>(PointSampler) : UINT32_MAX);
        bind(1, "s_bloom", blurColor);
        bind(2, "s_lut", gpu.lut(settings.lut));
        bind(3, "s_depth", sceneDepth);
        bind(4, "s_lut2", gpu.lut(settings.lutBlendTarget));
        const auto& camera = w.camera();
        set("u_dof", glm::vec4(camera.nearPlane, camera.farPlane,
                               settings.focusDistance > 0 ? settings.focusDistance : camera.distance,
                               settings.aperture));
        set("u_post",
            glm::vec4(settings.bloomStrength, settings.focus, settings.focusRange, settings.exposure));
        set("u_postFlags", glm::vec4(settings.bloom ? 1.0f : 0.0f, settings.depthOfField ? 1.0f : 0.0f,
                                     settings.vignette ? 1.0f : 0.0f, settings.colorGrade ? 1.0f : 0.0f));
        set("u_screen",
            glm::vec4(1.0f / static_cast<float>(sceneSize.x), 1.0f / static_cast<float>(sceneSize.y),
                      settings.vignetteStrength, settings.lutBlendTarget.empty() ? 0.0f : settings.lutBlend));
        set("u_tint", fx.tint);
        const auto& t = fx.transition;
        const float kind = t.kind == "swirl"    ? 1.0f
                           : t.kind == "mosaic" ? 2.0f
                           : t.kind == "wipe"   ? 3.0f
                           : t.kind == "iris"   ? 4.0f
                                                : 0.0f;
        set("u_transition", glm::vec4(kind, t.progress, t.centre.x, t.centre.y));
        set("u_transitionColor", t.color);
        screen(views.post, post.get());
    }
};

WorldRenderer::WorldRenderer(GpuAssets& gpu, std::filesystem::path shaderRoot, int pixelsPerUnit)
    : impl_(std::make_unique<Impl>(gpu, std::move(shaderRoot), pixelsPerUnit)) {
}

WorldRenderer::~WorldRenderer() = default;

void WorldRenderer::reloadPrograms() {
    impl_->reload();
    log(LogLevel::Info, "World shaders reloaded");
}

void WorldRenderer::render(World& world, double alpha, glm::ivec2 viewport, const ScreenFx& screen,
                           double time, int slot) {
    auto& p = *impl_;
    p.calls = 0;
    p.time = time;
    p.environment = &world.environment();
    const auto& env = world.environment();
    p.resize(viewport, env.post.pixelScale);
    const Views views(std::clamp(slot, 0, 3));
    const auto& camera = world.camera();
    const auto view = camera.view(alpha);
    const float aspect = static_cast<float>(viewport.x) / static_cast<float>(std::max(1, viewport.y));
    const auto projection = camera.projection(aspect, bgfx::getCaps()->homogeneousDepth);
    p.cameraPosition = camera.position(alpha);
    p.lights = world.activeLights();
    // Shadows cover the area around the camera focus.
    const auto focus = camera.focus(alpha);
    const auto sun = glm::normalize(env.sunDirection);
    const float extent = env.shadowDistance;
    const auto lightView = glm::lookAtRH(focus - sun * extent * 2.0f, focus,
                                         std::abs(sun.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0));
    const auto lightProjection = bgfx::getCaps()->homogeneousDepth
                                     ? glm::orthoRH_NO(-extent, extent, -extent, extent, 0.1f, extent * 4)
                                     : glm::orthoRH_ZO(-extent, extent, -extent, extent, 0.1f, extent * 4);
    p.lightViewProj = lightProjection * lightView;
    bgfx::setViewName(views.shadow, "Shadow");
    bgfx::setViewFrameBuffer(views.shadow, p.shadowBuffer.get());
    bgfx::setViewRect(views.shadow, 0, 0, ShadowSize, ShadowSize);
    bgfx::setViewTransform(views.shadow, glm::value_ptr(lightView), glm::value_ptr(lightProjection));
    bgfx::setViewClear(views.shadow, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xffffffff, 1);
    bgfx::touch(views.shadow);
    for (const auto id : {views.sky, views.opaque, views.overlay, views.transparent}) {
        bgfx::setViewFrameBuffer(id, p.sceneBuffer.get());
        bgfx::setViewRect(id, 0, 0, static_cast<std::uint16_t>(p.sceneSize.x),
                          static_cast<std::uint16_t>(p.sceneSize.y));
        bgfx::setViewTransform(id, glm::value_ptr(view), glm::value_ptr(projection));
    }
    bgfx::setViewName(views.sky, "Sky");
    bgfx::setViewName(views.opaque, "Opaque");
    bgfx::setViewName(views.overlay, "Grid overlays");
    bgfx::setViewName(views.transparent, "Transparent");
    bgfx::setViewMode(views.transparent, bgfx::ViewMode::Sequential);
    bgfx::setViewMode(views.overlay, bgfx::ViewMode::Sequential);
    const auto clear = glm::clamp(env.fogColor, glm::vec3(0), glm::vec3(1));
    bgfx::setViewClear(views.sky, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                       (static_cast<std::uint32_t>(clear.r * 255) << 24u) |
                           (static_cast<std::uint32_t>(clear.g * 255) << 16u) |
                           (static_cast<std::uint32_t>(clear.b * 255) << 8u) | 0xffu,
                       1.0f);
    bgfx::touch(views.sky);
    const Frustum frustum(projection * view);
    std::vector<AlphaDraw> alphaDraws;
    p.drawSky(world, views, focus);
    p.drawMap(world, views, alphaDraws, frustum);
    p.drawMeshes(world, alpha, views, alphaDraws, frustum);
    p.drawSprites(world, alpha, views, alphaDraws, frustum);
    p.drawOverlays(world, views);
    std::sort(alphaDraws.begin(), alphaDraws.end(),
              [](const AlphaDraw& a, const AlphaDraw& b) { return a.distance > b.distance; });
    for (const auto& draw : alphaDraws) {
        draw.submit();
    }
    p.drawParticles(world, alpha, views);
    p.postProcess(views, world, viewport, screen);
    drawCalls_ = p.calls;
}
} // namespace fe
