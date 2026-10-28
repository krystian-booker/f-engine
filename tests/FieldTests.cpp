#include <doctest/doctest.h>
#include "engine/world/World.h"
#include <chrono>
#include <fstream>

TEST_CASE("authored event pages, interaction and page changes preserve a running cutscene") {
    const auto folder = std::filesystem::temp_directory_path() /
        ("fengine-field-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "chest.lua") << R"(return function(ev)
      set_flag('opened'); wait(0.05); set_flag('finished')
    end)";
    auto data = fe::Json::parse(R"({
      "id":"test", "nav":{"width":6,"depth":6}, "spawns":{"entry":[1.5,0,1.5]},
      "entities":[{"id":"chest","position":[1.5,0,2.5],"pages":[
        {"script":"chest.lua","trigger":"action"},
        {"conditions":{"flags":{"opened":true}},"script":""}
      ]}]
    })");
    auto map = fe::MapData::fromJson(data);
    CHECK(fe::MapData::fromJson(map.toJson()).entities[0].pages.size() == 2);
    fe::Database db; fe::GameState state; fe::ScriptEngine scripts(folder,state,db);
    fe::World world(map,state,db,scripts); world.spawn("entry");
    world.update(1.0/60,{},true);
    CHECK(state.flag("opened")); CHECK(scripts.blocksField());
    for (int i = 0; i < 30; ++i) { world.update(1.0/60,{},false); scripts.update(1.0/60); }
    CHECK(state.flag("finished")); CHECK_FALSE(scripts.blocksField());
    world.update(1.0/60,{},true); CHECK(scripts.taskCount() == 0);
    const auto before = world.playerPosition();
    for (int i = 0; i < 60; ++i) world.update(1.0/60,{0,1},false);
    CHECK(world.playerPosition().z > before.z);
    CHECK(world.playerPosition().z < 2.1f); // solid chest blocks the player
    CHECK(state.position == world.playerPosition());
    std::filesystem::remove_all(folder);
}
TEST_CASE("event page variable comparisons and priority") {
    fe::GameState state; state.variables["chapter"] = 3;
    fe::EventPage page;
    page.conditions = {{"variables",fe::Json::array({{{"id","chapter"},{"op",">="},{"value",2}}})}};
    CHECK(page.matches(state)); state.variables["chapter"] = 1; CHECK_FALSE(page.matches(state));
    page.conditions = {{"flags",{{"quest",false}}}}; CHECK(page.matches(state));
    state.flags["quest"] = true; CHECK_FALSE(page.matches(state));
}
TEST_CASE("scripted movers follow a grid path around solid walls") {
    fe::MapData map;map.id="path";map.nav=fe::NavGrid(8,8);map.spawns["entry"]={1.5f,0,1.5f};
    for (int z=0;z<5;++z) map.nav.at(3,z).walkable=false;
    fe::Database db;fe::GameState state;fe::ScriptEngine scripts(std::filesystem::temp_directory_path(),state,db);
    fe::World world(std::move(map),state,db,scripts);world.spawn("entry");
    bool arrived=false;
    for (int i=0;i<1000 && !arrived;++i) {
        world.beginTick(1.0/60);arrived=world.moveEntity("player",{5.5f,1.5f},1.0/60);
        const auto p=world.playerPosition();CHECK(world.map().nav.canStand({p.x,p.z}));
    }
    REQUIRE(arrived);CHECK(world.playerPosition().x==doctest::Approx(5.5f).epsilon(.01));
    CHECK(world.playerPosition().z==doctest::Approx(1.5f).epsilon(.02));
}
