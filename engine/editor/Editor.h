#pragma once
#include "engine/editor/GridPicking.h"
#include "engine/core/Json.h"
#include <cstdint>
#include <vector>

namespace fe {
class GameSession;
class Editor {
public:
    explicit Editor(GameSession& game);
    void toggle() { open_=!open_; }
    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] bool paused() const { return open_&&paused_; }
    void draw(std::uint64_t ticks);
private:
    void drawMap();
    void drawEntities();
    void drawLighting();
    void drawCamera();
    void drawInspect();
    void drawBattle();
    void drawDatabase();
    void drawGrid();
    void checkpoint();
    void restore(const Json& map);
    void save();
    GameSession& game_;
    bool open_=false,paused_=true,showGrid_=true,painting_=false,dirty_=false,showDemo_=false;
    int brush_=0,region_=0,ramp_=0,entity_=0,troop_=0;
    bool walkable_=true;
    float height_=0;
    std::uint32_t flags_=0;
    std::optional<GridHit> hover_;
    std::string mapId_,status_;
    std::vector<Json> undo_,redo_;
    char console_[4096]{};
    char newFlag_[128]{},newVariable_[128]{};
    char newSpawn_[128]{};
};
}
