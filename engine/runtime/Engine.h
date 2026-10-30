#pragma once
#include "engine/core/AssetCache.h"
#include "engine/core/GameConfig.h"
#include "engine/core/Log.h"
#include "engine/platform/Display.h"
#include "engine/services/Audio.h"
#include "engine/services/Canvas.h"
#include "engine/services/Clipboard.h"
#include "engine/services/InputState.h"
#include "engine/services/PlatformServices.h"
#include "engine/services/SaveStore.h"
#include "engine/services/ScreenFx.h"
#include "engine/text/TextSystem.h"
#include "engine/world/Camera.h"
#include "engine/world/MapData.h"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// This header pulls in neither sol2 nor EnTT: code that needs the Lua state
// includes engine/script/LuaHost.h, and code that touches entities
// engine/world/World.h.
namespace fe {
class BindingDocs;
class EmitterAudio;
class LuaHost;
class World;
class WorldLoader;

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
    std::unique_ptr<SaveBackend> saveBackend;   // null = files in userData
    std::unique_ptr<Clipboard> clipboard;       // null = MemoryClipboard (headless, tests, replays)
    std::string locale = "en-US";               // the player's preferred locale, BCP 47
    // Decoded bytes the asset cache keeps before evicting assets no loaded
    // world references (see AssetCache).
    std::size_t assetBudget = AssetCache::DefaultBudget;
    // Installed with setLogSink (engine/core/Log.h) for the engine's lifetime:
    // every log record also reaches it (telemetry, crash reporters).
    LogSink logSink;
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

// What the development editor answers scripts (set by the windowed runtime
// when the editor exists). `leaveMap(world)` is asked before the game leaves a
// world's map: "ok", or "pending" while the editor prompts about unsaved
// edits; `leaveResult()` then reads the answer, "continue" or "cancel".
struct EditorHooks {
    std::function<std::string(const std::string& world)> leaveMap;
    std::function<std::optional<std::string>()> leaveResult;
};

// One world:draw call. With no rect the world fills the screen; a rect (canvas
// pixels) places it on screen, or sizes the named render target it draws into.
struct WorldDraw {
    std::shared_ptr<World> world;
    std::optional<Rect> rect;
    std::string target;
    std::optional<Camera> camera;     // replaces the world's camera for this draw
    std::optional<PostSettings> post; // replaces the world's post-processing for this draw
    // Editor only: the window pixels {x, y, w, h} the world is drawn into
    // (the Scene viewport); takes precedence over `rect`.
    std::optional<glm::ivec4> pixelRect;
    // The camera override is not interpolated between ticks (an editor camera
    // moved while the game is paused shows where it is at once).
    bool snapCamera = false;
};

struct FrameStats {
    double luaUpdate = 0, luaDraw = 0; // milliseconds, last frame
    // Filled by the windowed runtime after each frame; 0 headless.
    double frame = 0;            // milliseconds between the last two frames
    std::uint32_t drawCalls = 0; // world draw calls of the last frame
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
    // The window's size in pixels. With game.json canvas_fit = "expand" the
    // canvas widens to the viewport's aspect ratio (never below the
    // configured virtual width); otherwise it keeps the configured size.
    void setViewport(int width, int height);

    [[nodiscard]] const GameConfig& config() const { return config_; }
    [[nodiscard]] LuaHost& lua() { return *lua_; }
    [[nodiscard]] BindingDocs& docs() { return *docs_; }
    [[nodiscard]] AssetCache& assets() { return *assets_; }
    [[nodiscard]] TextSystem& text() { return *text_; }
    [[nodiscard]] Canvas& canvas() { return *canvas_; }
    [[nodiscard]] InputState& input() { return input_; }
    [[nodiscard]] ScreenFx& screen() { return screen_; }
    [[nodiscard]] Audio& audio() { return *audio_; }
    [[nodiscard]] SaveStore& saves() { return saves_; }
    [[nodiscard]] AppRequests& requests() { return requests_; }
    [[nodiscard]] EditorHooks& editorHooks() { return editorHooks_; }
    [[nodiscard]] PlatformServices& platform() { return *platform_; }
    [[nodiscard]] Clipboard& clipboard() { return *clipboard_; }
    // Content packs laid over the game folder at start (<game>/packs/*.pak,
    // then <user data>/packs/*.pak, each in name order): "packs/<name>.pak"
    // or "user://packs/<name>.pak", in mount order; the last mounted wins.
    [[nodiscard]] const std::vector<std::string>& packs() const { return packs_; }
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
    // Drops a background load that will not be finished (a cancelled warp):
    // a worker still parsing the map finishes and is discarded, assets it
    // queued stay cached, and the ticket becomes unknown.
    void cancelLoad(int ticket);
    std::shared_ptr<World> world(const std::string& name) const;
    // Also runs releaseUnusedAssets().
    void unloadWorld(const std::string& name);
    [[nodiscard]] const std::map<std::string, std::shared_ptr<World>>& worlds() const { return worlds_; }
    // Asset paths the loaded worlds use: their geometry, sky panorama and
    // backdrops, LUTs, weather and emitter sprites, entity sprites (with
    // their atlases and palettes) and meshes.
    [[nodiscard]] std::set<std::string> referencedAssets() const;
    // Evicts least recently used assets outside referencedAssets() while the
    // cache is over its budget; returns how many were evicted.
    std::size_t releaseUnusedAssets();
    // Worlds scripts asked to draw this frame, in order.
    void queueDraw(const std::shared_ptr<World>& world) { drawList_.push_back(WorldDraw{.world = world}); }
    void queueDraw(WorldDraw draw) { drawList_.push_back(std::move(draw)); }
    [[nodiscard]] const std::vector<WorldDraw>& drawList() const { return drawList_; }
    // The world most recently drawn (editor).
    [[nodiscard]] std::shared_ptr<World> focusWorld() const;

    // start() loaded the entry module (a table). Its functions are reached
    // through lua().require(config().entry).
    [[nodiscard]] bool hasMain() const;
    // Writes LuaLS stubs and the Markdown API reference.
    void writeApi(const std::filesystem::path& stubs, const std::filesystem::path& markdown) const;

private:
    // The entry module and the Lua state cell (Engine.cpp): what needs sol2.
    struct Script;
    void bind();
    [[nodiscard]] std::filesystem::path resolveMap(const std::string& mapId) const;
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
    EditorHooks editorHooks_;
    std::unique_ptr<PlatformServices> platform_;
    std::unique_ptr<Clipboard> clipboard_;
    std::vector<std::string> packs_;
    DisplayState display_;
    std::string locale_;
    std::unique_ptr<WorldLoader> loader_;
    std::unique_ptr<EmitterAudio> emitters_;
    FrameStats stats_;
    std::unique_ptr<BindingDocs> docs_;
    std::map<std::string, std::shared_ptr<World>> worlds_;
    std::vector<WorldDraw> drawList_;
    std::weak_ptr<World> focus_;
    std::unique_ptr<LuaHost> lua_;
    std::unique_ptr<Script> script_;
    bool ownsLogSink_ = false;
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
