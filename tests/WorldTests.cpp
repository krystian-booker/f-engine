#include "engine/editor/MapEdits.h"
#include "engine/platform/FileWatcher.h"
#include "engine/runtime/Engine.h"
#include "engine/world/Animation.h"
#include "engine/world/World.h"
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
        : root(prepare(name, files)), assets(root, FENGINE_LUALIB),
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
    AssetCache assets{FENGINE_TEST_GAME, FENGINE_LUALIB};
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
    CHECK(f.world.activeLights().size() == 2);
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
    options.libraryRoot = FENGINE_LUALIB;
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

TEST_CASE("walkers cross the Riverford bridge both ways along the planned path") {
    const auto game = std::filesystem::path(FENGINE_LUALIB).parent_path() / "samples" / "wayfarers";
    AssetCache assets(game, FENGINE_LUALIB);
    World world("riverford", MapData::load(game / "maps/riverford/map.json"), assets, 1);
    const auto e = world.spawn("", {2.5f, 0, 7.5f});
    world.registry().emplace<Mover>(e);
    for (const float x : {19.5f, 2.5f}) {
        world.moveTo(e, {x, 0, 7.5f}, true);
        bool climbed = false;
        for (int i = 0; i < 600 && world.moving(e); ++i) {
            world.beginTick();
            world.tick(1.0 / 60);
            climbed = climbed || world.registry().get<Transform>(e).layer == 1;
        }
        CHECK(climbed);
        CHECK(count(world.takeEvents(), WorldEvent::Kind::Arrived) == 1);
        CHECK(world.registry().get<Transform>(e).position.x == doctest::Approx(x).epsilon(0.02));
        CHECK(world.registry().get<Transform>(e).layer == 0);
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

TEST_CASE("maps reject placements and zones on layers the grid lacks") {
    auto map = MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json").toJson();
    auto bad = map;
    bad["placements"] = {{{"id", "a"}, {"type", "npc"}, {"position", {1, 0, 1}}, {"layer", 2}}};
    CHECK_THROWS(MapData::fromJson(bad));
    bad = map;
    bad["zones"] = {{{"id", "z"}, {"min", {0, 0}}, {"max", {1, 1}}, {"layer", -2}}};
    CHECK_THROWS(MapData::fromJson(bad));
    bad["zones"][0]["layer"] = 1;
    CHECK_NOTHROW(MapData::fromJson(bad));
}

TEST_CASE("the engine skips paused worlds and scales their time") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
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
              .console("fe.world.get('main'):path({x=2,z=2}, {x=3,z=2}, {tag_costs={water=-1}})")
              .find("non-negative") != std::string::npos);
}

TEST_CASE("the file watcher survives unreadable folders without reporting removals") {
    const auto root = std::filesystem::temp_directory_path() / "fengine-watch-test";
    std::filesystem::remove_all(root);
    write(root / "a.txt", "a");
    write(root / "sub" / "b.txt", "b");
    FileWatcher watcher(root);
    CHECK(watcher.poll().empty());
    write(root / "a.txt", "changed");
    write(root / "c.txt", "c");
    auto changes = watcher.poll();
    REQUIRE(changes.size() == 2);
    std::filesystem::permissions(root / "sub", std::filesystem::perms::none);
    changes = watcher.poll();
    std::filesystem::permissions(root / "sub", std::filesystem::perms::owner_all);
    CHECK(changes.empty());
    std::filesystem::remove(root / "c.txt");
    changes = watcher.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].kind == FileChangeKind::Removed);
    CHECK(changes[0].path.filename() == "c.txt");
}

namespace {
std::vector<WorldEvent::Kind> kinds(const std::vector<WorldEvent>& events) {
    std::vector<WorldEvent::Kind> result;
    for (const auto& e : events) {
        if (e.kind != WorldEvent::Kind::ZoneEnter && e.kind != WorldEvent::Kind::ZoneExit) {
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
    // A reach path from world:reachable carries the jump mark to the mover.
    StepRules rules;
    rules.jump = {2, 0.6f, 0.6f, false};
    const auto cells = reachable(s.world.grid(), {5, 1, 0}, 5, rules);
    std::vector<Waypoint> route;
    for (const auto& cell : reachPath(cells, {0, 1, 0})) {
        const auto found =
            std::find_if(cells.begin(), cells.end(), [&](const ReachCell& c) { return c.pos == cell; });
        route.push_back({centre(cell.x, cell.z), cell.layer, found->kind});
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
    options.libraryRoot = FENGINE_LUALIB;
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

TEST_CASE("Lua sees jump and climb marks, zoc, cost callbacks, routes and raycasts") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = std::filesystem::temp_directory_path() / "fengine-world-nav-lua";
    options.seed = 5;
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    lua.console("w = fe.world.load('main', 'arena')");
    CHECK(lua.console("#w:path({x=1,z=5}, {x=3,z=5})") == "2");
    CHECK(lua.console("#w:path({x=1,z=5}, {x=3,z=5}, {zoc = {{x=2,z=5}}})") == "4");
    CHECK(lua.console("#w:path({x=1,z=5}, {x=2,z=5}, {zoc = {{x=2,z=5}}})") == "1");
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
    CHECK(lua.console("local r = w:reachable({x=1,z=5}, 2, {jump = {distance = 1}}); "
                      "for _, c in ipairs(r) do if c.x == 3 and c.z == 5 then return c.jump end end") ==
          "true");
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
    CHECK(lua.console("raft:platform(nil); raft:platform{size = {x = 1, z = 1}}") == "");
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

TEST_CASE("Lua path and reach rules can leave ladders out") {
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
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = game / "user";
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    lua.console("w = fe.world.load('main', 'cliff')");
    CHECK(lua.console("local p = w:path({x=0,z=2}, {x=0,z=0}); return #p > 0, p.truncated") == "true	nil");
    CHECK(lua.console("return #w:path({x=0,z=2}, {x=0,z=0}, {ladders = false})") == "0");
    CHECK(lua.console("for _, c in ipairs(w:reachable({x=2,z=1}, 3, {ladders = false})) do "
                      "if c.z == 0 then return 'top' end end return 'none'") == "none");
    CHECK(lua.console("for _, c in ipairs(w:reachable({x=2,z=1}, 3)) do "
                      "if c.z == 0 then return c.climb end end") == "true");
    CHECK(engine.lua().errors().empty());
}

TEST_CASE("sprites and meshes name game shaders by validated names") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
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

TEST_CASE("game.json sets whether the game pauses when the window loses focus") {
    CHECK(GameConfig::load(FENGINE_TEST_GAME).pauseOnFocusLoss);
    const auto root = std::filesystem::temp_directory_path() / "fengine-config-focus";
    write(root / "game.json", R"({"title": "Focus", "pause_on_focus_loss": false})");
    CHECK_FALSE(GameConfig::load(root).pauseOnFocusLoss);
}
