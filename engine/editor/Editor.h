#pragma once
#include "engine/assets/SpriteSheetBuilder.h"
#include "engine/core/Json.h"
#include "engine/editor/AssetCatalog.h"
#include "engine/editor/EditorCamera.h"
#include "engine/editor/MapEdits.h"
#include "engine/editor/SceneHandles.h"
#include "engine/editor/ScenePicking.h"
#include "engine/editor/Selection.h"
#include "engine/editor/WalkBake.h"
#include "engine/nav/Grid.h"
#include <chrono>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <filesystem>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sol/forward.hpp>
#include <string>
#include <vector>

struct SDL_Window;

struct ImDrawList;
struct ImVec2;

namespace fe {
class Engine;
class World;
class GpuAssets;
class FrameRenderer;
struct WorldDraw;

// Files the OS file dialog chose, handed over from the dialog's thread.
struct ImportQueue {
    enum class Purpose { Assets, Geometry, NewMapGeometry };
    std::mutex mutex;
    std::vector<std::filesystem::path> files;
    std::string folder; // game-relative destination (assets)
    Purpose purpose = Purpose::Assets;
    int returned = 0, cancelled = 0, failed = 0; // how the dialog came back (tests)
};

// What the main loop hands the editor each frame.
struct EditorFrame {
    double frameMilliseconds = 0;
    std::uint32_t drawCalls = 0; // the previous frame's world draw calls
    double alpha = 1;            // tick interpolation of this frame
    glm::ivec2 viewport{1};      // window size in pixels
};

// The F1 developer editor (development builds): a full-window dock layout
// whose centre is the Scene viewport. The focus world is drawn into that
// region with the editor's own camera; panels around it are the Hierarchy
// (every authored object and live entity), the Inspector (the selection's
// form), the Map panel (grid paint, links, environment), Assets, Data,
// Console, Profiler and Launch. Play mode hides the panels and gives the
// window back to the game.
//
// Its panels live in one file each: Editor.cpp (shared state, map edits,
// undo, schema-driven prop forms), EditorShell.cpp (dockspace, menus, play
// bar, shortcuts), EditorViewport.cpp (camera, picking, gizmos, handles,
// drops), EditorHierarchy.cpp, EditorInspector.cpp (the selection's form,
// with the per-kind forms in EditorMapObjects.cpp and EditorWorldTab.cpp),
// EditorMapTab.cpp (the Map panel), EditorDataTab.cpp and
// EditorToolsTabs.cpp. Everything it edits is generic engine data with forms
// generated from the schemas the game declares; game-specific actions are
// optional hooks the game's entry module offers in its `editor` table
// (describe, play_from, placement_changed, inspect_entity, run_page, tasks,
// cancel_task, recent_events, validate_table, task_count, list, launch; see
// docs/editor.md). Without them the editor still opens, previews, edits and
// saves maps on its own.
//
// Map edits change a world's authored map and its live map together; Save
// writes the authored map, so run-time changes scripts make never reach the
// file. Undo and redo are kept per world and map.
// The placements a map's walkability bake reads (Editor::autoRebake compares them).
[[nodiscard]] Json bakedPlacementsJson(const MapData& map);

class Editor {
public:
    enum class Mode { Edit, Play };
    enum class Tool { Select, Translate, Rotate, Scale, Paint };

    explicit Editor(Engine& engine);
    // Writes the dock layout and detaches it from ImGui: the context outlives
    // the editor and would otherwise save through a pointer into this object.
    ~Editor();
    void toggle();
    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] bool paused() const { return open_ && paused_; }
    // The game is about to leave `worldName`'s map (a door, a zone). With
    // unsaved edits the editor switches to Edit mode and asks (save, discard
    // or stay) and answers "pending"; otherwise "ok". leaveMapResult() then
    // holds "continue" or "cancel" until the next request.
    std::string requestLeaveMap(const std::string& worldName);
    [[nodiscard]] std::optional<std::string> leaveMapResult() const { return leaveResult_; }
    // True when the editor is using the keyboard or mouse (the game should
    // not): in Edit mode always, in Play mode when an ImGui widget has them.
    [[nodiscard]] bool capturesInput() const;
    // Where ImGui keeps the dock layout between runs (set once at start-up).
    void setLayoutFile(std::filesystem::path file);
    // The frame renderer, for thumbnails (sprite atlases, and models drawn
    // once each into a kept render target); without it icons stand in.
    void attach(FrameRenderer& frame);
    // The SDL window, so file dialogs attach to it as sheets (and the frame
    // loop keeps running while one is open).
    void setWindow(SDL_Window* window) { window_ = window; }
    // Problems inside the editor are shown in its status line, never thrown.
    // Runs before the frame is rendered, so what it decides about the Scene
    // viewport (adjustDraws) applies to this frame.
    void draw(const EditorFrame& frame);
    // In Edit mode, redirects the focus world's draw into the Scene viewport
    // with the editor camera; otherwise leaves the list alone.
    void adjustDraws(std::vector<WorldDraw>& draws);
    // A file dropped onto the window (OS drag and drop), at a window point.
    void dropFile(const std::filesystem::path& file, glm::vec2 windowPoint);
    // Script errors pop up even when the editor is closed.
    void drawErrors();

private:
    using Clock = std::chrono::steady_clock;

    // --- Shared state and helpers (Editor.cpp)
    // The world the editor shows. Panels hold the pointer for the frame, so a
    // Lua call-out that replaces the world (play_from, a warp) cannot pull it
    // from under them; the next frame picks up the new one.
    std::shared_ptr<World> world();
    void close();
    void refreshSchemas();
    // Applies a map edit to the authored and live map; the first edit of a
    // gesture (a drag, a typed field, a paint stroke) records an undo step.
    void edit(World& world, const std::function<void(MapData&)>& change);
    void undo(World& world, bool forward);
    void saveMap(World& world);
    // A hook of the entry module's `editor` table, or nil when the game doesn't define it.
    sol::object devtool(const char* name);
    // True when a Lua call succeeded; otherwise its error goes to the status line.
    bool succeeded(const sol::protected_function_result& result);
    [[nodiscard]] std::string defaultType(const char* group) const;
    [[nodiscard]] const Json* fieldsFor(const std::string& type,
                                        std::initializer_list<const char*> groups) const;
    bool editProps(const Json* fields, Json& props);
    // One value by its exported schema spec ({type, of, fields, options,
    // asset, ...}): scalars as widgets, lists as rows with add/remove/move,
    // records as nested groups, maps as key/value rows; anything else as JSON.
    bool editValue(const char* label, const Json& spec, Json& value, int depth = 0);
    bool editJson(const char* label, Json& value, float height = 120);
    void setMode(Mode mode);
    void select(const Selection& selection);
    // After an authored placement changed (not just moved): respawn its
    // entity through the game's editor.placement_changed hook (or the preview).
    void placementChanged(World& world, const std::string& id);
    // Respawns every placement that differs between two maps (undo, redo).
    void placementsChanged(World& world, const MapData& before);
    void deleteSelection(World& world);
    void deleteOne(World& world, const Selection& selection);
    void duplicateSelection(World& world);
    // Ctrl+click: adds or removes a second selection (the primary stays).
    void toggleExtra(const Selection& selection);
    [[nodiscard]] bool isSelected(const Selection& selection) const;
    // Moves every extra selection by the same step as the primary just moved.
    void moveExtras(World& world, glm::vec3 delta);

    // --- Shell (EditorShell.cpp)
    void drawEditMode(const EditorFrame& frame);
    void drawMapDialogs(World* world);
    // Runs `action` now, or after the unsaved-edits prompt when the map has edits.
    void confirmDiscard(World* world, const char* what, std::function<void()> action);
    void openMap(const std::string& id);
    // The fallback of openMap for a game without an `editor.play_from` hook.
    void openPreview(const std::string& id);
    void respawnPreview(World& w, const std::string& id);
    // The world openPreview loads maps into.
    static constexpr const char* PreviewWorld = "editor_preview";
    void newMap();
    void drawPlayBar();
    void drawMenuBar(const EditorFrame& frame);
    void drawStatusBar();
    void buildLayout(unsigned int dockspace);
    void handleShortcuts(World* world);

    // --- Scene viewport (EditorViewport.cpp)
    void drawViewport(const EditorFrame& frame);
    void drawViewportToolbar(World& world);
    void viewportCamera(World& world, bool hovered, bool active);
    void drawHandles(World& world, ImDrawList* draw, double alpha);
    // Returns true while a gizmo is being used (clicks then do not pick).
    bool drawGizmo(World& world, ImDrawList* draw, const ImVec2& min, const ImVec2& size, double alpha);
    void groundDrag(World& world, bool hovered, bool active, double alpha);
    void pickAndSelect(World& world, bool hovered, bool clicked, double alpha);
    void acceptViewportDrop(World& world, double alpha);
    void frameSelection(World& world, double alpha);
    [[nodiscard]] glm::vec2 mousePixels() const;
    [[nodiscard]] float pixelScale() const;
    [[nodiscard]] const Camera& sceneCamera(const World& world) const;
    // The mouse's world ray through the Scene viewport, when the mouse is
    // over it and no widget has it.
    [[nodiscard]] std::optional<ViewRay> mouseRay(World& world, double alpha) const;

    // --- Scripted input (EditorScript.cpp): FENGINE_EDITOR_SCRIPT names a
    // JSON list of steps (mouse, clicks, keys, text, captures, checks) the
    // editor plays through ImGui's input queue, for automated walkthroughs.
    void loadScript();
    void runScript();
    // --- Hierarchy (EditorHierarchy.cpp)
    void drawHierarchy();
    // --- Assets (EditorAssetsPanel.cpp) and Scripts (EditorScripts.cpp)
    void drawAssetsPanel();
    void drawAssetDetails(const AssetEntry& entry);
    void drawSpriteWizard();
    void openImportDialog(ImportQueue::Purpose purpose = ImportQueue::Purpose::Assets);
    // Files the OS dialog returned since the last frame (drained once per frame).
    std::vector<std::filesystem::path> takeChosenFiles(ImportQueue::Purpose& purpose, std::string& folder);
    // Copies a model in as the current map's geometry and imports its markers.
    void importGeometry(World& world, const std::filesystem::path& file);
    // Copies files into the game folder (`folder`, game-relative) and rescans;
    // a PNG without an .anim.json opens the sprite sheet wizard.
    void importFiles(const std::vector<std::filesystem::path>& files, const std::string& folder);
    void drawScriptsPanel();
    // --- Pickers (EditorPickers.cpp): combo boxes with a filter that list
    // what a field may hold, accept drops from the Assets panel and show a
    // thumbnail; each returns true when the value changed.
    bool assetPicker(const char* label, std::string& value, AssetKind kind, bool optional = false);
    bool modulePicker(const char* label, std::string& value, const std::string& folder,
                      bool optional = false);
    bool mapPicker(const char* label, std::string& value, bool optional = false);
    bool pointPicker(const char* label, std::string& value, const std::string& mapId, bool optional = false);
    bool refPicker(const char* label, std::string& value, const std::string& table, bool optional = false);
    bool clipPicker(const char* label, std::string& value, const std::string& animation);
    bool choicePicker(const char* label, std::string& value, const std::vector<std::string>& candidates,
                      const std::vector<std::string>& labels, const char* hint, bool optional,
                      AssetKind thumbnail);
    // A sprite's first idle frame or an image, `size` points tall; false when
    // nothing could be shown (no GPU assets, or the file is missing).
    bool drawThumbnail(const std::string& path, AssetKind kind, float size);
    // Queues a model for a thumbnail draw (one per frame); drawThumbnail shows
    // it once the frame renderer has kept it.
    void requestModelThumbnail(const std::string& path);
    void queueThumbnailDraws(std::vector<WorldDraw>& draws);
    [[nodiscard]] std::vector<std::string> recordIds(const std::string& table);
    [[nodiscard]] std::vector<std::string> mapPoints(const std::string& mapId);
    // --- Inspector (EditorInspector.cpp)
    void drawInspector();
    // Per-kind forms (EditorMapObjects.cpp, EditorWorldTab.cpp)
    void drawPlacementForm(World& world, std::size_t index);
    void drawZoneForm(World& world, std::size_t index);
    void drawLightForm(World& world, std::size_t index);
    void drawEmitterForm(World& world, std::size_t index);
    void drawColliderForm(World& world, std::size_t index);
    void drawPointForm(World& world, const std::string& name);
    void drawEntity(World& world, entt::entity entity);
    void addPlacement(World& world);
    void addZone(World& world);
    void addLight(World& world);
    void addEmitter(World& world);
    void addCollider(World& world);
    void addPoint(World& world, const std::string& name);

    // --- Map panel (EditorMapTab.cpp)
    void drawMapTab();
    void drawPaint(World& world);
    void drawWalkability(World& world);
    void drawBakeOptions(World& world);
    // Re-derives walkability (region: only those cells) with the map's stored
    // options, or with `options` as given; `joinGesture` makes the bake part of
    // the undo step the edit that caused it made.
    void rebake(World& world, std::optional<glm::ivec4> region, bool joinGesture);
    void rebake(World& world, const BakeOptions& options, bool joinGesture, bool storeOptions);
    // After a gesture: re-bakes around the baked placements it changed (Auto bake).
    void autoRebake(World& world);
    void refreshBakeStatus(World& world);
    void drawLinks(World& world);
    void drawEnvironment(World& world);
    void importMarkers(World& world);
    void updateOverlays(World& world);
    void updateZoneOverlay(World& world);
    std::optional<GridPos> hoveredCell(World& world);
    // --- Data, tools (EditorDataTab.cpp, EditorToolsTabs.cpp)
    void drawDataTab();
    void saveTable(const Json& table);
    void drawConsoleTab();
    void drawProfilerTab(double frameMilliseconds, std::uint32_t drawCalls);
    void drawLaunchTab();

    Engine& engine_;
    MapEditHistory history_;
    const World* historyWorld_ = nullptr; // the world the Map panel last checked the history against
    bool open_ = false, paused_ = false, painting_ = false, showGrid_ = true;
    bool pauseInEdit_ = true; // entering Edit mode pauses the game
    bool gesture_ = false, cursorShown_ = false;
    Mode mode_ = Mode::Edit;
    Tool tool_ = Tool::Translate;
    bool snap_ = true, gameCamera_ = false, layoutBuilt_ = false;
    unsigned int dockspaceId_ = 0;
    std::string worldName_, status_, layoutFile_;
    Json schemas_ = Json::object();
    // Grid brush and overlays (rebuilt only when what they show changes)
    GridBrush brush_;
    int brushRadius_ = 0, layer_ = 0;
    enum class BrushShape { Single, Rectangle, Flood };
    BrushShape brushShape_ = BrushShape::Single;
    bool clearOverrides_ = false; // the brush hands cells back to the bake
    bool showBoxBlocked_ = true, showOverrides_ = true;
    std::optional<GridPos> fillStart_;
    BakeOptions bakeOptions_; // the Bake options popup's working copy
    bool bakeStale_ = false;  // the bake's inputs changed since it ran
    std::string bakeStatus_;
    std::uint64_t bakeStatusRevision_ = ~std::uint64_t{0};
    std::string bakeStatusWorld_;
    // The placements the bake reads, as they were when the current gesture's
    // first edit happened; a change re-bakes the cells they cover.
    std::optional<Json> gestureBakeSnapshot_;
    std::uint64_t revision_ = 0;
    std::string gridOverlayKey_, zoneOverlayKey_;
    // Scene viewport
    EditorCamera editorCamera_;
    bool cameraAdopted_ = false;
    ViewRect viewRect_;
    bool viewportHovered_ = false, viewportActive_ = false, gizmoUsing_ = false, groundDragging_ = false;
    glm::vec2 viewportMinPoints_{0}, viewportSizePoints_{0}; // ImGui points
    double alpha_ = 1;
    HandleOptions handles_;
    HandleBatch handleBatch_;
    Selection selection_, hovered_;
    std::vector<Selection> extra_; // Ctrl+click adds more; they move and delete with the selection
    // Map file dialogs (File menu).
    struct MapDialog {
        enum class Kind { None, New, SaveAs, Rename, Delete, Unsaved };
        Kind kind = Kind::None;
        std::string id, name, geometry, problem, what;
        int width = 20, depth = 16;
        std::function<void()> then;      // the action an Unsaved prompt guards
        std::function<void()> cancelled; // Cancel or Escape on an Unsaved prompt
    } mapDialog_;
    bool leavePending_ = false; // a scripted map change waits on the Unsaved prompt
    std::optional<std::string> leaveResult_;
    std::vector<std::string> pendingRespawns_; // placement ids to respawn when the gesture ends
    // Data editor
    std::string dataTable_;
    Json dataDocument_;
    int dataRecord_ = -1;
    std::vector<std::string> dataProblems_;
    // Console
    std::string consoleInput_;
    std::vector<std::string> consoleLog_, consoleHistory_;
    int historyPosition_ = -1;
    // Hierarchy
    std::string hierarchyFilter_, newPoint_ = "new_point";
    // Profiler and launchers, refreshed on a timer rather than every frame
    Clock::time_point profiled_{}, launchersListed_{}, schemasFetched_{};
    std::string taskCount_;
    std::vector<std::string> launchers_;
    std::size_t errorsSeen_ = 0;
    // Assets
    AssetCatalog catalog_;
    Clock::time_point catalogScanned_{};
    std::string assetFolder_, assetFilter_, assetSelected_;
    int assetKindFilter_ = -1; // -1 = every kind
    bool assetGrid_ = true;
    GpuAssets* gpu_ = nullptr;
    FrameRenderer* frame_ = nullptr;
    SDL_Window* window_ = nullptr;
    // A hidden world holding one mesh, drawn into a render target per model.
    std::shared_ptr<World> thumbnailWorld_;
    entt::entity thumbnailEntity_ = entt::null;
    std::vector<std::string> thumbnailQueue_;
    std::set<std::string> thumbnailsDrawn_;
    std::uint64_t thumbnailRevision_ = 0;
    // Record ids per table and point names per map, refreshed on a timer.
    std::map<std::string, std::pair<Clock::time_point, std::vector<std::string>>> recordIds_, mapPoints_;
    // The sprite sheet wizard's form (an image path in the game folder).
    struct SheetWizard {
        bool open = false;
        std::string image;
        int imageWidth = 0, imageHeight = 0;
        SheetSpec spec;
        std::string problem;
    } wizard_;
    // Files chosen in the OS file dialog, delivered on its own thread.
    std::shared_ptr<ImportQueue> importQueue_;
    // Files dropped onto the window since the last frame, with where.
    std::vector<std::pair<std::filesystem::path, glm::vec2>> droppedFiles_;
    // Scripted input
    Json script_ = Json::array();
    std::size_t scriptStep_ = 0;
    std::uint64_t scriptFrame_ = 0;
    int scriptPassed_ = 0, scriptFailed_ = 0;
    std::optional<glm::vec2> scriptMouse_;                // where the script put the mouse (points)
    std::vector<std::pair<int, bool>> scriptKeyReleases_; // keys to release next frame
    std::vector<int> scriptLateReleases_;                 // modifiers to release the frame after
};
} // namespace fe
