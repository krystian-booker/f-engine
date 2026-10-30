#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fe {
// How the virtual canvas meets a window of another aspect ratio: Letterbox
// keeps the configured size (bars at the sides), Expand widens it so the UI
// reaches the edges of a wider window (game.json "canvas_fit").
enum class CanvasFit { Letterbox, Expand };

// The few settings the engine itself needs before any script runs.
// Everything else a game wants to configure lives in its own Lua/data.
struct GameConfig {
    std::string title = "f-engine";
    // The stable name of the game's per-user folder (saves, settings, log):
    // 1 to 64 of a-z, 0-9, '_' and '-'. game.json "id", or derived from the
    // title (see gameId); renaming or translating the title keeps the saves.
    std::string id = "f-engine";
    std::string entry = "main";
    int windowWidth = 1280;
    int windowHeight = 720;
    int virtualWidth = 480;
    int virtualHeight = 270;
    int pixelsPerUnit = 32;
    // Stop ticking and pause audio while the window is in the background
    // ("pause_on_focus_loss"; replays never pause).
    bool pauseOnFocusLoss = true;
    // Frame cap while vsync is off ("max_fps"); 0 = uncapped.
    int maxFps = 0;
    CanvasFit canvasFit = CanvasFit::Letterbox;
    // Present to an HDR back buffer when the renderer offers one ("hdr_output",
    // default false; `fengine --hdr` turns it on too), with the frame's white
    // at "hdr_paper_white" nits (default 200, 80 to 10000).
    bool hdrOutput = false;
    float hdrPaperWhite = 200;
    static GameConfig load(const std::filesystem::path& gameFolder);
};

// Whether `id` is a valid game id: 1 to 64 of a-z, 0-9, '_' and '-'.
[[nodiscard]] bool validGameId(std::string_view id);
// The id derived from a title: lower-cased, with each run of spaces turned
// into '-'. Nullopt when the title holds any other character outside the id
// set (punctuation, non-ASCII), in which case game.json must name an `id`.
[[nodiscard]] std::optional<std::string> derivedGameId(std::string_view title);
} // namespace fe
