#include "engine/core/GameConfig.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/runtime/Headless.h"
#include "engine/runtime/MainLoop.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
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
                 "  --replay-skip <n>    a replay ticks n times without rendering between frames (0-600);\n"
                 "                       the frames it captures are always rendered\n"
                 "  --write-references   make a replay's captures its reference images for this backend\n"
                 "  --max-fps <n>        frame cap while vsync is off (0 = uncapped; default: game.json)\n"
                 "  --hdr                present to an HDR back buffer when the renderer has one\n"
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
            } else if (arg == "--replay-skip") {
                options.replaySkip = std::clamp(std::stoi(value()), 0, 600);
            } else if (arg == "--max-fps") {
                options.maxFps = std::stoi(value());
            } else if (arg == "--hdr") {
                options.hdr = true;
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
        options.builtin = fe::findBuiltinRoot(base / "fengine");
        auto config = fe::GameConfig::load(options.game);
        options.pauseOnFocusLoss = config.pauseOnFocusLoss;
        if (options.userData.empty()) {
            // Keyed by the game's id, never its title: a renamed or translated
            // title keeps the player's saves and settings.
            char* path = SDL_GetPrefPath("fengine", config.id.c_str());
            if (!path) {
                throw std::runtime_error(std::string("No user data folder: ") + SDL_GetError());
            }
            options.userData = path;
            SDL_free(path);
        }
        // <user data>/log.txt (the previous run's as log.txt.1) and crash.txt.
        try {
            fe::openLogFile(options.userData);
            fe::installCrashHandler(options.userData);
        } catch (const std::exception& error) {
            fe::log(fe::LogLevel::Warning, std::string("Logging to stderr only: ") + error.what());
        }
        fe::log(fe::LogLevel::Info, "fengine " + config.id + " (" + config.title + "); user data in " +
                                        options.userData.string());
        const int code = fe::MainLoop(std::move(options), std::move(config)).run();
        fe::closeLogFile();
        return code;
    } catch (const std::exception& error) {
        fe::log(fe::LogLevel::Error, error.what());
        return 1;
    }
}
