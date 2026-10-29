#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/core/Random.h"
#include "engine/runtime/Engine.h"
#include "engine/script/bindings/Convert.h"
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>

namespace fe {
namespace {
struct RandomStream {
    Random random;
};

// Days since 1970-01-01 of a proleptic Gregorian date.
std::int64_t daysFromCivil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

sol::table localDate(sol::state_view lua, std::time_t time) {
    std::tm local{};
#if defined(_WIN32)
    const bool converted = localtime_s(&local, &time) == 0;
#else
    const bool converted = localtime_r(&time, &local) != nullptr;
#endif
    if (!converted) {
        throw std::runtime_error("Time out of range for a local date");
    }
    const auto asUtc = daysFromCivil(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                                     static_cast<unsigned>(local.tm_mday)) *
                           86400 +
                       local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    auto date = lua.create_table();
    date["year"] = local.tm_year + 1900;
    date["month"] = local.tm_mon + 1;
    date["day"] = local.tm_mday;
    date["hour"] = local.tm_hour;
    date["min"] = local.tm_min;
    date["sec"] = local.tm_sec;
    date["weekday"] = local.tm_wday + 1;
    date["utc_offset"] = asUtc - static_cast<std::int64_t>(time);
    return date;
}

const std::pair<const char*, FullscreenMode> FullscreenModes[] = {{"window", FullscreenMode::Window},
                                                                  {"borderless", FullscreenMode::Borderless},
                                                                  {"exclusive", FullscreenMode::Exclusive}};
const std::pair<const char*, VsyncMode> VsyncModes[] = {
    {"off", VsyncMode::Off}, {"on", VsyncMode::On}, {"adaptive", VsyncMode::Adaptive}};

FullscreenMode fullscreenMode(const sol::object& value) {
    if (value.get_type() == sol::type::boolean) {
        return value.as<bool>() ? FullscreenMode::Borderless : FullscreenMode::Window;
    }
    if (value.get_type() != sol::type::string) {
        throw std::runtime_error("fullscreen is false, true, 'borderless' or 'exclusive'");
    }
    return lua::choice(value.as<std::string>(), FullscreenModes, "fullscreen mode");
}

VsyncMode vsyncMode(const sol::object& value) {
    if (value.get_type() == sol::type::boolean) {
        return value.as<bool>() ? VsyncMode::On : VsyncMode::Off;
    }
    if (value.get_type() != sol::type::string) {
        throw std::runtime_error("vsync is 'on', 'off', 'adaptive' or a boolean");
    }
    return lua::choice(value.as<std::string>(), VsyncModes, "vsync mode");
}

int windowSize(const sol::object& value, const char* what) {
    return static_cast<int>(lua::integer(value, what));
}

DisplayRequest displayRequest(const sol::table& settings) {
    DisplayRequest request;
    if (const auto value = lua::field(settings, "fullscreen"); !lua::absent(value)) {
        request.fullscreen = fullscreenMode(value);
    }
    if (const auto value = lua::field(settings, "vsync"); !lua::absent(value)) {
        request.vsync = vsyncMode(value);
    }
    if (const auto value = lua::field(settings, "width"); !lua::absent(value)) {
        request.width = windowSize(value, "width");
    }
    if (const auto value = lua::field(settings, "height"); !lua::absent(value)) {
        request.height = windowSize(value, "height");
    }
    if (const auto value = lua::field(settings, "monitor"); !lua::absent(value)) {
        request.monitor = windowSize(value, "monitor");
    }
    return request;
}

sol::table modeTable(sol::state_view lua, const DisplayMode& mode) {
    return lua.create_table_with("width", mode.width, "height", mode.height, "refresh", mode.refresh);
}

sol::table displayTable(sol::state_view lua, const DisplayState& display) {
    auto out = lua.create_table();
    if (display.fullscreen == FullscreenMode::Window) {
        out["fullscreen"] = false;
    } else {
        out["fullscreen"] = display.fullscreen == FullscreenMode::Borderless ? "borderless" : "exclusive";
    }
    out["width"] = display.width;
    out["height"] = display.height;
    out["vsync"] = display.vsync == VsyncMode::Off  ? "off"
                   : display.vsync == VsyncMode::On ? "on"
                                                    : "adaptive";
    out["monitor"] = display.monitor;
    auto monitors = lua.create_table();
    for (const auto& monitor : display.monitors) {
        auto entry = modeTable(lua, monitor.desktop);
        entry["name"] = monitor.name;
        monitors.add(entry);
    }
    out["monitors"] = monitors;
    auto modes = lua.create_table();
    if (display.monitor >= 1 && display.monitor <= static_cast<int>(display.monitors.size())) {
        for (const auto& mode : display.monitors[static_cast<std::size_t>(display.monitor - 1)].modes) {
            modes.add(modeTable(lua, mode));
        }
    }
    out["modes"] = modes;
    return out;
}

std::filesystem::path gameFile(const LuaHost& host, const std::string& path) {
    return path.starts_with("@fe/") ? resolveInside(host.libraryRoot() / "fe", path.substr(4))
                                    : resolveInside(host.gameRoot(), path);
}

void bindDev(Engine& engine) {
    auto& host = engine.lua();
    ModuleBinder(host, engine.docs(), "fe.log", "Logging to the console and log output.")
        .fn(
            "info", [](const std::string& m) { log(LogLevel::Info, m); }, "fun(message: string)",
            "Log information.")
        .fn(
            "warn", [](const std::string& m) { log(LogLevel::Warning, m); }, "fun(message: string)",
            "Log a warning.")
        .fn(
            "error", [&host](const std::string& m) { host.report(m); }, "fun(message: string)",
            "Report a script error (shown in the developer overlay).");
    ModuleBinder(host, engine.docs(), "fe.dev",
                 "Development helpers: tracebacks, watches, profiling and errors.")
        .fn(
            "traceback",
            [](sol::this_state state, sol::object thread, sol::object text) {
                lua_State* L = state;
                lua_State* target = L;
                if (thread.get_type() == sol::type::thread) {
                    target = thread.as<sol::thread>().thread_state();
                } else if (thread.get_type() == sol::type::string && lua::absent(text)) {
                    text = thread;
                } else if (!lua::absent(thread)) {
                    throw std::runtime_error("traceback takes a coroutine and/or a message, got " +
                                             lua::describe(thread));
                }
                const auto message = lua::opt<std::string>(text, "message");
                luaL_traceback(L, target, message ? message->c_str() : nullptr, 0);
                std::string result = lua_tostring(L, -1);
                lua_pop(L, 1);
                return result;
            },
            "fun(thread?: thread, message?: string): string",
            "Traceback of a coroutine (or the caller); like debug.traceback, the message may come first.")
        .fn(
            "enabled", [&engine] { return engine.development(); }, "fun(): boolean",
            "True in development builds (editor, hot reload, watchdog).")
        .fn(
            "zone",
            [&engine](const std::string& name, sol::object milliseconds) {
                engine.stats().zones[name] = lua::as<double>(milliseconds, "milliseconds");
            },
            "fun(name: string, milliseconds: number)", "Record a timing shown in the profiler.")
        .fn(
            "clock", [&host] { return host.now(); }, "fun(): number",
            "High-resolution seconds for profiling.")
        .fn(
            "errors",
            [&host](sol::this_state state) {
                sol::state_view lua(state);
                auto list = lua.create_table();
                for (const auto& error : host.errors()) {
                    list.add(lua.create_table_with("message", error.message, "time", error.time));
                }
                return list;
            },
            "fun(): {message: string, time: number}[]", "Recent script errors.")
        .fn("clear_errors", [&host] { host.clearErrors(); }, "fun()", "Clear the recorded script errors.");
    if (engine.devBindings()) {
        ModuleBinder(host, engine.docs(), "fe.dev", "")
            .fn(
                "console",
                [&host](const std::string& code) {
                    const auto result = host.evaluate(code);
                    return std::make_tuple(result.output, result.ok);
                },
                "fun(code: string): string, boolean",
                "Evaluate Lua; returns printable results (or the error) and whether it succeeded. Only in "
                "development builds, --test/--check runs and --replay/--record sessions.");
    }
}

void bindTimeAndRandom(Engine& engine) {
    auto& host = engine.lua();
    ModuleBinder(host, engine.docs(), "fe.time", "Game time.")
        .fn(
            "ticks", [&engine] { return engine.ticks(); }, "fun(): integer", "Fixed ticks since start.")
        .fn(
            "now", [&engine] { return engine.gameTime(); }, "fun(): number", "Game seconds since start.")
        .fn(
            "wall",
            [] {
                return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
            },
            "fun(): number", "Wall-clock seconds since the Unix epoch (save timestamps).")
        .fn(
            "local_date",
            [](sol::this_state state, sol::object epoch) {
                double seconds = 0;
                if (lua::absent(epoch)) {
                    seconds = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                                      std::chrono::system_clock::now().time_since_epoch())
                                                      .count());
                } else if (epoch.get_type() == sol::type::number && std::isfinite(epoch.as<double>()) &&
                           std::abs(epoch.as<double>()) < 1e13) {
                    seconds = std::floor(epoch.as<double>());
                } else {
                    throw std::runtime_error("local_date takes seconds since the Unix epoch");
                }
                return localDate(state, static_cast<std::time_t>(seconds));
            },
            "fun(epoch?: number): {year: integer, month: integer, day: integer, hour: integer, min: integer, "
            "sec: integer, weekday: integer, utc_offset: integer}",
            "Calendar date and time in the player's time zone of `epoch` (seconds since the Unix epoch, as "
            "fe.time.wall returns; default now). weekday is 1 for Sunday to 7 for Saturday; utc_offset is "
            "the "
            "zone's offset from UTC in seconds at that moment.");
    TypeBinder<RandomStream>(host, engine.docs(), "RandomStream",
                             "Deterministic PCG32 random stream. Same seed, same sequence on every platform.")
        .method(
            "float", [](RandomStream& r) { return r.random.unit(); }, "fun(): number", "Uniform in [0, 1).")
        .method(
            "int",
            [](RandomStream& r, sol::object low, sol::object high) {
                const auto a = lua::integer(low, "low");
                const auto b = lua::integer(high, "high");
                if (a > b) {
                    throw std::runtime_error("int: low must not be above high");
                }
                return r.random.range(a, b);
            },
            "fun(low: integer, high: integer): integer", "Uniform integer in [low, high].")
        .method(
            "chance",
            [](RandomStream& r, sol::object probability) {
                return r.random.unit() < lua::as<double>(probability, "probability");
            },
            "fun(probability: number): boolean", "True with the given probability.")
        .method(
            "range",
            [](RandomStream& r, sol::object low, sol::object high) {
                const auto a = lua::as<double>(low, "low");
                const auto b = lua::as<double>(high, "high");
                if (a > b) {
                    throw std::runtime_error("range: low must not be above high");
                }
                const double u = r.random.unit();
                if (std::isfinite(b - a)) {
                    return a + (b - a) * u;
                }
                // Bounds too far apart for their difference to be a double.
                return std::min(a * (1 - u) + b * u, std::nextafter(b, a));
            },
            "fun(low: number, high: number): number", "Uniform real in [low, high).")
        .method(
            "pick",
            [](RandomStream& r, sol::table list) -> sol::object {
                const auto size = static_cast<std::int64_t>(lua::length(list));
                if (size == 0) {
                    return sol::lua_nil;
                }
                return list.raw_get<sol::object>(r.random.range(1, size));
            },
            "fun(list: any[]): any", "Random element of a sequence.")
        .method(
            "shuffle",
            [](RandomStream& r, sol::table list) {
                for (auto i = static_cast<std::int64_t>(lua::length(list)); i > 1; --i) {
                    const auto j = r.random.range(1, i);
                    const auto a = list.raw_get<sol::object>(i);
                    const auto b = list.raw_get<sol::object>(j);
                    list.raw_set(i, b, j, a);
                }
                return list;
            },
            "fun(list: any[]): any[]", "Shuffle a sequence in place.")
        .method(
            "state", [](RandomStream& r) { return std::to_string(r.random.state()); }, "fun(): string",
            "Opaque state string (for debugging).");
    ModuleBinder(host, engine.docs(), "fe.random",
                 "Seeded random streams. Replays and tests fix the base seed.")
        .fn(
            "new",
            [&engine](sol::object seed) {
                return RandomStream{Random(
                    lua::absent(seed) ? engine.nextSeed()
                                      : static_cast<std::uint64_t>(lua::integer(seed, "A random seed")))};
            },
            "fun(seed?: integer): RandomStream",
            "New stream; without a seed it derives one from the run seed.")
        .fn(
            "seed", [&engine] { return static_cast<double>(engine.seed() & 0xffffffffffffULL); },
            "fun(): integer", "The run's base seed.");
}

void bindData(Engine& engine) {
    auto& host = engine.lua();
    ModuleBinder(host, engine.docs(), "fe.json",
                 "JSON conversion. A non-empty sequence (keys 1..n, nothing else) becomes an array and any "
                 "other table an object. Object keys carry the Lua key's type: an integer key k is written "
                 "\"#k\" and a "
                 "string key that starts with '#' gets a second '#', so {[1] = 'a', ['1'] = 'b', ['#t'] = "
                 "'c'} encodes as {\"#1\": \"a\", \"1\": \"b\", \"##t\": \"c\"} and decodes back to "
                 "the same table; other keys and keys like \"#x\" or \"#01\" stay strings. Values nest at "
                 "most 128 levels; deeper ones raise an error, as do functions, userdata, non-integer number "
                 "keys and NaN or infinite numbers. fe.save uses the same conversion.")
        .fn(
            "decode", [&host](const std::string& text) { return jsonToLua(host.lua(), parseJson(text)); },
            "fun(text: string): any", "Parse JSON text (\"#k\" keys become integer keys, see above).")
        .fn(
            "encode",
            [](sol::object value, sol::object pretty) {
                return luaToJson(value).dump(lua::opt<bool>(pretty, "pretty").value_or(false) ? 2 : -1);
            },
            "fun(value: any, pretty?: boolean): string",
            "Serialize a value (tables, strings, numbers, booleans); integer keys of non-sequence tables are "
            "written \"#k\".")
        .fn(
            "read",
            [&host](const std::string& path) {
                const auto text = readFile(gameFile(host, path));
                try {
                    return jsonToLua(host.lua(), parseJson(text));
                } catch (const std::exception& error) {
                    throw std::runtime_error(path + ": " + error.what());
                }
            },
            "fun(path: string): any",
            "Read and parse a JSON file inside the game folder ('@fe/' for lualib).");
    ModuleBinder(host, engine.docs(), "fe.fs", "Read-only access to files inside the game folder.")
        .fn(
            "read", [&host](const std::string& path) { return readFile(gameFile(host, path)); },
            "fun(path: string): string", "Read a text file.")
        .fn(
            "exists",
            [&host](const std::string& path) {
                try {
                    const auto file = gameFile(host, path);
                    return isFile(file) || isFolder(file);
                } catch (const std::exception&) {
                    return false;
                }
            },
            "fun(path: string): boolean", "Whether a file or folder exists.")
        .fn(
            "list",
            [&host](sol::this_state state, const std::string& path) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                const auto folder = resolveInside(host.gameRoot(), path.empty() ? "." : path);
                for (const auto& name : listFolder(folder)) {
                    out.add(name);
                }
                return out;
            },
            "fun(folder: string): string[]", "Sorted entries of a folder; folders end with '/'.");
}

void bindApp(Engine& engine) {
    auto& host = engine.lua();
    const auto& config = engine.config();
    auto title = std::make_shared<std::string>(config.title);
    ModuleBinder(host, engine.docs(), "fe.app", "Application, window and asset-loading services.")
        .fn(
            "title", [title] { return *title; }, "fun(): string",
            "The window title: game.json's title until fe.app.set_title changes it.")
        .fn(
            "set_title",
            [&engine, title](const std::string& text) {
                *title = text;
                engine.requests().title = text;
            },
            "fun(title: string)", "Change the window title.")
        .fn(
            "virtual_size",
            [&engine] { return std::make_tuple(engine.canvas().width(), engine.canvas().height()); },
            "fun(): integer, integer", "Virtual canvas resolution.")
        .fn(
            "viewport", [&engine] { return std::make_tuple(engine.viewport().x, engine.viewport().y); },
            "fun(): integer, integer", "Window size in pixels.")
        .fn(
            "pixels_per_unit", [&config] { return config.pixelsPerUnit; }, "fun(): integer",
            "Sprite pixels per world unit.")
        .fn(
            "quit", [&engine] { engine.requests().quit = true; }, "fun()", "Close the game.")
        .fn(
            "fullscreen",
            [&engine](sol::this_state state) {
                lua::atMostArguments(state, 0,
                                     "fe.app.fullscreen() only reads; use fe.app.set_fullscreen(mode)");
                return displayTable(state, engine.display())["fullscreen"].get<sol::object>();
            },
            "fun(): false|'borderless'|'exclusive'", "The fullscreen mode (as in fe.app.display()).")
        .fn(
            "set_fullscreen",
            [&engine](sol::object mode) {
                DisplayRequest request;
                request.fullscreen = fullscreenMode(mode);
                engine.requestDisplay(request);
            },
            "fun(mode: boolean|'borderless'|'exclusive')",
            "Switch fullscreen; shorthand for fe.app.set_display{fullscreen = mode}.")
        .fn(
            "display", [&engine](sol::this_state state) { return displayTable(state, engine.display()); },
            "fun(): {fullscreen: false|'borderless'|'exclusive', width: integer, height: integer, vsync: "
            "'on'|'off'|'adaptive', monitor: integer, monitors: {name: string, width: integer, height: "
            "integer, "
            "refresh: number}[], modes: {width: integer, height: integer, refresh: number}[]}",
            "Window and monitor settings. width and height are the window size in screen units, or the "
            "exclusive fullscreen mode; monitor indexes monitors; modes are the exclusive fullscreen modes "
            "of "
            "that monitor, largest first. Headless runs report one monitor the size of game.json's window.")
        .fn(
            "set_display",
            [&engine](sol::table settings) { engine.requestDisplay(displayRequest(settings)); },
            "fun(settings: {fullscreen?: boolean|'borderless'|'exclusive', width?: integer, height?: "
            "integer, "
            "vsync?: boolean|'on'|'off'|'adaptive', monitor?: integer})",
            "Change display settings; omitted fields stay. fullscreen = true means 'borderless' (a desktop-"
            "sized window); 'exclusive' switches the monitor to the display mode closest to width x height. "
            "Applied after the frame; fe.app.display() shows the request until then. The engine does not "
            "store these: keep them in the player's settings (fe.config) and apply them at start.")
        .fn(
            "locale", [&engine] { return engine.locale(); }, "fun(): string",
            "The player's preferred language and region as a BCP 47 tag, such as 'en-US' or 'ja' (from the "
            "operating system; 'en-US' in headless runs).")
        .fn(
            "capture",
            [&engine](const std::string& path) {
                if (std::filesystem::path(path).extension() != ".png") {
                    throw std::runtime_error("Screenshots are .png files: " + path);
                }
                engine.requests().captures.push_back(resolveInside(engine.saves().screenshotFolder(), path));
            },
            "fun(path: string)", "Write a PNG screenshot to <user data>/screenshots/<path> (a .png path).")
        .fn(
            "preload",
            [&engine](sol::object paths) {
                return engine.assets().preload(lua::strings(lua::tableArg(paths, "paths")));
            },
            "fun(paths: string[]): integer", "Decode assets on a worker thread; returns a ticket.")
        .fn(
            "progress", [&engine](int ticket) { return engine.assets().progress(ticket); },
            "fun(ticket: integer): number", "Preload progress 0..1.")
        .fn(
            "image_size",
            [&engine](const std::string& path) {
                const bool target = path.starts_with(Canvas::TargetPrefix);
                if (!target) {
                    engine.assets().image(path);
                }
                const auto size = engine.canvas().imageSize(path);
                if (target && size.x == 0) {
                    throw std::runtime_error("Render target '" + path.substr(Canvas::TargetPrefix.size()) +
                                             "' does not exist yet (draw a world into it first)");
                }
                return std::make_tuple(size.x, size.y);
            },
            "fun(path: string): integer, integer",
            "Pixel size of an image, or of a render target ('rt:<name>'). A missing or unreadable image, or "
            "a "
            "target no world was drawn into, raises an error.")
        .fn(
            "sprite_clips",
            [&engine](sol::this_state state, const std::string& path) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                if (path.empty()) {
                    return out;
                }
                for (const auto& [name, clip] : engine.assets().sprite(path)->clips) {
                    out[name] = lua.create_table_with("loop", clip.loop, "length", clip.length(0),
                                                      "directions", clip.directions.size());
                }
                return out;
            },
            "fun(path: string): table<string, {loop: boolean, length: number, directions: integer}>",
            "Clip information of a sprite animation (.anim.json). An empty path (a sprite without an "
            "animation) has no clips; a missing or invalid file raises an error.");
}
} // namespace

void bindCore(Engine& engine) {
    auto& docs = engine.docs();
    docs.alias({"Vec3", "{x: number, y?: number, z: number}",
                "A world position or vector; `{1, 2, 3}` also works. Positions need x and z; offsets and "
                "directions may leave components out (0). Components are within -1000000 to 1000000."});
    docs.alias({"Cell", "{x: integer, z: integer, layer?: integer}",
                "A grid cell: 0-based integer coordinates, layer 0 by default. `{x, z, layer}` positionally "
                "also works; cell tables the engine returns carry more fields, which are ignored."});
    docs.alias({"Rect", "{x: number, y: number, w: number, h: number}",
                "A rectangle in canvas or image pixels; `{x, y, w, h}` positionally also works."});
    docs.alias({"Color", "integer|string|{r: number, g: number, b: number, a?: number}|number[]",
                "0xRRGGBBAA, a color name or '#rrggbb[aa]', or 0..1 components (above 1 is HDR where "
                "supported). A plain number is always 0xRRGGBBAA, so 0.5 is an error."});
    docs.alias(
        {"TextOptions",
         "{font?: string, size?: number, color?: Color, width?: number, align?: 'left'|'center'|'right', "
         "visible?: integer, shadow?: Color, outline?: Color, line_gap?: number}",
         "Text layout and style: `size` is 0 to 1024 (0 = the font's own), `width` (0 to 1000000) wraps "
         "(0 = no wrapping), `visible` reveals that many units (typewriter)."});
    bindDev(engine);
    bindTimeAndRandom(engine);
    bindData(engine);
    bindApp(engine);
}
} // namespace fe
