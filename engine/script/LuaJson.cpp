// Lua <-> JSON conversion (declared in LuaHost.h).
#include "engine/script/LuaHost.h"
#include <charconv>
#include <cmath>
#include <limits>

namespace fe {
namespace {
std::optional<lua_Integer> canonicalInteger(std::string_view text) {
    lua_Integer value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || std::to_string(value) != text) {
        return std::nullopt;
    }
    return value;
}

// The length n of a table whose keys are exactly 1..n and nothing else, or 0.
// Lua's border (lua_rawlen) is any n with t[n] ~= nil and t[n+1] == nil, so
// {1, nil, 3} may report 3 and {1, 2, x = 1} reports 2; both are objects
// here. The table at `index` is walked twice: 1..n with rawgeti, and all keys.
std::size_t sequenceLength(lua_State* state, int index) {
    std::size_t n = 0;
    for (;;) {
        const int type = lua_rawgeti(state, index, static_cast<lua_Integer>(n) + 1);
        lua_pop(state, 1);
        if (type == LUA_TNIL) {
            break;
        }
        ++n;
    }
    if (n == 0) {
        return 0;
    }
    std::size_t keys = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        ++keys;
        lua_pop(state, 1);
    }
    return keys == n ? n : 0;
}
} // namespace

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
        if (const auto length = sequenceLength(state, index); length > 0) {
            Json out = Json::array();
            for (std::size_t i = 1; i <= length; ++i) {
                lua_rawgeti(state, index, static_cast<lua_Integer>(i));
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
