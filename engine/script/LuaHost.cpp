#include "engine/script/LuaHost.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <cmath>
#include <fstream>
#include <sstream>

namespace fe {
namespace {
LuaHost* hostOf(lua_State* state) {
    return static_cast<LuaHost*>(*static_cast<void**>(lua_getextraspace(state)));
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

std::string readFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open " + file.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}
} // namespace

LuaHost::LuaHost(std::filesystem::path gameRoot, std::filesystem::path libraryRoot, bool development)
    : gameRoot_(std::filesystem::weakly_canonical(gameRoot)),
      libraryRoot_(std::filesystem::weakly_canonical(libraryRoot)), start_(std::chrono::steady_clock::now()),
      frameStart_(start_) {
    *static_cast<void**>(lua_getextraspace(lua_.lua_state())) = this;
    lua_.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                        sol::lib::coroutine, sol::lib::utf8);
    for (const char* name : {"dofile", "loadfile", "load", "loadstring", "collectgarbage", "require"}) {
        lua_[name] = sol::lua_nil;
    }
    lua_["string"]["dump"] = sol::lua_nil;
    lua_["print"] = [](sol::variadic_args args) {
        std::string line;
        for (const auto& arg : args) {
            if (!line.empty()) {
                line += '\t';
            }
            std::size_t size = 0;
            lua_State* state = arg.lua_state();
            const char* text = luaL_tolstring(state, arg.stack_index(), &size);
            line.append(text, size);
            lua_pop(state, 1);
        }
        log(LogLevel::Info, "lua: " + line);
    };
    lua_.set_function("collectgarbage", [this](sol::optional<std::string> option) -> double {
        const auto what = option.value_or("count");
        if (what == "count") {
            return lua_gc(lua_.lua_state(), LUA_GCCOUNT, 0) +
                   lua_gc(lua_.lua_state(), LUA_GCCOUNTB, 0) / 1024.0;
        }
        if (what == "step") {
            lua_gc(lua_.lua_state(), LUA_GCSTEP, 0);
            return 0;
        }
        throw std::runtime_error("collectgarbage only supports 'count' and 'step'");
    });
    lua_gc(lua_.lua_state(), LUA_GCGEN, 0, 0);
    handler_ = sol::make_object(lua_, tracebackHandler).as<sol::function>();
    fe_ = lua_.create_named_table("fe");
    loaded_ = lua_.create_table();
    fe_["_loaded"] = loaded_;
    lua_.set_function("require", [this](const std::string& name) { return require(name); });
    if (development) {
        watchdog_ = 3.0;
        lua_sethook(lua_.lua_state(), watchdogHook, LUA_MASKCOUNT, 200000);
    }
}

LuaHost::~LuaHost() {
    lua_sethook(lua_.lua_state(), nullptr, 0, 0);
}

double LuaHost::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

sol::table LuaHost::module(const std::string& dottedName) {
    sol::table table = lua_.globals();
    std::size_t start = 0;
    while (start <= dottedName.size()) {
        const auto end = dottedName.find('.', start);
        const auto part =
            dottedName.substr(start, end == std::string::npos ? std::string::npos : end - start);
        sol::object existing = table[part];
        if (!existing.valid() || existing.get_type() != sol::type::table) {
            table[part] = lua_.create_table();
        }
        table = table[part];
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return table;
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
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

sol::object LuaHost::load(const std::string& name, const std::filesystem::path& file) {
    const auto code = readFile(file);
    auto relative = relativeTo(gameRoot_, file);
    if (relative.empty()) {
        relative = "lualib/" + relativeTo(libraryRoot_, file);
    }
    lua_State* state = lua_.lua_state();
    if (luaL_loadbufferx(state, code.data(), code.size(), ("@" + relative).c_str(), "t") != LUA_OK) {
        const std::string message = lua_tostring(state, -1);
        lua_pop(state, 1);
        throw std::runtime_error(message);
    }
    sol::protected_function chunk(sol::stack_reference(state, -1), handler_);
    lua_pop(state, 1);
    auto result = chunk(name);
    if (!result.valid()) {
        const sol::error error = result;
        throw std::runtime_error(error.what());
    }
    sol::object value = result.return_count() > 0 ? result.get<sol::object>() : sol::make_object(lua_, true);
    if (!value.valid() || value.get_type() == sol::type::lua_nil) {
        value = sol::make_object(lua_, true);
    }
    return value;
}

sol::object LuaHost::require(const std::string& name) {
    sol::object cached = loaded_[name];
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
        loaded_[name] = value;
        moduleFiles_[name] = *file;
        return value;
    } catch (...) {
        --loading_[name];
        throw;
    }
}

bool LuaHost::reloadFile(const std::filesystem::path& changed) {
    std::error_code ec;
    for (const auto& [name, file] : moduleFiles_) {
        if (!std::filesystem::equivalent(file, changed, ec)) {
            continue;
        }
        try {
            auto fresh = load(name, file);
            sol::object old = loaded_[name];
            if (old.get_type() == sol::type::table && fresh.get_type() == sol::type::table) {
                sol::table target = old;
                sol::table source = fresh;
                std::vector<sol::object> stale;
                for (const auto& [key, value] : target) {
                    (void)value;
                    if (!source[key].valid() || source[key].get_type() == sol::type::lua_nil) {
                        stale.push_back(key);
                    }
                }
                for (const auto& key : stale) {
                    target[key] = sol::lua_nil;
                }
                for (const auto& [key, value] : source) {
                    target[key] = value;
                }
                call(target["__on_reload"], target);
            } else {
                loaded_[name] = fresh;
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

void LuaHost::reportOnce(const std::string& message) {
    if (reportedOnce_.size() >= 256) {
        reportedOnce_.clear(); // messages that differ every frame must not grow this forever
    }
    if (reportedOnce_.insert(message).second) {
        report(message);
    }
}

std::string LuaHost::console(const std::string& code) {
    lua_State* state = lua_.lua_state();
    for (const auto& source : {"return " + code, code}) {
        if (luaL_loadbufferx(state, source.data(), source.size(), "=console", "t") != LUA_OK) {
            lua_pop(state, 1);
            continue;
        }
        sol::protected_function chunk(sol::stack_reference(state, -1), handler_);
        lua_pop(state, 1);
        auto result = chunk();
        if (!result.valid()) {
            const sol::error error = result;
            return error.what();
        }
        std::string output;
        for (int i = 0; i < result.return_count(); ++i) {
            std::size_t size = 0;
            lua_pushvalue(state, result.stack_index() + i);
            const char* text = luaL_tolstring(state, -1, &size);
            output += (i ? "\t" : "") + std::string(text, size);
            lua_pop(state, 2);
        }
        return output;
    }
    return "syntax error";
}

Json luaToJson(lua_State* state, int index, int depth) {
    if (depth > 32) {
        throw std::runtime_error("Value nesting too deep for JSON");
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
                out.push_back(luaToJson(state, -1, depth + 1));
                lua_pop(state, 1);
            }
            return out;
        }
        Json out = Json::object();
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            std::string key;
            if (lua_type(state, -2) == LUA_TSTRING) {
                key = lua_tostring(state, -2);
            } else if (lua_type(state, -2) == LUA_TNUMBER) {
                lua_pushvalue(state, -2);
                key = lua_tostring(state, -1);
                lua_pop(state, 1);
            } else {
                lua_pop(state, 2);
                throw std::runtime_error("Table keys must be strings or numbers to convert to JSON");
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

sol::object jsonToLua(sol::state_view lua, const Json& value) {
    if (value.is_null()) {
        return sol::make_object(lua, sol::lua_nil);
    }
    if (value.is_boolean()) {
        return sol::make_object(lua, value.get<bool>());
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
            table[i + 1] = jsonToLua(lua, value[i]);
        }
    } else {
        for (const auto& [key, element] : value.items()) {
            table[key] = jsonToLua(lua, element);
        }
    }
    return table;
}
} // namespace fe
