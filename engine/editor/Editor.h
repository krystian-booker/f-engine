#pragma once
#include "engine/core/Json.h"
#include "engine/editor/MapEdits.h"
#include "engine/nav/Grid.h"
#include <chrono>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <optional>
#include <sol/forward.hpp>
#include <string>
#include <vector>

namespace fe {
class Engine;
class World;

// The F1 developer editor (development builds). Its panels live in one file
// each: Editor.cpp (window, map edits, undo, schema-driven prop forms),
// EditorWorldTab.cpp (entity inspector), EditorMapTab.cpp (map tab, grid
// paint, links, points, marker import), EditorMapObjects.cpp (placements,
// zones, lights, emitters, colliders, environment), EditorDataTab.cpp (data
// tables) and EditorToolsTabs.cpp (console, profiler, launchers), with the
// shared form widgets in EditorWidgets.h. Everything it edits is
// generic engine data — map grids, placements, zones, lights, emitters,
// colliders, links, environment, data files — with forms generated from the
// schemas scripts declare, so game-specific types get proper inspectors
// without engine changes. Game-specific actions (play from a point, data
// validation, launchers) are fe.devtools functions the editor calls.
//
// Map edits change a world's authored map and its live map together; Save
// writes the authored map, so run-time changes scripts make never reach the
// file. Undo and redo are kept per world and map.
class Editor {
public:
    explicit Editor(Engine& engine);
    void toggle();
    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] bool paused() const { return open_ && paused_; }
    // True when ImGui is using the keyboard or mouse (the game should not).
    [[nodiscard]] bool capturesInput() const;
    // Problems inside the editor are shown in its status line, never thrown.
    void draw(double frameMilliseconds, std::uint32_t drawCalls);
    // Script errors pop up even when the editor is closed.
    void drawErrors();

private:
    World* world();
    void close();
    void refreshSchemas();
    // Applies a map edit to the authored and live map; the first edit of a
    // gesture (a drag, a typed field, a paint stroke) records an undo step.
    void edit(World& world, const std::function<void(MapData&)>& change);
    void undo(World& world, bool forward);
    void saveMap(World& world);
    // An fe.devtools function, or nil when the game doesn't define it.
    sol::object devtool(const char* name);
    // True when a Lua call succeeded; otherwise its error goes to the status line.
    bool succeeded(const sol::protected_function_result& result);
    [[nodiscard]] std::string defaultType(const char* group) const;
    [[nodiscard]] const Json* fieldsFor(const std::string& type,
                                        std::initializer_list<const char*> groups) const;
    bool editProps(const Json* fields, Json& props);
    bool editJson(const char* label, Json& value, float height = 120);

    void drawWorldTab();
    void drawEntity(World& world, entt::entity entity);
    void drawMapTab();
    void drawPaint(World& world);
    void drawPlacements(World& world);
    void drawZones(World& world);
    void drawLights(World& world);
    void drawEmitters(World& world);
    void drawColliders(World& world);
    void drawLinks(World& world);
    void drawPoints(World& world);
    void drawEnvironment(World& world);
    void importMarkers(World& world);
    void drawDataTab();
    void saveTable(const Json& table);
    void drawConsoleTab();
    void drawProfilerTab(double frameMilliseconds, std::uint32_t drawCalls);
    void drawLaunchTab();
    void updateOverlays(World& world);
    std::optional<GridPos> hoveredCell(World& world);

    Engine& engine_;
    MapEditHistory history_;
    const World* historyWorld_ = nullptr; // the world the Map tab last checked the history against
    bool open_ = false, paused_ = false, painting_ = false, showGrid_ = true, placing_ = false;
    bool gesture_ = false, cursorShown_ = false;
    std::string worldName_, status_;
    Json schemas_ = Json::object();
    // Grid brush and overlays (rebuilt only when what they show changes)
    GridBrush brush_;
    int brushRadius_ = 0, layer_ = 0;
    std::uint64_t revision_ = 0;
    std::string gridOverlayKey_, zoneOverlayKey_;
    int selectedPlacement_ = -1, selectedZone_ = -1, selectedLight_ = -1, selectedEmitter_ = -1,
        selectedCollider_ = -1, selectedLink_ = -1;
    std::string selectedPoint_, newPoint_ = "new_point";
    entt::entity selectedEntity_ = entt::null;
    // Data editor
    std::string dataTable_;
    Json dataDocument_;
    int dataRecord_ = -1;
    std::vector<std::string> dataProblems_;
    // Console
    std::string consoleInput_;
    std::vector<std::string> consoleLog_, consoleHistory_;
    int historyPosition_ = -1;
    // Profiler and launchers, refreshed on a timer rather than every frame
    using Clock = std::chrono::steady_clock;
    Clock::time_point profiled_{}, launchersListed_{}, schemasFetched_{};
    std::string taskCount_;
    std::vector<std::string> launchers_;
    std::size_t errorsSeen_ = 0;
};
} // namespace fe
