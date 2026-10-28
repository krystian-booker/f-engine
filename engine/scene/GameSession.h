#pragma once
#include "engine/core/GameConfig.h"
#include <filesystem>
#include <memory>
#include <glm/glm.hpp>
union SDL_Event;

namespace fe {
class World;
class GameState;
class Database;
class ScriptEngine;
class WorldRenderer;
class GameSession {
public:
    GameSession(std::filesystem::path gameRoot,std::filesystem::path userData,
        std::filesystem::path shaderRoot,GameConfig config,bool mute=false,bool skipTitle=false);
    ~GameSession();
    GameSession(const GameSession&)=delete;
    GameSession& operator=(const GameSession&)=delete;
    void processEvent(const SDL_Event& event);
    void update(double dt,bool editorPaused=false);
    void render(double alpha,int width,int height);
    void reload(const std::filesystem::path& file);
    void startBattle(const std::string& troop);
    void reloadMap();
    void rebuildEntities();
    void playFrom(glm::vec3 position);
    [[nodiscard]] World& world();
    [[nodiscard]] GameState& state();
    [[nodiscard]] Database& database();
    [[nodiscard]] ScriptEngine& scripts();
    [[nodiscard]] WorldRenderer& renderer();
    [[nodiscard]] const std::filesystem::path& gameRoot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
