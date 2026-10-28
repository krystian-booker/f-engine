#include "engine/runtime/Engine.h"
#include <doctest/doctest.h>
#include <fstream>

using namespace fe;

namespace {
void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

struct Folders {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "fengine-lua-test";
    std::filesystem::path game = root / "game", lib = root / "lib";
    Folders() {
        std::filesystem::remove_all(root);
        write(lib / "fe" / "init.lua", "return {}");
        write(lib / "shared.lua", "return {who = 'library'}");
        write(lib / "only_lib.lua", "return {who = 'library'}");
        write(game / "shared.lua", "return {who = 'game'}");
    }
};
} // namespace

TEST_CASE("require searches the game before the library and caches") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    sol::table shared = host.require("shared");
    CHECK(shared.get<std::string>("who") == "game");
    sol::table only = host.require("only_lib");
    CHECK(only.get<std::string>("who") == "library");
    CHECK(host.require("shared") == shared);
    CHECK_THROWS(host.require("../escape"));
    CHECK_THROWS(host.require("missing"));
}

TEST_CASE("the sandbox has no io, os, debug or dynamic loading") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    for (const char* name : {"io", "os", "debug", "package", "load", "loadstring", "dofile", "loadfile"}) {
        CHECK(host.console(name) == "nil");
    }
    CHECK(host.console("string.dump") == "nil");
}

TEST_CASE("hot reload patches the existing module table") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    sol::table module = host.require("shared");
    write(f.game / "shared.lua", "return {who = 'edited', extra = 1}");
    CHECK(host.reloadFile(f.game / "shared.lua"));
    CHECK(module.get<std::string>("who") == "edited");
    CHECK(module.get<int>("extra") == 1);
    write(f.game / "shared.lua", "return {who = ");
    CHECK(host.reloadFile(f.game / "shared.lua"));
    CHECK(module.get<std::string>("who") == "edited"); // broken edit keeps the old version
    CHECK_FALSE(host.errors().empty());
}

TEST_CASE("the development watchdog stops runaway scripts") {
    Folders f;
    LuaHost host(f.game, f.lib, true);
    host.setWatchdog(0.05);
    host.beginFrame();
    const auto result = host.console("while true do end");
    CHECK(result.find("watchdog") != std::string::npos);
}

TEST_CASE("Lua values convert to and from JSON") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    host.console("value = {name = 'x', list = {1, 2, 3}, nested = {ok = true}}");
    const auto json = luaToJson(host.lua()["value"]);
    CHECK(json.at("list").size() == 3);
    CHECK(json.at("nested").at("ok") == true);
    sol::table back = jsonToLua(host.lua(), json);
    CHECK(back.get<std::string>("name") == "x");
    host.console("bad = {print}");
    CHECK_THROWS(luaToJson(host.lua()["bad"]));
}

TEST_CASE("the engine binds services and runs the entry module headless") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = std::filesystem::temp_directory_path() / "fengine-engine-test";
    options.seed = 99;
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    CHECK(engine.lua().console("type(fe.world.load)") == "function");
    CHECK(engine.lua().console("fe.world.load('main', 'arena').map_id") == "arena");
    CHECK(engine.lua().console(
              "local e = fe.world.get('main'):spawn{id='hero', position={x=2.5,z=5.5}}; return e.id") ==
          "hero");
    CHECK(engine.lua().console("#fe.world.get('main'):reachable({x=2,z=5}, 2)") != "0");
    CHECK(engine.lua().console("fe.random.new(5):int(1, 100) == fe.random.new(5):int(1, 100)") == "true");
    engine.tick(1.0 / 60);
    engine.draw(1);
    CHECK(engine.lua().errors().empty());
}
