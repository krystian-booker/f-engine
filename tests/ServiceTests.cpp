#include "engine/services/Canvas.h"
#include "engine/services/InputState.h"
#include "engine/services/SaveStore.h"
#include <doctest/doctest.h>

using namespace fe;

TEST_CASE("a tap inside one tick still registers as a press") {
    InputState input;
    input.define("confirm", {"key:Z", "pad:a"});
    input.feed("key:Z", 1);
    input.feed("key:Z", 0);
    input.tick(1.0 / 60);
    CHECK(input.pressed("confirm"));
    CHECK_FALSE(input.held("confirm"));
    input.tick(1.0 / 60);
    CHECK_FALSE(input.pressed("confirm"));
}

TEST_CASE("held actions repeat after a delay; axes use the deadzone") {
    InputState input;
    input.define("down", {"key:Down", "pad:lefty+"});
    input.defineAxis("move_y", {{"key:Up"}, {"key:Down"}, {"pad:lefty"}, 0.2f});
    input.feed("key:Down", 1);
    int repeats = 0;
    for (int i = 0; i < 60; ++i) {
        input.tick(1.0 / 60);
        repeats += input.repeat("down", 0.3, 0.1) ? 1 : 0;
    }
    CHECK(repeats >= 7);
    CHECK(repeats <= 9);
    input.feed("key:Down", 0);
    input.feed("pad:lefty", 0.1f);
    input.tick(1.0 / 60);
    CHECK(input.axis("move_y") == doctest::Approx(0));
    input.feed("pad:lefty", 0.9f);
    input.tick(1.0 / 60);
    CHECK(input.axis("move_y") > 0.8f);
    CHECK(input.held("down"));
    CHECK(input.lastDevice() == "gamepad");
}

TEST_CASE("rebinding and raw capture") {
    InputState input;
    input.define("menu", {"key:Tab"});
    input.bind("menu", {"key:M"});
    input.feed("key:M", 1);
    input.tick(0.016);
    CHECK(input.pressed("menu"));
    CHECK(input.pressedInputs() == std::vector<std::string>{"key:M"});
    CHECK_THROWS(input.bind("missing", {}));
}

TEST_CASE("save slots are atomic JSON with metadata") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-test";
    std::filesystem::remove_all(folder);
    SaveStore store(folder);
    store.write("slot1", {{"gold", 10}}, {{"location", "Town"}});
    const auto loaded = store.read("slot1");
    REQUIRE(loaded);
    CHECK(loaded->data.at("gold") == 10);
    const auto slots = store.list();
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].meta.at("location") == "Town");
    CHECK_THROWS(store.write("../escape", {}, {}));
    store.remove("slot1");
    CHECK_FALSE(store.read("slot1"));
    store.writeConfig({{"volume", 0.5}});
    CHECK(store.readConfig().at("volume") == 0.5);
}

TEST_CASE("canvas records transformed, clipped quads") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_LUALIB);
    assets.defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    TextSystem text(assets);
    Canvas canvas(text, 320, 180);
    canvas.pushTransform({{10, 20}, {2, 2}, {0, 0}, 0, 0.5f});
    canvas.pushClip({0, 0, 5, 5});
    canvas.rect({0, 0, 4, 4}, 0xff0000ff);
    canvas.popClip();
    canvas.popTransform();
    REQUIRE(canvas.quads().size() == 1);
    const auto& q = canvas.quads()[0];
    CHECK(q.corners[2] == glm::vec2(18, 28));
    CHECK((q.color & 0xffu) == 127);
    CHECK(q.clip.w == doctest::Approx(10));
    const auto size = canvas.text("Hi", {0, 0}, {});
    CHECK(size.x == doctest::Approx(12));
    CHECK(canvas.quads().size() == 3);
    const auto p = canvas.present({1280, 720});
    CHECK(p.scale == doctest::Approx(4));
    CHECK(canvas.fromWindow(canvas.toWindow({100, 50}, {1280, 720}), {1280, 720}) == glm::vec2(100, 50));
}

TEST_CASE("asset preload decodes on the worker thread") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_LUALIB);
    const int ticket = assets.preload({"maps/arena/geometry.glb", "@fe/assets/fonts/pixel.png"});
    for (int i = 0; i < 200 && assets.progress(ticket) < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(assets.progress(ticket) == doctest::Approx(1));
    CHECK(assets.tryModel("maps/arena/geometry.glb"));
}
