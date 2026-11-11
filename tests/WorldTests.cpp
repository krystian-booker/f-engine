#include "engine/editor/MapEdits.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/Animation.h"
#include "engine/world/World.h"
#include <cmath>
#include <doctest/doctest.h>
#include <fstream>
#include <limits>

using namespace fe;

namespace {
void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

std::size_t count(const std::vector<WorldEvent>& events, WorldEvent::Kind kind) {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [&](const WorldEvent& e) { return e.kind == kind; }));
}

// A world built from inline map JSON inside a scratch game folder.
struct Scratch {
    std::filesystem::path root;
    AssetCache assets;
    World world;
    Scratch(const std::string& name, const Json& grid, const std::map<std::string, std::string>& files = {})
        : root(prepare(name, files)), assets(root, FENGINE_BUILTIN),
          world("scratch", MapData::fromJson({{"format", 2}, {"id", "scratch"}, {"grid", grid}}), assets, 3) {
    }
    static std::filesystem::path prepare(const std::string& name,
                                         const std::map<std::string, std::string>& files) {
        const auto root = std::filesystem::temp_directory_path() / ("fengine-world-" + name);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        for (const auto& [path, text] : files) {
            write(root / path, text);
        }
        return root;
    }
    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.beginTick();
            world.tick(1.0 / 60);
        }
    }
};
struct Fixture {
    AssetCache assets{FENGINE_TEST_GAME, FENGINE_BUILTIN};
    World world{"test", MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json"),
                assets, 7};

    entt::entity walker(glm::vec3 position) {
        const auto e = world.spawn("", position);
        world.registry().emplace<Mover>(e);
        world.registry().emplace<Collider>(e);
        return e;
    }
    void run(double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60); ++i) {
            world.beginTick();
            world.tick(1.0 / 60);
        }
    }
    bool has(const std::vector<WorldEvent>& events, WorldEvent::Kind kind) {
        return std::any_of(events.begin(), events.end(), [&](const WorldEvent& e) { return e.kind == kind; });
    }
};
} // namespace

TEST_CASE("map format 2 loads points, grid layers and links") {
    Fixture f;
    CHECK(f.world.map().points.contains("start"));
    CHECK(f.world.grid().layerCount() == 2);
    CHECK(f.world.grid().linkAt(6, 7, 0) != nullptr);
    const auto copy = MapData::fromJson(f.world.map().toJson());
    CHECK(copy.grid.width() == 12);
    CHECK(copy.lights.size() == 1);
}

TEST_CASE("movers path around walls and report arrival") {
    Fixture f;
    const auto e = f.walker({2.5f, 0, 3.5f});
    f.world.moveTo(e, {7.5f, 0, 3.5f}, true);
    f.run(8);
    const auto p = f.world.registry().get<Transform>(e).position;
    CHECK(p.x == doctest::Approx(7.5f).epsilon(0.02));
    CHECK(p.z == doctest::Approx(3.5f).epsilon(0.02));
    CHECK(f.has(f.world.takeEvents(), WorldEvent::Kind::Arrived));
}

TEST_CASE("driving into a wall stops at it and ramps raise the walker") {
    Fixture f;
    const auto e = f.walker({4.5f, 0, 2.5f});
    for (int i = 0; i < 60; ++i) {
        f.world.beginTick();
        f.world.registry().get<Mover>(e).drive = {2, 0};
        f.world.tick(1.0 / 60);
    }
    CHECK(f.world.registry().get<Transform>(e).position.x < 5.0f);
    f.world.place(e, {6.5f, 0, 3.0f}, 0);
    for (int i = 0; i < 60; ++i) {
        f.world.beginTick();
        f.world.registry().get<Mover>(e).drive = {2, 0};
        f.world.tick(1.0 / 60);
    }
    CHECK(f.world.registry().get<Transform>(e).position.y == doctest::Approx(1).epsilon(0.01));
}

TEST_CASE("walking east at a directional link climbs onto the bridge layer") {
    Fixture f;
    const auto e = f.walker({5.6f, 0, 7.5f});
    for (int i = 0; i < 70; ++i) {
        f.world.beginTick();
        f.world.registry().get<Mover>(e).drive = {2, 0};
        f.world.tick(1.0 / 60);
    }
    const auto& t = f.world.registry().get<Transform>(e);
    CHECK(t.layer == 1);
    CHECK(t.position.y > 0.9f);
}

TEST_CASE("jumps land on the target and emit an event") {
    Fixture f;
    const auto e = f.walker({2.5f, 0, 2.5f});
    f.world.jump(e, {3.5f, 0, 2.5f}, 1, 0.4f, 0);
    f.run(0.6);
    CHECK(f.world.registry().get<Transform>(e).position.x == doctest::Approx(3.5f));
    CHECK(f.has(f.world.takeEvents(), WorldEvent::Kind::Landed));
}

TEST_CASE("contacts begin and end; zones report entry") {
    Fixture f;
    f.world.map().zones.push_back({"pond", "trigger", {2, 7}, {4, 9}, -1, Json::object()});
    const auto a = f.walker({2.5f, 0, 5.5f});
    const auto b = f.walker({3.5f, 0, 5.5f});
    f.world.registry().get<Collider>(b).solid = false;
    f.run(0.1);
    auto events = f.world.takeEvents();
    CHECK(f.has(events, WorldEvent::Kind::ContactBegin) == false);
    f.world.place(b, {2.8f, 0, 5.5f}, 0);
    f.run(0.1);
    CHECK(f.has(f.world.takeEvents(), WorldEvent::Kind::ContactBegin));
    f.world.place(a, {3.0f, 0, 7.5f}, 0);
    f.run(0.1);
    events = f.world.takeEvents();
    CHECK(f.has(events, WorldEvent::Kind::ContactEnd));
    CHECK(f.has(events, WorldEvent::Kind::ZoneEnter));
}

TEST_CASE("events naming an entity destroyed before they are read are dropped") {
    Fixture f;
    const auto a = f.walker({2.5f, 0, 5.5f});
    const auto b = f.walker({2.8f, 0, 5.5f});
    const auto c = f.walker({3.1f, 0, 5.5f});
    f.world.registry().get<Collider>(b).solid = false;
    f.world.registry().get<Collider>(c).solid = false;
    f.run(0.1);
    // contact_begin for a-b, a-c and b-c are queued; c goes before they are read.
    f.world.destroy(c);
    f.run(0.1);
    const auto events = f.world.takeEvents();
    for (const auto& event : events) {
        CHECK(event.a != c);
        CHECK(event.b != c);
        CHECK(f.world.valid(event.a));
    }
    CHECK(std::count_if(events.begin(), events.end(), [&](const WorldEvent& e) {
              return e.kind == WorldEvent::Kind::ContactBegin && e.a == std::min(a, b) &&
                     e.b == std::max(a, b);
          }) == 1);
    CHECK(f.has(events, WorldEvent::Kind::ContactEnd) == false);
}

TEST_CASE("emitters and weather produce particles; lights flicker deterministically") {
    Fixture f;
    const auto e = f.world.spawn("fire", {6, 0, 5});
    EmitterSettings settings;
    settings.rate = 60;
    f.world.registry().emplace<Emitter>(e, Emitter{settings});
    f.world.environment().weather.kind = "snow";
    f.world.environment().weather.intensity = 1;
    f.run(0.5);
    CHECK(f.world.particles().size() > 30);
    LightSettings light;
    light.flicker = 0.5f;
    f.world.registry().emplace<LightSource>(e, LightSource{light});
    REQUIRE(f.world.activeLights().size() == 2);
    // The flicker wobbles the intensity over time, and the same world time
    // gives the same wobble in another world.
    const float before = f.world.activeLights()[1].intensity;
    f.run(0.25);
    const float after = f.world.activeLights()[1].intensity;
    CHECK(before != doctest::Approx(after));
    CHECK(before != doctest::Approx(light.intensity));
    Fixture g;
    const auto twin = g.world.spawn("fire", {6, 0, 5});
    g.world.registry().emplace<LightSource>(twin, LightSource{light});
    g.run(0.75);
    CHECK(g.world.activeLights()[1].intensity == doctest::Approx(after));
}

TEST_CASE("a light's flicker phase does not depend on the other lights") {
    Fixture f;
    LightSettings light;
    light.flicker = 0.8f;
    const auto first = f.world.spawn("", {2, 0, 2});
    const auto second = f.world.spawn("", {5, 0, 5});
    f.world.registry().emplace<LightSource>(first, LightSource{light});
    f.world.registry().emplace<LightSource>(second, LightSource{light});
    f.run(0.5);
    const auto intensityAt = [](const std::vector<LightSettings>& lights, float x) {
        const auto found = std::find_if(lights.begin(), lights.end(),
                                        [&](const LightSettings& l) { return l.position.x == x; });
        REQUIRE(found != lights.end());
        return found->intensity;
    };
    const auto before = f.world.activeLights();
    REQUIRE(before.size() == f.world.map().lights.size() + 2);
    CHECK(intensityAt(before, 2) != doctest::Approx(intensityAt(before, 5)));
    f.world.registry().remove<LightSource>(first);
    const auto after = f.world.activeLights();
    REQUIRE(after.size() == before.size() - 1);
    CHECK(intensityAt(after, 5) == doctest::Approx(intensityAt(before, 5)));
    for (std::size_t i = 0; i < f.world.map().lights.size(); ++i) {
        CHECK(after[i].intensity == doctest::Approx(before[i].intensity));
    }
    CHECK(flickerSeed(first) != flickerSeed(second));
    CHECK(flickerSeed(LightSettings{}) != flickerSeed(first));
}

TEST_CASE("a map light's flicker phase follows the light, not its place in the map's list") {
    Fixture f;
    auto& lights = f.world.map().lights;
    lights.clear();
    for (int i = 0; i < 3; ++i) {
        LightSettings light;
        light.flicker = 0.8f;
        light.position = {2.0f + 3.0f * static_cast<float>(i), 2, 2};
        lights.push_back(light);
    }
    f.run(0.5);
    const auto before = f.world.activeLights();
    REQUIRE(before.size() == 3);
    CHECK(before[1].intensity != doctest::Approx(before[2].intensity));
    // Removing the first light (as the editor does) keeps the others' phases.
    lights.erase(lights.begin());
    const auto after = f.world.activeLights();
    REQUIRE(after.size() == 2);
    CHECK(after[0].intensity == doctest::Approx(before[1].intensity));
    CHECK(after[1].intensity == doctest::Approx(before[2].intensity));
    CHECK(flickerSeed(lights[0]) >= 0);
    LightSettings negativeZero = lights[0];
    negativeZero.position.z = -0.0f;
    LightSettings zero = lights[0];
    zero.position.z = 0.0f;
    CHECK(flickerSeed(negativeZero) == flickerSeed(zero));
}

TEST_CASE("destroying an entity releases its name and calls the hook") {
    Fixture f;
    int calls = 0;
    f.world.setDestroyHook([&](entt::entity) { ++calls; });
    const auto e = f.world.spawn("npc", {2, 0, 2});
    f.world.registry().emplace<ScriptRef>(e, ScriptRef{5});
    f.world.destroy(e);
    CHECK((f.world.find("npc") == entt::null));
    CHECK(calls == 1);
    CHECK_NOTHROW(f.world.spawn("npc", {2, 0, 2}));
}

namespace {
Json flatGrid(int width, int depth) {
    return {{"width", width}, {"depth", depth}, {"layers", Json::array({Json{{"height_fill", 0}}})}};
}

void drive(Scratch& s, entt::entity e, glm::vec2 velocity, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        s.world.beginTick();
        s.world.registry().get<Mover>(e).drive = velocity;
        s.world.tick(1.0 / 60);
    }
}
} // namespace

TEST_CASE("can_occupy sees colliders added, moved and removed during the script update") {
    Fixture f;
    const auto a = f.walker({4.5f, 0, 3.5f});
    const auto b = f.world.spawn("", {2.5f, 0, 3.5f});
    f.world.registry().emplace<Mover>(b);
    f.run(1.0 / 60);
    CHECK_FALSE(f.world.canOccupy(b, {4.5f, 0, 3.5f}, 0));
    f.world.registry().remove<Collider>(a);
    CHECK(f.world.canOccupy(b, {4.5f, 0, 3.5f}, 0));
    const auto c = f.world.spawn("", {2.5f, 0, 6.5f});
    f.world.registry().emplace<Collider>(c);
    CHECK_FALSE(f.world.canOccupy(b, {2.5f, 0, 6.5f}, 0));
    f.world.place(c, {7.5f, 0, 6.5f}, 0);
    CHECK(f.world.canOccupy(b, {2.5f, 0, 6.5f}, 0));
    CHECK_FALSE(f.world.canOccupy(b, {7.5f, 0, 6.5f}, 0));
    f.world.destroy(c);
    CHECK(f.world.canOccupy(b, {7.5f, 0, 6.5f}, 0));
}

TEST_CASE("can_occupy sees a collider grown during the script update") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = std::filesystem::temp_directory_path() / "fengine-world-collider-grow";
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    lua.console("w = fe.world.load('main', 'arena')");
    lua.console("a = w:spawn{position = {x = 2.5, z = 6.5}, collider = {radius = 0.25}}");
    lua.console("b = w:spawn{position = {x = 8.5, z = 8.5}, mover = {}}");
    engine.tick(1.0 / 60);
    CHECK(lua.console("return b:can_occupy({x = 5.5, z = 6.5})") == "true");
    CHECK(lua.console("a:collider{radius = 3}; return b:can_occupy({x = 5.5, z = 6.5})") == "false");
    CHECK(lua.console("return b:can_occupy({x = 9.5, z = 5.2})") == "true");
    CHECK(lua.console("return w:spawn{position = {x = 9.5, z = 1.5}, collider = {radius = 4}} ~= nil") ==
          "true");
    CHECK(lua.console("return b:can_occupy({x = 9.5, z = 5.2})") == "false");
    auto& world = *engine.world("main");
    const auto c = world.spawn("", {2.5f, 0, 8.5f});
    world.registry().emplace<Collider>(c);
    const auto mover = world.spawn("", {6.5f, 0, 8.5f});
    world.registry().emplace<Mover>(mover);
    CHECK(world.canOccupy(mover, {6.5f, 0, 8.5f}, 0));
    world.registry().patch<Collider>(c, [](Collider& collider) { collider.radius = 4; });
    CHECK_FALSE(world.canOccupy(mover, {6.5f, 0, 8.5f}, 0));
    CHECK(engine.lua().errors().empty());
}

TEST_CASE("collision queries stay finite for any collider radius or position") {
    // The reviewer's probes: a collider of radius 3.4e38 made every contact
    // query walk 2^58 hash buckets, and positions at +-3.4e38 overflowed the
    // bucket coordinates. Script values are bounded (collider radius 0..64,
    // coordinates within 1e6), and the hash copes with anything C++ sets.
    Fixture f;
    const auto a = f.walker({2.5f, 0, 6.5f});
    const auto huge = f.world.spawn("", {5.5f, 0, 7.5f});
    f.world.registry().emplace<Collider>(huge, Collider{3.4e38f, 1.6f, false});
    const auto far = f.world.spawn("", {3.4e38f, 0, -3.4e38f}, 0);
    f.world.registry().emplace<Collider>(far);
    f.world.registry().emplace<Mover>(far);
    const auto nan = f.world.spawn("", {std::numeric_limits<float>::quiet_NaN(), 0, 1});
    f.world.registry().emplace<Collider>(nan);
    f.run(2.0 / 60);
    CHECK(f.has(f.world.takeEvents(), WorldEvent::Kind::ContactBegin));
    // Shrinking or removing the big collider narrows the queries again; the
    // results stay exact either way.
    f.world.registry().patch<Collider>(huge, [](Collider& c) { c.radius = 0.25f; });
    f.world.registry().get<Collider>(huge).solid = true;
    const auto b = f.world.spawn("", {8.5f, 0, 8.5f});
    f.world.registry().emplace<Mover>(b);
    CHECK_FALSE(f.world.canOccupy(b, {5.5f, 0, 7.5f}, 0));
    CHECK(f.world.canOccupy(b, {7.5f, 0, 7.5f}, 0));
    f.world.destroy(huge);
    CHECK(f.world.canOccupy(b, {5.5f, 0, 7.5f}, 0));
    CHECK_FALSE(f.world.canOccupy(b, {2.5f, 0, 6.5f}, 0));
    f.world.destroy(a);
    f.world.destroy(far);
    f.run(1.0 / 60);
    CHECK(f.world.canOccupy(b, {2.5f, 0, 6.5f}, 0));
}

TEST_CASE("zones track movers without colliders and report exits") {
    Fixture f;
    f.world.map().zones.push_back({"pond", "trigger", {2, 7}, {4, 9}, -1, Json::object()});
    const auto e = f.world.spawn("", {3.0f, 0, 5.5f});
    f.world.registry().emplace<Mover>(e);
    f.world.spawn("", {3.0f, 0, 7.5f}); // scenery: neither mover nor collider, not tracked
    f.run(0.05);
    CHECK(count(f.world.takeEvents(), WorldEvent::Kind::ZoneEnter) == 0);
    f.world.place(e, {3.0f, 0, 7.5f}, 0);
    f.run(0.05);
    auto events = f.world.takeEvents();
    REQUIRE(count(events, WorldEvent::Kind::ZoneEnter) == 1);
    f.world.place(e, {3.0f, 0, 5.5f}, 0);
    f.run(0.05);
    CHECK(count(f.world.takeEvents(), WorldEvent::Kind::ZoneExit) == 1);
    f.world.place(e, {3.0f, 0, 7.5f}, 0);
    f.run(0.05);
    f.world.takeEvents();
    f.world.map().zones.clear();
    f.run(0.05);
    CHECK(count(f.world.takeEvents(), WorldEvent::Kind::ZoneExit) == 1);
}

TEST_CASE("move_to plans onto a bridge layer and the mover walks the plan") {
    Fixture f;
    const auto e = f.walker({3.5f, 0, 7.5f});
    f.world.moveTo(e, {8.5f, 1, 7.5f}, true, 1);
    f.run(6);
    const auto& t = f.world.registry().get<Transform>(e);
    CHECK(t.layer == 1);
    CHECK(t.position.x == doctest::Approx(8.5f).epsilon(0.02));
    CHECK(t.position.y == doctest::Approx(1).epsilon(0.02));
    CHECK(f.has(f.world.takeEvents(), WorldEvent::Kind::Arrived));
    // Starting north of the ramp foot: the plan goes round to climb eastwards.
    const auto g = f.walker({6.5f, 0, 5.5f});
    f.world.moveTo(g, {7.5f, 1, 7.5f}, true, 1);
    f.run(6);
    const auto& u = f.world.registry().get<Transform>(g);
    CHECK(u.layer == 1);
    CHECK(u.position.x == doctest::Approx(7.5f).epsilon(0.02));
    CHECK(u.position.y == doctest::Approx(1).epsilon(0.02));
}

TEST_CASE("the camera starts on the start point") {
    Fixture f;
    CHECK(f.world.camera().target == f.world.map().points.at("start").position);
    Json grid = flatGrid(4, 4);
    Scratch s("camera-start", grid);
    CHECK(s.world.camera().target == glm::vec3(2, 0, 2));
}

TEST_CASE("move_to picks the walkable layer nearest the target height") {
    Json grid = flatGrid(9, 3);
    grid["layers"][0]["walk"] = {"111000111", "111000111", "111000111"};
    grid["layers"].push_back({{"walk", {"000000000", "001111100", "000000000"}},
                              {"height", {"0 0 0 0 0 0 0 0 0", "0 0 0 1 1 1 0 0 0", "0 0 0 0 0 0 0 0 0"}},
                              {"ramp", {".........", "..e...w..", "........."}}});
    grid["links"] = {{{"x", 2}, {"z", 1}, {"from", 0}, {"to", 1}, {"dir", "e"}},
                     {{"x", 6}, {"z", 1}, {"from", 0}, {"to", 1}, {"dir", "w"}}};
    Scratch s("layers", grid);
    const auto e = s.world.spawn("", {0.5f, 0, 1.5f});
    s.world.registry().emplace<Mover>(e);
    s.world.moveTo(e, {4.5f, 1, 1.5f}, true);
    s.step(240);
    const auto& t = s.world.registry().get<Transform>(e);
    CHECK(t.layer == 1);
    CHECK(t.position.x == doctest::Approx(4.5f).epsilon(0.02));
    CHECK(t.position.y == doctest::Approx(1).epsilon(0.02));
    s.world.moveTo(e, {8.5f, 0, 1.5f}, true);
    s.step(240);
    CHECK(s.world.registry().get<Transform>(e).layer == 0);
    CHECK(s.world.registry().get<Transform>(e).position.x == doctest::Approx(8.5f).epsilon(0.02));
}

TEST_CASE("a wide mover descends a rise-1 ramp through its directed link on a planned route") {
    // The planner tests a ramp-foot link at the junction edge; the mover used
    // to test it at its centre with its radius of standing room, so a body
    // wider than 0.25 stalled in the link band and reported blocked on a
    // route the planner approved.
    Json grid = flatGrid(9, 3);
    grid["layers"][0]["walk"] = {"111000111", "111000111", "111000111"};
    grid["layers"].push_back({{"walk", {"000000000", "001111100", "000000000"}},
                              {"height", {"0 0 0 0 0 0 0 0 0", "0 0 0 1 1 1 0 0 0", "0 0 0 0 0 0 0 0 0"}},
                              {"ramp", {".........", "..e...w..", "........."}}});
    grid["links"] = {{{"x", 2}, {"z", 1}, {"from", 0}, {"to", 1}, {"dir", "e"}},
                     {{"x", 6}, {"z", 1}, {"from", 0}, {"to", 1}, {"dir", "w"}}};
    Scratch s("wide-ramp", grid);
    const auto e = s.world.spawn("", {4.5f, 1, 1.5f}, 1);
    s.world.registry().emplace<Mover>(e);
    s.world.registry().emplace<Collider>(e, Collider{.radius = 0.45f});
    const auto& t = s.world.registry().get<Transform>(e);
    s.world.moveTo(e, {8.5f, 0, 1.5f}, true);
    s.step(300);
    CHECK(t.layer == 0);
    CHECK(t.position.x == doctest::Approx(8.5f).epsilon(0.02));
    CHECK(std::abs(t.position.y) < 0.02f);
    auto events = s.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::Arrived) == 1);
    CHECK(count(events, WorldEvent::Kind::Blocked) == 0);
    // And back up the same ramp, then down the other one.
    s.world.moveTo(e, {0.5f, 0, 1.5f}, true);
    s.step(600);
    CHECK(t.layer == 0);
    CHECK(t.position.x == doctest::Approx(0.5f).epsilon(0.02));
    events = s.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::Arrived) == 1);
    CHECK(count(events, WorldEvent::Kind::Blocked) == 0);
}

TEST_CASE("static boxes come from the map's authored and baked lists") {
    AssetCache assets{FENGINE_TEST_GAME, FENGINE_BUILTIN};
    auto map = MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json");
    map.colliders.push_back({{2, 0, 6}, {3, 1, 7}});
    map.bake.colliders.push_back({{4, 0, 6}, {5, 1, 7}});
    World world{"boxes", map, assets, 1};
    const auto& blocked = world.boxBlockedCells();
    CHECK(blocked.contains(cellKey({2, 6, 0})));
    CHECK(blocked.contains(cellKey({4, 6, 0})));
    CHECK_FALSE(blocked.contains(cellKey({3, 6, 0})));
    // A live edit of the derived list reaches collision at once, like the authored one.
    world.map().bake.colliders.clear();
    world.rebuildStatic();
    CHECK_FALSE(world.boxBlockedCells().contains(cellKey({4, 6, 0})));
    CHECK(world.boxBlockedCells().contains(cellKey({2, 6, 0})));
}

TEST_CASE("walkers climb onto the arena bridge deck and back down along the planned path") {
    // The arena's directed link at (6, 7) joins the ground to a deck one unit
    // up on layer 1 (cells 6..8 of row 7).
    const auto game = std::filesystem::path(FENGINE_TEST_GAME);
    AssetCache assets(game, FENGINE_BUILTIN);
    World world("arena", MapData::load(game / "maps/arena/map.json"), assets, 1);
    const auto e = world.spawn("", {2.5f, 0, 7.5f});
    world.registry().emplace<Mover>(e);
    for (const auto& [x, layer] : {std::pair{8.5f, 1}, std::pair{2.5f, 0}}) {
        world.moveTo(e, {x, static_cast<float>(layer), 7.5f}, true, layer);
        bool onDeck = false;
        for (int i = 0; i < 600 && world.moving(e); ++i) {
            world.beginTick();
            world.tick(1.0 / 60);
            onDeck = onDeck || world.registry().get<Transform>(e).layer == 1;
        }
        CHECK(onDeck);
        CHECK(count(world.takeEvents(), WorldEvent::Kind::Arrived) == 1);
        CHECK(world.registry().get<Transform>(e).position.x == doctest::Approx(x).epsilon(0.02));
        CHECK(world.registry().get<Transform>(e).layer == layer);
    }
}

TEST_CASE("undirected links carry movers off the end of a layer and back") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"11110000", "11110000", "11110000"};
    grid["layers"].push_back({{"walk", {"00000000", "00011111", "00000000"}}});
    grid["links"] = {{{"x", 3}, {"z", 1}, {"from", 0}, {"to", 1}}};
    Scratch s("undirected", grid);
    const auto e = s.world.spawn("", {1.5f, 0, 1.5f});
    s.world.registry().emplace<Mover>(e);
    drive(s, e, {2, 0}, 180);
    CHECK(s.world.registry().get<Transform>(e).layer == 1);
    CHECK(s.world.registry().get<Transform>(e).position.x > 6);
    drive(s, e, {-2, 0}, 180);
    CHECK(s.world.registry().get<Transform>(e).layer == 0);
    CHECK(s.world.registry().get<Transform>(e).position.x < 2);
}

TEST_CASE("move_to without a path reports blocked at once") {
    Fixture f;
    const auto e = f.walker({2.5f, 0, 3.5f});
    f.world.moveTo(e, {0.5f, 0, 0.5f}, true);
    CHECK(count(f.world.takeEvents(), WorldEvent::Kind::Blocked) == 1);
    CHECK_FALSE(f.world.moving(e));
    f.run(0.5);
    CHECK(f.world.registry().get<Transform>(e).position.x == doctest::Approx(2.5f));
}

TEST_CASE("emitters respect max particles and local-space particles follow the emitter") {
    Fixture f;
    EmitterSettings settings;
    settings.rate = 1000;
    settings.lifetime = 5;
    settings.lifetimeJitter = 0;
    settings.velocity = settings.velocityJitter = glm::vec3(0);
    settings.maxParticles = 25;
    settings.worldSpace = false;
    const auto local = f.world.spawn("", {3, 0, 3});
    f.world.registry().emplace<Emitter>(local, Emitter{settings});
    settings.worldSpace = true;
    const auto fixed = f.world.spawn("", {3, 0, 6});
    f.world.registry().emplace<Emitter>(fixed, Emitter{settings});
    f.run(0.5);
    REQUIRE(f.world.particles().size() == 50);
    CHECK(f.world.liveParticles(local) == 25);
    f.world.place(local, {6, 0, 3}, 0);
    f.world.place(fixed, {6, 0, 6}, 0);
    f.run(1.0 / 60);
    for (const auto& p : f.world.particles()) {
        CHECK(p.position.x == doctest::Approx(p.emitter == local ? 6.0f : 3.0f));
    }
    f.world.burst({1, 0, 1}, settings, 100);
    CHECK(f.world.particles().size() == 75);
}

TEST_CASE("replacing an emitter's settings keeps the cap of the particles still alive") {
    Fixture f;
    EmitterSettings settings;
    settings.rate = 1000;
    settings.lifetime = 5;
    settings.lifetimeJitter = 0;
    settings.maxParticles = 20;
    const auto e = f.world.spawn("", {3, 0, 3});
    f.world.registry().emplace<Emitter>(e, Emitter{settings});
    f.run(0.25);
    REQUIRE(f.world.liveParticles(e) == 20);
    settings.color = glm::vec4(1, 0, 0, 1);
    f.world.registry().emplace_or_replace<Emitter>(e, Emitter{settings});
    f.run(0.25);
    CHECK(f.world.liveParticles(e) == 20);
    CHECK(f.world.particles().size() == 20);
    settings.maxParticles = 30;
    f.world.registry().emplace_or_replace<Emitter>(e, Emitter{settings});
    f.run(0.25);
    CHECK(f.world.particles().size() == 30);
    f.world.destroy(e);
    f.run(6);
    CHECK(f.world.particles().empty());
    CHECK(f.world.liveParticles(e) == 0);
}

TEST_CASE("weather comes from data presets that games can override") {
    Fixture f;
    f.world.environment().weather = {"rain", 1, glm::vec3(0), glm::vec4(1), ""};
    f.run(0.2);
    REQUIRE_FALSE(f.world.particles().empty());
    CHECK(f.world.particles().front().color == glm::vec4(0.75f, 0.82f, 1, 0.5f));
    Fixture unknown;
    unknown.world.environment().weather = {"sandstorm", 1, glm::vec3(0), glm::vec4(1), ""};
    unknown.run(0.2);
    CHECK(unknown.world.particles().empty());
    Scratch s("weather", flatGrid(4, 4),
              {{"data/weather.json",
                R"({"rain": {"rate": 120, "color": [1, 0, 0, 1], "color_end": [1, 0, 0, 1]}})"}});
    s.world.environment().weather = {"rain", 1, glm::vec3(0), glm::vec4(1), ""};
    s.step(12);
    REQUIRE_FALSE(s.world.particles().empty());
    CHECK(s.world.particles().front().color == glm::vec4(1, 0, 0, 1));
}

TEST_CASE("worlds with the same seed simulate identically") {
    Fixture a, b;
    for (auto* f : {&a, &b}) {
        f->world.environment().weather = {"snow", 1, {0.3f, 0, 0}, glm::vec4(1), ""};
        f->world.moveTo(f->walker({2.5f, 0, 6.5f}), {9.5f, 0, 2.5f}, true);
        f->run(1);
    }
    REQUIRE(a.world.particles().size() == b.world.particles().size());
    for (std::size_t i = 0; i < a.world.particles().size(); ++i) {
        CHECK(a.world.particles()[i].position == b.world.particles()[i].position);
    }
}

TEST_CASE("the event queue is bounded and drops the oldest events") {
    EventQueue queue;
    for (int i = 0; i < 5000; ++i) {
        queue.push({WorldEvent::Kind::Arrived, entt::null, entt::null, std::to_string(i)});
    }
    CHECK(queue.size() == EventQueue::Capacity);
    CHECK(queue.dropped() == 5000 - EventQueue::Capacity);
    const auto events = queue.take();
    REQUIRE(events.size() == EventQueue::Capacity);
    CHECK(events.front().name == std::to_string(5000 - EventQueue::Capacity));
    CHECK(queue.size() == 0);
    CHECK(queue.dropped() == 0);
}

TEST_CASE("children of a destroyed parent detach and stay put") {
    Fixture f;
    const auto parent = f.world.spawn("", {2.5f, 0, 2.5f});
    const auto child = f.world.spawn("", {0, 0, 0});
    auto& link = f.world.registry().get<Transform>(child);
    link.parent = parent;
    link.offset = {0, 1, 0};
    f.run(1.0 / 60);
    CHECK(f.world.registry().get<Transform>(child).position == glm::vec3(2.5f, 1, 2.5f));
    f.world.destroy(parent);
    f.world.spawn("", {7, 0, 7});
    f.run(1.0 / 60);
    const auto& t = f.world.registry().get<Transform>(child);
    CHECK((t.parent == entt::null));
    CHECK(t.position == glm::vec3(2.5f, 1, 2.5f));
}

TEST_CASE("query filters by tag and distance") {
    Fixture f;
    const auto a = f.world.spawn("", {2, 0, 2});
    f.world.registry().emplace<Tags>(a, Tags{{"npc"}});
    const auto b = f.world.spawn("", {8, 0, 8});
    f.world.registry().emplace<Tags>(b, Tags{{"npc", "shop"}});
    f.world.spawn("", {2, 0, 3});
    CHECK(f.world.query("npc", std::nullopt, 0).size() == 2);
    CHECK(f.world.query("shop", std::nullopt, 0) == std::vector<entt::entity>{b});
    CHECK(f.world.query("npc", glm::vec3(2, 0, 2), 1.5f) == std::vector<entt::entity>{a});
    CHECK(f.world.query("", glm::vec3(2, 0, 2), 1.5f).size() == 2);
}

namespace {
// Four directions, one frame each, frame x = row index * 8; "attack" has a
// frame-0 event.
const char* SpriteJson = R"({"atlas": "hero.png", "clips": {
  "walk": {"directions": [[{"rect": [0, 0, 8, 8]}], [{"rect": [8, 0, 8, 8]}], [{"rect": [16, 0, 8, 8]}],
           [{"rect": [24, 0, 8, 8]}], [{"rect": [32, 0, 8, 8]}], [{"rect": [40, 0, 8, 8]}],
           [{"rect": [48, 0, 8, 8]}], [{"rect": [56, 0, 8, 8]}]]},
  "attack": {"loop": false, "directions": [
    [{"rect": [0, 0, 8, 8], "duration": 0.1, "event": "swing"}, {"rect": [8, 0, 8, 8], "duration": 0.1, "event": "hit"}],
    [{"rect": [0, 0, 8, 8], "duration": 0.1, "event": "swing"}, {"rect": [8, 0, 8, 8], "duration": 0.1, "event": "hit"}],
    [{"rect": [0, 0, 8, 8], "duration": 0.1, "event": "swing"}, {"rect": [8, 0, 8, 8], "duration": 0.1, "event": "hit"}],
    [{"rect": [0, 0, 8, 8], "duration": 0.1, "event": "swing"}, {"rect": [8, 0, 8, 8], "duration": 0.1, "event": "hit"}]]}}})";

std::vector<std::string> names(const std::vector<WorldEvent>& events) {
    std::vector<std::string> result;
    for (const auto& e : events) {
        if (e.kind == WorldEvent::Kind::AnimationEvent) {
            result.push_back(e.name);
        }
    }
    return result;
}
} // namespace

TEST_CASE("sprite rows follow the documented 8-direction order and clamp out of range") {
    const auto animation = SpriteAnimation::fromJson(Json::parse(SpriteJson));
    const float d = std::sqrt(0.5f);
    CHECK(animation.direction("walk", {0, 1}, 0) == 0);  // south
    CHECK(animation.direction("walk", {-d, d}, 0) == 1); // south-west
    CHECK(animation.direction("walk", {-1, 0}, 0) == 2); // west
    CHECK(animation.direction("walk", {-d, -d}, 0) == 3);
    CHECK(animation.direction("walk", {0, -1}, 0) == 4); // north
    CHECK(animation.direction("walk", {d, -d}, 0) == 5);
    CHECK(animation.direction("walk", {1, 0}, 0) == 6); // east
    CHECK(animation.direction("walk", {d, d}, 0) == 7);
    CHECK(animation.direction("walk", {0, 1}, 90) == 2); // camera turned a quarter
    CHECK(animation.frameFor("walk", 99, 0).rect.x == 56);
    CHECK(animation.frameFor("walk", -3, 0).rect.x == 0);
    std::vector<std::string> fired;
    animation.events("attack", 99, 0, 1, [&](const std::string& name) { fired.push_back(name); });
    CHECK(fired == std::vector<std::string>{"swing", "hit"});
}

TEST_CASE("frame-0 animation events fire when a clip starts; fixed directions never throw") {
    Scratch s("anim", flatGrid(4, 4), {{"sprites/hero.anim.json", SpriteJson}});
    const auto e = s.world.spawn("", {1.5f, 0, 1.5f});
    Sprite sprite;
    sprite.animation = "sprites/hero.anim.json";
    sprite.clip = "attack";
    sprite.direction = 7; // the clip only has four rows
    s.world.registry().emplace<Sprite>(e, sprite);
    s.step();
    CHECK(names(s.world.takeEvents()) == std::vector<std::string>{"swing"});
    s.step(6);
    CHECK(names(s.world.takeEvents()) == std::vector<std::string>{"hit"});
    s.step(10);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::AnimationFinished) == 1);
    s.world.setClip(e, "attack");
    s.step();
    CHECK(names(s.world.takeEvents()) == std::vector<std::string>{"swing"});
}

TEST_CASE("an unknown clip shows, fires and finishes like the clip it falls back to") {
    // No "idle" clip, so unknown names fall back to the first clip: "attack".
    const auto animation = SpriteAnimation::fromJson(Json::parse(SpriteJson));
    CHECK(&animation.clip("jump") == &animation.clips.at("attack"));
    CHECK(animation.frameFor("jump", 0, 0.15).rect.x == 8);
    std::vector<std::string> fired;
    animation.events("jump", 0, 0, 1, [&](const std::string& name) { fired.push_back(name); });
    CHECK(fired == std::vector<std::string>{"swing", "hit"});
    Scratch s("anim-fallback", flatGrid(4, 4), {{"sprites/hero.anim.json", SpriteJson}});
    const auto e = s.world.spawn("", {1.5f, 0, 1.5f});
    Sprite sprite;
    sprite.animation = "sprites/hero.anim.json";
    sprite.clip = "jump";
    s.world.registry().emplace<Sprite>(e, sprite);
    s.step(20);
    const auto events = s.world.takeEvents();
    CHECK(names(events) == std::vector<std::string>{"swing", "hit"});
    REQUIRE(count(events, WorldEvent::Kind::AnimationFinished) == 1);
    CHECK(std::find_if(events.begin(), events.end(), [](const WorldEvent& event) {
              return event.kind == WorldEvent::Kind::AnimationFinished;
          })->name == "jump");
}

TEST_CASE("camera yaw interpolates the short way and projection matches the ray at any alpha") {
    Camera camera;
    camera.yaw = 720;
    camera.snap();
    camera.yaw = 0;
    CHECK(std::fmod(camera.interpolatedYaw(0.5), 360.0f) == doctest::Approx(0));
    camera.yaw = 10;
    camera.snap();
    camera.yaw = -1070; // the same heading three turns away
    CHECK(camera.interpolatedYaw(0.5) == doctest::Approx(10));
    camera.yaw = 350;
    CHECK(camera.interpolatedYaw(0.5) == doctest::Approx(0));

    Camera moving;
    moving.snap();
    moving.target = {10, 0, 0};
    moving.fov = 60;
    const glm::vec3 point(4, 1, 2);
    const auto ndc = moving.project(point, 1.5f, 0.5);
    CHECK(glm::length(glm::vec2(ndc) - glm::vec2(moving.project(point, 1.5f, 1))) > 0.05f);
    glm::vec3 origin, direction;
    moving.ray(glm::vec2(ndc), 1.5f, origin, direction, 0.5);
    const auto toPoint = point - origin;
    CHECK(glm::length(toPoint - direction * glm::dot(toPoint, direction)) < 1e-3f);
}

TEST_CASE("maps reject placements and zones on layers the grid lacks, and duplicate zone ids") {
    auto map = MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json").toJson();
    auto bad = map;
    bad["placements"] = {{{"id", "a"}, {"type", "npc"}, {"position", {1, 0, 1}}, {"layer", 2}}};
    CHECK_THROWS(MapData::fromJson(bad));
    bad = map;
    bad["zones"] = {{{"id", "z"}, {"min", {0, 0}}, {"max", {1, 1}}, {"layer", -2}}};
    CHECK_THROWS(MapData::fromJson(bad));
    bad["zones"][0]["layer"] = 1;
    CHECK_NOTHROW(MapData::fromJson(bad));
    // Presence inside zones is keyed by id, so ids must be unique and non-empty.
    bad["zones"].push_back({{"id", "z"}, {"min", {2, 2}}, {"max", {3, 3}}});
    CHECK_THROWS_WITH(MapData::fromJson(bad), doctest::Contains("Duplicate or empty zone id"));
    bad["zones"][1]["id"] = "";
    CHECK_THROWS(MapData::fromJson(bad));
    bad["zones"][1]["id"] = "other";
    CHECK_NOTHROW(MapData::fromJson(bad));
}

TEST_CASE("maps reject tilt-shift focus outside 0..1 and negative weather intensity") {
    auto map = MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json").toJson();
    auto bad = map;
    bad["environment"]["post"] = {{"focus", 1.5}};
    CHECK_THROWS(MapData::fromJson(bad));
    bad["environment"]["post"] = {{"focus_range", -0.1}};
    CHECK_THROWS(MapData::fromJson(bad));
    bad["environment"]["post"] = {{"focus", 1}, {"focus_range", 0}};
    CHECK_NOTHROW(MapData::fromJson(bad));
    bad["environment"]["weather"] = {{"kind", "rain"}, {"intensity", -1}};
    CHECK_THROWS(MapData::fromJson(bad));
    bad["environment"]["weather"]["intensity"] = 0;
    CHECK_NOTHROW(MapData::fromJson(bad));
    bad["environment"]["post"] = {{"custom", {{{"shader", "sepia"}, {"params", 3}}}}};
    CHECK_THROWS(MapData::fromJson(bad));
}

TEST_CASE("the engine skips paused worlds and scales their time") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = std::filesystem::temp_directory_path() / "fengine-world-pause";
    options.seed = 5;
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    engine.lua().console("fe.world.load('main', 'arena')");
    const auto world = engine.world("main");
    REQUIRE(world);
    engine.tick(0.5);
    CHECK(world->time() == doctest::Approx(0.5));
    engine.lua().console("fe.world.get('main').paused = true");
    CHECK(world->paused);
    engine.tick(0.5);
    CHECK(world->time() == doctest::Approx(0.5));
    engine.lua().console("local w = fe.world.get('main'); w.paused = false; w.time_scale = 0.5");
    engine.tick(0.5);
    CHECK(world->time() == doctest::Approx(0.75));
    CHECK(engine.lua().console("fe.world.get('main').time_scale") == "0.5");
    for (const char* bad : {"-1", "10.5", "0/0", "1/0"}) {
        CAPTURE(bad);
        CHECK_FALSE(engine.lua().evaluate(std::string("fe.world.get('main').time_scale = ") + bad).ok);
    }
    CHECK(world->timeScale == doctest::Approx(0.5f));
    CHECK(engine.lua()
              .console("fe.world.get('main'):path({x=2,z=2}, {x=3,z=2}, {max_step=-1})")
              .find("non-negative") != std::string::npos);
}


namespace {
std::vector<WorldEvent::Kind> kinds(const std::vector<WorldEvent>& events) {
    std::vector<WorldEvent::Kind> result;
    for (const auto& e : events) {
        if (e.kind != WorldEvent::Kind::ZoneEnter && e.kind != WorldEvent::Kind::ZoneExit &&
            e.kind != WorldEvent::Kind::CellEnter) {
            result.push_back(e.kind);
        }
    }
    return result;
}

glm::vec3 centre(int x, int z, float y = 0) {
    return {static_cast<float>(x) + 0.5f, y, static_cast<float>(z) + 0.5f};
}
} // namespace

TEST_CASE("a followed route runs through its waypoints without stopping and arrives once") {
    Scratch s("route", flatGrid(10, 3));
    const auto e = s.world.spawn("", centre(0, 1));
    s.world.registry().emplace<Mover>(e).speed = 3;
    std::vector<Waypoint> route;
    for (int x = 1; x <= 6; ++x) {
        route.push_back({centre(x, 1), 0});
    }
    s.world.follow(e, route);
    // Six units at 3 units/second: two seconds, with no time lost at the waypoints.
    s.step(121);
    CHECK(s.world.registry().get<Transform>(e).position.x == doctest::Approx(6.5f).epsilon(0.01));
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Arrived) == 1);
    CHECK_FALSE(s.world.moving(e));
}

TEST_CASE("a mover does not follow a route whose search hit its visit cap") {
    Scratch s("capped", flatGrid(8, 3));
    const auto e = s.world.spawn("", centre(0, 1));
    s.world.registry().emplace<Mover>(e);
    // Two expansions settle the cell next to the start and reach the target
    // two cells away, but stop before settling it: a route, flagged truncated.
    StepRules rules;
    rules.diagonal = true;
    const auto capped = findRoute(s.world.grid(), {0, 1, 0}, {2, 1, 0}, rules, 2);
    REQUIRE(capped.truncated);
    REQUIRE_FALSE(capped.steps.empty());
    s.world.setRouteVisitLimit(2);
    s.world.moveTo(e, centre(2, 1), true);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked});
    CHECK_FALSE(s.world.moving(e));
    s.step(60);
    CHECK(s.world.registry().get<Transform>(e).position.x == doctest::Approx(0.5f));
    // With the default cap the same move plans fully and arrives.
    s.world.setRouteVisitLimit(DefaultRouteVisits);
    s.world.moveTo(e, centre(2, 1), true);
    s.step(120);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Arrived) == 1);
}

TEST_CASE("movers jump gaps on planned and given routes") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"11001111", "11001111", "11001111"};
    Scratch s("gap", grid);
    const auto e = s.world.spawn("", centre(0, 1));
    auto& mover = s.world.registry().emplace<Mover>(e);
    s.world.moveTo(e, centre(5, 1), true);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked});
    mover.jump = {2, 0.6f, 0.6f, false};
    s.world.moveTo(e, centre(5, 1), true);
    s.step(240);
    const auto& t = s.world.registry().get<Transform>(e);
    CHECK(t.position.x == doctest::Approx(5.5f).epsilon(0.01));
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(kinds(s.world.takeEvents()) ==
          std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed, WorldEvent::Kind::Arrived});
    // A planned route carries the jump mark to the mover.
    StepRules rules;
    rules.jump = {2, 0.6f, 0.6f, false};
    std::vector<Waypoint> route;
    for (const auto& step : findRoute(s.world.grid(), {5, 1, 0}, {0, 1, 0}, rules).steps) {
        route.push_back({centre(step.cell.x, step.cell.z), step.cell.layer, step.kind});
    }
    s.world.follow(e, route);
    s.step(240);
    CHECK(s.world.registry().get<Transform>(e).position.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
}

TEST_CASE("movers climb ladders on planned routes and when steered into them") {
    Json grid = flatGrid(5, 4);
    grid["layers"][0]["height"] = {"4 4 4 4 4", "0 0 0 0 0", "0 0 0 0 0", "0 0 0 0 0"};
    grid["links"] = {{{"x", 2}, {"z", 1}, {"from", 0}, {"to", 0}, {"dir", "n"}, {"kind", "ladder"}}};
    Scratch s("ladder", grid);
    const auto e = s.world.spawn("", centre(0, 2));
    s.world.registry().emplace<Mover>(e).climbSpeed = 2;
    s.world.moveTo(e, centre(4, 0, 4), true);
    s.step(600);
    const auto& t = s.world.registry().get<Transform>(e);
    CHECK(t.position.x == doctest::Approx(4.5f).epsilon(0.01));
    CHECK(t.position.y == doctest::Approx(4));
    CHECK(kinds(s.world.takeEvents()) ==
          std::vector{WorldEvent::Kind::ClimbStart, WorldEvent::Kind::ClimbEnd, WorldEvent::Kind::Arrived});
    // Walking south off the top at the ladder climbs down.
    s.world.place(e, centre(2, 0, 4), 0);
    drive(s, e, {0, 2}, 180);
    CHECK(t.position.z > 1);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::ClimbEnd) == 1);
    // Elsewhere the cliff still blocks.
    s.world.place(e, centre(0, 1), 0);
    drive(s, e, {0, -2}, 60);
    CHECK(t.position.y == doctest::Approx(0));
}

TEST_CASE("platforms carry riders and make the cells they cover walkable") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"10000001", "10000001", "10000001"};
    Scratch s("raft", grid);
    auto& registry = s.world.registry();
    const auto raft = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Platform>(raft);
    registry.emplace<Mover>(raft, Mover{.speed = 1, .collide = false});
    s.step();
    CHECK(s.world.grid().at(1, 1).walkable);
    CHECK(s.world.grid().at(1, 1).height == doctest::Approx(0.2f));
    const auto rider = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Mover>(rider);
    s.world.moveTo(raft, centre(5, 1, 0.2f), false);
    s.step(300);
    const auto& t = registry.get<Transform>(rider);
    CHECK(t.position.x == doctest::Approx(5.5f).epsilon(0.01));
    CHECK(t.position.y == doctest::Approx(0.2f));
    CHECK_FALSE(s.world.grid().at(1, 1).walkable);
    CHECK(s.world.grid().at(5, 1).walkable);
    // The rider can walk about on the raft but not off it into the water.
    s.world.moveTo(rider, centre(6, 1), false);
    s.step(60);
    CHECK(t.position.x < 6);
    // Removing the platform restores the cells under it.
    registry.remove<Platform>(raft);
    s.step();
    CHECK_FALSE(s.world.grid().at(5, 1).walkable);
}

TEST_CASE("carried riders still collide with solid entities that are not aboard") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"11111111", "10000001", "11111111"};
    Scratch s("raft-block", grid);
    auto& registry = s.world.registry();
    const auto raft = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Platform>(raft, Platform{.size = {1, 3}});
    registry.emplace<Mover>(raft, Mover{.speed = 1, .collide = false});
    registry.emplace<Collider>(raft);
    s.step();
    const auto rider = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Mover>(rider);
    registry.emplace<Collider>(rider);
    const auto crate = s.world.spawn("", centre(1, 0, 0.2f)); // aboard: rides along, never blocks
    registry.emplace<Collider>(crate);
    const auto ghost = s.world.spawn("", centre(1, 2, 0.2f)); // aboard, not solid
    registry.emplace<Collider>(ghost, Collider{.solid = false});
    const auto post = s.world.spawn("", centre(4, 1, -0.3f)); // a pole in the water, too low to board
    registry.emplace<Collider>(post);
    s.world.moveTo(raft, centre(6, 1, 0.2f), false);
    s.step(400);
    const auto& carried = registry.get<Transform>(rider);
    CHECK(registry.get<Transform>(raft).position.x == doctest::Approx(6.5f).epsilon(0.01));
    CHECK(carried.position.x > 3.5f); // carried up to the post, then held back and left behind
    CHECK(carried.position.x == doctest::Approx(4.05f).epsilon(0.01)); // touching it
    CHECK(registry.get<Transform>(crate).position.x == doctest::Approx(6.5f).epsilon(0.01));
    CHECK(registry.get<Transform>(ghost).position.x == doctest::Approx(6.5f).epsilon(0.01));
}

TEST_CASE("raycasts hit grid, map colliders and entities; edits to colliders apply live") {
    Scratch s("ray", flatGrid(8, 8));
    const auto down = s.world.raycast({2.5f, 5, 2.5f}, {0, -1, 0}, {});
    REQUIRE(down);
    CHECK(down->kind == WorldHit::Kind::Grid);
    CHECK(down->distance == doctest::Approx(5));
    s.world.map().colliders.push_back({{2, 0, 2}, {3, 2, 3}});
    s.world.rebuildStatic();
    const auto box = s.world.raycast({2.5f, 5, 2.5f}, {0, -1, 0}, {});
    REQUIRE(box);
    CHECK(box->kind == WorldHit::Kind::Collider);
    CHECK(box->point.y == doctest::Approx(2));
    const auto npc = s.world.spawn("", centre(5, 2));
    s.world.registry().emplace<Collider>(npc);
    s.world.registry().emplace<Tags>(npc, Tags{{"npc"}});
    RayOptions options;
    const auto side = s.world.raycast({0.5f, 0.5f, 2.5f}, {1, 0, 0}, options);
    REQUIRE(side);
    CHECK(side->kind == WorldHit::Kind::Collider); // the box comes first
    const auto past = s.world.raycast({3.5f, 0.5f, 2.5f}, {1, 0, 0}, options);
    REQUIRE(past);
    CHECK(past->kind == WorldHit::Kind::Entity);
    CHECK(past->entity == npc);
    CHECK(past->distance == doctest::Approx(5.5f - 0.25f - 3.5f));
    options.tags = {"enemy"};
    CHECK_FALSE(s.world.raycast({3.5f, 0.5f, 2.5f}, {1, 0, 0}, options));
    options.tags.clear();
    options.entities = false;
    CHECK_FALSE(s.world.raycast({3.5f, 0.5f, 2.5f}, {1, 0, 0}, options));
    options.maxDistance = 1;
    options.entities = true;
    CHECK_FALSE(s.world.raycast({3.5f, 0.5f, 2.5f}, {1, 0, 0}, options));
    // A trigger (non-solid collider) in front is passed through unless asked for.
    const auto trigger = s.world.spawn("", centre(4, 2));
    s.world.registry().emplace<Collider>(trigger, Collider{.solid = false});
    options.maxDistance = 200;
    const auto through = s.world.raycast({3.5f, 0.5f, 2.5f}, {1, 0, 0}, options);
    REQUIRE(through);
    CHECK(through->entity == npc);
    options.solidOnly = false;
    const auto blocked = s.world.raycast({3.5f, 0.5f, 2.5f}, {1, 0, 0}, options);
    REQUIRE(blocked);
    CHECK(blocked->entity == trigger);
}

TEST_CASE("sprite picking returns the sprite drawn on top") {
    Scratch s("pick", flatGrid(8, 8), {{"sprites/hero.anim.json", SpriteJson}});
    auto& registry = s.world.registry();
    const auto near = s.world.spawn("", centre(3, 4));
    const auto far = s.world.spawn("", centre(3, 2));
    for (const auto e : {near, far}) {
        auto& sprite = registry.emplace<Sprite>(e);
        sprite.animation = "sprites/hero.anim.json";
        sprite.clip = "walk";
        sprite.scale = 8; // 8 px frames at 32 px per unit: 2 x 2 units
    }
    auto& camera = s.world.camera();
    camera.target = centre(3, 3);
    camera.yaw = 0;
    camera.snap();
    const auto eye = camera.position();
    const auto aim = [&](glm::vec3 point) {
        return point - eye;
    };
    CHECK(s.world.pickSprite(eye, aim(centre(3, 4, 1)), 1, 32) == near);
    // Where the near sprite's top covers the far one's feet, the near one wins.
    const auto overlap = glm::normalize(aim(centre(3, 4, 1.9f)));
    CHECK(rayQuad(eye, overlap,
                  spriteQuad(registry.get<Sprite>(far), *s.assets.sprite("sprites/hero.anim.json"),
                             centre(3, 2), {0, 1}, camera, 1, 32)));
    CHECK(s.world.pickSprite(eye, overlap, 1, 32) == near);
    const auto quad = spriteQuad(registry.get<Sprite>(far), *s.assets.sprite("sprites/hero.anim.json"),
                                 centre(3, 2), {0, 1}, camera, 1, 32);
    // The top of the far sprite shows above the near one.
    CHECK(s.world.pickSprite(eye, aim(quad.corner + quad.right * 0.5f + quad.up * 0.95f), 1, 32) == far);
    CHECK(s.world.pickSprite(eye, aim(centre(7, 7, 0)), 1, 32) == entt::entity{entt::null});
    const auto middle = quad.corner + quad.right * 0.5f + quad.up * 0.5f;
    registry.get<Sprite>(near).visible = false;
    CHECK(s.world.pickSprite(eye, aim(middle), 1, 32) == far);
}

TEST_CASE("editor edits to map emitters and colliders apply without a reload") {
    Scratch s("live", flatGrid(6, 6));
    const auto emitters = [&] {
        return s.world.registry().view<Emitter>().size();
    };
    CHECK(emitters() == 0);
    MapEmitter emitter;
    emitter.position = centre(2, 2);
    emitter.settings.sprite = "@fe/assets/particles/dot.png";
    applyEdit(s.world, [&](MapData& m) { m.emitters.push_back(emitter); });
    CHECK(emitters() == 1);
    const auto first = *s.world.registry().view<Emitter>().begin();
    applyEdit(s.world, [&](MapData& m) { m.colliders.push_back({{0, 0, 0}, {1, 1, 1}}); });
    CHECK(s.world.colliders().size() == 1);
    CHECK(s.world.registry().valid(first)); // unchanged emitters keep running
    applyEdit(s.world, [&](MapData& m) { m.emitters.clear(); });
    CHECK(emitters() == 0);
    CHECK(s.world.authored().emitters.empty());
}

TEST_CASE("Lua opts into diagonal gap jumps and raycasts through triggers") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = std::filesystem::temp_directory_path() / "fengine-world-diagonal-lua";
    options.seed = 5;
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    // Two islands, (1, 6) and (3, 4), diagonally across a pit.
    lua.console(
        "w = fe.world.load('main', 'arena'); for z = 0, 9 do for x = 0, 11 do "
        "w:set_cell({x = x, z = z}, {walkable = (x == 1 and z == 6) or (x == 3 and z == 4)}) end end");
    CHECK(lua.console("#w:path({x=1,z=6}, {x=3,z=4}, {jump = {distance = 1}})") == "0");
    CHECK(lua.console("local p = w:path({x=1,z=6}, {x=3,z=4}, {jump = {distance = 1, diagonal = true}}); "
                      "return #p, p[1].jump") == "1\ttrue");
    lua.console(
        "hero = w:spawn{position = {x = 1.5, z = 6.5}, mover = {jump = {distance = 1, diagonal = true}}}");
    lua.console("hero:move_to({x = 3.5, z = 4.5})");
    for (int i = 0; i < 120; ++i) {
        engine.tick(1.0 / 60);
    }
    CHECK(lua.console("local p = hero.position; return p.x, p.z") == "3.5\t4.5");
    lua.console("zone = w:spawn{position = {x = 5.5, z = 1.5}, collider = {solid = false}}; "
                "wall = w:spawn{position = {x = 6.5, z = 1.5}, collider = {}}");
    CHECK(lua.console("return w:raycast({x=3.5, y=0.5, z=1.5}, {x=1, y=0, z=0}).entity == wall") == "true");
    CHECK(lua.console("return w:raycast({x=3.5, y=0.5, z=1.5}, {x=1, y=0, z=0}, {solid_only = false}).entity "
                      "== zone") == "true");
}

TEST_CASE("Lua sees jump and climb marks, cost callbacks, routes and raycasts") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = std::filesystem::temp_directory_path() / "fengine-world-nav-lua";
    options.seed = 5;
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    lua.console("w = fe.world.load('main', 'arena')");
    CHECK(lua.console("#w:path({x=1,z=5}, {x=3,z=5})") == "2");
    CHECK(lua.console("#w:path({x=1,z=5}, {x=3,z=5}, {cost = function(a, b, base) "
                      "if b.x == 2 and b.z == 5 then return false end return base end})") == "4");
    CHECK(lua.console("w:path({x=1,z=5}, {x=3,z=5}, {cost = function() error('nope') end})").find("nope") !=
          std::string::npos);
    CHECK(lua.console("w:path({x=1,z=5}, {x=3,z=5}, {cost = function() return 'x' end})")
              .find("number or false") != std::string::npos);
    lua.console("w:set_cell({x = 2, z = 5}, {walkable = false})");
    CHECK(lua.console(
              "local p = w:path({x=1,z=5}, {x=3,z=5}, {jump = {distance = 1}}); return #p, p[1].jump") ==
          "1	true");
    CHECK(lua.console("w:set_cell({x = 7, z = 2}, {rise = 2}); return w:cell({x = 7, z = 2}).rise") == "2.0");
    lua.console("hero = w:spawn{position = {x = 1.5, z = 5.5}, mover = {speed = 4, jump = {distance = 1}}, "
                "collider = {}}");
    lua.console("hero:move_to(nil, {path = w:path({x=1,z=5}, {x=3,z=6}, {jump = {distance = 1}})})");
    for (int i = 0; i < 120; ++i) {
        engine.tick(1.0 / 60);
    }
    CHECK(lua.console("local p = hero.position; return p.x, p.z") == "3.5	6.5");
    CHECK(lua.console(
              "local n = 0; for _, e in ipairs(w:events()) do if e.type == 'arrived' then n = n + 1 end "
              "end; return n") == "1");
    CHECK(lua.console("local hit = w:raycast({x=1.5, y=5, z=5.5}, {x=0, y=-1, z=0}); "
                      "return hit.kind, hit.cell.x, hit.distance") == "grid	1	5.0");
    CHECK(
        lua.console("return w:raycast({x=3.5, y=0.5, z=1.5}, {x=0, y=0, z=1}, {max = 10}).entity == hero") ==
        "true");
    CHECK(lua.console("return w:pick_entity(-100, -100)") == "nil");
    lua.console("raft = w:spawn{position = {x = 5.5, z = 7.5}, keep_height = true, platform = {size = {x = "
                "2, z = 1}}}");
    auto& world = *engine.world("main");
    REQUIRE(world.registry().view<Platform>().size() == 1);
    const auto raft = world.registry().view<Platform>().front();
    CHECK(world.registry().get<Platform>(raft).size == glm::vec2(2, 1));
    CHECK(lua.console("do raft:platform(nil) end") == ""); // a statement: the console prints no result
    CHECK_FALSE(world.registry().all_of<Platform>(raft));
    CHECK(lua.console("do raft:platform{size = {x = 1, z = 1}} end") == "");
    REQUIRE(world.registry().all_of<Platform>(raft));
    CHECK(world.registry().get<Platform>(raft).size == glm::vec2(1, 1));
    CHECK(engine.lua().errors().empty());
}

TEST_CASE("standing room inside a link cell counts every link of the cell") {
    Scratch s("link-support",
              {{"width", 3},
               {"depth", 3},
               {"layers", Json::array({Json{{"walk", {"111", "101", "111"}}},
                                       Json{{"walk", {"000", "010", "000"}}, {"height_fill", 3}},
                                       Json{{"walk", {"000", "010", "000"}}, {"height_fill", 0}}})},
               {"links", Json::array({Json{{"x", 1}, {"z", 1}, {"from", 0}, {"to", 1}},
                                      Json{{"x", 1}, {"z", 1}, {"from", 0}, {"to", 2}}})}});
    const auto e = s.world.spawn("", {0.5f, 0, 0.5f});
    s.world.registry().emplace<Mover>(e);
    // The first link's other surface is 3 units up; the second one's meets the ground.
    CHECK(s.world.canOccupy(e, {1.5f, 0, 1.5f}, 0));
    CHECK(s.world.canOccupy(e, {1.5f, 0, 1.5f}, 2));
    s.world.grid().at(1, 1, 2).walkable = false;
    CHECK_FALSE(s.world.canOccupy(e, {1.5f, 0, 1.5f}, 0));
}

TEST_CASE("Lua path rules can leave ladders out") {
    const auto game = Scratch::prepare(
        "ladder-rules",
        {{"game.json", R"({"title": "Ladders"})"},
         {"main.lua", "return {init = function() end, update = function() end, draw = function() end}"},
         {"maps/cliff/map.json",
          R"({"format": 2, "id": "cliff", "grid": {"width": 5, "depth": 3, "layers": [{"height": ["4 4 4 4 4",
          "0 0 0 0 0", "0 0 0 0 0"]}], "links": [{"x": 2, "z": 1, "from": 0, "to": 0, "dir": "n",
          "kind": "ladder"}]}})"}});
    EngineOptions options;
    options.gameRoot = game;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = game / "user";
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    lua.console("w = fe.world.load('main', 'cliff')");
    CHECK(lua.console("local p = w:path({x=0,z=2}, {x=0,z=0}); return #p > 0, p.truncated") == "true	nil");
    CHECK(lua.console("return #w:path({x=0,z=2}, {x=0,z=0}, {ladders = false})") == "0");
    CHECK(lua.console("local p = w:path({x=2,z=1}, {x=2,z=0}); return #p, p[1].climb") == "1\ttrue");
    CHECK(engine.lua().errors().empty());
}

TEST_CASE("sprites and meshes name game shaders by validated names") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = std::filesystem::temp_directory_path() / "fengine-world-shader-names";
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    lua.console("w = fe.world.load('main', 'arena')");
    lua.console("e = w:spawn{position = {x = 2.5, z = 2.5}, sprite = {animation = 'a.anim.json', shader = "
                "'hologram'}, "
                "mesh = {model = 'm.glb', shader = 'glint'}}");
    CHECK(lua.console("return e:sprite().shader") == "hologram");
    CHECK(lua.console("return e:sprite{shader = ''}.shader") == "nil");
    CHECK(lua.console("e:sprite{shader = 'No Spaces'}").find("must use a-z") != std::string::npos);
    CHECK(lua.console("e:mesh{shader = '../escape'}").find("must use a-z") != std::string::npos);
    auto& world = *engine.world("main");
    const auto entity = world.registry().view<MeshInstance>().front();
    CHECK(world.registry().get<MeshInstance>(entity).shader == "glint");
}


namespace {
std::vector<GridPos> cellsEntered(const std::vector<WorldEvent>& events, entt::entity who) {
    std::vector<GridPos> cells;
    for (const auto& e : events) {
        if (e.kind == WorldEvent::Kind::CellEnter && e.a == who) {
            cells.push_back(e.cell);
        }
    }
    return cells;
}
} // namespace

TEST_CASE("solid bodies that overlap separate instead of locking") {
    Fixture f;
    const auto a = f.walker({2.5f, 0, 5.5f});
    const auto b = f.walker({2.5f, 0, 5.5f}); // spawned on the same spot
    f.world.moveTo(a, {2.5f, 0, 7.5f}, false);
    f.run(2);
    const auto events = f.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::Arrived) == 1);
    CHECK(count(events, WorldEvent::Kind::Blocked) == 0);
    CHECK(f.world.registry().get<Transform>(a).position.z == doctest::Approx(7.5f).epsilon(0.01));
    CHECK(f.world.registry().get<Transform>(b).position.z == doctest::Approx(5.5f));
    // A body still inside another cannot push further in: only the way out is open.
    const auto c = f.walker({5.5f, 0, 5.5f});
    const auto d = f.walker({5.6f, 0, 5.5f});
    f.world.moveTo(d, {5.4f, 0, 5.5f}, false);
    f.run(1.5);
    CHECK(f.world.registry().get<Transform>(d).position.x >= 5.6f - 1e-4f);
    CHECK(f.world.registry().get<Transform>(c).position.x == doctest::Approx(5.5f));
}

TEST_CASE("a jump interrupted by a command lands under the body and emits landed") {
    Fixture f;
    const auto e = f.walker({2.5f, 0, 2.5f});
    f.world.jump(e, {5.5f, 0, 2.5f}, 1, 1, 0);
    f.run(0.3);
    const auto& t = f.world.registry().get<Transform>(e);
    REQUIRE(t.position.y > 0.5f);
    f.world.moveTo(e, {7.5f, 0, 3.5f}, true); // click-to-move in mid-air
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(t.position.x == doctest::Approx(3.4f).epsilon(0.01));
    CHECK(kinds(f.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    f.run(6);
    CHECK(t.position.x == doctest::Approx(7.5f).epsilon(0.02));
    CHECK(kinds(f.world.takeEvents()) == std::vector{WorldEvent::Kind::Arrived});
    // stop() and a plain route do the same; no command leaves the body in the air.
    f.world.jump(e, {5.5f, 0, 3.5f}, 1, 1, 0);
    f.run(0.5);
    REQUIRE(t.position.y > 0.5f);
    f.world.stop(e);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK_FALSE(f.world.moving(e));
    CHECK(kinds(f.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    f.world.jump(e, {6.5f, 0, 5.5f}, 1, 1, 0);
    f.run(0.5);
    f.world.moveTo(e, {6.5f, 0, 3.5f}, false);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(count(f.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
    f.run(3);
    CHECK(t.position.z == doctest::Approx(3.5f).epsilon(0.02));
}

TEST_CASE("a jump interrupted over a void lands at the nearer end of its arc") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"11001111", "11001111", "11001111"};
    Scratch s("void-jump", grid);
    const auto e = s.world.spawn("", centre(1, 1));
    s.world.registry().emplace<Mover>(e);
    s.world.jump(e, centre(4, 1), 1, 1, 0);
    s.step(24); // 0.4 s: over the gap, nearer the take-off
    const auto& t = s.world.registry().get<Transform>(e);
    REQUIRE(t.position.x > 2.2f);
    s.world.stop(e);
    CHECK(t.position == centre(1, 1));
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
    s.world.jump(e, centre(4, 1), 1, 1, 0);
    s.step(42); // 0.7 s: nearer the landing
    s.world.stop(e);
    CHECK(t.position == centre(4, 1));
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
}

TEST_CASE("a climb interrupted part-way steps off at the nearer end, on that end's layer") {
    // A ladder from the ground up a 4-unit face onto layer 1.
    Json grid = flatGrid(5, 4);
    grid["layers"][0]["walk"] = {"00000", "11111", "11111", "11111"};
    grid["layers"].push_back({{"walk", {"11111", "00000", "00000", "00000"}}, {"height_fill", 4}});
    grid["links"] = {{{"x", 2}, {"z", 1}, {"from", 0}, {"to", 1}, {"dir", "n"}, {"kind", "ladder"}}};
    const auto climbUntil = [&](Scratch& s, entt::entity e, float height) {
        for (int i = 0; i < 1200; ++i) {
            s.step();
            const auto& mover = s.world.registry().get<Mover>(e);
            if (mover.motion() == Motion::Climb &&
                s.world.registry().get<Transform>(e).position.y >= height) {
                return true;
            }
        }
        return false;
    };
    Scratch s("ladder-interrupt", grid);
    const auto e = s.world.spawn("", centre(0, 2));
    s.world.registry().emplace<Mover>(e).climbSpeed = 2;
    s.world.moveTo(e, centre(4, 0, 4), true, 1);
    REQUIRE(climbUntil(s, e, 3));
    const auto& t = s.world.registry().get<Transform>(e);
    // A target outside the grid is refused before the climb is touched.
    CHECK_THROWS(s.world.moveTo(e, {40, 0, 40}, true));
    CHECK(s.world.registry().get<Mover>(e).motion() == Motion::Climb);
    s.world.moveTo(e, centre(0, 0, 4), true, 1);
    CHECK(t.layer == 1);
    CHECK(t.position.y == doctest::Approx(4));
    CHECK(t.position == centre(2, 0, 4));
    auto events = s.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::ClimbStart) == 1);
    CHECK(count(events, WorldEvent::Kind::ClimbEnd) == 1);
    s.step(240);
    CHECK(t.position.x == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(t.layer == 1);
    // Interrupted near the foot: back on the ground where the climb began.
    s.world.place(e, centre(0, 2), 0);
    s.world.moveTo(e, centre(4, 0, 4), true, 1);
    REQUIRE(climbUntil(s, e, 0.5f));
    REQUIRE(t.position.y < 2);
    s.world.stop(e);
    CHECK(t.layer == 0);
    CHECK(t.position == centre(2, 1));
    CHECK_FALSE(s.world.moving(e));
    events = s.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::ClimbEnd) == 1);
}

TEST_CASE("platform carry stops at static boxes and only moves this tick's riders") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"11111111", "10000001", "11111111"};
    Scratch s("raft-quay", grid);
    auto& registry = s.world.registry();
    s.world.map().colliders.push_back({{4, 0, 1}, {4.5f, 2, 2}}); // a quay post in the water
    s.world.rebuildStatic();
    const auto raft = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Platform>(raft);
    registry.emplace<Mover>(raft, Mover{.speed = 1, .collide = false});
    s.step();
    const auto rider = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Mover>(rider);
    s.world.moveTo(raft, centre(6, 1, 0.2f), false);
    s.step(400);
    CHECK(registry.get<Transform>(raft).position.x == doctest::Approx(6.5f).epsilon(0.01));
    CHECK(registry.get<Transform>(rider).position.x == doctest::Approx(3.8f).epsilon(0.01)); // held at the box
    // A rider that steps ashore in the tick the raft leaves is not dragged along.
    Scratch q("raft-ashore", grid);
    auto& r2 = q.world.registry();
    const auto boat = q.world.spawn("", centre(1, 1, 0));
    r2.emplace<Platform>(boat);
    r2.emplace<Mover>(boat, Mover{.speed = 1, .collide = false});
    q.step();
    const auto hero = q.world.spawn("", centre(1, 1, 0));
    r2.emplace<Mover>(hero).speed = 60; // ashore in one tick
    q.world.moveTo(hero, centre(0, 1), false);
    q.world.moveTo(boat, centre(6, 1, 0), false);
    q.step();
    CHECK(r2.get<Transform>(hero).position.x == doctest::Approx(0.5f));
    q.step(60);
    CHECK(r2.get<Transform>(hero).position.x == doctest::Approx(0.5f));
}

TEST_CASE("cell_enter reports a mover's new cell, including layer changes") {
    Fixture f;
    const auto e = f.walker({2.5f, 0, 5.5f});
    f.run(2.0 / 60);
    CHECK(cellsEntered(f.world.takeEvents(), e).empty()); // the first cell is not an entry
    f.world.moveTo(e, {4.5f, 0, 5.5f}, false);
    f.run(2);
    CHECK(cellsEntered(f.world.takeEvents(), e) == std::vector<GridPos>{{3, 5, 0}, {4, 5, 0}});
    f.world.place(e, {7.5f, 1, 7.5f}, 1);
    f.run(1.0 / 60);
    CHECK(cellsEntered(f.world.takeEvents(), e) == std::vector<GridPos>{{7, 7, 1}});
    f.run(0.5);
    CHECK(cellsEntered(f.world.takeEvents(), e).empty());
}

TEST_CASE("a two-cell footprint paths and stands only where all its cells fit") {
    Json grid = flatGrid(6, 3);
    grid["layers"][0]["walk"] = {"111111", "111011", "111111"};
    Scratch s("footprint", grid);
    const auto e = s.world.spawn("", centre(0, 0));
    auto& mover = s.world.registry().emplace<Mover>(e);
    mover.footprint = 2;
    // Anchored at (2, 0) or (2, 1) the body would cover the wall at (3, 1).
    CHECK_FALSE(s.world.canOccupy(e, centre(2, 0), 0));
    CHECK_FALSE(s.world.canOccupy(e, centre(2, 1), 0));
    CHECK(s.world.canOccupy(e, centre(1, 0), 0));
    CHECK_FALSE(s.world.canOccupy(e, centre(4, 2), 0)); // hangs over the grid's edge
    s.world.moveTo(e, centre(4, 0), true);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked});
    s.world.grid().at(3, 1).walkable = true;
    s.world.moveTo(e, centre(4, 0), true);
    s.step(240);
    CHECK(s.world.registry().get<Transform>(e).position.x == doctest::Approx(4.5f).epsilon(0.01));
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Arrived});
}

TEST_CASE("flying movers path and stand over unwalkable cells but not blocked tags") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"11001111", "11001111", "11001111"};
    grid["tags"] = {"water"};
    Scratch s("fly", grid);
    const auto e = s.world.spawn("", centre(0, 1));
    auto& mover = s.world.registry().emplace<Mover>(e);
    CHECK_FALSE(s.world.canOccupy(e, centre(2, 1), 0));
    mover.fly = true;
    CHECK(s.world.canOccupy(e, centre(2, 1), 0));
    CHECK_FALSE(s.world.canOccupy(e, centre(-1, 1), 0));
    s.world.moveTo(e, centre(6, 1), true);
    s.step(300);
    const auto& t = s.world.registry().get<Transform>(e);
    CHECK(t.position.x == doctest::Approx(6.5f).epsilon(0.01));
    CHECK(t.position.y == doctest::Approx(0));
    auto events = s.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::Arrived) == 1);
    CHECK(count(events, WorldEvent::Kind::JumpStart) == 0);
    // Rules passed to move_to plan the route; blocked tags hold for flyers.
    for (int z = 0; z < 3; ++z) {
        s.world.grid().at(2, z).tags = s.world.grid().at(3, z).tags = 1;
    }
    StepRules rules;
    rules.blockedTags = 1;
    s.world.moveTo(e, centre(0, 1), true, std::nullopt, &rules);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked});
    rules.blockedTags = 0;
    s.world.moveTo(e, centre(0, 1), true, std::nullopt, &rules);
    s.step(300);
    CHECK(t.position.x == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("move_to plans with the rules it is given") {
    Fixture f;
    const auto e = f.walker({1.5f, 0, 7.5f});
    const auto water = f.world.grid().tagMask({"water"});
    REQUIRE(water != 0);
    StepRules rules;
    rules.blockedTags = water;
    f.world.moveTo(e, {4.5f, 0, 7.5f}, true, std::nullopt, &rules);
    f.run(4);
    for (const auto cell : cellsEntered(f.world.takeEvents(), e)) {
        CHECK((f.world.grid().at(cell).tags & water) == 0u);
    }
    CHECK(f.world.registry().get<Transform>(e).position.x == doctest::Approx(4.5f).epsilon(0.02));
    // Without rules the mover walks straight through the water cells.
    f.world.moveTo(e, {1.5f, 0, 7.5f}, true);
    f.run(3);
    const auto cells = cellsEntered(f.world.takeEvents(), e);
    CHECK(std::any_of(cells.begin(), cells.end(),
                      [&](GridPos cell) { return f.world.grid().at(cell).tags & water; }));
}

TEST_CASE("jumps do not land inside boxes or bodies: the landing moves or the jump is refused") {
    Scratch s("landing", flatGrid(8, 8));
    s.world.map().colliders.push_back({{5, 0, 2}, {6, 2, 3}});
    s.world.rebuildStatic();
    const auto e = s.world.spawn("", centre(2, 2));
    s.world.registry().emplace<Mover>(e);
    s.world.jump(e, centre(5, 2), 1, 0.4f, 0);
    s.step(40);
    const auto& t = s.world.registry().get<Transform>(e);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
    CHECK(s.world.canOccupy(e, t.position, 0));
    CHECK(glm::length(t.position - centre(5, 2)) <= s.world.movementTuning().landingSearch + 1e-4f);
    CHECK(glm::length(t.position - centre(5, 2)) >= 0.25f - 1e-4f);
    // A body on the landing spot moves the landing beside it.
    const auto other = s.world.spawn("", centre(2, 5));
    s.world.registry().emplace<Collider>(other);
    s.world.jump(e, centre(2, 5), 1, 0.4f, 0);
    s.step(40);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
    CHECK(s.world.canOccupy(e, t.position, 0));
    CHECK(glm::length(t.position - centre(2, 5)) >= 0.25f - 1e-4f);
    // No free spot within reach: refused with `blocked`, no take-off.
    s.world.movementTuning().landingSearch = 0;
    s.world.jump(e, centre(5, 2), 1, 0.4f, 0);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked});
    CHECK_FALSE(s.world.moving(e));
    // A route jump onto a box is refused the same way.
    Json gap = flatGrid(8, 3);
    gap["layers"][0]["walk"] = {"11011111", "11011111", "11011111"};
    Scratch r("landing-route", gap);
    r.world.map().colliders.push_back({{3, 0, 1}, {4, 2, 2}});
    r.world.rebuildStatic();
    r.world.movementTuning().landingSearch = 0;
    const auto g = r.world.spawn("", centre(1, 1));
    r.world.registry().emplace<Mover>(g).jump = {1, 0.6f, 0.6f, false};
    r.world.moveTo(g, centre(5, 1), true);
    r.step(120);
    CHECK(kinds(r.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked});
    CHECK(r.world.registry().get<Transform>(g).position.x < 2);
}

TEST_CASE("the spatial hash follows place, destroy and editor edits without a per-tick rebuild") {
    Fixture f;
    const auto post = f.world.spawn("", {4.5f, 0, 3.5f});
    f.world.registry().emplace<Collider>(post);
    const auto b = f.world.spawn("", {2.5f, 0, 3.5f});
    f.world.registry().emplace<Mover>(b);
    CHECK_FALSE(f.world.canOccupy(b, {4.5f, 0, 3.5f}, 0));
    f.world.place(post, {8.5f, 0, 8.5f}, 0); // another hash bucket
    CHECK(f.world.canOccupy(b, {4.5f, 0, 3.5f}, 0));
    CHECK_FALSE(f.world.canOccupy(b, {8.5f, 0, 8.5f}, 0));
    f.run(3.0 / 60);
    CHECK(f.world.canOccupy(b, {4.5f, 0, 3.5f}, 0));
    CHECK_FALSE(f.world.canOccupy(b, {8.5f, 0, 8.5f}, 0));
    // The editor writes the transform directly and asks for a refresh.
    f.world.registry().get<Transform>(post).position = {2.5f, 0, 8.5f};
    f.world.refreshCollision();
    CHECK_FALSE(f.world.canOccupy(b, {2.5f, 0, 8.5f}, 0));
    CHECK(f.world.canOccupy(b, {8.5f, 0, 8.5f}, 0));
    f.world.rebuildStatic(); // the editor's path after a map edit refreshes too
    CHECK_FALSE(f.world.canOccupy(b, {2.5f, 0, 8.5f}, 0));
    f.world.destroy(post);
    CHECK(f.world.canOccupy(b, {2.5f, 0, 8.5f}, 0));
    // Moving movers keep their buckets current through the tick.
    const auto walker = f.walker({2.5f, 0, 6.5f}); // row 6 is open all the way (row 5 has a wall at x = 5)
    f.world.moveTo(walker, {7.5f, 0, 6.5f}, false);
    f.run(3);
    CHECK_FALSE(f.world.canOccupy(b, {7.5f, 0, 6.5f}, 0));
    CHECK(f.world.canOccupy(b, {2.5f, 0, 6.5f}, 0));
}

TEST_CASE("entity collisions honour collider heights") {
    Fixture f;
    const auto low = f.world.spawn("", {4.5f, 0, 3.5f});
    f.world.registry().emplace<Collider>(low, Collider{0.25f, 0.5f, true}); // a crate half a unit tall
    const auto b = f.world.spawn("", {2.5f, 0, 3.5f});
    f.world.registry().emplace<Mover>(b);
    f.world.registry().emplace<Collider>(b);
    CHECK_FALSE(f.world.canOccupy(b, {4.5f, 0, 3.5f}, 0));
    CHECK(f.world.canOccupy(b, {4.5f, 0.6f, 3.5f}, 0)); // standing above the crate
    CHECK_FALSE(f.world.canOccupy(b, {4.5f, 0.4f, 3.5f}, 0));
    // A tall body under a bridge deck blocks a walker on the deck.
    const auto tall = f.world.spawn("", {7.5f, 0, 7.5f});
    f.world.registry().emplace<Collider>(tall, Collider{0.25f, 1.6f, true});
    CHECK_FALSE(f.world.canOccupy(b, {7.5f, 1, 7.5f}, 1));
    f.world.registry().patch<Collider>(tall, [](Collider& c) { c.height = 0.9f; });
    CHECK(f.world.canOccupy(b, {7.5f, 1, 7.5f}, 1));
}

TEST_CASE("the walk clip follows a mover's own motion, not a platform carrying it") {
    Json grid = flatGrid(8, 3);
    grid["layers"][0]["walk"] = {"10000001", "10000001", "10000001"};
    Scratch s("raft-anim", grid, {{"sprites/hero.anim.json", SpriteJson}});
    auto& registry = s.world.registry();
    const auto raft = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Platform>(raft);
    registry.emplace<Mover>(raft, Mover{.speed = 1, .collide = false});
    s.step();
    const auto rider = s.world.spawn("", centre(1, 1, 0.2f));
    registry.emplace<Mover>(rider);
    Sprite sprite;
    sprite.animation = "sprites/hero.anim.json";
    sprite.clip = "idle";
    sprite.moveClip = "walk";
    sprite.idleClip = "idle";
    registry.emplace<Sprite>(rider, sprite);
    s.world.moveTo(raft, centre(5, 1, 0.2f), false);
    s.step(60);
    CHECK(registry.get<Transform>(rider).position.x > 2);
    CHECK(registry.get<Sprite>(rider).clip == "idle");
    s.world.moveTo(rider, {registry.get<Transform>(raft).position.x + 0.3f, 0.2f, 1.5f}, false);
    s.step();
    CHECK(registry.get<Sprite>(rider).clip == "walk");
}

TEST_CASE("attachments follow the parent's yaw and are placed after their parent") {
    Fixture f;
    const auto parent = f.world.spawn("", {2.5f, 0, 2.5f});
    const auto child = f.world.spawn("", {0, 0, 0});
    const auto grandchild = f.world.spawn("", {0, 0, 0});
    auto& link = f.world.registry().get<Transform>(child);
    link.parent = parent;
    link.offset = {1, 0, 0};
    auto& deeper = f.world.registry().get<Transform>(grandchild);
    deeper.parent = child;
    deeper.offset = {0, 1, 0};
    f.world.registry().get<Transform>(parent).yaw = glm::half_pi<float>();
    f.run(1.0 / 60);
    const auto p = f.world.registry().get<Transform>(child).position;
    CHECK(p.x == doctest::Approx(2.5f));
    CHECK(p.z == doctest::Approx(1.5f)); // a quarter turn swings +x towards -z
    CHECK(f.world.registry().get<Transform>(grandchild).position == glm::vec3(p.x, 1, p.z));
    f.world.place(parent, {5.5f, 0, 5.5f}, 0);
    f.run(1.0 / 60);
    const auto q = f.world.registry().get<Transform>(child).position;
    CHECK(f.world.registry().get<Transform>(grandchild).position == glm::vec3(q.x, 1, q.z));
}

TEST_CASE("an orthographic camera keeps its framing at the target, casts parallel rays and flags points "
          "behind it") {
    Camera camera;
    camera.target = {8, 1, 6};
    camera.yaw = 25;
    camera.pitch = 50;
    camera.distance = 20;
    camera.fov = 40;
    camera.snap();
    // The derived height is what the perspective view covers at the target,
    // so a point beside the target projects to the same place either way.
    const auto beside = camera.target + camera.right() * 3.0f;
    const auto perspective = camera.project(beside, 1.5f);
    camera.orthographic = true;
    camera.snap();
    CHECK(camera.effectiveOrthoHeight() ==
          doctest::Approx(2 * 20 * std::tan(glm::radians(20.0f))).epsilon(1e-4));
    CHECK(camera.viewHeight() == doctest::Approx(camera.effectiveOrthoHeight()));
    const auto orthographic = camera.project(beside, 1.5f);
    CHECK(orthographic.x == doctest::Approx(perspective.x).epsilon(1e-3));
    CHECK(orthographic.y == doctest::Approx(perspective.y).epsilon(1e-3));
    CHECK(camera.project(camera.target, 1.5f).z <= 1);
    // Rays through different pixels are parallel and run along the camera's forward.
    glm::vec3 originA, directionA, originB, directionB;
    camera.ray({-0.5f, 0.2f}, 1.5f, originA, directionA);
    camera.ray({0.7f, -0.6f}, 1.5f, originB, directionB);
    CHECK(glm::length(directionA - directionB) < 1e-4f);
    CHECK(glm::dot(directionA, camera.forward()) == doctest::Approx(1).epsilon(1e-4));
    CHECK(glm::length(originA - originB) > 1);
    // The ray through a projected point passes through it.
    const auto ndc = camera.project(beside, 1.5f);
    camera.ray(glm::vec2(ndc), 1.5f, originA, directionA);
    const auto toPoint = beside - originA;
    CHECK(glm::length(toPoint - directionA * glm::dot(toPoint, directionA)) < 1e-3f);
    // A point behind the eye has no vanishing point but is still flagged.
    CHECK(camera.project(camera.position() - camera.forward() * 2.0f, 1.5f).z > 1);
    // An explicit height covers that many units vertically: a point half a
    // height above the target sits on the top edge.
    camera.orthoHeight = 12;
    camera.snap();
    CHECK(camera.effectiveOrthoHeight() == doctest::Approx(12));
    const auto up = glm::normalize(glm::cross(camera.right(), camera.forward()));
    CHECK(camera.project(camera.target + up * 6.0f, 1.5f).y == doctest::Approx(1).epsilon(1e-3));
    // The height interpolates like the other values.
    camera.beginTick();
    camera.orthoHeight = 24;
    CHECK(camera.viewHeight(0.5) == doctest::Approx(18));
    CHECK(camera.viewHeight(1) == doctest::Approx(24));
}

TEST_CASE("mover tags are a passability class: boats keep to water and walkers off lava") {
    // A river (x = 3..4, walkable, tagged water) and a lava cell at (6, 1).
    Json grid = flatGrid(8, 3);
    grid["tags"] = {"water", "lava"};
    grid["layers"][0]["tags"] = {"0 0 0 1 1 0 0 0", "0 0 0 1 1 0 2 0", "0 0 0 1 1 0 0 0"};
    Scratch s("tags", grid);
    const auto water = s.world.grid().tagMask({"water"});
    const auto lava = s.world.grid().tagMask({"lava"});
    const auto boat = s.world.spawn("", centre(3, 0));
    auto& hull = s.world.registry().emplace<Mover>(boat);
    hull.fly = true;
    hull.requiredTags = water;
    CHECK(s.world.canOccupy(boat, centre(4, 1), 0));
    CHECK_FALSE(s.world.canOccupy(boat, centre(2, 1), 0));
    CHECK_FALSE(s.world.canOccupy(boat, centre(5, 1), 0));
    // move_to plans with the mover's class: along the river, never onto land.
    s.world.moveTo(boat, centre(4, 2), true);
    s.step(240);
    const auto& t = s.world.registry().get<Transform>(boat);
    CHECK(t.position.z == doctest::Approx(2.5f).epsilon(0.01));
    auto events = s.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::Arrived) == 1);
    for (const auto cell : cellsEntered(events, boat)) {
        CHECK((s.world.grid().at(cell).tags & water) != 0u);
    }
    s.world.moveTo(boat, centre(5, 1), true);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked});
    // Steering onto the bank is refused too.
    drive(s, boat, {2, 0}, 60);
    CHECK(t.position.x < 5 - hull.radius + 1e-4f);
    // A walker that may not enter lava paths around it; the same walker with
    // explicit rules plans with those instead of its class.
    const auto walker = s.world.spawn("", centre(0, 1));
    auto& legs = s.world.registry().emplace<Mover>(walker);
    legs.blockedTags = lava;
    CHECK_FALSE(s.world.canOccupy(walker, centre(6, 1), 0));
    s.world.moveTo(walker, centre(7, 1), true);
    s.step(400);
    events = s.world.takeEvents();
    CHECK(count(events, WorldEvent::Kind::Arrived) == 1);
    for (const auto cell : cellsEntered(events, walker)) {
        CHECK((s.world.grid().at(cell).tags & lava) == 0u);
    }
    legs.blockedTags = water;
    s.world.moveTo(walker, centre(0, 1), true);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Blocked}); // the river bars the way
    StepRules rules;
    rules.diagonal = true;
    s.world.moveTo(walker, centre(0, 1), true, std::nullopt, &rules);
    CHECK(legs.motion() == Motion::Route); // planned straight across; standing still obeys the class
    CHECK(s.world.takeEvents().empty());
}

TEST_CASE("launches fly ballistic arcs under gravity, fall, and land with events") {
    Scratch s("launch", flatGrid(12, 4));
    const auto e = s.world.spawn("", centre(1, 1));
    s.world.registry().emplace<Mover>(e);
    const auto& t = s.world.registry().get<Transform>(e);
    s.world.launch(e, {4, 4, 0}); // 0.4 s in the air at g = 20, 1.6 units along x
    s.step(6);
    REQUIRE(t.position.y == doctest::Approx(0.3f)); // 0.1 s: 4 x 0.1 - 10 x 0.01
    CHECK(s.world.moving(e));
    CHECK(t.facing.x == doctest::Approx(1));
    s.step(40);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(t.position.x == doctest::Approx(3.1f).epsilon(0.03));
    CHECK(t.position.z == doctest::Approx(1.5f));
    CHECK_FALSE(s.world.moving(e));
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    // A zero velocity above the ground is a fall.
    s.world.place(e, centre(5, 1, 3), 0, true);
    s.world.launch(e, {0, 0, 0});
    s.step(20);
    CHECK(t.position.y > 0.5f);
    s.step(40);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(t.position.x == doctest::Approx(5.5f));
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    // Weaker gravity keeps it up longer.
    s.world.movementTuning().gravity = 5;
    s.world.launch(e, {0, 4, 0});
    s.step(40);
    CHECK(t.position.y > 0.5f);
    s.step(80);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    s.world.movementTuning().gravity = 20;
    // A launch during a jump or a launch replaces it from where the body is.
    s.world.jump(e, centre(9, 1), 1, 1, 0);
    s.step(12);
    REQUIRE(t.position.y > 0.3f);
    s.world.launch(e, {0, 2, 0});
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::JumpStart});
    const float x = t.position.x;
    s.step(60);
    CHECK(t.position.x == doctest::Approx(x));
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::Landed});
    // A command during a launch lands it under the body at once.
    s.world.launch(e, {3, 5, 0});
    s.step(10);
    REQUIRE(t.position.y > 0.5f);
    s.world.stop(e);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK_FALSE(s.world.moving(e));
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
}

TEST_CASE("launched bodies stop at boxes, cliff faces and the grid's edge and drop straight down") {
    Json grid = flatGrid(12, 4);
    grid["layers"][0]["height"] = {"0 0 0 0 0 0 0 0 2 2 2 2", "0 0 0 0 0 0 0 0 2 2 2 2", "0 0 0 0 0 0 0 0 2 2 2 2",
                                   "0 0 0 0 0 0 0 0 2 2 2 2"};
    Scratch s("launch-walls", grid);
    s.world.map().colliders.push_back({{4, 0, 0}, {5, 3, 1}});
    s.world.rebuildStatic();
    const auto e = s.world.spawn("", centre(2, 0));
    s.world.registry().emplace<Mover>(e);
    const auto& t = s.world.registry().get<Transform>(e);
    // Into the box: the flight would end inside it; the body drops in front of it instead.
    s.world.launch(e, {6, 3, 0});
    s.step(60);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(t.position.x < 4);
    CHECK(t.position.x > 3.5f);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    // Into the cliff face at x = 8: a low arc stops at the face, a high one lands on top.
    s.world.place(e, centre(6, 2), 0);
    s.world.launch(e, {8, 2, 0}); // apex 0.1: well under the 2-unit ledge
    s.step(60);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(t.position.x < 8);
    CHECK(t.position.x > 7.5f);
    s.world.place(e, centre(6, 2), 0);
    s.world.launch(e, {3, 10, 0}); // 2.5 up by the time it reaches the face: clears the ledge
    s.step(90);
    CHECK(t.position.y == doctest::Approx(2));
    CHECK(t.position.x > 8);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 2);
    // Off the top towards the grid's edge: it stops at the edge and falls onto the ledge.
    s.world.launch(e, {40, 1, 0});
    s.step(60);
    CHECK(t.position.y == doctest::Approx(2));
    CHECK(t.position.x < 12);
    CHECK(t.position.x > 11);
    // A solid body in the way stops it the same way.
    const auto other = s.world.spawn("", centre(6, 2));
    s.world.registry().emplace<Collider>(other);
    s.world.place(e, centre(3, 2), 0);
    s.world.launch(e, {6, 3, 0});
    s.step(60);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(t.position.x < 6.5f - 0.45f + 1e-3f);
    CHECK(t.position.x > 5);
    CHECK(s.world.canOccupy(e, t.position, 0));
}

TEST_CASE("launches land on the first standable surface, on the floor over a void, and validate the spot") {
    // A bridge deck (layer 1, height 2) over x = 4..6; the ground under x = 3 is a chasm.
    Json grid = flatGrid(10, 3);
    grid["layers"][0]["walk"] = {"1110111111", "1110111111", "1110111111"};
    grid["layers"].push_back({{"walk", {"0000111000", "0000111000", "0000111000"}}, {"height_fill", 2}});
    Scratch s("launch-layers", grid);
    const auto e = s.world.spawn("", centre(5, 1, 4));
    s.world.registry().emplace<Mover>(e);
    const auto& t = s.world.registry().get<Transform>(e);
    // A fall from above the deck lands on the deck, not the ground under it.
    s.world.place(e, centre(5, 1, 4), 0, true);
    s.world.launch(e, {0, 0, 0});
    s.step(60);
    CHECK(t.layer == 1);
    CHECK(t.position.y == doctest::Approx(2));
    // Launched past the deck's end it lands on the ground.
    s.world.launch(e, {4, 4, 0});
    s.step(90);
    CHECK(t.layer == 0);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(t.position.x > 7);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 2);
    // Over the chasm there is nothing to stand on: it lands on the lowest layer's surface anyway.
    s.world.place(e, centre(1, 1), 0);
    s.world.launch(e, {4, 5, 0}); // 0.5 s, 2 units: comes down inside x = 3
    s.step(45);
    CHECK(t.layer == 0);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(floorCell(t.position.x) == 3);
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    // A landing inside a solid body moves beside it; with no room it lands anyway and says blocked.
    const auto other = s.world.spawn("", centre(8, 1));
    s.world.registry().emplace<Collider>(other);
    s.world.place(e, centre(8, 1, 3), 0, true);
    s.world.launch(e, {0, 0, 0});
    s.step(60);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(glm::length(t.position - centre(8, 1)) >= 0.25f - 1e-4f);
    CHECK(s.world.canOccupy(e, t.position, 0));
    CHECK(kinds(s.world.takeEvents()) == std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed});
    s.world.movementTuning().landingSearch = 0;
    s.world.place(e, centre(8, 1, 3), 0, true);
    s.world.launch(e, {0, 0, 0});
    s.step(60);
    CHECK(t.position == centre(8, 1));
    CHECK(kinds(s.world.takeEvents()) ==
          std::vector{WorldEvent::Kind::JumpStart, WorldEvent::Kind::Landed, WorldEvent::Kind::Blocked});
    // A boat's class applies to landings too: a ground unit falling onto
    // water it may not stand on reaches the floor all the same.
    const auto water = s.world.grid().tagBit("water", true);
    s.world.grid().at(1, 1).tags = water;
    s.world.registry().get<Mover>(e).blockedTags = water;
    s.world.place(e, centre(1, 1, 2), 0, true);
    s.world.launch(e, {0, 0, 0});
    s.step(60);
    CHECK(t.position.y == doctest::Approx(0));
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
}

TEST_CASE("a launch during a ladder climb is refused") {
    Json grid = flatGrid(5, 4);
    grid["layers"][0]["height"] = {"4 4 4 4 4", "0 0 0 0 0", "0 0 0 0 0", "0 0 0 0 0"};
    grid["links"] = {{{"x", 2}, {"z", 1}, {"from", 0}, {"to", 0}, {"dir", "n"}, {"kind", "ladder"}}};
    Scratch s("launch-climb", grid);
    const auto e = s.world.spawn("", centre(2, 1));
    s.world.registry().emplace<Mover>(e).climbSpeed = 1;
    s.world.moveTo(e, centre(2, 0, 4), true);
    bool climbing = false;
    for (int i = 0; i < 300 && !climbing; ++i) {
        s.step();
        climbing = s.world.registry().get<Mover>(e).motion() == Motion::Climb &&
                   s.world.registry().get<Transform>(e).position.y > 1;
    }
    REQUIRE(climbing);
    CHECK_THROWS_AS(s.world.launch(e, {0, 1, 0}), std::runtime_error);
    CHECK(s.world.registry().get<Mover>(e).motion() == Motion::Climb);
}

namespace {
void steer(Fixture& f, entt::entity e, glm::vec2 velocity, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        f.world.beginTick();
        f.world.registry().get<Mover>(e).drive = velocity;
        f.world.tick(1.0 / 60);
    }
}
} // namespace

TEST_CASE("a mover's lift eases towards its altitude, place keeps it and ramps raise the lifted body") {
    Fixture f;
    auto& registry = f.world.registry();
    const auto e = f.walker({2.5f, 0, 3.5f});
    auto& mover = registry.get<Mover>(e);
    const auto& t = registry.get<Transform>(e);
    mover.altitude = 2;
    mover.altitudeSpeed = 4; // half a second up
    f.run(0.25);
    CHECK(mover.lift == doctest::Approx(1).epsilon(0.02));
    CHECK(t.position.y == doctest::Approx(1).epsilon(0.02));
    CHECK_FALSE(mover.moved); // hovering is not a motion
    CHECK_FALSE(f.world.moving(e));
    f.run(0.5);
    CHECK(mover.lift == doctest::Approx(2));
    CHECK(t.position.y == doctest::Approx(2));
    // place rests the body on the surface plus its lift, and the ramp at x = 7
    // raises the lifted body with the ground.
    f.world.place(e, {6.5f, 0, 3.0f}, 0);
    CHECK(t.position.y == doctest::Approx(2));
    CHECK(t.previous.y == doctest::Approx(2));
    steer(f, e, {2, 0}, 60);
    CHECK(t.position.x > 7.5f);
    CHECK(t.position.y == doctest::Approx(3).epsilon(0.01)); // the plateau's 1 plus the lift
    // An altitude speed of 0 applies at once, down as well as up.
    mover.altitudeSpeed = 0;
    mover.altitude = 0.5f;
    f.run(1.0 / 60);
    CHECK(mover.lift == doctest::Approx(0.5f));
    CHECK(t.position.y == doctest::Approx(1.5f));
    // A non-colliding mover is lifted the same way: idle, steered, routed or placed.
    const auto ghost = f.world.spawn("", {3.5f, 0, 5.5f});
    registry.emplace<Mover>(ghost, Mover{.collide = false, .altitude = 1.5f});
    const auto& g = registry.get<Transform>(ghost);
    f.run(1.0 / 60);
    CHECK(g.position.y == doctest::Approx(1.5f));
    steer(f, ghost, {2, 0}, 30);
    CHECK(g.position.x > 4.3f);
    CHECK(g.position.y == doctest::Approx(1.5f));
    f.world.moveTo(ghost, {5.5f, 0, 5.5f}, false);
    f.run(1);
    CHECK(g.position.x == doctest::Approx(5.5f).epsilon(0.02));
    CHECK(g.position.y == doctest::Approx(1.5f));
    f.world.place(ghost, {2.5f, 0, 5.5f}, 0);
    CHECK(g.position.y == doctest::Approx(1.5f));
}

TEST_CASE("a lifted body passes over boxes and bodies below its feet and still reports its cells") {
    Fixture f;
    auto& registry = f.world.registry();
    f.world.map().colliders.push_back({{3, 0, 6}, {3.5f, 1, 7}}); // a low wall across the row
    f.world.rebuildStatic();
    const auto tall = f.world.spawn("", {6.5f, 0, 6.5f});
    registry.emplace<Collider>(tall, Collider{0.25f, 1.6f, true});
    const auto e = f.walker({1.5f, 0, 6.5f});
    auto& mover = registry.get<Mover>(e);
    const auto& t = registry.get<Transform>(e);
    steer(f, e, {2, 0}, 90);
    CHECK(t.position.x < 2.9f); // on the ground the wall stops it
    f.world.place(e, {1.5f, 0, 6.5f}, 0);
    mover.altitude = 1.2f; // over the wall's top, under the tall body's
    steer(f, e, {2, 0}, 150);
    CHECK(t.position.x > 4);
    CHECK(t.position.x < 6.3f); // held by the tall body
    CHECK(t.position.y == doctest::Approx(1.2f));
    f.world.place(e, {1.5f, 0, 6.5f}, 0);
    mover.altitude = 2;
    f.world.takeEvents();
    steer(f, e, {2, 0}, 180);
    CHECK(t.position.x > 7);
    CHECK(t.position.y == doctest::Approx(2));
    // The place reports its cell, then every cell flown over.
    CHECK(cellsEntered(f.world.takeEvents(), e) ==
          std::vector<GridPos>{{1, 6, 0}, {2, 6, 0}, {3, 6, 0}, {4, 6, 0}, {5, 6, 0}, {6, 6, 0}, {7, 6, 0}});
    // Lifted or not, it may stand where its feet are free.
    CHECK(f.world.canOccupy(e, {6.5f, 2, 6.5f}, 0));
    CHECK_FALSE(f.world.canOccupy(e, {6.5f, 0, 6.5f}, 0));
}

TEST_CASE("a ground attachment rests on the surface under its parent") {
    Fixture f;
    auto& registry = f.world.registry();
    const auto parent = f.world.spawn("", {8.5f, 3, 2.5f}); // three units over the plateau at (8, 2)
    const auto shadow = f.world.spawn("", {0, 0, 0});
    const auto balloon = f.world.spawn("", {0, 0, 0});
    auto& under = registry.get<Transform>(shadow);
    under.parent = parent;
    under.ground = true;
    under.offset = {0.5f, 0, 0};
    auto& above = registry.get<Transform>(balloon);
    above.parent = parent;
    above.offset = {0.5f, 0, 0};
    f.run(1.0 / 60);
    CHECK(under.position == glm::vec3(9, 1, 2.5f));
    CHECK(under.previous == glm::vec3(9, 1, 2.5f));
    CHECK(above.position == glm::vec3(9, 3, 2.5f));
    // Off the plateau the shadow follows the ground down while the parent keeps its height.
    f.world.place(parent, {5.5f, 3, 2.5f}, 0, true);
    f.run(1.0 / 60);
    CHECK(under.position == glm::vec3(6, 0, 2.5f));
    CHECK(above.position == glm::vec3(6, 3, 2.5f));
}

namespace {
Json wrapGrid(int width, int depth) {
    auto grid = flatGrid(width, depth);
    grid["wrap"] = true;
    return grid;
}
} // namespace

TEST_CASE("on a wrapping map a driven mover crosses the east seam and arrives on the west") {
    Scratch s("torus-drive", wrapGrid(12, 10));
    CHECK(s.world.grid().wraps());
    const auto e = s.world.spawn("", {11.6f, 0, 5.5f});
    s.world.registry().emplace<Mover>(e);
    s.world.registry().emplace<Collider>(e);
    drive(s, e, {3, 0}, 30); // 1.5 units east at 3 u/s
    const auto& t = s.world.registry().get<Transform>(e);
    CHECK(t.position.x == doctest::Approx(1.1f).epsilon(0.01));
    CHECK(t.position.x >= 0);
    CHECK(t.position.x < 12);
    // `previous` is the nearest representative of itself, so a frame interpolates
    // across the seam and never across the map.
    CHECK(std::abs(t.previous.x - t.position.x) < 0.1f);
    CHECK(std::abs(s.world.interpolated(e, 0.5).x - t.position.x) < 0.1f);
    // The spatial hash followed the canonical position: a body on the west side is found.
    CHECK(s.world.registry().get<Mover>(e).cell == GridPos{1, 5, 0});
    const auto spawned = s.world.spawn("", {-0.4f, 0, 2.5f}); // spawned outside the range: folded
    CHECK(s.world.registry().get<Transform>(spawned).position.x == doctest::Approx(11.6f));
    // Back west across the seam the other way.
    drive(s, e, {-3, 0}, 30);
    CHECK(t.position.x == doctest::Approx(11.6f).epsilon(0.01));
    CHECK(std::abs(t.previous.x - t.position.x) < 0.1f);
}

TEST_CASE("on a wrapping map move_to crosses the seam the short way, planned or not") {
    Scratch s("torus-move", wrapGrid(12, 10));
    const auto e = s.world.spawn("", centre(11, 5));
    s.world.registry().emplace<Mover>(e).speed = 4;
    s.world.moveTo(e, centre(1, 5), true); // 2 cells across the seam, not 10 the long way
    s.step(60);
    const auto& t = s.world.registry().get<Transform>(e);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Arrived) == 1);
    CHECK(t.position.x == doctest::Approx(1.5f).epsilon(0.01));
    s.world.moveTo(e, centre(10, 5), false); // a straight walk, 3 units west across the seam
    s.step(60);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Arrived) == 1);
    CHECK(t.position.x == doctest::Approx(10.5f).epsilon(0.01));
    // A target given past the edge names the same cell.
    s.world.moveTo(e, {12.5f, 0, 5.5f}, true);
    s.step(60);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Arrived) == 1);
    CHECK(t.position.x == doctest::Approx(0.5f).epsilon(0.01));
    // A route jump across the seam lands on the far side.
    auto& mover = s.world.registry().get<Mover>(e);
    mover.jump = {1, 0, 0, false};
    s.world.grid().at(11, 5).walkable = false; // a pit on the seam's east side
    s.world.moveTo(e, centre(10, 5), true);
    s.step(120);
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::Landed) == 1);
    CHECK(t.position.x == doctest::Approx(10.5f).epsilon(0.01));
}

TEST_CASE("on a wrapping map attachments and contacts work across the seam") {
    Scratch s("torus-attach", wrapGrid(12, 10));
    const auto parent = s.world.spawn("", {0.1f, 0, 5.5f});
    const auto child = s.world.spawn("", {0, 0, 0});
    auto& ct = s.world.registry().get<Transform>(child);
    ct.parent = parent;
    ct.offset = {-0.5f, 0, 0};
    s.step();
    CHECK(ct.position.x == doctest::Approx(11.6f)); // mirrored to the far side, canonical
    CHECK(std::abs(ct.previous.x - ct.position.x) < 0.1f);
    // A solid body just west of the seam blocks a walker coming from the east...
    const auto wall = s.world.spawn("", {0.2f, 0, 2.5f});
    s.world.registry().emplace<Collider>(wall);
    const auto walker = s.world.spawn("", {11.4f, 0, 2.5f});
    s.world.registry().emplace<Mover>(walker);
    s.world.registry().emplace<Collider>(walker);
    drive(s, walker, {3, 0}, 30);
    const auto& wt = s.world.registry().get<Transform>(walker);
    CHECK(wt.position.x > 11.4f);
    CHECK(wt.position.x < 11.9f);
    // ...and the two touch across it.
    CHECK(count(s.world.takeEvents(), WorldEvent::Kind::ContactBegin) == 1);
    CHECK(s.world.query("", glm::vec3(11.9f, 0, 2.5f), 0.5f).size() == 2);
}

TEST_CASE("on a wrapping map the camera target is folded back into the map each tick") {
    Scratch s("torus-camera", wrapGrid(12, 10));
    s.world.camera().target = {12.7f, 1, -0.5f};
    s.step();
    CHECK(s.world.camera().target.x == doctest::Approx(0.7f));
    CHECK(s.world.camera().target.z == doctest::Approx(9.5f));
    CHECK(s.world.camera().target.y == doctest::Approx(1));
    // A map that does not wrap leaves it alone.
    Scratch plain("plain-camera", flatGrid(12, 10));
    plain.world.camera().target = {12.7f, 1, -0.5f};
    plain.step();
    CHECK(plain.world.camera().target.x == doctest::Approx(12.7f));
}
