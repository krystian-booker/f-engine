#pragma once
#include "engine/world/Map.h"
#include "engine/script/ScriptEngine.h"
#include <entt/entt.hpp>
#include <optional>
#include <random>
#include <unordered_map>

namespace fe {
struct Transform { glm::vec3 position{0}, previous{0}; glm::vec2 facing{0,1}; };
struct SpriteRenderer {
    std::string animation, clip = "idle"; double elapsed = 0; bool visible = true; double previousElapsed=0;
    float scale=1;glm::vec4 tint{1};bool blend=false,unlit=false,castShadow=true;
};
struct Mover {
    float speed = 2.5f, radius = 0.2f; bool solid = true;
    glm::vec2 destination{0};bool hasDestination=false;
    std::vector<glm::ivec2> path;
};
struct Interactable { std::size_t definition = 0; int page = -1; std::uint64_t task = 0; bool touching = false; };
struct NpcRoutine { std::size_t point = 0; double wait = 0; glm::vec2 target{0};bool moving=false;double moveTime=0; };
struct EntityName { std::string id; };
struct Light { MapLight data; };
struct CameraTarget {};

class World {
public:
    World(MapData map, GameState& state, const Database& db, ScriptEngine& scripts);
    void spawn(const std::string& name);
    void restorePosition(glm::vec3 position);
    void beginTick(double dt);
    std::optional<std::string> update(double dt, glm::vec2 movement, bool interact, bool paused = false);
    [[nodiscard]] entt::entity entity(const std::string& id) const;
    [[nodiscard]] entt::entity player() const { return player_; }
    [[nodiscard]] Transform& transform(const std::string& id);
    [[nodiscard]] glm::vec3 playerPosition() const;
    [[nodiscard]] entt::registry& registry() { return entities_; }
    [[nodiscard]] const entt::registry& registry() const { return entities_; }
    [[nodiscard]] MapData& map() { return map_; }
    [[nodiscard]] const MapData& map() const { return map_; }
    bool moveEntity(const std::string& id, glm::vec2 target, double dt, bool ignoreEntities = false);
    void faceEntity(const std::string& id, glm::vec2 direction);
    void rebuildEntities();
private:
    void updatePages();
    void startEvent(entt::entity entity);
    void interact();
    bool overlapsSolid(entt::entity self, glm::vec3 position) const;
    void move(entt::entity entity, glm::vec2 delta, bool ignoreEntities = false);
    MapData map_;
    GameState& state_;
    const Database& db_;
    ScriptEngine& scripts_;
    entt::registry entities_;
    entt::entity player_ = entt::null;
    std::unordered_map<std::string,entt::entity> names_;
    std::mt19937 random_{1};
    double encounterDistance_ = 0, encounterThreshold_ = 0, damageDistance_ = 0;
    std::string encounterTable_;
    bool prepared_=false;
};
}
