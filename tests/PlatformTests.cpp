#include "engine/platform/Display.h"
#include "engine/runtime/Engine.h"
#include "engine/runtime/Headless.h"
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
    o.libraryRoot = FENGINE_LUALIB;
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
    write(root / "bin" / "lualib" / "fe" / "init.lua", "return {}");
    CHECK(findLibraryRoot(root / "bin" / "fengine") ==
          std::filesystem::weakly_canonical(root / "bin" / "lualib"));
    write(root / "Game.app" / "Contents" / "Resources" / "lualib" / "fe" / "init.lua", "return {}");
    CHECK(findLibraryRoot(root / "Game.app" / "Contents" / "MacOS" / "fengine") ==
          std::filesystem::weakly_canonical(root / "Game.app" / "Contents" / "Resources" / "lualib"));
    std::filesystem::remove_all(root);
}
