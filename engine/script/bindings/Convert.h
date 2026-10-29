#pragma once
#include "engine/nav/Grid.h"
#include "engine/script/LuaHost.h"
#include "engine/text/TextSystem.h"
#include <cmath>
#include <glm/glm.hpp>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sol/sol.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fe::lua {
// Conversions raise a Lua error for values of the wrong type, non-finite
// numbers and fractions where an integer is expected; only a missing (nil)
// optional value falls back to a default. Tables scripts pass in are read
// raw (field(), raw element reads): no metamethod ever runs, so no script code
// runs while a binding holds engine references.
using fe::field;

inline bool absent(const sol::object& value) {
    return !value.valid() || value.get_type() == sol::type::lua_nil;
}

// Whole numbers print in full ("1000000", not "1e+06"), others as iostreams do.
inline std::string format(double value) {
    if (std::isfinite(value) && std::floor(value) == value && std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    std::ostringstream out;
    out << value;
    return out.str();
}

// What a script passed, for error messages: "a string", "nil", "1.5".
inline std::string describe(const sol::object& value) {
    if (absent(value)) {
        return "nil";
    }
    if (value.get_type() == sol::type::number) {
        return format(value.as<double>());
    }
    const std::string name = sol::type_name(value.lua_state(), value.get_type());
    return (name.find_first_of("aeiou") == 0 ? "an " : "a ") + name;
}

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
inline std::int64_t integer(const sol::object& value, const char* what) {
    return as<std::int64_t>(value, what);
}

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
inline void atMostArguments(lua_State* state, int count, const char* hint) {
    if (lua_gettop(state) > count) {
        throw std::runtime_error(hint);
    }
}

// An optional table argument (an options or settings table): empty when nil.
inline std::optional<sol::table> options(const sol::object& value, const char* what) {
    if (absent(value)) {
        return std::nullopt;
    }
    if (value.get_type() != sol::type::table) {
        throw std::runtime_error(std::string(what) + " must be a table, got " + describe(value));
    }
    return value.as<sol::table>();
}
inline sol::table tableArg(const sol::object& value, const char* what) {
    if (value.get_type() != sol::type::table) {
        throw std::runtime_error(std::string(what) + " must be a table, got " + describe(value));
    }
    return value.as<sol::table>();
}

// Length of a sequence, ignoring __len.
inline std::size_t length(const sol::table& t) {
    lua_State* state = t.lua_state();
    t.push(state);
    const auto size = lua_rawlen(state, -1);
    lua_pop(state, 1);
    return size;
}

// The raw elements 1..n of a sequence (no metamethods run).
inline std::vector<sol::object> elements(const sol::table& t) {
    std::vector<sol::object> out;
    const auto size = length(t);
    out.reserve(size);
    for (std::size_t i = 1; i <= size; ++i) {
        out.push_back(t.raw_get<sol::object>(i));
    }
    return out;
}

// The world bound: every component of a vector, position, rectangle or
// colour a script passes lies within -VectorBound..VectorBound, so world
// coordinates (and anything derived from them) stay far from the limits of
// float precision and of the integer grid and hash coordinates they map to.
constexpr float VectorBound = 1e6f;

// Vectors cross the boundary as {x=,y=,z=} tables; arrays {1,2,3} are also
// accepted on input. Missing components take the fallback, but a table with
// none of the components (an options table passed where a vector belongs) is
// an error.
inline float component(const sol::table& t, const char* key, int index, float fallback) {
    sol::object value = field(t, key);
    if (absent(value) && index > 0) {
        value = field(t, index);
    }
    if (absent(value)) {
        return fallback;
    }
    if (auto number = convert<float>(value)) {
        if (std::abs(*number) > VectorBound) {
            throw std::runtime_error(std::string("Component '") + key +
                                     "' must be from -1000000 to 1000000, got " + describe(value));
        }
        return *number;
    }
    throw std::runtime_error(std::string("Component '") + key + "' must be a finite number, got " +
                             describe(value));
}

// The vector table, which must hold at least one of `keys` (or element 1).
inline sol::table vectorTable(const sol::object& value, const char* what,
                              std::initializer_list<const char*> keys = {}) {
    if (value.get_type() != sol::type::table) {
        throw std::runtime_error(std::string("Expected ") + what + " table, got " + describe(value));
    }
    const auto t = value.as<sol::table>();
    if (keys.size() == 0) {
        return t;
    }
    std::string names;
    for (const char* key : keys) {
        if (!absent(field(t, key))) {
            return t;
        }
        names += (names.empty() ? "" : ", ") + std::string(key);
    }
    if (!absent(field(t, 1))) {
        return t;
    }
    throw std::runtime_error(std::string("Expected ") + what + " table, got a table with none of " + names);
}

// Offsets and directions: missing components are 0 (or the fallback).
inline glm::vec3 vec3(const sol::object& value, glm::vec3 fallback) {
    if (absent(value)) {
        return fallback;
    }
    const auto t = vectorTable(value, "a {x, y, z}", {"x", "y", "z"});
    return {component(t, "x", 1, fallback.x), component(t, "y", 2, fallback.y),
            component(t, "z", 3, fallback.z)};
}
inline glm::vec3 vec3(const sol::object& value) {
    return vec3(vectorTable(value, "a {x, y, z}"), glm::vec3(0));
}

// World positions: x and z are required, y is optional (0; spawn and place
// put the entity on the ground when it is missing).
inline glm::vec3 position(const sol::object& value) {
    const auto t = vectorTable(value, "a position {x, y, z}");
    const auto required = [&t](const char* key, int index) {
        if (absent(field(t, key)) && absent(field(t, index))) {
            throw std::runtime_error(std::string("A position needs '") + key + "' (positions are {x, y, z})");
        }
        return component(t, key, index, 0);
    };
    const float x = required("x", 1);
    const float z = required("z", 3);
    return {x, component(t, "y", 2, 0), z};
}

inline glm::vec2 vec2(const sol::object& value, glm::vec2 fallback) {
    if (absent(value)) {
        return fallback;
    }
    const auto t = vectorTable(value, "an {x, y}", {"x", "y"});
    return {component(t, "x", 1, fallback.x), component(t, "y", 2, fallback.y)};
}

// Ground-plane vector {x=,z=} (also accepts {1, 2}).
inline glm::vec2 vecXZ(const sol::object& value, glm::vec2 fallback) {
    if (absent(value)) {
        return fallback;
    }
    const auto t = vectorTable(value, "an {x, z}", {"x", "z"});
    return {component(t, "x", 1, fallback.x), component(t, "z", 2, fallback.y)};
}
inline glm::vec2 vecXZ(const sol::object& value) {
    return vecXZ(vectorTable(value, "an {x, z}"), glm::vec2(0));
}

// A rectangle {x, y, w, h} (or {1, 2, 3, 4}) in canvas or image pixels.
inline glm::vec4 rect(const sol::object& value) {
    const auto t = vectorTable(value, "a rectangle {x, y, w, h}", {"x", "y", "w", "h"});
    return {component(t, "x", 1, 0), component(t, "y", 2, 0), component(t, "w", 3, 0),
            component(t, "h", 4, 0)};
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

// Grid cells are {x, z, layer} (or {x, z, layer} positionally) with integer,
// 0-based coordinates; layer defaults to 0.
inline int cellCoordinate(const sol::table& t, const char* key, int index, bool required) {
    sol::object value = field(t, key);
    if (absent(value)) {
        value = field(t, index);
    }
    if (absent(value)) {
        if (required) {
            throw std::runtime_error(std::string("A grid cell needs '") + key + "'");
        }
        return 0;
    }
    const auto coordinate = convert<int>(value);
    if (!coordinate || std::abs(*coordinate) > 1000000000) {
        throw std::runtime_error(std::string("Grid cell '") + key + "' must be an integer, got " +
                                 describe(value) + " (use entity:cell() or math.floor for a position)");
    }
    return *coordinate;
}

inline GridPos cell(const sol::object& value) {
    const auto t = vectorTable(value, "a grid cell {x, z, layer}");
    return {cellCoordinate(t, "x", 1, true), cellCoordinate(t, "z", 2, true),
            cellCoordinate(t, "layer", 3, false)};
}

inline sol::table table(sol::state_view lua, GridPos p) {
    return lua.create_table_with("x", p.x, "z", p.z, "layer", p.layer);
}

// A list of cells (raw elements).
inline std::vector<GridPos> cells(const sol::object& value, const char* what) {
    std::vector<GridPos> out;
    for (const auto& element : elements(tableArg(value, what))) {
        out.push_back(cell(element));
    }
    return out;
}

inline glm::vec4 unpackColor(std::uint32_t rgba) {
    return {static_cast<float>((rgba >> 24u) & 0xffu) / 255.0f,
            static_cast<float>((rgba >> 16u) & 0xffu) / 255.0f,
            static_cast<float>((rgba >> 8u) & 0xffu) / 255.0f, static_cast<float>(rgba & 0xffu) / 255.0f};
}

// Colors: 0xRRGGBBAA integers, "#rrggbb[aa]" or named strings, or
// {r,g,b[,a]} tables with 0..1 components (values above 1 are HDR). Tables
// may be positional ({1, 0.5, 0, 1}), which is the form getters return.
inline glm::vec4 color(const sol::object& value, glm::vec4 fallback = glm::vec4(1)) {
    switch (value.get_type()) {
    case sol::type::lua_nil:
    case sol::type::none:
        return fallback;
    case sol::type::number: {
        const auto packed = convert<std::int64_t>(value);
        if (!packed || *packed < 0 || *packed > 0xffffffffLL) {
            throw std::runtime_error("A color given as a number must be 0xRRGGBBAA (0 to 0xffffffff), got " +
                                     describe(value));
        }
        return unpackColor(static_cast<std::uint32_t>(*packed));
    }
    case sol::type::string: {
        static const ColorPalette palette;
        const auto found = palette.find(value.as<std::string>());
        if (!found) {
            throw std::runtime_error("Unknown color '" + value.as<std::string>() + "'");
        }
        return unpackColor(*found);
    }
    case sol::type::table: {
        const auto t = vectorTable(value, "a color {r, g, b, a}", {"r", "g", "b", "a"});
        return {component(t, "r", 1, fallback.r), component(t, "g", 2, fallback.g),
                component(t, "b", 3, fallback.b), component(t, "a", 4, fallback.a)};
    }
    default:
        throw std::runtime_error("Expected a color (0xRRGGBBAA, name or {r, g, b, a}), got " +
                                 describe(value));
    }
}

inline std::uint32_t rgba(const sol::object& value, std::uint32_t fallback = 0xffffffff) {
    if (absent(value)) {
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

namespace detail {
inline Json shapedJson(const sol::object& value, Shape shape) {
    if (shape == Shape::Vector) {
        const auto v = vec3(value, glm::vec3(0));
        return Json::array({v.x, v.y, v.z});
    }
    const auto c = color(value);
    return shape == Shape::Rgb ? Json::array({c.r, c.g, c.b}) : Json::array({c.r, c.g, c.b, c.a});
}

inline void reshape(const sol::object& value, Json& json, std::string_view path, Shape shape) {
    if (absent(value) || (!json.is_object() && !json.is_array())) {
        return;
    }
    const auto dot = path.find('.');
    const auto head = path.substr(0, dot);
    const auto rest = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
    if (value.get_type() != sol::type::table) {
        return;
    }
    const auto t = value.as<sol::table>();
    if (head == "*") {
        if (!json.is_array()) {
            return;
        }
        const auto list = elements(t);
        for (std::size_t i = 0; i < list.size() && i < json.size(); ++i) {
            reshape(list[i], json[i], rest, shape);
        }
        return;
    }
    const std::string key(head);
    const sol::object child = field(t, key);
    if (absent(child) || !json.is_object() || !json.contains(key)) {
        return;
    }
    if (rest.empty()) {
        json[key] = shapedJson(child, shape);
    } else {
        reshape(child, json[key], rest, shape);
    }
}
} // namespace detail

// `t` as JSON (luaToJson) with the named fields converted to arrays.
inline Json settingsJson(const sol::table& t, std::initializer_list<ShapedField> fields) {
    auto json = luaToJson(t);
    for (const auto& f : fields) {
        detail::reshape(t, json, f.path, f.shape);
    }
    return json;
}

inline Json lightJson(const sol::table& t) {
    return settingsJson(t,
                        {{"position", Shape::Vector}, {"direction", Shape::Vector}, {"color", Shape::Rgb}});
}
inline Json emitterJson(const sol::table& t) {
    return settingsJson(t, {{"velocity", Shape::Vector},
                            {"velocity_jitter", Shape::Vector},
                            {"gravity", Shape::Vector},
                            {"area", Shape::Vector},
                            {"color", Shape::Rgba},
                            {"color_end", Shape::Rgba}});
}
inline Json environmentJson(const sol::table& t) {
    return settingsJson(t, {{"ambient", Shape::Rgb},
                            {"sun_color", Shape::Rgb},
                            {"sun_direction", Shape::Vector},
                            {"fog_color", Shape::Rgb},
                            {"water.deep_color", Shape::Rgb},
                            {"sky.top", Shape::Rgb},
                            {"sky.horizon", Shape::Rgb},
                            {"sky.bottom", Shape::Rgb},
                            {"sky.cloud_color", Shape::Rgb},
                            {"sky.backdrops.*.tint", Shape::Rgba},
                            {"weather.wind", Shape::Vector},
                            {"weather.color", Shape::Rgba}});
}

// Nothing (nil), a string or a sequence of strings (raw elements).
inline std::vector<std::string> strings(const sol::object& value) {
    std::vector<std::string> out;
    if (value.get_type() == sol::type::string) {
        out.push_back(value.as<std::string>());
    } else if (value.get_type() == sol::type::table) {
        const auto list = elements(value.as<sol::table>());
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].get_type() != sol::type::string) {
                throw std::runtime_error("Expected a list of strings, element " + std::to_string(i + 1) +
                                         " is " + describe(list[i]));
            }
            out.push_back(list[i].as<std::string>());
        }
    } else if (!absent(value)) {
        throw std::runtime_error("Expected a string or a list of strings, got " + describe(value));
    }
    return out;
}

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
