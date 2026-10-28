#include "engine/battle/Battle.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fe {
Battle::Battle(const Database& db, GameState& state, const std::string& troopId, std::uint32_t seed)
    : db_(db), state_(state), troop_(db.troops.at(troopId)), random_(seed) {
    for (const auto& id : state.party) {
        const auto& a = state.actor(id); const auto& def = db.actors.at(id);
        Battler b; b.id = id; b.name = def.name; b.sprite = def.sprite; b.source = id;
        b.stats = state.stats(db,a); b.hp = a.hp; b.mp = a.mp; b.skills = a.skills; b.states = a.states;
        b.level=a.level;b.elements=def.elements;
        b.gauge = random() * 0.25; battlers_.push_back(std::move(b));
    }
    for (const auto& id : troop_.enemies) {
        const auto& def = db.enemies.at(id);
        Battler b; b.id = id + "_" + std::to_string(battlers_.size()); b.source = id;
        b.name = def.name; b.sprite = def.sprite; b.enemy = true; b.stats = def.stats;
        b.hp = b.stats.hp; b.mp = b.stats.mp; b.skills = def.skills; b.elements = def.elements;
        b.level=def.level;
        b.gauge = random() * 0.25; battlers_.push_back(std::move(b));
    }
    if (state.party.empty()) throw std::runtime_error("Cannot start a battle without a party");
    checkOutcome();
}
double Battle::random() { return static_cast<double>(random_()) / 4294967296.0; }
bool Battle::canAct(const Battler& b) const {
    if (!b.alive()) return false;
    for (const auto& [id,duration] : b.states) { (void)duration; if (db_.states.at(id).preventAction) return false; }
    return true;
}
std::vector<int> Battle::targets(int user, const std::string& type) const {
    std::vector<int> result;
    if (user < 0 || user >= static_cast<int>(battlers_.size())) return result;
    const auto& b = battlers_[user];
    if (type == "self") return b.alive() ? std::vector<int>{user} : result;
    const bool ally = type == "ally" || type == "all_allies" || type == "dead_ally";
    for (int i = 0; i < static_cast<int>(battlers_.size()); ++i) {
        const auto& target = battlers_[i];
        if ((target.enemy == b.enemy) != ally) continue;
        if (type == "dead_ally" ? !target.alive() : target.alive()) result.push_back(i);
    }
    return result;
}
bool Battle::valid(const BattleAction& a, bool requireReady) const {
    if (outcome_ != BattleOutcome::Ongoing || a.user < 0 || a.user >= static_cast<int>(battlers_.size())) return false;
    const auto& b = battlers_[a.user];
    if (!canAct(b) || (requireReady && !b.ready())) return false;
    if (a.kind == ActionKind::Escape) return !b.enemy && troop_.escapable;
    if (a.kind == ActionKind::Guard) return true;
    std::string type;
    if (a.kind == ActionKind::Skill) {
        const auto found = db_.skills.find(a.id);
        if (found == db_.skills.end() || b.mp < found->second.mpCost ||
            std::find(b.skills.begin(),b.skills.end(),a.id) == b.skills.end()) return false;
        type = found->second.target;
    } else {
        const auto found = db_.items.find(a.id);
        if (b.enemy || found == db_.items.end() || found->second.type != "consumable" || state_.itemCount(a.id) < 1) return false;
        type = "ally";
        for (const auto& e : found->second.effects) if (e.type == "revive") type = "dead_ally";
    }
    auto candidates = targets(a.user,type);
    if (type == "all_enemies" || type == "all_allies") return !candidates.empty();
    return std::find(candidates.begin(),candidates.end(),a.target) != candidates.end();
}
bool Battle::submit(const BattleAction& action) {
    if (!valid(action,true)) return false;
    battlers_[action.user].queued = true;
    queue_.push_back(action);
    return true;
}
BattleAction Battle::defaultAi(int index) {
    const auto& b = battlers_[index];
    for (const auto& skill : b.skills) {
        if (b.mp < db_.skills.at(skill).mpCost) continue;
        auto candidates = targets(index,db_.skills.at(skill).target);
        if (!candidates.empty()) return {index,ActionKind::Skill,skill,candidates[static_cast<std::size_t>(random() * candidates.size())]};
    }
    return {index,ActionKind::Guard,"",-1};
}
void Battle::update(double dt, bool choosing) {
    if (outcome_ != BattleOutcome::Ongoing || !std::isfinite(dt) || dt <= 0) return;
    if (!(mode == AtbMode::Wait && choosing)) {
        for (int i = 0; i < static_cast<int>(battlers_.size()); ++i) {
            auto& b = battlers_[i];
            if (!b.alive() || b.queued) continue;
            double speed = b.stats.speed;
            for (const auto& [id,duration] : b.states) { (void)duration; speed *= db_.states.at(id).speedMultiplier; }
            b.gauge = std::min(1.0, b.gauge + std::min(dt,0.25) * (speed + 20) / 100.0);
            if (b.ready() && !canAct(b)) { b.gauge = 0; tickStates(i); }
            else if (b.enemy && b.ready()) {
                auto action = ai_ ? ai_(*this,i) : defaultAi(i);
                action.user = i;
                if (!submit(action)) submit(defaultAi(i));
            }
        }
    }
    if (!queue_.empty()) {
        const auto action = queue_.front(); queue_.pop_front();
        auto& b = battlers_[action.user];
        b.queued = false;
        if (valid(action,false)) { b.gauge = 0; b.guarding = false; execute(action); tickStates(action.user); }
    }
    checkOutcome();
}
void Battle::execute(const BattleAction& a) {
    auto& b = battlers_[a.user];
    if (a.kind == ActionKind::Guard) {
        b.guarding = true; events_.push_back({BattleEventKind::Guard,a.user,a.user,0,"Guard"}); return;
    }
    if (a.kind == ActionKind::Escape) {
        if (random() < 0.65) { events_.push_back({BattleEventKind::Escape,a.user,-1,0,"Escaped"}); finish(BattleOutcome::Escaped); }
        else events_.push_back({BattleEventKind::Miss,a.user,-1,0,"Could not escape"});
        return;
    }
    std::vector<EffectDef> effects;
    std::string targetType = "ally", name;
    if (a.kind == ActionKind::Skill) {
        const auto& skill = db_.skills.at(a.id); b.mp -= skill.mpCost;
        effects = skill.effects; targetType = skill.target; name = skill.name;
    } else {
        const auto& item = db_.items.at(a.id);
        if (!state_.addItem(db_,a.id,-1)) return;
        effects = item.effects; name = item.name;
    }
    events_.push_back({BattleEventKind::Action,a.user,a.target,0,name,a.id});
    std::vector<int> affected;
    if (targetType == "all_enemies" || targetType == "all_allies") affected = targets(a.user,targetType);
    else affected.push_back(a.target);
    for (int target : affected) for (const auto& effect : effects) apply(effect,a.user,target);
}
void Battle::apply(const EffectDef& e, int user, int target,int depth) {
    if (depth>8) throw std::runtime_error("Script effect recursion limit exceeded");
    if (e.recipient=="user") target=user;
    auto& b = battlers_[user]; auto& t = battlers_.at(target);
    if (random() >= e.chance) { events_.push_back({BattleEventKind::Miss,user,target,0,"Miss"}); return; }
    if (e.type=="script") {
        if (!scriptEffect_) throw std::runtime_error("No Lua handler for effect "+e.script);
        for (const auto& effect:scriptEffect_(e.script,b,t,e.power)) apply(effect,user,target,depth+1);
        return;
    }
    if ((e.type=="state" || e.type=="remove_state") && !db_.states.contains(e.state)) throw std::runtime_error("Unknown effect state: "+e.state);
    if (e.type == "state") {
        if (t.alive()) { t.states[e.state] = db_.states.at(e.state).duration; events_.push_back({BattleEventKind::State,user,target,0,db_.states.at(e.state).name}); }
        return;
    }
    if (e.type == "remove_state") { t.states.erase(e.state); return; }
    if (e.type == "revive") {
        if (!t.alive()) { t.hp = std::clamp(e.power,1,t.stats.hp); events_.push_back({BattleEventKind::Heal,user,target,t.hp,"Revive"}); }
        return;
    }
    if (!t.alive()) return;
    double amount = e.power;
    if (e.type == "damage") {
        if (formula_) amount = formula_(e.formula,b,t,e.power);
        else if (e.formula == "physical") amount = b.stats.attack * 2.0 + e.power - t.stats.defense;
        else if (e.formula == "magical") amount = b.stats.magic * 2.0 + e.power - t.stats.resistance;
        else if (e.formula != "flat") throw std::runtime_error("Unknown damage formula: " + e.formula);
        if (!e.element.empty()) { auto i = t.elements.find(e.element); if (i != t.elements.end()) amount *= i->second; }
        if (t.guarding) amount *= 0.5;
        if (!std::isfinite(amount)) throw std::runtime_error("Damage formula returned a non-finite value");
        const int damage = static_cast<int>(std::clamp(std::round(amount),0.0,9999999.0));
        const int actual = std::min(t.hp,damage); t.hp -= actual;
        if (!t.alive()) { t.gauge = 0; t.queued = false; t.states.clear(); }
        events_.push_back({BattleEventKind::Damage,user,target,actual,""});
    } else if (e.type == "heal") {
        const int actual = std::clamp(e.power,0,t.stats.hp - t.hp); t.hp += actual;
        events_.push_back({BattleEventKind::Heal,user,target,actual,""});
    } else if (e.type == "restore_mp") {
        const int actual = std::clamp(e.power,0,t.stats.mp - t.mp); t.mp += actual;
        events_.push_back({BattleEventKind::Heal,user,target,actual,"MP"});
    }
}
void Battle::tickStates(int index) {
    auto& b = battlers_[index];
    for (auto it = b.states.begin(); it != b.states.end();) {
        const auto& def = db_.states.at(it->first);
        const int damage = std::min(b.hp,static_cast<int>(std::ceil(b.stats.hp * def.damageFraction)));
        if (damage > 0) { b.hp -= damage; events_.push_back({BattleEventKind::Damage,index,index,damage,def.name}); }
        if (--it->second <= 0) it = b.states.erase(it); else ++it;
    }
    if (!b.alive()) { b.gauge = 0; b.states.clear(); }
}
void Battle::checkOutcome() {
    if (outcome_ != BattleOutcome::Ongoing) return;
    bool partyAlive = false, enemyAlive = false;
    for (const auto& b : battlers_) if (b.alive()) { if (b.enemy) enemyAlive = true; else partyAlive = true; }
    if (!partyAlive) finish(BattleOutcome::Defeat);
    else if (!enemyAlive) finish(BattleOutcome::Victory);
}
void Battle::finish(BattleOutcome result) {
    if (outcome_ != BattleOutcome::Ongoing) return;
    outcome_ = result;
    for (const auto& b : battlers_) if (!b.enemy) {
        auto& a = state_.actor(b.source); a.hp = b.hp; a.mp = b.mp; a.states = b.states;
    }
    if (result == BattleOutcome::Victory) {
        for (const auto& id : troop_.enemies) {
            const auto& e = db_.enemies.at(id); rewards_.experience += e.exp; rewards_.gold += e.gold;
            for (const auto& [item,chance] : e.drops) if (random() < chance) { rewards_.drops.push_back(item); state_.addItem(db_,item,1); }
        }
        state_.gold += rewards_.gold; state_.gainExperience(db_,rewards_.experience);
    }
    events_.push_back({BattleEventKind::Finished,-1,-1,0,
        result == BattleOutcome::Victory ? "Victory" : result == BattleOutcome::Defeat ? "Defeat" : "Escaped"});
}
std::vector<BattleEvent> Battle::takeEvents() { auto events = std::move(events_); events_.clear(); return events; }
}
