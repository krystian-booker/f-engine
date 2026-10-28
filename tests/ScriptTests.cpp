#include <doctest/doctest.h>
#include "engine/script/ScriptEngine.h"
#include <chrono>
#include <fstream>

namespace {
struct Scripts {
    std::filesystem::path folder = std::filesystem::temp_directory_path() /
        ("fengine-lua-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scripts() { std::filesystem::create_directories(folder); }
    ~Scripts() { std::filesystem::remove_all(folder); }
    void put(const char* name, const char* code) { std::ofstream(folder / name) << code; }
};
}
TEST_CASE("Lua events yield dialogue and choices and resume with host responses") {
    Scripts files;
    files.put("event.lua",R"(
      return function(ev)
        assert(ev.self == 'elder')
        assert(os == nil and io == nil and package == nil and dofile == nil)
        ev:face_player()
        say('Elder', 'The crystal is dark...')
        local answer = choice({'Help', 'Leave'})
        if answer == 1 then
          set_flag('quest')
          set_var('answer', answer)
          wait(0.1)
          set_flag('finished')
        end
      end
    )");
    fe::Database db; fe::GameState state;
    fe::ScriptEngine engine(files.folder,state,db);
    std::vector<std::string> requests;
    engine.setHandler([&](const fe::ScriptRequest& request,double) {
        requests.push_back(request.command);
        if (request.command == "say") CHECK(request.arguments[1] == "The crystal is dark...");
        return fe::ScriptResponse{true,request.command == "choice" ? fe::Json(1) : fe::Json()};
    });
    auto id = engine.start("event.lua","elder");
    CHECK(engine.running(id)); CHECK(engine.blocksField());
    for (int i = 0; i < 60 && engine.running(id); ++i) engine.update(1.0/60);
    CHECK_FALSE(engine.running(id)); CHECK_FALSE(engine.blocksField());
    CHECK(state.flag("quest")); CHECK(state.flag("finished")); CHECK(state.variable("answer") == 1);
    REQUIRE(requests.size() == 3);
    CHECK(requests[0] == "face"); CHECK(requests[1] == "say"); CHECK(requests[2] == "choice");
}
TEST_CASE("Lua budgets, isolated globals, cancellation and script reload") {
    Scripts files;
    files.put("event.lua","return function() private = 123; wait(1); set_flag('done') end");
    files.put("other.lua","return function() assert(private == nil); set_flag('isolated') end");
    files.put("loop.lua","return function() while true do end end");
    fe::Database db; fe::GameState state; fe::ScriptEngine engine(files.folder,state,db);
    auto id = engine.start("event.lua","",false);
    CHECK_FALSE(engine.blocksField()); engine.start("other.lua"); CHECK(state.flag("isolated"));
    engine.cancel(id); engine.update(2); CHECK_FALSE(state.flag("done"));
    files.put("event.lua","return function() set_flag('reloaded') end");
    engine.start("event.lua"); CHECK(state.flag("reloaded"));
    auto loop = engine.start("loop.lua"); CHECK_FALSE(engine.running(loop));
    CHECK(engine.lastError().find("instruction budget") != std::string::npos);
    CHECK_THROWS(engine.start("../outside.lua"));
}
TEST_CASE("damage formulas run through Lua") {
    Scripts files;
    files.put("rules.lua",R"(return {
      formulas = {physical = function(a,t,p) return math.max(0,a.attack*2+p-t.defense) end}, ai = {}
    })");
    fe::Database db; fe::GameState state; fe::ScriptEngine engine(files.folder,state,db);
    engine.loadRules("rules.lua");
    fe::Battler a,t; a.stats.attack = 15; t.stats.defense = 4;
    CHECK(engine.formula("physical",a,t,10) == 36);
    CHECK_THROWS(engine.formula("missing",a,t,1));
    files.put("rules.lua","error('bad edit')");
    CHECK_THROWS(engine.loadRules("rules.lua"));
    CHECK(engine.formula("physical",a,t,10) == 36);
}
TEST_CASE("Lua skill effects produce typed effect operations") {
    fe::Database db;fe::GameState state;fe::ScriptEngine engine(FENGINE_SAMPLE_DIR,state,db);engine.loadRules("scripts/rules.lua");
    fe::Battler user,target;user.stats.magic=15;target.hp=30;target.stats.resistance=5;
    const auto effects=engine.effect("drain",user,target,10);
    REQUIRE(effects.size()==2);CHECK(effects[0].type=="damage");CHECK(effects[0].power==30);CHECK(effects[1].power==15);CHECK(effects[1].recipient=="user");
}
TEST_CASE("recursive events are bounded and the scheduler remains usable") {
    Scripts files;files.put("recursive.lua","return function() run_event('recursive.lua') end");
    files.put("okay.lua","return function() set_flag('usable') end");
    fe::Database db;fe::GameState state;fe::ScriptEngine engine(files.folder,state,db);
    engine.start("recursive.lua");CHECK(engine.lastError().find("nesting/task limit")!=std::string::npos);
    engine.update(0);engine.start("okay.lua");CHECK(state.flag("usable"));
}
