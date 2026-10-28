#pragma once
#include "engine/core/Json.h"
#include <map>
#include <string>
#include <vector>

namespace fe {
struct Stats {
    int hp = 100, mp = 20, attack = 10, defense = 5, magic = 10, resistance = 5, speed = 10;
    Stats& operator+=(const Stats& other);
    static Stats read(const Json& data, bool bonuses = false);
    [[nodiscard]] Json json() const;
};
struct EffectDef {
    std::string type = "damage";
    std::string formula = "physical";
    int power = 0;
    std::string state, element;
    double chance = 1;
    std::string script,recipient="target";
    static EffectDef read(const Json& data);
};
struct SkillDef {
    std::string id, name, target = "enemy", timeline = "attack";
    int mpCost = 0;
    std::vector<EffectDef> effects;
};
struct ItemDef {
    std::string id, name, type = "consumable", slot;
    int price = 0;
    Stats bonus = Stats::read(Json::object(), true);
    std::vector<EffectDef> effects;
};
struct JobDef {
    std::string id, name;
    Stats growth = Stats::read(Json::object(), true);
    std::map<int, std::vector<std::string>> learn;
    std::map<std::string,std::vector<int>> curves;
};
struct ActorDef {
    std::string id, name, job, sprite;
    Stats stats;
    int level = 1;
    std::vector<std::string> skills;
    std::map<std::string,double> elements;
};
struct ElementDef {std::string id,name;};
struct StateDef {
    std::string id, name;
    int duration = 3;
    double damageFraction = 0;
    double speedMultiplier = 1;
    bool preventAction = false;
};
struct EnemyDef {
    std::string id, name, sprite, ai = "default";
    Stats stats;
    int exp = 10, gold = 5;
    int level=1;
    std::vector<std::string> skills;
    std::map<std::string, double> elements;
    std::vector<std::pair<std::string, double>> drops;
};
struct TroopDef { std::string id, stage = "grass"; std::vector<std::string> enemies; bool escapable = true; };
struct ShopDef { std::string id; std::vector<std::string> items; double buyRate = 1, sellRate = 0.5; };
struct EncounterDef { std::string id; double distance = 10; std::vector<std::pair<std::string, int>> troops; };

class Database {
public:
    Database();
    std::vector<int> experienceCurve;
    std::map<std::string, ActorDef> actors;
    std::map<std::string, JobDef> jobs;
    std::map<std::string, SkillDef> skills;
    std::map<std::string, ItemDef> items;
    std::map<std::string, StateDef> states;
    std::map<std::string, ElementDef> elements;
    std::map<std::string, EnemyDef> enemies;
    std::map<std::string, TroopDef> troops;
    std::map<std::string, ShopDef> shops;
    std::map<std::string, EncounterDef> encounters;
    static Database load(const std::filesystem::path& path);
    static Database fromJson(const Json& data);
    void validate() const;
    [[nodiscard]] const Json& source() const { return source_; }
private:
    Json source_;
};
}
