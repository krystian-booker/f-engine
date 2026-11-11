#include "engine/assets/Image.h"
#include "engine/core/Paths.h"
#include "engine/services/Canvas.h"
#include "engine/services/InputRecorder.h"
#include "engine/services/InputState.h"
#include "engine/services/SaveStore.h"
#include <chrono>
#include <doctest/doctest.h>
#include <map>
#include <thread>

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
    writeTextAtomic(file, "{\"n\": 1}", true);
    CHECK_FALSE(std::filesystem::exists(backupPath(file)));
    // The previous file becomes the backup by rename, not by copy: a hard
    // link to it is a link to the .bak afterwards, and nothing is left over.
    const auto link = folder / "data.link";
    std::filesystem::create_hard_link(file, link);
    writeTextAtomic(file, "{\"n\": 2}", true);
    CHECK(readFile(backupPath(file)) == "{\"n\": 1}");
    CHECK(readFile(file) == "{\"n\": 2}");
    CHECK(std::filesystem::equivalent(link, backupPath(file)));
    CHECK_FALSE(std::filesystem::equivalent(link, file));
    CHECK_FALSE(std::filesystem::exists(folder / "data.json.tmp"));
    std::filesystem::remove(link);
    // A second write renames the current file over the older backup.
    writeTextAtomic(file, "{\"n\": 3}", true);
    CHECK(readFile(backupPath(file)) == "{\"n\": 2}");
    // Without keepBackup the previous version is replaced and the backup untouched.
    writeTextAtomic(file, "{\"n\": 4}");
    CHECK(readFile(backupPath(file)) == "{\"n\": 2}");
    CHECK(readFile(file) == "{\"n\": 4}");
    // The crash window between the two renames: the file is missing and the
    // backup holds the last version, which the JSON reader falls back to.
    std::filesystem::remove(file);
    CHECK(readJsonOrBackup(file).at("n") == 2);
    writeTextAtomic(file, "{\"n\": 5}", true);
    CHECK(readFile(backupPath(file)) == "{\"n\": 2}"); // nothing to rename aside: the backup stays
    CHECK(readFile(file) == "{\"n\": 5}");
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
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_BUILTIN);
    const Json spec = {{"files", {"@fe/assets/fonts/pixel.fnt"}}};
    assets.defineFont("default", spec);
    std::weak_ptr<FontSet> previous = assets.font("default");
    assets.defineFont("default", spec);
    CHECK_FALSE(previous.expired());
    assets.releaseRetiredFonts();
    CHECK(previous.expired());
}

TEST_CASE("canvas records transformed, clipped quads") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_BUILTIN);
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
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_BUILTIN);
    const int ticket = assets.preload({"maps/arena/geometry.glb", "@fe/assets/fonts/pixel.png"});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (assets.progress(ticket) < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(assets.progress(ticket) == doctest::Approx(1));
    CHECK(assets.tryModel("maps/arena/geometry.glb"));
}

TEST_CASE("a blank or unparseable JSON file is never copied over the backup") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-backup-guard-test";
    std::filesystem::remove_all(folder);
    const auto file = folder / "slot.json";
    writeJson(file, {{"step", 1}}, true);
    writeJson(file, {{"step", 2}}, true);
    CHECK(readJson(backupPath(file)).at("step") == 1);
    // A truncated file (an interrupted write) must not become the backup.
    writeTextAtomic(file, "{\"step\": ");
    writeJson(file, {{"step", 3}}, true);
    CHECK(readJson(backupPath(file)).at("step") == 1);
    writeTextAtomic(file, "");
    writeJson(file, {{"step", 4}}, true);
    CHECK(readJson(backupPath(file)).at("step") == 1);
    // A complete previous version is backed up as before.
    writeJson(file, {{"step", 5}}, true);
    CHECK(readJson(backupPath(file)).at("step") == 4);
    CHECK(worthBackingUp(file, "{\"a\": 1}"));
    CHECK_FALSE(worthBackingUp(file, "{\"a\": "));
    CHECK_FALSE(worthBackingUp(file, " \n"));
    // Files that are not JSON are backed up whenever they hold something.
    CHECK(worthBackingUp(folder / "notes.txt", "{not json"));
    CHECK_FALSE(worthBackingUp(folder / "notes.txt", ""));
}

TEST_CASE("keys named with + or - bind as they are; the suffix rule is for analog inputs") {
    InputState input;
    input.define("zoom_out", {"key:-", "key:Keypad -"});
    input.define("zoom_in", {"key:Keypad +"});
    input.define("left", {"pad:leftx-"});
    input.define("right", {"pad:leftx+"});
    CHECK(input.parseBinding("key:-").input == "key:-");
    CHECK(input.parseBinding("key:-").direction == 0);
    CHECK(input.parseBinding("key:Keypad +").direction == 0);
    CHECK(input.parseBinding("pad:leftx-").input == "pad:leftx");
    CHECK(input.parseBinding("pad:leftx-").direction == -1);
    CHECK(input.parseBinding("pad2:righty+").direction == 1);
    input.feed("key:-", 1);
    input.tick(1.0 / 60);
    CHECK(input.pressed("zoom_out"));
    input.feed("key:-", 0);
    input.feed("key:Keypad +", 1);
    input.tick(1.0 / 60);
    CHECK(input.released("zoom_out"));
    CHECK(input.pressed("zoom_in"));
    input.feed("pad:leftx", -0.9f);
    input.tick(1.0 / 60);
    CHECK(input.held("left"));
    CHECK_FALSE(input.held("right"));
    input.feed("pad:leftx", 0.9f);
    input.tick(1.0 / 60);
    CHECK(input.held("right"));
    CHECK_FALSE(input.held("left"));
    // An input an axis lists as analog takes the suffix too.
    input.defineAxis("throttle", {{}, {}, {"wheel:axis"}, 0.1f});
    CHECK(input.parseBinding("wheel:axis+").input == "wheel:axis");
    CHECK(input.parseBinding("wheel:other+").input == "wheel:other+");
}

TEST_CASE("the per-tick collapse honours each action's threshold") {
    InputState input;
    input.define("fire", {"pad:righttrigger"}, 0.9f);
    input.define("aim", {"pad:righttrigger"}, 0.3f);
    // A pull past 0.9 and back to 0.6 inside one tick: fire is pressed this
    // tick and released the next; aim stays held throughout.
    input.feed("pad:righttrigger", 0.95f);
    input.feed("pad:righttrigger", 0.6f);
    input.tick(1.0 / 60);
    CHECK(input.pressed("fire"));
    CHECK(input.pressed("aim"));
    input.tick(1.0 / 60);
    CHECK(input.released("fire"));
    CHECK(input.held("aim"));
    CHECK(input.raw("pad:righttrigger") == doctest::Approx(0.6f));
    // Values between the thresholds collapse: nothing an action sees changes.
    input.feed("pad:righttrigger", 0.7f);
    input.feed("pad:righttrigger", 0.8f);
    input.feed("pad:righttrigger", 0.5f);
    input.tick(1.0 / 60);
    CHECK(input.held("aim"));
    CHECK_FALSE(input.held("fire"));
    CHECK(input.raw("pad:righttrigger") == doctest::Approx(0.5f));
    input.tick(1.0 / 60);
    CHECK(input.held("aim"));
    // A directional threshold: a flick through both sides of a stick axis.
    input.define("left", {"pad:leftx-"}, 0.7f);
    input.define("right", {"pad:leftx+"}, 0.7f);
    input.feed("pad:leftx", -0.8f);
    input.feed("pad:leftx", 0.8f);
    input.tick(1.0 / 60);
    CHECK(input.pressed("left"));
    CHECK_FALSE(input.held("right"));
    input.tick(1.0 / 60);
    CHECK(input.released("left"));
    CHECK(input.pressed("right"));
}

namespace {
class CountingRecorder final : public InputRecorder {
public:
    void input(const std::string& name, float value) override { inputs.emplace_back(name, value); }
    void mouse(glm::vec2 position) override { mice.push_back(position); }
    void wheel(float delta) override { wheels.push_back(delta); }
    void text(std::string_view utf8) override { texts.emplace_back(utf8); }
    void composition(const std::string& text, int cursor) override {
        compositions.emplace_back(text, cursor);
    }
    void pad(const InputState::PadEvent& event) override { pads.push_back(event); }
    std::vector<std::pair<std::string, float>> inputs;
    std::vector<glm::vec2> mice;
    std::vector<float> wheels;
    std::vector<std::string> texts;
    std::vector<std::pair<std::string, int>> compositions;
    std::vector<InputState::PadEvent> pads;
};
} // namespace

TEST_CASE("an input recorder sees everything fed, including focus-loss releases") {
    InputState input;
    CountingRecorder recorder;
    input.setRecorder(&recorder);
    input.define("confirm", {"key:Z"});
    input.feed("key:Z", 1);
    input.feed("pad:a", 1);
    input.feedMouse({3, 4});
    input.feedWheel(-1);
    input.feedText("hi");
    input.feedComposition("か", 1);
    input.tick(1.0 / 60);
    CHECK(input.held("confirm"));
    input.focusLost();
    input.tick(1.0 / 60);
    CHECK(input.released("confirm"));
    CHECK(input.composition().empty());
    REQUIRE(recorder.inputs.size() == 4);
    CHECK(recorder.inputs[0] == std::pair<std::string, float>{"key:Z", 1});
    CHECK(recorder.inputs[1] == std::pair<std::string, float>{"pad:a", 1});
    CHECK(recorder.inputs[2] == std::pair<std::string, float>{"key:Z", 0});
    CHECK(recorder.inputs[3] == std::pair<std::string, float>{"pad:a", 0});
    CHECK(recorder.mice == std::vector<glm::vec2>{{3, 4}});
    CHECK(recorder.wheels == std::vector<float>{-1});
    CHECK(recorder.texts == std::vector<std::string>{"hi"});
    REQUIRE(recorder.compositions.size() == 2);
    CHECK(recorder.compositions[1] == std::pair<std::string, int>{"", 0});
    // A second focus loss with nothing held and no composition records nothing.
    input.focusLost();
    CHECK(recorder.inputs.size() == 4);
    CHECK(recorder.compositions.size() == 2);
    input.setRecorder(nullptr);
    input.feed("key:Z", 1);
    CHECK(recorder.inputs.size() == 4);
}

TEST_CASE("gamepad connections are published on the next tick and reach a recorder attached later") {
    InputState input;
    input.connectPad({1, "Pad One", "xbox", true, false});
    CHECK(input.pads().contains(1));  // pads() changes at once
    CHECK(input.padEvents().empty()); // the event waits for the tick
    CountingRecorder recorder;
    input.setRecorder(&recorder); // sees the event still queued
    REQUIRE(recorder.pads.size() == 1);
    CHECK(recorder.pads[0].connected);
    CHECK(recorder.pads[0].pad.name == "Pad One");
    input.connectPad({2, "Pad Two", "generic", false, false});
    CHECK(recorder.pads.size() == 2);
    input.setRecorder(&recorder); // attaching again reports nothing twice
    CHECK(recorder.pads.size() == 2);
    input.tick(1.0 / 60);
    REQUIRE(input.padEvents().size() == 2);
    CHECK(input.padEvents()[1].pad.number == 2);
    input.tick(1.0 / 60);
    CHECK(input.padEvents().empty());
    input.disconnectPad(2);
    input.disconnectPad(9); // never connected: no event
    input.tick(1.0 / 60);
    REQUIRE(input.padEvents().size() == 1);
    CHECK_FALSE(input.padEvents()[0].connected);
    CHECK(input.padEvents()[0].pad.name == "Pad Two");
    CHECK(recorder.pads.size() == 3);
    // A flood keeps the first 64 pending events.
    for (int i = 0; i < 100; ++i) {
        input.connectPad({3, "Flood", "generic", false, false});
    }
    input.tick(1.0 / 60);
    CHECK(input.padEvents().size() == 64);
}

namespace {
// A backend that keeps documents in memory and counts reads, standing in for
// a cloud or console save API.
class MemorySaveBackend final : public SaveBackend {
public:
    [[nodiscard]] std::string name() const override { return "memory"; }
    [[nodiscard]] std::optional<std::string> read(const std::string& key, bool backup) const override {
        ++reads[key];
        const auto& store = backup ? backups : current;
        const auto found = store.find(key);
        return found == store.end() ? std::nullopt : std::optional(found->second);
    }
    void write(const std::string& key, const std::string& bytes, bool keepBackup) override {
        if (keepBackup && current.contains(key)) {
            backups[key] = current[key];
        }
        current[key] = bytes;
    }
    [[nodiscard]] std::vector<std::string> list(const std::string& folder) const override {
        std::vector<std::string> names;
        const auto prefix = folder + "/";
        for (const auto& [key, bytes] : current) {
            if (key.starts_with(prefix)) {
                names.push_back(key.substr(prefix.size()));
            }
        }
        for (const auto& [key, bytes] : backups) {
            if (key.starts_with(prefix)) {
                names.push_back(key.substr(prefix.size()) + ".bak");
            }
        }
        return names;
    }
    void remove(const std::string& key) override {
        current.erase(key);
        backups.erase(key);
    }
    [[nodiscard]] std::optional<Stat> stat(const std::string& key, bool backup) const override {
        const auto& store = backup ? backups : current;
        const auto found = store.find(key);
        if (found == store.end()) {
            return std::nullopt;
        }
        return Stat{1700000000.0, found->second.size()};
    }
    std::map<std::string, std::string> current, backups;
    mutable std::map<std::string, int> reads;
};
} // namespace

TEST_CASE("a save backend stores the slots, settings and sidecar summaries") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-backend-test";
    std::filesystem::remove_all(folder);
    auto owned = std::make_unique<MemorySaveBackend>();
    auto* memory = owned.get();
    SaveStore store(folder, std::move(owned));
    CHECK(store.backend().name() == "memory");
    store.write("slot1", {{"gold", 10}}, {{"location", "Town"}});
    store.write("slot1", {{"gold", 11}}, {{"location", "Woods"}});
    CHECK(memory->current.contains("saves/slot1.json"));
    CHECK(memory->current.contains("saves/slot1.meta.json"));
    CHECK(memory->backups.contains("saves/slot1.json"));
    CHECK_FALSE(std::filesystem::exists(folder / "saves"));
    CHECK(store.read("slot1")->data.at("gold") == 11);
    // Listing reads the sidecar, never the slot document.
    memory->reads.clear();
    const auto slots = store.list();
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].meta.at("location") == "Woods");
    CHECK(slots[0].modified == doctest::Approx(1700000000.0));
    CHECK(memory->reads["saves/slot1.meta.json"] == 1);
    CHECK(memory->reads["saves/slot1.json"] == 0);
    // A sidecar that no longer matches the document falls back to the full read.
    memory->current["saves/slot1.json"] = memory->backups["saves/slot1.json"];
    CHECK(store.list()[0].meta.at("location") == "Town");
    CHECK(memory->reads["saves/slot1.json"] == 1);
    // A cut-short document reads the backup, and the listing says so.
    memory->current["saves/slot1.json"] = "{\"format\": 1, \"data\": ";
    CHECK(store.read("slot1")->fromBackup);
    CHECK(store.list()[0].fromBackup);
    memory->current.erase("saves/slot1.json");
    CHECK(store.read("slot1")->data.at("gold") == 10);
    store.writeConfig({{"volume", 0.5}});
    CHECK(store.readConfig().at("volume") == 0.5);
    store.remove("slot1");
    CHECK_FALSE(store.read("slot1"));
    CHECK(store.list().empty());
    CHECK_FALSE(memory->current.contains("saves/slot1.meta.json"));
}

TEST_CASE("the file backend writes sidecar summaries and the save size is capped") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-sidecar-test";
    std::filesystem::remove_all(folder);
    SaveStore store(folder);
    CHECK(store.backend().name() == "file");
    store.write("slot1", {{"gold", 1}}, {{"where", "town"}});
    const auto sidecar = folder / "saves" / "slot1.meta.json";
    REQUIRE(std::filesystem::is_regular_file(sidecar));
    const auto summary = readJson(sidecar);
    CHECK(summary.at("meta").at("where") == "town");
    CHECK(summary.at("size") == std::filesystem::file_size(folder / "saves" / "slot1.json"));
    CHECK(store.list()[0].meta.at("where") == "town");
    // Without the sidecar the slot is read in full.
    std::filesystem::remove(sidecar);
    CHECK(store.list()[0].meta.at("where") == "town");
    // A slot document over the limit is refused and the previous save kept.
    const std::string huge(SaveStore::MaxSlotBytes, 'x');
    CHECK_THROWS_WITH(store.write("slot1", {{"blob", huge}}, {}), doctest::Contains("at most 16 MiB"));
    CHECK(store.read("slot1")->data.at("gold") == 1);
    store.remove("slot1");
    CHECK_FALSE(std::filesystem::exists(sidecar));
}

TEST_CASE("the asset cache evicts least recently used assets outside the referenced set") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_BUILTIN);
    CHECK(assets.budget() == AssetCache::DefaultBudget);
    const std::string a = "@fe/assets/ui/cursor.png", b = "@fe/assets/ui/bubble.png",
                      c = "@fe/assets/ui/spark.png";
    const auto first = assets.image(a);
    assets.image(b);
    assets.image(c);
    CHECK(assets.loaded(a));
    CHECK(assets.cachedCount() == 3);
    CHECK(assets.bytesOf(a) == decodedBytes(*first));
    CHECK(assets.cachedBytes() == assets.bytesOf(a) + assets.bytesOf(b) + assets.bytesOf(c));
    // Within budget nothing is evicted.
    CHECK(assets.releaseUnused({}) == 0);
    // `a` was used first; touching it makes `b` the oldest.
    assets.image(a);
    const auto generation = assets.generation();
    assets.setBudget(assets.cachedBytes() - 1);
    CHECK(assets.releaseUnused({}) == 1);
    CHECK_FALSE(assets.loaded(b));
    CHECK(assets.loaded(a));
    CHECK(assets.loaded(c));
    CHECK(assets.generation() == generation + 1);
    // Referenced assets stay however old; the budget is met by the others.
    assets.setBudget(0);
    CHECK(assets.releaseUnused({c}) == 1);
    CHECK(assets.loaded(c));
    CHECK_FALSE(assets.loaded(a));
    CHECK(assets.releaseUnused({c}) == 0);
    // release() drops one asset at once; an evicted asset loads again on use.
    CHECK(assets.release(c));
    CHECK_FALSE(assets.release(c));
    CHECK(assets.cachedCount() == 0);
    CHECK(assets.image(b)->width > 0);
    CHECK(assets.loaded(b));
    // Finished preload tickets are forgotten and read as complete.
    const int ticket = assets.preload({a});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (assets.progress(ticket) < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(assets.progress(ticket) == 1);
    CHECK(assets.progress(ticket + 100) == 1);
}

TEST_CASE("a font reloads only for the files it names") {
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_BUILTIN);
    assets.defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    const auto fonts = std::filesystem::path(FENGINE_BUILTIN) / "assets" / "fonts";
    const auto generation = assets.generation();
    std::weak_ptr<FontSet> before = assets.font("default");
    // The face and its page image rebuild the font; a neighbour in the folder does not.
    CHECK(assets.reload(fonts / "pixel.png"));
    CHECK(assets.generation() > generation);
    assets.releaseRetiredFonts();
    CHECK(before.expired());
    before = assets.font("default");
    CHECK_FALSE(assets.reload(fonts / "other-font.fnt"));
    assets.releaseRetiredFonts();
    CHECK_FALSE(before.expired());
    CHECK(assets.reload(fonts / "pixel.fnt"));
}

TEST_CASE("save blobs live next to their slot through the backend and go with the slot") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-blob-test";
    std::filesystem::remove_all(folder);
    std::string bytes("PNG\0\1\2\xff binary", 14);
    bytes.push_back('\0');
    SUBCASE("file backend") {
        SaveStore store(folder);
        store.write("slot1", {{"gold", 1}}, {});
        store.writeBlob("slot1", "snapshot", bytes);
        store.writeBlob("slot1", "inputs", "abc");
        CHECK(std::filesystem::is_regular_file(folder / "saves" / "slot1.snapshot.blob"));
        CHECK(store.readBlob("slot1", "snapshot") == bytes);
        CHECK(store.listBlobs("slot1") == std::vector<std::string>{"inputs", "snapshot"});
        // Blobs are not slots, and a slot need not exist for its blobs.
        CHECK(store.list().size() == 1);
        store.writeBlob("later", "preview", "x");
        CHECK(store.list().size() == 1);
        CHECK(store.listBlobs("later") == std::vector<std::string>{"preview"});
        store.removeBlob("slot1", "inputs");
        CHECK_FALSE(store.readBlob("slot1", "inputs"));
        store.remove("slot1");
        CHECK_FALSE(store.readBlob("slot1", "snapshot"));
        CHECK(store.listBlobs("slot1").empty());
        CHECK_FALSE(std::filesystem::exists(folder / "saves" / "slot1.snapshot.blob"));
        CHECK_THROWS(store.writeBlob("slot1", "Bad Name", "x"));
        CHECK_THROWS(store.writeBlob("../x", "name", "x"));
        CHECK_THROWS_WITH(store.writeBlob("slot1", "big", std::string(SaveStore::MaxSlotBytes + 1, 'x')),
                          doctest::Contains("at most 16 MiB"));
    }
    SUBCASE("a backend without blob storage of its own stores them as keys") {
        auto owned = std::make_unique<MemorySaveBackend>();
        auto* memory = owned.get();
        SaveStore store(folder, std::move(owned));
        store.write("slot1", {{"gold", 1}}, {});
        store.writeBlob("slot1", "snapshot", bytes);
        CHECK(memory->current.at("saves/slot1.snapshot.blob") == bytes);
        CHECK(store.readBlob("slot1", "snapshot") == bytes);
        CHECK(store.list().size() == 1);
        store.remove("slot1");
        CHECK_FALSE(memory->current.contains("saves/slot1.snapshot.blob"));
    }
}

#include "engine/services/SaveQueue.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

namespace {
// A backend in memory whose writes wait at a gate, so a test can look at the
// queue while a write is in progress, and which can be told to fail.
class GatedBackend final : public SaveBackend {
public:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::map<std::string, std::string> files, backups;
        bool open = true, fail = false;
        int writes = 0, overlapping = 0, active = 0;
    };
    explicit GatedBackend(std::shared_ptr<State> state, bool concurrent = true)
        : s_(std::move(state)), concurrent_(concurrent) {}
    [[nodiscard]] std::string name() const override { return "gated"; }
    [[nodiscard]] bool concurrentReads() const override { return concurrent_; }
    [[nodiscard]] std::optional<std::string> read(const std::string& key, bool backup) const override {
        const std::scoped_lock lock(s_->mutex);
        const auto& from = backup ? s_->backups : s_->files;
        const auto found = from.find(key);
        return found == from.end() ? std::nullopt : std::optional(found->second);
    }
    void write(const std::string& key, const std::string& bytes, bool keepBackup) override {
        std::unique_lock lock(s_->mutex);
        s_->overlapping += s_->active++ > 0 ? 1 : 0;
        s_->changed.wait(lock, [&] { return s_->open; });
        --s_->active;
        if (s_->fail) {
            throw std::runtime_error("disk full");
        }
        if (keepBackup && s_->files.contains(key)) {
            s_->backups[key] = s_->files[key];
        }
        s_->files[key] = bytes;
        ++s_->writes;
    }
    [[nodiscard]] std::vector<std::string> list(const std::string& folder) const override {
        const std::scoped_lock lock(s_->mutex);
        std::vector<std::string> names;
        for (const auto& [key, bytes] : s_->files) {
            if (key.starts_with(folder + "/")) {
                names.push_back(key.substr(folder.size() + 1));
            }
        }
        return names;
    }
    void remove(const std::string& key) override {
        const std::scoped_lock lock(s_->mutex);
        s_->files.erase(key);
        s_->backups.erase(key);
    }
    [[nodiscard]] std::optional<Stat> stat(const std::string& key, bool) const override {
        const std::scoped_lock lock(s_->mutex);
        const auto found = s_->files.find(key);
        if (found == s_->files.end()) {
            return std::nullopt;
        }
        Stat stat;
        stat.size = found->second.size();
        return stat;
    }

private:
    std::shared_ptr<State> s_;
    bool concurrent_;
};

void setGate(GatedBackend::State& state, bool open) {
    {
        const std::scoped_lock lock(state.mutex);
        state.open = open;
    }
    state.changed.notify_all();
}
} // namespace

TEST_CASE("queued saves return at once and read back from the queue until they are written") {
    const auto state = std::make_shared<GatedBackend::State>();
    QueuedSaveBackend queue(std::make_unique<GatedBackend>(state));
    setGate(*state, false); // storage is slow: nothing gets through yet
    queue.write("saves/a.json", "one", true);
    queue.write("config.json", "first", false);
    queue.write("config.json", "second", false); // replaces the queued one: no backup to keep
    queue.writeBlob("a", "snapshot", std::string("\0bytes", 6));
    CHECK(queue.pending() == 3);
    {
        const std::scoped_lock lock(state->mutex);
        CHECK(state->files.empty());
    }
    // The game sees its saves as written.
    CHECK(queue.read("saves/a.json", false) == std::optional<std::string>("one"));
    CHECK(queue.read("config.json", false) == std::optional<std::string>("second"));
    CHECK(queue.readBlob("a", "snapshot") == std::optional<std::string>(std::string("\0bytes", 6)));
    CHECK(queue.stat("saves/a.json", false)->size == 3);
    CHECK(queue.list("saves") == std::vector<std::string>{"a.json"});
    CHECK(queue.listBlobs("a") == std::vector<std::string>{"snapshot"});
    CHECK_FALSE(queue.read("saves/b.json", false).has_value());
    setGate(*state, true);
    queue.flush();
    CHECK(queue.pending() == 0);
    CHECK_FALSE(queue.takeError().has_value());
    const std::scoped_lock lock(state->mutex);
    CHECK(state->files.at("saves/a.json") == "one");
    CHECK(state->files.at("config.json") == "second");
    CHECK(state->writes == 3);      // the first config never reached storage
    CHECK(state->overlapping == 0); // the wrapped backend is never called twice at once
}

TEST_CASE("queued slot writes keep their order and their backups; removing drops what is queued") {
    const auto state = std::make_shared<GatedBackend::State>();
    QueuedSaveBackend queue(std::make_unique<GatedBackend>(state));
    setGate(*state, false);
    queue.write("saves/a.json", "one", true);
    queue.write("saves/a.json", "two", true);
    queue.write("saves/gone.json", "never", true);
    CHECK(queue.read("saves/a.json", false) == std::optional<std::string>("two"));
    queue.remove("saves/gone.json"); // nothing of it is in progress: dropped without waiting for storage
    CHECK_FALSE(queue.read("saves/gone.json", false).has_value());
    setGate(*state, true);
    // The backup is the save before the last one, as with writes carried out at once.
    CHECK(queue.read("saves/a.json", true) == std::optional<std::string>("one"));
    queue.flush();
    const std::scoped_lock lock(state->mutex);
    CHECK(state->files.at("saves/a.json") == "two");
    CHECK(state->backups.at("saves/a.json") == "one");
    CHECK_FALSE(state->files.contains("saves/gone.json"));
}

TEST_CASE("a failed background write is reported by flush, once") {
    const auto state = std::make_shared<GatedBackend::State>();
    {
        const std::scoped_lock lock(state->mutex);
        state->fail = true;
    }
    const auto folder = std::filesystem::temp_directory_path() / "fengine-save-queue-test";
    std::filesystem::remove_all(folder);
    SaveStore store(folder, std::make_unique<QueuedSaveBackend>(std::make_unique<GatedBackend>(state)));
    store.write("slot1", Json{{"gold", 5}}, Json::object()); // queued: no error yet
    CHECK_THROWS_WITH_AS(store.flush(), doctest::Contains("disk full"), std::runtime_error);
    CHECK_NOTHROW(store.flush()); // reported once
    {
        const std::scoped_lock lock(state->mutex);
        state->fail = false;
    }
    store.write("slot1", Json{{"gold", 6}}, Json::object());
    CHECK_NOTHROW(store.flush());
    CHECK(store.read("slot1")->data.at("gold") == 6);
    CHECK(store.pending() == 0);
}

TEST_CASE("a backend that cannot be read while it writes is never called twice at once") {
    const auto state = std::make_shared<GatedBackend::State>();
    QueuedSaveBackend queue(std::make_unique<GatedBackend>(state, false));
    for (int i = 0; i < 20; ++i) {
        queue.write("saves/slot" + std::to_string(i % 3) + ".json", std::to_string(i), true);
        // Reads of keys the queue does not hold reach the backend, each waiting its turn.
        CHECK_FALSE(queue.read("saves/other.json", false).has_value());
        CHECK(queue.list("saves").size() <= 3);
    }
    queue.flush();
    const std::scoped_lock lock(state->mutex);
    CHECK(state->overlapping == 0);
    CHECK(state->files.at("saves/slot1.json") == "19");
    CHECK(state->backups.at("saves/slot1.json") == "16");
}
