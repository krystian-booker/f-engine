#pragma once
#include "engine/script/Binder.h"
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fe {
struct GridPos;
}

namespace fe::lua {
// Conversions raise a Lua error for values of the wrong type, non-finite
// numbers and fractions where an integer is expected; only a missing (nil)
// optional value falls back to a default. Tables scripts pass in are read
// raw (field(), raw element reads): no metamethod ever runs, so no script code
// runs while a binding holds engine references. The bodies live in
// Convert.cpp; only the templates are here.
using fe::field;

inline bool absent(const sol::object& value) {
    return !value.valid() || value.get_type() == sol::type::lua_nil;
}

// Whole numbers print in full ("1000000", not "1e+06"), others as iostreams do.
std::string format(double value);

// What a script passed, for error messages: "a string", "nil", "1.5".
std::string describe(const sol::object& value);

// `value` as a T: bool, std::string, an integer type (whole numbers within
// its range) or a floating-point type (finite numbers). Empty when it is not.
template<class T> std::optional<T> convert(const sol::object& value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (value.get_type() == sol::type::boolean) {
            return value.as<bool>();
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (value.get_type() == sol::type::string) {
            return value.as<std::string>();
        }
    } else if constexpr (std::is_integral_v<T>) {
        if (value.get_type() == sol::type::number) {
            lua_State* state = value.lua_state();
            value.push(state);
            int exact = 0;
            const lua_Integer result = lua_tointegerx(state, -1, &exact);
            lua_pop(state, 1);
            const bool fits =
                std::is_signed_v<T>
                    ? result >= static_cast<lua_Integer>(std::numeric_limits<T>::min()) &&
                          result <= static_cast<lua_Integer>(std::numeric_limits<T>::max())
                    : result >= 0 && static_cast<std::uint64_t>(result) <= std::numeric_limits<T>::max();
            if (exact && fits) {
                return static_cast<T>(result);
            }
        }
    } else {
        static_assert(std::is_floating_point_v<T>, "unsupported conversion");
        if (value.get_type() == sol::type::number) {
            const double result = value.as<double>();
            if (std::isfinite(result) &&
                std::abs(result) <= static_cast<double>(std::numeric_limits<T>::max())) {
                return static_cast<T>(result);
            }
        }
    }
    return std::nullopt;
}

template<class T> constexpr const char* expected() {
    if constexpr (std::is_same_v<T, bool>) {
        return "a boolean";
    } else if constexpr (std::is_same_v<T, std::string>) {
        return "a string";
    } else if constexpr (std::is_integral_v<T>) {
        return "an integer";
    } else {
        return "a finite number";
    }
}

// An argument that must be a T; `what` names it in the error.
template<class T> T as(const sol::object& value, const char* what) {
    if (auto converted = convert<T>(value)) {
        return *converted;
    }
    throw std::runtime_error(std::string(what) + " must be " + expected<T>() + ", got " + describe(value));
}

// An optional argument: empty when nil, otherwise it must be a T.
template<class T> std::optional<T> opt(const sol::object& value, const char* what) {
    if (absent(value)) {
        return std::nullopt;
    }
    return as<T>(value, what);
}

// t[key] as a T, or the fallback when it is nil; any other type raises.
template<class T> T get(const sol::table& t, const char* key, T fallback) {
    const sol::object value = field(t, key);
    if (absent(value)) {
        return fallback;
    }
    if (auto converted = convert<T>(value)) {
        return *converted;
    }
    throw std::runtime_error(std::string("Field '") + key + "' must be " + expected<T>() + ", got " +
                             describe(value));
}

// Integers from Lua, rejecting fractions and out-of-range numbers.
std::int64_t integer(const sol::object& value, const char* what);

// `value` when low <= value <= high, otherwise an error naming the range.
template<class T> T within(T value, T low, T high, const char* what) {
    if (!(value >= low && value <= high)) {
        throw std::runtime_error(std::string(what) + " must be from " + format(static_cast<double>(low)) +
                                 " to " + format(static_cast<double>(high)) + ", got " +
                                 format(static_cast<double>(value)));
    }
    return value;
}
template<class T> T atLeast(T value, T low, const char* what) {
    if (!(value >= low)) {
        throw std::runtime_error(std::string(what) + " must be at least " + format(static_cast<double>(low)) +
                                 ", got " + format(static_cast<double>(value)));
    }
    return value;
}

// Getters that share a name with a setter's old spelling reject extra arguments, so
// `fe.audio.bus_volume("music", 0.5)` raises instead of silently reading.
// (Functions and methods that take no arguments get this from the Binder.)
void atMostArguments(lua_State* state, int count, const char* hint);

// An optional table argument (an options or settings table): empty when nil.
std::optional<sol::table> options(const sol::object& value, const char* what);
sol::table tableArg(const sol::object& value, const char* what);

// Length of a sequence, ignoring __len.
std::size_t length(const sol::table& t);

// The raw elements 1..n of a sequence (no metamethods run).
std::vector<sol::object> elements(const sol::table& t);

// The world bound: every component of a vector, position, rectangle or
// colour a script passes lies within -VectorBound..VectorBound, so world
// coordinates (and anything derived from them) stay far from the limits of
// float precision and of the integer grid and hash coordinates they map to.
constexpr float VectorBound = 1e6f;

// Vectors cross the boundary as {x=,y=,z=} tables; arrays {1,2,3} are also
// accepted on input. Missing components take the fallback, but a table with
// none of the components (an options table passed where a vector belongs) is
// an error.
float component(const sol::table& t, const char* key, int index, float fallback);

// The vector table, which must hold at least one of `keys` (or element 1).
sol::table vectorTable(const sol::object& value, const char* what,
                       std::initializer_list<const char*> keys = {});

// Offsets and directions: missing components are 0 (or the fallback).
glm::vec3 vec3(const sol::object& value, glm::vec3 fallback);
glm::vec3 vec3(const sol::object& value);

// World positions: x and z are required, y is optional (0; spawn and place
// put the entity on the ground when it is missing).
glm::vec3 position(const sol::object& value);

glm::vec2 vec2(const sol::object& value, glm::vec2 fallback);

// Ground-plane vector {x=,z=} (also accepts {1, 2}).
glm::vec2 vecXZ(const sol::object& value, glm::vec2 fallback);
glm::vec2 vecXZ(const sol::object& value);

// A rectangle {x, y, w, h} (or {1, 2, 3, 4}) in canvas or image pixels.
glm::vec4 rect(const sol::object& value);

sol::table table(sol::state_view lua, glm::vec3 v);
sol::table table(sol::state_view lua, glm::vec2 v);
sol::table tableXZ(sol::state_view lua, glm::vec2 v);

// Grid cells are {x, z, layer} (or {x, z, layer} positionally) with integer,
// 0-based coordinates; layer defaults to 0.
int cellCoordinate(const sol::table& t, const char* key, int index, bool required);
GridPos cell(const sol::object& value);
sol::table table(sol::state_view lua, GridPos p);

// A list of cells (raw elements).
std::vector<GridPos> cells(const sol::object& value, const char* what);

glm::vec4 unpackColor(std::uint32_t rgba);

// Colors: 0xRRGGBBAA integers, "#rrggbb[aa]" or named strings, or
// {r,g,b[,a]} tables with 0..1 components (values above 1 are HDR). Tables
// may be positional ({1, 0.5, 0, 1}), which is the form getters return.
glm::vec4 color(const sol::object& value, glm::vec4 fallback = glm::vec4(1));
std::uint32_t rgba(const sol::object& value, std::uint32_t fallback = 0xffffffff);
sol::table colorTable(sol::state_view lua, glm::vec4 c);

// Settings tables that the engine reads as JSON (lights, emitters, the
// environment) name some of their fields' shapes, so those fields accept the
// documented vector and colour forms ({x, y, z} or {1, 2, 3}; 0xRRGGBBAA,
// names, '#rrggbb' or {r, g, b, a}) and reach the JSON readers as arrays.
enum class Shape {
    Vector, // {x, y, z}: missing components are 0
    Rgb,    // a colour without its alpha
    Rgba,   // a colour
};
struct ShapedField {
    // Dotted path below the settings table; "*" stands for every element of
    // a list ("sky.backdrops.*.tint").
    const char* path;
    Shape shape;
};

// `t` as JSON (luaToJson) with the named fields converted to arrays.
Json settingsJson(const sol::table& t, std::initializer_list<ShapedField> fields);
Json lightJson(const sol::table& t);
Json emitterJson(const sol::table& t);
Json environmentJson(const sol::table& t);

// Nothing (nil), a string or a sequence of strings (raw elements).
std::vector<std::string> strings(const sol::object& value);

// One of a fixed set of names, e.g. a sprite mode.
template<class T, std::size_t N>
T choice(const std::string& name, const std::pair<const char*, T> (&options)[N], const char* what) {
    std::string known;
    for (const auto& [option, result] : options) {
        if (name == option) {
            return result;
        }
        known += (known.empty() ? "'" : ", '") + std::string(option) + "'";
    }
    throw std::runtime_error("Unknown " + std::string(what) + " '" + name + "' (expected " + known + ")");
}
} // namespace fe::lua
