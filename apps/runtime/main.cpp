#include "engine/core/GameConfig.h"
#include "engine/core/Log.h"
#include "engine/runtime/Headless.h"
#include "engine/runtime/MainLoop.h"
#include <SDL3/SDL.h>
#include <exception>
#include <iostream>
#include <string_view>

namespace {
void usage() {
    std::cout << "usage: fengine <game-folder> [options]\n"
                 "  --mute               no audio device\n"
                 "  --tools              open the F1 editor at start (development builds)\n"
                 "  --user-data <dir>    saves and settings folder (default: per-user app data)\n"
                 "  --replay <file>      play a scripted session; exit code reports failures\n"
                 "  --realtime           pace a replay in real time (listening checks)\n"
                 "  --seed <n>           fixed random seed\n"
                 "  --frames <n>         stop after n frames\n"
                 "  --capture <png>      screenshot at frame 30\n"
                 "  --shaders <dir>      compiled shader folder\n"
                 "Headless: fengine --test <game> | --check <game> | --write-api <folder>\n";
}

std::filesystem::path defaultShaders(const char* executable) {
    const auto installed = std::filesystem::path(executable).parent_path() / "shaders";
    if (std::filesystem::is_directory(installed)) {
        return installed;
    }
    return FENGINE_SHADER_DIR;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const int headless = fe::runHeadlessCommand(argc, argv);
        if (headless >= 0) {
            return headless;
        }
        fe::RuntimeOptions options;
        options.shaders = defaultShaders(argv[0]);
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
            } else if (arg == "--user-data") {
                options.userData = value();
            } else if (arg == "--replay") {
                options.replay = value();
            } else if (arg == "--seed") {
                options.seed = std::stoull(value());
            } else if (arg == "--frames") {
                options.frameLimit = std::stoull(value());
            } else if (arg == "--capture") {
                options.capture = value();
            } else if (arg == "--shaders") {
                options.shaders = value();
            } else if (!arg.starts_with("--") && options.game.empty()) {
                options.game = std::string(arg);
            } else {
                throw std::runtime_error("Unknown option " + std::string(arg));
            }
        }
        if (options.game.empty()) {
            usage();
            return 2;
        }
        options.lualib = fe::findLibraryRoot(argv[0]);
        if (!options.replay.empty() && !options.seed) {
            options.seed = 1; // replays are deterministic
        }
        if (options.userData.empty()) {
            const auto config = fe::GameConfig::load(options.game);
            char* path = SDL_GetPrefPath("fengine", config.title.c_str());
            if (!path) {
                throw std::runtime_error(std::string("No user data folder: ") + SDL_GetError());
            }
            options.userData = path;
            SDL_free(path);
        }
        return fe::MainLoop(std::move(options)).run();
    } catch (const std::exception& error) {
        fe::log(fe::LogLevel::Error, error.what());
        return 1;
    }
}
