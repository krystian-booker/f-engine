#include "engine/render/WorldRenderer.h"
#include "engine/render/FrameMath.h"
#include "engine/world/Animation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace fe {
namespace {
constexpr std::uint64_t OverlayState =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL | AlphaBlend;
constexpr std::uint64_t ShadowState = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
constexpr float BlendCutoff = 0.01f;
// A silhouette shows only where geometry is at least this much nearer than the sprite.
constexpr float SilhouetteMargin = 0.3f;
const glm::mat4 Identity(1);

std::uint64_t cull(const Material& material) {
    return material.doubleSided ? 0 : BGFX_STATE_CULL_CW;
}

void transformBounds(const glm::mat4& m, glm::vec3 min, glm::vec3 max, glm::vec3& outMin, glm::vec3& outMax) {
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = -outMin;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z);
        const auto p = glm::vec3(m * glm::vec4(corner, 1));
        outMin = glm::min(outMin, p);
        outMax = glm::max(outMax, p);
    }
}

float skyMode(const std::string& mode) {
    return mode == "panorama" ? 3.0f : mode == "gradient" ? 2.0f : mode == "color" ? 1.0f : 0.0f;
}

constexpr std::uint16_t InstanceStride = 80;       // model matrix, tint
constexpr std::uint16_t ShadowInstanceStride = 64; // model matrix

// Render-side caches in the world's registry, valid while their revision is
// GpuAssets::revision() and their names match the component's.
struct SpriteDrawCache {
    std::uint64_t revision = 0;
    std::string animation, palette, shader;
    const SpriteSheet* sheet = nullptr;
    bgfx::TextureHandle indexAtlas = BGFX_INVALID_HANDLE, paletteTexture = BGFX_INVALID_HANDLE;
    float paletteRows = 1;
    int shaderSlot = -1;
};

struct MeshDrawCache {
    std::uint64_t revision = 0;
    std::string model, shader;
    const GpuModel* gpu = nullptr;
    int shaderSlot = -1;
};

// Per particle sprite index (ParticleSystem::sprites()): its sheet and the
// clip particles play ("idle", else the first; SpriteAnimation::clip()).
struct ParticleSheetCache {
    struct Entry {
        std::string path;
        const SpriteSheet* sheet = nullptr;
        const SpriteClip* clip = nullptr;
    };
    std::uint64_t revision = 0;
    std::vector<Entry> entries;
};

// A CustomShaders slot for a shader name ("" = none).
int shaderSlot(CustomShaders& custom, const std::string& name) {
    return name.empty() ? -1 : custom.slot(name);
}
} // namespace

WorldRenderer::WorldRenderer(GpuAssets& gpu, const Uniforms& uniforms, CustomShaders& custom,
                             const std::filesystem::path& shaderRoot, int pixelsPerUnit)
    : gpu_(gpu), uniforms_(uniforms), custom_(custom), pixelsPerUnit_(pixelsPerUnit),
      distortUniform_(bgfx::createUniform("u_distort", bgfx::UniformType::Vec4, 2)) {
    reload(shaderRoot);
}

void WorldRenderer::reload(const std::filesystem::path& shaderRoot) {
    Program next[] = {Program(loadProgram(shaderRoot, "vs_world", "fs_world")),
                      Program(loadProgram(shaderRoot, "vs_world_instanced", "fs_world")),
                      Program(loadProgram(shaderRoot, "vs_sprite", "fs_sprite")),
                      Program(loadProgram(shaderRoot, "vs_world", "fs_water")),
                      Program(loadProgram(shaderRoot, "vs_shadow", "fs_shadow")),
                      Program(loadProgram(shaderRoot, "vs_shadow_instanced", "fs_shadow")),
                      Program(loadProgram(shaderRoot, "vs_sky", "fs_sky")),
                      Program(loadProgram(shaderRoot, "vs_world", "fs_overlay")),
                      Program(loadProgram(shaderRoot, "vs_sprite", "fs_distort"))};
    world_ = std::move(next[0]);
    worldInstanced_ = std::move(next[1]);
    sprite_ = std::move(next[2]);
    water_ = std::move(next[3]);
    shadow_ = std::move(next[4]);
    shadowInstanced_ = std::move(next[5]);
    sky_ = std::move(next[6]);
    overlay_ = std::move(next[7]);
    distort_ = std::move(next[8]);
}

void WorldRenderer::render(World& world, std::size_t slot, double alpha, double time, float aspect,
                           const SceneViews& views, SceneTarget& target, std::uint32_t& calls) {
    while (lightingSlots_.size() <= slot) {
        lightingSlots_.push_back(std::make_unique<SceneLighting>());
    }
    auto& lighting = *lightingSlots_[slot];
    const auto& camera = world.camera();
    const auto view = camera.view(alpha);
    const auto projection = camera.projection(aspect, bgfx::getCaps()->homogeneousDepth, alpha);
    viewProjection_ = projection * view;
    lighting.prepare(world, alpha, aspect, viewProjection_, views, atlas_);
    lighting_ = &lighting;
    environment_ = &world.environment();
    views_ = &views;
    target_ = &target;
    calls_ = &calls;
    time_ = time;
    frustum_ = Frustum(viewProjection_);
    cameraPosition_ = camera.position(alpha);
    cameraRight_ = camera.right(alpha);
    cameraUp_ = glm::normalize(glm::cross(cameraRight_, camera.forward(alpha)));
    nearPlane_ = camera.nearPlane;
    farPlane_ = camera.farPlane;
    spriteNormal_ = glm::normalize(-camera.forward(alpha) * 0.6f + glm::vec3(0, 0.4f, 0));
    opaqueCopied_ = false;
    info_ = glm::vec4(1.0f / glm::vec2(target.size), static_cast<float>(time),
                      bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f);
    opaque_.clear();
    casters_.clear();
    parts_.clear();
    transparent_.clear();
    sprites_.clear();
    distortQuads_.clear();
    target.distorted = false;

    setupViews(world, views, target, view, projection);
    drawSky(world, camera.focus(alpha));
    if (const auto* map = gpu_.model(world.geometryPath(), true)) {
        for (const auto& part : map->parts) {
            addPart(part, Identity, glm::vec4(1), true, std::nullopt, -1);
        }
    }
    drawMeshes(world, alpha);
    drawOpaqueParts();
    drawCasters();
    collectSprites(world, alpha);
    collectParticles(world, alpha);
    drawSprites();
    drawOverlays(world);
    drawTransparent();
    drawDistortion();
}

void WorldRenderer::setupViews(World& world, const SceneViews& views, const SceneTarget& target,
                               const glm::mat4& view, const glm::mat4& projection) const {
    for (const auto id : {views.sky, views.opaque, views.overlay, views.transparent, views.distortion}) {
        bgfx::setViewFrameBuffer(id, target.scene.get());
        bgfx::setViewRect(id, 0, 0, static_cast<std::uint16_t>(target.size.x),
                          static_cast<std::uint16_t>(target.size.y));
        bgfx::setViewTransform(id, glm::value_ptr(view), glm::value_ptr(projection));
    }
    bgfx::setViewName(views.sky, "Sky");
    bgfx::setViewName(views.opaque, "Opaque");
    bgfx::setViewName(views.overlay, "Grid overlays");
    bgfx::setViewName(views.transparent, "Transparent");
    // Sprite silhouettes test against the geometry drawn before them.
    bgfx::setViewMode(views.opaque, bgfx::ViewMode::Sequential);
    bgfx::setViewMode(views.overlay, bgfx::ViewMode::Sequential);
    bgfx::setViewMode(views.transparent, bgfx::ViewMode::Sequential);
    // With no sky the background stays transparent, so earlier worlds show through.
    const auto& env = world.environment();
    const auto clear =
        glm::vec4(glm::clamp(env.fogColor, glm::vec3(0), glm::vec3(1)), env.sky.mode == "none" ? 0.0f : 1.0f);
    bgfx::setViewClear(views.sky, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                       (static_cast<std::uint32_t>(clear.r * 255) << 24u) |
                           (static_cast<std::uint32_t>(clear.g * 255) << 16u) |
                           (static_cast<std::uint32_t>(clear.b * 255) << 8u) |
                           static_cast<std::uint32_t>(clear.a * 255),
                       1.0f);
    bgfx::touch(views.sky);
}

void WorldRenderer::drawSky(World& world, glm::vec3 focus) {
    const auto& settings = environment_->sky;
    const float mode = skyMode(settings.mode);
    if (mode > 0) {
        const glm::vec4 sky[Uniforms::SkyCount] = {
            glm::vec4(settings.top, 1), glm::vec4(settings.horizon, 1), glm::vec4(settings.bottom, 1),
            glm::vec4(mode, settings.cloudCover, static_cast<float>(time_) * settings.cloudSpeed, 0),
            glm::vec4(settings.cloudColor, 1)};
        bgfx::setUniform(uniforms_.sky.get(), sky, Uniforms::SkyCount);
        bgfx::setTexture(0, uniforms_.panorama.get(),
                         mode > 2.5f ? gpu_.image(settings.panorama, TextureUse::Panorama) : gpu_.white());
        fullscreen(views_->sky, sky_.get(),
                   BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS, *calls_);
    }
    // Backdrop cards: distant hills, cities, clouds.
    const glm::vec2 centre(static_cast<float>(world.grid().width()) * 0.5f,
                           static_cast<float>(world.grid().depth()) * 0.5f);
    for (const auto& b : settings.backdrops) {
        constexpr std::uint32_t indices[] = {0, 1, 2, 0, 2, 3};
        if (bgfx::getAvailTransientVertexBuffer(4, worldLayout()) < 4 ||
            bgfx::getAvailTransientIndexBuffer(6, true) < 6) {
            return;
        }
        const float yaw = glm::radians(b.yaw);
        const glm::vec3 direction(std::sin(yaw), 0, -std::cos(yaw));
        const glm::vec3 ground =
            glm::mix(focus, glm::vec3(centre.x, 0, centre.y), b.parallax) + direction * b.distance;
        const glm::vec3 anchor(ground.x, b.elevation, ground.z);
        const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), -direction)) * (b.width * 0.5f);
        const glm::vec3 up(0, b.height, 0);
        const WorldVertex vertices[] = {WorldVertex::pack(anchor - right, -direction, {0, 1}, b.tint),
                                        WorldVertex::pack(anchor + right, -direction, {1, 1}, b.tint),
                                        WorldVertex::pack(anchor + right + up, -direction, {1, 0}, b.tint),
                                        WorldVertex::pack(anchor - right + up, -direction, {0, 0}, b.tint)};
        bgfx::TransientVertexBuffer vb;
        bgfx::TransientIndexBuffer ib;
        bgfx::allocTransientVertexBuffer(&vb, 4, worldLayout());
        bgfx::allocTransientIndexBuffer(&ib, 6, true);
        std::memcpy(vb.data, vertices, sizeof(vertices));
        std::memcpy(ib.data, indices, sizeof(indices));
        bgfx::setVertexBuffer(0, &vb);
        bgfx::setIndexBuffer(&ib);
        bgfx::setUniform(uniforms_.normalMatrix.get(), glm::value_ptr(Identity));
        lighting_->bind(uniforms_);
        bgfx::setTexture(0, uniforms_.albedo.get(), gpu_.image(b.image, TextureUse::Scenery));
        bgfx::setTexture(1, uniforms_.normal.get(), gpu_.flatNormal());
        bgfx::setTexture(5, uniforms_.emissive.get(), gpu_.white());
        const glm::vec4 material[Uniforms::MaterialCount] = {glm::vec4(1), glm::vec4(BlendCutoff, 0, 1, 1),
                                                             glm::vec4(0)};
        bgfx::setUniform(uniforms_.material.get(), material, Uniforms::MaterialCount);
        bgfx::setState(OverlayState);
        submitDraw(views_->sky, world_.get(), *calls_);
    }
}

void WorldRenderer::addPart(const GpuPart& part, const glm::mat4& transform, const glm::vec4& tint,
                            bool castShadow, std::optional<BlendMode> blend, int shader) {
    glm::vec3 lo, hi;
    transformBounds(transform, part.min, part.max, lo, hi);
    const auto& material = part.material;
    const bool water = material.kind == MaterialKind::Water;
    const BlendMode mode = water
                               ? BlendMode::Alpha
                               : blend.value_or(material.transparent ? BlendMode::Alpha : BlendMode::Opaque);
    if (castsShadow(castShadow, mode)) {
        casters_.push_back({&part, transform, lo, hi});
    }
    if (!frustum_.visible(lo, hi)) {
        return;
    }
    if (shader < 0 && !material.shader.empty()) {
        if (partShadersRevision_ != gpu_.revision()) {
            partShadersRevision_ = gpu_.revision();
            partShaders_.clear();
        }
        const auto [found, added] = partShaders_.try_emplace(&part, 0);
        if (added) {
            found->second = custom_.slot(material.shader);
        }
        shader = found->second;
    }
    PartDraw draw{&part, transform, glm::mat4(glm::transpose(glm::inverse(glm::mat3(transform)))),
                  tint,  mode,      shader};
    if (blended(mode)) {
        if (water && !opaqueCopied_) {
            target_->copyOpaque(views_->transparent);
            opaqueCopied_ = true;
        }
        transparent_.push_back({{mode, glm::length((lo + hi) * 0.5f - cameraPosition_)},
                                false,
                                static_cast<int>(parts_.size())});
        parts_.push_back(draw);
        return;
    }
    opaque_.push_back(draw);
}

bgfx::ProgramHandle WorldRenderer::partProgram(const PartDraw& draw, bool instanced) {
    if (draw.shader >= 0) {
        const auto custom =
            custom_.program(draw.shader, instanced ? ShaderVertex::WorldInstanced : ShaderVertex::World);
        if (bgfx::isValid(custom)) {
            return custom;
        }
    }
    if (draw.part->material.kind == MaterialKind::Water) {
        return water_.get();
    }
    return instanced ? worldInstanced_.get() : world_.get();
}

void WorldRenderer::bindPart(const PartDraw& draw, std::uint64_t state, bool instanced) {
    const auto& part = *draw.part;
    const auto& m = part.material;
    const bool water = m.kind == MaterialKind::Water;
    if (!instanced) {
        bgfx::setTransform(glm::value_ptr(draw.transform));
        bgfx::setUniform(uniforms_.normalMatrix.get(), glm::value_ptr(draw.normal));
    }
    bgfx::setVertexBuffer(0, part.vertices.get());
    bgfx::setIndexBuffer(part.indices.get());
    lighting_->bind(uniforms_);
    const bool anisotropic = environment_->anisotropic;
    bgfx::setTexture(0, uniforms_.albedo.get(), part.albedo, samplerFlags(m.albedoSampler, anisotropic));
    // Instances carry their tint in the instance data.
    const glm::vec4 material[Uniforms::MaterialCount] = {
        instanced ? m.color : m.color * draw.tint,
        glm::vec4(m.alphaCutoff, part.normal.idx != gpu_.flatNormal().idx ? 1.0f : 0.0f,
                  m.kind == MaterialKind::Unlit ? 1.0f : 0.0f, m.normalScale),
        glm::vec4(m.emissive, blendOutput(draw.blend))};
    bgfx::setUniform(uniforms_.material.get(), material, Uniforms::MaterialCount);
    if (water) {
        WaterFrame frame;
        frame.time = static_cast<float>(time_);
        frame.nearPlane = nearPlane_;
        frame.farPlane = farPlane_;
        frame.horizon = environment_->sky.horizon;
        frame.sceneCopy = opaqueCopied_;
        frame.originBottomLeft = bgfx::getCaps()->originBottomLeft;
        const auto settings = packWater(environment_->water, frame);
        bgfx::setUniform(uniforms_.water.get(), settings.data(), Uniforms::WaterCount);
        bgfx::setTexture(1, uniforms_.sceneDepth.get(), target_->depthCopy.get(), PointClamp);
        bgfx::setTexture(6, uniforms_.sceneColor.get(), target_->colorCopy.get(), LinearClamp);
    } else {
        bgfx::setTexture(1, uniforms_.normal.get(), part.normal, samplerFlags(m.normalSampler, anisotropic));
        bgfx::setTexture(5, uniforms_.emissive.get(), part.emission,
                         samplerFlags(m.emissionSampler, anisotropic));
    }
    if (draw.shader >= 0) {
        bgfx::setUniform(uniforms_.info.get(), glm::value_ptr(info_));
    }
    bgfx::setState(state | cull(m));
}

void WorldRenderer::submitPart(std::uint16_t view, const PartDraw& draw, std::uint64_t state) {
    bindPart(draw, state, false);
    submitDraw(view, partProgram(draw, false), *calls_);
}

void WorldRenderer::drawOpaqueParts() {
    keys_.clear();
    for (const auto& draw : opaque_) {
        keys_.emplace_back(draw.part, draw.shader);
    }
    groupByFirstUse(keys_, order_);
    for (std::size_t first = 0; first < order_.size();) {
        std::size_t last = first + 1;
        while (last < order_.size() && keys_[static_cast<std::size_t>(order_[last])] ==
                                           keys_[static_cast<std::size_t>(order_[first])]) {
            ++last;
        }
        const auto count = static_cast<std::uint32_t>(last - first);
        if (count == 1 || bgfx::getAvailInstanceDataBuffer(count, InstanceStride) < count) {
            for (std::size_t k = first; k < last; ++k) {
                submitPart(views_->opaque, opaque_[static_cast<std::size_t>(order_[k])], OpaqueState);
            }
        } else {
            bgfx::InstanceDataBuffer instances;
            bgfx::allocInstanceDataBuffer(&instances, count, InstanceStride);
            auto* out = instances.data;
            for (std::size_t k = first; k < last; ++k) {
                const auto& draw = opaque_[static_cast<std::size_t>(order_[k])];
                std::memcpy(out, glm::value_ptr(draw.transform), sizeof(glm::mat4));
                std::memcpy(out + sizeof(glm::mat4), glm::value_ptr(draw.tint), sizeof(glm::vec4));
                out += InstanceStride;
            }
            const auto& draw = opaque_[static_cast<std::size_t>(order_[first])];
            bindPart(draw, OpaqueState, true);
            bgfx::setInstanceDataBuffer(&instances);
            submitDraw(views_->opaque, partProgram(draw, true), *calls_);
        }
        first = last;
    }
}

void WorldRenderer::drawCasters() {
    const auto cast = [&](std::uint16_t view, const Frustum& volume) {
        visible_.clear();
        for (std::size_t i = 0; i < casters_.size(); ++i) {
            if (volume.visible(casters_[i].min, casters_[i].max)) {
                visible_.push_back(static_cast<int>(i));
            }
        }
        castShadow(view, visible_);
    };
    for (std::size_t c = 0; lighting_->shadows() && c < ShadowCascadeCount; ++c) {
        cast(views_->shadow[c], lighting_->cascadeFrusta()[c]);
    }
    for (const auto& tile : lighting_->lightTiles()) {
        cast(tile.view, tile.frustum);
    }
}

void WorldRenderer::castShadow(std::uint16_t view, const std::vector<int>& casters) {
    keys_.clear();
    for (const auto index : casters) {
        keys_.emplace_back(casters_[static_cast<std::size_t>(index)].part, 0);
    }
    groupByFirstUse(keys_, order_);
    const auto bind = [&](const GpuPart& part) {
        const auto& material = part.material;
        const glm::vec4 shadowMaterial[Uniforms::MaterialCount] = {
            glm::vec4(1), glm::vec4(material.alphaCutoff, 0, 0, 0), glm::vec4(0)};
        bgfx::setVertexBuffer(0, part.vertices.get());
        bgfx::setIndexBuffer(part.indices.get());
        bgfx::setTexture(0, uniforms_.albedo.get(), part.albedo, samplerFlags(material.albedoSampler, false));
        bgfx::setUniform(uniforms_.material.get(), shadowMaterial, Uniforms::MaterialCount);
        bgfx::setState(ShadowState | cull(material));
    };
    const auto caster = [&](std::size_t k) -> const Caster& {
        return casters_[static_cast<std::size_t>(casters[static_cast<std::size_t>(order_[k])])];
    };
    for (std::size_t first = 0; first < order_.size();) {
        std::size_t last = first + 1;
        while (last < order_.size() && caster(last).part == caster(first).part) {
            ++last;
        }
        const auto count = static_cast<std::uint32_t>(last - first);
        if (count == 1 || bgfx::getAvailInstanceDataBuffer(count, ShadowInstanceStride) < count) {
            for (std::size_t k = first; k < last; ++k) {
                bgfx::setTransform(glm::value_ptr(caster(k).transform));
                bind(*caster(k).part);
                submitDraw(view, shadow_.get(), *calls_);
            }
        } else {
            bgfx::InstanceDataBuffer instances;
            bgfx::allocInstanceDataBuffer(&instances, count, ShadowInstanceStride);
            auto* out = instances.data;
            for (std::size_t k = first; k < last; ++k) {
                std::memcpy(out, glm::value_ptr(caster(k).transform), sizeof(glm::mat4));
                out += ShadowInstanceStride;
            }
            bind(*caster(first).part);
            bgfx::setInstanceDataBuffer(&instances);
            submitDraw(view, shadowInstanced_.get(), *calls_);
        }
        first = last;
    }
}

void WorldRenderer::drawMeshes(World& world, double alpha) {
    auto& registry = world.registry();
    const auto t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    for (const auto entity : registry.view<Transform, MeshInstance>()) {
        const auto& mesh = registry.get<MeshInstance>(entity);
        if (!mesh.visible) {
            continue;
        }
        auto& cache = registry.get_or_emplace<MeshDrawCache>(entity);
        if (cache.revision != gpu_.revision() || cache.model != mesh.model) {
            cache.revision = gpu_.revision();
            cache.model = mesh.model;
            cache.gpu = gpu_.model(mesh.model, false);
        }
        if (cache.shader != mesh.shader) {
            cache.shader = mesh.shader;
            cache.shaderSlot = shaderSlot(custom_, mesh.shader);
        }
        const auto* model = cache.gpu;
        if (!model) {
            continue;
        }
        const auto& transform = registry.get<Transform>(entity);
        const float turn = std::remainder(transform.yaw - transform.previousYaw, glm::two_pi<float>());
        const auto base = glm::translate(glm::mat4(1), world.interpolated(entity, alpha)) *
                          glm::rotate(glm::mat4(1), transform.previousYaw + turn * t, glm::vec3(0, 1, 0)) *
                          glm::scale(glm::mat4(1), transform.scale);
        model->source->pose(model->source->animation(mesh.animation), mesh.time, pose_);
        for (const auto& part : model->parts) {
            addPart(part, base * pose_[static_cast<std::size_t>(part.node)], mesh.tint, mesh.castShadow,
                    mesh.blend, cache.shaderSlot);
        }
    }
}

void WorldRenderer::collectSprites(World& world, double alpha) {
    auto& registry = world.registry();
    const auto& camera = world.camera();
    const float probeHeight = environment_->spriteLightHeight;
    const auto packedSpriteNormal = packDirection(spriteNormal_);
    for (const auto entity : registry.view<Transform, Sprite>()) {
        const auto& s = registry.get<Sprite>(entity);
        if (!s.visible || s.animation.empty()) {
            continue;
        }
        auto& cache = registry.get_or_emplace<SpriteDrawCache>(entity);
        if (cache.revision != gpu_.revision() || cache.animation != s.animation ||
            cache.palette != s.palette) {
            cache.revision = gpu_.revision();
            cache.animation = s.animation;
            cache.palette = s.palette;
            cache.sheet = gpu_.spriteSheet(s.animation);
            const bool palette = cache.sheet && !s.palette.empty();
            cache.indexAtlas = palette ? gpu_.image(cache.sheet->atlasPath, TextureUse::SpriteIndex)
                                       : bgfx::TextureHandle BGFX_INVALID_HANDLE;
            cache.paletteTexture = palette ? gpu_.image(s.palette, TextureUse::Sprite) : gpu_.white();
            cache.paletteRows = palette ? static_cast<float>(std::max(1, gpu_.imageSize(s.palette).y)) : 1.0f;
        }
        if (cache.shader != s.shader) {
            cache.shader = s.shader;
            cache.shaderSlot = shaderSlot(custom_, s.shader);
        }
        const auto* sheet = cache.sheet;
        if (!sheet) {
            continue;
        }
        const auto& animation = *sheet->animation;
        const auto& transform = registry.get<Transform>(entity);
        const auto feet = world.interpolated(entity, alpha);
        auto placement = placeSprite(s, animation, feet, transform.facing, camera, alpha, pixelsPerUnit_);
        if (s.pixelSnap.value_or(environment_->pixelSnap)) {
            placement.anchor =
                snapToPixel(placement.anchor, viewProjection_, cameraRight_, cameraUp_, target_->size);
        }
        const auto& frame = *placement.frame;
        const auto span = placement.quad();
        const auto bottomLeft = span.corner, right = placement.right;
        const bool lying = placement.lying;
        const glm::vec2 atlas(sheet->size);
        const glm::vec2 uv0 = glm::vec2(frame.rect.x, frame.rect.y) / atlas;
        const glm::vec2 uv1 = glm::vec2(frame.rect.x + frame.rect.z, frame.rect.y + frame.rect.w) / atlas;
        const bool palette = !s.palette.empty();
        SpriteBatch::Material material;
        material.atlas = palette ? cache.indexAtlas : sheet->color;
        material.palette = cache.paletteTexture;
        material.normal = sheet->normal;
        material.emissive = sheet->emissive;
        material.texel = 1.0f / atlas;
        material.paletteSwap = palette;
        if (cache.shaderSlot >= 0) {
            material.program = custom_.program(cache.shaderSlot, ShaderVertex::Sprite);
        }
        const float paletteRows = cache.paletteRows;
        const BlendMode blend = spriteBlend(s.blend, s.tint.a);
        SpriteVertex v;
        v.tint = s.tint;
        v.flash = packRgba8(s.flash);
        v.outline = packRgba8(s.outline);
        v.probe = glm::vec4(feet + glm::vec3(0, probeHeight, 0), s.unlit ? 1.0f : 0.0f);
        v.params = glm::vec2((static_cast<float>(s.paletteRow) + 0.5f) / paletteRows, 0);
        v.normal = lying ? packDirection(glm::vec3(0, 1, 0)) : packedSpriteNormal;
        v.tangent = packDirection(right);
        if (s.mode == SpriteMode::Decal) {
            addDecal(world, material, v, transform.layer, bottomLeft, placement.size, uv0, uv1, blend);
            continue;
        }
        std::array<SpriteVertex, 4> quad{v, v, v, v};
        quad[0].position = bottomLeft;
        quad[0].uv = {uv0.x, uv1.y};
        quad[1].position = bottomLeft + span.right;
        quad[1].uv = {uv1.x, uv1.y};
        quad[2].position = bottomLeft + span.right + span.up;
        quad[2].uv = {uv1.x, uv0.y};
        quad[3].position = bottomLeft + span.up;
        quad[3].uv = {uv0.x, uv0.y};
        glm::vec3 lo = glm::min(quad[0].position, quad[2].position),
                  hi = glm::max(quad[0].position, quad[2].position);
        const bool visible = frustum_.visible(lo - glm::vec3(0.1f), hi + glm::vec3(0.1f));
        if (s.distort > 0) {
            if (visible) {
                addDistortion(material.atlas, material.texel, quad, s.distort);
            }
            continue;
        }
        SpriteBatch::Quad settings;
        settings.blend = blend;
        settings.visible = visible;
        settings.castShadow = castsShadow(s.castShadow, blend);
        sprites_.add(material, quad, settings);
        if (visible && s.silhouette.a > 0 && !lying) {
            // Pulled towards the camera along each view ray, so it covers the
            // same pixels but shows only behind geometry nearer than the margin.
            for (auto& corner : quad) {
                corner.position += glm::normalize(cameraPosition_ - corner.position) * SilhouetteMargin;
                corner.outline = packRgba8(s.silhouette);
            }
            SpriteBatch::Quad silhouette;
            silhouette.silhouette = true;
            sprites_.add(material, quad, silhouette);
        }
    }
}

void WorldRenderer::addDecal(World& world, const SpriteBatch::Material& material, const SpriteVertex& base,
                             int layer, glm::vec3 bottomLeft, glm::vec2 size, glm::vec2 uv0, glm::vec2 uv1,
                             BlendMode blend) {
    decal_.clear();
    const glm::vec2 min(bottomLeft.x, bottomLeft.z - size.y), max(bottomLeft.x + size.x, bottomLeft.z);
    projectDecal(world.grid(), layer, min, max, uv0, uv1, FlatSpriteLift, decal_);
    for (const auto& piece : decal_) {
        std::array<SpriteVertex, 4> quad{base, base, base, base};
        for (std::size_t k = 0; k < 4; ++k) {
            quad[k].position = piece.positions[k];
            quad[k].uv = piece.uv[k];
            quad[k].normal = packDirection(piece.normal);
        }
        const glm::vec3 lo = glm::min(glm::min(piece.positions[0], piece.positions[1]),
                                      glm::min(piece.positions[2], piece.positions[3])),
                        hi = glm::max(glm::max(piece.positions[0], piece.positions[1]),
                                      glm::max(piece.positions[2], piece.positions[3]));
        SpriteBatch::Quad settings;
        settings.blend = blend;
        settings.visible = frustum_.visible(lo, hi);
        sprites_.add(material, quad, settings);
    }
}

void WorldRenderer::collectParticles(World& world, double alpha) {
    const auto& particles = world.particles();
    if (particles.empty()) {
        return;
    }
    const auto& camera = world.camera();
    const auto right = camera.right(alpha), forward = camera.forward(alpha);
    const auto up = glm::normalize(glm::cross(right, forward));
    const float focusDistance = camera.interpolatedDistance(alpha);
    const auto& paths = world.particleSprites();
    auto& sheets = world.registry().ctx().emplace<ParticleSheetCache>();
    if (sheets.revision != gpu_.revision()) {
        sheets.revision = gpu_.revision();
        sheets.entries.clear();
    }
    sheets.entries.resize(std::max(sheets.entries.size(), paths.size()));
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto& entry = sheets.entries[i];
        if (entry.path != paths[i]) {
            entry.path = paths[i];
            entry.sheet = entry.path.empty() ? nullptr : gpu_.spriteSheet(entry.path);
            entry.clip = entry.sheet ? &entry.sheet->animation->clip("idle") : nullptr;
        }
    }
    const auto spark = gpu_.image("@fe/assets/ui/spark.png", TextureUse::Scenery);
    const auto normal = packDirection(spriteNormal_);
    for (const auto& p : particles) {
        const float fade = particleNearFade(glm::dot(p.position - cameraPosition_, forward), focusDistance);
        if (fade <= 0) {
            continue;
        }
        const float t = p.age / p.lifetime;
        const float size = glm::mix(p.size, p.sizeEnd, t);
        glm::vec3 axisX = right * size, axisY = up * size;
        if (p.stretch > 0 && glm::length(p.velocity) > 0.01f) {
            axisY = glm::normalize(p.velocity) * (size + glm::length(p.velocity) * p.stretch);
            axisX = glm::normalize(glm::cross(axisY, forward)) * size;
        }
        const auto& cached = sheets.entries[p.sprite];
        const auto* sheet = cached.sheet;
        SpriteBatch::Material material;
        material.palette = gpu_.white();
        glm::vec2 uv0(0), uv1(1);
        if (sheet) {
            const auto& frame = cached.clip->frameAt(0, p.age);
            const glm::vec2 atlas(sheet->size);
            uv0 = glm::vec2(frame.rect.x, frame.rect.y) / atlas;
            uv1 = glm::vec2(frame.rect.x + frame.rect.z, frame.rect.y + frame.rect.w) / atlas;
            material.atlas = sheet->color;
            material.normal = sheet->normal;
            material.emissive = sheet->emissive;
            material.texel = 1.0f / atlas;
        } else {
            material.atlas = spark;
        }
        SpriteVertex v;
        v.tint = glm::mix(p.color, p.colorEnd, t);
        v.tint.a *= fade;
        v.probe = glm::vec4(p.position, p.unlit ? 1.0f : 0.0f);
        v.normal = normal;
        v.tangent = packDirection(glm::normalize(axisX));
        std::array<SpriteVertex, 4> quad{v, v, v, v};
        quad[0].position = p.position - axisX - axisY;
        quad[0].uv = {uv0.x, uv1.y};
        quad[1].position = p.position + axisX - axisY;
        quad[1].uv = {uv1.x, uv1.y};
        quad[2].position = p.position + axisX + axisY;
        quad[2].uv = {uv1.x, uv0.y};
        quad[3].position = p.position - axisX + axisY;
        quad[3].uv = {uv0.x, uv0.y};
        const glm::vec3 extent = glm::abs(axisX) + glm::abs(axisY);
        if (!frustum_.visible(p.position - extent, p.position + extent)) {
            continue;
        }
        if (p.distort > 0) {
            addDistortion(material.atlas, material.texel, quad, p.distort);
        } else {
            SpriteBatch::Quad settings;
            settings.blend = p.blend;
            sprites_.add(material, quad, settings);
        }
    }
}

void WorldRenderer::drawSprites() {
    if (!sprites_.upload()) {
        return;
    }
    const auto& quads = sprites_.quads();
    auto byMaterial = [&](int a, int b) {
        return quads[static_cast<std::size_t>(a)].material < quads[static_cast<std::size_t>(b)].material;
    };
    const SpriteBatch::Context shadowContext{uniforms_, nullptr, gpu_.white(), gpu_.flatNormal(), info_};
    const SpriteBatch::Context context{uniforms_, lighting_, gpu_.white(), gpu_.flatNormal(), info_};
    const auto castSprites = [&](std::uint16_t view, const Frustum& volume) {
        quadList_.clear();
        for (std::size_t i = 0; i < quads.size(); ++i) {
            if (quads[i].castShadow && volume.visible(quads[i].min, quads[i].max)) {
                quadList_.push_back(static_cast<int>(i));
            }
        }
        std::stable_sort(quadList_.begin(), quadList_.end(), byMaterial);
        sprites_.draw(view, quadList_, shadow_.get(), SpriteBatch::Pass::Shadow, shadowContext, *calls_);
    };
    for (std::size_t c = 0; lighting_->shadows() && c < ShadowCascadeCount; ++c) {
        castSprites(views_->shadow[c], lighting_->cascadeFrusta()[c]);
    }
    for (const auto& tile : lighting_->lightTiles()) {
        castSprites(tile.view, tile.frustum);
    }
    quadList_.clear();
    for (std::size_t i = 0; i < quads.size(); ++i) {
        if (quads[i].silhouette) {
            quadList_.push_back(static_cast<int>(i));
        }
    }
    sprites_.draw(views_->opaque, quadList_, sprite_.get(), SpriteBatch::Pass::Silhouette, context, *calls_);
    quadList_.clear();
    for (std::size_t i = 0; i < quads.size(); ++i) {
        if (!quads[i].visible || quads[i].silhouette) {
            continue;
        }
        if (blended(quads[i].blend)) {
            transparent_.push_back({{quads[i].blend, glm::length(quads[i].centre - cameraPosition_)},
                                    true,
                                    static_cast<int>(i)});
        } else {
            quadList_.push_back(static_cast<int>(i));
        }
    }
    std::stable_sort(quadList_.begin(), quadList_.end(), byMaterial);
    sprites_.draw(views_->opaque, quadList_, sprite_.get(), SpriteBatch::Pass::Opaque, context, *calls_);
}

void WorldRenderer::drawOverlays(World& world) {
    const auto& grid = world.grid();
    std::vector<const OverlayLayer*> layers;
    for (const auto& entry : world.overlays()) {
        if (entry.second.visible && !entry.second.cells.empty()) {
            layers.push_back(&entry.second);
        }
    }
    std::stable_sort(layers.begin(), layers.end(),
                     [](const OverlayLayer* a, const OverlayLayer* b) { return a->order < b->order; });
    for (const auto* layer : layers) {
        overlayMesh_.clear();
        for (const auto& cell : layer->cells) {
            if (grid.contains(cell)) {
                addCellQuad(grid, cell, 0.03f + static_cast<float>(layer->order) * 0.004f, 0.02f,
                            layer->color, overlayMesh_);
            }
        }
        submitOverlay(layer->pulse, layer->border);
    }
    for (const auto& entry : world.overlayPaths()) {
        overlayMesh_.clear();
        addPathArrow(grid, entry.second.cells, entry.second.color, overlayMesh_);
        submitOverlay(0, false);
    }
    const auto& cursor = world.cursor();
    if (cursor.visible && grid.contains(cursor.cell)) {
        overlayMesh_.clear();
        addCellQuad(grid, cursor.cell, 0.08f, -0.04f, cursor.color, overlayMesh_);
        submitOverlay(3, true);
    }
}

void WorldRenderer::submitOverlay(float pulse, bool border) {
    const auto vc = static_cast<std::uint32_t>(overlayMesh_.vertices.size()),
               ic = static_cast<std::uint32_t>(overlayMesh_.indices.size());
    if (vc == 0 || bgfx::getAvailTransientVertexBuffer(vc, worldLayout()) < vc ||
        bgfx::getAvailTransientIndexBuffer(ic, true) < ic) {
        return;
    }
    overlayVertices_.clear();
    for (const auto& v : overlayMesh_.vertices) {
        overlayVertices_.push_back(WorldVertex::pack(v.position, glm::vec3(0, 1, 0), v.uv, v.color));
    }
    bgfx::TransientVertexBuffer vb;
    bgfx::TransientIndexBuffer ib;
    bgfx::allocTransientVertexBuffer(&vb, vc, worldLayout());
    bgfx::allocTransientIndexBuffer(&ib, ic, true);
    std::memcpy(vb.data, overlayVertices_.data(), vc * sizeof(WorldVertex));
    std::memcpy(ib.data, overlayMesh_.indices.data(), ic * sizeof(std::uint32_t));
    bgfx::setVertexBuffer(0, &vb);
    bgfx::setIndexBuffer(&ib);
    bgfx::setUniform(uniforms_.normalMatrix.get(), glm::value_ptr(Identity));
    lighting_->bind(uniforms_);
    const glm::vec4 overlay(static_cast<float>(time_), pulse, border ? 1.0f : 0.0f, 0);
    bgfx::setUniform(uniforms_.overlay.get(), glm::value_ptr(overlay));
    bgfx::setState(OverlayState);
    submitDraw(views_->overlay, overlay_.get(), *calls_);
}

void WorldRenderer::drawTransparent() {
    std::stable_sort(transparent_.begin(), transparent_.end(),
                     [](const Transparent& a, const Transparent& b) { return drawsBefore(a.key, b.key); });
    quadList_.clear();
    const SpriteBatch::Context context{uniforms_, lighting_, gpu_.white(), gpu_.flatNormal(), info_};
    auto flushSprites = [&] {
        sprites_.draw(views_->transparent, quadList_, sprite_.get(), SpriteBatch::Pass::Transparent, context,
                      *calls_);
        quadList_.clear();
    };
    for (const auto& draw : transparent_) {
        if (draw.sprite) {
            quadList_.push_back(draw.index);
            continue;
        }
        flushSprites();
        const auto& part = parts_[static_cast<std::size_t>(draw.index)];
        submitPart(views_->transparent, part,
                   BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS |
                       blendFunction(part.blend));
    }
    flushSprites();
}

void WorldRenderer::addDistortion(bgfx::TextureHandle atlas, glm::vec2 texel,
                                  std::array<SpriteVertex, 4> quad, float strength) {
    for (auto& v : quad) {
        v.params.y = strength;
    }
    distortQuads_.push_back({atlas, texel, quad});
}

void WorldRenderer::drawDistortion() {
    if (distortQuads_.empty()) {
        return;
    }
    const auto view = views_->distortion;
    bgfx::setViewName(view, "Distortion");
    bgfx::setViewFrameBuffer(view, target_->distortionTarget());
    bgfx::setViewRect(view, 0, 0, static_cast<std::uint16_t>(target_->size.x),
                      static_cast<std::uint16_t>(target_->size.y));
    bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    bgfx::touch(view);
    target_->distorted = true;
    std::stable_sort(distortQuads_.begin(), distortQuads_.end(),
                     [](const DistortQuad& a, const DistortQuad& b) { return a.atlas.idx < b.atlas.idx; });
    for (std::size_t first = 0; first < distortQuads_.size();) {
        std::size_t last = first;
        distortVertices_.clear();
        while (last < distortQuads_.size() &&
               distortQuads_[last].atlas.idx == distortQuads_[first].atlas.idx) {
            const auto& v = distortQuads_[last].vertices;
            for (const int i : {0, 1, 2, 0, 2, 3}) {
                distortVertices_.push_back(v[static_cast<std::size_t>(i)]);
            }
            ++last;
        }
        const auto count = static_cast<std::uint32_t>(distortVertices_.size());
        if (bgfx::getAvailTransientVertexBuffer(count, spriteLayout()) < count) {
            return;
        }
        bgfx::TransientVertexBuffer vb;
        bgfx::allocTransientVertexBuffer(&vb, count, spriteLayout());
        std::memcpy(vb.data, distortVertices_.data(), count * sizeof(SpriteVertex));
        bgfx::setVertexBuffer(0, &vb);
        bgfx::setTexture(0, uniforms_.albedo.get(), distortQuads_[first].atlas, PointClamp);
        bgfx::setTexture(1, uniforms_.sceneDepth.get(), target_->depth.get(), PointClamp);
        const glm::vec4 settings[2] = {glm::vec4(static_cast<float>(time_), distortQuads_[first].texel, 0),
                                       glm::vec4(1.0f / glm::vec2(target_->size), 0, 0)};
        bgfx::setUniform(distortUniform_.get(), settings, 2);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ADD);
        submitDraw(view, distort_.get(), *calls_);
        first = last;
    }
}
} // namespace fe
