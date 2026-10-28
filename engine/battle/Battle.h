#pragma once
#include "engine/rpg/GameState.h"
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace fe {
enum class BattleOutcome { Ongoing, Victory, Defeat, Escaped };
enum class AtbMode { Active, Wait };
enum class ActionKind { Skill, Item, Guard, Escape };
struct Battler {
    std::string id, name, sprite, source;
    bool enemy = false, guarding = false, queued = false;
    Stats stats;
    int hp = 1, mp = 0;
    int level=1;
    double gauge = 0;
    std::vector<std::string> skills;
    std::map<std::string,int> states;
    std::map<std::string,double> elements;
    [[nodiscard]] bool alive() const { return hp > 0; }
    [[nodiscard]] bool ready() const { return alive() && gauge >= 1 && !queued; }
};
struct BattleAction {
    int user = -1;
    ActionKind kind = ActionKind::Skill;
    std::string id;
    int target = -1;
};
enum class BattleEventKind { Action, Damage, Heal, State, Miss, Guard, Escape, Finished };
struct BattleEvent {
    BattleEventKind kind;
    int user = -1, target = -1, amount = 0;
    std::string text;
    std::string actionId;
};
struct BattleRewards { int experience = 0, gold = 0; std::vector<std::string> drops; };

class Battle {
public:
    using Formula = std::function<double(const std::string&, const Battler&, const Battler&, int)>;
    using Ai = std::function<BattleAction(const Battle&, int)>;
    using ScriptEffect=std::function<std::vector<EffectDef>(const std::string&,const Battler&,const Battler&,int)>;
    Battle(const Database& db, GameState& state, const std::string& troopId, std::uint32_t seed = 1);
    void update(double dt, bool choosingCommand = false);
    bool submit(const BattleAction& action);
    [[nodiscard]] std::vector<int> targets(int user, const std::string& targetType) const;
    [[nodiscard]] const std::vector<Battler>& battlers() const { return battlers_; }
    [[nodiscard]] BattleOutcome outcome() const { return outcome_; }
    [[nodiscard]] const BattleRewards& rewards() const { return rewards_; }
    [[nodiscard]] const Database& database() const { return db_; }
    [[nodiscard]] std::vector<BattleEvent> takeEvents();
    void setFormula(Formula formula) { formula_ = std::move(formula); }
    void setAi(Ai ai) { ai_ = std::move(ai); }
    void setScriptEffect(ScriptEffect effect) {scriptEffect_=std::move(effect);}
    AtbMode mode = AtbMode::Wait;
private:
    bool valid(const BattleAction& action, bool requireReady) const;
    bool canAct(const Battler& battler) const;
    void execute(const BattleAction& action);
    void apply(const EffectDef& effect, int user, int target,int depth=0);
    void tickStates(int index);
    void checkOutcome();
    void finish(BattleOutcome outcome);
    BattleAction defaultAi(int index);
    double random();
    const Database& db_;
    GameState& state_;
    TroopDef troop_;
    std::vector<Battler> battlers_;
    std::deque<BattleAction> queue_;
    std::vector<BattleEvent> events_;
    BattleOutcome outcome_ = BattleOutcome::Ongoing;
    BattleRewards rewards_;
    std::mt19937 random_;
    Formula formula_;
    Ai ai_;
    ScriptEffect scriptEffect_;
};
}
