#include "engine/script/bindings/Convert.h"
#include "engine/nav/Grid.h"
#include "engine/text/Markup.h"
#include <cmath>
#include <sstream>
#include <string_view>

namespace fe::lua {
std::string format(double value) {
    if (std::isfinite(value) && std::floor(value) == value && std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string describe(const sol::object& value) {
    if (absent(value)) {
        return "nil";
    }
    if (value.get_type() == sol::type::number) {
        return format(value.as<double>());
    }
    const std::string name = sol::type_name(value.lua_state(), value.get_type());
    return (name.find_first_of("aeiou") == 0 ? "an " : "a ") + name;
}

std::int64_t integer(const sol::object& value, const char* what) {
    return as<std::int64_t>(value, what);
}

void atMostArguments(lua_State* state, int count, const char* hint) {
    if (lua_gettop(state) > count) {
        throw std::runtime_error(hint);
    }
}

std::optional<sol::table> options(const sol::object& value, const char* what) {
    if (absent(value)) {
        return std::nullopt;
    }
    if (value.get_type() != sol::type::table) {
        throw std::runtime_error(std::string(what) + " must be a table, got " + describe(value));
    }
    return value.as<sol::table>();
}

sol::table tableArg(const sol::object& value, const char* what) {
    if (value.get_type() != sol::type::table) {
        throw std::runtime_error(std::string(what) + " must be a table, got " + describe(value));
    }
    return value.as<sol::table>();
}

std::size_t length(const sol::table& t) {
    lua_State* state = t.lua_state();
    t.push(state);
    const auto size = lua_rawlen(state, -1);
    lua_pop(state, 1);
    return size;
}

std::vector<sol::object> elements(const sol::table& t) {
    std::vector<sol::object> out;
    const auto size = length(t);
    out.reserve(size);
    for (std::size_t i = 1; i <= size; ++i) {
        out.push_back(t.raw_get<sol::object>(i));
    }
    return out;
}

float component(const sol::table& t, const char* key, int index, float fallback) {
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

sol::table vectorTable(const sol::object& value, const char* what, std::initializer_list<const char*> keys) {
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

glm::vec3 vec3(const sol::object& value, glm::vec3 fallback) {
    if (absent(value)) {
        return fallback;
    }
    const auto t = vectorTable(value, "a {x, y, z}", {"x", "y", "z"});
    return {component(t, "x", 1, fallback.x), component(t, "y", 2, fallback.y),
            component(t, "z", 3, fallback.z)};
}

glm::vec3 vec3(const sol::object& value) {
    return vec3(vectorTable(value, "a {x, y, z}"), glm::vec3(0));
}

glm::vec3 position(const sol::object& value) {
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

glm::vec2 vec2(const sol::object& value, glm::vec2 fallback) {
    if (absent(value)) {
        return fallback;
    }
    const auto t = vectorTable(value, "an {x, y}", {"x", "y"});
    return {component(t, "x", 1, fallback.x), component(t, "y", 2, fallback.y)};
}

glm::vec2 vecXZ(const sol::object& value, glm::vec2 fallback) {
    if (absent(value)) {
        return fallback;
    }
    const auto t = vectorTable(value, "an {x, z}", {"x", "z"});
    return {component(t, "x", 1, fallback.x), component(t, "z", 2, fallback.y)};
}

glm::vec2 vecXZ(const sol::object& value) {
    return vecXZ(vectorTable(value, "an {x, z}"), glm::vec2(0));
}

glm::vec4 rect(const sol::object& value) {
    const auto t = vectorTable(value, "a rectangle {x, y, w, h}", {"x", "y", "w", "h"});
    return {component(t, "x", 1, 0), component(t, "y", 2, 0), component(t, "w", 3, 0),
            component(t, "h", 4, 0)};
}

sol::table table(sol::state_view lua, glm::vec3 v) {
    return lua.create_table_with("x", v.x, "y", v.y, "z", v.z);
}

sol::table table(sol::state_view lua, glm::vec2 v) {
    return lua.create_table_with("x", v.x, "y", v.y);
}

sol::table tableXZ(sol::state_view lua, glm::vec2 v) {
    return lua.create_table_with("x", v.x, "z", v.y);
}

int cellCoordinate(const sol::table& t, const char* key, int index, bool required) {
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

GridPos cell(const sol::object& value) {
    const auto t = vectorTable(value, "a grid cell {x, z, layer}");
    return {cellCoordinate(t, "x", 1, true), cellCoordinate(t, "z", 2, true),
            cellCoordinate(t, "layer", 3, false)};
}

sol::table table(sol::state_view lua, GridPos p) {
    return lua.create_table_with("x", p.x, "z", p.z, "layer", p.layer);
}

std::vector<GridPos> cells(const sol::object& value, const char* what) {
    std::vector<GridPos> out;
    for (const auto& element : elements(tableArg(value, what))) {
        out.push_back(cell(element));
    }
    return out;
}

glm::vec4 unpackColor(std::uint32_t rgba) {
    return {static_cast<float>((rgba >> 24u) & 0xffu) / 255.0f,
            static_cast<float>((rgba >> 16u) & 0xffu) / 255.0f,
            static_cast<float>((rgba >> 8u) & 0xffu) / 255.0f, static_cast<float>(rgba & 0xffu) / 255.0f};
}

glm::vec4 color(const sol::object& value, glm::vec4 fallback) {
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

std::uint32_t rgba(const sol::object& value, std::uint32_t fallback) {
    if (absent(value)) {
        return fallback;
    }
    const auto c = glm::clamp(color(value), glm::vec4(0), glm::vec4(1));
    return (static_cast<std::uint32_t>(c.r * 255 + 0.5f) << 24u) |
           (static_cast<std::uint32_t>(c.g * 255 + 0.5f) << 16u) |
           (static_cast<std::uint32_t>(c.b * 255 + 0.5f) << 8u) |
           static_cast<std::uint32_t>(c.a * 255 + 0.5f);
}

sol::table colorTable(sol::state_view lua, glm::vec4 c) {
    return lua.create_table_with(1, c.r, 2, c.g, 3, c.b, 4, c.a);
}

namespace {
Json shapedJson(const sol::object& value, Shape shape) {
    if (shape == Shape::Vector) {
        const auto v = vec3(value, glm::vec3(0));
        return Json::array({v.x, v.y, v.z});
    }
    const auto c = color(value);
    return shape == Shape::Rgb ? Json::array({c.r, c.g, c.b}) : Json::array({c.r, c.g, c.b, c.a});
}

void reshape(const sol::object& value, Json& json, std::string_view path, Shape shape) {
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
} // namespace

Json settingsJson(const sol::table& t, std::initializer_list<ShapedField> fields) {
    auto json = luaToJson(t);
    for (const auto& f : fields) {
        reshape(t, json, f.path, f.shape);
    }
    return json;
}

Json lightJson(const sol::table& t) {
    return settingsJson(t,
                        {{"position", Shape::Vector}, {"direction", Shape::Vector}, {"color", Shape::Rgb}});
}

Json emitterJson(const sol::table& t) {
    return settingsJson(t, {{"velocity", Shape::Vector},
                            {"velocity_jitter", Shape::Vector},
                            {"gravity", Shape::Vector},
                            {"area", Shape::Vector},
                            {"color", Shape::Rgba},
                            {"color_end", Shape::Rgba}});
}

Json environmentJson(const sol::table& t) {
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

std::vector<std::string> strings(const sol::object& value) {
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
} // namespace fe::lua
