// Hot reload: LuaHost::reloadFile and the ModulePatcher that merges a freshly
// loaded module into the live one. The contract is "Hot reload" in
// docs/architecture.md.
#include "engine/core/Log.h"
#include "engine/script/LuaHost.h"
#include <cstring>

namespace fe {
namespace {
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

    // New keys are added and references to other modules replaced; otherwise
    // only the module's own code is replaced: never a function another module
    // or the game assigned at runtime, and never live data (a `handlers = {}`
    // edited into `function handlers()` keeps the live table, as the hot
    // reload contract promises; only a `__reload` adopts such a change).
    [[nodiscard]] bool replaces(int current, int value) {
        if (lua_isnil(L_, current) || marked(atoms_, value)) {
            return true;
        }
        return lua_type(L_, current) == LUA_TFUNCTION && !lua_iscfunction(L_, current) &&
               ownedByModule(current);
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
} // namespace

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
} // namespace fe
