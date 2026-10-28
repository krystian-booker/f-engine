#include "engine/runtime/Headless.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/runtime/Engine.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace fe {
std::filesystem::path findLibraryRoot(const std::filesystem::path& executable) {
    auto folder = std::filesystem::weakly_canonical(executable).parent_path();
    for (int depth = 0; depth < 6 && !folder.empty(); ++depth) {
        for (const auto& candidate : {folder / "lualib", folder / "share" / "fengine" / "lualib"}) {
            if (std::filesystem::is_regular_file(candidate / "fe" / "init.lua")) {
                return candidate;
            }
        }
        if (folder == folder.parent_path()) {
            break;
        }
        folder = folder.parent_path();
    }
#ifdef FENGINE_SOURCE_LUALIB
    return FENGINE_SOURCE_LUALIB;
#else
    throw std::runtime_error("Cannot find the engine's lualib folder");
#endif
}

namespace {
std::unique_ptr<Engine> headlessEngine(const std::filesystem::path& game,
                                       const std::filesystem::path& lualib) {
    EngineOptions options;
    options.gameRoot = game;
    options.libraryRoot = lualib;
    options.userData = std::filesystem::temp_directory_path() / "fengine-headless" / game.filename();
    std::filesystem::remove_all(options.userData);
    options.development = true;
    options.seed = 1234;
    auto engine = std::make_unique<Engine>(std::move(options));
    engine->setViewport(engine->config().windowWidth, engine->config().windowHeight);
    return engine;
}

void bindHarness(Engine& engine) {
    ModuleBinder(engine.lua(), engine.docs(), "fe.harness", "Test-only controls (available under --test).")
        .fn(
            "start", [&engine] { return engine.start(); }, "fun(): boolean",
            "Load and initialise the entry module.")
        .fn(
            "tick",
            [&engine](sol::optional<int> ticks) {
                for (int i = 0; i < ticks.value_or(1); ++i) {
                    engine.tick(1.0 / 60.0);
                    engine.draw(1.0);
                }
            },
            "fun(ticks?: integer)", "Advance the engine by fixed ticks, drawing each one.")
        .fn(
            "errors", [&engine] { return engine.lua().errors().size(); }, "fun(): integer",
            "Recorded script errors.");
}
} // namespace

int runTests(const std::filesystem::path& game, const std::filesystem::path& lualib,
             const std::string& filter) {
    auto engine = headlessEngine(game, lualib);
    bindHarness(*engine);
    std::vector<std::string> modules;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(game / "tests", ec)) {
        if (entry.path().extension() == ".lua") {
            modules.push_back("tests." + entry.path().stem().string());
        }
    }
    std::sort(modules.begin(), modules.end());
    if (modules.empty()) {
        std::cerr << "No tests found in " << (game / "tests").string() << "\n";
        return 1;
    }
    auto& lua = engine->lua();
    try {
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
    auto engine = headlessEngine(game, lualib);
    std::error_code ec;
    std::vector<std::string> maps;
    for (const auto& entry : std::filesystem::directory_iterator(game / "maps", ec)) {
        if (std::filesystem::is_regular_file(entry.path() / "map.json")) {
            maps.push_back(entry.path().filename().string());
        }
    }
    std::sort(maps.begin(), maps.end());
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
    for (const auto& entry : std::filesystem::recursive_directory_iterator(game, ec)) {
        const auto name = entry.path().filename().string();
        if (name.size() > 10 && name.ends_with(".anim.json")) {
            const auto relative = relativeTo(game, entry.path());
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
        sol::table main = engine->mainModule();
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
    auto engine = headlessEngine(lualib / "tests" / "game", lualib);
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
    const auto lualib = findLibraryRoot(argv[0]);
    if (command == "--test") {
        return runTests(target, lualib, filter);
    }
    if (command == "--check") {
        return checkGame(target, lualib);
    }
    return writeApi(lualib, output);
}
} // namespace fe
