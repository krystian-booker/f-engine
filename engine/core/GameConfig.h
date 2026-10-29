#pragma once
#include <filesystem>
#include <string>

namespace fe {
// The few settings the engine itself needs before any script runs.
// Everything else a game wants to configure lives in its own Lua/data.
struct GameConfig {
    std::string title = "f-engine";
    std::string entry = "main";
    int windowWidth = 1280;
    int windowHeight = 720;
    int virtualWidth = 480;
    int virtualHeight = 270;
    int pixelsPerUnit = 32;
    // Stop ticking and pause audio while the window is in the background
    // ("pause_on_focus_loss"; replays never pause).
    bool pauseOnFocusLoss = true;
    static GameConfig load(const std::filesystem::path& gameFolder);
};
} // namespace fe
