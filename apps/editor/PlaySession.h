#pragma once
#include "engine/core/FixedStep.h"
#include "engine/core/GameConfig.h"
#include "engine/world/Camera.h"
#include <bgfx/bgfx.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
class QKeyEvent;

namespace fe {
class Engine;
class FrameRenderer;
class Renderer;
class SdlInput;
class World;
} // namespace fe

namespace fe::editor {
class ViewportWindow;

// Play mode: the game running in an engine of its own, drawn into the Game
// view. Starting creates the engine and the game from nothing, stopping
// destroys both, so nothing the game did while playing reaches the scene
// being edited.
class PlaySession {
public:
    struct Options {
        std::filesystem::path game, builtin, userData;
        GameConfig config;
        bool mute = false;
        // Where to start: the game's editor.play_from(scene, point) hook is
        // called with these after init; an empty scene (or a game without
        // the hook) starts the game from its beginning.
        std::string scene, point;
    };

    // Throws when the game cannot start at all (the entry module is broken).
    PlaySession(Renderer& renderer, ViewportWindow& view, Options options);
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    [[nodiscard]] Engine& engine() { return *engine_; }
    bool paused = false;
    // One logic tick while paused.
    void step();
    // Runs the logic ticks `seconds` of real time are worth.
    void advance(double seconds);
    // Draws the game's frame into the Game view, using bgfx views from
    // `firstView`; returns the first view it left unused.
    // With `sceneCamera` the world on show is also drawn through that camera
    // into a texture of its own (sceneTexture(), `sceneAspect` wide): what
    // the Scene view shows while the game plays. The game frame is drawn
    // even while the Game view is hidden, so that picture stays live.
    std::uint16_t render(std::uint16_t firstView, const Camera* sceneCamera = nullptr, float sceneAspect = 1);
    [[nodiscard]] bgfx::TextureHandle sceneTexture() const;
    // After bgfx::frame().
    void finishFrame(std::uint32_t frame);
    // The game asked to quit (fe.app.quit): the editor stops playing.
    [[nodiscard]] bool wantsQuit() const;
    // Reloads a changed file in the running game (hot reload).
    void reload(const std::filesystem::path& file);

    // The world the Hierarchy shows: the one chosen, else the one the game
    // drew full screen last.
    [[nodiscard]] std::shared_ptr<World> shownWorld() const;
    void showWorld(const std::string& name) { chosenWorld_ = name; }
    [[nodiscard]] std::vector<std::string> worldNames() const;
    [[nodiscard]] std::uint64_t ticks() const { return ticks_; }
    [[nodiscard]] double frameMs() const { return frameMs_; }
    [[nodiscard]] std::uint32_t drawCalls() const;

    // Keyboard focus left the Game view: every held input is released.
    void releaseInput();

private:
    void tick(double dt);
    void key(QKeyEvent* event, bool down);

    Renderer& renderer_;
    ViewportWindow& view_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<FrameRenderer> frame_;
    std::unique_ptr<SdlInput> pads_;
    FixedStep fixed_;
    std::string chosenWorld_;
    std::uint64_t ticks_ = 0;
    double frameMs_ = 0;
    bool gamepads_ = false;
};
} // namespace fe::editor
