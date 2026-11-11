#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/platform/Display.h"
#include "engine/runtime/Engine.h"
#include "engine/runtime/Headless.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <map>
#include <set>
#include <thread>

using namespace fe;

namespace {
void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

EngineOptions options(const std::filesystem::path& userData = {}) {
    EngineOptions o;
    o.gameRoot = FENGINE_TEST_GAME;
    o.builtinRoot = FENGINE_BUILTIN;
    o.userData = userData;
    o.seed = 7;
    return o;
}

std::string run(Engine& engine, const std::string& code) {
    const auto result = engine.lua().evaluate(code);
    CAPTURE(code);
    CAPTURE(result.output);
    CHECK(result.ok);
    return result.output;
}

std::string failure(Engine& engine, const std::string& code) {
    const auto result = engine.lua().evaluate(code);
    CAPTURE(code);
    CHECK_FALSE(result.ok);
    return result.output;
}

class MemoryPlatform final : public PlatformServices {
public:
    [[nodiscard]] std::string name() const override { return "memory"; }
    void unlock(const std::string& id) override { unlocks.insert(id); }
    [[nodiscard]] bool unlocked(const std::string& id) const override { return unlocks.contains(id); }
    void setStat(const std::string& stat, double value) override { stats[stat] = value; }
    [[nodiscard]] std::optional<double> stat(const std::string& stat) const override {
        const auto found = stats.find(stat);
        return found == stats.end() ? std::nullopt : std::optional<double>(found->second);
    }
    void update() override { ++updates; }
    std::set<std::string> unlocks;
    std::map<std::string, double> stats;
    int updates = 0;
};

#if !defined(_WIN32)
class TimeZone {
public:
    explicit TimeZone(const char* zone) {
        if (const char* current = std::getenv("TZ")) {
            previous_ = current;
        }
        setenv("TZ", zone, 1);
        tzset();
    }
    ~TimeZone() {
        if (previous_) {
            setenv("TZ", previous_->c_str(), 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }
    TimeZone(const TimeZone&) = delete;
    TimeZone& operator=(const TimeZone&) = delete;

private:
    std::optional<std::string> previous_;
};
#endif
} // namespace

TEST_CASE("text input is queued until the tick and composition follows the IME") {
    InputState input;
    CHECK_FALSE(input.takeTextEntryChange());
    input.beginText(glm::vec4(10, 20, 100, 12), "name");
    const auto change = input.takeTextEntryChange();
    REQUIRE(change);
    CHECK(change->active);
    CHECK(change->kind == "name");
    CHECK(change->rect->z == 100);
    CHECK_FALSE(input.takeTextEntryChange());

    input.feedText("Ka");
    input.feedText("i");
    input.feedComposition("ほん", 1);
    CHECK(input.text().empty());
    input.tick(1.0 / 60);
    CHECK(input.text() == "Kai");
    CHECK(input.composition() == "ほん");
    CHECK(input.compositionCursor() == 1);
    input.tick(1.0 / 60);
    CHECK(input.text().empty());
    CHECK(input.composition() == "ほん");

    input.endText();
    CHECK(input.composition().empty());
    CHECK_FALSE(input.takeTextEntryChange()->active);
    input.endText();
    CHECK_FALSE(input.takeTextEntryChange());
}

TEST_CASE("numbered gamepads' sticks are analog and report their pad's layout") {
    InputState input;
    input.connectPad({2, "DualSense Wireless Controller", "playstation", true, true});
    input.feed("pad2:leftx", 0.9f);
    input.feed("pad2:a", 1);
    input.tick(1.0 / 60);
    CHECK(input.pressedInputs() == std::vector<std::string>{"pad2:a"});
    CHECK(input.lastDevice() == "gamepad");
    CHECK(input.deviceType() == "playstation");
    CHECK(input.deviceName() == "DualSense Wireless Controller");
    input.feed("key:Z", 1);
    CHECK(input.deviceType() == "keyboard");
    input.feed("pad:b", 1);
    CHECK(input.deviceType() == "playstation");
    input.disconnectPad(2);
    CHECK(input.pads().empty());
}

TEST_CASE("the device in use follows deliberate input, not releases, drift or a nudged mouse") {
    InputState input;
    input.tick(1.0 / 60);
    CHECK(input.deviceType() == "keyboard");
    CHECK_FALSE(input.deviceChanged());

    // Plugging a gamepad in switches to it, reported on the next tick only.
    input.connectPad({1, "Xbox Wireless Controller", "xbox", true, false});
    CHECK(input.deviceType() == "xbox");
    input.tick(1.0 / 60);
    CHECK(input.deviceChanged());
    input.tick(1.0 / 60);
    CHECK_FALSE(input.deviceChanged());

    // A key going down switches to the keyboard; letting it go later does not switch back.
    input.feed("key:Z", 1);
    input.tick(1.0 / 60);
    CHECK(input.deviceType() == "keyboard");
    CHECK(input.deviceChanged());
    input.feed("pad1:a", 1);
    CHECK(input.deviceType() == "xbox");
    input.feed("key:Z", 0);
    CHECK(input.deviceType() == "xbox");

    // A stick resting off-centre (drift) below half way never takes the prompts...
    input.feed("key:X", 1);
    CHECK(input.deviceType() == "keyboard");
    for (int i = 0; i < 20; ++i) {
        input.feed("pad1:leftx", 0.34f + 0.01f * static_cast<float>(i % 3));
    }
    CHECK(input.deviceType() == "keyboard");
    // ...a real push does, once: held there, it doesn't fight a later key press.
    input.feed("pad1:leftx", 0.8f);
    CHECK(input.deviceType() == "xbox");
    input.feed("key:X", 1);
    input.feed("pad1:leftx", 0.82f);
    input.feed("pad1:leftx", 0.79f);
    CHECK(input.deviceType() == "keyboard");
    // Back at rest and pushed again, it switches again.
    input.feed("pad1:leftx", 0.0f);
    input.feed("pad1:leftx", 0.9f);
    CHECK(input.deviceType() == "xbox");

    // A nudged mouse does not take over from a gamepad; moving it away does.
    input.feedMouse({100, 100});
    input.feed("pad1:b", 1);
    CHECK(input.lastDevice() == "gamepad");
    for (int i = 0; i < 30; ++i) {
        input.feedMouse({100.0f + static_cast<float>(i % 2), 100.0f});
    }
    CHECK(input.lastDevice() == "gamepad");
    input.feedMouse({140, 100});
    CHECK(input.lastDevice() == "mouse");
    CHECK(input.deviceType() == "keyboard");
}

TEST_CASE("unplugging the gamepad in use falls back to another gamepad, then to the keyboard") {
    InputState input;
    input.connectPad({1, "Xbox Wireless Controller", "xbox", true, false});
    input.connectPad({2, "DualSense Wireless Controller", "playstation", true, true});
    CHECK(input.deviceType() == "playstation"); // the one plugged in last
    input.feed("pad1:a", 1);
    CHECK(input.deviceType() == "xbox");
    input.disconnectPad(2); // not the one in use: nothing changes
    CHECK(input.deviceType() == "xbox");
    input.connectPad({2, "DualSense Wireless Controller", "playstation", true, true});
    input.disconnectPad(2); // the one in use: the other pad takes over
    CHECK(input.deviceType() == "xbox");
    CHECK(input.deviceName() == "Xbox Wireless Controller");
    input.tick(1.0 / 60);
    input.disconnectPad(1);
    CHECK(input.lastDevice() == "keyboard");
    CHECK(input.deviceType() == "keyboard");
    input.tick(1.0 / 60);
    CHECK(input.deviceChanged());
}

TEST_CASE("input labels come from the runtime's keyboard layout, else from the input's name") {
    InputState input;
    CHECK(input.label("key:Z") == "Z");
    CHECK(input.label("pad:a") == "a");
    input.setLabeler(
        [](const std::string& name) { return name == "key:Z" ? std::string("Y") : std::string(); });
    CHECK(input.label("key:Z") == "Y"); // a German layout
    CHECK(input.label("key:Return") == "Return");
}

TEST_CASE("text, devices and rumble are scriptable") {
    Engine engine(options());
    engine.input().connectPad({1, "Xbox Wireless Controller", "xbox", true, false});
    run(engine, "fe.input.text_begin({x = 8, y = 100, w = 200, h = 16}, 'name')");
    CHECK(engine.input().textEntry().rect->x == 8);
    CHECK(run(engine, "fe.input.text_active()") == "true");
    run(engine, "fe.input.feed_text('Ada') fe.input.feed_text('漢', 1)");
    CHECK(run(engine, "fe.input.text()").empty());
    engine.tick(1.0 / 60);
    CHECK(run(engine, "fe.input.text()") == "Ada");
    CHECK(run(engine, "fe.input.composition()") == "漢\t1");
    run(engine, "fe.input.text_end()");
    CHECK(run(engine, "fe.input.text_active()") == "false");
    CHECK(failure(engine, "fe.input.text_begin(nil, 'email')").find("Unknown text kind") !=
          std::string::npos);

    CHECK(run(engine, "fe.input.device_type()") == "xbox"); // plugging a gamepad in is picking it up
    run(engine, "fe.input.feed('key:Z', 1)");
    CHECK(run(engine, "fe.input.device_type()") == "keyboard");
    run(engine, "fe.input.feed('pad1:a', 1)");
    CHECK(run(engine, "fe.input.device_type(), fe.input.device_name()") == "xbox\tXbox Wireless Controller");
    CHECK(run(engine, "fe.input.device_type(1), fe.input.device_type(2), fe.input.device_name(2)") ==
          "xbox\tnil\tnil");
    CHECK(run(engine, "local p = fe.input.pads()[1] return p.number, p.type, p.rumble, p.trigger_rumble") ==
          "1\txbox\ttrue\tfalse");

    run(engine, "fe.input.rumble(0.5, 1, 0.25) fe.input.rumble_triggers(1, 0, 60, 1)");
    const auto requests = engine.input().takeRumble();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].pad == 0);
    CHECK(requests[0].high == 1);
    CHECK(requests[0].seconds == doctest::Approx(0.25));
    CHECK(requests[1].triggers);
    CHECK(requests[1].pad == 1);
    CHECK(requests[1].seconds == 60);
    CHECK(engine.input().takeRumble().empty());
    CHECK(failure(engine, "fe.input.rumble(1, 1, 1, 0)").find("start at 1") != std::string::npos);
    CHECK(failure(engine, "fe.input.rumble(0.5, 2, 1)").find("high must be from 0 to 1") !=
          std::string::npos);
    CHECK(failure(engine, "fe.input.rumble_triggers(1, 0, 90)").find("seconds must be from 0 to 60") !=
          std::string::npos);
}

TEST_CASE("display settings validate, merge and read back") {
    Engine engine(options());
    CHECK(run(engine,
              "local d = fe.app.display() return d.fullscreen, d.width, d.height, d.vsync, d.monitor, "
              "#d.monitors, #d.modes") == "false\t" + std::to_string(engine.config().windowWidth) + "\t" +
                                              std::to_string(engine.config().windowHeight) + "\ton\t1\t1\t1");
    // Headless there is no back buffer, so none is HDR; the runtime sets the flag from bgfx's format.
    CHECK(run(engine, "return tostring(fe.app.display().hdr)") == "false");
    engine.display().hdr = true;
    CHECK(run(engine, "return tostring(fe.app.display().hdr)") == "true");
    // Likewise the pixel density: 1 headless, what the window reports in the runtime.
    CHECK(run(engine, "return fe.app.display().scale") == "1.0");
    engine.display().scale = 2;
    CHECK(run(engine, "return fe.app.display().scale") == "2.0");
    run(engine, "fe.app.set_display{scale = 3}"); // not a setting: ignored like hdr
    CHECK(run(engine, "return fe.app.display().scale") == "2.0");
    run(engine, "fe.app.set_display{width = 1600, height = 900, vsync = 'adaptive'}");
    run(engine, "fe.app.set_display{fullscreen = true}");
    CHECK(run(engine, "local d = fe.app.display() return d.fullscreen, d.width, d.vsync") ==
          "borderless\t1600\tadaptive");
    const auto& request = *engine.requests().display;
    CHECK(request.width == 1600);
    CHECK(request.fullscreen == FullscreenMode::Borderless);
    CHECK(request.vsync == VsyncMode::Adaptive);
    CHECK_FALSE(request.monitor);
    run(engine, "fe.app.set_fullscreen('exclusive') fe.app.set_display{vsync = false}");
    CHECK(run(engine, "local d = fe.app.display() return d.fullscreen, d.vsync") == "exclusive\toff");
    run(engine, "fe.app.set_fullscreen(false)");
    CHECK(engine.requests().display->fullscreen == FullscreenMode::Window);

    CHECK(failure(engine, "fe.app.set_display{monitor = 2}").find("Monitor 2 does not exist") !=
          std::string::npos);
    CHECK(failure(engine, "fe.app.set_display{vsync = 'sometimes'}").find("Unknown vsync mode") !=
          std::string::npos);
    CHECK(failure(engine, "fe.app.set_display{width = 10}").find("64 to 16384") != std::string::npos);
    CHECK(failure(engine, "fe.app.set_display{height = 720.5}").find("integer") != std::string::npos);
    CHECK(failure(engine, "fe.app.set_display{fullscreen = 'kiosk'}").find("Unknown fullscreen mode") !=
          std::string::npos);
}

TEST_CASE("adaptive vsync turns off when frames miss the display and back on when they keep up") {
    AdaptiveVsync adaptive;
    bool synced = true;
    for (int i = 0; i < 29; ++i) {
        synced = adaptive.update(1.0 / 60, 60, synced);
    }
    CHECK(synced);
    for (int i = 0; i < 40 && synced; ++i) {
        synced = adaptive.update(2.0 / 60, 60, synced);
    }
    CHECK_FALSE(synced);
    for (int i = 0; i < 40 && !synced; ++i) {
        synced = adaptive.update(1.2 / 60, 60, synced);
    }
    CHECK_FALSE(synced);
    for (int i = 0; i < 40 && !synced; ++i) {
        synced = adaptive.update(0.5 / 60, 60, synced);
    }
    CHECK(synced);
}

// Ticks until `done` or a generous wall-clock deadline: background loads
// finish on worker threads, so tick counts alone depend on the build's speed.
template<typename Done> void tickUntil(Engine& engine, Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        engine.tick(1.0 / 60);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

TEST_CASE("worlds load in the background and finish on the main thread") {
    Engine engine(options());
    const auto ticket = std::stoi(run(engine, "return fe.world.load_async('field', 'arena')"));
    double progress = 0;
    tickUntil(engine, [&] {
        const double now = engine.loadProgress(ticket);
        CHECK(now >= progress);
        progress = now;
        return progress >= 1;
    });
    CHECK(progress == 1);
    CHECK_FALSE(engine.world("field"));
    CHECK(run(engine, "local w = fe.world.finish_load(" + std::to_string(ticket) +
                          ") return w.map_id, w.name") == "arena\tfield");
    REQUIRE(engine.world("field"));
    CHECK(failure(engine, "fe.world.load_progress(" + std::to_string(ticket) + ")")
              .find("Unknown world load") != std::string::npos);

    // Finishing straight away waits for the map file.
    CHECK(run(engine, "return fe.world.finish_load(fe.world.load_async('second', 'arena')).map_id") ==
          "arena");
    CHECK(failure(engine, "fe.world.load_async('x', 'Bad Id')").find("Invalid map id") != std::string::npos);
    const auto missing = run(engine, "return fe.world.load_async('x', 'nowhere')");
    tickUntil(engine, [&] { return engine.loadProgress(std::stoi(missing)) >= 1; });
    CHECK(failure(engine, "fe.world.finish_load(" + missing + ")").find("nowhere") != std::string::npos);
    CHECK_FALSE(engine.world("x"));
}

TEST_CASE("cancelled background loads are dropped and never create a world") {
    Engine engine(options());
    // Cancelled at once, while the worker may still be parsing: the ticket is
    // gone and the parse's result is discarded once it arrives.
    const auto ticket = run(engine, "return fe.world.load_async('field', 'arena')");
    run(engine, "fe.world.cancel_load(" + ticket + ")");
    CHECK(failure(engine, "fe.world.load_progress(" + ticket + ")").find("Unknown world load") !=
          std::string::npos);
    CHECK(failure(engine, "fe.world.finish_load(" + ticket + ")").find("Unknown world load") !=
          std::string::npos);
    CHECK(failure(engine, "fe.world.cancel_load(" + ticket + ")").find("Unknown world load") !=
          std::string::npos);
    for (int i = 0; i < 30; ++i) {
        engine.tick(1.0 / 60);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(engine.world("field"));
    // A load that already finished decoding can be cancelled too, and the
    // same world still loads afterwards.
    const auto later = std::stoi(run(engine, "return fe.world.load_async('field', 'arena')"));
    tickUntil(engine, [&] { return engine.loadProgress(later) >= 1; });
    engine.cancelLoad(later);
    CHECK_THROWS_WITH_AS(engine.finishLoad(later), doctest::Contains("Unknown world load"),
                         std::runtime_error);
    CHECK(run(engine, "return fe.world.finish_load(fe.world.load_async('field', 'arena')).map_id") ==
          "arena");
    REQUIRE(engine.world("field"));
}

TEST_CASE("local dates follow the time zone") {
    Engine engine(options());
    CHECK(run(engine,
              "local d = fe.time.local_date() return d.year >= 2024 and d.month >= 1 and d.weekday <= 7") ==
          "true");
    CHECK(failure(engine, "fe.time.local_date('noon')").find("seconds since the Unix epoch") !=
          std::string::npos);
#if !defined(_WIN32)
    {
        TimeZone utc("UTC0");
        CHECK(run(engine,
                  "local d = fe.time.local_date(0) return d.year, d.month, d.day, d.hour, d.min, d.sec, "
                  "d.weekday, d.utc_offset") == "1970\t1\t1\t0\t0\t0\t5\t0");
    }
    {
        TimeZone tokyo("JST-9");
        CHECK(run(engine,
                  "local d = fe.time.local_date(1700000000.75) return d.year, d.month, d.day, d.hour, "
                  "d.min, d.sec, d.weekday, d.utc_offset") == "2023\t11\t15\t7\t13\t20\t4\t32400");
    }
    {
        TimeZone pacific("PST8PDT,M3.2.0,M11.1.0");
        CHECK(run(engine, "return fe.time.local_date(1720000000).utc_offset, "
                          "fe.time.local_date(1700000000).utc_offset") == "-25200\t-28800");
    }
#endif
}

TEST_CASE("the locale comes from the runtime") {
    Engine headless(options());
    CHECK(run(headless, "fe.app.locale()") == "en-US");
    auto japanese = options();
    japanese.locale = "ja-JP";
    Engine engine(std::move(japanese));
    CHECK(run(engine, "fe.app.locale()") == "ja-JP");
}

TEST_CASE("the default platform backend keeps unlocks and stats in user data") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-platform-test";
    std::filesystem::remove_all(folder);
    {
        Engine engine(options(folder));
        CHECK(run(engine, "fe.platform.name(), fe.platform.unlocked('first_steps')") == "local\tfalse");
        run(engine, "fe.platform.unlock('first_steps') fe.platform.unlock('first_steps')");
        run(engine, "fe.platform.set_stat('steps', 120) fe.platform.set_stat('steps', 125)");
        CHECK(run(engine, "fe.platform.unlocked('first_steps'), fe.platform.stat('steps'), "
                          "fe.platform.stat('never')") == "true\t125.0\tnil");
        CHECK(std::filesystem::exists(folder / "platform.json"));
        CHECK(failure(engine, "fe.platform.unlock('')").find("1 to 128") != std::string::npos);
        CHECK(failure(engine, "fe.platform.set_stat('x', 0/0)").find("finite") != std::string::npos);
    }
    const auto stored = readJson(folder / "platform.json");
    CHECK(stored["stats"]["steps"] == 125);
    CHECK(stored["unlocked"].contains("first_steps"));
    {
        Engine engine(options(folder));
        CHECK(run(engine, "fe.platform.unlocked('first_steps'), fe.platform.stat('steps')") == "true\t125.0");
    }
    std::filesystem::remove_all(folder);
}

TEST_CASE("a port can plug in its own platform backend") {
    auto custom = options();
    auto backend = std::make_unique<MemoryPlatform>();
    auto* platform = backend.get();
    custom.platform = std::move(backend);
    Engine engine(std::move(custom));
    run(engine, "fe.platform.unlock('boss_1') fe.platform.set_stat('time', 3.5)");
    engine.tick(1.0 / 60);
    CHECK(platform->unlocks.contains("boss_1"));
    CHECK(platform->stats["time"] == 3.5);
    CHECK(platform->updates == 1);
    CHECK(run(engine, "fe.platform.name()") == "memory");
}

namespace {
class UpperClipboard final : public Clipboard {
public:
    [[nodiscard]] std::optional<std::string> read() override { return text; }
    void write(const std::string& value) override {
        text = value;
        for (auto& c : *text) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    std::optional<std::string> text;
};
} // namespace

TEST_CASE("the clipboard is a string of the engine's own unless the runtime supplies one") {
    Engine engine(options());
    CHECK(run(engine, "return fe.app.clipboard()") == "nil");
    run(engine, "fe.app.set_clipboard('Aria')");
    CHECK(run(engine, "return fe.app.clipboard()") == "Aria");
    run(engine, "fe.app.set_clipboard('')");
    CHECK(run(engine, "return fe.app.clipboard()") == "nil");
    CHECK(failure(engine, "fe.app.set_clipboard(string.rep('x', 64 * 1024 + 1))").find("64 KiB") !=
          std::string::npos);
    run(engine, "fe.app.set_clipboard(string.rep('x', 64 * 1024))");
    CHECK(run(engine, "return #fe.app.clipboard()") == "65536");
    // Text another program put there is cut at a character boundary.
    engine.clipboard().write(std::string(MaxClipboardBytes - 1, 'a') + "\xe6\xbc\xa2" + "b");
    CHECK(run(engine, "local c = fe.app.clipboard() return #c, c:sub(-1)") ==
          std::to_string(MaxClipboardBytes - 1) + "\ta");

    auto custom = options();
    auto backend = std::make_unique<UpperClipboard>();
    auto* clipboard = backend.get();
    custom.clipboard = std::move(backend);
    Engine port(std::move(custom));
    run(port, "fe.app.set_clipboard('quiet')");
    CHECK(clipboard->text == "QUIET");
    CHECK(run(port, "return fe.app.clipboard()") == "QUIET");
}

TEST_CASE("bindings read the tables scripts pass raw, never running their metamethods") {
    Engine engine(options());
    run(engine, "touched = 0; local function touch(_, key) touched = touched + 1; error('consulted ' .. "
                "tostring(key)) end; hostile = setmetatable({}, {__index = touch, __len = touch})");
    for (const char* code :
         {"fe.screen.set(hostile)", "fe.input.define_axis('move', hostile)", "fe.app.set_display(hostile)",
          "fe.audio.sound('a.ogg', hostile)", "fe.input.text_begin(hostile)",
          "fe.input.define('confirm', hostile)", "fe.random.new(1):pick(hostile)"}) {
        CAPTURE(code);
        run(engine, std::string("pcall(function() ") + code + " end)");
    }
    CHECK(run(engine, "touched") == "0");
    CHECK(run(engine, "#fe.input.bindings('confirm')") == "0");
    CHECK(failure(engine, "fe.input.define('confirm', {'key:Z', 4})").find("list of strings") !=
          std::string::npos);
    // Fields inherited through __index are not seen: options must be in the table itself.
    run(engine, "fe.screen.set(setmetatable({}, {__index = {letterbox = 0.25}}))");
    CHECK(engine.screen().letterbox == doctest::Approx(0));
    run(engine, "fe.screen.set{letterbox = 0.25}");
    CHECK(engine.screen().letterbox == doctest::Approx(0.25));
}

TEST_CASE("the entry module's functions are read raw") {
    const auto game = std::filesystem::temp_directory_path() / "fengine-strict-main";
    std::filesystem::remove_all(game);
    std::filesystem::create_directories(game);
    std::filesystem::copy_file(std::filesystem::path(FENGINE_TEST_GAME) / "game.json", game / "game.json");
    write(game / "main.lua",
          "return setmetatable({}, {__index = function(_, k) error('strict: ' .. k) end})");
    auto o = options();
    o.gameRoot = game;
    Engine engine(std::move(o));
    CHECK(engine.start());
    engine.tick(1.0 / 60);
    engine.draw(1);
    CHECK(engine.lua().errors().empty());
    std::filesystem::remove_all(game);
}

TEST_CASE("the engine library is found next to the executable") {
    const auto root = std::filesystem::temp_directory_path() / "fengine-library-root";
    std::filesystem::remove_all(root);
    write(root / "bin" / "builtin" / "assets" / "weather.json", "{}");
    CHECK(findBuiltinRoot(root / "bin" / "fengine") ==
          std::filesystem::weakly_canonical(root / "bin" / "builtin"));
    write(root / "Game.app" / "Contents" / "Resources" / "builtin" / "assets" / "weather.json", "{}");
    CHECK(findBuiltinRoot(root / "Game.app" / "Contents" / "MacOS" / "fengine") ==
          std::filesystem::weakly_canonical(root / "Game.app" / "Contents" / "Resources" / "builtin"));
    std::filesystem::remove_all(root);
}

TEST_CASE("game ids are stable folder names derived from the title or given in game.json") {
    CHECK(derivedGameId("Ridgeline") == "ridgeline");
    CHECK(derivedGameId("Crystal of the Vale") == "crystal-of-the-vale");
    CHECK(derivedGameId("  Two   Words ") == "two-words");
    CHECK(derivedGameId("snake_case-ok 9") == "snake_case-ok-9");
    CHECK_FALSE(derivedGameId("Final Fantasy VI — Narshe"));
    CHECK_FALSE(derivedGameId("Dragon's Lair"));
    CHECK_FALSE(derivedGameId("   "));
    CHECK(validGameId("ff6-narshe"));
    CHECK_FALSE(validGameId("Bad Id"));
    CHECK_FALSE(validGameId(""));
    CHECK_FALSE(validGameId(std::string(65, 'a')));
    CHECK(GameConfig::load(FENGINE_TEST_GAME).id == "fengine-tests");

    const auto root = std::filesystem::temp_directory_path() / "fengine-config-id";
    write(root / "game.json", R"({"title": "Dragon's Lair"})");
    CHECK_THROWS_WITH(GameConfig::load(root), doctest::Contains("needs an \"id\""));
    write(root / "game.json", R"({"title": "Dragon's Lair", "id": "dragons-lair"})");
    CHECK(GameConfig::load(root).id == "dragons-lair");
    write(root / "game.json", R"({"title": "Plain", "id": "Not Valid"})");
    CHECK_THROWS_WITH(GameConfig::load(root), doctest::Contains("id must be"));
    write(root / "game.json", R"({"title": "Plain", "max_fps": 144, "canvas_fit": "expand"})");
    const auto config = GameConfig::load(root);
    CHECK(config.maxFps == 144);
    CHECK(config.canvasFit == CanvasFit::Expand);
    CHECK(GameConfig::load(FENGINE_TEST_GAME).maxFps == 0);
    CHECK(GameConfig::load(FENGINE_TEST_GAME).canvasFit == CanvasFit::Letterbox);
    write(root / "game.json", R"({"title": "Plain", "max_fps": -1})");
    CHECK_THROWS_WITH(GameConfig::load(root), doctest::Contains("max_fps"));
    write(root / "game.json", R"({"title": "Plain", "canvas_fit": "stretch"})");
    CHECK_THROWS_WITH(GameConfig::load(root), doctest::Contains("canvas_fit"));
    std::filesystem::remove_all(root);
}

TEST_CASE("canvas_fit expand widens the canvas to the viewport; letterbox keeps it") {
    auto config = GameConfig::load(FENGINE_TEST_GAME);
    REQUIRE(config.virtualWidth == 320);
    REQUIRE(config.virtualHeight == 180);
    {
        Engine engine(options(), config);
        engine.setViewport(2560, 1080);
        CHECK(engine.canvas().width() == 320);
        CHECK(engine.canvas().height() == 180);
    }
    config.canvasFit = CanvasFit::Expand;
    Engine engine(options(), config);
    engine.setViewport(1280, 720);
    CHECK(engine.canvas().width() == 320);
    engine.setViewport(2560, 1080);
    CHECK(engine.canvas().width() == 426); // floor(180 * 2560 / 1080)
    CHECK(engine.canvas().height() == 180);
    CHECK(engine.canvas().present({2560, 1080}).scale == doctest::Approx(6));
    // A narrower window keeps the configured width (bars above and below).
    engine.setViewport(720, 1280);
    CHECK(engine.canvas().width() == 320);
    CHECK(engine.canvas().height() == 180);
}

TEST_CASE("the log writes a rotated file, filters by level and reports crashes") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-log-test";
    std::filesystem::remove_all(folder);
    const auto previousLevel = logLevel();
    openLogFile(folder);
    CHECK(logFilePath() == folder / "log.txt");
    setLogLevel(LogLevel::Warning);
    log(LogLevel::Info, "quiet info line");
    log(LogLevel::Warning, "loud warning line");
    log(LogLevel::Error, "loud error line");
    setLogLevel(previousLevel);
    const auto recent = recentLogLines(2);
    REQUIRE(recent.size() == 2);
    CHECK(recent[0].find("loud warning line") != std::string::npos);
    CHECK(recent[1].find("loud error line") != std::string::npos);
    flushLog();
    const auto first = readFile(folder / "log.txt");
    CHECK(first.find("quiet info line") == std::string::npos);
    CHECK(first.find("loud warning line") != std::string::npos);
    CHECK(first.find("error   loud error line") != std::string::npos);
    const auto report = writeCrashReport(folder, "crash: SIGSEGV");
    const auto crash = readFile(report);
    CHECK(crash.starts_with("crash: SIGSEGV\n"));
    CHECK(crash.find("loud error line") != std::string::npos);
    // Opening again rotates the previous file once.
    openLogFile(folder);
    log(LogLevel::Error, "second run");
    flushLog();
    CHECK(readFile(folder / "log.txt.1") == first);
    CHECK(readFile(folder / "log.txt").find("second run") != std::string::npos);
    closeLogFile();
    CHECK(logFilePath().empty());
    std::filesystem::remove_all(folder);
}

TEST_CASE("a log sink sees every record that passes the filter, and never its own") {
    std::vector<std::pair<LogLevel, std::string>> seen;
    const auto previousLevel = logLevel();
    setLogLevel(LogLevel::Info);
    setLogSink([&](LogLevel level, std::string_view message) {
        seen.emplace_back(level, std::string(message));
        if (message == "forwarded") {
            log(LogLevel::Error, "from the sink"); // written, but not fed back
        }
    });
    log(LogLevel::Debug, "dropped by the level");
    log(LogLevel::Info, "forwarded");
    log(LogLevel::Warning, "and this");
    setLogSink({});
    log(LogLevel::Error, "after the sink was removed");
    setLogLevel(previousLevel);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].first == LogLevel::Info);
    CHECK(seen[0].second == "forwarded");
    CHECK(seen[1].first == LogLevel::Warning);
    CHECK(seen[1].second == "and this");
    const auto recent = recentLogLines(3);
    REQUIRE(recent.size() == 3);
    CHECK(recent[0].find("from the sink") != std::string::npos);
    CHECK(recent[2].find("after the sink was removed") != std::string::npos);
    // EngineOptions::logSink installs it for the engine's lifetime.
    {
        auto withSink = options();
        withSink.logSink = [&](LogLevel level, std::string_view message) {
            seen.emplace_back(level, std::string(message));
        };
        Engine engine(std::move(withSink));
        seen.clear();
        log(LogLevel::Warning, "while the engine lives");
        REQUIRE(seen.size() == 1);
        CHECK(seen[0].second == "while the engine lives");
    }
    seen.clear();
    log(LogLevel::Warning, "after the engine");
    CHECK(seen.empty());
}

TEST_CASE("the frame cap spaces frame starts one interval apart without drift") {
    FrameCap cap;
    CHECK(cap.maxFps() == 0);
    CHECK(cap.wait(1.0) == 0);
    cap.setMaxFps(100);
    CHECK(cap.maxFps() == 100);
    CHECK(cap.interval() == doctest::Approx(0.01));
    CHECK(cap.wait(10.0) == 0); // the first frame starts at once
    CHECK(cap.wait(10.004) == doctest::Approx(0.006));
    // Deadlines advance by whole intervals: a frame that starts late does not
    // push the next one back.
    CHECK(cap.wait(10.0125) == doctest::Approx(0.0075));
    CHECK(cap.wait(10.03) == 0);
    // Far behind (more than an interval late): start now and restart the schedule.
    CHECK(cap.wait(10.5) == 0);
    CHECK(cap.wait(10.505) == doctest::Approx(0.005));
    cap.setMaxFps(0);
    CHECK(cap.wait(11.0) == 0);
}

TEST_CASE("stat values that are not numbers are overwritten, and writes are coalesced") {
    const auto folder = std::filesystem::temp_directory_path() / "fengine-platform-stat-test";
    std::filesystem::remove_all(folder);
    const auto file = folder / "platform.json";
    writeJson(file, {{"unlocked", Json::object()}, {"stats", {{"steps", "many"}, {"jumps", 2}}}});
    {
        LocalPlatformServices platform(file);
        CHECK_FALSE(platform.stat("steps"));
        CHECK_NOTHROW(platform.setStat("steps", 3));
        CHECK(platform.stat("steps") == 3);
        CHECK(platform.dirty());
        platform.update(); // the first write happens at once
        CHECK_FALSE(platform.dirty());
        CHECK(readJson(file)["stats"]["steps"] == 3);
        platform.setStat("steps", 4);
        platform.update(); // within the 5 s coalescing window: not yet written
        CHECK(platform.dirty());
        CHECK(readJson(file)["stats"]["steps"] == 3);
        platform.flush();
        CHECK_FALSE(platform.dirty());
        CHECK(readJson(file)["stats"]["steps"] == 4);
        platform.setStat("steps", 5);
    }
    CHECK(readJson(file)["stats"]["steps"] == 5); // written on shutdown
    LocalPlatformServices quick(file, std::chrono::milliseconds(0));
    quick.setStat("jumps", 3);
    quick.update();
    CHECK(readJson(file)["stats"]["jumps"] == 3);
    std::filesystem::remove_all(folder);
}

TEST_CASE("programs locate their own executable through the operating system") {
    const auto self = executablePath("not-a-real-path");
    CHECK(self.is_absolute());
    CHECK(std::filesystem::is_regular_file(self));
    CHECK(self.filename().string().find("tests") != std::string::npos);
    const auto fallback = executablePath("relative/fallback");
    CHECK(fallback.is_absolute());
}

TEST_CASE("the engine keeps the loaded worlds' assets and evicts the rest on unload") {
    auto o = options();
    o.assetBudget = 0;
    Engine engine(std::move(o));
    const auto world = engine.loadWorld("field", "arena");
    const auto geometry = world->geometryPath();
    engine.assets().model(geometry);
    engine.assets().image("@fe/assets/ui/cursor.png");
    const auto referenced = engine.referencedAssets();
    CHECK(referenced.contains(geometry));
    CHECK_FALSE(referenced.contains("@fe/assets/ui/cursor.png"));
    const auto generation = engine.assets().generation();
    CHECK(engine.releaseUnusedAssets() == 1);
    CHECK(engine.assets().loaded(geometry));
    CHECK_FALSE(engine.assets().loaded("@fe/assets/ui/cursor.png"));
    CHECK(engine.assets().generation() > generation);
    engine.unloadWorld("field");
    CHECK_FALSE(engine.assets().loaded(geometry));
    CHECK(engine.assets().cachedCount() == 0);
    // With the default budget nothing is evicted.
    Engine roomy(options());
    roomy.loadWorld("field", "arena");
    roomy.assets().image("@fe/assets/ui/cursor.png");
    roomy.unloadWorld("field");
    CHECK(roomy.assets().loaded("@fe/assets/ui/cursor.png"));
}
