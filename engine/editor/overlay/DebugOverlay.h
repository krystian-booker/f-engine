#pragma once
#include "engine/core/Json.h"
#include "engine/editor/Selection.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fe {
class Engine;

// The running game's debug overlay (F1 in development builds): what a
// developer wants to look at while playing outside the editor application.
// The live entities with every component (editable), a Lua console, frame
// and script timings with pause and single-step, the game's running tasks
// and its launchers. Script errors pop up whether it is open or not.
//
// Scenes are authored in the editor application (fengine_editor); nothing
// here writes a file.
class DebugOverlay {
public:
    explicit DebugOverlay(Engine& engine);
    ~DebugOverlay();
    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    void toggle() { open_ = !open_; }
    [[nodiscard]] bool open() const { return open_; }
    // Whether the overlay, not the game, should get this frame's input.
    [[nodiscard]] bool capturesInput() const;
    // The game is held (the Profiler tab's Pause); takeStep() lets one tick
    // through when Step was pressed.
    [[nodiscard]] bool paused() const { return open_ && paused_; }
    [[nodiscard]] bool takeStep();
    // Where the overlay's window positions are kept between runs.
    void setLayoutFile(const std::filesystem::path& file);

    // Between the renderer's beginFrame and endFrame.
    void drawErrors();
    void draw(double frameMilliseconds, std::uint32_t drawCalls);

private:
    void drawEntities();
    void drawConsole();
    void drawProfiler(double frameMilliseconds, std::uint32_t drawCalls);
    void drawTasks();
    void drawLaunch();

    using Clock = std::chrono::steady_clock;
    Engine& engine_;
    bool open_ = false, paused_ = false, step_ = false;
    std::string layoutFile_;
    std::size_t errorsSeen_ = 0;
    std::string worldName_, filter_, problem_;
    Selection selected_;
    std::vector<std::string> consoleLog_, consoleHistory_;
    std::string consoleInput_;
    int historyPosition_ = -1;
    Json tasks_ = Json::array();
    std::vector<std::string> launchers_;
    std::string taskCount_ = "-";
    Clock::time_point polled_{};
};
} // namespace fe
