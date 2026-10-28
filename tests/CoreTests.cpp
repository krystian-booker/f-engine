#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "engine/core/FixedStep.h"
#include "engine/core/GameConfig.h"
#include "engine/core/Json.h"
#include "engine/platform/FileWatcher.h"
#include "engine/scene/SceneStack.h"
#include <chrono>
#include <fstream>

namespace {
struct TempFolder {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("fengine-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempFolder() { std::filesystem::create_directories(path); }
    ~TempFolder() { std::filesystem::remove_all(path); }
};
}
TEST_CASE("fixed tick is independent of render cadence and bounds catch-up") {
    for (int fps : {30, 60, 144}) {
        fe::FixedStep clock;
        int ticks = 0;
        for (int i = 0; i < fps * 10; ++i) clock.advance(1.0 / fps, [&](double dt) {
            CHECK(dt == doctest::Approx(1.0 / 60)); ++ticks;
        });
        CHECK(ticks == 600);
        CHECK(clock.alpha() == doctest::Approx(0).epsilon(1e-8));
    }
    fe::FixedStep clock;
    CHECK(clock.advance(60, [](double) {}) == 8);
    CHECK(clock.alpha() < 1);
    CHECK(clock.advance(-1, [](double) {}) == 0);
    CHECK_THROWS_AS(fe::FixedStep(0), std::invalid_argument);
}
TEST_CASE("game config validates authored data") {
    auto sample = fe::GameConfig::load(FENGINE_SAMPLE_DIR);
    CHECK(sample.pixelsPerUnit == 32);
    CHECK(sample.virtualWidth == 480);
    TempFolder temp;
    CHECK_THROWS(fe::GameConfig::load(temp.path));
    fe::writeJson(temp.path / "game.json", {{"pixels_per_unit", 0}});
    CHECK_THROWS(fe::GameConfig::load(temp.path));
    fe::writeJson(temp.path / "game.json", {{"title", "Test"}});
    CHECK(fe::GameConfig::load(temp.path).title == "Test");
}
TEST_CASE("watcher detects additions modifications and removals without sleeps") {
    TempFolder temp;
    fe::FileWatcher watcher(temp.path);
    CHECK(watcher.poll().empty());
    auto file = temp.path / "map.json";
    fe::writeJson(file, {{"map", 1}});
    auto changes = watcher.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].kind == fe::FileChangeKind::Added);
    fe::writeJson(file, {{"map", "different size"}});
    changes = watcher.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].kind == fe::FileChangeKind::Modified);
    std::filesystem::remove(file);
    changes = watcher.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].kind == fe::FileChangeKind::Removed);
    CHECK(watcher.poll().empty());
}
TEST_CASE("scene overlays block update while rendering the field below") {
    struct TestScene : fe::Scene {
        TestScene(int& u, int& r) : updates(u), renders(r) {}
        int& updates; int& renders;
        void update(double) override { ++updates; }
        void render(double) override { ++renders; }
    };
    fe::SceneStack stack;
    int fieldU = 0, fieldR = 0, menuU = 0, menuR = 0;
    stack.push(std::make_unique<TestScene>(fieldU, fieldR));
    stack.update(1.0 / 60);
    stack.push(std::make_unique<TestScene>(menuU, menuR));
    stack.update(1.0 / 60);
    stack.render(0.5);
    CHECK(fieldU == 1);
    CHECK(menuU == 1);
    CHECK(fieldR == 1);
    CHECK(menuR == 1);
    stack.pop(); stack.update(1.0 / 60);
    CHECK(fieldU == 2);
    CHECK(stack.size() == 1);
}
