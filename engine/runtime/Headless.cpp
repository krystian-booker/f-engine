#include "engine/runtime/Headless.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
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

std::filesystem::path findLibraryRoot(const std::filesystem::path& executable) {
    const auto folder = std::filesystem::weakly_canonical(executable).parent_path();
    const auto isLibrary = [](const std::filesystem::path& candidate) {
        mountFolderPack(candidate);
        return isFile(candidate / "fe" / "init.lua");
    };
    for (const auto& candidate : {folder / "lualib", folder.parent_path() / "Resources" / "lualib",
                                  folder / "share" / "fengine" / "lualib"}) {
        if (isLibrary(candidate)) {
            return candidate;
        }
    }
#if FENGINE_DEV
    // Development builds also run from build trees next to the sources.
    auto parent = folder;
    for (int depth = 0; depth < 6 && parent != parent.parent_path(); ++depth) {
        parent = parent.parent_path();
        for (const auto& candidate : {parent / "lualib", parent / "share" / "fengine" / "lualib"}) {
            if (isLibrary(candidate)) {
                return candidate;
            }
        }
    }
#ifdef FENGINE_SOURCE_LUALIB
    return FENGINE_SOURCE_LUALIB;
#endif
#endif
    throw std::runtime_error("Cannot find the engine's lualib folder next to " + folder.string());
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

std::unique_ptr<Engine> headlessEngine(const std::filesystem::path& game, const std::filesystem::path& lualib,
                                       const ScratchFolder& userData) {
    EngineOptions options;
    options.gameRoot = game;
    options.libraryRoot = lualib;
    options.userData = userData.path();
    options.development = true;
    options.testHooks = true;
    options.seed = 1234;
    auto engine = std::make_unique<Engine>(std::move(options));
    engine->setViewport(engine->config().windowWidth, engine->config().windowHeight);
    return engine;
}

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
            "Restart the script watchdog's clock. fe.test calls it before each test, so every test (like "
            "every tick) gets the whole watchdog limit.")
        .fn(
            "reload",
            [&engine](const std::string& target) {
                auto& lua = engine.lua();
                if (!target.ends_with(".lua")) {
                    const auto file = lua.resolve(target);
                    return file.has_value() && lua.reloadFile(*file);
                }
                return lua.reloadFile(target.starts_with("@fe/")
                                          ? resolveInside(lua.libraryRoot() / "fe", target.substr(4))
                                          : resolveInside(lua.gameRoot(), target));
            },
            "fun(target: string): boolean",
            "Hot-reload a loaded module, named as for require or by its Lua file ('@fe/...' for lualib), as "
            "a "
            "file change does in development builds; false when no loaded module comes from it.");
}
} // namespace

int runTests(const std::filesystem::path& game, const std::filesystem::path& lualib,
             const std::string& filter, double watchdog) {
    const ScratchFolder userData(game);
    auto engine = headlessEngine(game, lualib, userData);
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
        sol::table test = lua.require("fe.test");
        auto result = lua.invoke(test["run"], sol::as_table(modules), filter);
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

int checkGame(const std::filesystem::path& game, const std::filesystem::path& lualib) {
    int problems = 0;
    const ScratchFolder userData(game);
    auto engine = headlessEngine(game, lualib, userData);
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

int writeApi(const std::filesystem::path& lualib, const std::filesystem::path& folder) {
    const auto game = lualib / "tests" / "game";
    const ScratchFolder userData(game);
    auto engine = headlessEngine(game, lualib, userData);
    bindHarness(*engine);
    engine->writeApi(folder / "fengine.d.lua", folder / "api.md");
    std::cout << "Wrote fengine.d.lua and api.md to " << folder.string() << "\n";
    return 0;
}

int runHeadlessCommand(int argc, char** argv) {
    std::string command, target, filter, output;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--test" || arg == "--check") {
            command = arg;
            target = next();
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
    const auto lualib = findLibraryRoot(executablePath(argv[0]));
    if (command == "--test") {
        return runTests(target, lualib, filter);
    }
    if (command == "--check") {
        return checkGame(target, lualib);
    }
    return writeApi(lualib, output);
}
} // namespace fe
