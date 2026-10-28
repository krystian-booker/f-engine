#pragma once
#include "engine/nav/Grid.h"
#include "engine/text/TextSystem.h"
#include <glm/glm.hpp>
#include <optional>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>

namespace fe::lua {
// Vectors cross the boundary as {x=,y=,z=} tables; arrays {1,2,3} are also
// accepted on input.
inline float component(const sol::table& t, const char* key, int index, float fallback) {
    sol::optional<float> named = t[key];
    if (named) {
        return *named;
    }
    sol::optional<float> positional = t[index];
    return positional.value_or(fallback);
}

inline glm::vec3 vec3(const sol::object& value, glm::vec3 fallback = glm::vec3(0)) {
    if (value.get_type() != sol::type::table) {
        return fallback;
    }
    const sol::table t = value;
    return {component(t, "x", 1, fallback.x), component(t, "y", 2, fallback.y),
            component(t, "z", 3, fallback.z)};
}

inline glm::vec2 vec2(const sol::object& value, glm::vec2 fallback = glm::vec2(0)) {
    if (value.get_type() != sol::type::table) {
        return fallback;
    }
    const sol::table t = value;
    return {component(t, "x", 1, fallback.x), component(t, "y", 2, fallback.y)};
}

// Ground-plane vector {x=,z=} (also accepts {x=,y=} or {1,2}).
inline glm::vec2 vecXZ(const sol::object& value, glm::vec2 fallback = glm::vec2(0)) {
    if (value.get_type() != sol::type::table) {
        return fallback;
    }
    const sol::table t = value;
    sol::optional<float> z = t["z"];
    return {component(t, "x", 1, fallback.x), z ? *z : component(t, "y", 2, fallback.y)};
}

inline sol::table table(sol::state_view lua, glm::vec3 v) {
    return lua.create_table_with("x", v.x, "y", v.y, "z", v.z);
}
inline sol::table table(sol::state_view lua, glm::vec2 v) {
    return lua.create_table_with("x", v.x, "y", v.y);
}
inline sol::table tableXZ(sol::state_view lua, glm::vec2 v) {
    return lua.create_table_with("x", v.x, "z", v.y);
}

inline GridPos cell(const sol::object& value) {
    if (value.get_type() != sol::type::table) {
        throw std::runtime_error("Expected a grid cell {x=, z=, layer=}");
    }
    const sol::table t = value;
    sol::optional<int> z = t["z"];
    return {static_cast<int>(component(t, "x", 1, 0)), z ? *z : static_cast<int>(component(t, "y", 2, 0)),
            t.get_or("layer", 0)};
}

inline sol::table table(sol::state_view lua, GridPos p) {
    return lua.create_table_with("x", p.x, "z", p.z, "layer", p.layer);
}

// Colors: 0xRRGGBBAA integers, "#rrggbb[aa]" or named strings, or
// {r,g,b[,a]} tables with 0..1 components (values above 1 are HDR).
inline glm::vec4 color(const sol::object& value, glm::vec4 fallback = glm::vec4(1)) {
    switch (value.get_type()) {
    case sol::type::number: {
        const auto rgba = static_cast<std::uint32_t>(value.as<double>());
        return {static_cast<float>((rgba >> 24u) & 0xffu) / 255.0f,
                static_cast<float>((rgba >> 16u) & 0xffu) / 255.0f,
                static_cast<float>((rgba >> 8u) & 0xffu) / 255.0f, static_cast<float>(rgba & 0xffu) / 255.0f};
    }
    case sol::type::string: {
        static const ColorPalette palette;
        const auto found = palette.find(value.as<std::string>());
        if (!found) {
            throw std::runtime_error("Unknown color '" + value.as<std::string>() + "'");
        }
        return color(sol::make_object(value.lua_state(), static_cast<double>(*found)));
    }
    case sol::type::table: {
        const sol::table t = value;
        return {component(t, "r", 1, fallback.r), component(t, "g", 2, fallback.g),
                component(t, "b", 3, fallback.b), component(t, "a", 4, fallback.a)};
    }
    default:
        return fallback;
    }
}

inline std::uint32_t rgba(const sol::object& value, std::uint32_t fallback = 0xffffffff) {
    if (value.get_type() == sol::type::lua_nil || !value.valid()) {
        return fallback;
    }
    const auto c = glm::clamp(color(value), glm::vec4(0), glm::vec4(1));
    return (static_cast<std::uint32_t>(c.r * 255 + 0.5f) << 24u) |
           (static_cast<std::uint32_t>(c.g * 255 + 0.5f) << 16u) |
           (static_cast<std::uint32_t>(c.b * 255 + 0.5f) << 8u) |
           static_cast<std::uint32_t>(c.a * 255 + 0.5f);
}

inline sol::table colorTable(sol::state_view lua, glm::vec4 c) {
    return lua.create_table_with(1, c.r, 2, c.g, 3, c.b, 4, c.a);
}

template<class T> T get(const sol::table& t, const char* key, T fallback) {
    sol::optional<T> value = t[key];
    return value.value_or(fallback);
}

inline std::vector<std::string> strings(const sol::object& value) {
    std::vector<std::string> out;
    if (value.get_type() == sol::type::string) {
        out.push_back(value.as<std::string>());
    } else if (value.get_type() == sol::type::table) {
        const sol::table t = value;
        for (std::size_t i = 1; i <= t.size(); ++i) {
            out.push_back(t.get<std::string>(i));
        }
    }
    return out;
}
} // namespace fe::lua
