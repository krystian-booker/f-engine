#pragma once
#include "engine/core/GameConfig.h"
#include "engine/services/PlatformServices.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace fe {
struct RuntimeOptions {
    std::filesystem::path game, lualib, shaders, userData;
    std::filesystem::path replay, record, capture;
    std::optional<std::uint64_t> seed;
    std::uint64_t frameLimit = 0;
    bool mute = false, tools = false, realtime = false;
    // A replay's captures with a `compare` path replace their reference
    // images (for the running backend) instead of being compared.
    bool writeReferences = false;
    // Stop ticking and pause audio while the window is in the background
    // (game.json "pause_on_focus_loss", default true). Replays never pause.
    bool pauseOnFocusLoss = true;
    // Store backend for fe.platform, given the user-data folder; unset uses
    // LocalPlatformServices.
    std::function<std::unique_ptr<PlatformServices>(const std::filesystem::path&)> platform;
};

// The windowed game loop: SDL events, fixed-step ticks, rendering of the
// worlds and canvas scripts drew, the editor and development hot reload.
class MainLoop {
public:
    MainLoop(RuntimeOptions options, GameConfig config);
    int run();

private:
    RuntimeOptions options_;
    GameConfig config_;
};
} // namespace fe
