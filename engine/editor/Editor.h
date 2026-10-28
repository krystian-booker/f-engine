#pragma once
#include "engine/core/Json.h"
#include "engine/nav/Grid.h"
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace fe {
class Engine;
class World;

// The F1 developer editor (development builds). Everything it edits is
// generic engine data — map grids, placements, zones, lights, environment,
// data files — with forms generated from the schemas scripts declare, so
// game-specific types get proper inspectors without engine changes.
class Editor {
public:
    explicit Editor(Engine& engine);
    void toggle();
    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] bool paused() const { return open_ && paused_; }
    // True when ImGui is using the keyboard or mouse (the game should not).
    [[nodiscard]] bool capturesInput() const;
    void draw(double frameMilliseconds, std::uint32_t drawCalls);
    // Script errors pop up even when the editor is closed.
    void drawErrors();

private:
    World* world();
    void refreshSchemas();
    void checkpoint();
    void restore(const Json& snapshot);
    void saveMap();
    void drawWorldTab();
    void drawMapTab();
    void drawPaint(World& world);
    void drawPlacements(World& world);
    void drawZones(World& world);
    void drawLights(World& world);
    void drawEnvironment(World& world);
    void drawPoints(World& world);
    void importMarkers(World& world);
    void drawDataTab();
    void drawConsoleTab();
    void drawProfilerTab(double frameMilliseconds, std::uint32_t drawCalls);
    void drawLaunchTab();
    void updateOverlays(World& world);
    std::optional<GridPos> hoveredCell(World& world);
    bool editProps(const std::string& schemaName, bool entity, Json& props);

    Engine& engine_;
    bool open_ = false, paused_ = false, painting_ = false, showGrid_ = true, placing_ = false;
    std::string worldName_, status_;
    Json schemas_ = Json::object();
    std::deque<Json> undo_, redo_;
    // Paint brush
    int brushRadius_ = 0, layer_ = 0, paintMode_ = 0, ramp_ = 0;
    float height_ = 0;
    bool walkable_ = true;
    std::string tag_;
    int selectedPlacement_ = -1, selectedZone_ = -1, selectedLight_ = -1, selectedEntity_ = -1;
    // Data editor
    std::string dataTable_;
    Json dataDocument_;
    int dataRecord_ = -1;
    // Console
    std::string consoleInput_;
    std::vector<std::string> consoleLog_;
    std::size_t errorsSeen_ = 0;
};
} // namespace fe
