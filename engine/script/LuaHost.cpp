#include "engine/script/LuaHost.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(FENGINE_SANITIZERS) && defined(__linux__)
#include <sys/resource.h>
#endif

// Called by the patched Lua library's long loops (cmake/LuaPatches.cmake).
extern "C" {
extern void (*fe_lua_step_hook)(lua_State* L);
}

namespace fe {
namespace {
LuaHost* hostOf(lua_State* state) {
    return static_cast<LuaHost*>(*static_cast<void**>(lua_getextraspace(state)));
}

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

// The registry holds each host's pointer under this key, so the library step
// hook (which may run in a state the engine did not make) finds it safely.
const char hostKey = 0;

// Pattern matching, plain string.find and table.move run in C without
// executing Lua instructions, so the count hook never sees them; the patched
// library calls this every 65536 steps instead.
void libraryStepHook(lua_State* state) {
    if (!lua_checkstack(state, 1)) {
        return;
    }
    lua_rawgetp(state, LUA_REGISTRYINDEX, &hostKey);
    const auto* host = static_cast<const LuaHost*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    if (host && host->watchdogExpired()) {
        luaL_error(state, "script watchdog: a frame ran longer than %f seconds (infinite loop?)",
                   host->watchdog());
    }
}

void watchdogHook(lua_State* state, lua_Debug*) {
    auto* host = hostOf(state);
    if (!host || host->watchdog() <= 0) {
        return;
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - host->frameStart()).count();
    if (elapsed > host->watchdog()) {
        luaL_error(state, "script watchdog: a frame ran longer than %f seconds (infinite loop?)",
                   host->watchdog());
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

// Patches a live module with a freshly loaded copy of it. All bookkeeping
// tables live on the Lua stack between `top_` and the end of the object's life.
class ModulePatcher {
public:
    ModulePatcher(lua_State* state, std::string chunk) : L_(state), chunk_(std::move(chunk)) {
        if (!lua_checkstack(L_, 64)) {
            throw std::runtime_error("Lua stack exhausted during hot reload");
        }
        top_ = lua_gettop(L_);
        for (int i = 0; i < 8; ++i) {
            lua_newtable(L_);
        }
        mapping_ = top_ + 1;      // fresh table -> live table
        cells_ = top_ + 2;        // upvalue name -> {function, index} of the live module (false: ambiguous)
        scanned_ = top_ + 3;      // values that existed before the reload
        fixed_ = top_ + 4;        // values whose references were already fixed
        atoms_ = top_ + 5;        // other modules' tables, never patched
        freshCells_ = top_ + 6;   // upvalue name -> {function, index} of the fresh module (false: ambiguous)
        freshScanned_ = top_ + 7; // values reachable from the fresh module
        joined_ = top_ + 8;       // names whose live variable already took its value
    }
    ~ModulePatcher() { lua_settop(L_, top_); }
    ModulePatcher(const ModulePatcher&) = delete;
    ModulePatcher& operator=(const ModulePatcher&) = delete;

    void addAtom(int index) { mark(atoms_, index); }
    void removeAtom(int index) {
        lua_pushvalue(L_, index);
        lua_pushnil(L_);
        lua_rawset(L_, atoms_);
    }

    // Records the module-level upvalues of every function of the live module.
    void scan(int index) { scan(index, cells_, scanned_, 0); }
    // Records the module-level upvalues of every new function of the fresh
    // module (after scan(), so values that existed before are skipped).
    void scanFresh(int index) { scan(index, freshCells_, freshScanned_, scanned_); }

    // Copies code and new keys from `fresh` into `live`, descending into nested
    // tables and metatables. Existing non-function values stay.
    void patch(int live, int fresh) {
        live = lua_absindex(L_, live);
        fresh = lua_absindex(L_, fresh);
        const int work = beginWork();
        lua_Integer size = 0;
        push(work, size, live);
        push(work, size, fresh);
        while (size > 0) {
            reserve();
            take(work, size);
            take(work, size);
            patchPair(work, size, lua_gettop(L_), lua_gettop(L_) - 1);
            lua_pop(L_, 2);
        }
        lua_settop(L_, work - 1);
    }

    // Walks everything reachable from `index` and points new functions at
    // live state: references to patched fresh tables become the live tables,
    // and the module's locals keep their live values.
    void fix(int index) {
        index = lua_absindex(L_, index);
        const int work = beginWork();
        lua_Integer size = 0;
        push(work, size, index);
        while (size > 0) {
            reserve();
            take(work, size);
            const int value = lua_gettop(L_);
            const int type = lua_type(L_, value);
            if ((type == LUA_TTABLE || type == LUA_TFUNCTION) && !marked(fixed_, value) &&
                !marked(atoms_, value)) {
                mark(fixed_, value);
                if (type == LUA_TTABLE) {
                    fixTable(value, work, size);
                } else if (!lua_iscfunction(L_, value) && !marked(scanned_, value)) {
                    fixFunction(value, work, size);
                }
            }
            lua_pop(L_, 1);
        }
        lua_settop(L_, work - 1);
    }

private:
    // The walks keep the values they have still to visit in a Lua table (a
    // stack) rather than recursing, so deep data (a long linked list held by a
    // module) cannot exhaust the C stack.
    int beginWork() {
        reserve();
        lua_newtable(L_);
        return lua_gettop(L_);
    }
    void push(int work, lua_Integer& size, int index) {
        lua_pushvalue(L_, index);
        lua_rawseti(L_, work, ++size);
    }
    // Pushes the last value of the work list and removes it from the list.
    void take(int work, lua_Integer& size) {
        lua_rawgeti(L_, work, size);
        lua_pushnil(L_);
        lua_rawseti(L_, work, size--);
    }

    void patchPair(int work, lua_Integer& size, int live, int fresh) {
        if (lua_rawequal(L_, live, fresh) || marked(mapping_, fresh)) {
            return;
        }
        lua_pushvalue(L_, fresh);
        lua_pushvalue(L_, live);
        lua_rawset(L_, mapping_);
        lua_pushnil(L_);
        while (lua_next(L_, fresh) != 0) {
            const int value = lua_gettop(L_);
            const int key = value - 1;
            lua_pushvalue(L_, key);
            lua_rawget(L_, live);
            const int current = value + 1;
            if (patchable(current, value)) {
                push(work, size, current);
                push(work, size, value);
            } else if (replaces(current, value)) {
                lua_pushvalue(L_, key);
                lua_pushvalue(L_, value);
                lua_rawset(L_, live);
            }
            lua_pop(L_, 2);
        }
        if (lua_getmetatable(L_, fresh)) {
            const int freshMeta = lua_gettop(L_);
            if (lua_getmetatable(L_, live)) {
                if (patchable(freshMeta + 1, freshMeta)) {
                    push(work, size, freshMeta + 1);
                    push(work, size, freshMeta);
                }
                lua_pop(L_, 1);
            } else {
                // fix() later swaps a fresh metatable that turns out to be
                // patched into a live table for its live counterpart.
                pushLive(freshMeta);
                lua_setmetatable(L_, live);
            }
            lua_pop(L_, 1);
        }
    }

    void scan(int index, int cells, int seen, int skip) {
        index = lua_absindex(L_, index);
        const int work = beginWork();
        lua_Integer size = 0;
        push(work, size, index);
        while (size > 0) {
            reserve();
            take(work, size);
            const int value = lua_gettop(L_);
            const int type = lua_type(L_, value);
            if ((type == LUA_TTABLE || type == LUA_TFUNCTION) && !marked(seen, value) &&
                !marked(atoms_, value) && (skip == 0 || !marked(skip, value))) {
                mark(seen, value);
                if (type == LUA_TTABLE) {
                    lua_pushnil(L_);
                    while (lua_next(L_, value) != 0) {
                        push(work, size, -1);
                        lua_pop(L_, 1);
                    }
                    if (lua_getmetatable(L_, value)) {
                        push(work, size, -1);
                        lua_pop(L_, 1);
                    }
                } else if (!lua_iscfunction(L_, value)) {
                    const bool own = ownedByModule(value);
                    for (int n = 1;; ++n) {
                        const char* name = lua_getupvalue(L_, value, n);
                        if (!name) {
                            break;
                        }
                        if (own && std::strcmp(name, "_ENV") != 0) {
                            recordCell(cells, name, value, n);
                        }
                        push(work, size, -1);
                        lua_pop(L_, 1);
                    }
                }
            }
            lua_pop(L_, 1);
        }
        lua_settop(L_, work - 1);
    }

    void reserve() {
        if (!lua_checkstack(L_, 16)) {
            throw std::runtime_error("Lua stack exhausted during hot reload");
        }
    }

    [[nodiscard]] bool marked(int set, int index) {
        lua_pushvalue(L_, index);
        const bool found = lua_rawget(L_, set) != LUA_TNIL;
        lua_pop(L_, 1);
        return found;
    }

    void mark(int set, int index) {
        index = lua_absindex(L_, index);
        lua_pushvalue(L_, index);
        lua_pushboolean(L_, 1);
        lua_rawset(L_, set);
    }

    // Pushes the live counterpart of a fresh table, or the value itself.
    void pushLive(int index) {
        lua_pushvalue(L_, index);
        if (lua_rawget(L_, mapping_) == LUA_TNIL) {
            lua_pop(L_, 1);
            lua_pushvalue(L_, index);
        }
    }

    [[nodiscard]] bool ownedByModule(int function) {
        lua_Debug info{};
        lua_pushvalue(L_, function);
        lua_getinfo(L_, ">S", &info);
        return info.source && chunk_ == info.source;
    }

    [[nodiscard]] bool patchable(int current, int value) {
        return lua_type(L_, current) == LUA_TTABLE && lua_type(L_, value) == LUA_TTABLE &&
               !marked(atoms_, current) && !marked(atoms_, value);
    }

    // New keys are added; code replaces the module's own code, but never a
    // function another module or the game assigned at runtime.
    [[nodiscard]] bool replaces(int current, int value) {
        if (lua_isnil(L_, current) || marked(atoms_, value)) {
            return true;
        }
        const bool ownCode =
            lua_type(L_, current) == LUA_TFUNCTION && !lua_iscfunction(L_, current) && ownedByModule(current);
        if (lua_type(L_, value) == LUA_TFUNCTION) {
            return lua_type(L_, current) != LUA_TFUNCTION || ownCode;
        }
        return ownCode;
    }

    void recordCell(int cells, const char* name, int function, int n) {
        void* id = lua_upvalueid(L_, function, n);
        const int found = lua_getfield(L_, cells, name);
        if (found == LUA_TTABLE) {
            lua_rawgeti(L_, -1, 1);
            lua_rawgeti(L_, -2, 2);
            void* known = lua_upvalueid(L_, -2, static_cast<int>(lua_tointeger(L_, -1)));
            lua_pop(L_, 3);
            if (known != id) {
                lua_pushboolean(L_, 0);
                lua_setfield(L_, cells, name);
            }
            return;
        }
        lua_pop(L_, 1);
        if (found == LUA_TNIL) {
            lua_createtable(L_, 2, 0);
            lua_pushvalue(L_, function);
            lua_rawseti(L_, -2, 1);
            lua_pushinteger(L_, n);
            lua_rawseti(L_, -2, 2);
            lua_setfield(L_, cells, name);
        }
    }

    void fixTable(int table, int work, lua_Integer& size) {
        lua_pushnil(L_);
        while (lua_next(L_, table) != 0) {
            const int value = lua_gettop(L_);
            if (lua_type(L_, value) == LUA_TTABLE && marked(mapping_, value)) {
                lua_pushvalue(L_, value - 1);
                pushLive(value);
                lua_rawset(L_, table);
            } else {
                push(work, size, value);
            }
            lua_pop(L_, 1);
        }
        if (lua_getmetatable(L_, table)) {
            if (marked(mapping_, -1)) {
                pushLive(lua_gettop(L_));
                lua_setmetatable(L_, table);
            } else {
                push(work, size, -1);
            }
            lua_pop(L_, 1);
        }
    }

    void fixFunction(int function, int work, lua_Integer& size) {
        const bool own = ownedByModule(function);
        for (int n = 1;; ++n) {
            const char* name = lua_getupvalue(L_, function, n);
            if (!name) {
                break;
            }
            const int value = lua_gettop(L_);
            if (own && std::strcmp(name, "_ENV") != 0 && joinLocal(name, function, n, value)) {
                lua_pop(L_, 1);
                continue;
            }
            if (lua_type(L_, value) == LUA_TTABLE && marked(mapping_, value)) {
                pushLive(value);
                lua_setupvalue(L_, function, n);
            } else {
                push(work, size, value);
            }
            lua_pop(L_, 1);
        }
    }

    // Joins a module-level local of the new code to the live local of the same
    // name, so old and new closures share one variable. The first new function
    // that reaches it decides the variable's value. Returns false when either
    // version uses the name for more than one variable, or the live module has
    // no such local; the new code then keeps its own.
    [[nodiscard]] bool joinLocal(const char* name, int function, int n, int value) {
        if (lua_getfield(L_, freshCells_, name) != LUA_TTABLE) {
            lua_pop(L_, 1);
            return false;
        }
        lua_pop(L_, 1);
        if (lua_getfield(L_, cells_, name) != LUA_TTABLE) {
            lua_pop(L_, 1);
            return false;
        }
        lua_rawgeti(L_, -1, 1);
        const int oldFunction = lua_gettop(L_);
        lua_rawgeti(L_, -2, 2);
        const int oldIndex = static_cast<int>(lua_tointeger(L_, -1));
        lua_pop(L_, 1);
        const bool decided = lua_getfield(L_, joined_, name) != LUA_TNIL;
        lua_pop(L_, 1);
        if (!decided) {
            lua_pushboolean(L_, 1);
            lua_setfield(L_, joined_, name);
            lua_getupvalue(L_, oldFunction, oldIndex);
            pushJoinedValue(lua_gettop(L_), value);
            // New closures the walk never reaches keep the fresh variable,
            // which therefore takes the value too.
            lua_pushvalue(L_, -1);
            lua_setupvalue(L_, function, n);
            lua_pushvalue(L_, -1);
            lua_setupvalue(L_, oldFunction, oldIndex);
            lua_upvaluejoin(L_, function, n, oldFunction, oldIndex);
            fix(-1);
            lua_pop(L_, 4);
            return true;
        }
        lua_upvaluejoin(L_, function, n, oldFunction, oldIndex);
        lua_pop(L_, 2);
        return true;
    }

    // Pushes the value a joined local takes: live tables are patched and kept,
    // live data is kept unless the new code changed its type or the live value
    // is nil, new code replaces the module's own functions but not functions
    // assigned at runtime, and references to other modules are replaced.
    void pushJoinedValue(int old, int fresh) {
        if (keepsLive(old, fresh)) {
            lua_pushvalue(L_, old);
        } else {
            pushLive(fresh);
        }
    }

    [[nodiscard]] bool keepsLive(int old, int fresh) {
        const int oldType = lua_type(L_, old), freshType = lua_type(L_, fresh);
        if (marked(atoms_, old) || marked(atoms_, fresh)) {
            return false;
        }
        if (oldType == LUA_TTABLE && freshType == LUA_TTABLE) {
            patch(old, fresh);
            return true;
        }
        if (freshType == LUA_TFUNCTION) {
            return oldType == LUA_TFUNCTION && !replaces(old, fresh);
        }
        return oldType != LUA_TNIL && (oldType == freshType || freshType == LUA_TNIL);
    }

    lua_State* L_;
    std::string chunk_;
    int top_ = 0, mapping_ = 0, cells_ = 0, scanned_ = 0, fixed_ = 0, atoms_ = 0, freshCells_ = 0,
        freshScanned_ = 0, joined_ = 0;
};

std::optional<lua_Integer> canonicalInteger(std::string_view text) {
    lua_Integer value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || std::to_string(value) != text) {
        return std::nullopt;
    }
    return value;
}
} // namespace

LuaHost::LuaHost(std::filesystem::path gameRoot, std::filesystem::path libraryRoot, bool development)
    : gameRoot_(std::filesystem::weakly_canonical(gameRoot)),
      libraryRoot_(std::filesystem::weakly_canonical(libraryRoot)), start_(std::chrono::steady_clock::now()),
      frameStart_(start_) {
    lua_ = std::make_unique<sol::state>();
    *static_cast<void**>(lua_getextraspace(lua_->lua_state())) = this;
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
    loaded_ = lua_->create_table();
    fe_["_loaded"] = loaded_;
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
    if (loading_[name] > 0) {
        throw std::runtime_error("Circular require of module '" + name + "'");
    }
    const auto file = resolve(name);
    if (!file) {
        throw std::runtime_error("Module '" + name + "' not found in the game folder or lualib");
    }
    ++loading_[name];
    try {
        auto value = load(name, *file);
        --loading_[name];
        loaded_.raw_set(name, value);
        moduleFiles_[name] = *file;
        return value;
    } catch (...) {
        --loading_[name];
        throw;
    }
}

void LuaHost::patchModule(const sol::table& live, const sol::table& fresh, const std::string& chunk) {
    lua_State* state = lua_->lua_state();
    ModulePatcher patcher(state, chunk);
    loaded_.push(state);
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        if (lua_type(state, -1) == LUA_TTABLE) {
            patcher.addAtom(-1);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    live.push(state);
    fresh.push(state);
    const int liveIndex = lua_gettop(state) - 1;
    patcher.removeAtom(liveIndex);
    patcher.scan(liveIndex);
    patcher.scanFresh(liveIndex + 1);
    patcher.patch(liveIndex, liveIndex + 1);
    patcher.fix(liveIndex);
}

bool LuaHost::reloadFile(const std::filesystem::path& changed) {
    std::error_code ec;
    for (const auto& [name, file] : moduleFiles_) {
        if (!std::filesystem::equivalent(file, changed, ec)) {
            continue;
        }
        try {
            auto fresh = load(name, file);
            const auto old = loaded_.raw_get<sol::object>(name);
            if (old.get_type() == sol::type::table && fresh.get_type() == sol::type::table) {
                sol::table live = old;
                patchModule(live, fresh, chunkName(file));
                call(field(live, "__reload"), live, fresh);
            } else {
                loaded_.raw_set(name, fresh);
            }
            log(LogLevel::Info, "Reloaded Lua module " + name);
        } catch (const std::exception& error) {
            report("Reload of '" + name + "' failed; keeping the previous version:\n" + error.what());
        }
        return true;
    }
    return false;
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

Json luaToJson(lua_State* state, int index, int depth) {
    if (depth > maxJsonDepth) {
        throw std::runtime_error("Value nests deeper than " + std::to_string(maxJsonDepth) +
                                 " levels; cannot convert it to JSON");
    }
    if (!lua_checkstack(state, 4)) {
        throw std::runtime_error("Lua stack exhausted while converting a value to JSON");
    }
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        return nullptr;
    case LUA_TBOOLEAN:
        return lua_toboolean(state, index) != 0;
    case LUA_TNUMBER: {
        if (lua_isinteger(state, index)) {
            return lua_tointeger(state, index);
        }
        const double value = lua_tonumber(state, index);
        if (!std::isfinite(value)) {
            throw std::runtime_error("Cannot store a non-finite number");
        }
        return value;
    }
    case LUA_TSTRING: {
        std::size_t size = 0;
        const char* text = lua_tolstring(state, index, &size);
        return std::string(text, size);
    }
    case LUA_TTABLE: {
        const auto length = lua_rawlen(state, index);
        std::size_t count = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            ++count;
            lua_pop(state, 1);
        }
        if (length > 0 && count == length) {
            Json out = Json::array();
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(length); ++i) {
                lua_rawgeti(state, index, i);
                try {
                    out.push_back(luaToJson(state, -1, depth + 1));
                } catch (...) {
                    lua_pop(state, 1);
                    throw;
                }
                lua_pop(state, 1);
            }
            return out;
        }
        Json out = Json::object();
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            std::string key;
            if (lua_type(state, -2) == LUA_TSTRING) {
                std::size_t size = 0;
                const char* text = lua_tolstring(state, -2, &size);
                key.assign(text, size);
                if (key.starts_with('#')) {
                    key.insert(0, 1, '#');
                }
            } else if (lua_isinteger(state, -2)) {
                key = "#" + std::to_string(lua_tointeger(state, -2));
            } else {
                lua_pop(state, 2);
                throw std::runtime_error("Table keys must be strings or integers to convert to JSON");
            }
            try {
                out[key] = luaToJson(state, -1, depth + 1);
            } catch (...) {
                lua_pop(state, 2);
                throw;
            }
            lua_pop(state, 1);
        }
        return out;
    }
    default:
        throw std::runtime_error(std::string("Cannot convert a ") +
                                 lua_typename(state, lua_type(state, index)) + " to JSON");
    }
}

Json luaToJson(const sol::object& value) {
    lua_State* state = value.lua_state();
    value.push(state);
    try {
        auto result = luaToJson(state, -1);
        lua_pop(state, 1);
        return result;
    } catch (...) {
        lua_pop(state, 1);
        throw;
    }
}

sol::object jsonToLua(sol::state_view lua, const Json& value, int depth) {
    if (depth > maxJsonDepth) {
        throw std::runtime_error("JSON nests deeper than " + std::to_string(maxJsonDepth) + " levels");
    }
    if (value.is_null()) {
        return sol::make_object(lua, sol::lua_nil);
    }
    if (value.is_boolean()) {
        return sol::make_object(lua, value.get<bool>());
    }
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max())) {
        return sol::make_object(lua, value.get<double>());
    }
    if (value.is_number_integer()) {
        return sol::make_object(lua, value.get<lua_Integer>());
    }
    if (value.is_number()) {
        return sol::make_object(lua, value.get<double>());
    }
    if (value.is_string()) {
        return sol::make_object(lua, value.get_ref<const std::string&>());
    }
    auto table = lua.create_table();
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            table[i + 1] = jsonToLua(lua, value[i], depth + 1);
        }
        return table;
    }
    for (const auto& [key, element] : value.items()) {
        if (key.starts_with("##")) {
            table[key.substr(1)] = jsonToLua(lua, element, depth + 1);
        } else if (const auto integer =
                       key.starts_with('#') ? canonicalInteger(key.substr(1)) : std::nullopt) {
            table[*integer] = jsonToLua(lua, element, depth + 1);
        } else {
            table[key] = jsonToLua(lua, element, depth + 1);
        }
    }
    return table;
}
} // namespace fe

// The collector hold reaches into Lua's global state: the public API's
// LUA_GCSTOP/LUA_GCRESTART would also reset the collector's debt, forcing a
// collection after every binding call. Only GCSTPUSR is touched (scripts
// cannot stop the collector themselves: collectgarbage only counts and
// steps), so the collector's own stops while it runs finalizers or closes
// the state are left alone. Included last: these headers define short macros.
#include "lgc.h"
#include "lstate.h"

namespace fe {
LuaHost::BindingCall::BindingCall(LuaHost& host) : state_(host.lua_ ? host.lua_->lua_state() : nullptr) {
    if (host.closing_ || !state_) {
        throw std::runtime_error("engine is shutting down");
    }
    global_State* g = G(state_);
    if ((g->gcstp & GCSTPUSR) == 0) {
        g->gcstp |= GCSTPUSR;
        held_ = true;
    }
}

LuaHost::BindingCall::~BindingCall() {
    if (held_) {
        G(state_)->gcstp &= static_cast<lu_byte>(~GCSTPUSR);
    }
}

LuaHost::Callout::Callout(LuaHost& host) : state_(host.lua_ ? host.lua_->lua_state() : nullptr) {
    if (!state_) {
        return;
    }
    global_State* g = G(state_);
    if ((g->gcstp & GCSTPUSR) != 0) {
        g->gcstp &= static_cast<lu_byte>(~GCSTPUSR);
        released_ = true;
    }
}

LuaHost::Callout::~Callout() {
    if (released_) {
        G(state_)->gcstp |= GCSTPUSR;
    }
}
} // namespace fe
