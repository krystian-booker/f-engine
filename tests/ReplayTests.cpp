#include "engine/assets/Image.h"
#include "engine/runtime/Engine.h"
#include "engine/runtime/ReplayHarness.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <doctest/doctest.h>

using namespace fe;

namespace {
std::filesystem::path scratch(const std::string& name) {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-replay-tests";
    std::filesystem::create_directories(folder);
    return folder / name;
}

std::filesystem::path writeReplay(const std::string& name, const Json& replay) {
    const auto file = scratch(name);
    writeJson(file, replay);
    return file;
}

std::unique_ptr<Engine> fixtureEngine() {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = scratch("user");
    options.seed = 1;
    auto engine = std::make_unique<Engine>(std::move(options));
    REQUIRE(engine->start());
    return engine;
}

Image image(std::uint8_t value) {
    Image result;
    result.width = result.height = 2;
    result.rgba.assign(16, value);
    return result;
}
} // namespace

TEST_CASE("replays accept a seed and commands object and reject unknown fields") {
    const auto file = writeReplay("seeded.json", {{"seed", 42}, {"commands", {{{"tick", 1}, {"wheel", 1}}}}});
    ReplayHarness replay(file);
    CHECK(replay.seed() == 42u);
    CHECK(replay.lastTick() == 1);
    CHECK_THROWS(ReplayHarness(writeReplay("bad.json", Json::array({{{"tick", 1}, {"typo", 1}}}))));
    CHECK_THROWS(
        ReplayHarness(writeReplay("compare.json", Json::array({{{"tick", 1}, {"compare", "a.png"}}}))));
}

TEST_CASE("replay commands feed input, mouse and wheel and report comparisons") {
    auto engine = fixtureEngine();
    engine->input().define("confirm", {"key:Z"});
    const auto file = writeReplay(
        "input.json",
        Json::array(
            {{{"tick", 1}, {"input", {{"key:Z", 1}}}},
             {{"tick", 1}, {"mouse", {12, 34}}, {"wheel", 2}},
             {{"tick", 2}, {"capture", "shot.png"}, {"compare", "reference/shot.png"}, {"tolerance", 0.1}}}));
    ReplayHarness replay(file);
    replay.apply(1, *engine);
    engine->tick(1.0 / 60);
    CHECK(engine->input().pressed("confirm"));
    CHECK(engine->input().mouse() == glm::vec2(12, 34));
    CHECK(engine->input().wheel() == doctest::Approx(2));
    const auto effects = replay.apply(2, *engine);
    REQUIRE(effects.reference);
    CHECK(*effects.reference == file.parent_path() / "reference/shot.png");
    CHECK(effects.tolerance == doctest::Approx(0.1));
    CHECK(replay.finished());
    CHECK_FALSE(replay.done(2));
    CHECK(replay.done(33));
}

TEST_CASE("image comparison measures mean and largest channel difference") {
    const auto same = compareImages(image(100), image(100));
    CHECK(same.mean == doctest::Approx(0));
    auto changed = image(100);
    changed.rgba[0] = 151;
    const auto difference = compareImages(changed, image(100));
    CHECK(difference.max == doctest::Approx(51.0 / 255));
    CHECK(difference.mean == doctest::Approx(51.0 / 255 / 16));
    Image wide = image(100);
    wide.width = 4;
    wide.height = 1;
    CHECK(compareImages(wide, image(100)).mean == doctest::Approx(1));
}

TEST_CASE("the recorder writes taps as ordered commands that replay identically") {
    ReplayRecorder recorder(7);
    recorder.input("key:Z", 1);
    recorder.input("key:Z", 0);
    recorder.mouse({5, 6});
    recorder.stamp(3);
    recorder.input("key:X", 1);
    recorder.stamp(10);
    const auto json = recorder.toJson(20);
    CHECK(json.at("seed") == 7);
    const auto& commands = json.at("commands");
    REQUIRE(commands.size() == 4);
    CHECK(commands[0].at("mouse") == Json::array({5, 6}));
    CHECK(commands[1].at("input") == Json::array({Json::array({"key:Z", 1.0}), Json::array({"key:Z", 0.0})}));
    CHECK(commands[2].at("input") == Json::array({Json::array({"key:X", 1.0})}));
    CHECK(commands[3] == Json{{"tick", 20}});

    auto engine = fixtureEngine();
    engine->input().define("confirm", {"key:Z"});
    ReplayHarness replay(writeReplay("recorded.json", json));
    CHECK(replay.seed() == 7u);
    replay.apply(3, *engine);
    engine->tick(1.0 / 60); // the recorded tap reads as pressed, then released
    CHECK(engine->input().pressed("confirm"));
    engine->tick(1.0 / 60);
    CHECK(engine->input().released("confirm"));
    CHECK_FALSE(engine->input().held("confirm"));
}

TEST_CASE("recorded typed text and compositions replay through the text input path") {
    ReplayRecorder recorder(3);
    recorder.text("Ar");
    recorder.text("ia");
    recorder.composition("かな", 2);
    recorder.stamp(2);
    recorder.composition("", 0);
    recorder.stamp(4);
    const auto json = recorder.toJson(4);
    const auto& commands = json.at("commands");
    REQUIRE(commands.size() == 3);
    CHECK(commands[0] == Json{{"tick", 2}, {"text", "Aria"}});
    CHECK(commands[1] == Json{{"tick", 2}, {"text", "かな"}, {"cursor", 2}});

    auto engine = fixtureEngine();
    ReplayHarness replay(writeReplay("text.json", json));
    replay.apply(2, *engine);
    engine->tick(1.0 / 60);
    CHECK(engine->input().text() == "Aria");
    CHECK(engine->input().composition() == "かな");
    CHECK(engine->input().compositionCursor() == 2);
    replay.apply(4, *engine);
    engine->tick(1.0 / 60);
    CHECK(engine->input().text().empty());
    CHECK(engine->input().composition().empty());
    CHECK_THROWS(ReplayHarness(writeReplay("cursor.json", Json::array({{{"tick", 1}, {"cursor", 1}}}))));
}

TEST_CASE("headless replays run Lua, waits and asserts without a window") {
    const auto pass =
        writeReplay("pass.json", Json::array({{{"tick", 1}, {"lua", "replay_counter = 0"}},
                                              {{"tick", 2}, {"lua", "replay_counter = replay_counter + 1"}},
                                              {{"tick", 3}, {"wait_for", "replay_counter == 1"}},
                                              {{"tick", 4}, {"assert", "replay_counter == 1"}}}));
    CHECK(runHeadlessReplay(FENGINE_TEST_GAME, FENGINE_LUALIB, pass, {}, std::nullopt) == 0);
    const auto fail = writeReplay("fail.json", Json::array({{{"tick", 1}, {"assert", "1 == 2"}}}));
    CHECK(runHeadlessReplay(FENGINE_TEST_GAME, FENGINE_LUALIB, fail, {}, std::nullopt) == 1);
}

TEST_CASE("replay captures must be rendered frames and match the backend's references") {
    const auto references =
        std::filesystem::path(FENGINE_LUALIB) / ".." / "samples" / "vale" / "replays" / "reference" / "metal";
    const auto live = Image::load(references / "postfx-live.png");
    const auto custom = Image::load(references / "postfx-custom.png");
    CHECK(live.width == ReferenceWidth);
    // Rendered scenes, dark or sepia ones included, pass; black, cleared and
    // single-colour frames don't.
    for (const auto* name : {"postfx-live.png", "postfx-custom.png", "postfx-shatter.png"}) {
        CAPTURE(name);
        CHECK(blankFrameProblem(Image::load(references / name)).empty());
    }
    Image black;
    black.width = 640;
    black.height = 360;
    black.rgba.assign(static_cast<std::size_t>(640) * 360 * 4, 0);
    for (std::size_t i = 3; i < black.rgba.size(); i += 4) {
        black.rgba[i] = 255;
    }
    CHECK(blankFrameProblem(black).find("covers 100%") != std::string::npos);
    auto noisy = black;
    for (std::size_t i = 0; i < noisy.rgba.size(); i += 4 * 7) {
        noisy.rgba[i] = noisy.rgba[i + 1] = noisy.rgba[i + 2] = 8; // near-black dither
    }
    CHECK_FALSE(blankFrameProblem(noisy).empty());
    CHECK_FALSE(blankFrameProblem(Image{}).empty());
    // Captures are compared at reference size, whatever the window's density.
    Image large;
    large.width = live.width * 8;
    large.height = live.height * 8;
    large.rgba.resize(static_cast<std::size_t>(large.width) * large.height * 4);
    for (int y = 0; y < large.height; ++y) {
        for (int x = 0; x < large.width; ++x) {
            for (int c = 0; c < 4; ++c) {
                large.rgba[(static_cast<std::size_t>(y) * large.width + x) * 4 + c] =
                    live.rgba[(static_cast<std::size_t>(y / 8) * live.width + x / 8) * 4 + c];
            }
        }
    }
    CHECK(compareCapture(large, live).mean < 1e-9);
    CHECK(compareCapture(live, live).mean == 0);
    CHECK(compareCapture(custom, live).mean > 0.02);
    CHECK(compareCapture(black, live).mean > 0.02);
    CHECK(resizedTo(large, ReferenceWidth).height == live.height);
    // "{backend}" picks the running backend's folder.
    CHECK(backendFolder("Metal") == "metal");
    CHECK(backendFolder("Direct3D 11") == "direct3d11");
    CHECK(backendFolder("OpenGL 2.1") == "opengl21");
    CHECK(referenceFor("reference/{backend}/a.png", "Vulkan") ==
          std::filesystem::path("reference/vulkan/a.png"));
    CHECK(referenceFor("reference/a.png", "Vulkan") == std::filesystem::path("reference/a.png"));
}

TEST_CASE("recorded inputs replay in the order they happened; the object form still works") {
    ReplayRecorder recorder(1);
    recorder.input("key:X", 1);
    recorder.input("pad:a", 1);
    recorder.stamp(1);
    recorder.input("pad:a", 0);
    recorder.input("key:X", 0);
    recorder.stamp(2);
    const auto json = recorder.toJson(2);
    CHECK(json.at("commands")[0].at("input") ==
          Json::array({Json::array({"key:X", 1.0}), Json::array({"pad:a", 1.0})}));

    auto engine = fixtureEngine();
    ReplayHarness replay(writeReplay("ordered.json", json));
    replay.apply(1, *engine);
    CHECK(engine->input().lastDevice() == "gamepad"); // the gamepad event came last
    engine->tick(1.0 / 60);
    CHECK(engine->input().raw("key:X") == 1);
    CHECK(engine->input().raw("pad:a") == 1);
    replay.apply(2, *engine);
    CHECK(engine->input().lastDevice() == "keyboard");

    // A sorted object feeds in key order (key:X before pad:a).
    auto other = fixtureEngine();
    ReplayHarness legacy(writeReplay(
        "legacy.json", Json::array({{{"tick", 1}, {"input", {{"pad:a", 1}, {"key:X", 1}}}}})));
    legacy.apply(1, *other);
    CHECK(other->input().lastDevice() == "gamepad");
    other->tick(1.0 / 60);
    CHECK(other->input().raw("key:X") == 1);
    CHECK(other->input().raw("pad:a") == 1);
}

TEST_CASE("a wait_for that times out fails without skipping the command's other fields") {
    auto engine = fixtureEngine();
    ReplayHarness replay(writeReplay(
        "timeout.json",
        Json::array({{{"tick", 1}, {"wait_for", "false"}, {"timeout", 1}, {"lua", "replay_after_wait = 7"}},
                     {{"tick", 2}, {"lua", "replay_next = 1"}}})));
    replay.apply(1, *engine);
    replay.apply(2, *engine);
    CHECK(replay.failures().empty());
    CHECK(engine->lua().console("replay_after_wait") == "nil");
    replay.apply(3, *engine);
    REQUIRE(replay.failures().size() == 1);
    CHECK(replay.failures()[0].find("Timed out") != std::string::npos);
    CHECK(engine->lua().console("replay_after_wait") == "7");
    // Later ticks are shifted by the wait, as after a satisfied wait_for.
    CHECK(engine->lua().console("replay_next") == "nil");
    replay.apply(4, *engine);
    CHECK(engine->lua().console("replay_next") == "1");
    CHECK(replay.finished());
}
