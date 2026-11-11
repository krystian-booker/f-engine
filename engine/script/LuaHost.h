#pragma once
#include "engine/core/Json.h"
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sol/sol.hpp>
#include <string>
#include <vector>

namespace fe {
struct ScriptError {
    std::string message; // includes a traceback when one was available
    double time = 0;
};

struct Evaluation {
    bool ok = false;
    std::string output; // printable results, or the error with a traceback
};

// Owns the Lua state. Scripts get base, math, string, table, coroutine and
// utf8; there is no io, os, debug, package or dynamic code loading, the
// metatables of strings and engine objects are locked, and engine objects show
// scripts only their documented members (see sealTypes). `require` searches the
// game folder only: the engine ships no Lua modules, so every module a game
// requires is its own. `builtinRoot` is the engine's asset folder ('@fe/...').
//
// The implementation is split by concern: LuaHost.cpp (state, sandbox,
// sealing, require, console), ModulePatcher.cpp (hot reload), LuaJson.cpp
// (the JSON conversion below) and GcHold.cpp (BindingCall and Callout, the
// only file that includes Lua's internal headers).
class LuaHost {
public:
    LuaHost(std::filesystem::path gameRoot, std::filesystem::path builtinRoot, bool development);
    ~LuaHost();
    LuaHost(const LuaHost&) = delete;
    LuaHost& operator=(const LuaHost&) = delete;

    [[nodiscard]] sol::state& lua() { return *lua_; }
    [[nodiscard]] sol::table fe() { return fe_; }
    // Creates (or returns) a nested table such as fe.world.
    sol::table module(const std::string& dottedName);
    [[nodiscard]] const std::filesystem::path& gameRoot() const { return gameRoot_; }
    [[nodiscard]] const std::filesystem::path& builtinRoot() const { return builtinRoot_; }

    sol::object require(const std::string& name);
    [[nodiscard]] std::optional<std::filesystem::path> resolve(const std::string& name) const;
    // Re-runs the module that owns `file` and patches the live module in place
    // (the contract is "Hot reload" in docs/architecture.md). Returns false when
    // no loaded module comes from `file`. A failed reload reports an error and
    // keeps the previous version.
    bool reloadFile(const std::filesystem::path& file);
    // The members scripts may use on an engine type: TypeBinder records every
    // method and property here. `metatable` is sol2's registry key of the
    // type's value metatable.
    struct TypeMembers {
        std::string metatable;
        std::set<std::string> readable, writable;
    };
    TypeMembers& exposeType(const std::string& name, const std::string& metatable);
    // Call once after registering types. Locks the string metatable and every
    // sol2 metatable (`getmetatable` returns false). On each engine type's
    // metatables, indexing an object yields only its recorded members (nil for
    // anything else, including metamethods and sol2's own keys such as
    // `__gc`, `__type` or `class_cast`), assigning works only for its writable
    // properties, and `__pairs` is removed. The global type tables sol2 creates
    // (`World`, `Entity`...) are removed, so their storage is unreachable.
    void sealTypes();

    // Every engine binding runs inside a BindingCall (Binder wraps them all).
    // It refuses with "engine is shutting down" once shutdown has begun, and
    // holds the garbage collector while the binding's C++ runs, so no
    // finalizer (__gc) can run Lua code in the middle of it: the only Lua a
    // binding runs is what it calls on purpose, through a Callout.
    class BindingCall {
    public:
        explicit BindingCall(LuaHost& host);
        ~BindingCall();
        BindingCall(const BindingCall&) = delete;
        BindingCall& operator=(const BindingCall&) = delete;

    private:
        lua_State* state_;
        bool held_ = false;
    };
    // Lua code a binding runs on purpose (callbacks such as path cost
    // functions and the text resolver, the console, module loading, tests'
    // engine ticks): the collector runs normally for its duration. A binding
    // that makes a callout must tolerate anything the callout does.
    class Callout {
    public:
        explicit Callout(LuaHost& host);
        ~Callout();
        Callout(const Callout&) = delete;
        Callout& operator=(const Callout&) = delete;

    private:
        lua_State* state_;
        bool released_ = false;
    };
    // From here on every engine binding (and `require`) raises "engine is
    // shutting down" instead of running; the engine calls it before it
    // releases its references into the Lua state and closes it.
    void beginShutdown() { closing_ = true; }
    [[nodiscard]] bool closing() const { return closing_; }
    // Begins shutdown and closes the Lua state: pending finalizers run, and
    // any engine binding they call refuses. Nothing may hold a reference into
    // the state afterwards (the engine releases its own first).
    void close();

    // Keeps `function` alive until the host is destroyed (after the state is
    // closed) and returns it. Module bindings hand Lua only a pointer to it:
    // sol2 destroys a function object it stores through a __gc metatable it
    // finds by the C++ type's printed name, and lambdas of the same function
    // (or template) can print the same name on some compilers, which would run
    // one lambda's destructor on another's storage.
    template<class F> F* keep(F function) {
        auto owned = std::make_shared<F>(std::move(function));
        F* pointer = owned.get();
        kept_.push_back(std::move(owned));
        return pointer;
    }

    // Runs `release` when the state is about to close (before finalizers run):
    // a binding's kept state that holds Lua references drops them there.
    void atClose(std::function<void()> release) { atClose_.push_back(std::move(release)); }

    // The message handler protected calls use: it turns any error object
    // into a string with a traceback, so converting the error never fails.
    [[nodiscard]] const sol::function& errorHandler() const { return handler_; }

    // Calls `fn` in protected mode; errors are recorded and false returned.
    template<class... Args> bool call(const sol::object& fn, Args&&... args) {
        if (!fn.valid() || fn.get_type() != sol::type::function) {
            return false;
        }
        sol::protected_function protectedFn(fn, handler_);
        const Callout callout(*this);
        const auto result = protectedFn(std::forward<Args>(args)...);
        if (!result.valid()) {
            const sol::error error = result;
            report(error.what());
            return false;
        }
        return true;
    }
    template<class... Args> sol::protected_function_result invoke(const sol::object& fn, Args&&... args) {
        sol::protected_function protectedFn(fn, handler_);
        const Callout callout(*this);
        return protectedFn(std::forward<Args>(args)...);
    }

    void report(const std::string& message);
    [[nodiscard]] const std::deque<ScriptError>& errors() const { return errors_; }
    void clearErrors() { errors_.clear(); }
    std::function<void(const ScriptError&)> onError;

    // The host that owns `state` (also from a coroutine of it), or null for a
    // state the engine did not make. Found through the registry, which every
    // thread of a state shares.
    static LuaHost* of(lua_State* state);

    // Evaluates a console line, as an expression first and then as statements.
    Evaluation evaluate(const std::string& code);
    std::string console(const std::string& code) { return evaluate(code).output; }
    void beginFrame() { frameStart_ = std::chrono::steady_clock::now(); }
    void setWatchdog(double seconds) { watchdog_ = seconds; }
    [[nodiscard]] double watchdog() const { return watchdog_; }
    // True once the running frame (or test) is past the watchdog limit;
    // callbacks whose errors a binding would otherwise report and swallow
    // re-raise them then, so the watchdog stops the whole frame.
    [[nodiscard]] bool watchdogExpired() const;
    [[nodiscard]] std::chrono::steady_clock::time_point frameStart() const { return frameStart_; }
    [[nodiscard]] double now() const;

private:
    [[nodiscard]] std::string chunkName(const std::filesystem::path& file) const;
    sol::object load(const std::string& name, const std::filesystem::path& file);
    void patchModule(const sol::table& live, const sol::table& fresh, const std::string& chunk);
    std::filesystem::path gameRoot_, builtinRoot_;
    std::vector<std::shared_ptr<void>> kept_; // destroyed after the state closes (see keep)
    std::vector<std::function<void()>> atClose_;
    std::unique_ptr<sol::state> lua_;
    sol::table fe_, loaded_;
    sol::function handler_;
    std::map<std::string, std::filesystem::path> moduleFiles_;
    std::map<std::string, int> loading_; // modules being loaded (entries removed at zero)
    std::map<std::string, TypeMembers> types_;
    std::deque<ScriptError> errors_;
    std::chrono::steady_clock::time_point start_, frameStart_;
    double watchdog_ = 0;
    bool closing_ = false;
};

// tostring() for any value. A failing __tostring is described in the result
// instead of raising a Lua error through C++ frames.
std::string displayString(lua_State* state, int index);

// table[key] read raw: no metamethod (__index) runs, so reading a table a
// script passed in can never run script code while a binding holds engine
// references. Fields must be in the table itself. Use it for every read of a
// table a script passed in; nil when `table` is not a table.
template<class Key> sol::object field(const sol::table& table, const Key& key) {
    lua_State* state = table.lua_state();
    if (!lua_checkstack(state, 2)) {
        throw std::runtime_error("Lua stack exhausted");
    }
    table.push(state);
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return sol::make_object(state, sol::lua_nil);
    }
    sol::stack::push(state, key);
    lua_rawget(state, -2);
    sol::object value(state, -1);
    lua_pop(state, 2);
    return value;
}

// Lua <-> JSON conversion. A table becomes an array only when its keys are
// exactly 1..n (n > 0) and nothing else; every other table becomes an object.
// Integer keys are written as "#<n>" and string keys starting with '#' get a
// second '#', so tables with sparse or mixed keys round-trip exactly.
// Functions, userdata and non-integer number keys are rejected, and so are
// values nested deeper than maxJsonDepth in either direction.
Json luaToJson(lua_State* state, int index, int depth = 0);
Json luaToJson(const sol::object& value);
sol::object jsonToLua(sol::state_view lua, const Json& value, int depth = 0);
} // namespace fe
