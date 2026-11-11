#include "engine/runtime/Headless.h"
#include "engine/core/AssetCache.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/editor/AssetCatalog.h"
#include "engine/editor/WalkBake.h"
#include "engine/runtime/Engine.h"
#include "engine/script/Binder.h"
#include "engine/world/World.h"
#include <cstring>
#include <iostream>
#include <random>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fe {
std::filesystem::path executablePath(const std::filesystem::path& fallback) {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (size > 0 && _NSGetExecutablePath(buffer.data(), &size) == 0) {
        buffer.resize(std::strlen(buffer.c_str()));
        std::error_code ec;
        const auto canonical = std::filesystem::canonical(buffer, ec);
        return ec ? std::filesystem::path(buffer) : canonical;
    }
#else
    std::error_code ec;
    const auto link = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !link.empty()) {
        return link;
    }
#endif
    return std::filesystem::absolute(fallback);
}

std::filesystem::path findBuiltinRoot(const std::filesystem::path& executable) {
    const auto folder = std::filesystem::weakly_canonical(executable).parent_path();
    // The folder (or builtin.pak standing in for it) holds the default font,
    // the weather presets and the input glyph sheets: no Lua.
    const auto isLibrary = [](const std::filesystem::path& candidate) {
        mountFolderPack(candidate);
        return isFile(candidate / "assets" / "weather.json");
    };
    for (const auto& candidate : {folder / "builtin", folder.parent_path() / "Resources" / "builtin",
                                  folder / "share" / "fengine" / "builtin"}) {
        if (isLibrary(candidate)) {
            return candidate;
        }
    }
#if FENGINE_DEV
    // Development builds also run from build trees next to the sources.
    auto parent = folder;
    for (int depth = 0; depth < 6 && parent != parent.parent_path(); ++depth) {
        parent = parent.parent_path();
        for (const auto& candidate : {parent / "builtin", parent / "share" / "fengine" / "builtin"}) {
            if (isLibrary(candidate)) {
                return candidate;
            }
        }
    }
#ifdef FENGINE_SOURCE_BUILTIN
    return FENGINE_SOURCE_BUILTIN;
#endif
#endif
    throw std::runtime_error("Cannot find the engine's builtin folder next to " + folder.string());
}

namespace {
// A fresh user-data folder for one headless run, removed afterwards. Runs
// never share one, so parallel test processes cannot wipe each other's saves.
class ScratchFolder {
public:
    explicit ScratchFolder(const std::filesystem::path& game) {
        auto name = std::filesystem::weakly_canonical(game).filename().string();
        if (name.empty()) {
            name = "game";
        }
        const auto base = std::filesystem::temp_directory_path() / "fengine-headless";
        std::filesystem::create_directories(base);
        std::random_device entropy;
        for (int attempt = 0; attempt < 16 && path_.empty(); ++attempt) {
            const auto candidate = base / (name + "-" + std::to_string(entropy()));
            if (std::filesystem::create_directory(candidate)) {
                path_ = candidate;
            }
        }
        if (path_.empty()) {
            throw std::runtime_error("Cannot create a user data folder in " + base.string());
        }
    }
    ~ScratchFolder() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::unique_ptr<Engine> headlessEngine(const std::filesystem::path& game,
                                       const std::filesystem::path& builtin, const ScratchFolder& userData) {
    EngineOptions options;
    options.gameRoot = game;
    options.builtinRoot = builtin;
    options.userData = userData.path();
    options.development = true;
    options.testHooks = true;
    options.seed = 1234;
    auto engine = std::make_unique<Engine>(std::move(options));
    engine->setViewport(engine->config().windowWidth, engine->config().windowHeight);
    return engine;
}

// The test runner behind `--test`. The engine ships no Lua modules, so this
// is the one piece of Lua it carries, and no game can require it: it is
// compiled from this string for a `--test` run and nowhere else.
//
// Every tests/*.lua module returns a table of named test functions; each is
// called with the helper table `t`. Keys starting with "_" are not tests:
// `_before_each` and `_after_each` run around every test of their module
// (a game resets its own state there).
const char* const TestRunner = R"lua(
local modules, filter = ...

local function describe(value)
  if type(value) == "string" then return string.format("%q", value) end
  return tostring(value)
end

local function deep_equal(a, b)
  if a == b then return true end
  if type(a) ~= "table" or type(b) ~= "table" then return false end
  for k, v in pairs(a) do
    if not deep_equal(v, b[k]) then return false end
  end
  for k in pairs(b) do
    if a[k] == nil then return false end
  end
  return true
end

local t = {}
function t.ok(value, message)
  if not value then error(message or "expected a true value", 2) end
end
function t.eq(actual, expected, message)
  if not deep_equal(actual, expected) then
    error((message and message .. ": " or "") .. "expected " .. describe(expected) .. ", got " .. describe(actual), 2)
  end
end
function t.near(actual, expected, epsilon, message)
  epsilon = epsilon or 1e-6
  if type(actual) ~= "number" or math.abs(actual - expected) > epsilon then
    error((message and message .. ": " or "") .. "expected ~" .. describe(expected) .. ", got " .. describe(actual), 2)
  end
end
function t.error(fn, pattern)
  local ok, err = pcall(fn)
  if ok then error("expected an error", 2) end
  if pattern and not tostring(err):find(pattern) then
    error("error '" .. tostring(err) .. "' does not match '" .. pattern .. "'", 2)
  end
end
-- Advance the game by fixed ticks (update, world ticks, post_update, draw).
function t.tick(count)
  fe.harness.tick(count or 1)
end
-- Tick until predicate is true; fails once `ticks` ticks have passed and at
-- least `wall_seconds` (default 10) of real time, so work on engine worker
-- threads (background world loads) gets time to finish however fast the
-- build ticks.
function t.wait_until(predicate, ticks, message, wall_seconds)
  local deadline = fe.dev.clock() + (wall_seconds or 10)
  local count = 0
  while not predicate() do
    if count >= (ticks or 600) and fe.dev.clock() >= deadline then
      error(message or "condition not reached in time", 2)
    end
    fe.harness.tick(1)
    count = count + 1
  end
end

local passed, failed = 0, 0
local function guarded(fn, ...)
  return xpcall(fn, function(m) return fe.dev.traceback(nil, tostring(m)) end, ...)
end
for _, module_name in ipairs(modules) do
  local ok, cases = pcall(require, module_name)
  if not ok or type(cases) ~= "table" then
    print("FAIL " .. module_name .. " (load): " .. tostring(ok and "the module must return a table of tests" or cases))
    failed = failed + 1
  else
    local names = {}
    for name, fn in pairs(cases) do
      if type(name) == "string" and name:sub(1, 1) ~= "_" and type(fn) == "function" then names[#names + 1] = name end
    end
    table.sort(names)
    for _, name in ipairs(names) do
      if not filter or filter == "" or name:find(filter, 1, true) or module_name:find(filter, 1, true) then
        local errors_before = fe.harness.errors()
        fe.harness.begin_test()
        local result = true
        if type(cases._before_each) == "function" then
          local fine, err = guarded(cases._before_each, t)
          if not fine then result = "_before_each: " .. tostring(err) end
        end
        if result == true then
          local fine, err = guarded(cases[name], t)
          if not fine then result = err end
        end
        if type(cases._after_each) == "function" then
          local fine, err = guarded(cases._after_each, t)
          if not fine and result == true then result = "_after_each: " .. tostring(err) end
        end
        if result == true and fe.harness.errors() > errors_before then
          result = "script errors were reported during the test (see log)"
        end
        if result == true then
          passed = passed + 1
        else
          failed = failed + 1
          print("FAIL " .. module_name .. " :: " .. name .. "\n  " .. tostring(result))
        end
      end
    end
  end
end
print(string.format("%d passed, %d failed", passed, failed))
return failed
)lua";

void bindHarness(Engine& engine) {
    ModuleBinder(engine.lua(), engine.docs(), "fe.harness",
                 "Test-only controls. They exist only while `fengine_headless --test` runs a game's tests; "
                 "games never see them.")
        .fn(
            "start", [&engine] { return engine.start(); }, "fun(): boolean",
            "Load and initialise the entry module.")
        .fn(
            "tick",
            [&engine](sol::optional<int> ticks) {
                // Each tick restarts the watchdog's clock, so the count is bounded instead.
                const int count = ticks.value_or(1);
                if (count < 0 || count > 36000) {
                    throw std::runtime_error("fe.harness.tick takes 0 to 36000 ticks, got " +
                                             std::to_string(count));
                }
                for (int i = 0; i < count; ++i) {
                    engine.tick(1.0 / 60.0);
                    engine.draw(1.0);
                }
            },
            "fun(ticks?: integer)",
            "Advance the engine by fixed ticks (default 1, at most 36000), drawing each one.")
        .fn(
            "errors", [&engine] { return engine.lua().errors().size(); }, "fun(): integer",
            "Recorded script errors.")
        .fn(
            "begin_test", [&engine] { engine.lua().beginFrame(); }, "fun()",
            "Restart the script watchdog's clock. The test runner calls it before each test, so every test "
            "(like every tick) gets the whole watchdog limit.")
        .fn(
            "reload",
            [&engine](const std::string& target) {
                auto& lua = engine.lua();
                if (!target.ends_with(".lua")) {
                    const auto file = lua.resolve(target);
                    return file.has_value() && lua.reloadFile(*file);
                }
                return lua.reloadFile(resolveInside(lua.gameRoot(), target));
            },
            "fun(target: string): boolean",
            "Hot-reload a loaded module, named as for require or by its Lua file, as a file change does in "
            "development builds; false when no loaded module comes from it.");
}
} // namespace

int runTests(const std::filesystem::path& game, const std::filesystem::path& builtin,
             const std::string& filter, double watchdog) {
    const ScratchFolder userData(game);
    auto engine = headlessEngine(game, builtin, userData);
    engine->lua().setWatchdog(watchdog);
    bindHarness(*engine);
    std::vector<std::string> modules;
    for (const auto& name : listFolder(game / "tests")) {
        if (name.size() > 4 && name.ends_with(".lua")) {
            modules.push_back("tests." + name.substr(0, name.size() - 4));
        }
    }
    if (modules.empty()) {
        std::cerr << "No tests found in " << (game / "tests").string() << "\n";
        return 1;
    }
    auto& lua = engine->lua();
    try {
        lua.beginFrame();
        const auto loaded = lua.lua().load(TestRunner, "=fengine test runner", sol::load_mode::text);
        if (!loaded.valid()) {
            const sol::error error = loaded;
            std::cerr << error.what() << "\n";
            return 1;
        }
        auto result =
            lua.invoke(sol::object(loaded.get<sol::protected_function>()), sol::as_table(modules), filter);
        if (!result.valid()) {
            const sol::error error = result;
            std::cerr << error.what() << "\n";
            return 1;
        }
        const int failed = result.get<int>();
        return failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}

int checkGame(const std::filesystem::path& game, const std::filesystem::path& builtin) {
    int problems = 0;
    const ScratchFolder userData(game);
    auto engine = headlessEngine(game, builtin, userData);
    std::vector<std::string> maps;
    for (const auto& name : listFolder(game / "maps")) {
        if (name.ends_with('/') && isFile(game / "maps" / name / "map.json")) {
            maps.push_back(name.substr(0, name.size() - 1));
        }
    }
    for (const auto& id : maps) {
        try {
            const auto world = engine->loadWorld("check_" + id, id);
            engine->assets().model(world->geometryPath());
            for (const auto& [name, point] : world->map().points) {
                if (!world->grid().canStand({point.position.x, point.position.z}, 0.2f, point.layer)) {
                    throw std::runtime_error("Point '" + name + "' is not on walkable ground");
                }
            }
            // A baked map whose inputs changed since: a warning, not a problem
            // (a packed game has no loose files to compare, so it is skipped).
            const auto& bake = world->map().bake;
            if (!bake.fingerprint.empty() && std::filesystem::is_directory(game) &&
                bakeFingerprint(world->map(), world->geometryPath(), gameFileReader(game)) !=
                    bake.fingerprint) {
                std::cout << "map " << id
                          << ": warning: the walkability bake is out of date (run --bake or press "
                             "Bake now in the editor)\n";
            }
            engine->unloadWorld("check_" + id);
            std::cout << "map " << id << ": ok\n";
        } catch (const std::exception& error) {
            std::cerr << "map " << id << ": " << error.what() << "\n";
            ++problems;
        }
    }
    for (const auto& relative : listFilesBelow(game)) {
        const auto name = std::filesystem::path(relative).filename().string();
        if (name.size() > 10 && name.ends_with(".anim.json")) {
            try {
                const auto sprite = engine->assets().sprite(relative);
                engine->assets().image(
                    (std::filesystem::path(relative).parent_path() / sprite->atlas).generic_string());
            } catch (const std::exception& error) {
                std::cerr << "sprite " << relative << ": " << error.what() << "\n";
                ++problems;
            }
        }
    }
    {
        // Every file, module, map and audio id that maps, data tables and
        // sprites name must exist.
        AssetCatalog catalog(game, builtin);
        catalog.scan();
        for (const auto& [target, user] : catalog.missing()) {
            std::cerr << "missing " << target << " (named by " << user.file << " at " << user.where << ")\n";
            ++problems;
        }
    }
    if (!engine->start()) {
        ++problems;
    } else {
        sol::table main = engine->lua().require(engine->config().entry);
        if (main["validate"].valid()) {
            auto result = engine->lua().invoke(main["validate"]);
            if (!result.valid()) {
                const sol::error error = result;
                std::cerr << "validate: " << error.what() << "\n";
                ++problems;
            }
        }
        engine->tick(1.0 / 60.0);
        engine->draw(1.0);
    }
    problems += static_cast<int>(engine->lua().errors().size());
    std::cout << (problems ? "FAILED: " : "OK: ") << problems << " problem(s)\n";
    return problems ? 1 : 0;
}

int bakeGame(const std::filesystem::path& game, const std::filesystem::path& builtin, bool checkOnly) {
    if (!std::filesystem::is_directory(game)) {
        std::cerr << "--bake needs a game folder, not an archive\n";
        return 1;
    }
    AssetCache assets(game, builtin);
    const auto readFile = gameFileReader(game);
    int stale = 0, problems = 0;
    for (const auto& name : listFolder(game / "maps")) {
        if (!name.ends_with('/') || !isFile(game / "maps" / name / "map.json")) {
            continue;
        }
        const auto id = name.substr(0, name.size() - 1);
        const auto file = game / "maps" / id / "map.json";
        try {
            auto map = MapData::load(file);
            const auto geometry = geometryPathOf(map);
            if (!checkOnly && map.bake.options.empty()) {
                map.bake.options = BakeOptions{}.toJson(); // part of the fingerprint: settle it first
            }
            const auto fingerprint = bakeFingerprint(map, geometry, readFile);
            if (checkOnly) {
                if (map.bake.empty()) {
                    std::cout << "map " << id << ": not baked (hand-painted)\n";
                } else if (map.bake.fingerprint == fingerprint) {
                    std::cout << "map " << id << ": bake up to date\n";
                } else {
                    std::cout << "map " << id << ": bake OUT OF DATE\n";
                    ++stale;
                }
                continue;
            }
            const auto scene = sceneForMap(map, assets, geometry);
            for (const auto& problem : scene.problems) {
                std::cerr << "map " << id << ": " << problem << "\n";
                ++problems;
            }
            const auto options = BakeOptions::fromJson(map.bake.options);
            const auto result = bakeWalkability(scene.pieces, map.grid, options);
            const int changed = applyBake(map, result, options);
            map.bake.fingerprint = fingerprint;
            writeJson(file, MapData::fromJson(map.toJson()).toJson());
            std::cout << "map " << id << ": baked, " << changed << " cell(s) changed, "
                      << result.walkable + result.partial << " walkable, " << result.blocked << " blocked, "
                      << map.bake.colliders.size() << " box(es)\n";
        } catch (const std::exception& error) {
            std::cerr << "map " << id << ": " << error.what() << "\n";
            ++problems;
        }
    }
    if (checkOnly) {
        std::cout << (stale ? "FAILED: " : "OK: ") << stale << " map(s) with a stale bake\n";
    }
    return stale || problems ? 1 : 0;
}

int writeApi(const std::filesystem::path& builtin, const std::filesystem::path& folder) {
    // The bindings need an engine, and an engine a game folder: an empty one.
    const ScratchFolder userData(folder);
    const auto game = userData.path() / "game";
    std::filesystem::create_directories(game);
    writeTextAtomic(game / "game.json", "{\"title\": \"fengine api\"}\n");
    writeTextAtomic(game / "main.lua", "return {}\n");
    auto engine = headlessEngine(game, builtin, userData);
    bindHarness(*engine);
    engine->writeApi(folder / "fengine.d.lua", folder / "api.md");
    std::cout << "Wrote fengine.d.lua and api.md to " << folder.string() << "\n";
    return 0;
}

int runHeadlessCommand(int argc, char** argv) {
    std::string command, target, filter, output;
    bool checkOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--test" || arg == "--check" || arg == "--bake") {
            command = arg;
            target = next();
        } else if (arg == "--check-only") {
            checkOnly = true;
        } else if (arg == "--write-api") {
            command = arg;
            output = next();
        } else if (arg == "--filter") {
            filter = next();
        }
    }
    if (command.empty()) {
        return -1;
    }
    if (!target.empty()) {
        target = openGame(target).string();
    }
    const auto builtin = findBuiltinRoot(executablePath(argv[0]));
    if (command == "--test") {
        return runTests(target, builtin, filter);
    }
    if (command == "--check") {
        return checkGame(target, builtin);
    }
    if (command == "--bake") {
        return bakeGame(target, builtin, checkOnly);
    }
    return writeApi(builtin, output);
}
} // namespace fe
