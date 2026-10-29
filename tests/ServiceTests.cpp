#include "engine/assets/Image.h"
#include "engine/core/Paths.h"
#include "engine/services/Canvas.h"
#include "engine/services/InputState.h"
#include "engine/services/SaveStore.h"
#include <doctest/doctest.h>

using namespace fe;

TEST_CASE("a tap inside one tick is pressed this tick and released the next") {
    InputState input;
    input.define("confirm", {"key:Z", "pad:a"});
    input.feed("key:Z", 1);
    input.feed("key:Z", 0);
    input.tick(1.0 / 60);
    CHECK(input.pressed("confirm"));
    CHECK(input.held("confirm"));
    CHECK_FALSE(input.released("confirm"));
    CHECK(input.pressedInputs() == std::vector<std::string>{"key:Z"});
    input.tick(1.0 / 60);
    CHECK_FALSE(input.pressed("confirm"));
    CHECK(input.released("confirm"));
    CHECK_FALSE(input.held("confirm"));
    input.tick(1.0 / 60);
    CHECK_FALSE(input.released("confirm"));
}

TEST_CASE("a release and re-press inside one tick keeps the press") {
    InputState input;
    input.define("confirm", {"key:Z"});
    input.feed("key:Z", 1);
    input.tick(1.0 / 60);
    input.feed("key:Z", 0);
    input.feed("key:Z", 1);
    input.tick(1.0 / 60);
    CHECK(input.released("confirm"));
    input.tick(1.0 / 60);
    CHECK(input.pressed("confirm"));
    CHECK(input.held("confirm"));
}

TEST_CASE("queued input stays bounded and focus loss releases everything") {
    InputState input;
    input.define("confirm", {"key:Z"});
    for (int i = 0; i < 1000; ++i) {
        input.feed("key:Z", static_cast<float>(i % 2 == 0));
    }
    for (int i = 0; i < 10; ++i) {
        input.tick(1.0 / 60);
    }
    CHECK_FALSE(input.held("confirm"));
    input.tick(1.0 / 60);
    CHECK_FALSE(input.pressed("confirm"));
    CHECK_FALSE(input.released("confirm"));
    input.feed("key:Z", 1);
    input.tick(1.0 / 60);
    CHECK(input.held("confirm"));
    input.feed("key:Z", 0);
    input.feed("key:Z", 1);
    input.focusLost();
    input.tick(1.0 / 60);
    CHECK(input.released("confirm"));
    input.tick(1.0 / 60);
    CHECK_FALSE(input.held("confirm"));
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
    for (const char* reserved : {"con", "nul", "aux", "prn", "com1", "lpt9"}) {
        CAPTURE(reserved);
        CHECK_THROWS(store.write(reserved, {}, {}));
    }
    CHECK_NOTHROW(store.write("console", {}, {}));
    CHECK_FALSE(std::filesystem::exists(folder / "saves" / "console.json.tmp"));
}

TEST_CASE("save reads fall back to the previous version") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-backup-test";
    std::filesystem::remove_all(folder);
    SaveStore store(folder);
    store.write("slot1", {{"step", 1}}, {});
    store.write("slot1", {{"step", 2}}, {});
    CHECK(store.read("slot1")->data.at("step") == 2);
    const auto file = folder / "saves" / "slot1.json";
    // A file cut short (an interrupted write) reads the previous save, and says so.
    writeTextAtomic(file, "{\"format\": 1, \"data\": ");
    CHECK(store.read("slot1")->data.at("step") == 1);
    CHECK(store.read("slot1")->fromBackup);
    CHECK(store.list()[0].fromBackup);
    writeTextAtomic(file, "");
    CHECK(store.read("slot1")->fromBackup);
    std::filesystem::remove(file);
    CHECK(store.read("slot1")->data.at("step") == 1);
    CHECK(store.read("slot1")->fromBackup);
    REQUIRE(store.list().size() == 1);
    CHECK(store.list()[0].slot == "slot1");
    store.remove("slot1");
    CHECK_FALSE(store.read("slot1"));
    CHECK(store.list().empty());
    store.writeConfig({{"volume", 1}});
    store.writeConfig({{"volume", 0.25}});
    std::filesystem::remove(folder / "config.json");
    CHECK(store.readConfig().at("volume") == 1);
}

TEST_CASE("a complete but unreadable save raises instead of loading the previous one") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-stale-test";
    std::filesystem::remove_all(folder);
    SaveStore store(folder);
    store.write("slot1", {{"step", 1}}, {});
    store.write("slot1", {{"step", 2}}, {});
    const auto file = folder / "saves" / "slot1.json";
    CHECK_FALSE(store.read("slot1")->fromBackup);
    for (const char* text : {"{\"format\": 1, \"data\": {\"step\": 3}} trailing",
                             "{\"format\": 1 \"data\": 3}", "[1, 2, 3]", "{\"format\": 2, \"data\": {}}"}) {
        CAPTURE(text);
        writeTextAtomic(file, text);
        CHECK_THROWS((void)store.read("slot1"));
        REQUIRE(store.list().size() == 1);
        CHECK(store.list()[0].meta.value("corrupt", false));
    }
    // Too deep to read back: the slot's file wraps the data in one more level.
    std::string deep = "{\"format\": 1, \"data\": ";
    for (int i = 0; i <= maxJsonDepth; ++i) {
        deep += "[";
    }
    deep += std::string(static_cast<std::size_t>(maxJsonDepth) + 1, ']') + "}";
    writeTextAtomic(file, deep);
    CHECK_THROWS_WITH((void)store.read("slot1"), doctest::Contains("nests deeper"));
}

TEST_CASE("saves that could not be read back are not written") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-depth-test";
    std::filesystem::remove_all(folder);
    SaveStore store(folder);
    store.write("slot1", {{"gold", 1}}, {});
    // The data may nest maxJsonDepth - 1 levels below its own table; the file adds one.
    const auto nested = [](int levels) {
        Json value = Json::object();
        for (int i = 0; i < levels; ++i) {
            value = Json{{"x", value}};
        }
        return value;
    };
    CHECK(jsonDepth(nested(maxJsonDepth - 1)) == maxJsonDepth - 1);
    CHECK_THROWS_WITH(store.write("slot1", nested(maxJsonDepth), {}), doctest::Contains("nests deeper"));
    CHECK_THROWS(store.write("slot1", {{"gold", 2}}, nested(maxJsonDepth)));
    CHECK(store.read("slot1")->data.at("gold") == 1);
    store.write("slot1", nested(maxJsonDepth - 1), {});
    const auto loaded = store.read("slot1");
    REQUIRE(loaded);
    CHECK(jsonDepth(loaded->data) == maxJsonDepth - 1);
    CHECK_FALSE(loaded->fromBackup);
    // Every JSON writer refuses what parseJson would refuse to read.
    CHECK_THROWS(writeJson(folder / "deep.json", nested(maxJsonDepth + 1)));
    CHECK_FALSE(std::filesystem::exists(folder / "deep.json"));
    writeJson(folder / "deep.json", nested(maxJsonDepth));
    CHECK(jsonDepth(readJson(folder / "deep.json")) == maxJsonDepth);
}

TEST_CASE("backups and PNG thumbnails go through the durable atomic writer") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-durable-test";
    std::filesystem::remove_all(folder);
    const auto file = folder / "data.json";
    writeTextAtomic(file, "first", true);
    CHECK_FALSE(std::filesystem::exists(backupPath(file)));
    writeTextAtomic(file, "second", true);
    CHECK(readFile(backupPath(file)) == "first");
    CHECK(readFile(file) == "second");
    const auto png = folder / "thumbnails" / "slot1.png";
    Image::solid(10, 20, 30).savePng(png);
    CHECK_FALSE(std::filesystem::exists(folder / "thumbnails" / "slot1.png.tmp"));
    const auto loaded = Image::load(png);
    CHECK(loaded.width == 1);
    CHECK(loaded.rgba == std::vector<std::uint8_t>{10, 20, 30, 255});
}

TEST_CASE("save lists skip files that are not slots") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-stray-test";
    std::filesystem::remove_all(folder);
    SaveStore store(folder);
    store.write("slot1", {{"step", 1}}, {{"where", "town"}});
    std::vector<std::string> strays = {"slot1 (conflicted copy).json", "Slot2.json", "notes.json.bak",
                                       "a-name-that-is-far-too-long-for-a-slot.json"};
#if !defined(_WIN32)
    // Windows opens the console device for con.json, so no such file exists there.
    strays.push_back("con.json");
#endif
    for (const auto& stray : strays) {
        writeTextAtomic(folder / "saves" / stray, "{\"format\": 1, \"data\": {}}");
    }
    const auto slots = store.list();
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].slot == "notes");
    CHECK(slots[1].slot == "slot1");
    CHECK(slots[1].meta.at("where") == "town");
}

TEST_CASE("redefined fonts stay alive until the frame's quads are gone") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_LUALIB);
    const Json spec = {{"files", {"@fe/assets/fonts/pixel.fnt"}}};
    assets.defineFont("default", spec);
    std::weak_ptr<FontSet> previous = assets.font("default");
    assets.defineFont("default", spec);
    CHECK_FALSE(previous.expired());
    assets.releaseRetiredFonts();
    CHECK(previous.expired());
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (assets.progress(ticket) < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(assets.progress(ticket) == doctest::Approx(1));
    CHECK(assets.tryModel("maps/arena/geometry.glb"));
}
