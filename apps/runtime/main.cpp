#include "engine/core/App.h"
#include "engine/core/Log.h"
#include <SDL3/SDL_main.h>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    try {
        fe::AppOptions options;
#if FENGINE_WITH_GAME_MODULE
        options.moduleFactory=fe::createGameModule;
#endif
        for (int i = 1; i < argc; ++i) {
            std::string_view argument(argv[i]);
            if (argument == "--help" || argument == "-h") {
                std::cout << "Usage: fengine <game-folder> [--frames N] [--shaders directory]\n"
                             "       [--user-data directory] [--mute] [--skip-title] [--tools]\n"
                             "       [--capture path-without-extension] [--start-battle troop]\n"
                             "       [--replay commands.json] [--replay-realtime] [--dump-state state.json]\n";
                return 0;
            }
            if (argument == "--frames" && i + 1 < argc) {
                std::string_view value(argv[++i]);
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), options.frameLimit);
                if (ec != std::errc{} || end != value.data() + value.size() || !options.frameLimit)
                    throw std::runtime_error("--frames requires a positive integer");
            } else if (argument == "--shaders" && i + 1 < argc) options.shaderFolder = argv[++i];
            else if (argument == "--user-data" && i + 1 < argc) options.userDataFolder = argv[++i];
            else if (argument == "--mute") options.mute = true;
            else if (argument == "--skip-title") options.skipTitle = true;
            else if (argument == "--tools") options.showTools = true;
            else if (argument == "--capture" && i + 1 < argc) options.capturePath = argv[++i];
            else if (argument == "--start-battle" && i + 1 < argc) { options.startBattle=argv[++i];options.skipTitle=true; }
            else if (argument == "--replay" && i + 1 < argc) options.replayPath=argv[++i];
            else if (argument == "--replay-realtime") options.replayRealtime=true;
            else if (argument == "--dump-state" && i + 1 < argc) options.dumpStatePath=argv[++i];
            else if (!argument.starts_with('-') && options.gameFolder.empty()) options.gameFolder = argv[i];
            else throw std::runtime_error("Unknown or incomplete argument: " + std::string(argument));
        }
        if (options.gameFolder.empty()) throw std::runtime_error("Specify a game folder; try fengine samples/sandbox");
        return fe::App(std::move(options)).run();
    } catch (const std::exception& error) {
        fe::log(fe::LogLevel::Error, error.what());
        return 1;
    }
}
