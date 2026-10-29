#include "engine/core/GameConfig.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/runtime/Headless.h"
#include "engine/runtime/MainLoop.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#include <cstdio>
#include <windows.h>
#endif

namespace {
void usage() {
    std::cout << "usage: fengine [game-folder | game.pak] [options]\n"
                 "  (without a folder, plays the game packaged next to the executable)\n"
                 "  --mute               no audio device\n"
                 "  --tools              open the F1 editor at start (development builds)\n"
                 "  --user-data <dir>    saves and settings folder (default: per-user app data)\n"
                 "  --replay <file>      play a scripted session; exit code reports failures\n"
                 "  --record <file>      write this session's input as a replay\n"
                 "  --realtime           pace a replay in real time (listening checks)\n"
                 "  --write-references   make a replay's captures its reference images for this backend\n"
                 "  --seed <n>           fixed random seed\n"
                 "  --frames <n>         stop after n frames\n"
                 "  --capture <png>      screenshot at frame 30\n"
                 "  --shaders <dir>      compiled shader folder\n"
                 "Headless: fengine --test <game> | --check <game> | --write-api <folder>\n";
}

#if defined(_WIN32)
// fengine.exe is a GUI program; show output in the console it was started from.
void attachConsole() {
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN &&
        AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
}
#endif

// The folder holding the executable (an app bundle's Resources on macOS).
std::filesystem::path basePath() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        throw std::runtime_error(std::string("Cannot locate the executable: ") + SDL_GetError());
    }
    return std::filesystem::path(base);
}

std::filesystem::path defaultShaders(const std::filesystem::path& base) {
    if (std::filesystem::is_directory(base / "shaders")) {
        return base / "shaders";
    }
#ifdef FENGINE_SHADER_DIR
    return FENGINE_SHADER_DIR;
#else
    throw std::runtime_error("No compiled shaders in " + (base / "shaders").string());
#endif
}

// A packaged game: <base>/game (app bundles) or <base>/../game (bin/ layouts),
// as a folder or a game.pak archive.
std::filesystem::path packagedGame(const std::filesystem::path& base) {
    for (const auto& candidate : {base / "game", base.parent_path().parent_path() / "game"}) {
        if (fe::isFile(fe::openGame(candidate) / "game.json")) {
            return candidate;
        }
    }
    return {};
}
} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    attachConsole();
#endif
    try {
        // Library lookups start from SDL's base path, not argv[0] (which may
        // be relative, a symlink or found through PATH).
        const auto base = basePath();
        auto executable = (base / "fengine").string();
        std::vector<char*> arguments(argv, argv + argc);
        arguments[0] = executable.data();
        const int headless = fe::runHeadlessCommand(argc, arguments.data());
        if (headless >= 0) {
            return headless;
        }
        fe::RuntimeOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error(std::string(arg) + " needs a value");
                }
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") {
                usage();
                return 0;
            } else if (arg == "--mute") {
                options.mute = true;
            } else if (arg == "--tools") {
                options.tools = true;
            } else if (arg == "--realtime") {
                options.realtime = true;
            } else if (arg == "--write-references") {
                options.writeReferences = true;
            } else if (arg == "--user-data") {
                options.userData = value();
            } else if (arg == "--replay") {
                options.replay = value();
            } else if (arg == "--record") {
                options.record = value();
            } else if (arg == "--seed") {
                options.seed = std::stoull(value());
            } else if (arg == "--frames") {
                options.frameLimit = std::stoull(value());
            } else if (arg == "--capture") {
                options.capture = value();
            } else if (arg == "--shaders") {
                options.shaders = value();
            } else if (!arg.starts_with("--") && options.game.empty()) {
                options.game = fe::openGame(std::string(arg));
            } else {
                throw std::runtime_error("Unknown option " + std::string(arg));
            }
        }
        if (options.game.empty()) {
            options.game = packagedGame(base);
        }
        if (options.game.empty()) {
            usage();
            return 2;
        }
        if (!options.replay.empty() && !options.record.empty()) {
            throw std::runtime_error("--replay and --record cannot be combined");
        }
        if (options.shaders.empty()) {
            options.shaders = defaultShaders(base);
        }
        options.lualib = fe::findLibraryRoot(base / "fengine");
        auto config = fe::GameConfig::load(options.game);
        options.pauseOnFocusLoss = config.pauseOnFocusLoss;
        if (options.userData.empty()) {
            char* path = SDL_GetPrefPath("fengine", config.title.c_str());
            if (!path) {
                throw std::runtime_error(std::string("No user data folder: ") + SDL_GetError());
            }
            options.userData = path;
            SDL_free(path);
        }
        return fe::MainLoop(std::move(options), std::move(config)).run();
    } catch (const std::exception& error) {
        fe::log(fe::LogLevel::Error, error.what());
        return 1;
    }
}
