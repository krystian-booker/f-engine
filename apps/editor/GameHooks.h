#pragma once
#include "engine/core/Json.h"
#include "engine/core/Log.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include <optional>
#include <string>

namespace fe::editor {
// The game's editor hooks: functions of the `editor` table of its entry
// module (docs/authoring.md). Every hook is optional, and looked up each
// time, so a hot reload that adds one takes effect at once.
inline sol::object gameHook(Engine& engine, const char* name) {
    try {
        const auto module = engine.lua().require(engine.config().entry);
        if (module.get_type() == sol::type::table) {
            const auto hooks = module.as<sol::table>().raw_get<sol::object>("editor");
            if (hooks.get_type() == sol::type::table) {
                sol::object hook = hooks.as<sol::table>().raw_get<sol::object>(name);
                if (hook.get_type() == sol::type::function) {
                    return hook;
                }
            }
        }
    } catch (const std::exception&) {
        // The entry module does not load; the Console already says so.
    }
    return sol::lua_nil;
}

// Calls a hook that answers with JSON text; nullopt when the game has no
// such hook, it failed, or what it returned is not JSON.
template<class... Args> std::optional<Json> hookJson(Engine& engine, const char* name, Args&&... args) {
    const auto hook = gameHook(engine, name);
    if (hook == sol::lua_nil) {
        return std::nullopt;
    }
    const auto result = engine.lua().invoke(hook, std::forward<Args>(args)...);
    if (!result.valid()) {
        const sol::error error = result;
        log(LogLevel::Warning, std::string("editor.") + name + " failed: " + error.what());
        return std::nullopt;
    }
    if (result.get_type() != sol::type::string) {
        return std::nullopt;
    }
    auto parsed = Json::parse(result.template get<std::string>(), nullptr, false);
    return parsed.is_discarded() ? std::nullopt : std::optional<Json>(std::move(parsed));
}

// Calls a hook for its effect; false when there is none or it failed.
template<class... Args> bool hookCall(Engine& engine, const char* name, Args&&... args) {
    const auto hook = gameHook(engine, name);
    if (hook == sol::lua_nil) {
        return false;
    }
    const auto result = engine.lua().invoke(hook, std::forward<Args>(args)...);
    if (!result.valid()) {
        const sol::error error = result;
        log(LogLevel::Warning, std::string("editor.") + name + " failed: " + error.what());
        return false;
    }
    return true;
}
} // namespace fe::editor
