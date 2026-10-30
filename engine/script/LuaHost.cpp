#include "engine/script/LuaHost.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <cstring>
#if defined(FENGINE_SANITIZERS) && defined(__linux__)
#include <sys/resource.h>
#endif

// Called by the patched Lua library's long loops (cmake/LuaPatches.cmake).
extern "C" {
extern void (*fe_lua_step_hook)(lua_State* L);
}

namespace fe {
namespace {
#if defined(FENGINE_SANITIZERS) && defined(__linux__)
// Sanitizer builds need a deeper C stack for Lua's 200 nested C calls (see
// CMakeLists.txt). Linux grows the main thread's stack on demand up to
// RLIMIT_STACK, so raising the soft limit before any script runs is enough.
[[maybe_unused]] const bool stackRaised = [] {
    constexpr rlim_t Wanted = rlim_t{256} << 20;
    rlimit limit{};
    if (getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < Wanted) {
        limit.rlim_cur = limit.rlim_max == RLIM_INFINITY ? Wanted : std::min(Wanted, limit.rlim_max);
        setrlimit(RLIMIT_STACK, &limit);
    }
    return true;
}();
#endif

// The registry holds each host's pointer under this key (LuaHost::of). The
// registry is the one lookup both hooks use: the library step hook may run in
// a state the engine did not make, where anything else would be garbage.
const char hostKey = 0;

void raiseWatchdog(lua_State* state, const LuaHost& host) {
    luaL_error(state, "script watchdog: a frame ran longer than %f seconds (infinite loop?)", host.watchdog());
}

// Pattern matching, plain string.find and table.move run in C without
// executing Lua instructions, so the count hook never sees them; the patched
// library calls this every 65536 steps instead.
void libraryStepHook(lua_State* state) {
    const auto* host = LuaHost::of(state);
    if (host && host->watchdogExpired()) {
        raiseWatchdog(state, *host);
    }
}

void watchdogHook(lua_State* state, lua_Debug*) {
    const auto* host = LuaHost::of(state);
    if (host && host->watchdogExpired()) {
        raiseWatchdog(state, *host);
    }
}

int tracebackHandler(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    if (!message) {
        message = luaL_tolstring(state, 1, nullptr);
    }
    luaL_traceback(state, state, message, 1);
    return 1;
}

int toStringThunk(lua_State* state) {
    luaL_tolstring(state, 1, nullptr);
    return 1;
}

void lockMetatable(lua_State* state, int metatable) {
    metatable = lua_absindex(state, metatable);
    lua_pushliteral(state, "__metatable");
    lua_pushboolean(state, 0);
    lua_rawset(state, metatable);
}

// __index of a sealed engine type. Upvalues: readable member names, sol2's
// original __index (the metatable itself or its index function).
int sealedIndex(lua_State* state) {
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushvalue(state, 2);
    if (lua_rawget(state, lua_upvalueindex(1)) == LUA_TNIL) {
        return 1;
    }
    lua_pop(state, 1);
    if (lua_type(state, lua_upvalueindex(2)) == LUA_TTABLE) {
        lua_pushvalue(state, 2);
        lua_rawget(state, lua_upvalueindex(2));
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(2));
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 2);
    lua_call(state, 2, 1);
    return 1;
}

// __newindex of a sealed engine type. Upvalues: readable member names,
// writable member names, sol2's original __newindex, the type's name.
int sealedNewIndex(lua_State* state) {
    const char* type = lua_tostring(state, lua_upvalueindex(4));
    if (lua_type(state, 2) != LUA_TSTRING) {
        return luaL_error(state, "%s fields are named by strings", type);
    }
    lua_pushvalue(state, 2);
    if (lua_rawget(state, lua_upvalueindex(2)) == LUA_TNIL) {
        lua_pushvalue(state, 2);
        if (lua_rawget(state, lua_upvalueindex(1)) != LUA_TNIL) {
            return luaL_error(state, "%s.%s is read-only", type, lua_tostring(state, 2));
        }
        return luaL_error(state, "%s has no field '%s' to assign", type, lua_tostring(state, 2));
    }
    lua_settop(state, 3);
    lua_pushvalue(state, lua_upvalueindex(3));
    lua_insert(state, 1);
    lua_call(state, 3, 0);
    return 0;
}

// __eq of a sealed engine type: values of different types are unequal rather
// than an error from the type's own comparison. Upvalues: the type table every
// metatable of the type shares under __type, sol2's original __eq.
int sealedEquals(lua_State* state) {
    for (int i = 1; i <= 2; ++i) {
        if (!lua_getmetatable(state, i) || lua_getfield(state, -1, "__type") == LUA_TNIL ||
            !lua_rawequal(state, -1, lua_upvalueindex(1))) {
            lua_pushboolean(state, 0);
            return 1;
        }
    }
    lua_settop(state, 2);
    lua_pushvalue(state, lua_upvalueindex(2));
    lua_insert(state, 1);
    lua_call(state, 2, 1);
    return 1;
}

// Pushes a set {name = true} of `names`.
void pushNameSet(lua_State* state, const std::set<std::string>& names) {
    lua_createtable(state, 0, static_cast<int>(names.size()));
    for (const auto& name : names) {
        lua_pushboolean(state, 1);
        lua_setfield(state, -2, name.c_str());
    }
}

// Replaces sol2's __index, __newindex and __eq on one of a type's metatables
// with the sealed versions and drops __pairs (no engine type is a container).
void sealMetatable(lua_State* state, int metatable, int typeTable, int readable, int writable, int name) {
    lua_pushliteral(state, "__index");
    lua_pushvalue(state, readable);
    lua_pushliteral(state, "__index");
    lua_rawget(state, metatable);
    lua_pushcclosure(state, sealedIndex, 2);
    lua_rawset(state, metatable);
    lua_pushliteral(state, "__newindex");
    lua_pushvalue(state, readable);
    lua_pushvalue(state, writable);
    lua_pushliteral(state, "__newindex");
    lua_rawget(state, metatable);
    lua_pushvalue(state, name);
    lua_pushcclosure(state, sealedNewIndex, 4);
    lua_rawset(state, metatable);
    lua_pushliteral(state, "__eq");
    lua_pushvalue(state, typeTable);
    lua_pushliteral(state, "__eq");
    if (lua_rawget(state, metatable) == LUA_TNIL) {
        lua_pop(state, 3);
    } else {
        lua_pushcclosure(state, sealedEquals, 2);
        lua_rawset(state, metatable);
    }
    lua_pushliteral(state, "__pairs");
    lua_pushnil(state);
    lua_rawset(state, metatable);
    // Error messages and the default tostring name the documented type, not
    // sol2's "sol.fe::WorldRef".
    lua_pushliteral(state, "__name");
    lua_pushvalue(state, name);
    lua_rawset(state, metatable);
}

bool isSolMetatable(lua_State* state, int key, int value) {
    return lua_type(state, key) == LUA_TSTRING && lua_type(state, value) == LUA_TTABLE &&
           std::strncmp(lua_tostring(state, key), "sol.", 4) == 0;
}


} // namespace

LuaHost::LuaHost(std::filesystem::path gameRoot, std::filesystem::path libraryRoot, bool development)
    : gameRoot_(std::filesystem::weakly_canonical(gameRoot)),
      libraryRoot_(std::filesystem::weakly_canonical(libraryRoot)), start_(std::chrono::steady_clock::now()),
      frameStart_(start_) {
    lua_ = std::make_unique<sol::state>();
    lua_pushlightuserdata(lua_->lua_state(), this);
    lua_rawsetp(lua_->lua_state(), LUA_REGISTRYINDEX, &hostKey);
    fe_lua_step_hook = libraryStepHook;
    lua_->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                         sol::lib::coroutine, sol::lib::utf8);
    for (const char* name : {"dofile", "loadfile", "load", "loadstring", "collectgarbage", "require"}) {
        (*lua_)[name] = sol::lua_nil;
    }
    (*lua_)["string"]["dump"] = sol::lua_nil;
    (*lua_)["print"] = [](sol::this_state state, sol::variadic_args args) {
        std::string line;
        for (const auto& arg : args) {
            if (!line.empty()) {
                line += '\t';
            }
            line += displayString(state, arg.stack_index());
        }
        log(LogLevel::Info, "lua: " + line);
    };
    lua_->set_function("collectgarbage",
                       [](sol::this_state state, sol::optional<std::string> option) -> double {
                           const auto what = option.value_or("count");
                           if (what == "count") {
                               return lua_gc(state, LUA_GCCOUNT, 0) + lua_gc(state, LUA_GCCOUNTB, 0) / 1024.0;
                           }
                           if (what == "step") {
                               lua_gc(state, LUA_GCSTEP, 0);
                               return 0;
                           }
                           throw std::runtime_error("collectgarbage only supports 'count' and 'step'");
                       });
    lua_gc(lua_->lua_state(), LUA_GCGEN, 0, 0);
    handler_ = sol::make_object(*lua_, tracebackHandler).as<sol::function>();
    fe_ = lua_->create_named_table("fe");
    // The module cache is a registry reference only: scripts cannot reach it
    // (a script that could would split a kit's state in two).
    loaded_ = lua_->create_table();
    lua_->set_function("require", [this](const std::string& name) { return require(name); });
    lua_pushliteral(lua_->lua_state(), "");
    lua_getmetatable(lua_->lua_state(), -1);
    lockMetatable(lua_->lua_state(), -1);
    lua_pop(lua_->lua_state(), 2);
    if (development) {
        watchdog_ = 3.0;
        lua_sethook(lua_->lua_state(), watchdogHook, LUA_MASKCOUNT, 200000);
    }
}

LuaHost::~LuaHost() {
    close();
}

void LuaHost::close() {
    closing_ = true;
    if (!lua_) {
        return;
    }
    lua_sethook(lua_->lua_state(), nullptr, 0, 0);
    for (const auto& release : atClose_) {
        release();
    }
    atClose_.clear();
    fe_ = sol::table();
    loaded_ = sol::table();
    handler_ = sol::function();
    // lua_close runs the pending finalizers; the engine bindings and require
    // they call refuse, so nothing new is created in (or refers into) a state
    // that is going away.
    lua_.reset();
}

LuaHost* LuaHost::of(lua_State* state) {
    if (!lua_checkstack(state, 1)) {
        return nullptr;
    }
    lua_rawgetp(state, LUA_REGISTRYINDEX, &hostKey);
    auto* host = static_cast<LuaHost*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return host;
}

bool LuaHost::watchdogExpired() const {
    return watchdog_ > 0 &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - frameStart_).count() > watchdog_;
}

double LuaHost::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

sol::table LuaHost::module(const std::string& dottedName) {
    sol::table table = lua_->globals();
    std::size_t start = 0;
    while (start <= dottedName.size()) {
        const auto end = dottedName.find('.', start);
        const auto part =
            dottedName.substr(start, end == std::string::npos ? std::string::npos : end - start);
        sol::object existing = table[part];
        if (!existing.valid() || existing.get_type() != sol::type::table) {
            table[part] = lua_->create_table();
        }
        table = table[part];
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return table;
}

LuaHost::TypeMembers& LuaHost::exposeType(const std::string& name, const std::string& metatable) {
    auto& type = types_[name];
    type.metatable = metatable;
    return type;
}

void LuaHost::sealTypes() {
    lua_State* state = lua_->lua_state();
    if (!lua_checkstack(state, 16)) {
        throw std::runtime_error("Lua stack exhausted while sealing engine types");
    }
    for (const auto& [name, type] : types_) {
        if (lua_getfield(state, LUA_REGISTRYINDEX, type.metatable.c_str()) != LUA_TTABLE) {
            throw std::runtime_error("Engine type " + name + " has no metatable to seal");
        }
        // Every metatable of the type (value, pointer, const, unique) shares
        // sol2's type table under __type.
        lua_pushliteral(state, "__type");
        lua_rawget(state, -2);
        const int typeTable = lua_gettop(state);
        pushNameSet(state, type.readable);
        pushNameSet(state, type.writable);
        lua_pushstring(state, name.c_str());
        const int typeName = lua_gettop(state);
        lua_pushnil(state);
        while (lua_next(state, LUA_REGISTRYINDEX) != 0) {
            if (isSolMetatable(state, -2, -1)) {
                lua_pushliteral(state, "__type");
                lua_rawget(state, -2);
                const bool own = lua_rawequal(state, -1, typeTable) != 0;
                lua_pop(state, 1);
                if (own) {
                    sealMetatable(state, lua_gettop(state), typeTable, typeTable + 1, typeTable + 2,
                                  typeName);
                }
            }
            lua_pop(state, 1);
        }
        lua_settop(state, typeTable - 2);
        lua_pushnil(state);
        lua_setglobal(state, name.c_str());
    }
    types_.clear();
    // sol2 keeps each type's storage userdata (with a destructor in an
    // unlocked metatable) and its exception handler in globals named "sol.*".
    // It reads them only while registering types, so they move to the registry.
    lua_newtable(state);
    const int stash = lua_gettop(state);
    lua_pushglobaltable(state);
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING && std::strncmp(lua_tostring(state, -2), "sol.", 4) == 0) {
            lua_pushvalue(state, -2);
            lua_insert(state, -2);
            lua_rawset(state, stash);
            lua_pushvalue(state, -1);
            lua_pushnil(state);
            lua_rawset(state, stash + 1);
        } else {
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, "fe.sol_globals");
    lua_pushnil(state);
    while (lua_next(state, LUA_REGISTRYINDEX) != 0) {
        if (isSolMetatable(state, -2, -1)) {
            lockMetatable(state, -1);
        }
        lua_pop(state, 1);
    }
}

std::optional<std::filesystem::path> LuaHost::resolve(const std::string& name) const {
    if (name.empty() ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") !=
            std::string::npos ||
        name.find("..") != std::string::npos || name.front() == '.' || name.back() == '.') {
        return std::nullopt;
    }
    std::string relative = name;
    std::replace(relative.begin(), relative.end(), '.', '/');
    for (const auto& root : {gameRoot_, libraryRoot_}) {
        for (const auto& candidate : {root / (relative + ".lua"), root / relative / "init.lua"}) {
            if (isFile(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

std::string LuaHost::chunkName(const std::filesystem::path& file) const {
    auto relative = relativeTo(gameRoot_, file);
    if (relative.empty()) {
        relative = "lualib/" + relativeTo(libraryRoot_, file);
    }
    return "@" + relative;
}

sol::object LuaHost::load(const std::string& name, const std::filesystem::path& file) {
    const auto code = readFile(file);
    lua_State* state = lua_->lua_state();
    if (luaL_loadbufferx(state, code.data(), code.size(), chunkName(file).c_str(), "t") != LUA_OK) {
        const std::string message = lua_tostring(state, -1);
        lua_pop(state, 1);
        throw std::runtime_error(message);
    }
    sol::protected_function chunk(sol::stack_reference(state, -1), handler_);
    lua_pop(state, 1);
    const Callout callout(*this);
    auto result = chunk(name);
    if (!result.valid()) {
        const sol::error error = result;
        throw std::runtime_error(error.what());
    }
    sol::object value = result.return_count() > 0 ? result.get<sol::object>() : sol::make_object(*lua_, true);
    if (!value.valid() || value.get_type() == sol::type::lua_nil) {
        value = sol::make_object(*lua_, true);
    }
    return value;
}

sol::object LuaHost::require(const std::string& name) {
    if (closing_) {
        throw std::runtime_error("engine is shutting down");
    }
    const auto cached = loaded_.raw_get<sol::object>(name);
    if (cached.valid() && cached.get_type() != sol::type::lua_nil) {
        return cached;
    }
    if (loading_.contains(name)) {
        throw std::runtime_error("Circular require of module '" + name + "'");
    }
    const auto file = resolve(name);
    if (!file) {
        throw std::runtime_error("Module '" + name + "' not found in the game folder or lualib");
    }
    // The entry lives only while the module loads, whatever the outcome.
    const auto loading = loading_.emplace(name, 1).first;
    struct Done {
        std::map<std::string, int>& map;
        std::map<std::string, int>::iterator entry;
        ~Done() { map.erase(entry); }
    } done{loading_, loading};
    auto value = load(name, *file);
    loaded_.raw_set(name, value);
    moduleFiles_[name] = *file;
    return value;
}


void LuaHost::report(const std::string& message) {
    log(LogLevel::Error, message);
    errors_.push_back({message, now()});
    while (errors_.size() > 50) {
        errors_.pop_front();
    }
    if (onError) {
        onError(errors_.back());
    }
}

Evaluation LuaHost::evaluate(const std::string& code) {
    lua_State* state = lua_->lua_state();
    std::string syntaxError;
    for (const auto& source : {"return " + code, code}) {
        if (luaL_loadbufferx(state, source.data(), source.size(), "=console", "t") != LUA_OK) {
            syntaxError = lua_tostring(state, -1);
            lua_pop(state, 1);
            continue;
        }
        sol::protected_function chunk(sol::stack_reference(state, -1), handler_);
        lua_pop(state, 1);
        const Callout callout(*this);
        auto result = chunk();
        if (!result.valid()) {
            const sol::error error = result;
            return {false, error.what()};
        }
        std::string output;
        for (int i = 0; i < result.return_count(); ++i) {
            output += (i ? "\t" : "") + displayString(state, result.stack_index() + i);
        }
        return {true, output};
    }
    return {false, syntaxError};
}

std::string displayString(lua_State* state, int index) {
    index = lua_absindex(state, index);
    if (!lua_checkstack(state, 2)) {
        return "?";
    }
    lua_pushcfunction(state, toStringThunk);
    lua_pushvalue(state, index);
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        std::string message = "<tostring failed: ";
        message += lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "error object";
        lua_pop(state, 1);
        return message + ">";
    }
    std::size_t size = 0;
    const char* text = lua_tolstring(state, -1, &size);
    std::string result(text, size);
    lua_pop(state, 1);
    return result;
}
} // namespace fe
