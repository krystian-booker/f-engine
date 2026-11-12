#include "engine/assets/Model.h"
#include "engine/render/DrawOrder.h"
#include "engine/render/FrameMath.h"
#include "engine/render/LightGrid.h"
#include "engine/render/MeshChunks.h"
#include "engine/render/OverlayGeometry.h"
#include "engine/render/ShadowCascades.h"
#include "engine/render/SpriteGeometry.h"
#include "engine/world/MapData.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

using namespace fe;

namespace {
SceneLight light(glm::vec3 position, float radius, float intensity, float flickered) {
    SceneLight out;
    out.settings.position = position;
    out.settings.radius = radius;
    out.settings.intensity = intensity;
    out.settings.color = glm::vec3(1);
    out.intensity = flickered;
    return out;
}

ShadowCamera orbitCamera(glm::vec3 target) {
    ShadowCamera camera;
    camera.forward = glm::normalize(glm::vec3(0, -0.67f, -0.74f));
    camera.position = target - camera.forward * 17.0f;
    camera.right = glm::vec3(1, 0, 0);
    camera.up = glm::normalize(glm::cross(camera.right, camera.forward));
    camera.fovY = glm::radians(30.0f);
    camera.aspect = 16.0f / 9.0f;
    camera.nearPlane = 0.1f;
    camera.farPlane = 41;
    camera.splitStart = 8.5f;
    return camera;
}
} // namespace

TEST_CASE("light grid ranks the strongest reaching lights first and ignores flicker") {
    std::vector<SceneLight> lights = {
        light({5, 2, 5}, 6, 1, 1),       // dim
        light({5.5f, 2, 5}, 6, 4, 0.1f), // bright, currently flickered almost off
        light({40, 2, 40}, 3, 9, 9),     // far away: reaches no cell near (5, 5)
    };
    LightGrid grid;
    grid.build(lights, {0, 0}, {48, 48}, {5, 0, 5});
    const auto cell = grid.cellOf({5, 5});
    const auto ids = grid.cell(cell.x, cell.y);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 0);
    // The flickered intensity is what gets shaded.
    CHECK(grid.lightTexels[LightGrid::MaxLights + 1].r == doctest::Approx(0.1f));
    // Unused point and spot sentinel: point lights store -2 in the inner cone slot.
    CHECK(grid.lightTexels[LightGrid::MaxLights].w == doctest::Approx(-2.0f));
    const auto far = grid.cellOf({40, 40});
    CHECK(grid.cell(far.x, far.y) == std::vector<int>{2});
}

TEST_CASE("light grid keeps at most MaxPerCell lights per cell and MaxLights in total") {
    std::vector<SceneLight> lights;
    for (int i = 0; i < LightGrid::MaxLights + 20; ++i) {
        lights.push_back(
            light({4 + static_cast<float>(i % 3) * 0.1f, 1, 4}, 5, 1 + static_cast<float>(i), 1));
    }
    LightGrid grid;
    grid.build(lights, {0, 0}, {8, 8}, {4, 0, 4});
    CHECK(grid.lightCount == LightGrid::MaxLights);
    const auto cell = grid.cellOf({4, 4});
    const auto ids = grid.cell(cell.x, cell.y);
    CHECK(ids.size() == static_cast<std::size_t>(LightGrid::MaxPerCell));
    const auto brightness = [&](int id) {
        return grid.lightTexels[static_cast<std::size_t>(LightGrid::MaxLights + id)].r;
    };
    for (std::size_t i = 1; i < ids.size(); ++i) {
        CHECK(brightness(ids[i - 1]) >= brightness(ids[i]));
    }
}

TEST_CASE("light grid uses the same lights across a cell, with no per-object choice") {
    std::vector<SceneLight> lights = {light({2, 1, 2}, 4, 2, 2)};
    LightGrid grid;
    grid.build(lights, {0, 0}, {16, 16}, {2, 0, 2});
    // Neighbouring cells both inside the radius see the light.
    CHECK(grid.cell(0, 0) == std::vector<int>{0});
    CHECK(grid.cell(1, 0) == std::vector<int>{0});
    CHECK(grid.cell(5, 5).empty());
}

TEST_CASE("flicker curves interpolate keyframes and loop") {
    const auto settings = LightSettings::fromJson(
        Json::parse(R"({"intensity": 2, "flicker_curve": [[0, 1], [0.5, 0], [1, 1]]})"));
    CHECK(settings.flickerScale(0.0, 0) == doctest::Approx(1));
    CHECK(settings.flickerScale(0.25, 0) == doctest::Approx(0.5));
    CHECK(settings.flickerScale(0.5, 0) == doctest::Approx(0));
    CHECK(settings.flickerScale(1.25, 0) == doctest::Approx(0.5));
    CHECK(LightSettings::fromJson(settings.toJson()).flickerCurve == settings.flickerCurve);
    CHECK_THROWS(LightSettings::fromJson(Json::parse(R"({"flicker_curve": [[1, 1], [0.5, 1]]})")));
    const auto steady = LightSettings::fromJson(Json::parse(R"({"intensity": 2})"));
    CHECK(steady.flickerScale(3.7, 1) == doctest::Approx(1));
}

TEST_CASE("environment render settings round-trip and validate") {
    auto json = Environment{}.toJson();
    json["post"]["tonemap"] = "none";
    json["water"]["depth_falloff"] = 2.5;
    json["shadow_strength"] = 0.4;
    const auto env = Environment::fromJson(json);
    CHECK(env.post.tonemap == "none");
    CHECK(env.water.depthFalloff == doctest::Approx(2.5f));
    CHECK(env.shadowStrength == doctest::Approx(0.4f));
    json["post"]["tonemap"] = "filmic";
    CHECK_THROWS(Environment::fromJson(json));
    json["post"]["tonemap"] = "aces";
    json["fog_max"] = 2;
    CHECK_THROWS(Environment::fromJson(json));
}

TEST_CASE("shadow cascades cover their frustum slices") {
    const auto camera = orbitCamera({10, 0, 10});
    ShadowSettings settings;
    settings.sunDirection = {-0.6f, -1, -0.4f};
    settings.sceneMax = {20, 8, 20};
    const auto cascades = fitShadowCascades(camera, settings);
    const auto splits = cascadeSplits(camera);
    CHECK(splits.front() == doctest::Approx(camera.nearPlane));
    CHECK(splits.back() == doctest::Approx(camera.farPlane));
    for (std::size_t c = 0; c < cascades.size(); ++c) {
        CHECK(splits[c] < splits[c + 1]);
        // The point on the view axis in the middle of the slice maps inside the tile.
        const auto point = camera.position + camera.forward * ((splits[c] + splits[c + 1]) * 0.5f);
        const auto p = cascades[c].sample * glm::vec4(point, 1);
        CHECK(p.x > 0);
        CHECK(p.x < 1);
        CHECK(p.y > 0);
        CHECK(p.y < 1);
        CHECK(p.z > 0);
        CHECK(p.z < 1);
    }
    CHECK(cascades[0].radius < cascades[2].radius);
}

TEST_CASE("shadow cascades snap to whole texels so moving the camera does not shimmer") {
    ShadowSettings settings;
    settings.sunDirection = {-0.6f, -1, -0.4f};
    settings.sceneMax = {20, 8, 20};
    const auto a = fitShadowCascades(orbitCamera({10, 0, 10}), settings);
    const auto b = fitShadowCascades(orbitCamera({10.0013f, 0, 10.0021f}), settings);
    for (std::size_t c = 0; c < a.size(); ++c) {
        CHECK(a[c].radius == doctest::Approx(b[c].radius));
        // A fixed world point lands on the same texel (or exactly one texel over).
        const glm::vec4 point(10, 0, 10, 1);
        const float texels = static_cast<float>(settings.resolution);
        const auto shift = glm::vec2(a[c].sample * point - b[c].sample * point) * texels;
        CHECK(std::abs(shift.x - std::round(shift.x)) < 1e-2f);
        CHECK(std::abs(shift.y - std::round(shift.y)) < 1e-2f);
    }
}

TEST_CASE("path overlays skip same-column steps and end in an arrow head") {
    Grid grid(6, 6, 2);
    OverlayMesh mesh;
    addPathArrow(grid, {{1, 1, 0}, {2, 1, 0}, {2, 1, 1}, {3, 1, 1}}, glm::vec4(1), mesh);
    // Two strips of 4 vertices plus a 3-vertex head; the bridge step adds nothing.
    CHECK(mesh.vertices.size() == 11);
    CHECK(mesh.indices.size() == 15);
    for (const auto& v : mesh.vertices) {
        CHECK(std::isfinite(v.position.x));
        CHECK(std::isfinite(v.position.z));
    }
    // The head's tip reaches the centre of the last cell.
    CHECK(mesh.vertices.back().position.x == doctest::Approx(3.5f));
    mesh.clear();
    addPathArrow(grid, {{1, 1, 0}}, glm::vec4(1), mesh);
    CHECK(mesh.vertices.empty());
}

TEST_CASE("mip levels halve each side and average colour in linear light") {
    Image image;
    image.width = 3;
    image.height = 2;
    image.rgba = {0, 0, 0, 255, 255, 255, 255, 255, 9, 9, 9, 255,
                  0, 0, 0, 255, 255, 255, 255, 255, 9, 9, 9, 255};
    const auto srgb = image.halved(true);
    CHECK(srgb.width == 1);
    CHECK(srgb.height == 1);
    // Half black, half white is 188 in sRGB, not the 128 of a naive average.
    CHECK(srgb.rgba[0] == 188);
    CHECK(srgb.rgba[3] == 255);
    CHECK(image.halved(false).rgba[0] == 128);
    const auto last = Image::solid(1, 2, 3).halved(true);
    CHECK(last.width == 1);
    CHECK(last.rgba == Image::solid(1, 2, 3).rgba);
    // Colour is weighted by alpha: a transparent black neighbour does not
    // darken a cutout's edge, and alpha averages as coverage.
    Image edge;
    edge.width = edge.height = 2;
    edge.rgba = {200, 100, 50, 255, 0, 0, 0, 0, 200, 100, 50, 255, 0, 0, 0, 0};
    const auto half = edge.halved(false);
    CHECK(half.rgba[0] == 200);
    CHECK(half.rgba[1] == 100);
    CHECK(half.rgba[3] == 128);
    Image clear;
    clear.width = clear.height = 2;
    clear.rgba.assign(16, 0);
    clear.rgba[0] = 90;
    CHECK(clear.halved(false).rgba[0] == 23); // nothing covered: a plain average
}

TEST_CASE("glTF samplers, scene membership and spot lights are imported") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-render-tests";
    std::filesystem::create_directories(folder);
    const auto file = folder / "scene.gltf";
    std::ofstream(file) << R"({
      "asset": {"version": "2.0"},
      "scene": 0,
      "scenes": [{"nodes": [0, 2]}],
      "nodes": [
        {"name": "shown", "mesh": 0},
        {"name": "orphan", "mesh": 0},
        {"name": "lamp", "translation": [1, 3, 0], "rotation": [-0.7071068, 0, 0, 0.7071068],
         "extensions": {"KHR_lights_punctual": {"light": 0}}}
      ],
      "extensionsUsed": ["KHR_lights_punctual"],
      "extensions": {"KHR_lights_punctual": {"lights": [
        {"type": "spot", "color": [1, 1, 1], "intensity": 3, "range": 6,
         "spot": {"innerConeAngle": 0.2, "outerConeAngle": 0.5}}]}},
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
      "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
      "textures": [{"sampler": 0}],
      "samplers": [{"wrapS": 33071, "wrapT": 33648, "magFilter": 9728, "minFilter": 9984}],
      "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/"}],
      "bufferViews": [{"buffer": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                     "min": [0, 0, 0], "max": [1, 0, 1]}]
    })";
    const auto model = Model::load(file);
    REQUIRE(model.parts.size() == 2);
    CHECK_FALSE(model.parts[0].hidden);
    CHECK(model.parts[1].hidden);
    const auto& sampler = model.parts[0].material.albedoSampler;
    CHECK(sampler.wrapU == TextureSampler::Wrap::Clamp);
    CHECK(sampler.wrapV == TextureSampler::Wrap::Mirror);
    CHECK(sampler.nearestMag);
    CHECK(sampler.nearestMin);
    CHECK(sampler.mips == TextureSampler::Mips::Nearest);
    CHECK(model.parts[0].max.x == doctest::Approx(1));
    const auto lamp = std::find_if(model.markers.begin(), model.markers.end(),
                                   [](const ModelMarker& m) { return m.name == "lamp"; });
    REQUIRE(lamp != model.markers.end());
    const auto settings = LightSettings::fromJson(lamp->properties.at("light"));
    CHECK(settings.type == LightType::Spot);
    CHECK(settings.innerCone == doctest::Approx(glm::degrees(0.2f)));
    CHECK(settings.outerCone == doctest::Approx(glm::degrees(0.5f)));
    // Rotated -90 degrees about X, the node's -Z axis points down.
    CHECK(settings.direction.y == doctest::Approx(-1).epsilon(1e-4));
    std::filesystem::remove_all(folder);
}

TEST_CASE("spot cones must be 0 <= inner < outer <= 90; hard-edged glTF cones import with an edge") {
    auto spot = LightSettings{};
    spot.type = LightType::Spot;
    auto json = spot.toJson();
    CHECK_NOTHROW(static_cast<void>(LightSettings::fromJson(json)));
    for (const auto& [inner, outer] :
         std::vector<std::pair<float, float>>{{30, 30}, {40, 30}, {-5, 30}, {10, 95}}) {
        json["inner_cone"] = inner;
        json["outer_cone"] = outer;
        CHECK_THROWS(static_cast<void>(LightSettings::fromJson(json)));
    }
    json["type"] = "point";
    CHECK_NOTHROW(static_cast<void>(LightSettings::fromJson(json))); // point lights ignore their cones
    const auto folder = std::filesystem::temp_directory_path() / "fengine-render-spot";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "lamp.gltf") << R"({
      "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "lamp", "extensions": {"KHR_lights_punctual": {"light": 0}}}],
      "extensionsUsed": ["KHR_lights_punctual"],
      "extensions": {"KHR_lights_punctual": {"lights": [
        {"type": "spot", "intensity": 3, "spot": {"innerConeAngle": 0.6, "outerConeAngle": 0.6}}]}}
    })";
    const auto model = Model::load(folder / "lamp.gltf");
    REQUIRE(model.markers.size() == 1);
    const auto lamp = LightSettings::fromJson(model.markers[0].properties.at("light"));
    CHECK(lamp.outerCone == doctest::Approx(glm::degrees(0.6f)));
    CHECK(lamp.innerCone == doctest::Approx(glm::degrees(0.6f) - 0.5f));
    std::filesystem::remove_all(folder);
}

TEST_CASE("blend modes parse by name, accept the boolean alias and round-trip on emitters") {
    CHECK(blendModeFromName("additive") == BlendMode::Additive);
    CHECK(blendModeFromJson(Json(true)) == BlendMode::Alpha);
    CHECK(blendModeFromJson(Json(false)) == BlendMode::Opaque);
    CHECK_THROWS((void)blendModeFromName("screen"));
    for (const auto mode : {BlendMode::Opaque, BlendMode::Alpha, BlendMode::Additive, BlendMode::Multiply,
                            BlendMode::Premultiplied}) {
        CHECK(blendModeFromName(blendModeName(mode)) == mode);
    }
    const auto emitter = EmitterSettings::fromJson({{"blend", "multiply"}});
    CHECK(emitter.blend == BlendMode::Multiply);
    CHECK(EmitterSettings::fromJson(emitter.toJson()).blend == BlendMode::Multiply);
    CHECK(EmitterSettings::fromJson(Json::object()).blend == BlendMode::Alpha);
}

TEST_CASE("water surfaces draw first in the alpha pass, before every other blended draw") {
    std::vector<TransparentKey> draws = {{BlendMode::Alpha, 2},
                                         {BlendMode::Alpha, 5, true},
                                         {BlendMode::Additive, 9},
                                         {BlendMode::Alpha, 30},
                                         {BlendMode::Alpha, 12, true}};
    std::stable_sort(draws.begin(), draws.end(), drawsBefore);
    // Both water surfaces (back to front among themselves), then the sprites
    // behind and in front of them, then the additive pass.
    CHECK(draws[0].water);
    CHECK(draws[0].distance == doctest::Approx(12));
    CHECK(draws[1].water);
    CHECK(draws[1].distance == doctest::Approx(5));
    CHECK(draws[2].distance == doctest::Approx(30));
    CHECK(draws[3].distance == doctest::Approx(2));
    CHECK(draws[4].blend == BlendMode::Additive);
    CHECK_FALSE(drawsBefore({BlendMode::Alpha, 1, true}, {BlendMode::Alpha, 1, true}));
}

TEST_CASE("blended draws sort by pass, then back to front") {
    std::vector<TransparentKey> draws = {{BlendMode::Additive, 9}, {BlendMode::Alpha, 2},
                                         {BlendMode::Multiply, 1}, {BlendMode::Premultiplied, 7},
                                         {BlendMode::Alpha, 12},   {BlendMode::Additive, 3}};
    std::stable_sort(draws.begin(), draws.end(), drawsBefore);
    const std::vector<std::pair<BlendMode, float>> expected = {
        {BlendMode::Alpha, 12},   {BlendMode::Premultiplied, 7}, {BlendMode::Alpha, 2},
        {BlendMode::Multiply, 1}, {BlendMode::Additive, 9},      {BlendMode::Additive, 3}};
    REQUIRE(draws.size() == expected.size());
    for (std::size_t i = 0; i < draws.size(); ++i) {
        CHECK(draws[i].blend == expected[i].first);
        CHECK(draws[i].distance == doctest::Approx(expected[i].second));
    }
    CHECK_FALSE(blended(BlendMode::Opaque));
    CHECK(blended(BlendMode::Premultiplied));
}

TEST_CASE("light shadows go to the most important lights that fit the tile budget") {
    LightSettings light;
    light.castShadows = true;
    light.color = glm::vec3(1);
    light.radius = 4;
    light.position = {0, 2, 0};
    const float near = lightShadowPriority(light, {0, 0, 0});
    light.position = {20, 2, 0};
    const float far = lightShadowPriority(light, {0, 0, 0});
    CHECK(near > far);
    CHECK(far > 0);
    light.castShadows = false;
    CHECK(lightShadowPriority(light, {0, 0, 0}) == 0);

    // Two points (6 tiles each) and three spots compete for 16 tiles.
    const std::vector<LightShadowCandidate> candidates = {{5, true}, {1, false}, {9, true}, {0, false},
                                                          {3, true}, {2, false}, {4, false}};
    const auto slots = chooseShadowLights(candidates);
    REQUIRE(slots.size() == 5);
    CHECK(slots[0].light == 2); // strongest point light first
    CHECK(slots[0].firstTile == 0);
    CHECK(slots[0].tiles == 6);
    CHECK(slots[1].light == 0);
    CHECK(slots[1].firstTile == 6);
    CHECK(slots[2].light == 6); // the third point light no longer fits; spots still do
    CHECK(slots[2].tiles == 1);
    CHECK(slots[2].firstTile == 12);
    CHECK(slots[3].light == 5);
    CHECK(slots[3].firstTile == 13);
    CHECK(slots[4].light == 1);
    CHECK(slots[4].firstTile == 14);                      // the zero-priority spot gets nothing
    CHECK(chooseShadowLights(candidates, 5).size() == 3); // only spots fit five tiles
}

TEST_CASE("light shadow views put each light's view in its own atlas tile") {
    LightSettings spot;
    spot.type = LightType::Spot;
    spot.position = {3, 4, 3};
    spot.direction = {0, -1, 0};
    spot.radius = 8;
    spot.outerCone = 30;
    const auto views = lightShadowViews(spot, 5, false, false);
    REQUIRE(views.size() == 1);
    auto sample = [](const LightShadowView& view, glm::vec3 p) {
        const auto clip = view.sample * glm::vec4(p, 1);
        return glm::vec3(clip) / clip.w;
    };
    const auto below = sample(views[0], {3, 0, 3});
    CHECK(below.x == doctest::Approx(5.5f / LightShadowTiles));
    CHECK(below.y == doctest::Approx(0.5f));
    CHECK(below.z > 0);
    CHECK(below.z < 1);
    // Further along the beam is deeper.
    CHECK(sample(views[0], {3, -2, 3}).z > below.z);

    LightSettings point;
    point.position = {0, 1, 0};
    point.radius = 6;
    const auto faces = lightShadowViews(point, 2, false, true);
    REQUIRE(faces.size() == 6);
    const glm::vec3 directions[] = {{1, 0.2f, -0.3f}, {-1, 0.1f, 0.4f}, {0.3f, 1, 0},
                                    {0, -1, 0.2f},    {0.1f, 0.3f, 1},  {-0.4f, 0, -1}};
    for (int face = 0; face < 6; ++face) {
        const auto d = directions[face];
        CHECK(cubeFace(d) == face);
        const auto p = sample(faces[static_cast<std::size_t>(face)], point.position + d * 2.0f);
        const float tile = static_cast<float>(2 + face);
        CHECK(p.x > tile / LightShadowTiles);
        CHECK(p.x < (tile + 1) / LightShadowTiles);
        CHECK(p.y > 0);
        CHECK(p.y < 1);
    }
    CHECK(lightShadowTexelScale(point) > 2.0f / LightShadowTileResolution);
}

TEST_CASE("the light grid records each slot's source light and shadow tiles") {
    std::vector<SceneLight> lights = {light({5, 2, 5}, 6, 1, 1), light({8, 2, 5}, 6, 2, 2)};
    LightGrid grid;
    grid.build(lights, {0, 0}, {16, 16}, {5, 0, 5});
    REQUIRE(grid.sources.size() == 2);
    CHECK(grid.sources[1] == 1);
    CHECK(grid.lightTexels[LightGrid::MaxLights * 3].x == doctest::Approx(-1.0f));
    grid.setShadow(1, 4, 6, 0.01f);
    CHECK(grid.lightTexels[LightGrid::MaxLights * 3 + 1] == glm::vec4(4, 6, 0.01f, 0));
}

TEST_CASE("decals are cut at cell borders and follow heights and ramps") {
    Grid grid(4, 2, 2);
    grid.at(1, 0).height = 0.5f;
    grid.at(2, 0).ramp = Ramp::East; // rises from 0 to 1 across the cell
    grid.at(2, 1, 1).walkable = false;
    std::vector<DecalQuad> quads;
    // 2.5 cells wide, one cell deep, starting half-way into cell 0.
    projectDecal(grid, 0, {0.5f, 0}, {3, 1}, {0, 0}, {1, 1}, 0.02f, quads);
    REQUIRE(quads.size() == 3);
    // Corners: bottom-left, bottom-right, top-right, top-left; north (z = 0) is the top.
    CHECK(quads[0].positions[0] == glm::vec3(0.5f, 0.02f, 1));
    CHECK(quads[0].positions[3] == glm::vec3(0.5f, 0.02f, 0));
    CHECK(quads[0].uv[0] == glm::vec2(0, 1));
    CHECK(quads[0].uv[2].x == doctest::Approx(0.2f));
    CHECK(quads[1].positions[0].y == doctest::Approx(0.52f));
    CHECK(quads[1].normal == glm::vec3(0, 1, 0));
    CHECK(quads[2].positions[0].y == doctest::Approx(0.02f));
    CHECK(quads[2].positions[1].y == doctest::Approx(1.02f));
    CHECK(quads[2].uv[1] == glm::vec2(1, 1));
    CHECK(quads[2].normal.x < -0.5f); // the ramp leans west, facing up the slope
    CHECK(quads[2].normal.y > 0.5f);

    // Off the grid is dropped; an upper layer without a surface falls back to the ground.
    quads.clear();
    projectDecal(grid, 1, {2, 1}, {5, 2}, {0, 0}, {1, 1}, 0, quads);
    REQUIRE(quads.size() == 2);
    CHECK(quads[0].positions[0].y == doctest::Approx(0.0f));
}

TEST_CASE("pixel snapping moves a point onto a pixel centre by less than a pixel") {
    const glm::vec3 eye(0, 6, 10), right(1, 0, 0);
    const auto view = glm::lookAtRH(eye, glm::vec3(0), glm::vec3(0, 1, 0));
    const auto viewProjection = glm::perspectiveRH_ZO(glm::radians(30.0f), 16.0f / 9.0f, 0.1f, 100.0f) * view;
    const glm::vec3 up = glm::normalize(glm::cross(right, glm::normalize(-eye)));
    const glm::ivec2 target(640, 360);
    auto pixel = [&](glm::vec3 p) {
        const auto clip = viewProjection * glm::vec4(p, 1);
        return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * glm::vec2(target);
    };
    for (const auto p :
         {glm::vec3(0.013f, 0.2f, -0.31f), glm::vec3(1.77f, 0, 2.05f), glm::vec3(-3.3f, 1, 0.5f)}) {
        const auto snapped = snapToPixel(p, viewProjection, right, up, target);
        const auto at = pixel(snapped);
        CHECK(at.x - std::floor(at.x) == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(at.y - std::floor(at.y) == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(glm::length(at - pixel(p)) <= 0.75f);
        CHECK(snapToPixel(snapped, viewProjection, right, up, target).x == doctest::Approx(snapped.x));
    }
}

TEST_CASE("generated tangents follow +u and record mirrored UVs") {
    std::vector<MeshVertex> quad(4);
    const glm::vec3 positions[] = {{0, 0, 0}, {1, 0, 0}, {1, 0, -1}, {0, 0, -1}};
    for (std::size_t i = 0; i < 4; ++i) {
        quad[i].position = positions[i];
        quad[i].normal = {0, 1, 0};
        quad[i].uv = {positions[i].x, 1 + positions[i].z};
    }
    const std::vector<std::uint32_t> indices = {0, 1, 2, 0, 2, 3};
    generateTangents(quad, indices);
    // cross(normal, tangent) is -z; +v runs towards +z, so the UVs are mirrored.
    for (const auto& v : quad) {
        CHECK(v.tangent.x == doctest::Approx(1.0f));
        CHECK(std::abs(v.tangent.y) + std::abs(v.tangent.z) == doctest::Approx(0.0f));
        CHECK(v.tangent.w == doctest::Approx(-1.0f));
    }
    for (auto& v : quad) {
        v.uv.y = -v.position.z;
    }
    generateTangents(quad, indices);
    CHECK(quad[0].tangent.w == doctest::Approx(1.0f));
    std::vector<MeshVertex> noUv(3);
    generateTangents(noUv, {0, 1, 2});
    CHECK(glm::dot(glm::vec3(noUv[0].tangent), noUv[0].normal) == doctest::Approx(0.0f));
    CHECK(glm::length(glm::vec3(noUv[0].tangent)) == doctest::Approx(1.0f));
}

TEST_CASE("glTF normal and emissive maps, emissive strength and generated tangents are imported") {
    const auto file =
        std::filesystem::path(FENGINE_BUILTIN) / ".." / "samples" / "starfall" / "fx" / "crate.gltf";
    const auto model = Model::load(file);
    REQUIRE(model.parts.size() == 1);
    const auto& material = model.parts[0].material;
    REQUIRE(material.albedo);
    REQUIRE(material.normal);
    REQUIRE(material.emission);
    CHECK(material.emission.file.filename() == "crate_glow.png");
    CHECK(material.emissive == glm::vec3(3));
    CHECK(material.normalScale == doctest::Approx(1.0f));
    for (const auto& v : model.parts[0].vertices) {
        CHECK(glm::dot(glm::vec3(v.tangent), v.normal) == doctest::Approx(0.0f));
    }
}

TEST_CASE("cooked KTX headers are recognised and found next to their image") {
    std::vector<std::uint8_t> bytes = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
    auto word = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    for (const std::uint32_t value :
         {0x04030201u, 0u, 1u, 0u, 0x8E8Du, 0x1908u, 64u, 32u, 0u, 0u, 1u, 7u, 0u}) {
        word(value);
    }
    const auto info = readKtxHeader(bytes);
    REQUIRE(info);
    CHECK(info->format == KtxInfo::Format::Bc7);
    CHECK(info->srgb);
    CHECK(info->width == 64);
    CHECK(info->height == 32);
    CHECK(info->mips == 7);
    auto cube = bytes;
    cube[12 + 10 * 4] = 6; // six faces: not a 2D texture
    CHECK_FALSE(readKtxHeader(cube));
    CHECK_FALSE(readKtxHeader(std::span(bytes).first(40)));

    const auto folder = std::filesystem::temp_directory_path() / "fengine-ktx-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "stone.png") << "png";
    CHECK(cookedTexture(folder / "stone.png").empty());
    std::ofstream(folder / "stone.ktx", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(cookedTexture(folder / "stone.png") == folder / "stone.ktx");
    CHECK(cookedTexture({}).empty());
}

TEST_CASE("pixel snapping and water reflection settings round-trip and validate") {
    auto json = Environment{}.toJson();
    CHECK(json["pixel_snap"] == false);
    json["pixel_snap"] = true;
    json["water"]["reflection"] = 0.25;
    json["water"]["refraction"] = 0.1;
    const auto env = Environment::fromJson(json);
    CHECK(env.pixelSnap);
    CHECK(env.water.reflection == doctest::Approx(0.25f));
    CHECK(env.water.refraction == doctest::Approx(0.1f));
    json["water"]["reflection"] = 2;
    CHECK_THROWS(Environment::fromJson(json));
}

TEST_CASE("map chunks keep only their own vertices, each once, in first-use order") {
    // Two triangles in chunk (0, 0) sharing an edge, one in chunk (1, 0) reusing a far vertex.
    const std::vector<glm::vec3> positions = {{0, 0, 0},  {1, 0, 0},  {0, 0, 1},  {1, 0, 1},
                                              {12, 0, 0}, {13, 0, 0}, {12, 0, 1}, {50, 0, 50}};
    const std::vector<std::uint32_t> indices = {4, 5, 6, 0, 1, 2, 1, 3, 2, 5, 4, 1};
    const auto chunks = splitChunks(positions, indices, 8);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0].cell == glm::ivec2(0, 0));
    CHECK(chunks[0].vertices == std::vector<std::uint32_t>{0, 1, 2, 3});
    CHECK(chunks[0].indices == std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2});
    CHECK(chunks[1].cell == glm::ivec2(1, 0));
    CHECK(chunks[1].vertices == std::vector<std::uint32_t>{4, 5, 6, 1});
    CHECK(chunks[1].indices == std::vector<std::uint32_t>{0, 1, 2, 1, 0, 3});
    std::size_t total = 0;
    for (const auto& chunk : chunks) {
        total += chunk.vertices.size();
        for (const auto index : chunk.indices) {
            CHECK(index < chunk.vertices.size());
        }
    }
    CHECK(total == 8); // vertex 1 is in both chunks; unused vertex 7 in none
}

TEST_CASE("instanced draws group equal parts in first-use order") {
    const std::vector<std::pair<int, int>> keys = {{3, 0}, {1, 0}, {3, 0}, {3, 1}, {1, 0}, {2, 0}};
    std::vector<int> order, rank;
    groupByFirstUse(keys, order, rank);
    CHECK(order == std::vector<int>{0, 2, 1, 4, 3, 5});
    // Warm buffers: a later call of the same size allocates nothing.
    const auto* orderData = order.data();
    const auto* rankData = rank.data();
    groupByFirstUse(keys, order, rank);
    CHECK(order.data() == orderData);
    CHECK(rank.data() == rankData);
    groupByFirstUse(std::vector<int>{}, order, rank);
    CHECK(order.empty());
}

TEST_CASE("instanced normals match the inverse transpose, scaled and mirrored") {
    for (const auto& scale : {glm::vec3(1), glm::vec3(2, 0.5f, 3), glm::vec3(-1, 1, 1)}) {
        const auto model = glm::translate(glm::mat4(1), glm::vec3(3, 1, -2)) *
                           glm::rotate(glm::mat4(1), 0.7f, glm::normalize(glm::vec3(1, 2, 0.5f))) *
                           glm::scale(glm::mat4(1), scale);
        const auto inverseTranspose = glm::transpose(glm::inverse(glm::mat3(model)));
        for (const auto& normal : {glm::vec3(0, 1, 0), glm::normalize(glm::vec3(1, -2, 0.3f))}) {
            const auto expected = glm::normalize(inverseTranspose * normal);
            const auto got = glm::normalize(instanceNormal(model, normal));
            CHECK(glm::dot(expected, got) == doctest::Approx(1).epsilon(1e-5));
        }
    }
}

TEST_CASE("only depth-writing sprites and parts cast shadows") {
    CHECK(castsShadow(true, spriteBlend(BlendMode::Opaque, 1)));
    CHECK_FALSE(castsShadow(false, spriteBlend(BlendMode::Opaque, 1)));
    CHECK(spriteBlend(BlendMode::Opaque, 0.5f) == BlendMode::Alpha);
    CHECK_FALSE(castsShadow(true, spriteBlend(BlendMode::Opaque, 0.5f))); // a fading sprite
    for (const auto mode :
         {BlendMode::Alpha, BlendMode::Premultiplied, BlendMode::Additive, BlendMode::Multiply}) {
        CHECK_FALSE(castsShadow(true, spriteBlend(mode, 1)));
    }
}

TEST_CASE("glTF materials and nodes name game shaders; invalid names keep the engine's") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-render-material-shader";
    std::filesystem::create_directories(folder);
    const std::string buffers =
        R"("buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/"}],
      "bufferViews": [{"buffer": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 0, 1]}])";
    std::ofstream(folder / "shaded.gltf")
        << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0, 1, 2]}],
      "nodes": [{"mesh": 0}, {"mesh": 1}, {"mesh": 2, "extras": {"shader": "rune_glow"}}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]},
                 {"primitives": [{"attributes": {"POSITION": 0}, "material": 1}]},
                 {"primitives": [{"attributes": {"POSITION": 0}, "material": 2}]}],
      "materials": [{"extras": {"fengine": {"shader": "glint"}}}, {"extras": {"shader": "Bad Name"}}, {}],)"
        << buffers << "}";
    const auto model = Model::load(folder / "shaded.gltf");
    REQUIRE(model.parts.size() == 3);
    CHECK(model.parts[0].material.shader == "glint");
    CHECK(model.parts[1].material.shader.empty());
    CHECK(model.parts[2].material.shader == "rune_glow");
    CHECK(model.parts[2].material.alphaCutoff == 0); // OPAQUE materials are not alpha-cut
    CHECK_FALSE(model.parts[2].material.transparent);
    CHECK(isShaderName("sepia-2_b"));
    CHECK_FALSE(isShaderName(""));
    CHECK_FALSE(isShaderName("../x"));
    CHECK_FALSE(isShaderName(std::string(65, 'a')));
    std::filesystem::remove_all(folder);
}

TEST_CASE("mirrored glTF matrix nodes import mirrored, and image URIs stay in the model's folder") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-render-mirror";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "mirror.gltf")
        << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
      "nodes": [{"matrix": [-2, 0, 0, 0,  0, 3, 0, 0,  0, 0, 1, 0,  4, 5, 6, 1]}]})";
    const auto model = Model::load(folder / "mirror.gltf");
    REQUIRE(model.nodes.size() == 1);
    const auto& node = model.nodes[0];
    CHECK(node.translation == glm::vec3(4, 5, 6));
    CHECK(node.scale.x == doctest::Approx(-2)); // the mirroring survives as a negative scale
    CHECK(node.scale.y == doctest::Approx(3));
    CHECK(std::abs(node.rotation.w) == doctest::Approx(1).epsilon(1e-5)); // a proper rotation (none)
    REQUIRE(model.restPose.size() == 1);
    CHECK(model.restPose[0][0].x == doctest::Approx(-2));
    CHECK(model.restPose[0][3].x == doctest::Approx(4));

    std::ofstream(folder / "escape.gltf")
        << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
      "nodes": [{"mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
      "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
      "textures": [{"source": 0}], "images": [{"uri": "../outside.png"}],
      "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/"}],
      "bufferViews": [{"buffer": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 0, 1]}]})";
    CHECK_THROWS_WITH(static_cast<void>(Model::load(folder / "escape.gltf")),
                      doctest::Contains("outside the model's folder"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("the renderer's clip-resolved sprite placement matches placeSprite") {
    const auto animation = SpriteAnimation::fromJson(Json::parse(R"({
      "atlas": "a.png", "pixels_per_unit": 16, "pivot": [0.5, 1],
      "clips": {"idle": {"directions": [[{"rect": [0, 0, 16, 32]}]]},
                "walk": {"directions": [[{"rect": [0, 0, 16, 32], "duration": 0.1}, {"rect": [16, 0, 16, 32], "duration": 0.1}],
                                        [{"rect": [0, 32, 16, 32]}], [{"rect": [0, 64, 16, 32]}], [{"rect": [0, 96, 16, 32]}]]}}})"));
    Camera camera;
    camera.yaw = 37;
    camera.snap();
    const glm::vec3 feet(3.2f, 0.5f, 7.1f);
    for (const auto mode :
         {SpriteMode::Upright, SpriteMode::Facing, SpriteMode::Flat, SpriteMode::Standing}) {
        for (const auto* clip : {"walk", "idle", "missing"}) {
            for (const auto facing : {glm::vec2(0, 1), glm::vec2(-1, 0), glm::vec2(0.7f, -0.7f)}) {
                Sprite s;
                s.clip = clip;
                s.mode = mode;
                s.time = 0.15;
                s.previousTime = 0.1;
                s.scale = 1.5f;
                s.pixelOffset = {2, -3};
                s.depthBias = 0.2f;
                const auto expected = placeSprite(s, animation, feet, facing, camera, 0.5, 32);
                const auto got =
                    placeSpriteClip(s, animation, animation.clip(clip), feet, facing, camera, 0.5, 32);
                CHECK(got.frame == expected.frame);
                CHECK(got.anchor == expected.anchor);
                CHECK(got.right == expected.right);
                CHECK(got.up == expected.up);
                CHECK(got.size == expected.size);
                CHECK(got.lying == expected.lying);
            }
        }
    }
    {
        // A standing sprite stays vertical but is stretched by 1 / cos(pitch), so
        // a texel projects as tall as it is wide under an elevated camera.
        Camera pitched;
        pitched.pitch = 35;
        pitched.snap();
        Sprite s;
        s.mode = SpriteMode::Standing;
        const auto placed = placeSprite(s, animation, feet, {0, 1}, pitched, 1.0, 32);
        CHECK(placed.up.x == 0);
        CHECK(placed.up.z == 0);
        CHECK(placed.up.y == doctest::Approx(1.0 / std::cos(glm::radians(35.0))).epsilon(0.01));
        CHECK(placed.anchor.y == doctest::Approx(feet.y));
    }
    CHECK(clipDirection(4, {0, 1}, 0) == animation.direction("walk", {0, 1}, 0));
    CHECK(clipDirection(4, {1, 0}, 90) == animation.direction("walk", {1, 0}, 90));
    CHECK(clipDirection(4, {-1, 0.2f}, 200) == animation.direction("walk", {-1, 0.2f}, 200));
    CHECK(clipDirection(1, {-1, 0.2f}, 200) == 0);
}

TEST_CASE("model poses copy the rest pose and animate without allocating") {
    Model model;
    model.nodes.resize(2);
    model.nodes[0].translation = {1, 0, 0};
    model.nodes[1].parent = 0;
    model.nodes[1].translation = {0, 2, 0};
    model.nodes[1].scale = {2, 2, 2};
    model.order = {0, 1};
    ModelAnimation spin;
    spin.name = "rise";
    AnimationChannel channel;
    channel.node = 1;
    channel.target = AnimationChannel::Target::Translation;
    channel.times = {0, 1};
    channel.values = {{0, 2, 0, 0}, {0, 4, 0, 0}};
    spin.channels.push_back(channel);
    spin.duration = 1;
    model.animations.push_back(spin);
    model.bakeRestPose();
    REQUIRE(model.restPose.size() == 2);
    CHECK(glm::vec3(model.restPose[1][3]) == glm::vec3(1, 2, 0));
    CHECK(model.restPose[1][0].x == doctest::Approx(2));
    std::vector<glm::mat4> pose;
    model.pose(-1, 0, pose);
    CHECK(pose == model.restPose);
    model.pose(0, 0.5f, pose);
    const auto* data = pose.data();
    CHECK(glm::vec3(pose[1][3]) == glm::vec3(1, 3, 0)); // half way up, under the parent's offset
    CHECK(glm::vec3(pose[0][3]) == glm::vec3(1, 0, 0));
    model.pose(0, 1.0f, pose);
    CHECK(pose.data() == data); // warm: nothing reallocated
    CHECK(glm::vec3(pose[1][3]) == glm::vec3(1, 4, 0));
    CHECK(model.animation("rise") == 0);
    CHECK(model.animation("none") == -1);
}

TEST_CASE("shadow selection and views into caller buffers match the returning forms") {
    const std::vector<LightShadowCandidate> candidates = {{5, true}, {1, false}, {9, true}, {4, false}};
    std::vector<LightShadowSlot> slots;
    std::vector<std::size_t> order;
    chooseShadowLights(candidates, slots, order);
    const auto expected = chooseShadowLights(candidates);
    REQUIRE(slots.size() == expected.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        CHECK(slots[i].light == expected[i].light);
        CHECK(slots[i].firstTile == expected[i].firstTile);
    }
    LightSettings point;
    point.position = {0, 1, 0};
    point.radius = 6;
    std::vector<LightShadowView> views;
    lightShadowViews(point, 2, false, true, views);
    const auto returned = lightShadowViews(point, 2, false, true);
    REQUIRE(views.size() == returned.size());
    CHECK(views[3].sample == returned[3].sample);
}

TEST_CASE("shadow cascades fit an orthographic view's box slices") {
    auto camera = orbitCamera({10, 0, 10});
    camera.orthographic = true;
    camera.orthoHeight = 9;
    ShadowSettings settings;
    settings.sunDirection = {-0.6f, -1, -0.4f};
    settings.sceneMax = {20, 8, 20};
    const auto cascades = fitShadowCascades(camera, settings);
    const auto splits = cascadeSplits(camera);
    for (std::size_t c = 0; c < cascades.size(); ++c) {
        // The slice's corners (the same box at every depth) map inside the tile.
        for (const float d : {splits[c], splits[c + 1]}) {
            for (const float sx : {-1.0f, 1.0f}) {
                for (const float sy : {-1.0f, 1.0f}) {
                    const auto corner = camera.position + camera.forward * d +
                                        camera.right * (sx * 4.5f * camera.aspect) + camera.up * (sy * 4.5f);
                    const auto p = cascades[c].sample * glm::vec4(corner, 1);
                    CHECK(p.x >= -1e-3f);
                    CHECK(p.x <= 1 + 1e-3f);
                    CHECK(p.y >= -1e-3f);
                    CHECK(p.y <= 1 + 1e-3f);
                }
            }
        }
    }
    // The near cascade of an orthographic view is no smaller than its box's half diagonal.
    const float halfDiagonal = std::hypot(4.5f * camera.aspect, 4.5f);
    CHECK(cascades[0].radius >= halfDiagonal);
    // Perspective fitting is untouched by the orthographic fields.
    auto perspective = orbitCamera({10, 0, 10});
    perspective.orthoHeight = 99;
    CHECK(fitShadowCascades(perspective, settings)[1].radius ==
          fitShadowCascades(orbitCamera({10, 0, 10}), settings)[1].radius);
}

TEST_CASE("the scene target divides exactly for a whole pixel scale and rounds for a fraction") {
    CHECK(sceneSizeFor({1280, 720}, 1) == glm::ivec2(1280, 720));
    CHECK(sceneSizeFor({1280, 720}, 2) == glm::ivec2(640, 360));
    CHECK(sceneSizeFor({1281, 721}, 2) == glm::ivec2(640, 360)); // truncates like integer division
    CHECK(sceneSizeFor({1280, 720}, 1.5f) == glm::ivec2(853, 480));
    CHECK(sceneSizeFor({1280, 720}, 2.5f) == glm::ivec2(512, 288));
    CHECK(sceneSizeFor({3, 3}, 8) == glm::ivec2(1, 1));
    CHECK(sceneSizeFor({1280, 720}, 0.5f) == glm::ivec2(1280, 720)); // never above 1:1
}

TEST_CASE("the screen pass leaves an sRGB back buffer alone and scales linear light for HDR ones") {
    OutputColor sdr;
    CHECK(outputColor({0.5f, 0.25f, 1.0f}, sdr) == glm::vec3(0.5f, 0.25f, 1.0f));
    CHECK(packOutput(sdr) == glm::vec4(0, 1, 0, 0));
    CHECK(srgbToLinear(1.0f) == doctest::Approx(1.0f));
    CHECK(srgbToLinear(0.5f) == doctest::Approx(0.2140f).epsilon(0.001));
    CHECK(linearToSrgb(srgbToLinear(0.3f)) == doctest::Approx(0.3f));
    // scRGB: white lands at the paper white over the 80-nit reference.
    OutputColor scrgb{OutputEncoding::ScRgb, 200, 80};
    CHECK(outputColor({1, 1, 1}, scrgb).x == doctest::Approx(2.5f));
    CHECK(outputColor({0.5f, 0.5f, 0.5f}, scrgb).y == doctest::Approx(0.2140f * 2.5f).epsilon(0.001));
    CHECK(packOutput(scrgb) == glm::vec4(1, 2.5f, 0, 0));
    // macOS: the layer's 1.0 is already the display's SDR white.
    OutputColor edr{OutputEncoding::ScRgb, 200, 200};
    CHECK(outputColor({1, 1, 1}, edr).z == doctest::Approx(1.0f));
    // HDR10: PQ of absolute nits, after Rec. 709 -> Rec. 2020.
    CHECK(pqEncode(0) == doctest::Approx(0));
    CHECK(pqEncode(100) == doctest::Approx(0.5081f).epsilon(0.001));
    CHECK(pqEncode(10000) == doctest::Approx(1.0f));
    OutputColor hdr10{OutputEncoding::Hdr10, 200, 80};
    const auto white = outputColor({1, 1, 1}, hdr10);
    CHECK(white.x == doctest::Approx(pqEncode(200)).epsilon(0.001));
    CHECK(white.y == doctest::Approx(pqEncode(200)).epsilon(0.001));
    CHECK(packOutput(hdr10) == glm::vec4(2, 200, 0, 0));
    const auto red = rec709ToRec2020({1, 0, 0});
    CHECK(red.x == doctest::Approx(0.6274f));
    CHECK(red.x + red.y + red.z == doctest::Approx(0.7129f).epsilon(0.001));
    // Half floats, for screenshots of a 16F back buffer.
    CHECK(halfToFloat(0x3c00) == doctest::Approx(1.0f));
    CHECK(halfToFloat(0x4100) == doctest::Approx(2.5f));
    CHECK(halfToFloat(0xb800) == doctest::Approx(-0.5f));
    CHECK(halfToFloat(0x0001) == doctest::Approx(5.9604645e-8f));
    CHECK(halfToFloat(0) == 0.0f);
}

TEST_CASE("a wrapping map is drawn under the mirror offsets the view reaches, and once otherwise") {
    // A 12 x 10 map; a view is the part of the x/z plane it reaches.
    const glm::vec3 mapMin(0), mapMax(12, 16, 10);
    const auto reaching = [](glm::vec2 viewMin, glm::vec2 viewMax) {
        return [=](glm::vec3 lo, glm::vec3 hi) {
            return lo.x <= viewMax.x && hi.x >= viewMin.x && lo.z <= viewMax.y && hi.z >= viewMin.y;
        };
    };
    // Over the north-west corner, 8 units out: the west, north and corner
    // mirrors matter, the east and south ones do not.
    const auto corner = reaching({-7.5f, -7.5f}, {8.5f, 8.5f});
    std::vector<glm::vec3> offsets;
    appendWrapOffsets(mapMin, mapMax, 12, 10, corner, offsets);
    REQUIRE(offsets.size() == 4);
    CHECK(offsets[0] == glm::vec3(0));
    const auto has = [&](glm::vec3 offset) {
        return std::find(offsets.begin(), offsets.end(), offset) != offsets.end();
    };
    CHECK(has({-12, 0, 0}));
    CHECK(has({0, 0, -10}));
    CHECK(has({-12, 0, -10}));
    CHECK_FALSE(has({12, 0, 0}));
    CHECK_FALSE(has({0, 0, 10}));
    // A map that does not wrap has the zero offset only, whatever the view.
    offsets.clear();
    appendWrapOffsets(mapMin, mapMax, 0, 0, corner, offsets);
    CHECK(offsets == std::vector<glm::vec3>{glm::vec3(0)});
    // From the middle no mirror is in view.
    const auto middle = reaching({1.4f, 0.4f}, {10.6f, 9.6f});
    offsets.clear();
    appendWrapOffsets(mapMin, mapMax, 12, 10, middle, offsets);
    CHECK(offsets == std::vector<glm::vec3>{glm::vec3(0)});
    // Appending keeps what a first view found and adds the rest once.
    appendWrapOffsets(mapMin, mapMax, 12, 10, corner, offsets);
    CHECK(offsets.size() == 4);
    appendWrapOffsets(mapMin, mapMax, 12, 10, corner, offsets);
    CHECK(offsets.size() == 4);
    // The far side counts too: a view past the east edge takes the east mirror.
    offsets.clear();
    appendWrapOffsets(mapMin, mapMax, 12, 10, reaching({8, 2}, {16, 6}), offsets);
    CHECK(offsets == std::vector<glm::vec3>{glm::vec3(0), glm::vec3(12, 0, 0)});
}

TEST_CASE("map geometry is baked at rest and merged by material") {
    Model model;
    model.nodes.resize(4);
    model.nodes[1].translation = {10, 0, 0};
    model.nodes[2].scale = {-1, 1, 1}; // mirrored
    model.nodes[3].parent = 1;
    model.nodes[3].translation = {0, 5, 0};
    model.order = {0, 1, 2, 3};
    model.bakeRestPose();
    const auto image = std::make_shared<Image>(Image::solid(1, 2, 3));
    const auto triangle = [&](int node, Material material) {
        ModelPart part;
        part.vertices.resize(3);
        part.vertices[0].position = {0, 0, 0};
        part.vertices[1].position = {1, 0, 0};
        part.vertices[2].position = {0, 0, 1};
        part.indices = {0, 1, 2};
        part.node = node;
        part.material = std::move(material);
        model.parts.push_back(std::move(part));
    };
    Material stone, moss, glass, water;
    stone.albedo.image = image;
    moss.albedo.image = image;
    moss.color = {0, 1, 0, 1};
    glass.transparent = true;
    water.kind = MaterialKind::Water;
    triangle(0, stone);
    triangle(1, moss);
    triangle(2, stone); // mirrored
    triangle(3, stone); // a child node
    triangle(0, glass);
    triangle(1, glass);
    triangle(0, water);
    triangle(1, water);
    triangle(0, stone);
    model.parts.back().hidden = true;

    // A sampler only tells materials apart where there is an image to sample.
    Material plain, plainNearest;
    plainNearest.albedoSampler.nearestMag = true;
    CHECK(sameMaterial(plain, plainNearest));
    Material stoneNearest = stone;
    stoneNearest.albedoSampler.nearestMag = true;
    CHECK_FALSE(sameMaterial(stone, stoneNearest));
    CHECK_FALSE(sameMaterial(stone, moss));
    Material shaded = stone;
    shaded.shader = "glow";
    CHECK_FALSE(sameMaterial(stone, shaded));

    const auto meshes = bakeGeometry(model);
    // Stone (three parts in one), moss, then glass and water each on their own.
    REQUIRE(meshes.size() == 6);
    const auto& merged = meshes[0];
    CHECK(sameMaterial(*merged.material, stone));
    REQUIRE(merged.vertices.size() == 9);
    CHECK(merged.indices == std::vector<std::uint32_t>{0, 1, 2, 3, 5, 4, 6, 7, 8});
    CHECK(merged.vertices[1].position == glm::vec3(1, 0, 0));
    CHECK(merged.vertices[4].position == glm::vec3(-1, 0, 0)); // through the mirrored node
    CHECK(merged.vertices[7].position == glm::vec3(11, 5, 0)); // through the child and its parent
    // Every triangle keeps its facing, the mirrored one included.
    for (std::size_t i = 0; i < merged.indices.size(); i += 3) {
        const auto a = merged.vertices[merged.indices[i]].position;
        const auto b = merged.vertices[merged.indices[i + 1]].position;
        const auto c = merged.vertices[merged.indices[i + 2]].position;
        CHECK(glm::cross(b - a, c - a).y < 0);
    }
    CHECK(sameMaterial(*meshes[1].material, moss));
    CHECK(meshes[1].vertices[0].position == glm::vec3(10, 0, 0));
    for (std::size_t i = 2; i < 6; ++i) {
        CHECK(meshes[i].vertices.size() == 3);
    }
    CHECK(meshes[2].material->transparent);
    CHECK(meshes[4].material->kind == MaterialKind::Water);
}

TEST_CASE("sRGB mip levels round to the nearest byte without a pow per texel") {
    // A solid colour survives halving, whatever its value.
    for (int v = 0; v < 256; ++v) {
        const auto byte = static_cast<std::uint8_t>(v);
        Image image;
        image.width = 2;
        image.height = 2;
        for (int i = 0; i < 4; ++i) {
            image.rgba.insert(image.rgba.end(), {byte, byte, byte, 255});
        }
        CHECK(image.halved(true).rgba[0] == byte);
    }
    // Averages match the sRGB formulas computed directly.
    const auto decode = [](double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const auto encode = [](double c) {
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
    };
    int worst = 0;
    for (int a = 0; a < 256; a += 5) {
        for (int b = a; b < 256; b += 7) {
            Image image;
            image.width = 2;
            image.height = 1;
            const auto x = static_cast<std::uint8_t>(a), y = static_cast<std::uint8_t>(b);
            image.rgba = {x, x, x, 255, y, y, y, 255};
            const double expected = encode((decode(a / 255.0) + decode(b / 255.0)) / 2) * 255.0;
            const int got = image.halved(true).rgba[0];
            worst = std::max(worst, std::abs(got - static_cast<int>(std::lround(expected))));
        }
    }
    CHECK(worst <= 1); // a value on a rounding edge may fall either way
}
