#pragma once
#include "engine/core/AssetCache.h"
#include "engine/core/GameConfig.h"
#include "engine/platform/Display.h"
#include "engine/script/Binder.h"
#include "engine/script/LuaHost.h"
#include "engine/services/Audio.h"
#include "engine/services/Canvas.h"
#include "engine/services/InputState.h"
#include "engine/services/PlatformServices.h"
#include "engine/services/SaveStore.h"
#include "engine/services/ScreenFx.h"
#include "engine/text/TextSystem.h"
#include "engine/world/World.h"
#include <future>
#include <memory>
#include <optional>

namespace fe {
struct EngineOptions {
    std::filesystem::path gameRoot;
    std::filesystem::path libraryRoot; // lualib
    std::filesystem::path userData;
    bool development = true;
    // Tests and replays: binds fe.input.feed and fe.dev.console even when
    // development is off.
    bool testHooks = false;
    std::optional<std::uint64_t> seed;          // fixed seed (tests and replays)
    std::unique_ptr<Audio> audio;               // null = silent NullAudio
    std::unique_ptr<PlatformServices> platform; // null = LocalPlatformServices in userData
    std::string locale = "en-US";               // the player's preferred locale, BCP 47
};

// Requests from scripts that only the windowed runtime can fulfil.
struct AppRequests {
    bool quit = false;
    std::optional<DisplayRequest> display;
    std::optional<std::string> title;
    std::vector<std::filesystem::path> captures; // screenshots to write as PNG
    // World-only captures (save thumbnails): the 3D scenes without the canvas
    // UI, screen effects or editor, downscaled and written atomically as PNG.
    std::vector<std::filesystem::path> sceneCaptures;
};

// One world:draw call. With no rect the world fills the screen; a rect (canvas
// pixels) places it on screen, or sizes the named render target it draws into.
struct WorldDraw {
    std::shared_ptr<World> world;
    std::optional<Rect> rect;
    std::string target;
    std::optional<Camera> camera;     // replaces the world's camera for this draw
    std::optional<PostSettings> post; // replaces the world's post-processing for this draw
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
    // Loads <gameRoot>/game.json.
    explicit Engine(EngineOptions options);
    // Uses `config` when given (a caller that already loaded game.json).
    Engine(EngineOptions&& options, std::optional<GameConfig> config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Loads the entry module and calls its init(); returns false on script error.
    bool start();
    // One fixed step: input, worlds' beginTick, main.update(dt), worlds' tick,
    // main.post_update(dt), audio.
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
    [[nodiscard]] PlatformServices& platform() { return *platform_; }
    // The window and monitors; the windowed runtime keeps this current.
    [[nodiscard]] DisplayState& display() { return display_; }
    // Validates a display change against the known monitors, queues it for
    // the runtime and shows it in display() until the runtime reports back.
    void requestDisplay(const DisplayRequest& request);
    [[nodiscard]] const std::string& locale() const { return locale_; }
    [[nodiscard]] FrameStats& stats() { return stats_; }
    [[nodiscard]] bool development() const { return development_; }
    // fe.input.feed and fe.dev.console are bound (development, tests, replays).
    [[nodiscard]] bool devBindings() const { return development_ || testHooks_; }
    [[nodiscard]] std::uint64_t ticks() const { return ticks_; }
    // Interpolation alpha of the frame being drawn while `draw` runs, 1 otherwise;
    // camera and entity projections default to it so scripts see what renders.
    [[nodiscard]] double drawAlpha() const { return drawAlpha_; }
    [[nodiscard]] double gameTime() const { return time_; }
    [[nodiscard]] glm::ivec2 viewport() const { return viewport_; }
    [[nodiscard]] std::uint64_t seed() const { return seed_; }
    [[nodiscard]] std::uint64_t nextSeed() { return seed_ + ++seedCounter_ * 0x9e3779b97f4a7c15ULL; }

    std::shared_ptr<World> loadWorld(const std::string& name, const std::string& mapId);
    // Background loading: the map file is parsed on a worker thread, then the
    // models, images and sprites it names decode through AssetCache's worker.
    // Returns a ticket; loading advances every tick and while progress is read.
    int loadWorldAsync(const std::string& name, const std::string& mapId);
    // 0..1; 1 once finishLoad will not wait (or the load failed).
    float loadProgress(int ticket);
    // Creates the world on the calling (main) thread, waiting for the map
    // parse if needed; assets still decoding load on first use. Throws the
    // load's error. The ticket is consumed.
    std::shared_ptr<World> finishLoad(int ticket);
    std::shared_ptr<World> world(const std::string& name) const;
    void unloadWorld(const std::string& name);
    [[nodiscard]] const std::map<std::string, std::shared_ptr<World>>& worlds() const { return worlds_; }
    // Worlds scripts asked to draw this frame, in order.
    void queueDraw(const std::shared_ptr<World>& world) { drawList_.push_back({world, {}, {}, {}, {}}); }
    void queueDraw(WorldDraw draw) { drawList_.push_back(std::move(draw)); }
    [[nodiscard]] const std::vector<WorldDraw>& drawList() const { return drawList_; }
    // The world most recently drawn (editor).
    [[nodiscard]] std::shared_ptr<World> focusWorld() const;

    [[nodiscard]] sol::object mainModule() const { return main_; }
    // Writes LuaLS stubs and the Markdown API reference.
    void writeApi(const std::filesystem::path& stubs, const std::filesystem::path& markdown) const;

private:
    struct EmitterVoice {
        std::weak_ptr<World> world;
        entt::entity entity = entt::null;
    };
    void bind();
    // Calls an optional function of the entry module; false on script error.
    template<class... Args> bool callMain(const char* name, Args&&... args);
    [[nodiscard]] std::filesystem::path resolveMap(const std::string& mapId) const;
    void updateAudioEmitters();
    struct PendingWorld {
        enum class Stage { Parsing, Assets, Atlases, Ready, Failed };
        std::string name, mapId;
        std::future<MapData> parse;
        std::optional<MapData> map;
        std::string error;
        Stage stage = Stage::Parsing;
        int ticket = 0;
        std::vector<std::string> sprites;
        std::size_t assetCount = 0;
    };
    void advance(PendingWorld& load);
    std::shared_ptr<World> addWorld(const std::string& name, const std::string& mapId, MapData map);
    GameConfig config_;
    bool development_, testHooks_;
    std::uint64_t seed_, seedCounter_ = 0;
    std::unique_ptr<AssetCache> assets_;
    std::unique_ptr<TextSystem> text_;
    std::unique_ptr<Canvas> canvas_;
    InputState input_;
    ScreenFx screen_;
    std::unique_ptr<Audio> audio_;
    SaveStore saves_;
    AppRequests requests_;
    std::unique_ptr<PlatformServices> platform_;
    DisplayState display_;
    std::string locale_;
    std::map<int, PendingWorld> loads_;
    int nextLoad_ = 1;
    FrameStats stats_;
    BindingDocs docs_;
    std::map<std::string, std::shared_ptr<World>> worlds_;
    std::vector<WorldDraw> drawList_;
    std::weak_ptr<World> focus_;
    std::map<int, EmitterVoice> emitterVoices_;
    std::unique_ptr<LuaHost> lua_;
    std::shared_ptr<lua_State*> scriptState_; // null once the Lua state is closed
    sol::object main_;
    std::uint64_t ticks_ = 0;
    double time_ = 0;
    double drawAlpha_ = 1;
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
void bindPlatform(Engine& engine); // fe.platform and background world loading
} // namespace fe
