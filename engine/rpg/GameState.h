#pragma once
#include "engine/rpg/Database.h"
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace fe {
struct ActorState {
    std::string id, job;
    int level = 1, exp = 0, hp = 1, mp = 0;
    std::map<std::string,std::string> equipment;
    std::vector<std::string> skills;
    std::map<std::string,int> states;
};
class GameState {
public:
    std::vector<ActorState> actors;
    std::vector<std::string> party;
    std::map<std::string,int> inventory;
    std::map<std::string,bool> flags;
    std::map<std::string,double> variables;
    int gold = 0;
    std::string map, spawn;
    glm::vec3 position{0};
    double playtime = 0;
    void addActor(const Database& db, const std::string& id);
    void setParty(const std::vector<std::string>& ids);
    void changeJob(const Database& db,const std::string& actorId,const std::string& jobId);
    ActorState& actor(const std::string& id);
    [[nodiscard]] const ActorState& actor(const std::string& id) const;
    [[nodiscard]] Stats stats(const Database& db, const ActorState& actor) const;
    [[nodiscard]] bool flag(const std::string& id) const;
    [[nodiscard]] double variable(const std::string& id) const;
    [[nodiscard]] int itemCount(const std::string& id) const;
    bool addItem(const Database& db, const std::string& id, int count);
    bool equip(const Database& db, const std::string& actorId, const std::string& slot, const std::string& itemId);
    bool buy(const Database& db, const std::string& shopId, const std::string& itemId, int count = 1);
    bool sell(const Database& db, const std::string& shopId, const std::string& itemId, int count = 1);
    bool useItem(const Database& db, const std::string& itemId, const std::string& targetId);
    void gainExperience(const Database& db, int amount);
    void rest(const Database& db);
    [[nodiscard]] Json toJson() const;
    static GameState fromJson(const Database& db, const Json& data);
    void validate(const Database& db) const;
};
class SaveSystem {
public:
    explicit SaveSystem(std::filesystem::path directory) : directory_(std::move(directory)) {}
    void save(int slot, const GameState& state) const;
    [[nodiscard]] GameState load(int slot, const Database& db) const;
    [[nodiscard]] bool exists(int slot) const;
private:
    [[nodiscard]] std::filesystem::path path(int slot) const;
    std::filesystem::path directory_;
};
}
