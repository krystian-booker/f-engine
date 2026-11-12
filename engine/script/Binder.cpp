#include "engine/script/Binder.h"
#include "engine/core/Json.h"
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace fe {
namespace detail {
void badArgument(lua_State* state, const std::string& function, int index, const std::string& expected) {
    std::string actual;
    const int type = lua_type(state, index);
    if (type == LUA_TNONE) {
        actual = "no value";
    } else if (type == LUA_TNUMBER) {
        char number[32];
        if (lua_isinteger(state, index)) {
            std::snprintf(number, sizeof(number), "%lld",
                          static_cast<long long>(lua_tointeger(state, index)));
        } else {
            std::snprintf(number, sizeof(number), "%.14g", lua_tonumber(state, index));
        }
        actual = std::string("the number ") + number;
    } else if (type == LUA_TUSERDATA && luaL_getmetafield(state, index, "__name") == LUA_TSTRING) {
        // Engine types' metatables carry their documented names (LuaHost::sealTypes).
        actual = lua_tostring(state, -1);
        lua_pop(state, 1);
    } else {
        actual = lua_typename(state, type);
    }
    throw std::runtime_error("bad argument #" + std::to_string(index) + " to '" + function + "' (" +
                             expected + " expected, got " + actual + ")");
}

void noArguments(LuaHost& host, const std::string& function, bool method, int got) {
    std::string message = "'" + function + "' takes no arguments (got " + std::to_string(got) + ")";
    // "fe.app.fullscreen" -> is there an "fe.app.set_fullscreen"?
    const auto dot = function.rfind('.');
    if (!method && function.starts_with("fe.") && dot != std::string::npos) {
        sol::object owner = host.fe();
        std::size_t start = 3;
        while (owner.get_type() == sol::type::table && start < dot) {
            const auto end = std::min(function.find('.', start), dot);
            owner = field(owner.as<sol::table>(), function.substr(start, end - start));
            start = end + 1;
        }
        const std::string setter = "set_" + function.substr(dot + 1);
        if (owner.get_type() == sol::type::table &&
            field(owner.as<sol::table>(), setter).get_type() == sol::type::function) {
            message += "; it only reads, use " + function.substr(0, dot + 1) + setter + " to change it";
        }
    }
    throw std::runtime_error(message);
}
} // namespace detail

void BindingDocs::owner(const std::string& name, const std::string& doc, bool isClass) {
    const auto found =
        std::find_if(owners_.begin(), owners_.end(), [&](const Owner& o) { return o.name == name; });
    if (found == owners_.end()) {
        owners_.push_back({name, doc, isClass});
    }
}

int BindingDocs::signatureArity(const std::string& signature) {
    const auto open = signature.find('(');
    if (signature.rfind("fun", 0) != 0 || open == std::string::npos) {
        return -1;
    }
    int depth = 0, count = 0;
    bool any = false;
    for (std::size_t i = open + 1; i < signature.size(); ++i) {
        const char c = signature[i];
        if (c == '(' || c == '{' || c == '[' || c == '<') {
            ++depth;
        } else if (c == ')' && depth == 0) {
            return any ? count + 1 : 0;
        } else if (c == ')' || c == '}' || c == ']' || c == '>') {
            --depth;
        } else if (c == ',' && depth == 0) {
            ++count;
        } else if (c != ' ') {
            any = true;
        }
    }
    return -1;
}

void BindingDocs::add(Entry entry) {
    if (entry.arity >= 0 && (entry.kind == Kind::Function || entry.kind == Kind::Method)) {
        // An empty or non-function signature (test probes) is not checked.
        const int declared = signatureArity(entry.signature);
        if (declared >= 0 && declared != entry.arity) {
            const auto problem = entry.owner + (entry.kind == Kind::Method ? ":" : ".") + entry.name +
                                 ": the signature '" + entry.signature + "' declares " +
                                 std::to_string(declared) + " parameter(s) but the binding reads " +
                                 std::to_string(entry.arity);
#if FENGINE_DEV
            throw std::logic_error(problem);
#else
            problems_.push_back(problem);
#endif
        }
    }
    entries_.push_back(std::move(entry));
}

void BindingDocs::checkWritable() const {
    if (problems_.empty()) {
        return;
    }
    std::string message = "The API documentation disagrees with the bindings:";
    for (const auto& problem : problems_) {
        message += "\n  " + problem;
    }
    throw std::logic_error(message);
}

namespace {
std::string comment(const std::string& text) {
    std::istringstream lines(text);
    std::string out, line;
    while (std::getline(lines, line)) {
        out += "--- " + line + "\n";
    }
    return out;
}

std::string withSelf(const std::string& owner, const std::string& signature) {
    // "fun(a: x): y" -> "fun(self: Owner, a: x): y"
    const auto open = signature.find('(');
    if (signature.rfind("fun", 0) != 0 || open == std::string::npos) {
        return signature;
    }
    const bool empty = signature.size() > open + 1 && signature[open + 1] == ')';
    return signature.substr(0, open + 1) + "self: " + owner + (empty ? "" : ", ") +
           signature.substr(open + 1);
}
} // namespace

void BindingDocs::writeStubs(const std::filesystem::path& file) const {
    checkWritable();
    std::ostringstream out;
    out << "---@meta\n-- Generated by `fengine --write-api`; do not edit.\n\n";
    for (const auto& alias : aliases_) {
        out << comment(alias.doc) << "---@alias " << alias.name << " " << alias.type << "\n\n";
    }
    for (const auto& owner : owners_) {
        out << comment(owner.doc);
        if (owner.isClass) {
            for (const auto& e : entries_) {
                if (e.owner == owner.name && e.kind == Kind::Operator) {
                    out << "--- `" << e.name << "`: " << e.doc << "\n";
                }
            }
            out << "---@class " << owner.name << "\n";
            for (const auto& e : entries_) {
                if (e.owner == owner.name && (e.kind == Kind::Field || e.kind == Kind::ReadOnlyField)) {
                    out << "---@field " << e.name << " " << e.signature << " "
                        << (e.kind == Kind::ReadOnlyField ? "(read-only) " : "") << e.doc << "\n";
                }
            }
            out << "local " << owner.name << " = {}\n\n";
        } else {
            out << owner.name << " = " << owner.name << " or {}\n\n";
        }
        for (const auto& e : entries_) {
            if (e.owner != owner.name || (e.kind != Kind::Function && e.kind != Kind::Method)) {
                continue;
            }
            out << comment(e.doc) << "---@type "
                << (e.kind == Kind::Method ? withSelf(owner.name, e.signature) : e.signature) << "\n"
                << owner.name << "." << e.name << " = nil\n\n";
        }
    }
    writeTextAtomic(file, out.str());
}

void BindingDocs::writeMarkdown(const std::filesystem::path& file) const {
    checkWritable();
    std::ostringstream out;
    out << "# Engine API reference\n\nGenerated by `fengine --write-api` from the C++ bindings. These are "
           "the\n"
           "services the engine provides; every game rule is the game's own Lua (see "
           "docs/scripting.md).\n\n"
           "## Conventions\n\n"
           "- **Angles are degrees** everywhere: camera yaw, pitch and fov, `entity.yaw`, "
           "`world:spawn{yaw}`, canvas `rotation`, light cones and backdrop yaw.\n"
           "- **World positions, directions and grid cells are tables**, as arguments and results: positions "
           "`{x, y, z}` (x and z required; y is optional where an entity is put on the ground), ground-plane "
           "vectors `{x, z}`, grid cells `{x, z, layer}` with integer, 0-based coordinates (cell x covers "
           "world x..x+1; layer defaults to 0). Lists returned to Lua are 1-based.\n"
           "- **Canvas positions and sizes are plain numbers** (`fe.canvas.rect(x, y, w, h)`, "
           "`fe.input.mouse()` returns `x, y`, `camera:project` returns `x, y, visible`); a rectangle inside "
           "an options table, or an optional one, is a `Rect` table.\n"
           "- **Multiple results** are used for sizes and canvas positions (`w, h`, `x, y`) and for a few "
           "pairs of related values (`fe.save.read`: data, meta; `camera:ray`: "
           "origin, direction); everything else returns one value or one table.\n"
           "- **Getters are nouns and setters `set_<noun>`** (`fe.app.title()` / `fe.app.set_title()`, "
           "`fe.audio.bus_volume()` / `fe.audio.set_bus_volume()`, `world:cell()` / `world:set_cell()`). "
           "Plain values of handles are properties (`camera.yaw`, `entity.yaw`, `world.paused`); "
           "read-only ones are marked. Vector properties return a copy: assign a whole table "
           "(`camera.target = {x = 1, y = 0, z = 2}`); `camera.target.x = 1` changes nothing.\n"
           "- **Entity components** (`sprite`, `mesh`, `mover`, `light`, `emitter`, `collider`, `audio`, "
           "`platform`) follow one rule: `e:light{...}` adds the component or changes it and returns its "
           "new state as a table, `e:light()` with no argument reads that table (nil when the entity has "
           "no light; nothing is added, so `e:light{}` adds one with its defaults), and an explicit nil, "
           "`e:light(nil)`, removes it.\n"
           "- **Options tables** carry the optional settings of a call (`entity:move_to(target, {speed = "
           "3})`), after its required arguments. Every table a binding reads (options, settings, "
           "vectors, colours, cells, lists) is read raw: no metamethod runs, so fields must be in the table "
           "itself (one inherited through `__index` is not seen).\n"
           "- **Invalid input raises a Lua error**: arguments and option fields of the wrong type, unknown "
           "names (enums, input actions and axes), fractions where an integer is expected, integers outside "
           "the range the engine stores them in (`bad argument #3 to 'fe.text.paginate' (integer from "
           "-2147483648 to 2147483647 expected, got the number 9223372036854775807)`), NaN or infinite "
           "numbers and numbers beyond the float range (about 3.4e38), vectors without their components, "
           "colours that are not colours, paths containing a NUL character, and values outside a "
           "documented range. Leaving an optional value out (nil) uses its default. A few camera and "
           "canvas values are held within their range instead (camera pitch, canvas alpha), as noted. An "
           "argument of the wrong type raises `bad argument #n to 'World:spawn' (World expected, got "
           "Entity)`, naming the documented types.\n"
           "- **The world bound**: every component of a position, vector, rectangle or colour table is "
           "within -1000000 to 1000000. Sizes, counts and speeds have documented ranges (collider radius "
           "0 to 64, mover speed 0 to 1000, light radius at most 1000, overlays of at most 65536 cells, "
           "emitter `max` 1 to 16384...), so no single call can make the engine loop or allocate without "
           "end.\n\n"
           "## Types\n\n| Name | Type | Description |\n| --- | --- | --- |\n";
    for (const auto& alias : aliases_) {
        auto type = alias.type;
        std::replace(type.begin(), type.end(), '|', '/');
        out << "| `" << alias.name << "` | `" << type << "` | " << alias.doc << " |\n";
    }
    out << "\n";
    for (const auto& owner : owners_) {
        out << "## " << (owner.isClass ? "class " : "") << "`" << owner.name << "`\n\n"
            << owner.doc << "\n\n";
        out << "| Name | Type | Description |\n| --- | --- | --- |\n";
        for (const auto& e : entries_) {
            if (e.owner != owner.name) {
                continue;
            }
            auto signature = e.kind == Kind::Operator ? std::string("operator") : e.signature;
            std::replace(signature.begin(), signature.end(), '|', '/');
            const char* prefix = e.kind == Kind::Method                                   ? ":"
                                 : e.kind == Kind::Field || e.kind == Kind::ReadOnlyField ? "."
                                                                                          : "";
            out << "| `" << prefix << e.name << "` | `" << signature << "` | "
                << (e.kind == Kind::ReadOnlyField ? "Read-only. " : "") << e.doc << " |\n";
        }
        out << "\n";
    }
    writeTextAtomic(file, out.str());
}
} // namespace fe
