#pragma once
#include "engine/core/GameConfig.h"
#include "engine/core/GameModule.h"
#include <filesystem>
#include <cstdint>
#include <functional>

namespace fe {
struct AppOptions {
    std::filesystem::path gameFolder;
    std::filesystem::path shaderFolder;
    std::filesystem::path userDataFolder;
    std::filesystem::path capturePath;
    std::filesystem::path replayPath, dumpStatePath;
    std::string startBattle;
    std::uint64_t frameLimit = 0;
    bool mute = false, skipTitle = false, showTools = false, replayRealtime = false;
    std::function<std::unique_ptr<GameModule>()> moduleFactory;
};
class App {
public:
    explicit App(AppOptions options);
    int run();
private:
    AppOptions options_;
    GameConfig config_;
};
}
