#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/core/Random.h"
#include "engine/runtime/Engine.h"
#include "engine/script/bindings/Convert.h"
#include <chrono>
#include <fstream>
#include <sstream>

namespace fe {
namespace {
struct RandomStream {
    Random random;
};

std::string readGameFile(const LuaHost& host, const std::string& path) {
    const auto file = path.starts_with("@fe/") ? resolveInside(host.libraryRoot() / "fe", path.substr(4))
                                               : resolveInside(host.gameRoot(), path);
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open " + path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
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
            [](sol::this_state state, sol::object thread, sol::optional<std::string> message) {
                lua_State* L = state;
                lua_State* target = L;
                if (thread.get_type() == sol::type::thread) {
                    target = thread.as<sol::thread>().thread_state();
                }
                luaL_traceback(L, target, message ? message->c_str() : nullptr, 0);
                std::string result = lua_tostring(L, -1);
                lua_pop(L, 1);
                return result;
            },
            "fun(thread?: thread, message?: string): string", "Traceback of a coroutine (or the caller).")
        .fn(
            "enabled", [&engine] { return engine.development(); }, "fun(): boolean",
            "True in development builds (editor, hot reload, watchdog).")
        .fn(
            "zone",
            [&engine](const std::string& name, double milliseconds) {
                engine.stats().zones[name] = milliseconds;
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
        .fn(
            "clear_errors", [&host] { host.clearErrors(); }, "fun()", "Clear the recorded script errors.")
        .fn(
            "console", [&host](const std::string& code) { return host.console(code); },
            "fun(code: string): string", "Evaluate Lua and return printable results.");
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
            "fun(): number", "Wall-clock seconds since the Unix epoch (save timestamps).");
    TypeBinder<RandomStream>(host, engine.docs(), "RandomStream",
                             "Deterministic PCG32 random stream. Same seed, same sequence on every platform.")
        .method(
            "float", [](RandomStream& r) { return r.random.unit(); }, "fun(): number", "Uniform in [0, 1).")
        .method(
            "int", [](RandomStream& r, std::int64_t a, std::int64_t b) { return r.random.range(a, b); },
            "fun(low: integer, high: integer): integer", "Uniform integer in [low, high].")
        .method(
            "chance", [](RandomStream& r, double p) { return r.random.unit() < p; },
            "fun(probability: number): boolean", "True with the given probability.")
        .method(
            "range", [](RandomStream& r, double a, double b) { return a + (b - a) * r.random.unit(); },
            "fun(low: number, high: number): number", "Uniform real in [low, high).")
        .method(
            "pick",
            [](RandomStream& r, sol::table list) -> sol::object {
                if (list.size() == 0) {
                    return sol::lua_nil;
                }
                return list[r.random.range(1, static_cast<std::int64_t>(list.size()))];
            },
            "fun(list: any[]): any", "Random element of a sequence.")
        .method(
            "shuffle",
            [](RandomStream& r, sol::table list) {
                for (auto i = static_cast<std::int64_t>(list.size()); i > 1; --i) {
                    const auto j = r.random.range(1, i);
                    sol::object a = list[i];
                    sol::object b = list[j];
                    list[i] = b;
                    list[j] = a;
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
            [&engine](sol::optional<double> seed) {
                return RandomStream{Random(seed ? static_cast<std::uint64_t>(*seed) : engine.nextSeed())};
            },
            "fun(seed?: integer): RandomStream",
            "New stream; without a seed it derives one from the run seed.")
        .fn(
            "seed", [&engine] { return static_cast<double>(engine.seed() & 0xffffffffffffULL); },
            "fun(): integer", "The run's base seed.");
}

void bindData(Engine& engine) {
    auto& host = engine.lua();
    ModuleBinder(host, engine.docs(), "fe.json", "JSON conversion.")
        .fn(
            "decode", [&host](const std::string& text) { return jsonToLua(host.lua(), Json::parse(text)); },
            "fun(text: string): any", "Parse JSON text.")
        .fn(
            "encode",
            [](sol::object value, sol::optional<bool> pretty) {
                return luaToJson(value).dump(pretty.value_or(false) ? 2 : -1);
            },
            "fun(value: any, pretty?: boolean): string",
            "Serialize a value (tables, strings, numbers, booleans).")
        .fn(
            "read",
            [&host](const std::string& path) {
                return jsonToLua(host.lua(), Json::parse(readGameFile(host, path)));
            },
            "fun(path: string): any",
            "Read and parse a JSON file inside the game folder ('@fe/' for lualib).");
    ModuleBinder(host, engine.docs(), "fe.fs", "Read-only access to files inside the game folder.")
        .fn(
            "read", [&host](const std::string& path) { return readGameFile(host, path); },
            "fun(path: string): string", "Read a text file.")
        .fn(
            "exists",
            [&host](const std::string& path) {
                try {
                    const auto file = path.starts_with("@fe/")
                                          ? resolveInside(host.libraryRoot() / "fe", path.substr(4))
                                          : resolveInside(host.gameRoot(), path);
                    return std::filesystem::exists(file);
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
                std::vector<std::string> names;
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
                    names.push_back(entry.path().filename().string() + (entry.is_directory() ? "/" : ""));
                }
                std::sort(names.begin(), names.end());
                for (const auto& name : names) {
                    out.add(name);
                }
                return out;
            },
            "fun(folder: string): string[]", "Sorted entries of a folder; folders end with '/'.");
}

void bindApp(Engine& engine) {
    auto& host = engine.lua();
    const auto& config = engine.config();
    ModuleBinder(host, engine.docs(), "fe.app", "Application, window and asset-loading services.")
        .fn(
            "title", [&config] { return config.title; }, "fun(): string", "Game title from game.json.")
        .fn(
            "set_title", [&engine](const std::string& title) { engine.requests().title = title; },
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
            "fullscreen", [&engine](bool on) { engine.requests().fullscreen = on; }, "fun(enabled: boolean)",
            "Switch fullscreen.")
        .fn(
            "capture",
            [&engine, &host](const std::string& path) {
                engine.requests().captures.push_back(resolveInside(engine.saves().directory(), path));
                (void)host;
            },
            "fun(path: string)", "Write a PNG screenshot into the user data folder.")
        .fn(
            "preload", [&engine](sol::table paths) { return engine.assets().preload(lua::strings(paths)); },
            "fun(paths: string[]): integer", "Decode assets on a worker thread; returns a ticket.")
        .fn(
            "progress", [&engine](int ticket) { return engine.assets().progress(ticket); },
            "fun(ticket: integer): number", "Preload progress 0..1.")
        .fn(
            "image_size",
            [&engine](const std::string& path) {
                const auto size = engine.canvas().imageSize(path);
                return std::make_tuple(size.x, size.y);
            },
            "fun(path: string): integer, integer", "Pixel size of an image (0, 0 when missing).")
        .fn(
            "sprite_clips",
            [&engine](sol::this_state state, const std::string& path) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                if (const auto sprite = engine.assets().trySprite(path)) {
                    for (const auto& [name, clip] : sprite->clips) {
                        double length = 0;
                        for (const auto& frame : clip.directions.front()) {
                            length += frame.duration;
                        }
                        out[name] = lua.create_table_with("loop", clip.loop, "length", length, "directions",
                                                          clip.directions.size());
                    }
                }
                return out;
            },
            "fun(path: string): table<string, {loop: boolean, length: number, directions: integer}>",
            "Clip information of a sprite animation.");
}
} // namespace

void bindCore(Engine& engine) {
    bindDev(engine);
    bindTimeAndRandom(engine);
    bindData(engine);
    bindApp(engine);
}
} // namespace fe
