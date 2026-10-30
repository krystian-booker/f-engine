#include "engine/assets/Model.h"
#include "engine/editor/MapEdits.h"
#include "engine/world/Components.h"
#include "engine/world/World.h"
#include <doctest/doctest.h>

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

TEST_CASE("marker import skips markers that don't fit instead of throwing") {
    auto map = arena();
    Model model;
    ModelMarker bad;
    bad.name = "deck";
    bad.hasBounds = true;
    bad.max = {2, 1, 2};
    bad.properties = {{"collision", {{"layer", 9}}}};
    ModelMarker outside;
    outside.name = "far";
    outside.position = {500, 0, 500};
    outside.properties = {{"spawn", "far"}};
    ModelMarker wall;
    wall.name = "wall";
    wall.hasBounds = true;
    wall.max = {1, 2, 1};
    wall.properties = {{"collision", true}};
    model.markers = {bad, outside, wall};
    std::vector<std::string> problems;
    CHECK(importMarkers(map, model, problems) == 1);
    CHECK(problems.size() == 2);
    CHECK_FALSE(map.grid.at(0, 0).walkable);
    CHECK_FALSE(map.points.contains("far"));
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
