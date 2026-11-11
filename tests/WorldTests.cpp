#include "engine/runtime/Engine.h"
#include "engine/world/World.h"
#include <doctest/doctest.h>
#include <fstream>

using namespace fe;

namespace {
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

TEST_CASE("a fixed sprite direction wraps onto clips with fewer rows") {
    // The reported case: direction 2 on a clip with a single direction row.
    Fixture f;
    Sprite spark;
    spark.animation = "@fe/assets/ui/spark.anim.json";
    spark.direction = 2;
    f.world.registry().emplace<Sprite>(f.world.spawn("", {2, 0, 2}), spark);
    CHECK_NOTHROW(f.run(0.5));

    // One sprite whose clips have different direction counts.
    const auto root = std::filesystem::temp_directory_path() / "fengine-world-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::string frame = R"({"rect": [0, 0, 8, 8], "duration": 0.1})";
    const std::string row = "[" + frame + "]";
    std::ofstream(root / "mixed.anim.json")
        << R"({"atlas": "mixed.png", "clips": {"idle": {"directions": [)" << row << "," << row << "," << row
        << "," << row << R"(]}, "ko": {"loop": false, "directions": [[)" << frame
        << R"(, {"rect": [8, 0, 8, 8], "duration": 0.1, "event": "thud"}]]}}})";
    AssetCache assets{root, FENGINE_LUALIB};
    World world{"mixed", MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json"),
                assets, 7};
    const auto e = world.spawn("", {2, 0, 2});
    Sprite sprite;
    sprite.animation = "mixed.anim.json";
    sprite.direction = 2;
    world.registry().emplace<Sprite>(e, sprite);
    REQUIRE(assets.trySprite(sprite.animation));
    world.beginTick();
    world.tick(1.0 / 60);
    world.setClip(e, "ko", true);
    for (int i = 0; i < 30; ++i) {
        world.beginTick();
        CHECK_NOTHROW(world.tick(1.0 / 60));
    }
    const auto events = world.takeEvents();
    CHECK(std::count_if(events.begin(), events.end(), [](const WorldEvent& event) {
              return event.kind == WorldEvent::Kind::AnimationEvent && event.name == "thud";
          }) == 1);
    CHECK(f.has(events, WorldEvent::Kind::AnimationFinished));
    CHECK(world.registry().get<Sprite>(e).finished);
}

TEST_CASE("a throwing world tick is reported once and does not escape the engine tick") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = std::filesystem::temp_directory_path() / "fengine-world-tick-test";
    options.seed = 99;
    Engine engine(std::move(options));
    const auto broken = engine.loadWorld("broken", "arena");
    const auto healthy = engine.loadWorld("healthy", "arena");
    // A looping clip advanced by thousands of loops in one tick trips the
    // event interval guard, which throws from World::advanceAnimations.
    Sprite sprite;
    sprite.animation = "@fe/assets/ui/spark.anim.json";
    sprite.speed = 1e6f;
    broken->registry().emplace<Sprite>(broken->spawn("", {2, 0, 2}), sprite);
    broken->beginTick();
    REQUIRE_THROWS(broken->tick(1.0 / 60));

    CHECK_NOTHROW(engine.tick(1.0 / 60));
    REQUIRE(engine.lua().errors().size() == 1);
    CHECK(engine.lua().errors().back().message.find("broken") != std::string::npos);
    CHECK(healthy->time() == doctest::Approx(1.0 / 60)); // later worlds still tick
    CHECK_NOTHROW(engine.tick(1.0 / 60));
    CHECK(engine.lua().errors().size() == 1); // the same message is not repeated
    engine.lua().clearErrors();
    CHECK_NOTHROW(engine.tick(1.0 / 60));
    CHECK(engine.lua().errors().size() == 1); // reported again once cleared
}
