#pragma once
#include "engine/core/AssetCache.h"
#include "engine/core/GameConfig.h"
#include "engine/script/Binder.h"
#include "engine/script/LuaHost.h"
#include "engine/services/Audio.h"
#include "engine/services/Canvas.h"
#include "engine/services/InputState.h"
#include "engine/services/SaveStore.h"
#include "engine/services/ScreenFx.h"
#include "engine/text/TextSystem.h"
#include "engine/world/World.h"
#include <memory>
#include <optional>

namespace fe {
struct EngineOptions {
    std::filesystem::path gameRoot;
    std::filesystem::path libraryRoot; // lualib
    std::filesystem::path userData;
    bool development = true;
    std::optional<std::uint64_t> seed; // fixed seed (tests and replays)
    std::unique_ptr<Audio> audio;      // null = silent NullAudio
};

// Requests from scripts that only the windowed runtime can fulfil.
struct AppRequests {
    bool quit = false;
    std::optional<bool> fullscreen;
    std::optional<std::string> title;
    std::vector<std::filesystem::path> captures; // screenshots to write as PNG
};

struct FrameStats {
    double luaUpdate = 0, luaDraw = 0; // milliseconds, last frame
    std::map<std::string, double> zones;
};

// The engine core: owns every service and the Lua host, and drives the
// game's entry module. It has no window, GPU or audio device of its own, so
// tests and validation run the same code headless.
class Engine {
public:
    explicit Engine(EngineOptions options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Loads the entry module and calls its init(); returns false on script error.
    bool start();
    void tick(double dt);
    void draw(double alpha);
    // A file changed on disk (development hot reload).
    void reload(const std::filesystem::path& file);
    void setViewport(int width, int height);

    [[nodiscard]] const GameConfig& config() const { return config_; }
    [[nodiscard]] LuaHost& lua() { return *lua_; }
    [[nodiscard]] BindingDocs& docs() { return docs_; }
    [[nodiscard]] AssetCache& assets() { return *assets_; }
    [[nodiscard]] TextSystem& text() { return *text_; }
    [[nodiscard]] Canvas& canvas() { return *canvas_; }
    [[nodiscard]] InputState& input() { return input_; }
    [[nodiscard]] ScreenFx& screen() { return screen_; }
    [[nodiscard]] Audio& audio() { return *audio_; }
    [[nodiscard]] SaveStore& saves() { return saves_; }
    [[nodiscard]] AppRequests& requests() { return requests_; }
    [[nodiscard]] FrameStats& stats() { return stats_; }
    [[nodiscard]] bool development() const { return development_; }
    [[nodiscard]] std::uint64_t ticks() const { return ticks_; }
    [[nodiscard]] double gameTime() const { return time_; }
    [[nodiscard]] glm::ivec2 viewport() const { return viewport_; }
    [[nodiscard]] std::uint64_t seed() const { return seed_; }
    [[nodiscard]] std::uint64_t nextSeed() { return seed_ + ++seedCounter_ * 0x9e3779b97f4a7c15ULL; }

    std::shared_ptr<World> loadWorld(const std::string& name, const std::string& mapId);
    std::shared_ptr<World> world(const std::string& name) const;
    void unloadWorld(const std::string& name);
    [[nodiscard]] const std::map<std::string, std::shared_ptr<World>>& worlds() const { return worlds_; }
    // Worlds scripts asked to draw this frame, in order.
    void queueDraw(const std::shared_ptr<World>& world) { drawList_.push_back(world); }
    [[nodiscard]] const std::vector<std::shared_ptr<World>>& drawList() const { return drawList_; }
    // The world most recently drawn (editor, audio listener).
    [[nodiscard]] std::shared_ptr<World> focusWorld() const;

    [[nodiscard]] sol::object mainModule() const { return main_; }
    // Writes LuaLS stubs and the Markdown API reference.
    void writeApi(const std::filesystem::path& stubs, const std::filesystem::path& markdown) const;

private:
    void bind();
    [[nodiscard]] bool hasMain() const;
    [[nodiscard]] std::filesystem::path resolveMap(const std::string& mapId) const;
    void updateAudioEmitters();
    GameConfig config_;
    bool development_;
    std::uint64_t seed_, seedCounter_ = 0;
    std::unique_ptr<AssetCache> assets_;
    std::unique_ptr<TextSystem> text_;
    std::unique_ptr<Canvas> canvas_;
    InputState input_;
    ScreenFx screen_;
    std::unique_ptr<Audio> audio_;
    SaveStore saves_;
    AppRequests requests_;
    FrameStats stats_;
    BindingDocs docs_;
    std::map<std::string, std::shared_ptr<World>> worlds_;
    std::vector<std::shared_ptr<World>> drawList_;
    std::weak_ptr<World> focus_;
    std::unique_ptr<LuaHost> lua_;
    sol::object main_;
    std::uint64_t ticks_ = 0;
    double time_ = 0;
    glm::ivec2 viewport_{1280, 720};
};

// Each binding file contributes one of these.
void bindCore(Engine& engine);
void bindSave(Engine& engine);
void bindInput(Engine& engine);
void bindText(Engine& engine);
void bindCanvas(Engine& engine);
void bindAudio(Engine& engine);
void bindScreen(Engine& engine);
void bindWorld(Engine& engine);
} // namespace fe
