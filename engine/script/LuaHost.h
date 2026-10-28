#pragma once
#include "engine/core/Json.h"
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <sol/sol.hpp>
#include <string>

namespace fe {
struct ScriptError {
    std::string message; // includes a traceback when one was available
    double time = 0;
};

// Owns the Lua state. Scripts get base, math, string, table, coroutine and
// utf8; there is no io, os, debug, package or dynamic code loading. `require`
// searches the game folder first and the engine's lualib second, so a game
// can override any library module by placing a file at the same path.
class LuaHost {
public:
    LuaHost(std::filesystem::path gameRoot, std::filesystem::path libraryRoot, bool development);
    ~LuaHost();
    LuaHost(const LuaHost&) = delete;
    LuaHost& operator=(const LuaHost&) = delete;

    [[nodiscard]] sol::state& lua() { return lua_; }
    [[nodiscard]] sol::table fe() { return fe_; }
    // Creates (or returns) a nested table such as fe.world.
    sol::table module(const std::string& dottedName);
    [[nodiscard]] const std::filesystem::path& gameRoot() const { return gameRoot_; }
    [[nodiscard]] const std::filesystem::path& libraryRoot() const { return libraryRoot_; }

    sol::object require(const std::string& name);
    [[nodiscard]] std::optional<std::filesystem::path> resolve(const std::string& name) const;
    // Re-runs the module that owns `file`, patching the old table in place.
    bool reloadFile(const std::filesystem::path& file);

    // Calls `fn` in protected mode; errors are recorded and false returned.
    template<class... Args> bool call(const sol::object& fn, Args&&... args) {
        if (!fn.valid() || fn.get_type() != sol::type::function) {
            return false;
        }
        sol::protected_function protectedFn(fn, handler_);
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
        return protectedFn(std::forward<Args>(args)...);
    }

    void report(const std::string& message);
    [[nodiscard]] const std::deque<ScriptError>& errors() const { return errors_; }
    void clearErrors() { errors_.clear(); }
    std::function<void(const ScriptError&)> onError;

    // Evaluates a console line; returns printable results or the error.
    std::string console(const std::string& code);
    void beginFrame() { frameStart_ = std::chrono::steady_clock::now(); }
    void setWatchdog(double seconds) { watchdog_ = seconds; }
    [[nodiscard]] double watchdog() const { return watchdog_; }
    [[nodiscard]] std::chrono::steady_clock::time_point frameStart() const { return frameStart_; }
    [[nodiscard]] double now() const;

private:
    sol::object load(const std::string& name, const std::filesystem::path& file);
    std::filesystem::path gameRoot_, libraryRoot_;
    sol::state lua_;
    sol::table fe_, loaded_;
    sol::function handler_;
    std::map<std::string, std::filesystem::path> moduleFiles_;
    std::map<std::string, int> loading_;
    std::deque<ScriptError> errors_;
    std::chrono::steady_clock::time_point start_, frameStart_;
    double watchdog_ = 0;
};

// Lua <-> JSON conversion (tables with string keys become objects; sequences
// become arrays; functions and userdata are rejected).
Json luaToJson(lua_State* state, int index, int depth = 0);
Json luaToJson(const sol::object& value);
sol::object jsonToLua(sol::state_view lua, const Json& value);
} // namespace fe
