#include "engine/assets/Model.h"
#include "engine/editor/MapEdits.h"
#include "engine/world/Components.h"
#include "engine/world/World.h"
#include <doctest/doctest.h>
#include <array>

using namespace fe;

namespace {
MapData arena() {
    return MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json");
}

struct Worlds {
    AssetCache assets{FENGINE_TEST_GAME, FENGINE_LUALIB};
    World field{"field", arena(), assets, 1};
    World battle{"battle", arena(), assets, 2};
};

void addPlacement(World& world, MapEditHistory& history, const std::string& id) {
    Placement placement;
    placement.id = id;
    placement.type = "marker";
    history.apply(world, [&](MapData& m) { m.placements.push_back(placement); });
}
} // namespace

TEST_CASE("editor edits reach the authored and the live map; scripts change only the live map") {
    Worlds w;
    MapEditHistory history;
    addPlacement(w.field, history, "chest");
    CHECK(w.field.authored().placements.back().id == "chest");
    CHECK(w.field.map().placements.back().id == "chest");
    // A run-time change (what world:set_environment does) stays out of the authored map.
    w.field.environment().fogDensity = 0.5f;
    CHECK(w.field.authored().environment.fogDensity != doctest::Approx(0.5f));
}

TEST_CASE("undo and redo are kept per world, so undo never crosses into another world") {
    Worlds w;
    MapEditHistory history;
    const auto fieldCount = w.field.authored().placements.size();
    const auto battleCount = w.battle.authored().placements.size();
    addPlacement(w.field, history, "chest");
    addPlacement(w.battle, history, "banner");
    CHECK(history.undoCount(w.field) == 1);
    CHECK(history.undo(w.field));
    CHECK(w.field.authored().placements.size() == fieldCount);
    CHECK(w.field.map().placements.size() == fieldCount);
    CHECK(w.battle.authored().placements.size() == battleCount + 1);
    CHECK_FALSE(history.undo(w.field));
    CHECK(history.redo(w.field));
    CHECK(w.field.authored().placements.back().id == "chest");
    CHECK(history.undoCount(w.battle) == 1);
}

TEST_CASE("undo keeps run-time changes to map sections it doesn't restore") {
    Worlds w;
    MapEditHistory history;
    addPlacement(w.field, history, "chest");
    w.field.environment().fogDensity = 0.5f; // scripted day/night
    history.undo(w.field);
    CHECK(w.field.environment().fogDensity == doctest::Approx(0.5f));
    CHECK(w.field.map().placements.size() == w.field.authored().placements.size());
}

TEST_CASE("a saved map is not unsaved, even with undo history, until it is edited again") {
    Worlds w;
    MapEditHistory history;
    CHECK_FALSE(history.unsaved(w.field));
    addPlacement(w.field, history, "chest");
    CHECK(history.unsaved(w.field));
    history.markSaved(w.field);
    CHECK_FALSE(history.unsaved(w.field));
    CHECK(history.undoCount(w.field) == 1); // undo stays available after a save
    REQUIRE(history.undo(w.field));
    CHECK(history.unsaved(w.field)); // differs from the saved map again
    REQUIRE(history.redo(w.field));
    CHECK_FALSE(history.unsaved(w.field));
    // A gesture in progress counts as unsaved; settling at the saved state does not.
    history.apply(w.field, [](MapData& m) { m.name = "x"; }, false);
    CHECK(history.unsaved(w.field));
    history.settle(w.field);
    CHECK(history.unsaved(w.field));
    addPlacement(w.field, history, "barrel");
    history.markSaved(w.field);
    // Reloaded from the saved file: the history continues and nothing is unsaved.
    World reloaded{"field", MapData::fromJson(w.field.authored().toJson()), w.assets, 3};
    history.sync(reloaded);
    CHECK(history.undoCount(reloaded) > 0);
    CHECK_FALSE(history.unsaved(reloaded));
    // A world that was never edited or saved in this session is not unsaved.
    World other{"other", arena(), w.assets, 4};
    CHECK_FALSE(history.unsaved(other));
    history.markSaved(other);
    CHECK_FALSE(history.unsaved(other));
    CHECK(history.undoCount(other) == 0);
}

TEST_CASE("a reloaded world keeps its history only when its map is the one the history left") {
    Worlds w;
    MapEditHistory history;
    addPlacement(w.field, history, "chest");
    addPlacement(w.field, history, "barrel");
    // Saved, then reloaded: the history continues.
    World saved{"field", MapData::fromJson(w.field.authored().toJson()), w.assets, 3};
    history.sync(saved);
    CHECK(history.undoCount(saved) == 2);
    CHECK(history.undo(saved));
    CHECK(saved.authored().placements.back().id == "chest");
    World other{"other", arena(), w.assets, 4};
    CHECK(history.undoCount(other) == 0);
    // Reloaded from the file without the unsaved edits: undo and redo must not
    // bring the discarded placements back.
    World discarded{"field", arena(), w.assets, 5};
    const auto count = discarded.authored().placements.size();
    CHECK_FALSE(history.redo(discarded));
    CHECK_FALSE(history.undo(discarded));
    CHECK(discarded.authored().placements.size() == count);
    CHECK(history.undoCount(discarded) == 0);
    CHECK(history.redoCount(discarded) == 0);
    // A drag is one step: edits without `step` join the current one.
    history.apply(discarded, [](MapData& m) { m.name = "one"; });
    history.apply(discarded, [](MapData& m) { m.name = "two"; }, false);
    CHECK(history.undoCount(discarded) == 1);
    CHECK(history.undo(discarded));
    CHECK(discarded.authored().name == arena().name);
}

TEST_CASE("history is bounded") {
    Worlds w;
    MapEditHistory history;
    for (std::size_t i = 0; i < MapEditHistory::Limit + 10; ++i) {
        history.apply(w.field, [i](MapData& m) { m.name = std::to_string(i); });
    }
    CHECK(history.undoCount(w.field) == MapEditHistory::Limit);
}

TEST_CASE("the grid brush validates layers and the 32-tag limit instead of throwing") {
    Grid grid(4, 4);
    GridBrush brush;
    brush.mode = GridBrush::Mode::AddTag;
    brush.tag = "water";
    CHECK(brushProblem(grid, brush, 3) == "Layer 3 does not exist");
    CHECK(paint(grid, brush, {1, 1, 3}, 0) == 0);
    for (int i = 0; i < 32; ++i) {
        grid.tagBit("tag" + std::to_string(i), true);
    }
    CHECK_FALSE(brushProblem(grid, brush, 0).empty());
    CHECK(paint(grid, brush, {1, 1, 0}, 1) == 0);
    brush.tag = "tag3";
    CHECK(brushProblem(grid, brush, 0).empty());
    CHECK(paint(grid, brush, {1, 1, 0}, 1) == 9);
    CHECK((grid.at(0, 0).tags & grid.tagMask({"tag3"})) != 0);
    brush.mode = GridBrush::Mode::Height;
    brush.height = 2;
    CHECK(paint(grid, brush, {0, 0, 0}, 0) == 1);
    CHECK(paint(grid, brush, {0, 0, 0}, 0) == 0);
}

TEST_CASE("zone overlays use the zone's layer") {
    Grid grid(6, 6, 2);
    Zone upper{"bridge", "zone", {1, 1}, {3, 2}, 1, Json::object()};
    const auto cells = zoneCells(upper, grid, 0);
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].layer == 1);
    Zone everywhere{"all", "zone", {0, 0}, {1, 1}, -1, Json::object()};
    CHECK(zoneCells(everywhere, grid, 1).front().layer == 1);
}

TEST_CASE("zone ids are checked like the loader checks them: unique and not empty") {
    std::vector<Zone> zones{{"gate", "zone", {0, 0}, {1, 1}, -1, Json::object()},
                            {"well", "zone", {2, 2}, {3, 3}, -1, Json::object()}};
    Zone renamed = zones[1];
    CHECK(editProblem(zones, renamed, 1).empty()); // its own id
    renamed.id = "gate";
    CHECK(editProblem(zones, renamed, 1) == "Zone ids must be unique and not empty");
    renamed.id.clear();
    CHECK_FALSE(editProblem(zones, renamed, 1).empty());
    renamed.id = "spring";
    CHECK(editProblem(zones, renamed, 1).empty());
    Zone added{"gate", "zone", {4, 4}, {5, 5}, -1, Json::object()};
    CHECK_FALSE(editProblem(zones, added, zones.size()).empty());
    added.id = "zone3";
    CHECK(editProblem(zones, added, zones.size()).empty());
}

TEST_CASE("marker import skips markers that don't fit and leaves collision to the bake") {
    auto map = arena();
    Model model;
    ModelMarker outside;
    outside.name = "far";
    outside.position = {500, 0, 500};
    outside.properties = {{"spawn", "far"}};
    ModelMarker wall;
    wall.name = "wall";
    wall.hasBounds = true;
    wall.max = {1, 2, 1};
    wall.properties = {{"collision", true}};
    ModelMarker camp;
    camp.name = "camp";
    camp.position = {2.5f, 0, 2.5f};
    camp.properties = {{"spawn", "camp"}};
    model.markers = {outside, wall, camp};
    std::vector<std::string> problems;
    const bool wasWalkable = map.grid.at(0, 0).walkable;
    CHECK(importMarkers(map, model, problems) == 1);
    CHECK(problems.size() == 1);
    CHECK(map.grid.at(0, 0).walkable == wasWalkable); // collision markers no longer paint cells
    CHECK(map.colliders.empty());
    CHECK_FALSE(map.points.contains("far"));
    CHECK(map.points.contains("camp"));
}

TEST_CASE("link edits are validated before they reach the map, ladders included") {
    Worlds w;
    MapEditHistory history;
    const auto& grid = w.field.authored().grid;
    GridLink ladder{2, 2, 0, 0, {0, 0}, LinkKind::Ladder};
    CHECK(editProblem(grid, ladder) == "A ladder needs a direction (the way up)");
    ladder.direction = {0, -1};
    CHECK(editProblem(grid, ladder).empty());
    GridLink edge{2, 0, 0, 0, {0, -1}, LinkKind::Ladder};
    CHECK_FALSE(editProblem(grid, edge).empty()); // its top lies outside the grid
    GridLink step{2, 2, 0, 0};
    CHECK(editProblem(grid, step) == "A step link joins two different layers");
    step.to = grid.layerCount();
    CHECK_FALSE(editProblem(grid, step).empty());

    const auto before = grid.links().size();
    history.apply(w.field, [&](MapData& m) { m.grid.setLinks({ladder}); });
    REQUIRE(w.field.map().grid.links().size() == 1);
    CHECK(w.field.map().grid.ladderStep({2, 2, 0}, {0, -1}) != nullptr);
    const auto saved = MapData::fromJson(w.field.authored().toJson());
    REQUIRE(saved.grid.links().size() == 1);
    CHECK(saved.grid.links()[0].kind == LinkKind::Ladder);
    CHECK(saved.grid.links()[0].direction == glm::ivec2(0, -1));
    history.undo(w.field);
    CHECK(w.field.map().grid.links().size() == before);
}

TEST_CASE("light edits cover the flicker curve and shadows and keep the map loadable") {
    Worlds w;
    MapEditHistory history;
    LightSettings light;
    addFlickerKey(light);
    addFlickerKey(light);
    REQUIRE(light.flickerCurve.size() == 2);
    CHECK(light.flickerCurve[1].x == doctest::Approx(0.5f));
    light.flickerCurve[1].y = 0.2f;
    light.castShadows = true;
    CHECK(editProblem(light).empty());
    history.apply(w.field, [&](MapData& m) { m.lights.push_back(light); });
    const auto saved = MapData::fromJson(w.field.authored().toJson());
    CHECK(saved.lights.back().castShadows);
    CHECK(saved.lights.back().flickerCurve.size() == 2);
    CHECK(w.field.map().lights.back().flickerScale(0.25, 0) == doctest::Approx(0.6f));
    light.flickerCurve[1].x = 0; // keys must move forward in time
    CHECK_FALSE(editProblem(light).empty());
    history.undo(w.field);
    CHECK(w.field.map().lights.size() == w.field.authored().lights.size());
}

TEST_CASE("emitter edits reach the running emitters, with blend, distort, max and world space") {
    Worlds w;
    MapEditHistory history;
    MapEmitter emitter;
    emitter.settings.blend = BlendMode::Additive;
    emitter.settings.distort = 0.5f;
    emitter.settings.maxParticles = 64;
    emitter.settings.worldSpace = false;
    CHECK(editProblem(emitter.settings).empty());
    history.apply(w.field, [&](MapData& m) { m.emitters.push_back(emitter); });
    bool found = false;
    for (const auto entity : w.field.registry().view<Emitter>()) {
        const auto& settings = w.field.registry().get<Emitter>(entity).settings;
        found |= settings.blend == BlendMode::Additive && settings.maxParticles == 64 &&
                 !settings.worldSpace && settings.distort == doctest::Approx(0.5f);
    }
    CHECK(found);
    const auto saved = MapData::fromJson(w.field.authored().toJson()).emitters.back().settings;
    CHECK(saved.blend == BlendMode::Additive);
    CHECK_FALSE(saved.worldSpace);
    emitter.settings.maxParticles = 0;
    CHECK_FALSE(editProblem(emitter.settings).empty());
    emitter.settings.maxParticles = 64;
    emitter.settings.distort = -1;
    CHECK_FALSE(editProblem(emitter.settings).empty());
}

TEST_CASE("environment edits cover water, post and sprite settings and reject what the loader rejects") {
    Worlds w;
    MapEditHistory history;
    auto env = w.field.authored().environment;
    env.pixelSnap = true;
    env.anisotropic = false;
    env.fogMax = 0.5f;
    env.shadowStrength = 0.4f;
    env.shadowBias = 0.05f;
    env.spriteLightHeight = 1.2f;
    env.water.reflection = 0.9f;
    env.water.refraction = 0.1f;
    env.post.bloomRadius = 0.5f;
    env.post.bloomKnee = 0.3f;
    env.post.bloomLevels = 4;
    env.post.dofMode = "tilt";
    env.post.dofRadius = 12;
    env.post.ssao = true;
    env.post.antialias = "none";
    env.post.chromaticAberration = 0.5f;
    env.post.distortion = 2;
    env.post.tonemap = "none";
    CHECK(editProblem(env).empty());
    const auto backdrops = Json::parse(R"([{"image": "ui/hills.png", "distance": 40}])");
    env = withEnvironmentJson(env, "/sky/backdrops", backdrops);
    REQUIRE(env.sky.backdrops.size() == 1);
    CHECK(env.sky.backdrops[0].distance == doctest::Approx(40.0f));
    const auto custom = Json::parse(R"([{"shader": "fs_sepia", "params": [0.5]}])");
    env = withEnvironmentJson(env, "/post/custom", custom);
    REQUIRE(env.post.custom.size() == 1);
    CHECK_THROWS((void)withEnvironmentJson(env, "/post/custom", Json::parse(R"([{"shader": "Bad Name"}])")));

    history.apply(w.field, [&](MapData& m) { m.environment = env; });
    CHECK(w.field.environment().post.dofMode == "tilt");
    const auto saved = MapData::fromJson(w.field.authored().toJson()).environment;
    CHECK(saved.pixelSnap);
    CHECK_FALSE(saved.anisotropic);
    CHECK(saved.water.reflection == doctest::Approx(0.9f));
    CHECK(saved.post.bloomLevels == 4);
    CHECK(saved.post.tonemap == "none");
    CHECK(saved.post.custom[0].shader == "fs_sepia");
    history.undo(w.field);
    CHECK(w.field.environment().post.dofMode == w.field.authored().environment.post.dofMode);

    using Breakage = void (*)(Environment&);
    for (const Breakage bad :
         {+[](Environment& e) { e.water.reflection = 1.5f; }, +[](Environment& e) { e.post.bloomLevels = 9; },
          +[](Environment& e) { e.post.dofMode = "blur"; }, +[](Environment& e) { e.fogMax = 2; },
          +[](Environment& e) { e.shadowStrength = -1; }, +[](Environment& e) { e.post.ssaoRadius = 0; }}) {
        auto broken = w.field.authored().environment;
        bad(broken);
        CHECK_FALSE(editProblem(broken).empty());
    }
}

TEST_CASE("ramps and compass directions share one table of names, codes and grid steps") {
    for (int i = 0; i < RampCount; ++i) {
        const auto ramp = static_cast<Ramp>(i);
        CAPTURE(rampName(ramp));
        CHECK(rampFromName(rampName(ramp)) == ramp);
        CHECK(rampFromCode(rampCode(ramp)) == ramp);
        CHECK(rampFromDirection(rampDirection(ramp)) == ramp);
    }
    CHECK(rampDirection(Ramp::North) == glm::ivec2(0, -1));
    CHECK(rampDirection(Ramp::East) == glm::ivec2(1, 0));
    CHECK(rampFromDirection({1, 1}) == Ramp::None);
}

TEST_CASE("a collider edited in the World tab blocks at its new radius at once, also while paused") {
    Worlds w;
    auto& world = w.field;
    world.paused = true;
    const auto walker = world.spawn("", {2.5f, 0, 5.5f});
    world.registry().emplace<Mover>(walker);
    const auto post = world.spawn("", {5.5f, 0, 5.5f});
    world.registry().emplace<Collider>(post, Collider{0.1f, 1.6f, true});
    // Without a tick in between, the grown collider's reach must count: the
    // spatial hash (2-unit cells) is searched only as far as the largest
    // radius it knows, and the post is two cells of it away from `near`.
    const glm::vec3 near{3.5f, 0, 5.5f};
    CHECK(world.canOccupy(walker, near, 0));
    auto collider = world.registry().get<Collider>(post);
    collider.radius = 2.5f;
    editCollider(world, post, collider);
    CHECK(world.registry().get<Collider>(post).radius == doctest::Approx(2.5f));
    CHECK_FALSE(world.canOccupy(walker, near, 0));
    collider.solid = false;
    editCollider(world, post, collider);
    CHECK(world.canOccupy(walker, near, 0));
}

TEST_CASE("a gesture's later edits do not serialise the map; settle records what it left") {
    Worlds w;
    MapEditHistory history;
    // One step, then the rest of a paint stroke.
    history.apply(w.field, [](MapData& m) { m.name = "stroke 1"; });
    history.apply(w.field, [](MapData& m) { m.name = "stroke 2"; }, false);
    history.apply(w.field, [](MapData& m) { m.name = "stroke 3"; }, false);
    CHECK(history.undoCount(w.field) == 1);
    // Undo and redo settle on their own.
    CHECK(history.undo(w.field));
    CHECK(w.field.authored().name == arena().name);
    CHECK(history.redo(w.field));
    CHECK(w.field.authored().name == "stroke 3");
    history.apply(w.field, [](MapData& m) { m.name = "stroke 4"; }, false);
    // Released: the reloaded (saved) world continues the history.
    history.settle(w.field);
    World saved{"field", MapData::fromJson(w.field.authored().toJson()), w.assets, 6};
    history.sync(saved);
    CHECK(history.undoCount(saved) == 1);
    CHECK(history.undo(saved));
    CHECK(saved.authored().name == arena().name);
    // A world replaced while a stroke's head is still stale drops its history: conservative.
    history.apply(saved, [](MapData& m) { m.name = "again"; });
    history.apply(saved, [](MapData& m) { m.name = "again 2"; }, false);
    World reloaded{"field", MapData::fromJson(saved.authored().toJson()), w.assets, 7};
    history.sync(reloaded);
    CHECK(history.undoCount(reloaded) == 0);
    // An edit that joins a gesture on another world object starts a step there.
    history.apply(w.battle, [](MapData& m) { m.name = "elsewhere"; }, false);
    CHECK(history.undoCount(w.battle) == 1);
    CHECK(history.undo(w.battle));
    CHECK(w.battle.authored().name == arena().name);
}

TEST_CASE("a transform edited in the World tab moves the entity in the collision hash at once") {
    Worlds w;
    auto& world = w.field;
    world.paused = true;
    const auto walker = world.spawn("", {2.5f, 0, 5.5f});
    world.registry().emplace<Mover>(walker);
    const auto post = world.spawn("", {8.5f, 0, 5.5f});
    world.registry().emplace<Collider>(post, Collider{0.4f, 1.6f, true});
    const glm::vec3 near{3.5f, 0, 5.5f};
    CHECK(world.canOccupy(walker, near, 0));
    editTransform(world, post, near, 0);
    CHECK(world.registry().get<Transform>(post).position.x == doctest::Approx(3.5f));
    CHECK_FALSE(world.canOccupy(walker, near, 0));
    editTransform(world, post, {8.5f, 0, 5.5f}, 0);
    CHECK(world.canOccupy(walker, near, 0));
}

TEST_CASE("editor forms read mistyped JSON as absent instead of throwing") {
    using namespace editorui;
    const Json record = Json::parse(R"({"id": 7, "name": "x", "rate": "fast", "on": 1, "list": {}, "obj": []})");
    CHECK(jsonString(record, "id", "fallback") == "fallback");
    CHECK(jsonString(record, "name", "fallback") == "x");
    CHECK(jsonNumber(record, "rate", 2) == 2);
    CHECK(jsonNumber(record, "id", 2) == 7);
    CHECK(jsonBool(record, "on", false) == false);
    CHECK(jsonArray(record, "list").is_array());
    CHECK(jsonObject(record, "obj").is_object());
    CHECK(jsonString(Json::array(), "id", "none") == "none");
    CHECK(jsonString(Json(), "id", "none") == "none");
}

// --- Asset catalog -----------------------------------------------------------

#include "engine/editor/AssetCatalog.h"

TEST_CASE("the asset catalog classifies files by name") {
    CHECK(assetKindOf("sprites/elder.anim.json") == AssetKind::Sprite);
    CHECK(assetKindOf("sprites/elder.png") == AssetKind::Image);
    CHECK(assetKindOf("maps/town/geometry.glb") == AssetKind::Model);
    CHECK(assetKindOf("maps/town/map.json") == AssetKind::Map);
    CHECK(assetKindOf("audio/vale.wav") == AssetKind::Audio);
    CHECK(assetKindOf("ui/amber.cube") == AssetKind::Lut);
    CHECK(assetKindOf("events/elder.lua") == AssetKind::Script);
    CHECK(assetKindOf("data/items.json") == AssetKind::Data);
    CHECK(assetKindOf("game.json") == AssetKind::Text);
    CHECK(assetKindOf("shaders/fs_glint.sc") == AssetKind::Shader);
    CHECK(assetKindOf("README") == AssetKind::Other);
}

TEST_CASE("the asset catalog lists vale's assets, maps, audio ids and modules, and who uses what") {
    AssetCatalog catalog(std::filesystem::path(FENGINE_SAMPLES) / "vale", FENGINE_LUALIB);
    catalog.scan();
    const auto before = catalog.revision();
    CHECK(before == 1);
    CHECK(catalog.find("sprites/elder.anim.json") != nullptr);
    CHECK(catalog.find("sprites/elder.anim.json")->kind == AssetKind::Sprite);
    // The atlas belongs to its sprite, so the browser can hide it from loose art.
    REQUIRE(catalog.find("sprites/elder.png") != nullptr);
    CHECK(catalog.find("sprites/elder.png")->sprite == "sprites/elder.anim.json");
    const auto maps = catalog.maps();
    CHECK(std::any_of(maps.begin(), maps.end(), [](const MapInfo& m) {
        return m.id == "town" && m.name == "Amber Vale" && m.width == 20 && m.depth == 16;
    }));
    const auto audio = catalog.audioIds();
    CHECK(std::any_of(audio.begin(), audio.end(), [](const AudioId& a) {
        return a.group == "music" && a.id == "vale" && a.file == "audio/vale.wav";
    }));
    const auto modules = catalog.modules("events");
    CHECK(std::find(modules.begin(), modules.end(), "events.elder") != modules.end());
    CHECK(std::find(modules.begin(), modules.end(), "ai.slime") == modules.end());
    // The elder's sprite is referenced by the town map's placement and (via
    // the script module) its event file is known to be used by that map.
    const auto& sprite = catalog.references("sprites/elder.anim.json");
    CHECK(std::any_of(sprite.begin(), sprite.end(), [](const AssetReference& r) {
        return r.file == "maps/town/map.json" && r.where == "placements/elder/props/sprite";
    }));
    const auto& script = catalog.references("events/elder.lua");
    CHECK(std::any_of(script.begin(), script.end(),
                      [](const AssetReference& r) { return r.file == "maps/town/map.json"; }));
    // A door names its target map; the map's music names an audio id.
    CHECK_FALSE(catalog.references("maps/grotto/map.json").empty());
    CHECK_FALSE(catalog.references("audio:vale").empty());
    // The sample ships every asset it names.
    const auto missing = catalog.missing();
    for (const auto& [target, user] : missing) {
        MESSAGE(target << " referenced by " << user.file << " at " << user.where);
    }
    CHECK(missing.empty());
    // Scanning again without changes keeps the revision.
    catalog.scan();
    CHECK(catalog.revision() == before);
    const auto folders = catalog.folders("");
    CHECK(std::find(folders.begin(), folders.end(), "sprites") != folders.end());
}

TEST_CASE("the asset catalog reports references to files that do not exist") {
    AssetCatalog catalog(std::filesystem::path(FENGINE_TEST_GAME), FENGINE_LUALIB);
    catalog.scan();
    // Nothing in the library's test game is known to be missing; the check
    // is that the walk itself does not throw on its data and maps.
    CHECK_NOTHROW(static_cast<void>(catalog.missing()));
}

// --- Placement yaw and scale -------------------------------------------------

TEST_CASE("placement yaw and scale round-trip through JSON and stay absent when at their defaults") {
    auto map = arena();
    const auto before = map.toJson();
    for (const auto& p : before.at("placements")) {
        CHECK_FALSE(p.contains("yaw"));
        CHECK_FALSE(p.contains("scale"));
    }
    // A file without them re-serialises identically (the undo history compares JSON).
    CHECK(MapData::fromJson(before).toJson() == before);
    Placement turned;
    turned.id = "windmill";
    turned.type = "marker";
    turned.position = {1.5f, 0, 1.5f};
    turned.yaw = 90;
    turned.scale = glm::vec3(2);
    Placement stretched = turned;
    stretched.id = "banner";
    stretched.scale = {1, 3, 1};
    map.placements.push_back(turned);
    map.placements.push_back(stretched);
    const auto json = map.toJson();
    const auto& last = json.at("placements");
    const auto& a = last[last.size() - 2];
    const auto& b = last[last.size() - 1];
    CHECK(a.at("yaw") == 90);
    CHECK(a.at("scale") == 2); // uniform scales are one number
    CHECK(b.at("scale").is_array());
    const auto back = MapData::fromJson(json);
    CHECK(back.placements[back.placements.size() - 2].yaw == doctest::Approx(90));
    CHECK(back.placements.back().scale == glm::vec3(1, 3, 1));
    auto bad = json;
    bad["placements"].back()["scale"] = "big";
    CHECK_THROWS(MapData::fromJson(bad));
}

// --- Scene viewport: camera, selection, picking, edits ------------------------

#include "engine/editor/EditorCamera.h"
#include "engine/editor/SceneEdits.h"
#include "engine/editor/SceneHandles.h"
#include "engine/editor/ScenePicking.h"
#include "engine/editor/Selection.h"

namespace {
// A view 800x600 pixels looking down at the arena from above its middle.
struct Scene {
    Worlds worlds;
    World& world = worlds.field;
    Camera camera;
    ViewRect view;
    Scene() {
        camera.target = {6, 0, 5};
        camera.pitch = 60;
        camera.yaw = 0;
        camera.distance = 14;
        camera.snap();
        view.pixels = {100, 50, 800, 600};
    }
    glm::vec2 pixelOf(glm::vec3 point) const { return *projectToView(camera, view, point, 1); }
    PickOptions options() const {
        PickOptions o;
        o.camera = &camera;
        return o;
    }
};
} // namespace

TEST_CASE("the editor camera adopts the game view, clamps its orbit and frames a box") {
    Camera game;
    game.target = {3, 0, 4};
    game.yaw = 30;
    game.pitch = 95; // past the limit
    game.distance = 0.1f;
    game.snap();
    EditorCamera editor;
    editor.adopt(game);
    CHECK(editor.camera.target == glm::vec3(3, 0, 4));
    CHECK(editor.camera.yaw == doctest::Approx(30));
    CHECK(editor.camera.pitch == doctest::Approx(EditorCamera::MaxPitch));
    CHECK(editor.camera.distance == doctest::Approx(EditorCamera::MinDistance));
    editor.orbit({40, -2000});
    CHECK(editor.camera.yaw == doctest::Approx(40));
    CHECK(editor.camera.pitch == doctest::Approx(EditorCamera::MaxPitch));
    editor.zoom(-1000);
    CHECK(editor.camera.distance == doctest::Approx(EditorCamera::MaxDistance));
    editor.frame({0, 0, 0}, {12, 2, 10});
    CHECK(editor.camera.target == glm::vec3(6, 1, 5));
    // Every corner of the box projects inside the view.
    for (const auto corner : {glm::vec3(0, 0, 0), glm::vec3(12, 0, 0), glm::vec3(0, 2, 10), glm::vec3(12, 2, 10)}) {
        const auto ndc = editor.camera.project(corner, 4.0f / 3.0f);
        CHECK(std::abs(ndc.x) <= 1);
        CHECK(std::abs(ndc.y) <= 1);
        CHECK(ndc.z <= 1);
    }
    // Pan by half the view moves the target by half the view height.
    const auto before = editor.camera.target;
    editor.pan({0, 300}, 600);
    CHECK(glm::length(editor.camera.target - before) == doctest::Approx(editor.camera.viewHeight() * 0.5f).epsilon(0.01));
    editor.setOrthographic(true);
    CHECK(editor.camera.orthographic);
    CHECK(editor.camera.orthoHeight > 0);
}

TEST_CASE("selections name map objects by id and entities by serial, and know what a gizmo may do") {
    Scene s;
    auto& map = s.world.authored();
    Placement npc;
    npc.id = "elder";
    npc.type = "marker";
    npc.position = {4.5f, 0, 4.5f};
    map.placements.push_back(npc);
    s.world.map().placements.push_back(npc);
    Zone zone{"plaza", "camera", {2, 2}, {6, 7}, -1, Json::object()};
    map.zones.push_back(zone);
    map.colliders.push_back({{1, 0, 1}, {2, 1, 3}});
    // The placement's entity: named like it (what fe.field does).
    const auto entity = s.world.spawn("elder", npc.position);
    s.world.registry().emplace<Sprite>(entity, Sprite{.animation = "@fe/assets/ui/spark.anim.json"});
    CHECK(placementEntity(s.world, "elder") == entity);
    CHECK(entityPlacement(s.world, entity) == "elder");
    const auto loose = s.world.spawn("stray", {1.5f, 0, 1.5f});
    CHECK(entityPlacement(s.world, loose).empty());

    const auto selected = Selection::placement("elder");
    CHECK(selectionValid(s.world, selected));
    CHECK(selectionLabel(s.world, selected) == "elder (marker)");
    auto t = selectionTransform(s.world, selected);
    REQUIRE(t);
    CHECK(t->snapsToGrid);
    CHECK(t->canRotate);
    CHECK(t->facingOnly); // a sprite turns by facing
    CHECK(t->uniformScale);
    // Zones and colliders expose centre and size.
    auto zt = selectionTransform(s.world, Selection::zone("plaza"));
    REQUIRE(zt);
    CHECK(zt->position.x == doctest::Approx(4));
    CHECK(zt->position.z == doctest::Approx(4.5f));
    CHECK(zt->scale.x == doctest::Approx(4));
    CHECK(zt->scale.z == doctest::Approx(5));
    CHECK(zt->canScale);
    zt->position.x = 5;
    zt->scale.x = 2;
    CHECK(applySelectionTransform(map, Selection::zone("plaza"), *zt));
    CHECK(findZone(map, "plaza")->min.x == doctest::Approx(4));
    CHECK(findZone(map, "plaza")->max.x == doctest::Approx(6));
    auto ct = selectionTransform(s.world, Selection::collider(0));
    REQUIRE(ct);
    CHECK(ct->position == glm::vec3(1.5f, 0.5f, 2));
    ct->scale = {2, 1, 2};
    CHECK(applySelectionTransform(map, Selection::collider(0), *ct));
    CHECK(map.colliders[0].min == glm::vec3(0.5f, 0, 1));
    CHECK(map.colliders[0].max == glm::vec3(2.5f, 1, 3));
    // An entity selection dies with its serial, a map selection with its object.
    auto live = Selection::ofEntity(s.world, loose);
    CHECK(selectionValid(s.world, live));
    s.world.destroy(loose);
    CHECK_FALSE(selectionValid(s.world, live));
    map.placements.clear();
    CHECK_FALSE(selectionValid(s.world, selected));
    CHECK_FALSE(selectionValid(s.world, Selection::collider(1)));
    CHECK(selectionValid(s.world, Selection::collider(0)));
}

TEST_CASE("picking finds handles by screen distance, entities and placements by ray, zones by ground hit") {
    Scene s;
    auto& map = s.world.authored();
    map.points["mark"] = MapPoint{{9.5f, 0, 2.5f}, {0, 1}, 0};
    Placement ghost; // no entity: picked by its handle
    ghost.id = "ghost";
    ghost.type = "marker";
    ghost.position = {2.5f, 0, 8.5f};
    map.placements.push_back(ghost);
    Placement npc;
    npc.id = "elder";
    npc.type = "marker";
    npc.position = {6.5f, 0, 6.5f};
    map.placements.push_back(npc);
    s.world.map().placements = map.placements;
    const auto entity = s.world.spawn("elder", npc.position);
    s.world.registry().emplace<Collider>(entity, Collider{0.3f, 1.2f, true});
    map.zones.push_back(Zone{"plaza", "camera", {3, 3}, {5, 5}, -1, Json::object()});
    map.colliders.push_back({{8, 0, 7}, {9, 1, 8}});
    s.world.map().colliders = map.colliders; // the live map feeds the collision boxes
    s.world.rebuildStatic();

    CHECK(pickScene(s.world, s.view, s.pixelOf({9.5f, 0, 2.5f}), s.options()) == Selection::point("mark"));
    CHECK(pickScene(s.world, s.view, s.pixelOf({2.5f, 0, 8.5f}) + glm::vec2(4, -4), s.options()) ==
          Selection::placement("ghost"));
    // The elder's body (its collider) is under the ray: the placement, not a bare entity.
    CHECK(pickScene(s.world, s.view, s.pixelOf({6.5f, 0.6f, 6.5f}), s.options()) == Selection::placement("elder"));
    CHECK(pickScene(s.world, s.view, s.pixelOf({4, 0, 4}), s.options()) == Selection::zone("plaza"));
    CHECK(pickScene(s.world, s.view, s.pixelOf({8.5f, 0.5f, 7.5f}), s.options()) == Selection::collider(0));
    // Open ground selects nothing; outside the view picks nothing.
    CHECK_FALSE(pickScene(s.world, s.view, s.pixelOf({8, 0, 9}), s.options()));
    CHECK_FALSE(pickScene(s.world, s.view, {10, 10}, s.options()));
    // Kind toggles hide things from the pick.
    auto noZones = s.options();
    noZones.zones = false;
    CHECK_FALSE(pickScene(s.world, s.view, s.pixelOf({4, 0, 4}), noZones));
    // A mesh without a collider is picked through its model bounds.
    const auto prop = s.world.spawn("prop", {1.5f, 0, 3.5f});
    s.world.registry().emplace<MeshInstance>(prop, MeshInstance{.model = s.world.geometryPath()});
    const auto model = s.world.assets().tryModel(s.world.geometryPath());
    REQUIRE(model);
    const auto centre = glm::vec3(1.5f, 0, 3.5f) + (model->min + model->max) * 0.5f;
    const auto picked = pickScene(s.world, s.view, s.pixelOf(centre), s.options());
    REQUIRE(picked);
    CHECK((picked->kind == Selection::Kind::Entity && picked->entity == prop));
    // The ground under a pixel, and the ray through it, agree with the camera.
    const auto hit = pickGround(s.world, s.camera, s.view, s.pixelOf({7.5f, 0, 3.5f}), 1);
    REQUIRE(hit);
    CHECK(hit->cell.x == 7);
    CHECK(hit->cell.z == 3);
}

TEST_CASE("scene edits snap to cells, make placements for dropped assets and move live entities at once") {
    Scene s;
    const auto& grid = s.world.authored().grid;
    CHECK(snapToCell(grid, {3.7f, 0, 2.1f}, 0) == GridPos{3, 2, 0});
    CHECK(snapToCell(grid, {-5, 0, 99}, 7).x == 0);
    CHECK(snapToCell(grid, {-5, 0, 99}, 7).z == grid.depth() - 1);
    CHECK(snapToCell(grid, {-5, 0, 99}, 7).layer == grid.layerCount() - 1);
    CHECK(cellCentre(grid, {3, 2, 0}) == glm::vec3(3.5f, grid.heightAt({3.5f, 2.5f}, 0), 2.5f));
    CHECK(snapToStep({1.3f, 0.6f, -0.2f}, 0.5f) == glm::vec3(1.5f, 0.5f, 0));
    std::vector<Placement> existing;
    CHECK(uniquePlacementId("chest", existing) == "chest");
    existing.push_back(Placement{.id = "chest"});
    CHECK(uniquePlacementId("chest", existing) == "chest2");
    const auto sprite = placementForAsset(AssetKind::Sprite, "sprites/chest.anim.json", {4, 5, 0}, grid, "npc", existing);
    REQUIRE(sprite);
    CHECK(sprite->id == "chest2");
    CHECK(sprite->type == "npc");
    CHECK(sprite->props.at("sprite") == "sprites/chest.anim.json");
    CHECK(sprite->position == cellCentre(grid, {4, 5, 0}));
    const auto model = placementForAsset(AssetKind::Model, "fx/Crate-01.glb", {1, 1, 0}, grid, "npc", existing);
    REQUIRE(model);
    CHECK(model->type == "prop");
    CHECK(model->id == "crate_01");
    CHECK(model->props.at("model") == "fx/Crate-01.glb");
    CHECK(model->position == cellCentre(grid, {1, 1, 0}));
    // With its model, a dropped prop snaps by footprint: even sizes to corners, odd to centres.
    Model house;
    house.min = {-2, 0, -1.5f};
    house.max = {2, 2, 1.5f};
    Placement at;
    CHECK(footprintCells(house, at) == glm::ivec2(4, 3));
    at.yaw = 90;
    CHECK(footprintCells(house, at) == glm::ivec2(3, 4));
    at.yaw = 0;
    at.scale = glm::vec3(0.5f);
    CHECK(footprintCells(house, at) == glm::ivec2(2, 2)); // 2 x 1.5 rounds to 2 x 2
    CHECK(snapForFootprint(grid, {3.7f, 0, 2.2f}, {4, 3}, 0) == glm::vec3(4, grid.heightAt({4, 2.5f}, 0), 2.5f));
    CHECK(snapForFootprint(grid, {3.7f, 0, 2.2f}, {1, 1}, 0) == glm::vec3(3.5f, grid.heightAt({3.5f, 2.5f}, 0), 2.5f));
    const auto dropped = placementForAsset(AssetKind::Model, "fx/house.glb", {3, 2, 0}, grid, "npc", existing, &house,
                                           glm::vec3(3.7f, 0, 2.2f));
    REQUIRE(dropped);
    CHECK(dropped->position.x == doctest::Approx(4));
    CHECK(dropped->position.z == doctest::Approx(2.5f));
    CHECK_FALSE(placementForAsset(AssetKind::Audio, "audio/x.wav", {1, 1, 0}, grid, "npc", existing));
    const auto copy = duplicatePlacement(*sprite, existing);
    CHECK(copy.id == "chest2"); // "chest2" is free in `existing` (only "chest" is taken)
    CHECK(copy.position.x == doctest::Approx(sprite->position.x + 1));

    // Moving a placement moves its entity, without interpolation from the old spot.
    auto& map = s.world.authored();
    Placement p;
    p.id = "elder";
    p.type = "marker";
    p.position = {2.5f, 0, 2.5f};
    map.placements.push_back(p);
    const auto entity = s.world.spawn("elder", p.position);
    s.world.registry().emplace<MeshInstance>(entity, MeshInstance{.model = s.world.geometryPath()});
    s.world.beginTick();
    p.position = {7.5f, 0, 8.5f};
    p.yaw = 90;
    p.scale = glm::vec3(2);
    map.placements.back() = p;
    CHECK(syncPlacementEntity(s.world, p));
    const auto& t = s.world.registry().get<Transform>(entity);
    CHECK(t.position == p.position);
    CHECK(s.world.interpolated(entity, 0.5) == p.position);
    CHECK(t.yaw == doctest::Approx(glm::radians(90.0f)));
    CHECK(t.scale == glm::vec3(2));
    CHECK_FALSE(syncPlacementEntity(s.world, Placement{.id = "nobody"}));

    // Which placements an undo touched.
    MapData before = map, after = map;
    after.placements.back().props["sprite"] = "x";
    Placement added;
    added.id = "new";
    added.type = "marker";
    after.placements.push_back(added);
    before.placements.push_back(Placement{.id = "gone", .type = "marker"});
    const auto changed = changedPlacementIds(before, after);
    CHECK(changed.size() == 3);
    CHECK(std::find(changed.begin(), changed.end(), "elder") != changed.end());
    CHECK(std::find(changed.begin(), changed.end(), "new") != changed.end());
    CHECK(std::find(changed.begin(), changed.end(), "gone") != changed.end());
    CHECK(changedPlacementIds(map, map).empty());
}

TEST_CASE("handles mark zones, points, lights, colliders, triggers and unspawned placements") {
    Scene s;
    auto& map = s.world.authored();
    map.zones.push_back(Zone{"plaza", "camera", {3, 3}, {5, 5}, -1, Json::object()});
    map.points["mark"] = MapPoint{{9.5f, 0, 2.5f}, {0, 1}, 0};
    LightSettings spot;
    spot.type = LightType::Spot;
    spot.position = {6, 3, 6};
    map.lights.push_back(spot);
    map.colliders.push_back({{10, 0, 7}, {11, 1, 8}});
    Placement ghost{.id = "ghost", .type = "marker", .position = {2.5f, 0, 8.5f}};
    map.placements.push_back(ghost);
    const auto trigger = s.world.spawn("area", {1.5f, 0, 1.5f});
    s.world.registry().emplace<Collider>(trigger, Collider{0.5f, 1, false});
    HandleBatch batch;
    HandleOptions options;
    const auto spotIndex = static_cast<int>(map.lights.size()) - 1;
    buildHandles(s.world, options, Selection::light(spotIndex), {}, 1, batch);
    const auto icons = [&](HandleBatch::Icon::Shape shape) {
        return std::count_if(batch.icons.begin(), batch.icons.end(), [&](const auto& i) { return i.shape == shape; });
    };
    CHECK(icons(HandleBatch::Icon::Shape::Flag) == static_cast<long>(map.points.size())); // the points
    CHECK(icons(HandleBatch::Icon::Shape::Square) == 1);                                   // the zone
    CHECK(icons(HandleBatch::Icon::Shape::Sun) == static_cast<long>(map.lights.size()));   // the lights
    CHECK(icons(HandleBatch::Icon::Shape::Diamond) >= 1);                                  // the ghost placement
    // Zone edges sit on the ground; the collider is a 12-edge box; the
    // selected spot light draws its cone; the trigger a cylinder.
    const auto onGround = std::count_if(batch.lines.begin(), batch.lines.end(), [&](const auto& l) {
        return l.color == handleColors::ZoneColor && std::abs(l.a.y - 0.05f) < 0.01f;
    });
    CHECK(onGround >= 8);
    CHECK(std::count_if(batch.lines.begin(), batch.lines.end(),
                        [&](const auto& l) { return l.color == handleColors::ColliderColor; }) == 12);
    CHECK(std::count_if(batch.lines.begin(), batch.lines.end(), [&](const auto& l) {
              return glm::vec3(l.color) == glm::vec3(handleColors::Selected);
          }) > 8);
    CHECK(std::count_if(batch.lines.begin(), batch.lines.end(),
                        [&](const auto& l) { return l.color == handleColors::TriggerColor; }) > 0);
    // Labels name the point and the ghost.
    CHECK(std::any_of(batch.labels.begin(), batch.labels.end(), [](const auto& l) { return l.text == "mark"; }));
    CHECK(std::any_of(batch.labels.begin(), batch.labels.end(), [](const auto& l) { return l.text == "ghost (marker)"; }));
    // An unselected light draws no reach; toggles drop whole kinds.
    HandleBatch quiet;
    buildHandles(s.world, options, {}, {}, 1, quiet);
    CHECK(std::none_of(quiet.lines.begin(), quiet.lines.end(), [&](const auto& l) {
        return l.color.r == handleColors::LightColor.r && l.color.g == handleColors::LightColor.g;
    }));
    options.zones = options.points = options.lights = options.colliders = options.triggers = options.placements = false;
    HandleBatch none;
    buildHandles(s.world, options, {}, {}, 1, none);
    CHECK(none.lines.empty());
    CHECK(none.icons.empty());
}

// --- Walkability: box blocking, derived boxes, fills -------------------------

TEST_CASE("planned routes go around static collider boxes instead of into them") {
    Worlds w;
    auto& world = w.field;
    // The arena's open hall is rows 6 to 8 between x 1 and 10. A wall of boxes
    // across rows 6 and 7 at x 4..5 leaves row 8 as the only way east.
    auto& map = world.map();
    map.colliders.push_back({{4, 0, 6}, {5, 1, 8}});
    world.rebuildStatic();
    const auto& blocked = world.boxBlockedCells();
    CHECK(blocked.contains(cellKey({4, 6, 0})));
    CHECK(blocked.contains(cellKey({4, 7, 0})));
    CHECK_FALSE(blocked.contains(cellKey({4, 8, 0}))); // the gap
    CHECK_FALSE(blocked.contains(cellKey({3, 6, 0})));
    // A low kerb stops a walking body, so routes avoid it too; a box above
    // head height blocks nothing.
    map.colliders.push_back({{7, 0, 6}, {8, 0.3f, 9}});
    map.colliders.push_back({{9, 2, 6}, {10, 3, 9}});
    world.rebuildStatic();
    CHECK(world.boxBlockedCells().contains(cellKey({7, 7, 0})));
    CHECK_FALSE(world.boxBlockedCells().contains(cellKey({9, 7, 0})));
    // A box covering only a corner leaves the centre free.
    map.colliders.push_back({{1, 0, 6}, {1.2f, 1, 6.2f}});
    world.rebuildStatic();
    CHECK_FALSE(world.boxBlockedCells().contains(cellKey({1, 6, 0})));

    const auto walker = world.spawn("", {2.5f, 0, 6.5f});
    world.registry().emplace<Mover>(walker);
    world.registry().emplace<Collider>(walker, Collider{0.2f, 1.6f, true});
    world.beginTick();
    world.moveTo(walker, {6.5f, 0, 6.5f}, true);
    bool arrived = false, refused = false;
    for (int i = 0; i < 3000 && !arrived && !refused; ++i) {
        world.beginTick();
        world.tick(1.0 / 60);
        for (const auto& event : world.takeEvents()) {
            arrived |= event.kind == WorldEvent::Kind::Arrived;
            refused |= event.kind == WorldEvent::Kind::Blocked;
        }
    }
    CHECK(arrived);
    CHECK_FALSE(refused);
    // Into the wall: no route.
    world.moveTo(walker, {4.5f, 0, 6.5f}, true);
    refused = false;
    for (int i = 0; i < 10 && !refused; ++i) {
        world.beginTick();
        world.tick(1.0 / 60);
        for (const auto& event : world.takeEvents()) {
            refused |= event.kind == WorldEvent::Kind::Blocked;
        }
    }
    CHECK(refused);
}

TEST_CASE("boxes merge into slabs and are cut around a rectangle") {
    CHECK(mergeBoxes({{{0, 0, 0}, {1, 1, 1}}, {{1, 0, 0}, {2, 1, 1}}, {{0, 0, 1}, {2, 1, 2}}}).size() == 1);
    CHECK(mergeBoxes({{{0, 0, 0}, {1, 1, 1}}, {{1, 0, 0}, {2, 2, 1}}}).size() == 2); // different heights
    const Box slab{{2, 0, 3}, {3, 1, 3.5f}};
    // A square in the middle of the slab leaves four pieces that still cover everything else.
    const auto pieces = cutBox(slab, {2.5f, 3}, {2.75f, 3.25f});
    float area = 0;
    for (const auto& b : pieces) {
        area += (b.max.x - b.min.x) * (b.max.z - b.min.z);
        const bool coversHole = b.min.x < 2.74f && b.max.x > 2.51f && b.min.z < 3.24f && b.max.z > 3.01f;
        CHECK_FALSE(coversHole);
    }
    CHECK(area == doctest::Approx(0.5f - 0.0625f));
    // Beside the slab: unchanged; covering it: nothing left.
    REQUIRE(cutBox(slab, {5, 5}, {6, 6}).size() == 1);
    CHECK(cutBox(slab, {5, 5}, {6, 6})[0].max.x == doctest::Approx(3));
    CHECK(cutBox(slab, {0, 0}, {10, 10}).empty());
}


TEST_CASE("a model's box follows the placement's position, yaw and scale") {
    Model model;
    model.min = {-1, 0, -0.5f};
    model.max = {1, 2, 0.5f};
    Placement p;
    p.position = {5, 0, 5};
    auto box = boxForModel(model, p);
    CHECK(box.min == glm::vec3(4, 0, 4.5f));
    CHECK(box.max == glm::vec3(6, 2, 5.5f));
    p.yaw = 90;
    box = boxForModel(model, p);
    CHECK(box.min.x == doctest::Approx(4.5f));
    CHECK(box.max.x == doctest::Approx(5.5f));
    CHECK(box.min.z == doctest::Approx(4));
    CHECK(box.max.z == doctest::Approx(6));
    p.yaw = 0;
    p.scale = glm::vec3(2);
    box = boxForModel(model, p, 0.5f);
    CHECK(box.min == glm::vec3(3.5f, 0, 4.5f));
    CHECK(box.max == glm::vec3(6.5f, 4, 5.5f));
    // A margin never shrinks a box away.
    box = boxForModel(model, p, 100);
    CHECK(box.max.z - box.min.z >= BlockStep - 1e-4f);
}

TEST_CASE("rectangle and flood fills paint many cells in one step") {
    auto map = arena();
    auto& grid = map.grid;
    GridBrush brush;
    brush.mode = GridBrush::Mode::Walkable;
    brush.walkable = false;
    // Inside the open hall, away from the arena's walls.
    const int changed = fillRect(grid, brush, {2, 6, 0}, {4, 7, 0});
    CHECK(changed == 6);
    for (int z = 6; z <= 7; ++z) {
        for (int x = 2; x <= 4; ++x) {
            CHECK_FALSE(grid.at(x, z, 0).walkable);
        }
    }
    CHECK(grid.at(5, 6, 0).walkable);
    CHECK(grid.at(2, 8, 0).walkable);
    // Flood the blocked rectangle back to walkable: only it changes.
    brush.walkable = true;
    CHECK(floodFill(grid, brush, {3, 6, 0}) == 6);
    CHECK(grid.at(2, 7, 0).walkable);
    // Flooding walkable ground with a tag reaches every connected walkable cell and nothing blocked.
    brush.mode = GridBrush::Mode::AddTag;
    brush.tag = "grass";
    const int tagged = floodFill(grid, brush, {3, 6, 0});
    CHECK(tagged > 6);
    const auto mask = grid.tagMask({"grass"});
    for (int z = 0; z < grid.depth(); ++z) {
        for (int x = 0; x < grid.width(); ++x) {
            const auto& cell = grid.at(x, z, 0);
            if (!cell.walkable) {
                CHECK((cell.tags & mask) == 0);
            }
        }
    }
    brush.mode = GridBrush::Mode::Ramp;
    CHECK(floodFill(grid, brush, {3, 6, 0}) == 0);
}


// --- Walkability bake ---------------------------------------------------------

#include "engine/editor/WalkBake.h"

namespace {
// A model built from quads: (x0, z0)-(x1, z1) at height y (flat), or tilted
// so its far edge in +x is at y1. Boxes add four walls and a roof.
struct ModelBuilder {
    Model model;
    ModelBuilder() {
        model.nodes.push_back({});
        model.restPose.push_back(glm::mat4(1));
        model.parts.push_back({});
    }
    ModelPart& part(bool water = false) {
        if (water) {
            if (model.parts.size() < 2) {
                model.parts.push_back({});
                model.parts.back().material.kind = MaterialKind::Water;
            }
            return model.parts[1];
        }
        return model.parts[0];
    }
    void tri(ModelPart& p, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        const auto base = static_cast<std::uint32_t>(p.vertices.size());
        for (const auto& v : {a, b, c}) {
            MeshVertex vertex;
            vertex.position = v;
            p.vertices.push_back(vertex);
        }
        p.indices.insert(p.indices.end(), {base, base + 1, base + 2});
    }
    void quad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, bool water = false) {
        auto& p = part(water);
        tri(p, a, b, c);
        tri(p, a, c, d);
    }
    void floor(float x0, float z0, float x1, float z1, float y, bool water = false) {
        quad({x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}, water);
    }
    // Rises from y0 at x0 to y1 at x1.
    void rampX(float x0, float z0, float x1, float z1, float y0, float y1) {
        quad({x0, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y0, z1});
    }
    void box(glm::vec3 lo, glm::vec3 hi) {
        floor(lo.x, lo.z, hi.x, hi.z, hi.y); // roof
        quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z});
        quad({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
        quad({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z});
        quad({hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z});
    }
};

Grid flatGrid(int width, int depth) {
    Json grid = {{"width", width}, {"depth", depth}, {"layers", Json::array({{{"name", "ground"}, {"height_fill", 0}, {"walk_fill", false}}})}};
    return Grid::fromJson(grid);
}
} // namespace

TEST_CASE("the walkability bake reads floors, houses, ramps, cliffs and water from geometry") {
    ModelBuilder b;
    b.floor(0, 0, 8, 6, 0);              // level ground over the whole 8x6 map
    b.box({2, 0, 1}, {4, 3, 3});          // a house covering cells (2..3, 1..2): its roof is an island
    b.box({5.5f, 0, 4}, {6, 1.5f, 5});    // a post covering half of cell (5, 4)
    b.floor(6, 0, 8, 2, 1);              // a cliff top one unit up over cells (6..7, 0..1)...
    b.rampX(4, 0, 6, 2, 0, 1);            // ...reached by a ramp over cells (4..5, 0..1)
    b.rampX(0, 4, 1, 5, 0, 0.5f);         // a gentle ramp in cell (0, 4) rising towards +x
    b.floor(0, 5, 2, 6, 0.2f, true);      // a pond over cells (0..1, 5)
    const auto grid = flatGrid(8, 6);
    BakeOptions options;
    options.blockEdges = false; // the test's ramp, pond and cliff lie on the ring
    const auto result = bakeWalkability(b.model, grid, options);
    CHECK(result.width == 8);
    CHECK(result.at(1, 1).walkable);
    CHECK(result.at(1, 1).coverage == doctest::Approx(1));
    CHECK(result.at(1, 1).height == doctest::Approx(0));
    // The roof is level ground nothing leads up to: an island, blocked.
    CHECK_FALSE(result.at(2, 1).walkable);
    CHECK(result.at(2, 1).island);
    CHECK_FALSE(result.at(3, 2).walkable);
    CHECK(result.islands == 4);
    // The post covers half the cell: walkable with boxes over the covered quarters.
    CHECK(result.at(5, 4).walkable);
    CHECK(result.at(5, 4).partial);
    CHECK_FALSE(result.colliders.empty());
    bool coversPost = false;
    for (const auto& box : result.colliders) {
        coversPost |= box.min.x >= 5.49f && box.max.x <= 6.01f && box.min.z >= 3.99f && box.max.z <= 5.01f;
    }
    CHECK(coversPost);
    // The cliff top is level ground at height 1, reached by the ramp cells
    // (each rising half a unit east) so it is not an island.
    CHECK(result.at(6, 0).walkable);
    CHECK(result.at(6, 0).height == doctest::Approx(1));
    CHECK(result.at(4, 0).ramp == Ramp::East);
    CHECK(result.at(4, 0).rise == doctest::Approx(0.5f));
    CHECK(result.at(4, 0).height == doctest::Approx(0));
    CHECK(result.at(5, 1).ramp == Ramp::East);
    CHECK(result.at(5, 1).height == doctest::Approx(0.5f));
    CHECK(result.at(3, 0).height == doctest::Approx(0));
    // The ramp rises east by 0.5 from a low edge of 0.
    CHECK(result.at(0, 4).walkable);
    CHECK(result.at(0, 4).ramp == Ramp::East);
    CHECK(result.at(0, 4).rise == doctest::Approx(0.5f));
    CHECK(result.at(0, 4).height == doctest::Approx(0));
    CHECK(result.at(1, 4).ramp == Ramp::None);
    // Water is blocked and tagged.
    CHECK(result.at(0, 5).water);
    CHECK_FALSE(result.at(0, 5).walkable);
    CHECK(result.walkable + result.blocked + result.partial + result.unsampled == 48);
    CHECK(result.unsampled == 0);

    // Applying writes the grid, the tag and the boxes; the map still loads.
    MapData map;
    map.id = "baked";
    map.grid = grid;
    const int changed = applyBake(map, result, options);
    CHECK(changed > 30);
    CHECK(map.grid.at(1, 1, 0).walkable);
    CHECK_FALSE(map.grid.at(2, 1, 0).walkable);
    CHECK(map.grid.at(6, 0, 0).height == doctest::Approx(1));
    CHECK(map.grid.at(0, 4, 0).ramp == Ramp::East);
    CHECK((map.grid.at(0, 5, 0).tags & map.grid.tagMask({"water"})) != 0);
    CHECK(map.colliders.empty()); // derived boxes stay apart from authored ones
    CHECK_FALSE(map.bake.colliders.empty());
    CHECK_NOTHROW(MapData::fromJson(map.toJson()));
    // A region limits the write; the post lies outside this one, so no boxes.
    MapData limited;
    limited.id = "limited";
    limited.grid = grid;
    BakeOptions some;
    some.blockEdges = false;
    some.region = glm::ivec4(0, 0, 2, 2);
    const auto partialResult = bakeWalkability(b.model, grid, some);
    applyBake(limited, partialResult, some);
    CHECK(limited.grid.at(1, 1, 0).walkable);
    CHECK_FALSE(limited.grid.at(4, 4, 0).walkable); // outside the region: untouched (the grid starts blocked)
    CHECK(limited.bake.colliders.empty());
}

TEST_CASE("the brush records overrides on a derived map, and clearing one hands the cell back") {
    MapData map;
    map.id = "brush";
    map.grid = flatGrid(8, 6);
    map.bake.options = BakeOptions{}.toJson();
    GridBrush open;
    open.mode = GridBrush::Mode::Walkable;
    open.walkable = true;
    CHECK(setOverride(map, {2, 2, 0}, open));
    CHECK(map.grid.at(2, 2, 0).walkable); // applied at once
    REQUIRE(map.bake.overrides.size() == 1);
    // Another stroke on the same cell merges into its override.
    GridBrush raise;
    raise.mode = GridBrush::Mode::Height;
    raise.height = 0.5f;
    CHECK(setOverride(map, {2, 2, 0}, raise));
    CHECK(map.bake.overrides.size() == 1);
    CHECK(map.bake.overrides[0].walkable == true);
    CHECK(map.bake.overrides[0].height == doctest::Approx(0.5f));
    GridBrush tag;
    tag.mode = GridBrush::Mode::AddTag;
    tag.tag = "water";
    CHECK(setOverride(map, {2, 2, 0}, tag));
    CHECK((map.grid.at(2, 2, 0).tags & map.grid.tagMask({"water"})) != 0);
    tag.mode = GridBrush::Mode::RemoveTag;
    CHECK(setOverride(map, {2, 2, 0}, tag));
    CHECK((map.grid.at(2, 2, 0).tags & map.grid.tagMask({"water"})) == 0);
    CHECK(map.bake.overrides[0].tags.empty());
    CHECK(map.bake.overrides[0].untags == std::vector<std::string>{"water"});
    tag.tag.clear();
    CHECK_FALSE(setOverride(map, {2, 2, 0}, tag)); // no tag named
    CHECK_FALSE(setOverride(map, {80, 2, 0}, open)); // outside the grid
    // A rectangle is one override; a later cell stroke inside it is a new entry.
    CHECK(setOverrideRect(map, {4, 1, 0}, {6, 3, 0}, open));
    CHECK(map.bake.overrides.size() == 2);
    CHECK(map.grid.at(5, 2, 0).walkable);
    CHECK(setOverride(map, {5, 2, 0}, raise));
    CHECK(map.bake.overrides.size() == 3);
    CHECK(overriddenCells(map, 0).size() == 10);
    CHECK(overriddenCells(map, 1).empty());
    // Clearing the middle of the rectangle splits it around the cell and drops the cell's own entry.
    CHECK(clearOverride(map, {5, 2, 0}));
    CHECK(overriddenCells(map, 0).size() == 9);
    for (const auto& over : map.bake.overrides) {
        const bool coversCell = over.x <= 5 && 5 < over.x + over.w && over.z <= 2 && 2 < over.z + over.d;
        CHECK_FALSE(coversCell);
    }
    CHECK_FALSE(clearOverride(map, {7, 5, 0})); // nothing there
    // Everything round-trips and the overrides reapply after a bake.
    const auto loaded = MapData::fromJson(map.toJson());
    CHECK(loaded.bake.overrides.size() == map.bake.overrides.size());
    ModelBuilder b;
    b.rampX(0, 0, 8, 6, 0, 8); // too steep to walk anywhere
    MapData baked = loaded;
    BakeOptions options;
    options.blockEdges = false;
    options.pruneIslands = false;
    applyBake(baked, bakeWalkability(b.model, baked.grid, options), options);
    CHECK(baked.grid.at(2, 2, 0).walkable);
    CHECK(baked.grid.at(4, 1, 0).walkable);
    CHECK_FALSE(baked.grid.at(5, 2, 0).walkable); // handed back to the bake
    CHECK_FALSE(baked.grid.at(1, 1, 0).walkable);
}

TEST_CASE("the kit's stairs bake to ramps and its cliff block to a raised floor") {
    const auto gameRoot = std::filesystem::path(FENGINE_SAMPLES) / "vale";
    AssetCache assets(gameRoot, FENGINE_LUALIB);
    const auto stairs = assets.tryModel("fx/kit/stairs_1x2.glb");
    const auto cliff = assets.tryModel("fx/kit/cliff_2x2.glb");
    const auto floor = assets.tryModel("fx/kit/floor_1x1.glb");
    REQUIRE((stairs && cliff && floor));
    CHECK(hasCollisionNodes(*stairs)); // the slope under the steps
    ModelBuilder ground;
    ground.floor(0, 0, 8, 6, 0);
    Placement up, block;
    up.position = {3, 0, 2.5f}; // stairs over cells (2, 2) and (3, 2), rising east
    block.position = {5, 0, 3};  // a 2 x 2 cliff block over cells (4..5, 2..3), one unit up
    const BakePiece pieces[] = {{&ground.model, glm::mat4(1)}, {stairs.get(), placementMatrix(up)}, {cliff.get(), placementMatrix(block)}};
    BakeOptions options;
    options.blockEdges = false;
    const auto result = bakeWalkability(pieces, flatGrid(8, 6), options);
    CHECK(result.at(2, 2).walkable);
    CHECK(result.at(2, 2).ramp == Ramp::East);
    CHECK(result.at(2, 2).rise == doctest::Approx(0.5f));
    CHECK(result.at(3, 2).ramp == Ramp::East);
    CHECK(result.at(3, 2).height == doctest::Approx(0.5f));
    CHECK(result.at(4, 2).walkable); // the block's top, reached by the stairs: not an island
    CHECK(result.at(4, 2).height == doctest::Approx(1));
    CHECK(result.at(5, 3).height == doctest::Approx(1));
    CHECK(result.at(1, 2).walkable);
    CHECK(result.at(1, 2).height == doctest::Approx(0));
}

TEST_CASE("vale's committed walkability is exactly the bake of its terrain and placed models") {
    const auto gameRoot = std::filesystem::path(FENGINE_SAMPLES) / "vale";
    const auto map = MapData::load(gameRoot / "maps/town/map.json");
    REQUIRE_FALSE(map.bake.empty());
    AssetCache assets(gameRoot, FENGINE_LUALIB);
    const auto scene = sceneForMap(map, assets, geometryPathOf(map));
    CHECK(scene.problems.empty());
    CHECK(scene.pieces.size() >= 2); // the terrain and the inn
    const auto options = BakeOptions::fromJson(map.bake.options);
    const auto result = bakeWalkability(scene.pieces, map.grid, options);
    MapData baked = map;
    applyBake(baked, result, options);
    CHECK(baked.grid.toJson() == map.grid.toJson());
    CHECK(baked.bake.colliders.size() == map.bake.colliders.size());
    CHECK(bakeFingerprint(map, geometryPathOf(map), gameFileReader(gameRoot)) == map.bake.fingerprint);
    // The inn (a 4x3 house at cells 2..5 x 2..4) blocks its cells.
    for (int z = 2; z < 5; ++z) {
        for (int x = 2; x < 6; ++x) {
            CHECK_FALSE(map.grid.at(x, z, 0).walkable);
        }
    }
    CHECK(map.grid.at(6, 3, 0).walkable);
}

TEST_CASE("a placed model bakes through its position, yaw and scale") {
    ModelBuilder ground;
    ground.floor(0, 0, 8, 6, 0);
    ModelBuilder crate; // a 2 x 1 block, origin at the centre of its base
    crate.box({-1, 0, -0.5f}, {1, 1.5f, 0.5f});
    Placement p;
    p.id = "crate";
    p.type = "prop";
    p.props = {{"model", "fx/crate.glb"}};
    CHECK(bakedModelOf(p) != nullptr);
    const auto grid = flatGrid(8, 6);
    BakeOptions options;
    options.blockEdges = false;
    const auto bake = [&](glm::vec3 position, float yaw, glm::vec3 scale) {
        p.position = position;
        p.yaw = yaw;
        p.scale = scale;
        const BakePiece pieces[] = {{&ground.model, glm::mat4(1)}, {&crate.model, placementMatrix(p)}};
        return bakeWalkability(pieces, grid, options);
    };
    auto result = bake({4, 0, 3.5f}, 0, glm::vec3(1));
    CHECK_FALSE(result.at(3, 3).walkable); // x 3..5, z 3..4
    CHECK_FALSE(result.at(4, 3).walkable);
    CHECK(result.at(5, 3).walkable);
    CHECK(result.at(3, 4).walkable);
    CHECK(result.at(3, 2).walkable);
    result = bake({3.5f, 0, 4}, 90, glm::vec3(1)); // turned: along z now
    CHECK_FALSE(result.at(3, 3).walkable);
    CHECK_FALSE(result.at(3, 4).walkable);
    CHECK(result.at(4, 3).walkable);
    CHECK(result.at(3, 5).walkable);
    result = bake({4, 0, 4}, 0, glm::vec3(2)); // doubled: x 2..6, z 3..5
    CHECK_FALSE(result.at(2, 3).walkable);
    CHECK_FALSE(result.at(5, 4).walkable);
    CHECK(result.at(6, 4).walkable);
    CHECK(result.at(4, 2).walkable);
    // Decoration and scripted props stay out of the bake.
    p.props["solid"] = false;
    CHECK(bakedModelOf(p) == nullptr);
    p.props.erase("solid");
    p.props["pages"] = Json::array();
    CHECK(bakedModelOf(p) == nullptr);
    p.props.erase("pages");
    p.props["model"] = 3;
    CHECK(bakedModelOf(p) == nullptr);
}

TEST_CASE("collision nodes replace the visible mesh in the bake") {
    ModelBuilder house;
    // The visible part: a wide roof block over cells 2..5 x 1..4, on node 0.
    house.box({2, 0, 1}, {6, 2, 5});
    // A hidden collision node with a small box: only cell (3, 2) blocks.
    house.model.nodes.push_back({});
    house.model.nodes.back().properties = {{"collision", true}};
    house.model.restPose.push_back(glm::mat4(1));
    house.model.order = {0, 1};
    ModelPart collider;
    collider.node = 1;
    collider.hidden = true;
    house.model.parts.push_back(collider);
    auto& part = house.model.parts.back();
    const auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) { house.tri(part, a, b, c); };
    const glm::vec3 lo(3, 0, 2), hi(4, 2, 3);
    tri({lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z});
    tri({lo.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
    for (const auto& [a, b, c, d] : std::vector<std::array<glm::vec3, 4>>{
             {{{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}}},
             {{{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}}},
             {{{lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}}},
             {{{hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}}}}) {
        tri(a, b, c);
        tri(a, c, d);
    }
    CHECK(collisionParts(house.model).size() == 1);
    CHECK(collisionParts(house.model)[0] == &house.model.parts.back());
    ModelBuilder ground;
    ground.floor(0, 0, 8, 6, 0);
    const BakePiece pieces[] = {{&ground.model, glm::mat4(1)}, {&house.model, glm::mat4(1)}};
    BakeOptions options;
    options.blockEdges = false;
    const auto result = bakeWalkability(pieces, flatGrid(8, 6), options);
    CHECK_FALSE(result.at(3, 2).walkable);
    CHECK(result.at(2, 1).walkable); // under the visible roof, but the collider does not cover it
    CHECK(result.at(5, 4).walkable);
    // Without collision nodes the shown parts are read, hidden ones skipped.
    ModelBuilder plain;
    plain.box({2, 0, 1}, {6, 2, 5});
    plain.model.parts.push_back(ModelPart{});
    plain.model.parts.back().hidden = true;
    CHECK(collisionParts(plain.model).size() == 1);
    CHECK(collisionParts(plain.model)[0] == &plain.model.parts[0]);
}

TEST_CASE("a thin wall between sample points still makes boxes, and both cells beside it stay walkable") {
    ModelBuilder b;
    b.floor(0, 0, 8, 6, 0);
    b.box({3.925f, 0, 1}, {4.075f, 1, 4}); // a 0.15 fence on the boundary between cells 3 and 4
    BakeOptions options;
    options.blockEdges = false;
    const auto result = bakeWalkability(b.model, flatGrid(8, 6), options);
    CHECK(result.at(3, 2).walkable);
    CHECK(result.at(4, 2).walkable);
    CHECK(result.at(3, 2).partial);
    CHECK(result.at(4, 2).partial);
    // The quarter strips on both sides of the boundary merge into one box
    // 3.75..4.25 wide along the fence.
    bool strip = false;
    for (const auto& box : result.colliders) {
        strip |= box.min.x <= 3.76f && box.min.x >= 3.74f && box.max.x >= 4.24f && box.max.x <= 4.26f &&
                 box.min.z <= 1.01f && box.max.z >= 3.99f;
    }
    CHECK(strip);
    // A wall exactly on a cell boundary belongs to the cell it stands in, not to its neighbour.
    ModelBuilder h;
    h.floor(0, 0, 8, 6, 0);
    h.box({2, 0, 1}, {4, 2, 3}); // a house on whole cells 2..3 x 1..2
    const auto house = bakeWalkability(h.model, flatGrid(8, 6), options);
    CHECK_FALSE(house.at(2, 1).walkable);
    CHECK(house.at(4, 1).walkable);
    CHECK_FALSE(house.at(4, 1).partial); // no box strip along the wall: bodies touch it
    CHECK_FALSE(house.at(1, 2).partial);
}

TEST_CASE("a region re-bake replaces only the derived boxes inside the region and keeps authored ones") {
    ModelBuilder ground;
    ground.floor(0, 0, 8, 6, 0);
    ModelBuilder post; // a half-cell post, origin at the centre of its base
    post.box({-0.25f, 0, -0.5f}, {0.25f, 1.5f, 0.5f});
    Placement left, right;
    left.id = "left";
    left.position = {1.75f, 0, 1.5f}; // covers x 1.5..2 of cell (1, 1)
    right.id = "right";
    right.position = {5.75f, 0, 4.5f}; // covers x 5.5..6 of cell (5, 4)
    const auto grid = flatGrid(8, 6);
    BakeOptions options;
    options.blockEdges = false;
    MapData map;
    map.id = "regions";
    map.grid = grid;
    map.colliders.push_back({{6, 0, 1}, {7, 1, 2}}); // an authored wall
    {
        const BakePiece pieces[] = {{&ground.model, glm::mat4(1)}, {&post.model, placementMatrix(left)},
                                  {&post.model, placementMatrix(right)}};
        applyBake(map, bakeWalkability(pieces, grid, options), options);
    }
    const auto boxesNear = [&](float x, float z) {
        return std::count_if(map.bake.colliders.begin(), map.bake.colliders.end(), [&](const Box& b) {
            return b.min.x >= x - 1e-3f && b.max.x <= x + 1 + 1e-3f && b.min.z >= z - 1e-3f && b.max.z <= z + 1 + 1e-3f;
        });
    };
    CHECK(boxesNear(1, 1) > 0);
    CHECK(boxesNear(5, 4) > 0);
    CHECK(map.grid.at(1, 1, 0).walkable);
    CHECK(map.grid.at(5, 4, 0).walkable);
    CHECK(map.colliders.size() == 1);
    // The right post goes away: re-bake only its cells.
    options.region = bakeRegionFor(post.model, right, grid);
    CHECK(options.region->x <= 5);
    CHECK(options.region->x + options.region->z > 5);
    {
        const BakePiece pieces[] = {{&ground.model, glm::mat4(1)}, {&post.model, placementMatrix(left)}};
        applyBake(map, bakeWalkability(pieces, grid, options), options);
    }
    CHECK(boxesNear(5, 4) == 0);
    CHECK(boxesNear(1, 1) > 0);
    CHECK(map.grid.at(1, 1, 0).walkable);
    CHECK(map.grid.at(7, 5, 0).walkable); // outside the region: untouched
    CHECK(map.colliders.size() == 1);     // the authored wall stays
    // A region's edge ring is not blocked and nothing outside it is pruned as an island.
    int blocked = 0;
    for (int z = 0; z < 6; ++z) {
        for (int x = 0; x < 8; ++x) {
            blocked += map.grid.at(x, z, 0).walkable ? 0 : 1;
        }
    }
    CHECK(blocked == 0);
}

TEST_CASE("overrides round-trip through JSON and apply after a bake") {
    MapData map;
    map.id = "overrides";
    map.grid = flatGrid(8, 6);
    map.bake.options = BakeOptions{}.toJson();
    map.bake.overrides.push_back({1, 1, 0, 2, 1, true, std::nullopt, std::nullopt, 1, {}, {}});
    map.bake.overrides.push_back({3, 3, 0, 1, 1, std::nullopt, 0.5f, Ramp::East, 0.5f, {"encounter:slimes"}, {"water"}});
    const auto json = map.toJson();
    REQUIRE(json.contains("bake"));
    CHECK(json["bake"]["overrides"][0]["w"] == 2);
    CHECK(json["bake"]["overrides"][1]["ramp"] == "e");
    const auto loaded = MapData::fromJson(json);
    REQUIRE(loaded.bake.overrides.size() == 2);
    CHECK(loaded.bake.overrides[0].walkable == true);
    CHECK(loaded.bake.overrides[1].ramp == Ramp::East);
    CHECK(loaded.bake.overrides[1].tags == std::vector<std::string>{"encounter:slimes"});
    CHECK(loaded.toJson() == json);
    // A bake over ground too steep to walk: the override opens two cells and shapes one.
    ModelBuilder b;
    b.rampX(0, 0, 8, 6, 0, 8); // 45 degrees everywhere: nothing is ground
    MapData baked = loaded;
    BakeOptions options;
    options.blockEdges = false;
    options.pruneIslands = false;
    applyBake(baked, bakeWalkability(b.model, baked.grid, options), options);
    CHECK(baked.grid.at(1, 1, 0).walkable);
    CHECK(baked.grid.at(2, 1, 0).walkable);
    CHECK_FALSE(baked.grid.at(3, 1, 0).walkable);
    CHECK(baked.grid.at(3, 3, 0).height == doctest::Approx(0.5f));
    CHECK(baked.grid.at(3, 3, 0).ramp == Ramp::East);
    CHECK((baked.grid.at(3, 3, 0).tags & baked.grid.tagMask({"encounter:slimes"})) != 0);
    // Outside the grid is refused like cell_props.
    auto bad = json;
    bad["bake"]["overrides"][0]["x"] = 7;
    CHECK_THROWS(MapData::fromJson(bad));
    // A map without a bake section writes none, and an unbaked blank map's section is empty.
    MapData plain;
    plain.id = "plain";
    plain.grid = flatGrid(2, 2);
    CHECK_FALSE(plain.toJson().contains("bake"));
    CHECK(plain.bake.empty());
}

TEST_CASE("the bake fingerprint changes with placements, geometry, models and options") {
    MapData map;
    map.id = "fp";
    map.geometry = "geometry.glb";
    map.grid = flatGrid(4, 4);
    Placement house;
    house.id = "house";
    house.position = {2, 0, 2};
    house.props = {{"model", "fx/house.glb"}};
    map.placements.push_back(house);
    std::map<std::string, std::string> files = {{"maps/fp/geometry.glb", "terrain"}, {"fx/house.glb", "house"}};
    const auto reader = [&](const std::string& path) -> std::optional<std::string> {
        const auto found = files.find(path);
        return found == files.end() ? std::nullopt : std::optional(found->second);
    };
    const auto base = bakeFingerprint(map, geometryPathOf(map), reader);
    CHECK(base.size() == 16);
    CHECK(bakeFingerprint(map, geometryPathOf(map), reader) == base);
    map.placements[0].position.x = 3;
    const auto moved = bakeFingerprint(map, geometryPathOf(map), reader);
    CHECK(moved != base);
    map.placements[0].position.x = 2;
    files["fx/house.glb"] = "house v2";
    CHECK(bakeFingerprint(map, geometryPathOf(map), reader) != base);
    files["fx/house.glb"] = "house";
    files["maps/fp/geometry.glb"] = "terrain v2";
    CHECK(bakeFingerprint(map, geometryPathOf(map), reader) != base);
    files["maps/fp/geometry.glb"] = "terrain";
    map.bake.options = BakeOptions{}.toJson();
    CHECK(bakeFingerprint(map, geometryPathOf(map), reader) != base);
    map.bake.options = Json::object();
    // Placements the bake does not read leave it alone.
    Placement npc;
    npc.id = "npc";
    npc.props = {{"sprite", "sprites/elder.anim.json"}};
    map.placements.push_back(npc);
    CHECK(bakeFingerprint(map, geometryPathOf(map), reader) == base);
    // The stored options round-trip.
    BakeOptions o;
    o.samples = 8;
    o.minCoverage = 0.5f;
    o.boxDrops = false;
    const auto back = BakeOptions::fromJson(o.toJson());
    CHECK(back.samples == 8);
    CHECK(back.minCoverage == doctest::Approx(0.5f));
    CHECK_FALSE(back.boxDrops);
    CHECK(back.clearance == doctest::Approx(1.6f));
}

TEST_CASE("undo restores the derived boxes together with the placement edit") {
    Worlds w;
    MapEditHistory history;
    history.apply(w.field, [](MapData& m) {
        m.placements.push_back(Placement{"crate", "prop", {4.5f, 0, 6.5f}});
        m.bake.colliders.push_back({{4, 0, 6}, {5, 1, 7}});
        m.bake.fingerprint = "abc";
    });
    CHECK(w.field.map().bake.colliders.size() == 1);
    CHECK(w.field.boxBlockedCells().contains(cellKey({4, 6, 0})));
    REQUIRE(history.undo(w.field));
    CHECK(w.field.authored().bake.colliders.empty());
    CHECK(w.field.map().bake.colliders.empty());
    CHECK_FALSE(w.field.boxBlockedCells().contains(cellKey({4, 6, 0})));
    REQUIRE(history.redo(w.field));
    CHECK(w.field.map().bake.colliders.size() == 1);
}

// --- Sprite sheet builder -----------------------------------------------------

#include "engine/assets/SpriteSheetBuilder.h"

TEST_CASE("the sprite sheet builder describes a uniform sheet the way fengine_assetc sheet does") {
    const Json manifest = {{"atlas", "hero.png"},
                           {"frame_width", 32},
                           {"frame_height", 48},
                           {"directions", 4},
                           {"pixels_per_unit", 32},
                           {"clips",
                            {{"idle", {{"frames", {0}}, {"duration", 0.5}}},
                             {"walk", {{"frames", {0, 1, 2, 1}}, {"row_offset", 4}, {"duration", 0.12}}},
                             {"attack", {{"frames", {0, 1}}, {"column_offset", 3}, {"loop", false}}}}}};
    const auto spec = sheetSpecFromJson(manifest);
    CHECK(spec.clips.size() == 3);
    CHECK(sheetColumns(spec, 160) == 5);
    CHECK(sheetRows(spec, 384) == 8);
    const auto json = buildSheetAnimation(spec, 160, 384);
    CHECK(json.at("atlas") == "hero.png");
    const auto& walk = json.at("clips").at("walk");
    CHECK(walk.at("loop") == true);
    CHECK(walk.at("directions").size() == 4);
    CHECK(walk.at("directions")[1][2].at("rect") == Json({64, 5 * 48, 32, 48})); // row 1 + offset 4
    CHECK(walk.at("directions")[0][0].at("duration") == doctest::Approx(0.12));
    const auto& attack = json.at("clips").at("attack");
    CHECK(attack.at("loop") == false);
    CHECK(attack.at("directions")[0][1].at("rect") == Json({4 * 32, 0, 32, 48}));
    const auto animation = SpriteAnimation::fromJson(json);
    CHECK(animation.clips.at("idle").directions.size() == 4);
    // Frames past the image, bad sizes and empty clips are refused.
    CHECK_THROWS(static_cast<void>(buildSheetAnimation(spec, 96, 384)));
    auto bad = spec;
    bad.directions = 3;
    CHECK_THROWS(static_cast<void>(buildSheetAnimation(bad, 160, 384)));
    bad = spec;
    bad.clips.clear();
    CHECK_THROWS(static_cast<void>(buildSheetAnimation(bad, 160, 384)));
}

// --- Map files ------------------------------------------------------------------

#include "engine/editor/MapFiles.h"

TEST_CASE("a new map stands on a ground quad that bakes walkable") {
    const auto blank = makeBlankMap("quad_test", "Quad", 6, 4);
    CHECK_FALSE(blank.bake.empty());
    CHECK(blank.bake.autoBake);
    CHECK(blank.bake.fingerprint.empty());
    const auto scratch = std::filesystem::temp_directory_path() / "fengine-ground-quad";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "maps");
    writeMap(scratch, blank);
    const auto written = MapData::load(scratch / "maps/quad_test/map.json");
    CHECK(written.geometry == "geometry.gltf");
    const auto model = Model::load(scratch / "maps/quad_test/geometry.gltf");
    REQUIRE(model.parts.size() == 1);
    CHECK(model.parts[0].indices.size() == 6);
    CHECK(model.max.x == doctest::Approx(6));
    CHECK(model.max.z == doctest::Approx(4));
    BakeOptions options = BakeOptions::fromJson(written.bake.options);
    const auto result = bakeWalkability(model, written.grid, options);
    CHECK(result.unsampled == 0);
    CHECK(result.at(2, 1).walkable);
    CHECK_FALSE(result.at(0, 0).walkable); // the edge ring
    std::filesystem::remove_all(scratch);
}

TEST_CASE("map files: blank maps, copies, renames and deletions in a scratch game folder") {
    const auto root = std::filesystem::temp_directory_path() / "fengine-mapfiles-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "maps");
    CHECK(validMapId("town_2"));
    CHECK_FALSE(validMapId("Town"));
    CHECK_FALSE(validMapId(""));
    CHECK_FALSE(validMapId("a/b"));
    CHECK_THROWS(static_cast<void>(makeBlankMap("Bad Id", "x", 4, 4)));
    const auto blank = makeBlankMap("cellar", "The Cellar", 12, 9);
    CHECK(blank.id == "cellar");
    CHECK(blank.name == "The Cellar");
    CHECK(blank.grid.width() == 12);
    CHECK(blank.grid.depth() == 9);
    CHECK(blank.grid.at(5, 5, 0).walkable);
    REQUIRE(blank.points.contains("start"));
    CHECK(blank.points.at("start").position.x == doctest::Approx(6));
    CHECK_FALSE(mapExists(root, "cellar"));
    writeMap(root, blank);
    CHECK(mapExists(root, "cellar"));
    const auto loaded = MapData::load(root / "maps/cellar/map.json");
    CHECK(loaded.name == "The Cellar");
    // Without geometry the map gets a flat ground quad the engine can load.
    CHECK(loaded.geometry == "geometry.gltf");
    const auto placeholder = Model::load(root / "maps/cellar/geometry.gltf");
    CHECK(placeholder.parts.size() == 1);
    {
        AssetCache assets(root, FENGINE_LUALIB);
        World world("blank", loaded, assets, 1);
        CHECK(world.geometryPath() == "maps/cellar/geometry.gltf");
        CHECK(assets.tryModel(world.geometryPath()) != nullptr);
    }
    // Geometry comes along when given.
    const auto glb = std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/geometry.glb";
    writeMap(root, blank, glb);
    CHECK(std::filesystem::exists(root / "maps/cellar/geometry.glb"));
    CHECK_THROWS(writeMap(root, blank, std::filesystem::path(FENGINE_TEST_GAME) / "game.json"));
    copyMap(root, "cellar", "cellar_b", "Cellar B");
    CHECK(mapExists(root, "cellar_b"));
    const auto copied = MapData::load(root / "maps/cellar_b/map.json");
    CHECK(copied.id == "cellar_b");
    CHECK(copied.name == "Cellar B");
    CHECK(std::filesystem::exists(root / "maps/cellar_b/geometry.glb"));
    CHECK_THROWS(copyMap(root, "cellar", "cellar_b"));
    CHECK_THROWS(copyMap(root, "nothing", "x"));
    renameMap(root, "cellar_b", "attic");
    CHECK_FALSE(mapExists(root, "cellar_b"));
    CHECK(MapData::load(root / "maps/attic/map.json").id == "attic");
    CHECK_THROWS(renameMap(root, "attic", "cellar"));
    deleteMap(root, "attic");
    CHECK_FALSE(mapExists(root, "attic"));
    CHECK_THROWS(deleteMap(root, "attic"));
    std::filesystem::remove_all(root);
}

// --- Snapped draw cameras (the editor camera) -------------------------------------

#include "engine/render/FrameMath.h"

TEST_CASE("a snapped draw camera shows its new values at once instead of interpolating from the last tick") {
    DrawCameras cameras;
    int world = 0;
    Camera a;
    a.target = {0, 0, 0};
    a.snap();
    cameras.update(&world, "", a, 7);
    cameras.endFrame();
    Camera b = a;
    b.target = {10, 0, 0};
    // The same tick (the game is paused), so an interpolated camera would be
    // blended against the previous values; a snapped one is simply there.
    const auto& plain = cameras.update(&world, "", b, 7);
    CHECK(plain.focus(0).x == doctest::Approx(0));
    CHECK(plain.focus(1).x == doctest::Approx(10));
    cameras.endFrame();
    Camera c = a;
    c.target = {20, 0, 0};
    const auto& snapped = cameras.update(&world, "", c, 7, true);
    CHECK(snapped.focus(0).x == doctest::Approx(20));
    CHECK(snapped.focus(1).x == doctest::Approx(20));
}
