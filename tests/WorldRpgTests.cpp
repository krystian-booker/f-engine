#include <doctest/doctest.h>
#include "engine/battle/Battle.h"
#include "engine/core/AssetRegistry.h"
#include "engine/world/NavGrid.h"
#include <chrono>

namespace {
fe::Database testDatabase() {
    return fe::Database::fromJson(fe::Json::parse(R"({
      "jobs":[{"id":"mage","name":"Mage","growth":{"hp":10,"magic":2},"learn":[{"level":2,"skills":["fire"]}]}],
      "actors":[{"id":"hero","name":"Hero","job":"mage","stats":{"hp":100,"mp":20,"attack":20,"speed":40},"skills":["attack","revive"]}],
      "skills":[
        {"id":"attack","name":"Attack","effects":[{"type":"damage","power":10}]},
        {"id":"fire","name":"Fire","mp_cost":4,"effects":[{"type":"damage","formula":"magical","power":20,"element":"fire"}]},
        {"id":"revive","name":"Revive","target":"dead_ally","mp_cost":5,"effects":[{"type":"revive","power":25}]}
      ],
      "items":[
        {"id":"potion","name":"Potion","price":20,"effects":[{"type":"heal","power":50}]},
        {"id":"sword","name":"Sword","type":"equipment","slot":"weapon","price":50,"bonus":{"attack":7}}
      ],
      "states":[{"id":"poison","name":"Poison","damage_fraction":0.1,"duration":3}],
      "enemies":[{"id":"slime","name":"Slime","stats":{"hp":30,"attack":1,"speed":1},"skills":["attack"],"exp":50,"gold":10,"drops":[{"item":"potion","chance":1}]}],
      "troops":[{"id":"slimes","enemies":["slime"]}],
      "shops":[{"id":"shop","items":["potion","sword"]}],
      "encounters":[{"id":"grass","troops":[{"troop":"slimes"}]}]
    })"));
}
fe::GameState testState(const fe::Database& db) {
    fe::GameState state; state.addActor(db,"hero"); state.map = "town"; state.position = {1.5f,0,1.5f}; return state;
}
}
TEST_CASE("nav grid blocks walls, prevents tunnelling and slides along them") {
    fe::NavGrid grid(8,8);
    for (int z = 0; z < 8; ++z) grid.at(3,z).walkable = false;
    auto result = grid.move({1.5f,0,1.5f},{5,1});
    CHECK(result.x < 2.81f);
    CHECK(result.z == doctest::Approx(2.5f));
    CHECK_FALSE(grid.canStand({-0.1f,2}));
    CHECK(grid.findPath({1,1},{5,5}).empty());
    grid.at(3,4).walkable = true;
    auto path = grid.findPath({1,1},{5,5});
    CHECK_FALSE(path.empty());
    CHECK(path.back() == glm::ivec2(5,5));
    auto loaded = fe::NavGrid::fromJson(grid.toJson());
    CHECK_FALSE(loaded.at(3,3).walkable);
    CHECK(loaded.at(3,4).walkable);
}
TEST_CASE("nav grid permits stairs but blocks cliffs") {
    fe::NavGrid grid(4,3);
    grid.at(1,1).ramp = fe::Ramp::East;
    grid.at(2,1).height = 1;
    auto result = grid.move({0.5f,0,1.5f},{2,0});
    CHECK(result.x == doctest::Approx(2.5f));
    CHECK(result.y == doctest::Approx(1));
    grid.at(1,1).ramp = fe::Ramp::None;
    result = grid.move({0.5f,0,1.5f},{2,0});
    CHECK(result.x < 2);
}
TEST_CASE("asset reload preserves handles and failures preserve the old payload") {
    int value = 1;
    fe::AssetRegistry<int> registry([&](const auto&) {
        if (value == -1) throw std::runtime_error("bad edit");
        return std::make_shared<int>(value);
    });
    auto first = registry.load("map.json");
    CHECK(first == registry.load("./map.json"));
    value = 2; registry.reload(first); CHECK(*registry.get(first) == 2);
    value = -1; CHECK_THROWS(registry.reload(first)); CHECK(*registry.get(first) == 2);
    registry.release(first); CHECK_FALSE(registry.get(first));
    value = 3; auto second = registry.load("other.json");
    CHECK(second.index == first.index); CHECK(second.generation != first.generation);
    CHECK_FALSE(registry.get(first));
}
TEST_CASE("database rejects duplicate IDs and broken references") {
    const auto db = testDatabase();
    auto j = db.source(); j["actors"].push_back(j["actors"][0]);
    CHECK_THROWS(fe::Database::fromJson(j));
    j = db.source(); j["troops"][0]["enemies"] = {"missing"};
    CHECK_THROWS(fe::Database::fromJson(j));
    j = db.source(); j["skills"][0]["target"] = "nonsense";
    CHECK_THROWS(fe::Database::fromJson(j));
}
TEST_CASE("shop and equipment transactions preserve inventory on failure") {
    auto db = testDatabase(); auto state = testState(db);
    CHECK_FALSE(state.buy(db,"shop","potion")); CHECK(state.itemCount("potion") == 0);
    state.gold = 100;
    CHECK(state.buy(db,"shop","sword")); CHECK(state.gold == 50);
    CHECK(state.equip(db,"hero","weapon","sword")); CHECK(state.itemCount("sword") == 0);
    CHECK(state.stats(db,state.actor("hero")).attack == 27);
    CHECK_FALSE(state.equip(db,"hero","armor","sword"));
    CHECK(state.equip(db,"hero","weapon","")); CHECK(state.itemCount("sword") == 1);
    CHECK(state.sell(db,"shop","sword")); CHECK(state.gold == 75);
    CHECK_FALSE(state.sell(db,"shop","sword")); CHECK(state.gold == 75);
    CHECK(state.buy(db,"shop","potion"));
    CHECK_FALSE(state.useItem(db,"potion","hero")); CHECK(state.itemCount("potion") == 1);
    state.actor("hero").hp = 10;
    CHECK(state.useItem(db,"potion","hero")); CHECK(state.actor("hero").hp == 60);
}
TEST_CASE("save round trip, migration and invalid-state rejection") {
    auto db = testDatabase(); auto state = testState(db);
    state.gold = 123; state.flags["quest"] = true; state.variables["count"] = 2;
    state.addItem(db,"potion",3); state.gainExperience(db,50);
    CHECK(state.actor("hero").level == 2);
    CHECK(state.actor("hero").skills.back() == "fire");
    auto folder = std::filesystem::temp_directory_path() / ("fengine-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fe::SaveSystem saves(folder);
    saves.save(1,state); saves.save(1,state);
    CHECK(saves.load(1,db).toJson() == state.toJson());
    CHECK_THROWS(saves.load(0,db));
    auto old = state.toJson(); old["version"] = 1; old["vars"] = old["variables"]; old.erase("variables");
    CHECK(fe::GameState::fromJson(db,old).variable("count") == 2);
    old["actors"][0]["hp"] = -100;
    CHECK_THROWS(fe::GameState::fromJson(db,old));
    std::filesystem::remove_all(folder);
}
TEST_CASE("ATB wait mode pauses gauges, rejects invalid commands and grants rewards once") {
    auto db = testDatabase(); auto state = testState(db);
    fe::Battle battle(db,state,"slimes",42);
    const auto gauge = battle.battlers()[0].gauge;
    battle.update(1.0/60,true); CHECK(battle.battlers()[0].gauge == gauge);
    CHECK_FALSE(battle.submit({0,fe::ActionKind::Skill,"attack",1}));
    for (int tick = 0; tick < 600 && !battle.battlers()[0].ready(); ++tick) battle.update(1.0/60);
    REQUIRE(battle.battlers()[0].ready());
    CHECK_FALSE(battle.submit({0,fe::ActionKind::Skill,"fire",1})); // not learned yet
    CHECK_FALSE(battle.submit({0,fe::ActionKind::Skill,"attack",0})); // invalid side
    CHECK(battle.submit({0,fe::ActionKind::Skill,"attack",1}));
    CHECK_FALSE(battle.submit({0,fe::ActionKind::Skill,"attack",1}));
    battle.update(1.0/60);
    CHECK(battle.outcome() == fe::BattleOutcome::Victory);
    CHECK(state.gold == 10); CHECK(state.itemCount("potion") == 1);
    CHECK(state.actor("hero").level == 2);
    battle.update(10); CHECK(state.gold == 10);
    auto events = battle.takeEvents(); CHECK_FALSE(events.empty()); CHECK(battle.takeEvents().empty());
}
TEST_CASE("battle formula hook is used and non-finite damage is rejected") {
    auto db = testDatabase(); auto state = testState(db);
    fe::Battle battle(db,state,"slimes");
    int calls = 0;
    battle.setFormula([&](const auto&,const auto&,const auto&,int) { ++calls; return 7; });
    for (int tick = 0; tick < 600 && !battle.battlers()[0].ready(); ++tick) battle.update(1.0/60);
    REQUIRE(battle.submit({0,fe::ActionKind::Skill,"attack",1})); battle.update(1.0/60);
    CHECK(calls == 1); CHECK(battle.battlers()[1].hp == 23);
}
TEST_CASE("battle revives only dead allies and spends MP when executing") {
    auto db=testDatabase();auto ally=db.actors.at("hero");ally.id="ally";db.actors["ally"]=ally;
    db.enemies.at("slime").stats.hp=1000;db.enemies.at("slime").skills.clear();
    auto state=testState(db);state.addActor(db,"ally");state.actor("ally").hp=0;
    fe::Battle battle(db,state,"slimes");
    for (int i=0;i<600 && !battle.battlers()[0].ready();++i) battle.update(1.0/60);
    CHECK_FALSE(battle.submit({0,fe::ActionKind::Skill,"revive",0}));
    REQUIRE(battle.submit({0,fe::ActionKind::Skill,"revive",1}));
    CHECK(battle.battlers()[0].mp==20);battle.update(1.0/60);
    CHECK(battle.battlers()[0].mp==15);CHECK(battle.battlers()[1].hp==25);
}
TEST_CASE("guard reduces damage and status durations tick at turns") {
    auto db=testDatabase();auto state=testState(db);
    db.enemies.at("slime").stats.hp=1000;
    fe::Battle battle(db,state,"slimes");battle.setFormula([](const auto&,const auto&,const auto&,int){return 20;});
    for (int i=0;i<600 && !battle.battlers()[0].ready();++i) battle.update(1.0/60);
    REQUIRE(battle.submit({0,fe::ActionKind::Guard,"",-1}));battle.update(1.0/60);
    const auto before=battle.battlers()[0].hp;
    for (int i=0;i<600 && battle.battlers()[0].hp==before;++i) battle.update(1.0/60);
    CHECK(battle.battlers()[0].hp==before-10);
    db.enemies.at("slime").skills.clear();state=testState(db);state.actor("hero").states["poison"]=3;
    fe::Battle poisoned(db,state,"slimes");
    for (int turn=0;turn<3;++turn) {
        for (int i=0;i<600 && !poisoned.battlers()[0].ready();++i) poisoned.update(1.0/60);
        REQUIRE(poisoned.submit({0,fe::ActionKind::Guard,"",-1}));poisoned.update(1.0/60);
    }
    CHECK(poisoned.battlers()[0].hp==70);CHECK(poisoned.battlers()[0].states.empty());
}
TEST_CASE("defeat commits HP and never grants victory rewards") {
    auto db=testDatabase();auto state=testState(db);state.actor("hero").hp=1;
    fe::Battle battle(db,state,"slimes");
    for (int i=0;i<900 && battle.outcome()==fe::BattleOutcome::Ongoing;++i) battle.update(1.0/60);
    CHECK(battle.outcome()==fe::BattleOutcome::Defeat);CHECK(state.actor("hero").hp==0);CHECK(state.gold==0);
}
TEST_CASE("authored experience and stat curves drive levels and party formation") {
    auto db=testDatabase();db.experienceCurve={0,10,30,60};db.jobs.at("mage").curves["hp"]={0,7,50,120};db.validate();
    auto state=testState(db);state.gainExperience(db,10);CHECK(state.actor("hero").level==2);CHECK(state.stats(db,state.actor("hero")).hp==107);
    state.gainExperience(db,20);CHECK(state.actor("hero").level==3);CHECK(state.stats(db,state.actor("hero")).hp==150);CHECK(state.stats(db,state.actor("hero")).magic==14);
    auto ally=db.actors.at("hero");ally.id="ally";db.actors["ally"]=ally;state.addActor(db,"ally");
    state.setParty({"ally","hero"});CHECK(state.party.front()=="ally");
    CHECK_THROWS(state.setParty({"hero","hero"}));CHECK(state.party.front()=="ally");
    CHECK_THROWS(state.setParty({"missing"}));CHECK(state.party.front()=="ally");
}
TEST_CASE("custom skill effects can damage the target and heal their user") {
    auto db=testDatabase();db.enemies.at("slime").stats.hp=1000;db.enemies.at("slime").skills.clear();
    fe::SkillDef siphon;siphon.id="siphon";siphon.name="Siphon";siphon.effects={fe::EffectDef::read({{"type","script"},{"script","drain"}})};
    db.skills["siphon"]=siphon;db.actors.at("hero").skills.push_back("siphon");
    auto state=testState(db);state.actor("hero").hp=30;fe::Battle battle(db,state,"slimes");
    battle.setScriptEffect([](const auto&,const auto&,const auto&,int) {return std::vector<fe::EffectDef>{
        fe::EffectDef::read({{"type","damage"},{"formula","flat"},{"power",20}}),fe::EffectDef::read({{"type","heal"},{"recipient","user"},{"power",10}})};});
    for (int i=0;i<600 && !battle.battlers()[0].ready();++i) battle.update(1.0/60);
    REQUIRE(battle.submit({0,fe::ActionKind::Skill,"siphon",1}));battle.update(1.0/60);
    CHECK(battle.battlers()[0].hp==40);CHECK(battle.battlers()[1].hp==980);
}
TEST_CASE("declared elements validate references and protect party battlers") {
    auto db=testDatabase();db.elements["fire"]={"fire","Fire"};
    db.actors.at("hero").elements["fire"]=0;
    db.enemies.at("slime").stats.hp=1000;db.enemies.at("slime").skills={"fire"};db.validate();
    auto state=testState(db);fe::Battle battle(db,state,"slimes");
    for (int i=0;i<600;++i) battle.update(1.0/60);
    CHECK(battle.battlers()[0].hp==100);
    db.actors.at("hero").elements["missing"]=1;CHECK_THROWS(db.validate());
}
