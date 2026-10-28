#include "engine/rpg/Database.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace fe {
Database::Database() {for (int level=1;level<=99;++level) experienceCurve.push_back((level-1)*(level-1)*50);}
EffectDef EffectDef::read(const Json& e) {
    EffectDef out;out.type=e.value("type",out.type);out.formula=e.value("formula",out.formula);out.power=e.value("power",0);
    out.state=e.value("state","");out.element=e.value("element","");out.chance=e.value("chance",1.0);out.script=e.value("script","");out.recipient=e.value("recipient","target");
    const std::set<std::string> types{"damage","heal","restore_mp","state","remove_state","revive","script"};
    if (!types.contains(out.type)) throw std::runtime_error("Unknown effect type: "+out.type);
    if (!std::isfinite(out.chance) || out.chance<0 || out.chance>1 || (out.recipient!="target" && out.recipient!="user")) throw std::runtime_error("Invalid effect chance/recipient");
    return out;
}
Stats& Stats::operator+=(const Stats& o) {
    hp += o.hp; mp += o.mp; attack += o.attack; defense += o.defense;
    magic += o.magic; resistance += o.resistance; speed += o.speed;
    return *this;
}
Stats Stats::read(const Json& j, bool bonuses) {
    Stats s;
    if (bonuses) s = {0,0,0,0,0,0,0};
    s.hp = j.value("hp", s.hp); s.mp = j.value("mp", s.mp);
    s.attack = j.value("attack", s.attack); s.defense = j.value("defense", s.defense);
    s.magic = j.value("magic", s.magic); s.resistance = j.value("resistance", s.resistance);
    s.speed = j.value("speed", s.speed);
    return s;
}
Json Stats::json() const { return {{"hp",hp},{"mp",mp},{"attack",attack},{"defense",defense},
    {"magic",magic},{"resistance",resistance},{"speed",speed}}; }
namespace {
template <class T, class F> void readTable(const Json& source, const char* name, std::map<std::string,T>& table, F parse) {
    if (!source.contains(name)) return;
    const auto& entries = source.at(name);
    if (!entries.is_array()) throw std::runtime_error(std::string(name) + " must be an array");
    for (const auto& j : entries) {
        const auto id = j.at("id").get<std::string>();
        if (id.empty() || table.contains(id)) throw std::runtime_error(std::string(name) + ": duplicate/empty id " + id);
        T value = parse(j); value.id = id;
        table.emplace(id, std::move(value));
    }
}
std::vector<EffectDef> effects(const Json& j) {
    std::vector<EffectDef> result;
    for (const auto& e : j.value("effects", Json::array())) {
        result.push_back(EffectDef::read(e));
    }
    return result;
}
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void validateStats(const Stats& s, const std::string& owner) {
    require(s.hp > 0 && s.hp <= 9999999 && s.mp >= 0 && s.mp <= 9999999 &&
        s.attack >= 0 && s.defense >= 0 && s.magic >= 0 && s.resistance >= 0 && s.speed > 0,
        owner + ": invalid stats");
}
}
Database Database::load(const std::filesystem::path& path) {
    try { return fromJson(readJson(path)); }
    catch (const std::exception& e) { throw std::runtime_error(path.string() + ": " + e.what()); }
}
Database Database::fromJson(const Json& j) {
    Database d; d.source_ = j;
    if (j.contains("experience_curve")) d.experienceCurve=j.at("experience_curve").get<std::vector<int>>();
    readTable(j, "actors", d.actors, [](const Json& a) {
        ActorDef out; out.name = a.at("name"); out.job = a.value("job", ""); out.sprite = a.value("sprite", "");
        out.stats = Stats::read(a.value("stats", Json::object())); out.level = a.value("level", 1);
        out.skills = a.value("skills", std::vector<std::string>{});out.elements=a.value("elements",std::map<std::string,double>{});return out;
    });
    readTable(j, "jobs", d.jobs, [](const Json& a) {
        JobDef out; out.name = a.at("name"); out.growth = Stats::read(a.value("growth", Json::object()), true);
        out.curves=a.value("curves",std::map<std::string,std::vector<int>>{});
        for (const auto& l : a.value("learn", Json::array())) out.learn[l.at("level").get<int>()] = l.at("skills").get<std::vector<std::string>>();
        return out;
    });
    readTable(j, "skills", d.skills, [](const Json& a) {
        SkillDef out; out.name = a.at("name"); out.target = a.value("target", "enemy");
        out.mpCost = a.value("mp_cost", 0); out.timeline = a.value("timeline", "attack"); out.effects = effects(a); return out;
    });
    readTable(j, "items", d.items, [](const Json& a) {
        ItemDef out; out.name = a.at("name"); out.type = a.value("type", "consumable"); out.slot = a.value("slot", "");
        out.price = a.value("price", 0); out.bonus = Stats::read(a.value("bonus", Json::object()), true); out.effects = effects(a); return out;
    });
    readTable(j,"elements",d.elements,[](const Json& a){ElementDef out;out.name=a.value("name",a.at("id").get<std::string>());return out;});
    readTable(j, "states", d.states, [](const Json& a) {
        StateDef out; out.name = a.at("name"); out.duration = a.value("duration", 3);
        out.damageFraction = a.value("damage_fraction", 0.0); out.speedMultiplier = a.value("speed_multiplier", 1.0);
        out.preventAction = a.value("prevent_action", false); return out;
    });
    readTable(j, "enemies", d.enemies, [](const Json& a) {
        EnemyDef out; out.name = a.at("name"); out.sprite = a.value("sprite", ""); out.ai = a.value("ai", "default");
        out.stats = Stats::read(a.value("stats", Json::object())); out.exp = a.value("exp", 10); out.gold = a.value("gold", 5);
        out.level=a.value("level",1);
        out.skills = a.value("skills", std::vector<std::string>{}); out.elements = a.value("elements", std::map<std::string,double>{});
        for (const auto& drop : a.value("drops", Json::array())) out.drops.emplace_back(drop.at("item"), drop.value("chance", 1.0));
        return out;
    });
    readTable(j, "troops", d.troops, [](const Json& a) {
        TroopDef out; out.enemies = a.at("enemies").get<std::vector<std::string>>();
        out.stage = a.value("stage", "grass"); out.escapable = a.value("escapable", true); return out;
    });
    readTable(j, "shops", d.shops, [](const Json& a) {
        ShopDef out; out.items = a.at("items").get<std::vector<std::string>>();
        out.buyRate = a.value("buy_rate", 1.0); out.sellRate = a.value("sell_rate", 0.5); return out;
    });
    readTable(j, "encounters", d.encounters, [](const Json& a) {
        EncounterDef out; out.distance = a.value("distance", 10.0);
        for (const auto& t : a.at("troops")) out.troops.emplace_back(t.at("troop"), t.value("weight", 1)); return out;
    });
    d.validate(); return d;
}
void Database::validate() const {
    require(experienceCurve.size()>=2 && experienceCurve.size()<=99 && experienceCurve.front()==0,"Experience curve needs 2..99 levels starting at zero");
    for (std::size_t i=1;i<experienceCurve.size();++i) require(experienceCurve[i]>experienceCurve[i-1] && experienceCurve[i]<=999999999,"Experience thresholds must increase");
    auto validateEffects = [&](const auto& list, const std::string& owner) {
        for (const auto& e : list) {
            require(e.type == "damage" || e.type == "heal" || e.type == "restore_mp" || e.type == "state" ||
                e.type == "remove_state" || e.type == "revive" || e.type=="script", owner + ": unknown effect " + e.type);
            require(std::isfinite(e.chance) && e.chance >= 0 && e.chance <= 1, owner + ": invalid effect chance");
            if (e.type == "state" || e.type == "remove_state") require(states.contains(e.state), owner + ": missing state " + e.state);
            if (e.type=="script") require(!e.script.empty(),owner+": script effect needs a function name");
            if (!e.element.empty() && !elements.empty()) require(elements.contains(e.element),owner+": missing element "+e.element);
        }
    };
    auto validateSkills = [&](const auto& ids, const std::string& owner) {
        for (const auto& id : ids) require(skills.contains(id), owner + ": missing skill " + id);
    };
    auto validateElements=[&](const auto& multipliers,const std::string& id) {
        for (const auto& [element,multiplier]:multipliers) require(!element.empty() && (elements.empty() || elements.contains(element)) && std::isfinite(multiplier) && multiplier>=0 && multiplier<=100,id+": invalid element multiplier "+element);
    };
    for (const auto& [id,a] : actors) {
        validateStats(a.stats, "actor " + id); require(a.level >= 1 && a.level <= static_cast<int>(experienceCurve.size()), id + ": invalid level");
        require(a.job.empty() || jobs.contains(a.job), id + ": missing job " + a.job); validateSkills(a.skills, id);
        validateElements(a.elements,id);
    }
    const std::set<std::string> statNames{"hp","mp","attack","defense","magic","resistance","speed"};
    for (const auto& [id,j] : jobs) {
      for (const auto& [level,ids] : j.learn) {
        require(level >= 1 && level <= static_cast<int>(experienceCurve.size()), id + ": invalid learning level"); validateSkills(ids, id);
      }
      for (const auto& [stat,curve]:j.curves) {
        require(statNames.contains(stat) && !curve.empty() && curve.size()<=99,id+": invalid stat curve "+stat);
        for (int value:curve) require(value>=-1000000 && value<=1000000,id+": stat curve out of range");
      }
    }
    const std::set<std::string> targets{"enemy","ally","all_enemies","all_allies","self","dead_ally"};
    for (const auto& [id,s] : skills) {
        require(s.mpCost >= 0 && targets.contains(s.target), "skill " + id + ": invalid cost/target");
        validateEffects(s.effects, "skill " + id);
    }
    for (const auto& [id,i] : items) {
        require(i.price >= 0 && (i.type == "consumable" || i.type == "equipment" || i.type == "key"), id + ": invalid item");
        require(i.type != "equipment" || !i.slot.empty(), id + ": equipment needs a slot"); validateEffects(i.effects, id);
    }
    for (const auto& [id,s] : states) require(s.duration > 0 && std::isfinite(s.damageFraction) &&
        s.damageFraction >= 0 && s.damageFraction <= 1 && std::isfinite(s.speedMultiplier) && s.speedMultiplier >= 0,
        id + ": invalid state");
    for (const auto& [id,e] : enemies) {
        validateStats(e.stats, id); validateSkills(e.skills, id); require(e.exp >= 0 && e.gold >= 0, id + ": invalid rewards");
        require(e.level>=1 && e.level<=99,id+": invalid enemy level");
        for (const auto& [item,chance] : e.drops) require(items.contains(item) && std::isfinite(chance) && chance >= 0 && chance <= 1, id + ": invalid drop");
        validateElements(e.elements,id);
    }
    for (const auto& [id,t] : troops) {
        require(!t.enemies.empty() && t.enemies.size() <= 16, id + ": troop size must be 1..16");
        for (const auto& enemy : t.enemies) require(enemies.contains(enemy), id + ": missing enemy " + enemy);
    }
    for (const auto& [id,s] : shops) {
        require(std::isfinite(s.buyRate) && std::isfinite(s.sellRate) && s.buyRate > 0 && s.sellRate >= 0 && s.sellRate <= s.buyRate, id + ": invalid shop rates");
        for (const auto& item : s.items) require(items.contains(item), id + ": missing item " + item);
    }
    for (const auto& [id,e] : encounters) {
        require(std::isfinite(e.distance) && e.distance > 0 && !e.troops.empty(), id + ": invalid encounter table");
        for (const auto& [troop,weight] : e.troops) require(troops.contains(troop) && weight > 0, id + ": invalid encounter troop");
    }
}
}
