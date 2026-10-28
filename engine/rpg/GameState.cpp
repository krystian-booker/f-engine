#include "engine/rpg/GameState.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace fe {
void GameState::addActor(const Database& db, const std::string& id) {
    for (const auto& a : actors) if (a.id == id) return;
    const auto& def = db.actors.at(id);
    ActorState a; a.id = id; a.job = def.job; a.level = def.level; a.skills = def.skills;a.exp=db.experienceCurve.at(static_cast<std::size_t>(a.level-1));
    if (!a.job.empty()) for (const auto& [level,skills] : db.jobs.at(a.job).learn)
        if (level <= a.level) for (const auto& skill : skills)
            if (std::find(a.skills.begin(), a.skills.end(), skill) == a.skills.end()) a.skills.push_back(skill);
    const auto attributes = stats(db, a); a.hp = attributes.hp; a.mp = attributes.mp;
    actors.push_back(std::move(a));
    if (party.size() < 4) party.push_back(id);
}
void GameState::setParty(const std::vector<std::string>& ids) {
    if (ids.empty() || ids.size()>4) throw std::runtime_error("Party needs 1..4 members");
    std::set<std::string> seen;
    for (const auto& id:ids) {actor(id);if (!seen.insert(id).second) throw std::runtime_error("Duplicate party member: "+id);}
    party=ids;
}
void GameState::changeJob(const Database& db,const std::string& actorId,const std::string& jobId) {
    const auto& job=db.jobs.at(jobId);auto& a=actor(actorId);a.job=jobId;
    for (const auto& [level,skills]:job.learn) if (level<=a.level) for (const auto& skill:skills)
        if (std::find(a.skills.begin(),a.skills.end(),skill)==a.skills.end()) a.skills.push_back(skill);
    const auto attributes=stats(db,a);a.hp=std::min(a.hp,attributes.hp);a.mp=std::min(a.mp,attributes.mp);
}
ActorState& GameState::actor(const std::string& id) {
    for (auto& a : actors) if (a.id == id) return a;
    throw std::runtime_error("Actor not recruited: " + id);
}
const ActorState& GameState::actor(const std::string& id) const {
    for (const auto& a : actors) if (a.id == id) return a;
    throw std::runtime_error("Actor not recruited: " + id);
}
Stats GameState::stats(const Database& db, const ActorState& a) const {
    auto s = db.actors.at(a.id).stats;
    if (!a.job.empty()) {
        const auto& growth = db.jobs.at(a.job).growth;
        for (int i = 1; i < a.level; ++i) s += growth;
        const std::map<std::string,int Stats::*> fields{{"hp",&Stats::hp},{"mp",&Stats::mp},{"attack",&Stats::attack},{"defense",&Stats::defense},{"magic",&Stats::magic},{"resistance",&Stats::resistance},{"speed",&Stats::speed}};
        for (const auto& [name,curve]:db.jobs.at(a.job).curves) {
            const auto member=fields.at(name);s.*member=db.actors.at(a.id).stats.*member+curve[std::min<std::size_t>(a.level-1,curve.size()-1)];
        }
    }
    for (const auto& [slot,id] : a.equipment) { (void)slot; if (!id.empty()) s += db.items.at(id).bonus; }
    s.hp = std::clamp(s.hp, 1, 9999999); s.mp = std::clamp(s.mp, 0, 9999999);
    s.attack = std::max(0, s.attack); s.defense = std::max(0, s.defense);
    s.magic = std::max(0, s.magic); s.resistance = std::max(0, s.resistance); s.speed = std::max(1, s.speed);
    return s;
}
bool GameState::flag(const std::string& id) const { auto i = flags.find(id); return i != flags.end() && i->second; }
double GameState::variable(const std::string& id) const { auto i = variables.find(id); return i == variables.end() ? 0 : i->second; }
int GameState::itemCount(const std::string& id) const { auto i = inventory.find(id); return i == inventory.end() ? 0 : i->second; }
bool GameState::addItem(const Database& db, const std::string& id, int count) {
    if (!db.items.contains(id)) return false;
    const auto next = static_cast<std::int64_t>(itemCount(id)) + count;
    if (next < 0 || next > 999) return false;
    if (next == 0) inventory.erase(id); else inventory[id] = static_cast<int>(next);
    return true;
}
bool GameState::equip(const Database& db, const std::string& actorId, const std::string& slot, const std::string& itemId) {
    auto& a = actor(actorId);
    if (!itemId.empty() && (!db.items.contains(itemId) || itemCount(itemId) < 1 ||
        db.items.at(itemId).type != "equipment" || db.items.at(itemId).slot != slot)) return false;
    auto old = a.equipment.find(slot);
    const std::string oldItem = old == a.equipment.end() ? "" : old->second;
    if (oldItem == itemId) return true;
    if (!oldItem.empty() && itemCount(oldItem) >= 999) return false;
    if (!itemId.empty()) addItem(db, itemId, -1);
    if (!oldItem.empty()) addItem(db, oldItem, 1);
    if (itemId.empty()) a.equipment.erase(slot); else a.equipment[slot] = itemId;
    const auto s = stats(db, a); a.hp = std::min(a.hp, s.hp); a.mp = std::min(a.mp, s.mp);
    return true;
}
bool GameState::buy(const Database& db, const std::string& shopId, const std::string& itemId, int count) {
    const auto& shop = db.shops.at(shopId);
    if (count < 1 || count > 999 || !db.items.contains(itemId) ||
        std::find(shop.items.begin(), shop.items.end(), itemId) == shop.items.end()) return false;
    const auto price = static_cast<std::int64_t>(std::ceil(db.items.at(itemId).price * shop.buyRate)) * count;
    if (price > gold || !addItem(db, itemId, count)) return false;
    gold -= static_cast<int>(price); return true;
}
bool GameState::sell(const Database& db, const std::string& shopId, const std::string& itemId, int count) {
    const auto& shop = db.shops.at(shopId);
    if (count < 1 || count > 999 || !db.items.contains(itemId) || db.items.at(itemId).type == "key") return false;
    const auto price = static_cast<std::int64_t>(std::floor(db.items.at(itemId).price * shop.sellRate)) * count;
    if (price > std::numeric_limits<int>::max() - gold || !addItem(db, itemId, -count)) return false;
    gold += static_cast<int>(price); return true;
}
bool GameState::useItem(const Database& db, const std::string& itemId, const std::string& targetId) {
    const auto found = db.items.find(itemId);
    if (found == db.items.end() || found->second.type != "consumable" || itemCount(itemId) < 1) return false;
    auto& a = actor(targetId); const auto s = stats(db,a); bool changed = false;
    for (const auto& e : found->second.effects) {
        if (e.type == "heal" && a.hp > 0) { const int next = std::clamp(a.hp + e.power, 0, s.hp); changed |= next != a.hp; a.hp = next; }
        if (e.type == "restore_mp" && a.hp > 0) { const int next = std::clamp(a.mp + e.power, 0, s.mp); changed |= next != a.mp; a.mp = next; }
        if (e.type == "revive" && a.hp == 0) { a.hp = std::clamp(e.power, 1, s.hp); changed = true; }
        if (e.type == "remove_state") changed |= a.states.erase(e.state) != 0;
    }
    if (changed) addItem(db, itemId, -1);
    return changed;
}
void GameState::gainExperience(const Database& db, int amount) {
    if (amount < 0) throw std::invalid_argument("Negative experience");
    for (const auto& id : party) {
        auto& a = actor(id);
        if (a.hp <= 0) continue;
        a.exp = static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(a.exp) + amount, 999999999));
        const auto before = stats(db,a);
        while (a.level < static_cast<int>(db.experienceCurve.size()) && a.exp >= db.experienceCurve[a.level]) ++a.level;
        if (!a.job.empty()) for (const auto& [level,skills] : db.jobs.at(a.job).learn) if (level <= a.level)
            for (const auto& skill : skills) if (std::find(a.skills.begin(), a.skills.end(), skill) == a.skills.end()) a.skills.push_back(skill);
        const auto after = stats(db,a); a.hp += after.hp - before.hp; a.mp += after.mp - before.mp;
    }
}
void GameState::rest(const Database& db) { for (auto& a : actors) { const auto s = stats(db,a); a.hp = s.hp; a.mp = s.mp; a.states.clear(); } }
Json GameState::toJson() const {
    Json list = Json::array();
    for (const auto& a : actors) list.push_back({{"id",a.id},{"job",a.job},{"level",a.level},{"exp",a.exp},
        {"hp",a.hp},{"mp",a.mp},{"equipment",a.equipment},{"skills",a.skills},{"states",a.states}});
    return {{"version",2},{"actors",list},{"party",party},{"inventory",inventory},{"gold",gold},
        {"flags",flags},{"variables",variables},{"map",map},{"spawn",spawn},
        {"position",{position.x,position.y,position.z}},{"playtime",playtime}};
}
GameState GameState::fromJson(const Database& db, const Json& original) {
    auto j = original;
    const int version = j.at("version");
    if (version == 1) { j["version"] = 2; j["variables"] = j.value("vars", Json::object()); }
    else if (version != 2) throw std::runtime_error("Unsupported save version " + std::to_string(version));
    GameState out;
    for (const auto& v : j.at("actors")) {
        ActorState a; a.id = v.at("id"); a.job = v.value("job", db.actors.at(a.id).job);
        a.level = v.at("level"); a.exp = v.at("exp"); a.hp = v.at("hp"); a.mp = v.at("mp");
        a.equipment = v.value("equipment", std::map<std::string,std::string>{});
        a.skills = v.value("skills", std::vector<std::string>{}); a.states = v.value("states", std::map<std::string,int>{});
        out.actors.push_back(std::move(a));
    }
    out.party = j.at("party").get<std::vector<std::string>>(); out.inventory = j.at("inventory").get<std::map<std::string,int>>();
    out.gold = j.at("gold"); out.flags = j.at("flags").get<std::map<std::string,bool>>();
    out.variables = j.value("variables", std::map<std::string,double>{}); out.map = j.at("map"); out.spawn = j.value("spawn", "entry");
    const auto& p = j.at("position");
    if (!p.is_array() || p.size() != 3) throw std::runtime_error("Save position needs three coordinates");
    out.position = {p[0].get<float>(),p[1].get<float>(),p[2].get<float>()}; out.playtime = j.at("playtime");
    out.validate(db); return out;
}
void GameState::validate(const Database& db) const {
    if (gold < 0 || party.empty() || party.size() > 4 || map.empty() || !std::isfinite(playtime) || playtime < 0 ||
        !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) throw std::runtime_error("Invalid save state");
    std::set<std::string> seen;
    for (const auto& a : actors) {
        if (!db.actors.contains(a.id) || !seen.insert(a.id).second || a.level < 1 || a.level > static_cast<int>(db.experienceCurve.size()) || a.exp < 0 ||
            (!a.job.empty() && !db.jobs.contains(a.job))) throw std::runtime_error("Invalid saved actor " + a.id);
        for (const auto& [slot,item] : a.equipment) if (!db.items.contains(item) || db.items.at(item).type != "equipment" || db.items.at(item).slot != slot)
            throw std::runtime_error("Invalid saved equipment " + item);
        const auto s = stats(db,a);
        if (a.hp < 0 || a.hp > s.hp || a.mp < 0 || a.mp > s.mp) throw std::runtime_error("Invalid saved HP/MP");
        for (const auto& skill : a.skills) if (!db.skills.contains(skill)) throw std::runtime_error("Unknown saved skill " + skill);
        for (const auto& [state,duration] : a.states) if (!db.states.contains(state) || duration < 1) throw std::runtime_error("Invalid saved state");
    }
    std::set<std::string> members;
    for (const auto& id : party) if (!seen.contains(id) || !members.insert(id).second) throw std::runtime_error("Invalid saved party");
    for (const auto& [id,count] : inventory) if (!db.items.contains(id) || count < 1 || count > 999) throw std::runtime_error("Invalid saved inventory");
    for (const auto& [id,value] : variables) if (id.empty() || !std::isfinite(value)) throw std::runtime_error("Invalid saved variable");
}
std::filesystem::path SaveSystem::path(int slot) const {
    if (slot < 1 || slot > 99) throw std::invalid_argument("Save slot must be 1..99");
    return directory_ / ("slot_" + std::to_string(slot) + ".json");
}
void SaveSystem::save(int slot, const GameState& state) const { writeJson(path(slot),state.toJson()); }
GameState SaveSystem::load(int slot, const Database& db) const { return GameState::fromJson(db,readJson(path(slot))); }
bool SaveSystem::exists(int slot) const { return std::filesystem::is_regular_file(path(slot)); }
}
