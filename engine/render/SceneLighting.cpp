#include "engine/render/SceneLighting.h"
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

namespace fe {
namespace {
constexpr std::uint32_t ShadowSampler =
    BGFX_SAMPLER_COMPARE_LEQUAL | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
// Height above the map that sprites, meshes and props may cast shadows from.
constexpr float CasterHeadroom = 8;
// Lights this far outside the grid still reach its edge cells.
constexpr float LightMargin = 8;
} // namespace

ShadowAtlas::ShadowAtlas() {
    depth_ = Texture(bgfx::createTexture2D(Resolution * ShadowCascadeCount, Resolution, false, 1,
                                           bgfx::TextureFormat::D16, BGFX_TEXTURE_RT | ShadowSampler));
    const bgfx::TextureHandle attachments[] = {depth_.get()};
    buffer_ = Framebuffer(bgfx::createFrameBuffer(1, attachments, false));
    lightDepth_ =
        Texture(bgfx::createTexture2D(LightShadowTileResolution * LightShadowTiles, LightShadowTileResolution,
                                      false, 1, bgfx::TextureFormat::D16, BGFX_TEXTURE_RT | ShadowSampler));
    const bgfx::TextureHandle lightAttachments[] = {lightDepth_.get()};
    lightBuffer_ = Framebuffer(bgfx::createFrameBuffer(1, lightAttachments, false));
}

SceneLighting::SceneLighting() {
    lights_ = Texture(bgfx::createTexture2D(LightGrid::MaxLights, LightGrid::Rows, false, 1,
                                            bgfx::TextureFormat::RGBA32F, PointClamp));
}

void SceneLighting::prepare(World& world, double alpha, float aspect, const glm::mat4& viewProjection,
                            const SceneViews& views, const ShadowAtlas& atlas) {
    const auto& env = world.environment();
    const auto& camera = world.camera();
    const auto& grid = world.grid();

    sceneLights_.clear();
    auto add = [&](const LightSettings& light, glm::vec3 offset, float seed) {
        auto& added = sceneLights_.emplace_back(SceneLight{light, 0});
        added.settings.position += offset;
        added.intensity = light.intensity * light.flickerScale(world.time(), seed);
    };
    const auto& mapLights = world.map().lights;
    for (const auto& light : mapLights) {
        add(light, glm::vec3(0), flickerSeed(light));
    }
    auto& registry = world.registry();
    for (const auto entity : registry.view<Transform, LightSource>()) {
        const auto& source = registry.get<LightSource>(entity);
        if (source.enabled) {
            add(source.light, world.interpolated(entity, alpha), flickerSeed(entity));
        }
    }
    const glm::vec2 extent(static_cast<float>(grid.width()), static_cast<float>(grid.depth()));
    grid_.build(sceneLights_, glm::vec2(-LightMargin), extent + LightMargin, camera.focus(alpha));
    prepareLightShadows(world.camera().focus(alpha), Frustum(viewProjection), views, atlas);
    bgfx::updateTexture2D(
        lights_.get(), 0, 0, 0, 0, LightGrid::MaxLights, LightGrid::Rows,
        bgfx::copy(grid_.lightTexels.data(),
                   static_cast<std::uint32_t>(grid_.lightTexels.size() * sizeof(glm::vec4))));
    const glm::ivec2 cellTexture(grid_.cells.x * LightGrid::MaxPerCell / 4, grid_.cells.y);
    if (cellTexture != cellTextureSize_) {
        cellTextureSize_ = cellTexture;
        cells_ = Texture(bgfx::createTexture2D(static_cast<std::uint16_t>(cellTexture.x),
                                               static_cast<std::uint16_t>(cellTexture.y), false, 1,
                                               bgfx::TextureFormat::RGBA32F, PointClamp));
    }
    bgfx::updateTexture2D(cells_.get(), 0, 0, 0, 0, static_cast<std::uint16_t>(cellTexture.x),
                          static_cast<std::uint16_t>(cellTexture.y),
                          bgfx::copy(grid_.cellTexels.data(),
                                     static_cast<std::uint32_t>(grid_.cellTexels.size() * sizeof(float))));

    ShadowCamera view;
    view.position = camera.position(alpha) - camera.shake;
    view.forward = camera.forward(alpha);
    view.right = camera.right(alpha);
    view.up = glm::normalize(glm::cross(view.right, view.forward));
    view.fovY = glm::radians(camera.interpolatedFov(alpha));
    view.aspect = aspect;
    view.nearPlane = camera.nearPlane;
    const float distance = camera.interpolatedDistance(alpha);
    view.farPlane = std::max(camera.nearPlane + 1, std::min(camera.farPlane, distance + env.shadowDistance));
    view.splitStart = distance * 0.5f;
    ShadowSettings settings;
    settings.sunDirection = env.sunDirection;
    settings.sceneMin = glm::vec3(0);
    settings.sceneMax = glm::vec3(extent.x, CasterHeadroom, extent.y);
    if (const auto model = world.assets().tryModel(world.geometryPath())) {
        settings.sceneMin = glm::min(settings.sceneMin, model->min);
        settings.sceneMax = glm::max(settings.sceneMax, model->max + glm::vec3(0, CasterHeadroom, 0));
    }
    settings.resolution = ShadowAtlas::Resolution;
    settings.bias = env.shadowBias;
    settings.homogeneousDepth = bgfx::getCaps()->homogeneousDepth;
    settings.originBottomLeft = bgfx::getCaps()->originBottomLeft;
    const auto cascades = fitShadowCascades(view, settings);
    shadows_ = env.shadows;
    atlas_ = atlas.texture();
    for (std::size_t c = 0; c < cascades.size(); ++c) {
        cascades_[c] = cascades[c].sample;
        frusta_[c] = Frustum(cascades[c].projection * cascades[c].view);
        if (!shadows_) {
            continue;
        }
        const auto id = views.shadow[c];
        bgfx::setViewName(id, "Shadow cascade");
        bgfx::setViewFrameBuffer(id, atlas.framebuffer());
        bgfx::setViewRect(id, static_cast<std::uint16_t>(c * ShadowAtlas::Resolution), 0,
                          ShadowAtlas::Resolution, ShadowAtlas::Resolution);
        bgfx::setViewTransform(id, glm::value_ptr(cascades[c].view), glm::value_ptr(cascades[c].projection));
        bgfx::setViewClear(id, BGFX_CLEAR_DEPTH, 0, 1.0f);
        bgfx::touch(id);
    }

    constexpr float atlasWidth = ShadowAtlas::Resolution * ShadowCascadeCount;
    env_ = {
        glm::vec4(env.ambient, 1),
        glm::vec4(glm::normalize(env.sunDirection), 0),
        glm::vec4(env.sunColor, 1),
        glm::vec4(camera.position(alpha), 1),
        glm::vec4(env.fogColor, env.fogDensity),
        glm::vec4(env.fogHeight, env.fogHeightFalloff, env.fogMax, 0),
        glm::vec4(shadows_ ? env.shadowStrength : 0.0f, 1.0f / atlasWidth, 1.0f / ShadowAtlas::Resolution, 0),
        glm::vec4(grid_.origin, 1.0f / grid_.cellSize, static_cast<float>(grid_.cells.x)),
        glm::vec4(static_cast<float>(grid_.cells.y), 1.0f / static_cast<float>(cellTexture.x),
                  1.0f / static_cast<float>(cellTexture.y), 1.0f / LightGrid::MaxLights),
        glm::vec4(lightTiles_.empty() ? 0.0f : 1.0f, 1.0f / (LightShadowTileResolution * LightShadowTiles),
                  1.0f / LightShadowTileResolution, 0)};
}

void SceneLighting::prepareLightShadows(glm::vec3 focus, const Frustum& view, const SceneViews& views,
                                        const ShadowAtlas& atlas) {
    lightTiles_.clear();
    lightAtlas_ = atlas.lightTexture();
    shadowCandidates_.clear();
    for (int slot = 0; slot < grid_.lightCount; ++slot) {
        const auto& light = sceneLights_[grid_.sources[static_cast<std::size_t>(slot)]].settings;
        const glm::vec3 reach(light.radius);
        const bool visible = view.visible(light.position - reach, light.position + reach);
        shadowCandidates_.push_back(
            {visible ? lightShadowPriority(light, focus) : 0.0f, light.type == LightType::Point});
    }
    const auto& caps = *bgfx::getCaps();
    for (const auto& slot : chooseShadowLights(shadowCandidates_)) {
        const auto& light = sceneLights_[grid_.sources[slot.light]].settings;
        grid_.setShadow(static_cast<int>(slot.light), slot.firstTile, slot.tiles,
                        lightShadowTexelScale(light));
        const auto tiles =
            lightShadowViews(light, slot.firstTile, caps.homogeneousDepth, caps.originBottomLeft);
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const int tile = slot.firstTile + static_cast<int>(i);
            const auto id = views.lightShadow[static_cast<std::size_t>(tile)];
            lightShadows_[static_cast<std::size_t>(tile)] = tiles[i].sample;
            lightTiles_.push_back({id, Frustum(tiles[i].projection * tiles[i].view)});
            bgfx::setViewName(id, "Light shadow");
            bgfx::setViewFrameBuffer(id, atlas.lightFramebuffer());
            bgfx::setViewRect(id, static_cast<std::uint16_t>(tile * LightShadowTileResolution), 0,
                              LightShadowTileResolution, LightShadowTileResolution);
            bgfx::setViewTransform(id, glm::value_ptr(tiles[i].view), glm::value_ptr(tiles[i].projection));
            bgfx::setViewClear(id, BGFX_CLEAR_DEPTH, 0, 1.0f);
            bgfx::touch(id);
        }
    }
}

void SceneLighting::bind(const Uniforms& uniforms) const {
    bgfx::setUniform(uniforms.env.get(), env_.data(), Uniforms::EnvCount);
    bgfx::setUniform(uniforms.cascades.get(), cascades_.data(), Uniforms::CascadeCount);
    bgfx::setTexture(2, uniforms.shadow.get(), atlas_, ShadowSampler);
    bgfx::setTexture(3, uniforms.lights.get(), lights_.get(), PointClamp);
    bgfx::setTexture(4, uniforms.lightGrid.get(), cells_.get(), PointClamp);
    bgfx::setUniform(uniforms.lightShadows.get(), lightShadows_.data(), LightShadowTiles);
    bgfx::setTexture(7, uniforms.lightShadow.get(), lightAtlas_, ShadowSampler);
}
} // namespace fe
