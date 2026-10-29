#include "engine/core/Paths.h"
#include "engine/core/Random.h"
#include "engine/runtime/Engine.h"
#include "engine/runtime/Headless.h"
#include "engine/script/bindings/WorldTypes.h"
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <glm/gtc/constants.hpp>
#include <iostream>
#include <limits>
#include <set>

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

// A game folder with the fixture's maps and a custom entry module.
struct TestGame {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "fengine-engine-game";
    explicit TestGame(const std::string& main) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::copy(std::filesystem::path(FENGINE_TEST_GAME) / "maps", root / "maps",
                              std::filesystem::copy_options::recursive);
        std::filesystem::copy_file(std::filesystem::path(FENGINE_TEST_GAME) / "game.json",
                                   root / "game.json");
        write(root / "main.lua", main);
    }
};

class RecordingAudio final : public NullAudio {
public:
    [[nodiscard]] bool available() const override { return true; }
    int playSound(const std::filesystem::path& file, const SoundOptions&) override {
        ++attempts;
        if (file.filename() == "missing.ogg") {
            throw std::runtime_error("Cannot open " + file.string());
        }
        playing.insert(++next);
        return next;
    }
    void stopSound(int voice, double) override { playing.erase(voice); }
    std::set<int> playing;
    int next = 0, attempts = 0;
};

EngineOptions engineOptions(const std::filesystem::path& game = FENGINE_TEST_GAME) {
    EngineOptions options;
    options.gameRoot = game;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = std::filesystem::temp_directory_path() / "fengine-engine-test";
    options.seed = 99;
    return options;
}

std::string pairsOrder(LuaHost& host) {
    return host.console(
        "local t = {} for i = 1, 64 do t['key' .. i] = i end "
        "local out = {} for k in pairs(t) do out[#out + 1] = k end return table.concat(out, ',')");
}
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
    CHECK(host.console("getmetatable('')") == "false");
    CHECK_FALSE(host.evaluate("getmetatable('').__index.upper = nil").ok);
    CHECK_FALSE(host.evaluate("collectgarbage('stop')").ok);
}

TEST_CASE("engine objects cannot reach their metamethods") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    lua.evaluate(
        "w = fe.world.load('main', 'arena'); e = w:spawn{id = 'hero', position = {x = 2.5, z = 5.5}}; "
        "c = w:camera(); r = fe.random.new(1)");
    for (const char* value : {"w", "e", "c", "r"}) {
        CAPTURE(value);
        const std::string v = value;
        CHECK(lua.console("getmetatable(" + v + ")") == "false");
        CHECK_FALSE(lua.evaluate("setmetatable(" + v + ", nil)").ok);
        // sol2 stores metamethods and its own bookkeeping in the metatable that
        // also serves as __index; none of it is a member.
        for (const char* key :
             {"__gc", "__index", "__newindex", "__eq", "__tostring", "__type", "__name", "__pairs", "__call",
              "__metatable", "__sol.storage", "class_cast", "class_check", "new", "is", "\\226\\153\\187"}) {
            CAPTURE(key);
            CHECK(lua.console(v + "['" + key + "']") == "nil");
        }
        for (const char* key : {"1", "true", "2.5", v.c_str(), "{}"}) {
            CHECK(lua.console(v + "[" + key + "]") == "nil");
        }
        // The original crashes: calling __gc by hand destroyed the handle twice.
        CHECK_FALSE(lua.evaluate(v + ".__gc(" + v + ")").ok);
        CHECK_FALSE(lua.evaluate("getmetatable(" + v + ").__gc(" + v + ")").ok);
        for (const char* key : {"__gc", "__index", "__newindex", "__eq", "class_cast", "new"}) {
            CAPTURE(key);
            CHECK_FALSE(lua.evaluate(v + "['" + key + "'] = print").ok);
        }
        CHECK_FALSE(lua.evaluate(v + "[1] = 1").ok);
        CHECK_FALSE(lua.evaluate("rawget(" + v + ", '__gc')").ok);
        CHECK_FALSE(lua.evaluate("rawset(" + v + ", '__gc', print)").ok);
        CHECK_FALSE(lua.evaluate("next(" + v + ")").ok);
        CHECK_FALSE(lua.evaluate("for k in pairs(" + v + ") do end").ok);
        CHECK(lua.evaluate("for _ in ipairs(" + v + ") do error('a handle is not a list') end").ok);
        CHECK(lua.console(v + " == " + v + ", rawequal(" + v + ", " + v + ")") == "true\ttrue");
    }
    // Members still work, and only writable properties take assignments.
    CHECK(lua.console("tostring(w), tostring(e), w.name, e:valid()") ==
          "World(main)\tEntity(hero)\tmain\ttrue");
    CHECK(lua.console("w == fe.world.get('main'), e == w:find('hero'), e == w, r == c") ==
          "true\ttrue\tfalse\tfalse");
    CHECK(lua.evaluate("e.yaw = 90; c.fov = 30").ok);
    CHECK(lua.console("e.yaw, c.fov") == "90.0\t30.0");
    CHECK(lua.evaluate("w.spawn = print").output.find("World.spawn is read-only") != std::string::npos);
    CHECK(lua.evaluate("e.key = 3").output.find("Entity.key is read-only") != std::string::npos);
    CHECK(lua.evaluate("e.speed = 3").output.find("Entity has no field 'speed'") != std::string::npos);
    // Methods check their object, so one type's method cannot run on another.
    CHECK_FALSE(lua.evaluate("w.spawn(e, {position = {x = 1, z = 1}})").ok);
    CHECK_FALSE(lua.evaluate("e.destroy(w)").ok);
    CHECK_FALSE(lua.evaluate("r.int(c, 1, 2)").ok);
    CHECK(lua.console("w.spawn ~= nil and e.destroy ~= nil") == "true");
    CHECK(lua.evaluate("w = nil; e = nil; c = nil; r = nil; fe.world.unload('main'); "
                       "for i = 1, 50 do collectgarbage('step') end")
              .ok);
    CHECK(lua.console("e == nil and fe.world.get('main') == nil") == "true");
}

TEST_CASE("engine type tables and sol2 storage are not reachable from scripts") {
    Engine engine(engineOptions());
    REQUIRE(engine.start());
    engine.tick(1.0 / 60);
    auto& lua = engine.lua();
    CHECK(lua.console("World, Entity, Camera, RandomStream") == "nil\tnil\tnil\tnil");
    // The reviewer's type confusion: a World destructor on RandomStream values.
    CHECK_FALSE(lua.evaluate("RandomStream.__gc = fe.world.load('main', 'arena').__gc").ok);
    CHECK(lua.evaluate("for i = 1, 100 do local r = fe.random.new(i) end; "
                       "for i = 1, 50 do collectgarbage('step') end")
              .ok);
    // Nothing reachable from the globals is sol2's: no "sol.*" globals (type
    // storage, exception handler), no userdata, no locked tables.
    CHECK(lua.console(R"(
local seen, found = {}, {}
local function walk(t, path)
  if seen[t] then return end
  seen[t] = true
  if getmetatable(t) == false then found[#found + 1] = 'locked ' .. path end
  for k, v in pairs(t) do
    local name = path .. '.' .. tostring(k)
    if type(k) == 'string' and k:sub(1, 4) == 'sol.' then found[#found + 1] = name end
    if type(v) == 'userdata' then found[#found + 1] = 'userdata ' .. name end
    if type(v) == 'table' then walk(v, name) end
  end
end
walk(_G, '_G')
return #found == 0 and 'clean' or table.concat(found, ', '))") == "clean");
    CHECK(lua.console("fe.random.new(3):int(1, 6) == fe.random.new(3):int(1, 6)") == "true");
}

TEST_CASE("evaluate reports success, results and errors") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    const auto value = host.evaluate("1 + 2, 'x'");
    CHECK(value.ok);
    CHECK(value.output == "3\tx");
    const auto statement = host.evaluate("answer = 42");
    CHECK(statement.ok);
    CHECK(statement.output.empty());
    const auto failed = host.evaluate("error('boom')");
    CHECK_FALSE(failed.ok);
    CHECK(failed.output.find("boom") != std::string::npos);
    const auto syntax = host.evaluate("answer = = 1");
    CHECK_FALSE(syntax.ok);
    CHECK(syntax.output.find("unexpected symbol") != std::string::npos);
}

TEST_CASE("a failing __tostring cannot unwind through C++") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    host.evaluate("bad = setmetatable({}, {__tostring = function() error('no text') end})");
    CHECK(host.evaluate("print('value', bad)").ok);
    const auto shown = host.evaluate("bad");
    CHECK(shown.ok);
    CHECK(shown.output.find("no text") != std::string::npos);
    host.evaluate("odd = setmetatable({}, {__tostring = function() return {} end})");
    CHECK(host.evaluate("odd").output.find("tostring failed") != std::string::npos);
}

TEST_CASE("string hashing is seeded deterministically") {
    // Lua 5.4 normally seeds string hashes from the clock and addresses,
    // which changes `pairs` order from one state to the next.
    Folders f;
    LuaHost first(f.game, f.lib, false), second(f.game, f.lib, false);
    CHECK(pairsOrder(first) == pairsOrder(second));
}

TEST_CASE("hot reload keeps live state and replaces code") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    write(f.game / "counter.lua", R"(
local M = {count = 0, limit = 5}
function M.bump() M.count = M.count + 1 return M.count end
function M.hook() return 'default' end
function M.old() return 'old' end
return M)");
    sol::table module = host.require("counter");
    host.evaluate("counter = require('counter'); counter.bump(); counter.bump(); counter.extra = 'runtime'");
    host.evaluate("counter.hook = function() return 'game' end");
    write(f.game / "counter.lua", R"(
local M = {count = 0, limit = 9, added = true}
function M.bump() M.count = M.count + 10 return M.count end
function M.hook() return 'default' end
return M)");
    CHECK(host.reloadFile(f.game / "counter.lua"));
    CHECK(host.require("counter") == module);
    // The new function's upvalue M is the live table, whose count survived.
    CHECK(host.console("counter.bump()") == "12");
    CHECK(module.get<int>("count") == 12);
    CHECK(module.get<int>("limit") == 5);
    CHECK(module.get<bool>("added"));
    CHECK(host.console("counter.extra") == "runtime");
    CHECK(host.console("counter.old()") == "old");
    CHECK(host.console("counter.hook()") == "game");

    write(f.game / "counter.lua", R"(
local M = {count = 0, limit = 9}
function M.bump() return M.count end
function M.__reload(live, fresh) live.limit = fresh.limit end
return M)");
    CHECK(host.reloadFile(f.game / "counter.lua"));
    CHECK(module.get<int>("limit") == 9);

    write(f.game / "counter.lua", "return {count = ");
    CHECK(host.reloadFile(f.game / "counter.lua"));
    CHECK(host.console("counter.bump()") == "12"); // a broken edit keeps the old version
    CHECK_FALSE(host.errors().empty());
    CHECK_FALSE(host.reloadFile(f.game / "unrelated.lua"));
}

TEST_CASE("hot reload keeps module locals shared between old and new code") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    write(f.game / "store.lua", R"(
local M = {}
local items = {}
local calls = 0
local function remember(x) items[#items + 1] = x end
function M.add(x) calls = calls + 1 remember(x) end
function M.size() return #items, calls end
return M)");
    host.evaluate("store = require('store'); store.add('a'); store.add('b')");
    write(f.game / "store.lua", R"(
local M = {}
local items = {}
local calls = 0
local function remember(x) items[#items + 1] = x end
function M.add(x) calls = calls + 1 remember(x) end
function M.size() return #items * 100, calls end
return M)");
    CHECK(host.reloadFile(f.game / "store.lua"));
    CHECK(host.console("store.size()") == "200\t2");
    host.evaluate("store.add('c')");
    CHECK(host.console("store.size()") == "300\t3");
}

TEST_CASE("hot reload joins every closure to one variable per module local") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    const std::string source = R"(
local M = {}
local box
local count = 0
local function close() box = nil end
function M.get() return box end
function M.close() close() end
function M.count() return count end
M.handlers = {open = function(v) box = v count = count + 1 end}
return M)";
    write(f.game / "box.lua", source);
    host.evaluate("box = require('box'); box.handlers.open({1}); "
                  "old_get, old_close, old_open = box.get, box.close, box.handlers.open");
    write(f.game / "box.lua", source + "\n");
    CHECK(host.reloadFile(f.game / "box.lua"));
    // The live table survives although the fresh copy's `box` is nil.
    CHECK(host.console("box.get()[1], box.count()") == "1\t1");
    host.evaluate("box.close()");
    CHECK(host.console("box.get(), old_get()") == "nil\tnil");
    host.evaluate("old_open({2})");
    CHECK(host.console("box.get()[1], box.count()") == "2\t2");
    host.evaluate("box.handlers.open({3})");
    CHECK(host.console("old_get()[1], old_close(), box.get()") == "3\tnil\tnil");
    host.evaluate("box.handlers.open({4})");
    CHECK(host.console("old_get()[1], box.get()[1], box.count(), old_open == box.handlers.open") ==
          "4\t4\t4\tfalse");
    CHECK(host.errors().empty());
}

TEST_CASE("hot reload joins locals whatever the live and fresh values are") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    write(f.game / "vars.lua", R"(
local M = {}
local slot = {}
local empty
local n = 1
local only_old = 'old'
function M.set_slot(v) slot = v end
function M.slot() return slot end
function M.set_empty(v) empty = v end
function M.n() n = n + 1 return n end
function M.peek() return n end
function M.only_old() return only_old end
return M)");
    host.evaluate("vars = require('vars'); vars.set_slot(nil); vars.n(); "
                  "old_slot, old_empty, old_n, old_peek = vars.slot, vars.set_empty, vars.n, vars.peek");
    write(f.game / "vars.lua", R"(
local M = {}
local slot = {fresh = true}
local empty = {fresh = true}
local n = 1
local only_new = 10
function M.set_slot(v) slot = v end
function M.slot() return slot end
function M.set_empty(v) empty = v end
function M.empty() return empty end
function M.n() n = n + 1 return n end
function M.only_new() only_new = only_new + 1 return only_new end
return M)");
    CHECK(host.reloadFile(f.game / "vars.lua"));
    // nil live -> the fresh table; the variable stays shared with old closures.
    CHECK(host.console("vars.slot().fresh, old_slot().fresh") == "true\ttrue");
    host.evaluate("vars.set_slot('x')");
    CHECK(host.console("old_slot()") == "x");
    // A local only the new code reads joins the old closures that set it.
    CHECK(host.console("vars.empty().fresh") == "true");
    host.evaluate("old_empty(5)");
    CHECK(host.console("vars.empty()") == "5");
    // Numbers keep their live value and stay shared.
    CHECK(host.console("vars.n(), old_n(), vars.n()") == "3\t4\t5");
    CHECK(host.console("vars.only_old(), vars.only_new(), vars.only_new()") == "old\t11\t12");

    write(f.game / "vars.lua", R"(
local M = {}
local slot = 'text'
local n = 'changed'
function M.slot() return slot end
function M.n() return n end
return M)");
    CHECK(host.reloadFile(f.game / "vars.lua"));
    // A changed type takes the fresh value for old and new closures alike.
    CHECK(host.console("vars.n(), old_peek()") == "changed\tchanged");
    CHECK(host.console("vars.slot()") == "x");
    host.evaluate("vars.set_slot(nil)");
    CHECK(host.console("vars.slot(), old_slot()") == "nil\tnil");
    CHECK(host.errors().empty());
}

TEST_CASE("hot reload walks deep module data without recursing") {
    // The reviewer's probe: a module-level linked list 150000 nodes long
    // overflowed the C++ stack of the recursive patcher.
    Folders f;
    LuaHost host(f.game, f.lib, false);
    const std::string source = R"(
local M = {}
local head = nil
function M.build(n) for i = 1, n do head = {next = head, i = i} end end
function M.depth() local d, h = 0, head while h do d = d + 1 h = h.next end return d end
function M.top() return head and head.i end
M.nested = {a = {b = {c = {value = 1}}}}
return M)";
    write(f.game / "deep.lua", source);
    host.evaluate("deep = require('deep'); deep.build(150000)");
    std::string edited = source;
    edited.replace(edited.find("return d end"), 12, "return d * 2 end");
    edited.replace(edited.find("value = 1"), 9, "value = 1, added = true");
    write(f.game / "deep.lua", edited);
    CHECK(host.reloadFile(f.game / "deep.lua"));
    CHECK(host.errors().empty());
    CHECK(host.console("deep.depth(), deep.top()") == "300000\t150000");
    CHECK(host.console("deep.nested.a.b.c.value, deep.nested.a.b.c.added") == "1\ttrue");
}

TEST_CASE("hot reload keeps two variables of the same name apart") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    const std::string source = R"(
local M = {}
local value = 1
function M.first() value = value + 1 return value end
do
    local value = 100
    function M.second() value = value + 1 return value end
end
return M)";
    write(f.game / "twin.lua", source);
    host.evaluate("twin = require('twin'); twin.first(); twin.second()");
    write(f.game / "twin.lua", source + "\n");
    CHECK(host.reloadFile(f.game / "twin.lua"));
    CHECK(host.console("twin.first(), twin.second()") == "2\t101");
}

TEST_CASE("hot reload patches class tables so existing instances use new methods") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    write(f.game / "shapes.lua", R"(
local M = {}
local Box = {}
Box.__index = Box
function Box.new(v) return setmetatable({v = v}, Box) end
function Box:get() return self.v end
M.Box = Box
M.Derived = setmetatable({}, {__index = Box})
M.Derived.__index = M.Derived
function M.Derived.new(v) return setmetatable({v = v}, M.Derived) end
return M)");
    host.evaluate("shapes = require('shapes'); box = shapes.Box.new(3); derived = shapes.Derived.new(5)");
    write(f.game / "shapes.lua", R"(
local M = {}
local Box = {}
Box.__index = Box
function Box.new(v) return setmetatable({v = v}, Box) end
function Box:get() return self.v * 2 end
function Box:twice() return Box.get(self) * 2 end
M.Box = Box
M.Derived = setmetatable({}, {__index = Box})
M.Derived.__index = M.Derived
function M.Derived.new(v) return setmetatable({v = v}, M.Derived) end
return M)");
    CHECK(host.reloadFile(f.game / "shapes.lua"));
    CHECK(host.console("box:get()") == "6");
    CHECK(host.console("box:twice()") == "12");
    CHECK(host.console("derived:get()") == "10");
    CHECK(host.console("getmetatable(shapes.Box.new(1)) == getmetatable(box)") == "true");
    CHECK(host.console("getmetatable(shapes.Derived).__index == shapes.Box") == "true");
}

TEST_CASE("hot reload follows a module that now requires a different module") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    write(f.game / "dep_a.lua", "return {name = 'a'}");
    write(f.game / "dep_b.lua", "return {name = 'b'}");
    write(f.game / "user.lua",
          "local dep = require('dep_a') return {dep = dep, name = function() return dep.name end}");
    host.evaluate("user = require('user')");
    write(f.game / "user.lua",
          "local dep = require('dep_b') return {dep = dep, name = function() return dep.name end}");
    CHECK(host.reloadFile(f.game / "user.lua"));
    CHECK(host.console("user.name(), user.dep == require('dep_b'), require('dep_a').name") == "b\ttrue\ta");
}

TEST_CASE("reloading stateful kits keeps their state") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(
        lua.evaluate("task = require('fe.task'); state = require('fe.state'); scene = require('fe.scene'); "
                     "done = false; task.spawn(function() task.wait(0.1); done = true end); "
                     "state.set_flag('met', true); scene.push({name = 'probe'})")
            .ok);
    const auto lib = std::filesystem::path(FENGINE_LUALIB) / "fe";
    for (const char* file : {"task.lua", "state.lua", "scene.lua"}) {
        CHECK(lua.reloadFile(lib / file));
    }
    CHECK(lua.errors().empty());
    CHECK(lua.console("task.count(), state.flag('met'), scene.top().name") == "1\ttrue\tprobe");
    lua.evaluate("for i = 1, 10 do task.update(1 / 60) end");
    CHECK(lua.console("done, task.count()") == "true\t0");
}

TEST_CASE("the development watchdog stops runaway scripts") {
    Folders f;
    LuaHost host(f.game, f.lib, true);
    host.setWatchdog(0.05);
    host.beginFrame();
    const auto result = host.evaluate("while true do end");
    CHECK_FALSE(result.ok);
    CHECK(result.output.find("watchdog") != std::string::npos);
}

TEST_CASE("the headless test runner gives every test the whole watchdog limit") {
    TestGame game("return {}");
    const auto spin = [](const std::string& seconds) {
        return "function(t) local stop = fe.time.wall() + " + seconds +
               " while fe.time.wall() < stop do end t.ok(true) end";
    };
    // Three tests of 0.2 s each: together, but not one by one, over the limit.
    write(game.root / "tests" / "slow.lua",
          "return {a = " + spin("0.2") + ", b = " + spin("0.2") + ", c = " + spin("0.2") + "}");
    CHECK(runTests(game.root, FENGINE_LUALIB, "", 0.5) == 0);
    write(game.root / "tests" / "slow.lua", "return {runaway = " + spin("5") + "}");
    CHECK(runTests(game.root, FENGINE_LUALIB, "", 0.5) == 1);
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
    host.console("float_key = {[1.5] = true}");
    CHECK_THROWS(luaToJson(host.lua()["float_key"]));
    host.console("loop = {} loop.self = loop");
    CHECK_THROWS_WITH(luaToJson(host.lua()["loop"]), doctest::Contains("deeper than 128"));
}

TEST_CASE("deeply nested JSON raises an error instead of overflowing the stack") {
    // The reviewer's probes: fe.json.decode of 100000 nested arrays crashed,
    // and so did reading a save or config nested that deep.
    TestGame game("return {}");
    Engine engine(engineOptions(game.root));
    auto& lua = engine.lua();
    const auto nested = [](int depth) {
        return std::string(depth, '[') + std::string(depth, ']');
    };
    lua.lua()["deep"] = nested(100000);
    lua.lua()["fits"] = nested(maxJsonDepth + 1);
    lua.lua()["over"] = nested(maxJsonDepth + 2);
    CHECK(lua.evaluate("fe.json.decode(deep)").output.find("deeper than 128") != std::string::npos);
    CHECK(lua.evaluate("fe.json.decode(over)").output.find("deeper than 128") != std::string::npos);
    CHECK(lua.console("type(fe.json.decode(fits))") == "table");
    CHECK(lua.evaluate("fe.json.encode(fe.json.decode(fits))").ok);
    writeTextAtomic(engine.saves().directory() / "saves" / "deep.json",
                    "{\"format\": 1, \"meta\": {}, \"data\": " + nested(100000) + "}");
    CHECK(lua.evaluate("fe.save.read('deep')").output.find("deeper than 128") != std::string::npos);
    writeTextAtomic(engine.saves().directory() / "config.json", "{\"a\": " + nested(100000) + "}");
    CHECK(lua.evaluate("fe.save.read_config()").output.find("deeper than 128") != std::string::npos);
}

TEST_CASE("JSON keeps integer and '#' keys distinct from string keys") {
    Folders f;
    LuaHost host(f.game, f.lib, false);
    host.console(
        "value = {[1] = 'a', [3] = 'c', [-2] = 'n', ['1'] = 's', ['#tag'] = 't', mixed = {10, 20, n = 2}}");
    const auto json = luaToJson(host.lua()["value"]);
    CHECK(json.at("#1") == "a");
    CHECK(json.at("#3") == "c");
    CHECK(json.at("1") == "s");
    CHECK(json.at("##tag") == "t");
    host.lua()["back"] = jsonToLua(host.lua(), Json::parse(json.dump()));
    CHECK(host.console("back[1], back[3], back[-2], back['1'], back['#tag']") == "a\tc\tn\ts\tt");
    CHECK(host.console("back[2] == nil and back.mixed[2] == 20 and back.mixed.n == 2") == "true");
    host.lua()["authored"] = jsonToLua(host.lua(), Json::parse(R"({"#x": 1, "#01": 2, "2": 3})"));
    CHECK(host.console("authored['#x'], authored['#01'], authored['2']") == "1\t2\t3");
}

TEST_CASE("development-only bindings are absent from release games") {
    auto release = engineOptions();
    release.development = false;
    Engine shipped(std::move(release));
    CHECK(shipped.lua().console("type(fe.dev.console), type(fe.input.feed)") == "nil\tnil");
    auto replay = engineOptions();
    replay.development = false;
    replay.testHooks = true;
    Engine replaying(std::move(replay));
    CHECK(replaying.lua().console("type(fe.dev.console), type(fe.input.feed)") == "function\tfunction");
    Engine development(engineOptions());
    CHECK(development.lua().console("select(2, fe.dev.console('error(1)'))") == "false");
}

TEST_CASE("the engine binds services and runs the entry module headless") {
    Engine engine(engineOptions());
    REQUIRE(engine.start());
    CHECK(engine.lua().console("type(fe.world.load)") == "function");
    CHECK(engine.lua().console("fe.world.load('main', 'arena').map_id") == "arena");
    CHECK(engine.lua().console(
              "local e = fe.world.get('main'):spawn{id='hero', position={x=2.5,z=5.5}}; return e.id") ==
          "hero");
    CHECK(engine.lua().console("#fe.world.get('main'):reachable({x=2,z=5}, 2)") != "0");
    CHECK(engine.lua().console("fe.random.new(5):int(1, 100) == fe.random.new(5):int(1, 100)") == "true");
    CHECK(engine.lua().console("tostring(fe.world.get('main'))") == "World(main)");
    engine.tick(1.0 / 60);
    engine.draw(1);
    CHECK(engine.lua().errors().empty());
}

TEST_CASE("an engine can reuse a configuration the caller loaded") {
    auto config = GameConfig::load(FENGINE_TEST_GAME);
    config.title = "Preloaded";
    Engine engine(engineOptions(), config);
    CHECK(engine.lua().console("fe.app.title()") == "Preloaded");
}

TEST_CASE("post_update runs after the worlds tick") {
    TestGame game(R"(
local log = {}
return {
  log = log,
  init = function() fe.world.load('main', 'arena') end,
  update = function() log[#log + 1] = 'update ' .. fe.world.get('main'):time() end,
  post_update = function(dt) log[#log + 1] = 'post ' .. fe.world.get('main'):time() end,
})");
    Engine engine(engineOptions(game.root));
    REQUIRE(engine.start());
    engine.tick(0.5);
    sol::table log = engine.mainModule().as<sol::table>()["log"];
    REQUIRE(log.size() == 2);
    CHECK(log.get<std::string>(1) == "update 0.0");
    CHECK(log.get<std::string>(2) == "post 0.5");
}

TEST_CASE("invalid arguments raise clear errors instead of defaulting") {
    TestGame game("return {}");
    write(game.root / "data" / "broken.json", "{\"a\": ");
    Engine engine(engineOptions(game.root));
    auto& lua = engine.lua();
    lua.evaluate("w = fe.world.load('main', 'arena'); e = w:spawn{position = {x = 2.5, z = 5.5}}");
    const auto fails = [&lua](const std::string& code, const std::string& message) {
        const auto result = lua.evaluate(code);
        CAPTURE(code);
        CAPTURE(result.output);
        CHECK_FALSE(result.ok);
        CHECK(result.output.find(message) != std::string::npos);
    };
    fails("e:face('nrth')", "Unknown direction 'nrth'");
    fails("e:sprite{mode = 'billboard'}", "Unknown sprite mode");
    fails("fe.canvas.text('x', 0, 0, {align = 'middle'})", "Unknown text align");
    fails("e:place('here')", "Expected a position {x, y, z} table");
    fails("e:place({x = 'one', z = 1})", "must be a finite number");
    fails("fe.screen.set{fade = 0.5}", "0xRRGGBBAA");
    fails("fe.screen.set{tint = true}", "Expected a color");
    fails("fe.random.new(1.5)", "must be an integer");
    fails("fe.json.read('data/broken.json')", "data/broken.json");
    fails("e:audio{}", "needs a `sound`");
    fails("fe.app.capture('../saves/slot1.png')", "");
    fails("fe.app.capture('shot.json')", ".png");
    CHECK(lua.evaluate("fe.screen.set(fe.screen.get())").ok);
    CHECK(lua.evaluate("fe.screen.set{fade = 0x000000ff}").ok);
    CHECK(lua.evaluate("e:face('west')").ok);
    CHECK(lua.evaluate("fe.random.new(-3):int(1, 6)").ok);
    CHECK(lua.evaluate("fe.app.capture('shots/one.png')").ok);
    CHECK(engine.requests().captures.back() ==
          std::filesystem::weakly_canonical(engine.saves().screenshotFolder() / "shots" / "one.png"));
}

TEST_CASE("entities attach only to live entities of the same world") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    lua.evaluate("a = fe.world.load('a', 'arena'):spawn{position = {x = 2.5, z = 5.5}}");
    lua.evaluate("b = fe.world.load('b', 'arena'):spawn{position = {x = 2.5, z = 5.5}}");
    lua.evaluate("c = fe.world.get('a'):spawn{position = {x = 3.5, z = 5.5}}");
    CHECK_FALSE(lua.evaluate("a:attach(b)").ok);
    CHECK_FALSE(lua.evaluate("a:attach(a)").ok);
    CHECK(lua.evaluate("a:attach(c, {y = 1})").ok);
}

TEST_CASE("positional sounds follow drawn worlds and stop with their entity") {
    auto options = engineOptions();
    auto audio = std::make_unique<RecordingAudio>();
    auto& sounds = *audio;
    options.audio = std::move(audio);
    Engine engine(std::move(options));
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); p = w:camera().target; "
                         "e = w:spawn{position = p, audio = {sound = 'audio/falls.ogg'}}")
                .ok);
    const auto frame = [&engine](bool drawn) {
        engine.draw(1);
        if (drawn) {
            engine.queueDraw(engine.world("main"));
        }
        engine.tick(1.0 / 60);
    };
    frame(true);
    CHECK(sounds.playing.size() == 1);
    frame(false);
    CHECK(sounds.playing.empty()); // the world is no longer drawn
    frame(true);
    CHECK(sounds.playing.size() == 1);
    lua.evaluate("e:destroy()");
    frame(true);
    CHECK(sounds.playing.empty());
    lua.evaluate("e = w:spawn{position = p, audio = {sound = 'audio/falls.ogg'}}");
    frame(true);
    CHECK(sounds.playing.size() == 1);
    lua.evaluate("w = nil; e = nil; fe.world.unload('main')");
    frame(false);
    CHECK(sounds.playing.empty());

    lua.evaluate("w = fe.world.load('main', 'arena'); w:spawn{position = w:camera().target, audio = {sound = "
                 "'audio/missing.ogg'}}");
    const int before = sounds.attempts;
    for (int i = 0; i < 5; ++i) {
        frame(true);
    }
    CHECK(sounds.attempts == before + 1); // a broken sound is reported once, not every tick
}

namespace {
// Runs `code`, expecting a Lua error whose message contains `message`.
void raises(LuaHost& lua, const std::string& code, const std::string& message) {
    const auto result = lua.evaluate(code);
    CAPTURE(code);
    CAPTURE(result.output);
    CHECK_FALSE(result.ok);
    CHECK(result.output.find(message) != std::string::npos);
}

// Tables whose metamethods would run script code (and fail loudly, counting
// how often they were consulted) if a binding ever looked past the table.
struct HostileFixture {
    Engine engine{engineOptions()};
    LuaHost& lua = engine.lua();
    HostileFixture() {
        REQUIRE(lua.evaluate("touched = 0; local function touch(_, key) touched = touched + 1; "
                             "error('consulted ' .. tostring(key)) end; "
                             "hostile = setmetatable({}, {__index = touch, __newindex = touch, "
                             "__len = touch, __pairs = touch, __eq = touch, __lt = touch, __le = touch, "
                             "__call = touch, __concat = touch, __tostring = touch, __name = 'Hostile'}); "
                             "w = fe.world.load('main', 'arena'); e = w:spawn{position = {x = 2.5, z = 5.5}}")
                    .ok);
    }
    // Runs `code` (it may succeed or raise its own error, as for an empty
    // table) and checks no metamethod of `hostile` ran.
    void untouched(const std::string& code) {
        CAPTURE(code);
        REQUIRE(lua.evaluate("touched = 0").ok);
        const auto result =
            lua.evaluate("local ok, message = pcall(function() " + code + " end); return ok, message");
        CAPTURE(result.output);
        CHECK(lua.console("touched") == "0");
        CHECK(result.output.find("consulted") == std::string::npos);
    }
};
} // namespace

// Option, vector and colour tables are read raw: a metamethod never runs while
// a binding holds engine references, so it cannot unload a world or destroy
// an entity under it. Fields must be in the table itself.
TEST_CASE("core bindings never run metamethods of the tables they read") {
    HostileFixture f;
    for (const char* code : {"fe.app.set_display(hostile)", "fe.app.preload(hostile)",
                             "fe.json.encode(hostile)", "fe.save.write('slot1', hostile)",
                             "fe.random.new(1):pick(hostile)", "fe.random.new(1):shuffle(hostile)"}) {
        f.untouched(code);
    }
    CHECK(f.lua.console("1 + 1") == "2");
}

TEST_CASE("entity bindings never run metamethods of the tables they read") {
    HostileFixture f;
    for (const char* code :
         {"w:spawn(hostile)",
          "w:spawn{position = hostile}",
          "e:configure(hostile)",
          "e:configure{facing = hostile, scale = hostile, sprite = hostile, mesh = hostile, light = hostile}",
          "e:sprite(hostile)",
          "e:sprite{tint = hostile, offset = hostile}",
          "e:mover(hostile)",
          "e:mover{jump = hostile}",
          "e:move_to({x = 3.5, z = 5.5}, hostile)",
          "e:move_to(hostile)",
          "e:move_to(nil, {path = hostile})",
          "e:jump({x = 3.5, z = 5.5}, hostile)",
          "e:place(hostile)",
          "e:face(hostile)",
          "e:drive(hostile)",
          "e:light(hostile)",
          "e:light{color = hostile}",
          "e:emitter(hostile)",
          "e:collider(hostile)",
          "e:audio(hostile)",
          "e:platform(hostile)",
          "e:platform{size = hostile}",
          "e:mesh(hostile)",
          "e:attach(e, hostile)",
          "e:can_occupy(hostile)",
          "w:camera().target = hostile",
          "w:camera().shake = hostile"}) {
        f.untouched(code);
    }
    CHECK(f.lua.console("e:valid()") == "true");
}

TEST_CASE("world bindings never run metamethods of the tables they read") {
    HostileFixture f;
    for (const char* code : {"w:cell(hostile)",
                             "w:set_cell({x = 1, z = 1}, hostile)",
                             "w:path({x = 1, z = 1}, {x = 2, z = 1}, hostile)",
                             "w:path(hostile, hostile)",
                             "w:reachable({x = 1, z = 1}, 2, {jump = hostile, tag_costs = hostile})",
                             "w:reachable({x = 1, z = 1}, 2, {blocked = hostile})",
                             "w:draw(hostile)",
                             "w:draw{camera = hostile, post = hostile, rect = hostile}",
                             "w:query(hostile)",
                             "w:query{near = hostile}",
                             "w:raycast({x = 1, y = 5, z = 1}, {x = 0, y = -1, z = 0}, hostile)",
                             "w:raycast(hostile, hostile)",
                             "w:overlay('move', hostile, hostile)",
                             "w:overlay_path('p', hostile, hostile)",
                             "w:cursor(hostile, hostile)",
                             "w:burst({x = 1, z = 1}, hostile)",
                             "w:set_environment(hostile)",
                             "w:set_environment{sky = hostile, ambient = hostile}",
                             "w:height_at(hostile)",
                             "w:zones_at(hostile)",
                             "w:area(hostile, 'diamond', 0, 1, hostile)",
                             "w:line_of_sight(hostile, hostile, 0, hostile)"}) {
        f.untouched(code);
    }
    CHECK(f.lua.console("1 + 1") == "2");
}

TEST_CASE("service bindings never run metamethods of the tables they read") {
    HostileFixture f;
    for (const char* code :
         {"fe.audio.music('music/theme.ogg', hostile)", "fe.audio.set_voice(1, hostile)",
          "fe.audio.set_bus_effect('music', hostile)", "fe.audio.duck('music', 0.5, hostile)",
          "fe.screen.set(hostile)", "fe.screen.set{transition = hostile, tint = hostile}",
          "fe.input.define('confirm', hostile)", "fe.input.define_axis('move', hostile)",
          "fe.input.text_begin(hostile)", "fe.audio.sound('a.ogg', hostile)"}) {
        f.untouched(code);
    }
    CHECK(f.lua.console("1 + 1") == "2");
}

TEST_CASE("ui bindings never run metamethods of the tables they read") {
    HostileFixture f;
    for (const char* code :
         {"fe.canvas.text('x', 0, 0, hostile)",
          "fe.canvas.text('x', 0, 0, {color = hostile, shadow = hostile})",
          "fe.canvas.image('a.png', 0, 0, hostile)", "fe.canvas.image('a.png', 0, 0, {src = hostile})",
          "fe.canvas.nine_slice('a.png', 0, 0, 8, 8, hostile)", "fe.canvas.push(hostile)",
          "fe.canvas.rect(0, 0, 1, 1, hostile)", "fe.text.icon('i', hostile)",
          "fe.text.measure('x', hostile)", "fe.text.paginate('x', hostile, 1)", "fe.text.color('c', hostile)",
          "fe.text.font('f', hostile)"}) {
        f.untouched(code);
    }
    CHECK(f.lua.console("1 + 1") == "2");
}

TEST_CASE("angles are degrees") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate(
                   "w = fe.world.load('main', 'arena'); e = w:spawn{position = {x = 2.5, z = 5.5}, yaw = 90}")
                .ok);
    CHECK(lua.console("math.floor(e.yaw + 0.5)") == "90");
    lua.evaluate("e.id = 'turned'; e.yaw = 180");
    const auto world = engine.world("main");
    CHECK(world->registry().get<Transform>(world->find("turned")).yaw == doctest::Approx(glm::pi<float>()));
    // A quarter turn maps the rectangle's +x edge onto +y (clockwise on screen).
    lua.evaluate("fe.canvas.push{rotation = 90}; fe.canvas.rect(0, 0, 10, 1); fe.canvas.pop()");
    REQUIRE_FALSE(engine.canvas().quads().empty());
    const auto& corners = engine.canvas().quads().back().corners;
    CHECK(corners[1].x == doctest::Approx(0).epsilon(1e-4));
    CHECK(corners[1].y == doctest::Approx(10));
    raises(lua, "e.yaw = 0/0", "finite");
}

TEST_CASE("grid cells are tables with integer coordinates") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    lua.evaluate("w = fe.world.load('main', 'arena')");
    CHECK(lua.console("w:cell({x = 2, z = 5}).layer") == "0");
    CHECK(lua.console("w:cell({2, 5, 0}).x") == "2");
    CHECK(lua.console("w:cell({x = -1, z = 0})") == "nil");
    CHECK(lua.evaluate("w:set_cell({x = 2, z = 5}, {add_tags = {'ice'}})").ok);
    CHECK(lua.console("w:cell({x = 2, z = 5}).tags[1]") == "ice");
    CHECK(lua.console("type(w:height_at({x = 2.5, z = 5.5}))") == "number");
    raises(lua, "w:cell(2, 5)", "Expected a grid cell");
    raises(lua, "w:cell({x = 2.5, z = 5})", "must be an integer");
    raises(lua, "w:cell({x = 2})", "needs 'z'");
    raises(lua, "w:set_cell({x = 999, z = 0}, {walkable = false})", "outside the grid");
    // The start cell has no parent; every other cell's parent is a list index.
    CHECK(lua.console("local r = w:reachable({x = 2, z = 5}, 2) return r[1].parent, r[2].parent") ==
          "nil\t1");
    CHECK(lua.console("local r = w:reachable({x = 2, z = 5}, 3) local c = r[#r] local n = 0 "
                      "while c.parent do c = r[c.parent] n = n + 1 end return c.x, c.z, n > 0") ==
          "2\t5\ttrue");
}

TEST_CASE("getters and setters come in pairs") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    lua.evaluate("fe.app.set_title('Renamed')");
    CHECK(lua.console("fe.app.title()") == "Renamed");
    lua.evaluate("fe.app.set_fullscreen('exclusive')");
    CHECK(lua.console("fe.app.fullscreen()") == "exclusive");
    lua.evaluate("fe.audio.set_bus_volume('music', 0.5)");
    CHECK(lua.console("fe.audio.bus_volume('music')") == "0.5");
    lua.evaluate("fe.audio.set_bus_effect('music', {lowpass = 800})");
    CHECK(lua.console("fe.audio.bus_effect('music').lowpass") == "800.0");
    lua.evaluate("fe.audio.set_paused(true)");
    CHECK(lua.console("fe.audio.paused()") == "true");
    lua.evaluate("fe.audio.music('music/theme.ogg'); fe.audio.set_music_volume('main', 0.25); "
                 "fe.audio.set_music_pitch('main', 2)");
    CHECK(lua.console("fe.audio.music_volume(), fe.audio.music_pitch('main')") == "0.25\t2.0");
    // The old get-or-set spellings raise instead of silently only reading.
    raises(lua, "fe.audio.bus_volume('music', 1)", "set_bus_volume");
    raises(lua, "fe.app.fullscreen(true)", "set_fullscreen");
    raises(lua, "fe.audio.music_volume('main', 1)", "set_music_volume");
    raises(lua, "fe.audio.bus_effect('music', {lowpass = 1})", "set_bus_effect");
}

TEST_CASE("bindings reject wrong types, missing components and out-of-range values") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    lua.evaluate("fe.input.define('confirm', {'key:Z'}); w = fe.world.load('main', 'arena'); "
                 "e = w:spawn{position = {x = 2.5, z = 5.5}}");
    // Optional arguments of the wrong type are errors, not defaults.
    raises(lua, "e:place({x = 2.5, z = 5.5}, 'two')", "layer must be an integer");
    raises(lua, "fe.canvas.text('x', 0, 0, 'red')", "text options must be a table");
    raises(lua, "fe.audio.stop_music(3)", "layer must be a string");
    raises(lua, "e:sprite{visible = 0}", "Field 'visible' must be a boolean");
    raises(lua, "e:mover{speed = 'fast'}", "Field 'speed' must be a finite number");
    raises(lua, "e:sprite{direction = 1.5}", "Field 'direction' must be an integer");
    raises(lua, "e:light(true)", "light settings must be a table");
    // A table with no components is not a position (an options table in the wrong place).
    raises(lua, "e:move_to{path = {{x = 1, z = 1}}}", "A position needs 'x'");
    raises(lua, "e:place{y = 1}", "A position needs 'x'");
    raises(lua, "e:attach(w:spawn{position = {x = 3.5, z = 5.5}}, {}) ", "none of x, y, z");
    raises(lua, "e:drive(1, 0)", "Expected an {x, z} table");
    raises(lua, "e:place({x = 0/0, z = 1})", "finite");
    raises(lua, "fe.canvas.rect(0, 0, 1, 1, {})", "none of r, g, b, a");
    CHECK(lua.evaluate("e:move_to(nil, {path = {{x = 3, z = 5}}})").ok);
    CHECK(lua.evaluate("e:attach(w:spawn{position = {x = 3.5, z = 5.5}}, {y = 1})").ok);
    CHECK(lua.evaluate("e:drive({x = 1, z = 0})").ok);
    // Unknown names.
    raises(lua, "fe.input.pressed('confrim')", "Unknown input action 'confrim'");
    raises(lua, "fe.input.axis('nope')", "Unknown input axis 'nope'");
    raises(lua, "fe.screen.set{transition = {kind = 'spin'}}", "Unknown transition kind");
    raises(lua, "w:area({x = 2, z = 5}, 'blob', 0, 1)", "Unknown area shape");
    // Ranges.
    raises(lua, "e:mover{radius = 0.6}", "mover radius must be from 0 to 0.49");
    raises(lua, "w:area({x = 2, z = 5}, 'diamond', 3, 1)", "area max");
    raises(lua, "fe.audio.duck('music', 2)", "amount must be from 0 to 1");
    raises(lua, "fe.input.rumble(2, 0, 1)", "low must be from 0 to 1");
    raises(lua, "w:camera().fov = 500", "fov must be from 1 to 170");
    raises(lua, "fe.random.new(1):int(6, 1)", "low must not be above high");
    raises(lua, "fe.text.resolver(5)", "resolver must be a function or nil");
    CHECK(lua.evaluate("fe.input.pressed('confirm')").ok);
}

TEST_CASE("every entity component is added by a table or no argument and removed by nil") {
    TestGame game("return {}");
    write(game.root / "loop.ogg", "");
    Engine engine(engineOptions(game.root));
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); "
                         "e = w:spawn{id = 'box', position = {x = 2.5, z = 5.5}}")
                .ok);
    auto& world = *engine.world("main");
    const auto box = world.find("box");
    auto& registry = world.registry();
    const auto has = [&](const std::string& component) {
        if (component == "sprite")
            return registry.all_of<Sprite>(box);
        if (component == "mesh")
            return registry.all_of<MeshInstance>(box);
        if (component == "mover")
            return registry.all_of<Mover>(box);
        if (component == "light")
            return registry.all_of<LightSource>(box);
        if (component == "emitter")
            return registry.all_of<Emitter>(box);
        if (component == "collider")
            return registry.all_of<Collider>(box);
        if (component == "audio")
            return registry.all_of<AudioEmitter>(box);
        return registry.all_of<Platform>(box);
    };
    for (const std::string component :
         {"sprite", "mesh", "mover", "light", "emitter", "collider", "platform", "audio"}) {
        CAPTURE(component);
        const std::string settings = component == "audio" ? "{sound = 'loop.ogg'}" : "{}";
        CHECK(lua.evaluate("e:" + component + "(" + settings + ")").ok);
        CHECK(has(component));
        CHECK(lua.evaluate("e:" + component + "(nil)").ok);
        CHECK_FALSE(has(component));
        if (component != "audio") {
            CHECK(lua.evaluate("e:" + component + "()").ok);
            CHECK(has(component));
            CHECK(lua.evaluate("e:" + component + "(nil)").ok);
        }
        CHECK_FALSE(lua.evaluate("e:" + component + "(true)").ok);
        CHECK_FALSE(has(component));
    }
    // A failed audio change leaves no half-made component.
    CHECK_FALSE(lua.evaluate("e:audio()").ok);
    CHECK_FALSE(has("audio"));
    CHECK(lua.console("e:sprite{visible = false}.visible, e:sprite().visible") == "false\tfalse");
    CHECK(lua.console("e:sprite(nil), e:sprite(nil)") == "nil\tnil");
    // Emitters pause with a `paused` field; booleans are no longer settings.
    CHECK(lua.evaluate("e:emitter{rate = 7}").ok);
    CHECK(lua.evaluate("e:emitter{paused = true}").ok);
    CHECK_FALSE(registry.get<Emitter>(box).active);
    CHECK(registry.get<Emitter>(box).settings.rate == doctest::Approx(7));
    CHECK(lua.evaluate("e:emitter{paused = false}").ok);
    CHECK(registry.get<Emitter>(box).active);
    CHECK(registry.get<Emitter>(box).settings.rate == doctest::Approx(7));
    CHECK(lua.evaluate("e:emitter{rate = 3, paused = true}").ok);
    CHECK_FALSE(registry.get<Emitter>(box).active);
    CHECK(registry.get<Emitter>(box).settings.rate == doctest::Approx(3));
    CHECK_FALSE(lua.evaluate("e:emitter(false)").ok);
    CHECK_FALSE(lua.evaluate("e:emitter{paused = 1}").ok);
}

TEST_CASE("path arrows and the cursor take an options table") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena')").ok);
    auto& world = *engine.world("main");
    CHECK(lua.evaluate("w:overlay_path('p', {{x = 1, z = 1}, {x = 2, z = 1}}, {color = 0xff0000ff})").ok);
    CHECK(world.overlayPaths().at("p").color == glm::vec4(1, 0, 0, 1));
    CHECK(lua.evaluate("w:overlay_path('p', {{x = 1, z = 1}})").ok);
    CHECK(world.overlayPaths().at("p").color == glm::vec4(1, 0, 0, 1));
    CHECK(lua.evaluate("w:cursor({x = 1, z = 2}, {color = 'red'})").ok);
    CHECK(world.cursor().visible);
    CHECK(world.cursor().color.r == doctest::Approx(1));
    CHECK(lua.evaluate("w:cursor(nil)").ok);
    CHECK_FALSE(world.cursor().visible);
    raises(lua, "w:overlay_path('p', {}, 0xff0000ff)", "overlay_path options must be a table");
    raises(lua, "w:cursor({x = 1, z = 2}, 'red')", "cursor options must be a table");
    raises(lua, "w:cursor({x = 1, z = 2}, {color = {0/0, 1, 1}})", "finite");
}

TEST_CASE("image and sprite queries raise errors for missing files") {
    TestGame game("return {}");
    std::filesystem::copy_file(std::filesystem::path(FENGINE_LUALIB) / ".." / "samples" / "vale" / "sprites" /
                                   "slime.anim.json",
                               game.root / "slime.anim.json");
    std::filesystem::copy_file(std::filesystem::path(FENGINE_LUALIB) / ".." / "samples" / "vale" / "sprites" /
                                   "slime.png",
                               game.root / "slime.png");
    Engine engine(engineOptions(game.root));
    auto& lua = engine.lua();
    CHECK(lua.console("fe.app.image_size('@fe/assets/ui/cursor.png') > 0") == "true");
    CHECK(lua.console("fe.app.sprite_clips('slime.anim.json').idle.loop") == "true");
    CHECK(lua.console("next(fe.app.sprite_clips(''))") == "nil");
    raises(lua, "fe.app.image_size('missing.png')", "missing.png");
    raises(lua, "fe.app.image_size('rt:minimap')", "Render target 'minimap' does not exist");
    raises(lua, "fe.app.sprite_clips('missing.anim.json')", "missing.anim.json");
}

TEST_CASE("colour components must be finite numbers") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    for (const char* color :
         {"{0/0, 1, 1, 1}", "{r = 1, g = 1/0, b = 1}", "{r = 1, g = 1, b = 1, a = -(0/0)}"}) {
        CAPTURE(color);
        raises(lua, std::string("fe.canvas.rect(0, 0, 1, 1, ") + color + ")", "finite");
        raises(lua, std::string("fe.screen.set{tint = ") + color + "}", "finite");
    }
    CHECK(lua.evaluate("fe.canvas.rect(0, 0, 1, 1, {1, 0.5, 0, 1})").ok);
}

// --- Reentrancy, finalizers and shutdown (final-probes/sb/gcases) ---------

TEST_CASE("option tables that would unload the world or destroy the entity are read raw") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(
        lua.evaluate("w = fe.world.load('rw', 'arena'); e = w:spawn{position = {x = 1, z = 1}}; "
                     "others = {} for i = 1, 5 do others[i] = w:spawn{id = 'o' .. i, position = {x = 2, z = "
                     "2}, yaw = 0} end; ran = 0; "
                     "function wrecking(action) return setmetatable({position = {x = 1, z = 1}}, {__index "
                     "= function() ran = ran + 1; action() return 3 end}) end; "
                     "destroy = function() e:destroy() end; unload = function() fe.world.unload('rw') end")
            .ok);
    for (const char* code :
         {"e:configure(wrecking(destroy))", "e:configure(wrecking(unload))", "w:spawn(wrecking(unload))",
          "e:move_to({x = 3, z = 3}, wrecking(unload))", "e:place(wrecking(unload))",
          "w:raycast({x = 1, y = 5, z = 1}, {x = 0, y = -1, z = 0}, wrecking(unload))",
          "e:light(wrecking(destroy))", "e:sprite(wrecking(destroy))", "w:spawn{position = wrecking(unload)}",
          "w:path({x = 1, z = 1}, {x = 3, z = 3}, wrecking(unload))",
          "e:jump({x = 2, z = 2}, wrecking(unload))", "w:draw{camera = wrecking(unload)}",
          "fe.canvas.text('x', 0, 0, wrecking(unload))"}) {
        CAPTURE(code);
        lua.evaluate(std::string("pcall(function() ") + code + " end)");
    }
    CHECK(lua.console("ran") == "0");
    CHECK(lua.console("e:valid()") == "true");
    CHECK(lua.console("fe.world.get('rw') == w") == "true");
    CHECK(lua.console("others[5].yaw + others[1].yaw") == "0.0");
}

TEST_CASE("bindings hold the collector while their C++ runs; callouts release it") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    ModuleBinder(lua, engine.docs(), "fe.probe", "")
        .fn(
            "collecting", [](sol::this_state s) { return lua_gc(s, LUA_GCISRUNNING) == 1; }, "", "")
        .fn("call", [&lua](sol::object fn) { lua.call(fn); }, "", "");
    lua.lua()["raw_collecting"] = [](sol::this_state s) {
        return lua_gc(s, LUA_GCISRUNNING) == 1;
    };
    CHECK(lua.console("raw_collecting()") == "true");
    CHECK(lua.console("fe.probe.collecting()") == "false");
    CHECK(lua.console("local r; fe.probe.call(function() r = {raw_collecting(), fe.probe.collecting()} end) "
                      "return r[1], r[2]") == "true\tfalse");
    CHECK(lua.console("pcall(fe.probe.collecting, 'x', 'y') and raw_collecting()") == "true");
    CHECK(lua.console("pcall(fe.world.load, {}) or raw_collecting()") == "true");
}

TEST_CASE("finalizers never run in the middle of a binding") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    bool inside = false;
    int ran = 0, ranInside = 0;
    ModuleBinder(lua, engine.docs(), "fe.probe", "")
        .fn(
            "allocate",
            [&inside](sol::this_state s, int count) {
                inside = true;
                for (int i = 0; i < count; ++i) {
                    lua_createtable(s, 0, 8);
                    lua_pop(s, 1);
                }
                inside = false;
            },
            "", "");
    lua.lua()["finalized"] = [&] {
        ++ran;
        ranInside += inside ? 1 : 0;
    };
    REQUIRE(lua.evaluate("for i = 1, 20000 do setmetatable({}, {__gc = function() finalized() end}) end "
                         "for i = 1, 20 do fe.probe.allocate(20000) end "
                         "for i = 1, 100 do collectgarbage('step') end")
                .ok);
    CHECK(ran > 0);
    CHECK(ranInside == 0);
    // Finalizers that wreck the world between binding calls are harmless.
    REQUIRE(lua.evaluate("w = fe.world.load('fw', 'arena') "
                         "for i = 1, 2000 do setmetatable({}, {__gc = function() fe.world.unload('fw') end}) "
                         "pcall(w.spawn, w, {position = {x = 1, z = 1}, tags = {'a', 'b'}, sprite = {tint = "
                         "{1, 1, 1, 1}, offset = {x = 1, y = 2}}, props = {a = {b = {c = i}}}}) end "
                         "for i = 1, 100 do collectgarbage('step') end")
                .ok);
    CHECK(lua.console("fe.world.get('fw')") == "nil");
}

TEST_CASE("finalizers that run while the engine shuts down cannot use it") {
    auto options = engineOptions();
    const auto late = options.userData / "saves" / "late.json";
    std::filesystem::remove(late);
    std::vector<std::string> notes;
    {
        Engine engine(std::move(options));
        auto& lua = engine.lua();
        lua.lua()["note"] = [&notes](sol::object ok, sol::object message) {
            notes.push_back(message.get_type() == sol::type::string ? message.as<std::string>()
                            : ok.as<bool>()                         ? "ok"
                                                                    : "?");
        };
        REQUIRE(
            lua.evaluate(
                   "w = fe.world.load('main', 'arena'); e = w:spawn{position = {x = 1, z = 1}}; e:data().x = "
                   "1 "
                   "keep = {"
                   "  setmetatable({}, {__gc = function() note(pcall(fe.world.load, 'late', 'arena')) end}),"
                   "  setmetatable({}, {__gc = function() note(pcall(w.spawn, w, {position = {x = 1, z = "
                   "1}})) end}),"
                   "  setmetatable({}, {__gc = function() note(pcall(e.data, e)) end}),"
                   "  setmetatable({}, {__gc = function() note(pcall(fe.save.write, 'late', {x = 1})) end}),"
                   "  setmetatable({}, {__gc = function() note(pcall(fe.text.resolver, print)) end}),"
                   "  setmetatable({}, {__gc = function() note(pcall(require, 'fe.signal')) end}),"
                   "} "
                   "local function zombie() return setmetatable({}, {__gc = function(o) resurrected = o; "
                   "note(pcall(fe.world.load, 'z', 'arena')); zombie() end}) end zombie()")
                .ok);
    }
    REQUIRE(notes.size() >= 7);
    for (const auto& note : notes) {
        CHECK(note.find("engine is shutting down") != std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(late));
}

TEST_CASE("a text resolver may replace or remove itself while it runs") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(
        lua.evaluate("fe.text.resolver(function(tag) fe.text.resolver(nil); for i = 1, 1000 do local _ = {i, "
                     "('x'):rep(i)} end; collectgarbage('step'); return 'P' end)")
            .ok);
    CHECK(lua.console("fe.text.plain('hi {player} {other}')") == "hi P {other}");
    REQUIRE(lua.evaluate("fe.text.resolver(function(tag) fe.text.resolver(function() return 'z' end); "
                         "error('boom') end)")
                .ok);
    CHECK(lua.console("fe.text.plain('{x} {y} {z}')") == "{x} z z");
    REQUIRE(lua.errors().size() == 1);
    CHECK(lua.errors().back().message.find("boom") != std::string::npos);
}

TEST_CASE("text resolvers nest at most 8 calls deep and stop with one error") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("depth = 0; fe.text.resolver(function(tag) depth = depth + 1; "
                         "return fe.text.plain('{' .. tag .. '}') end)")
                .ok);
    const auto result = lua.evaluate("return fe.text.measure('{loop} {again}')");
    CHECK_FALSE(result.ok);
    CHECK(result.output.find("nests more than 8") != std::string::npos);
    CHECK(lua.console("depth") == "8");
    CHECK(lua.errors().empty());
    // A resolver that resolves other tags (not itself) still works, and so do later calls.
    REQUIRE(lua.evaluate("fe.text.resolver(function(tag) if tag == 'name' then return 'Cora' end "
                         "if tag == 'greeting' then return fe.text.plain('Hi {name}') end end)")
                .ok);
    CHECK(lua.console("fe.text.plain('{greeting}!')") == "Hi Cora!");
}

TEST_CASE("the watchdog stops a runaway text resolver") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    lua.setWatchdog(0.2);
    REQUIRE(
        lua.evaluate("calls = 0; fe.text.resolver(function(tag) calls = calls + 1; while true do end end)")
            .ok);
    lua.beginFrame();
    const auto result = lua.evaluate("return fe.text.plain('{x} {y} {z}')");
    CHECK_FALSE(result.ok);
    CHECK(result.output.find("watchdog") != std::string::npos);
    CHECK(lua.console("calls") == "1");
    CHECK(lua.errors().empty());
}

TEST_CASE("a path cost function may change the world being searched") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(
        lua.evaluate("w = fe.world.load('cw', 'arena'); e = w:spawn{position = {x = 1, z = 1}}; calls = 0 "
                     "route = w:path({x = 1, z = 1}, {x = 4, z = 4}, {cost = function(a, b, base) "
                     "  calls = calls + 1 "
                     "  if calls == 1 then e:destroy(); w:set_cell({x = 3, z = 3}, {height = 1}); "
                     "    inner = #w:path({x = 1, z = 1}, {x = 2, z = 2}); fe.world.unload('cw'); "
                     "    collectgarbage('step') end "
                     "  return base end})")
            .ok);
    CHECK(lua.console("#route > 0 and inner > 0") == "true");
    CHECK(lua.console("fe.world.get('cw')") == "nil");
    CHECK(lua.console("(pcall(w.path, w, {x = 1, z = 1}, {x = 2, z = 2}))") == "false");
}

TEST_CASE("lights, emitters and the environment take the documented vector and colour forms") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); "
                         "lamp = w:spawn{id = 'lamp', position = {x = 1, z = 1}, light = {color = '#ff8000', "
                         "position = {y = 3}, direction = {x = 1, y = -1}}, emitter = {color = '#ff000080', "
                         "color_end = {r = 0, g = 0, b = 1}, velocity = {y = 2}, area = {1, 0, 1}}}")
                .ok);
    auto world = engine.world("main");
    REQUIRE(world);
    auto& registry = world->registry();
    const auto lamp = world->find("lamp");
    const auto& light = registry.get<LightSource>(lamp).light;
    CHECK(light.color.r == doctest::Approx(1));
    CHECK(light.color.g == doctest::Approx(128.0 / 255));
    CHECK(light.position.y == doctest::Approx(3));
    CHECK(light.position.x == doctest::Approx(0));
    CHECK(light.direction.x == doctest::Approx(1));
    const auto& emitter = registry.get<Emitter>(lamp).settings;
    CHECK(emitter.color.a == doctest::Approx(128.0 / 255));
    CHECK(emitter.colorEnd.b == doctest::Approx(1));
    CHECK(emitter.colorEnd.a == doctest::Approx(1));
    CHECK(emitter.velocity.y == doctest::Approx(2));
    CHECK(emitter.area.z == doctest::Approx(1));
    REQUIRE(
        lua.evaluate(
               "lamp:light{color = 0x00ff00ff, position = {0, 2, 0}}; "
               "w:burst({x = 1, z = 1}, {color = 'white', gravity = {y = -9}}, 4); "
               "w:set_environment{ambient = '#336699', sun_direction = {x = -1, y = -1}, "
               "sky = {top = {r = 0.1, g = 0.2, b = 0.3}, backdrops = {{image = '', tint = '#ffffff80'}}}, "
               "weather = {wind = {x = 2}, color = 0xffffff40}}")
            .ok);
    CHECK(registry.get<LightSource>(lamp).light.color.g == doctest::Approx(1));
    const auto& environment = world->environment();
    CHECK(environment.ambient.b == doctest::Approx(0x99 / 255.0));
    CHECK(environment.sunDirection.z == doctest::Approx(0));
    CHECK(environment.sky.top.g == doctest::Approx(0.2));
    REQUIRE(environment.sky.backdrops.size() == 1);
    CHECK(environment.sky.backdrops[0].tint.a == doctest::Approx(128.0 / 255));
    CHECK(environment.weather.wind.x == doctest::Approx(2));
    CHECK(environment.weather.color.a == doctest::Approx(0x40 / 255.0));
    raises(lua, "lamp:light{color = 'not a colour'}", "Unknown color");
    raises(lua, "lamp:emitter{velocity = {x = 'fast'}}", "Component 'x'");
}

TEST_CASE("engine objects and argument errors use the documented type names") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); e = w:spawn{position = {x = 1, z = 1}}").ok);
    CHECK(lua.console("tostring(fe.random.new(1)):match('^RandomStream: ') ~= nil") == "true");
    CHECK(lua.console("tostring(w:camera()):match('^Camera: ') ~= nil") == "true");
    CHECK(lua.console("tostring(w) .. ' ' .. tostring(e)") == "World(main) Entity(0)");
    // sol2 would otherwise print every error to stderr as well.
    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    const std::pair<const char*, const char*> cases[] = {
        {"w.spawn(e, {})", "bad argument #1 to 'World:spawn' (World expected, got Entity)"},
        {"e.destroy(w)", "bad argument #1 to 'Entity:destroy' (Entity expected, got World)"},
        {"e:attach(w)", "bad argument #2 to 'Entity:attach' (Entity expected, got World)"},
        {"fe.random.new(1).float()",
         "bad argument #1 to 'RandomStream:float' (RandomStream expected, got no value)"},
        {"fe.world.load({}, 'arena')", "bad argument #1 to 'fe.world.load' (string expected, got table)"},
        {"fe.app.progress(1.5)",
         "bad argument #1 to 'fe.app.progress' (integer expected, got the number 1.5)"},
        {"w.paused = 1", "paused must be a boolean"},
        {"w:camera().yaw = 'x'", "yaw must be a finite number"},
        {"fe.world.load('x', 'no_such_map')", "no_such_map"},
    };
    for (const auto& [code, message] : cases) {
        const auto result = lua.evaluate(code);
        CAPTURE(code);
        CAPTURE(result.output);
        CHECK_FALSE(result.ok);
        CHECK(result.output.find(message) != std::string::npos);
        CHECK(result.output.find("sol.") == std::string::npos);
        CHECK(result.output.find("fe::") == std::string::npos);
    }
    std::cerr.rdbuf(previous);
    CHECK(captured.str().empty());
}

namespace {
// Closures of one template share a printed type name on some compilers; sol2
// finds the destructor of a function object it stores by that name.
template<class T> auto capturing(T value) {
    return [value](sol::this_state) {
        return static_cast<int>(sizeof(value));
    };
}
} // namespace

TEST_CASE("engines with bindings that capture objects build and shut down cleanly, repeatedly") {
    for (int round = 0; round < 3; ++round) {
        CAPTURE(round);
        Engine engine(engineOptions());
        auto& lua = engine.lua();
        ModuleBinder(lua, engine.docs(), "fe.probe", "")
            .fn("path", capturing(std::filesystem::path("music/a/b.ogg")), "", "")
            .fn("names", capturing(std::vector<std::string>{"x", std::string(64, 'y')}), "", "")
            .fn("text", capturing(std::string(100, 'z')), "", "");
        CHECK(lua.console("fe.probe.path() > 0 and fe.probe.names() > 0 and fe.probe.text() > 0") == "true");
        REQUIRE(lua.evaluate("fe.text.resolver(function(tag) return tag end) "
                             "w = fe.world.load('main', 'arena'); e = w:spawn{position = {x = 1, z = 1}}; "
                             "e:data().x = 1; fe.audio.music('music/theme.ogg') "
                             "keep = setmetatable({}, {__gc = function() pcall(fe.probe.path) "
                             "pcall(fe.audio.music, 'music/theme.ogg') pcall(fe.text.plain, '{x}') end})")
                    .ok);
    }
}

TEST_CASE("callbacks may raise any error object") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); "
                         "evil = setmetatable({}, {__tostring = function() return 'evil object' end}) "
                         "broken = setmetatable({}, {__tostring = function() error('no text') end})")
                .ok);
    for (const char* error : {"evil", "broken", "nil", "{}", "42"}) {
        CAPTURE(error);
        lua.clearErrors();
        REQUIRE(lua.evaluate(std::string("fe.text.resolver(function() error(") + error + ") end)").ok);
        CHECK(lua.console("fe.text.plain('{q}')") == "{q}");
        CHECK(lua.errors().size() == 1);
        const auto result = lua.evaluate(
            std::string("return w:path({x = 1, z = 1}, {x = 3, z = 3}, {cost = function() error(") + error +
            ") end})");
        CHECK_FALSE(result.ok);
        CHECK(result.output.find("path cost function") != std::string::npos);
    }
    REQUIRE(lua.evaluate("fe.text.resolver(nil)").ok);
    CHECK(lua.console("#w:path({x = 1, z = 1}, {x = 3, z = 3}) > 0") == "true");
}

TEST_CASE("fe.save reports a save read from its backup and refuses data it could not read back") {
    auto options = engineOptions();
    const auto saves = options.userData / "saves";
    std::filesystem::remove_all(saves);
    Engine engine(std::move(options));
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("fe.save.write('slot1', {gold = 1}); fe.save.write('slot1', {gold = 2})").ok);
    // final-probes/r2/savestale.json: 128 levels below the data table (129 in the file).
    raises(lua,
           "local t = {gold = 999} local c = t for i = 1, 128 do c.x = {} c = c.x end "
           "fe.save.write('slot1', t)",
           "nests deeper than 127");
    CHECK(lua.console("local data, meta, backup = fe.save.read('slot1') return data.gold, backup") ==
          "2\tnil");
    write(saves / "slot1.json", "{\"format\": 1, \"data\": {\"gold\": ");
    CHECK(lua.console("local data, meta, backup = fe.save.read('slot1') return data.gold, backup") ==
          "1\ttrue");
    CHECK(lua.console("fe.save.list()[1].backup") == "true");
    write(saves / "slot1.json", "{\"format\": 1, \"data\": {\"gold\": 3}} junk");
    raises(lua, "fe.save.read('slot1')", "slot1.json");
}

TEST_CASE("an entity handle never names a later entity that reuses its slot") {
    // The reviewer's probe: entt wraps an entity slot's 12-bit version after
    // 4095 reuses, and the old handle then named (and moved) the new entity.
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); x = w:spawn{position = {x = 1, z = 1}}; "
                         "key = x.key; x:destroy(); "
                         "for i = 1, 4095 do y = w:spawn{position = {x = 2, z = 2}} "
                         "if i < 4095 then y:destroy() end end")
                .ok);
    const auto x = lua.lua()["x"].get<EntityRef>();
    const auto y = lua.lua()["y"].get<EntityRef>();
    CHECK(x.id == y.id); // the same slot, with the version wrapped around
    CHECK(x.serial != y.serial);
    CHECK_FALSE(x.valid());
    CHECK(y.valid());
    CHECK(lua.console("x:valid(), x == y, y.key ~= key, x.key == key, tostring(x)") ==
          "false\tfalse\ttrue\ttrue\tEntity(destroyed)");
    raises(lua, "x:place({x = 7, z = 7})", "This entity no longer exists");
    CHECK(lua.console("y.position.x") == "2.0");
}

TEST_CASE("RandomStream:int spans the whole integer range without overflow") {
    Random a(5), b(5);
    // Spans up to 2^32 values draw exactly as before, so seeded streams replay.
    for (const std::int64_t span :
         {std::int64_t{2}, std::int64_t{6}, std::int64_t{1} << 31, std::int64_t{1} << 32}) {
        const auto value = a.range(-3, -3 + span - 1);
        CHECK(value == -3 + static_cast<std::int64_t>(b.unit() * static_cast<double>(span)));
    }
    constexpr auto Min = std::numeric_limits<std::int64_t>::min();
    constexpr auto Max = std::numeric_limits<std::int64_t>::max();
    bool negative = false, positive = false;
    for (int i = 0; i < 64; ++i) {
        const auto value = a.range(Min, Max);
        negative = negative || value < 0;
        positive = positive || value > 0;
        const auto near = a.range(Max - 1, Max);
        CHECK((near == Max - 1 || near == Max));
        const auto low = a.range(Min, Min + 1);
        CHECK((low == Min || low == Min + 1));
        const auto wide = a.range(-(std::int64_t{1} << 40), std::int64_t{1} << 40);
        CHECK(std::abs(wide) <= std::int64_t{1} << 40);
    }
    CHECK(negative);
    CHECK(positive);
    CHECK(a.range(Max, Max) == Max);
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    CHECK(
        lua.console("local r = fe.random.new(7) return math.type(r:int(math.mininteger, math.maxinteger))") ==
        "integer");
    CHECK(lua.console("local r = fe.random.new(7) local v = r:range(-1.7e308, 1.7e308) "
                      "return v >= -1.7e308 and v < 1.7e308") == "true");
}

TEST_CASE("numeric arguments must fit the binding's parameter type") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    raises(lua, "fe.text.paginate('a b', {width = 1}, math.maxinteger)",
           "bad argument #3 to 'fe.text.paginate' (integer from -2147483648 to 2147483647 expected, got the "
           "number 9223372036854775807)");
    raises(lua, "fe.text.paginate('a b', {width = 1}, (1 << 32) + 1)", "integer from -2147483648");
    raises(lua, "fe.canvas.rect(0, 0, 1e300, 1)",
           "bad argument #3 to 'fe.canvas.rect' (finite number expected, got the number 1e+300)");
    raises(lua, "fe.canvas.rect(0, 0, 1 / 0, 1)", "finite number expected");
    raises(lua, "fe.canvas.rect(0 / 0, 0, 1, 1)", "finite number expected");
    CHECK(lua.evaluate("fe.canvas.rect(0, 0, 3e38, 1); fe.canvas.set_layer(2147483647)").ok);
}

TEST_CASE("the watchdog stops long-running standard library calls") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    lua.setWatchdog(0.2);
    for (const char* code :
         {"string.find(('a'):rep(800), '.-.-.-b')", "('a'):rep(300000):gsub('.-.-.-.-b', '')",
          "local s = ('a'):rep(1 << 22) return s:find(('a'):rep(1 << 21) .. 'b', 1, true)"}) {
        lua.beginFrame();
        const auto start = std::chrono::steady_clock::now();
        const auto result = lua.evaluate(code);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CHECK_FALSE(result.ok);
        CHECK(result.output.find("script watchdog") != std::string::npos);
        CHECK(seconds < 5);
    }
    lua.setWatchdog(0);
    lua.beginFrame();
    raises(lua, "table.move({}, 1, math.maxinteger - 1, 1, {})", "too many elements to move");
    raises(lua, "string.rep('x', (1 << 28) + 1)", "string too large (the limit is 256 MiB)");
    CHECK(lua.console("#string.rep('ab', 1000, ',')") == "2999");
}

TEST_CASE("paths with an embedded NUL are refused") {
    Engine engine(engineOptions());
    auto& lua = engine.lua();
    raises(lua, "fe.app.capture('x.lua\\0.png')", "Path contains a NUL character: x.lua\\0...");
    CHECK(engine.requests().captures.empty());
    raises(lua, "fe.fs.read('main.lua\\0.png')", "NUL");
    raises(lua, "fe.json.read('data/items.json\\0')", "NUL");
    raises(lua, "fe.world.load('w', 'arena\\0')", "Invalid map id");
    CHECK(lua.console("fe.fs.exists('main.lua\\0')") == "false");
    CHECK_THROWS_WITH(resolveInside(FENGINE_TEST_GAME, std::string("main.lua\0.png", 13)),
                      doctest::Contains("NUL"));
}
