#include "engine/script/ScriptEngine.h"
#include "engine/core/Log.h"
#include <sol/sol.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace fe {
namespace {
void budgetHook(lua_State* state, lua_Debug*) { luaL_error(state,"Script exceeded its instruction budget"); }
struct InstructionBudget {
    lua_State* state;
    explicit InstructionBudget(lua_State* s) : state(s) { lua_sethook(state,budgetHook,LUA_MASKCOUNT,200000); }
    ~InstructionBudget() { lua_sethook(state,nullptr,0,0); }
};
Json fromLua(lua_State* state, int index, int depth = 0) {
    if (depth > 16) throw std::runtime_error("Script value nesting limit exceeded");
    index = lua_absindex(state,index);
    switch (lua_type(state,index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return lua_toboolean(state,index) != 0;
    case LUA_TNUMBER: {
        const auto value = lua_tonumber(state,index);
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite script number");
        if (lua_isinteger(state,index)) return lua_tointeger(state,index);
        return value;
    }
    case LUA_TSTRING: {
        std::size_t size = 0; const char* value = lua_tolstring(state,index,&size);
        return std::string(value,size);
    }
    case LUA_TTABLE: {
        const auto length = lua_rawlen(state,index);
        Json out = length > 0 ? Json::array() : Json::object();
        if (length > 4096) throw std::runtime_error("Script table too large");
        if (length > 0) for (std::size_t i = 1; i <= length; ++i) {
            lua_rawgeti(state,index,static_cast<lua_Integer>(i)); out.push_back(fromLua(state,-1,depth+1)); lua_pop(state,1);
        }
        else {
            lua_pushnil(state); int count = 0;
            while (lua_next(state,index) != 0) {
                if (++count > 4096 || lua_type(state,-2) != LUA_TSTRING) { lua_pop(state,2); throw std::runtime_error("Script object needs string keys"); }
                out[lua_tostring(state,-2)] = fromLua(state,-1,depth+1); lua_pop(state,1);
            }
        }
        return out;
    }
    default: throw std::runtime_error("Unsupported script value type");
    }
}
void pushJson(lua_State* state, const Json& value) {
    if (value.is_null()) lua_pushnil(state);
    else if (value.is_boolean()) lua_pushboolean(state,value.get<bool>());
    else if (value.is_number_integer()) lua_pushinteger(state,value.get<lua_Integer>());
    else if (value.is_number()) lua_pushnumber(state,value.get<double>());
    else if (value.is_string()) { const auto& text = value.get_ref<const std::string&>(); lua_pushlstring(state,text.data(),text.size()); }
    else {
        lua_newtable(state);
        if (value.is_array()) for (std::size_t i = 0; i < value.size(); ++i) { pushJson(state,value[i]); lua_rawseti(state,-2,i+1); }
        else for (const auto& [key,item] : value.items()) { pushJson(state,item); lua_setfield(state,-2,key.c_str()); }
    }
}
std::filesystem::path scriptPath(const std::filesystem::path& root, const std::string& relative) {
    const auto path = std::filesystem::weakly_canonical(root / relative);
    const auto rel = path.lexically_relative(root);
    if (rel.empty() || rel.is_absolute() || *rel.begin() == ".." || path.extension() != ".lua")
        throw std::runtime_error("Script path must be a .lua file inside the game folder");
    return path;
}
sol::table battlerTable(sol::state& lua, const Battler& b) {
    auto t = lua.create_table();
    t["id"] = b.id; t["hp"] = b.hp; t["mp"] = b.mp; t["max_hp"] = b.stats.hp; t["max_mp"] = b.stats.mp;
    t["attack"] = b.stats.attack; t["defense"] = b.stats.defense; t["magic"] = b.stats.magic;
    t["resistance"] = b.stats.resistance; t["speed"] = b.stats.speed; t["enemy"] = b.enemy;
    t["level"]=b.level;t["gauge"]=b.gauge;t["skills"]=sol::as_table(b.skills);t["states"]=sol::as_table(b.states);
    return t;
}
}
struct ScriptEngine::Impl {
    struct Task {
        std::uint64_t id = 0;
        lua_State* thread = nullptr;
        int reference = LUA_NOREF;
        sol::environment environment;
        bool blocking = true, done = false;
        double remaining = 0;
        ScriptRequest request;
    };
    std::filesystem::path root;
    GameState& state;
    const Database& database;
    sol::state lua;
    sol::table rules;
    sol::environment rulesEnvironment;
    std::vector<std::unique_ptr<Task>> tasks;
    std::uint64_t nextId = 1;
    Handler handler;
    std::string error;
    bool updating = false;
    int startDepth = 0;
    explicit Impl(std::filesystem::path folder, GameState& s, const Database& db)
        : root(std::filesystem::weakly_canonical(folder)), state(s), database(db) {
        lua.open_libraries(sol::lib::base,sol::lib::math,sol::lib::table,sol::lib::string,sol::lib::coroutine,sol::lib::utf8);
        lua["math"]["randomseed"](0);
        for (const auto* name : {"dofile","loadfile","load","collectgarbage"}) lua[name] = sol::lua_nil;
        lua["string"]["dump"] = sol::lua_nil;
        lua["get_flag"] = [&](const std::string& id) { return state.flag(id); };
        lua["set_flag"] = [&](const std::string& id, sol::optional<bool> value) { state.flags[id] = value.value_or(true); };
        lua["get_var"] = [&](const std::string& id) { return state.variable(id); };
        lua["set_var"] = [&](const std::string& id, double value) {
            if (!std::isfinite(value)) throw std::runtime_error("Variable must be finite"); state.variables[id] = value;
        };
        lua["item_count"] = [&](const std::string& id) { return state.itemCount(id); };
        lua["give_item"] = [&](const std::string& id, sol::optional<int> count) { return state.addItem(database,id,count.value_or(1)); };
        lua["use_item"] = [&](const std::string& item,const std::string& actor) {return state.useItem(database,item,actor);};
        lua["equip"] = [&](const std::string& actor,const std::string& slot,const std::string& item) {return state.equip(database,actor,slot,item);};
        lua["item_name"] = [&](const std::string& id) {return database.items.at(id).name;};
        lua["item_price"] = [&](const std::string& id) {return database.items.at(id).price;};
        lua["gold"] = [&] { return state.gold; };
        lua["add_gold"] = [&](int amount) {
            const auto next = static_cast<std::int64_t>(state.gold) + amount;
            if (next < 0 || next > std::numeric_limits<int>::max()) return false;
            state.gold = static_cast<int>(next); return true;
        };
        lua["recruit"] = [&](const std::string& id) { state.addActor(database,id); };
        lua["party"] = [&] {return sol::as_table(state.party);};
        lua["set_party"] = [&](sol::table members) {
            std::vector<std::string> ids;for (std::size_t i=1;i<=members.size();++i) ids.push_back(members.get<std::string>(i));state.setParty(ids);
        };
        lua["change_job"] = [&](const std::string& id,const std::string& job) {state.changeJob(database,id,job);};
        lua["learn_skill"] = [&](const std::string& id,const std::string& skill) {
            if (!database.skills.contains(skill)) throw std::runtime_error("Unknown skill: "+skill);
            auto& a=state.actor(id);if (std::find(a.skills.begin(),a.skills.end(),skill)==a.skills.end()) a.skills.push_back(skill);
        };
        lua["actor_hp"] = [&](const std::string& id) {return state.actor(id).hp;};
        lua["actor_mp"] = [&](const std::string& id) {return state.actor(id).mp;};
        lua["actor_level"] = [&](const std::string& id) {return state.actor(id).level;};
        lua["actor_stat"] = [&](const std::string& id,const std::string& stat) {return state.stats(database,state.actor(id)).json().at(stat).get<int>();};
        lua["change_hp"] = [&](const std::string& id,int amount,sol::optional<bool> allowKo) {
            auto& a=state.actor(id);const auto max=state.stats(database,a).hp;
            a.hp=static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(a.hp)+amount,allowKo.value_or(false)?0:1,max));
        };
        lua["change_mp"] = [&](const std::string& id,int amount) {
            auto& a=state.actor(id);const auto max=state.stats(database,a).mp;
            a.mp=static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(a.mp)+amount,0,max));
        };
        lua["gain_experience"] = [&](int amount) {state.gainExperience(database,amount);};
        lua["rest_party"] = [&] { state.rest(database); };
        lua["player_name"] = [&] { return state.party.empty() ? std::string("Hero") : database.actors.at(state.party.front()).name; };
        lua["print"] = [](const std::string& message) { log(LogLevel::Info,"Lua: " + message); };
        lua.script(R"(
          function wait(seconds) return coroutine.yield("wait", seconds) end
          function say(speaker, text, portrait) return coroutine.yield("say", speaker, text, portrait) end
          function choice(options) return coroutine.yield("choice", options) end
          function menu(settings) return coroutine.yield("menu", settings) end
          function wait_input(action) return coroutine.yield("input", action or "Confirm") end
          function move(entity, destination) return coroutine.yield("move", entity, destination) end
          function face(entity, direction) return coroutine.yield("face", entity, direction) end
          function animate(entity, clip) return coroutine.yield("animate", entity, clip) end
          function camera(settings) return coroutine.yield("camera", settings) end
          function fade_out(seconds) return coroutine.yield("fade_out", seconds) end
          function fade_in(seconds) return coroutine.yield("fade_in", seconds) end
          function warp(map, spawn) return coroutine.yield("warp", map, spawn) end
          function battle(troop) return coroutine.yield("battle", troop) end
          function shop(id) return coroutine.yield("shop", id) end
          function inn(price) return coroutine.yield("inn", price) end
          function play_bgm(path, options) return coroutine.yield("bgm", path, options) end
          function stop_bgm(seconds) return coroutine.yield("stop_bgm", seconds or 0.5) end
          function play_sfx(path) return coroutine.yield("sfx", path) end
          function save(slot) return coroutine.yield("save", slot) end
          function load_game(slot) return coroutine.yield("load", slot) end
          function screen_fx(settings) return coroutine.yield("screen_fx", settings) end
        )");
    }
    sol::environment environment() {
        sol::environment env(lua,sol::create,lua.globals());
        env["_G"] = env;
        // Each event owns its library tables as well as its script globals.
        for (const auto* name : {"math","string","table","coroutine","utf8"}) {
            auto copy = lua.create_table(); sol::table library = lua[name];
            for (const auto& pair : library) copy[pair.first] = pair.second;
            env[name] = copy;
        }
        return env;
    }
    void fail(Task& task, const std::string& message) {
        error = "Event '" + task.request.event + "': " + message;
        log(LogLevel::Error,error); task.done = true;
    }
    void resume(Task& task, int arguments) {
        lua_sethook(task.thread,budgetHook,LUA_MASKCOUNT,200000);
        int results = 0;
        const int status = lua_resume(task.thread,nullptr,arguments,&results);
        lua_sethook(task.thread,nullptr,0,0);
        if (status == LUA_OK) { task.done = true; lua_settop(task.thread,0); return; }
        if (status != LUA_YIELD) {
            const char* message = lua_tostring(task.thread,-1);
            fail(task,message ? message : "Unknown Lua error"); lua_settop(task.thread,0); return;
        }
        if (results < 1 || !lua_isstring(task.thread,-results)) { fail(task,"Yield must name an engine command"); return; }
        try {
            task.request.command = lua_tostring(task.thread,-results);
            task.request.arguments = Json::array();
            for (int i = -results+1; i <= -1; ++i) task.request.arguments.push_back(fromLua(task.thread,i));
            if (task.request.command == "wait") {
                task.remaining = task.request.arguments.at(0).get<double>();
                if (!std::isfinite(task.remaining) || task.remaining < 0) throw std::runtime_error("Invalid wait duration");
            }
        } catch (const std::exception& e) { fail(task,e.what()); }
        lua_settop(task.thread,0);
    }
    void cleanup() {
        std::erase_if(tasks,[&](const auto& task) {
            if (!task->done) return false;
            lua_resetthread(task->thread); luaL_unref(lua.lua_state(),LUA_REGISTRYINDEX,task->reference); return true;
        });
    }
};
ScriptEngine::ScriptEngine(std::filesystem::path root, GameState& state, const Database& db)
    : impl_(std::make_unique<Impl>(std::move(root),state,db)) {
    impl_->lua["run_event"]=[this](const std::string& file,sol::optional<std::string> event,sol::optional<bool> blocking) {return start(file,event.value_or(""),blocking.value_or(true));};
}
ScriptEngine::~ScriptEngine() { cancelAll(); }
void ScriptEngine::setHandler(Handler handler) { impl_->handler = std::move(handler); }
std::uint64_t ScriptEngine::start(const std::string& file, const std::string& event, bool blocking) {
    auto& p = *impl_;
    if (p.startDepth>=16 || p.tasks.size()>=256) throw std::runtime_error("Event nesting/task limit exceeded");
    struct StartDepth {int& value;explicit StartDepth(int& v):value(v){++value;}~StartDepth(){--value;}} depth(p.startDepth);
    const auto path = scriptPath(p.root,file);
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open script " + path.string());
    std::ostringstream buffer; buffer << stream.rdbuf(); const auto code = buffer.str();
    auto task = std::make_unique<Impl::Task>();
    task->id = p.nextId++; task->blocking = blocking; task->request.task = task->id; task->request.event = event;
    task->thread = lua_newthread(p.lua.lua_state());
    task->reference = luaL_ref(p.lua.lua_state(),LUA_REGISTRYINDEX);
    task->environment = p.environment();
    const auto id = task->id;
    if (luaL_loadbufferx(task->thread,code.data(),code.size(),file.c_str(),"t") != LUA_OK) {
        const auto message = std::string(lua_tostring(task->thread,-1));
        luaL_unref(p.lua.lua_state(),LUA_REGISTRYINDEX,task->reference); throw std::runtime_error(file + ": " + message);
    }
    task->environment.push(task->thread); lua_setupvalue(task->thread,-2,1);
    lua_sethook(task->thread,budgetHook,LUA_MASKCOUNT,200000);
    int results = 0; const int status = lua_resume(task->thread,nullptr,0,&results);
    lua_sethook(task->thread,nullptr,0,0);
    if (status != LUA_OK || results != 1 || !lua_isfunction(task->thread,-1)) {
        const char* message = lua_tostring(task->thread,-1);
        const std::string description = message ? message : "Script must return an event function";
        luaL_unref(p.lua.lua_state(),LUA_REGISTRYINDEX,task->reference); throw std::runtime_error(file + ": " + description);
    }
    lua_newtable(task->thread);
    lua_pushlstring(task->thread,event.data(),event.size()); lua_setfield(task->thread,-2,"self");
    // The convenience method is Lua so yielding never crosses a C++ stack frame.
    p.lua.script("function __fe_face_player(ev) return face(ev.self, 'player') end");
    sol::function facePlayer = p.lua["__fe_face_player"];
    facePlayer.push(task->thread); lua_setfield(task->thread,-2,"face_player");
    p.resume(*task,1);
    p.tasks.push_back(std::move(task));
    if (!p.updating) p.cleanup();
    return id;
}
void ScriptEngine::update(double dt) {
    auto& p = *impl_;
    if (!std::isfinite(dt) || dt < 0 || p.updating) return;
    p.updating = true;
    // Requests may start another task; snapshot IDs keeps iteration stable.
    std::vector<std::uint64_t> ids;
    for (const auto& task : p.tasks) ids.push_back(task->id);
    for (auto id : ids) {
        auto found = std::find_if(p.tasks.begin(),p.tasks.end(),[&](const auto& t) { return t->id == id; });
        if (found == p.tasks.end() || (*found)->done) continue;
        auto& task = **found;
        try {
            if (task.request.command == "wait") {
                task.remaining -= dt;
                if (task.remaining <= 0) p.resume(task,0);
            } else {
                if (!p.handler) throw std::runtime_error("No handler for command " + task.request.command);
                auto response = p.handler(task.request,dt);
                if (response.complete && !task.done) { pushJson(task.thread,response.value); p.resume(task,1); }
            }
        } catch (const std::exception& e) { p.fail(task,e.what()); }
    }
    p.updating = false;
    p.cleanup();
}
void ScriptEngine::cancel(std::uint64_t id) {
    for (auto& task : impl_->tasks) if (task->id == id) task->done = true;
    // Defer reclamation to update: a host command may cancel its own task.
}
void ScriptEngine::cancelAll() {
    for (auto& task : impl_->tasks) task->done = true;
    if (!impl_->updating) impl_->cleanup();
}
void ScriptEngine::cancelAllExcept(std::uint64_t id) {
    for (auto& task:impl_->tasks) if (task->id!=id) task->done=true;
    if (!impl_->updating) impl_->cleanup();
}
bool ScriptEngine::running(std::uint64_t id) const {
    for (const auto& task : impl_->tasks) if (task->id == id && !task->done) return true;
    return false;
}
bool ScriptEngine::blocksField() const { for (const auto& t : impl_->tasks) if (t->blocking && !t->done) return true; return false; }
std::size_t ScriptEngine::taskCount() const { return impl_->tasks.size(); }
void ScriptEngine::loadRules(const std::string& file) {
    auto& p = *impl_;
    auto env = p.environment();
    InstructionBudget budget(p.lua.lua_state());
    auto result = p.lua.safe_script_file(scriptPath(p.root,file).string(),env,sol::script_pass_on_error);
    if (!result.valid()) { sol::error error = result; throw std::runtime_error(error.what()); }
    if (result.get_type() != sol::type::table) throw std::runtime_error("Rules script must return a table");
    sol::table replacement = result;
    p.rules = replacement; p.rulesEnvironment = std::move(env);
}
double ScriptEngine::formula(const std::string& name, const Battler& user, const Battler& target, int power) {
    auto& p = *impl_;
    if (!p.rules.valid()) throw std::runtime_error("No Lua battle rules loaded");
    sol::optional<sol::protected_function> fn = p.rules["formulas"][name];
    if (!fn) throw std::runtime_error("Missing Lua formula " + name);
    InstructionBudget budget(p.lua.lua_state());
    auto result = (*fn)(battlerTable(p.lua,user),battlerTable(p.lua,target),power);
    if (!result.valid()) { sol::error error = result; throw std::runtime_error(error.what()); }
    const auto value = result.get<double>();
    if (!std::isfinite(value)) throw std::runtime_error("Lua formula returned non-finite damage");
    return value;
}
BattleAction ScriptEngine::ai(const Battle& battle, int index) {
    auto& p = *impl_;
    const auto& b = battle.battlers().at(index);
    const auto& name = p.database.enemies.at(b.source).ai;
    if (!p.rules.valid()) throw std::runtime_error("No Lua battle rules loaded");
    sol::optional<sol::protected_function> fn = p.rules["ai"][name];
    if (!fn) return {index,ActionKind::Skill,"",-1};
    auto list = p.lua.create_table();
    for (std::size_t i = 0; i < battle.battlers().size(); ++i) list[i+1] = battlerTable(p.lua,battle.battlers()[i]);
    InstructionBudget budget(p.lua.lua_state());
    auto result = (*fn)(index+1,list);
    if (!result.valid()) { sol::error error = result; throw std::runtime_error(error.what()); }
    sol::table action = result;
    if (action.get_or<std::string>("kind","skill")=="guard") return {index,ActionKind::Guard,"",-1};
    return {index,ActionKind::Skill,action.get<std::string>("skill"),action.get<int>("target")-1};
}
std::vector<EffectDef> ScriptEngine::effect(const std::string& name,const Battler& user,const Battler& target,int power) {
    auto& p=*impl_;
    if (!p.rules.valid()) throw std::runtime_error("No Lua battle rules loaded");
    sol::optional<sol::table> effects=p.rules["effects"];
    if (!effects) throw std::runtime_error("Missing Lua effect table");
    sol::optional<sol::protected_function> function=(*effects)[name];
    if (!function) throw std::runtime_error("Missing Lua effect "+name);
    InstructionBudget budget(p.lua.lua_state());
    auto result=(*function)(battlerTable(p.lua,user),battlerTable(p.lua,target),power);
    if (!result.valid()) {sol::error error=result;throw std::runtime_error(error.what());}
    if (result.return_count()==0 || result.get_type()==sol::type::lua_nil) return {};
    sol::table table=result;table.push(p.lua.lua_state());Json data;
    try {data=fromLua(p.lua.lua_state(),-1);} catch (...) {lua_pop(p.lua.lua_state(),1);throw;}
    lua_pop(p.lua.lua_state(),1);
    if (data.empty()) return {};
    std::vector<EffectDef> out;
    if (data.is_object()) out.push_back(EffectDef::read(data));
    else for (const auto& entry:data) out.push_back(EffectDef::read(entry));
    return out;
}
const std::string& ScriptEngine::lastError() const { return impl_->error; }
bool ScriptEngine::executeConsole(const std::string& code) {
    auto& p = *impl_; auto env = p.environment();
    lua_sethook(p.lua.lua_state(),budgetHook,LUA_MASKCOUNT,200000);
    auto result = p.lua.safe_script(code,env,sol::script_pass_on_error);
    lua_sethook(p.lua.lua_state(),nullptr,0,0);
    if (!result.valid()) { sol::error error = result; p.error = error.what(); log(LogLevel::Error,p.error); return false; }
    return true;
}
}
