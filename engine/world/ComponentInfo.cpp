#include "engine/world/ComponentInfo.h"
#include "engine/assets/Model.h"
#include "engine/core/Paths.h"
#include "engine/editor/MapEdits.h"
#include "engine/world/World.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/gtc/constants.hpp>
#include <stdexcept>
#include <utility>

namespace fe {
namespace {
// The limits scripts are held to (EntityBindings.cpp, Convert.h), so an edit
// made here is one a script could have made.
constexpr float Bound = 1e6f;       // coordinates, colours and other free numbers
constexpr float MaxSpeed = 1000.0f; // movement speeds, units per second

constexpr std::pair<const char*, SpriteMode> SpriteModes[] = {{"upright", SpriteMode::Upright},
                                                              {"flat", SpriteMode::Flat},
                                                              {"facing", SpriteMode::Facing},
                                                              {"decal", SpriteMode::Decal},
                                                              {"standing", SpriteMode::Standing}};
constexpr const char* MotionNames[] = {"none", "route", "jump", "climb", "launch"}; // in Motion's order

[[noreturn]] void fail(const char* key, const std::string& problem) {
    throw std::runtime_error(std::string("Field '") + key + "' " + problem);
}

std::string format(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", value);
    return text;
}

// The edited value of a field, or null when the edit leaves the field alone.
const Json* field(const Json& edit, const char* key) {
    const auto found = edit.find(key);
    return found == edit.end() ? nullptr : &*found;
}

// The readers below change `value` only when the edit has the key, and throw
// (naming the field) when what it holds is of another type or out of range.
void number(const Json& edit, const char* key, float& value, float low = -Bound, float high = Bound) {
    const auto* given = field(edit, key);
    if (!given) {
        return;
    }
    const double read = given->is_number() ? given->get<double>() : std::nan("");
    if (!(read >= static_cast<double>(low) && read <= static_cast<double>(high))) {
        fail(key, "must be a number from " + format(low) + " to " + format(high) + ", got " + given->dump());
    }
    value = static_cast<float>(read);
}

// Whole numbers, also when written as a float (2.0).
void integer(const Json& edit, const char* key, int& value, int low, int high) {
    const auto* given = field(edit, key);
    if (!given) {
        return;
    }
    const double read = given->is_number() ? given->get<double>() : std::nan("");
    if (!(read >= low && read <= high) || std::floor(read) != read) {
        fail(key,
             "must be a whole number from " + format(low) + " to " + format(high) + ", got " + given->dump());
    }
    value = static_cast<int>(read);
}

void boolean(const Json& edit, const char* key, bool& value) {
    if (const auto* given = field(edit, key)) {
        if (!given->is_boolean()) {
            fail(key, "must be true or false, got " + given->dump());
        }
        value = given->get<bool>();
    }
}

void text(const Json& edit, const char* key, std::string& value) {
    if (const auto* given = field(edit, key)) {
        if (!given->is_string()) {
            fail(key, "must be a string, got " + given->dump());
        }
        value = given->get<std::string>();
    }
}

// An array of `count` numbers within the bound (`fewest` to `count` of them
// when a tail may be left out, as a colour's alpha).
template<class Vector>
void numbers(const Json& edit, const char* key, Vector& value, std::size_t fewest, const char* shape) {
    const auto* given = field(edit, key);
    if (!given) {
        return;
    }
    const auto count = static_cast<std::size_t>(value.length());
    if (!given->is_array() || given->size() < fewest || given->size() > count) {
        fail(key, std::string("must be ") + shape + ", got " + given->dump());
    }
    for (std::size_t i = 0; i < given->size(); ++i) {
        const auto& part = (*given)[i];
        const double read = part.is_number() ? part.get<double>() : std::nan("");
        if (!(read >= static_cast<double>(-Bound) && read <= static_cast<double>(Bound))) {
            fail(key, std::string("must be ") + shape + " of numbers up to 1000000, got " + given->dump());
        }
        value[static_cast<glm::length_t>(i)] = static_cast<float>(read);
    }
}
void vector(const Json& edit, const char* key, glm::vec2& value) {
    numbers(edit, key, value, 2, "[x, y]");
}
void vector(const Json& edit, const char* key, glm::vec3& value) {
    numbers(edit, key, value, 3, "[x, y, z]");
}
// [r, g, b] keeps the alpha at 1, as in map files.
void color(const Json& edit, const char* key, glm::vec4& value) {
    if (const auto* given = field(edit, key); given && given->is_array() && given->size() == 3) {
        value.a = 1;
    }
    numbers(edit, key, value, 3, "[r, g, b, a]");
}

void blend(const Json& edit, const char* key, BlendMode& value) {
    if (const auto* given = field(edit, key)) {
        if (!given->is_string() && !given->is_boolean()) {
            fail(key, "must be a blend mode name, got " + given->dump());
        }
        value = blendModeFromJson(*given); // throws for an unknown name, listing them
    }
}

// A game shader name, or "" for the engine's own.
void shader(const Json& edit, const char* key, std::string& value) {
    text(edit, key, value);
    if (!value.empty() && !isShaderName(value)) {
        fail(key, "'" + value + "' must use a-z, 0-9, _ and - (at most 64)");
    }
}

Json strings(const std::vector<std::string>& values) {
    Json list = Json::array();
    for (const auto& value : values) {
        list.push_back(value);
    }
    return list;
}

void strings(const Json& edit, const char* key, std::vector<std::string>& values) {
    const auto* given = field(edit, key);
    if (!given) {
        return;
    }
    if (!given->is_array()) {
        fail(key, "must be a list of strings, got " + given->dump());
    }
    std::vector<std::string> read;
    for (const auto& entry : *given) {
        if (!entry.is_string()) {
            fail(key, "must be a list of strings, got " + given->dump());
        }
        // Each name once, as Entity:add_tag keeps them.
        if (auto name = entry.get<std::string>(); std::find(read.begin(), read.end(), name) == read.end()) {
            read.push_back(std::move(name));
        }
    }
    values = std::move(read);
}

// Grid tag names as the bit mask movers keep; unknown names are refused.
void gridTags(const Json& edit, const char* key, const Grid& grid, std::uint32_t& mask) {
    if (!field(edit, key)) {
        return;
    }
    std::vector<std::string> names;
    strings(edit, key, names);
    for (const auto& name : names) {
        if (grid.tagMask({name}) == 0) {
            fail(key, "names '" + name + "', which is not a tag of this map's grid");
        }
    }
    mask = grid.tagMask(names);
}

template<class T> const T* find(const World& world, entt::entity entity) {
    return world.valid(entity) ? world.registry().try_get<T>(entity) : nullptr;
}

// ---- name

Json readName(const World&, entt::entity, const Name& name) {
    return {{"id", name.id}};
}
void applyName(World&, entt::entity, Name& name, const Json& edit) {
    text(edit, "id", name.id);
    if (name.id.empty()) {
        fail("id", "cannot be empty");
    }
}

// ---- tags

Json readTags(const World&, entt::entity, const Tags& tags) {
    return {{"values", strings(tags.values)}};
}
void applyTags(World&, entt::entity, Tags& tags, const Json& edit) {
    strings(edit, "values", tags.values);
}

// ---- transform

Json readTransform(const World& world, entt::entity, const Transform& t) {
    const auto* parent = find<Name>(world, t.parent);
    return {{"position", toJson(t.position)}, {"layer", t.layer},
            {"facing", toJson(t.facing)},     {"yaw", glm::degrees(t.yaw)},
            {"scale", toJson(t.scale)},       {"parent", parent ? Json(parent->id) : Json()},
            {"offset", toJson(t.offset)},     {"ground", t.ground}};
}
void applyTransform(World& world, entt::entity, Transform& t, const Json& edit) {
    vector(edit, "position", t.position);
    // Canonical on a wrapping map, as World::spawn keeps it.
    const auto flat = world.grid().wrapPoint({t.position.x, t.position.z});
    t.position = {flat.x, t.position.y, flat.y};
    integer(edit, "layer", t.layer, 0, world.grid().layerCount() - 1);
    auto facing = t.facing;
    vector(edit, "facing", facing);
    if (facing != t.facing) {
        if (glm::length(facing) < 1e-4f) {
            fail("facing", "needs a direction [x, z], not zero");
        }
        t.facing = glm::normalize(facing);
    }
    // In degrees; an angle that was not edited keeps its exact radians.
    float degrees = glm::degrees(t.yaw);
    number(edit, "yaw", degrees);
    if (degrees != glm::degrees(t.yaw)) {
        t.yaw = glm::radians(degrees);
    }
    vector(edit, "scale", t.scale);
    vector(edit, "offset", t.offset);
    boolean(edit, "ground", t.ground);
}

// ---- sprite

Json readSprite(const World&, entt::entity, const Sprite& s) {
    return {{"animation", s.animation},
            {"clip", s.clip},
            {"move_clip", s.moveClip},
            {"idle_clip", s.idleClip},
            {"time", s.time},
            {"speed", s.speed},
            {"direction", s.direction},
            {"visible", s.visible},
            {"blend", blendModeName(s.blend)},
            {"unlit", s.unlit},
            {"cast_shadow", s.castShadow},
            {"mode", SpriteModes[static_cast<std::size_t>(s.mode)].first},
            {"scale", s.scale},
            {"depth_bias", s.depthBias},
            {"offset", toJson(s.pixelOffset)},
            {"tint", toJson(s.tint)},
            {"flash", toJson(s.flash)},
            {"outline", toJson(s.outline)},
            {"outline_width", s.outlineWidth},
            {"silhouette", toJson(s.silhouette)},
            {"pixel_snap", s.pixelSnap ? Json(*s.pixelSnap) : Json()},
            {"palette", s.palette},
            {"palette_row", s.paletteRow},
            {"distort", s.distort},
            {"shader", s.shader},
            {"finished", s.finished}};
}
void applySprite(World&, entt::entity, Sprite& s, const Json& edit) {
    const auto previousClip = s.clip;
    text(edit, "animation", s.animation);
    text(edit, "clip", s.clip);
    text(edit, "move_clip", s.moveClip);
    text(edit, "idle_clip", s.idleClip);
    number(edit, "speed", s.speed, -1000.0f, 1000.0f);
    integer(edit, "direction", s.direction, -1, 1 << 20);
    boolean(edit, "visible", s.visible);
    blend(edit, "blend", s.blend);
    boolean(edit, "unlit", s.unlit);
    boolean(edit, "cast_shadow", s.castShadow);
    if (field(edit, "mode")) {
        std::string mode;
        text(edit, "mode", mode);
        const auto* found = std::find_if(std::begin(SpriteModes), std::end(SpriteModes),
                                         [&](const auto& entry) { return mode == entry.first; });
        if (found == std::end(SpriteModes)) {
            fail("mode", "must be upright, flat, facing, decal or standing, got '" + mode + "'");
        }
        s.mode = found->second;
    }
    number(edit, "scale", s.scale, 0.0f, 1000.0f);
    number(edit, "depth_bias", s.depthBias);
    vector(edit, "offset", s.pixelOffset);
    color(edit, "tint", s.tint);
    color(edit, "flash", s.flash);
    color(edit, "outline", s.outline);
    number(edit, "outline_width", s.outlineWidth, 0.5f, 4.0f);
    color(edit, "silhouette", s.silhouette);
    // null: back to the environment's pixel_snap.
    if (const auto* snap = field(edit, "pixel_snap"); snap && snap->is_null()) {
        s.pixelSnap.reset();
    } else if (snap) {
        bool value = false;
        boolean(edit, "pixel_snap", value);
        s.pixelSnap = value;
    }
    text(edit, "palette", s.palette);
    integer(edit, "palette_row", s.paletteRow, 0, 1 << 20);
    number(edit, "distort", s.distort, 0.0f, Bound);
    shader(edit, "shader", s.shader);
    // Another clip starts from its beginning, as when a script sets it.
    if (s.clip != previousClip) {
        s.time = s.previousTime = 0;
        s.finished = false;
    }
}

// ---- mesh

Json readMesh(const World&, entt::entity, const MeshInstance& m) {
    return {{"model", m.model},       {"animation", m.animation},
            {"time", m.time},         {"speed", m.speed},
            {"loop", m.loop},         {"playing", m.playing},
            {"visible", m.visible},   {"cast_shadow", m.castShadow},
            {"tint", toJson(m.tint)}, {"blend", m.blend ? Json(blendModeName(*m.blend)) : Json()},
            {"shader", m.shader},     {"from_animation", m.fromAnimation},
            {"fade", m.fade},         {"fade_elapsed", m.fadeElapsed}};
}
void applyMesh(World&, entt::entity, MeshInstance& m, const Json& edit) {
    text(edit, "model", m.model);
    text(edit, "animation", m.animation);
    number(edit, "speed", m.speed, -1000.0f, 1000.0f);
    boolean(edit, "loop", m.loop);
    boolean(edit, "playing", m.playing);
    boolean(edit, "visible", m.visible);
    boolean(edit, "cast_shadow", m.castShadow);
    color(edit, "tint", m.tint);
    // null: each glTF material's own alpha mode.
    if (const auto* given = field(edit, "blend"); given && given->is_null()) {
        m.blend.reset();
    } else if (given) {
        auto mode = BlendMode::Opaque;
        blend(edit, "blend", mode);
        m.blend = mode;
    }
    shader(edit, "shader", m.shader);
}

// ---- light and emitter: their settings in the map file's JSON, checked by its loader

// `settings` with the edit's fields over it, read back through the loader.
template<class Settings> Settings merged(const Settings& settings, const Json& edit) {
    Json json = settings.toJson();
    json.update(edit);
    return Settings::fromJson(json);
}

Json readLight(const World&, entt::entity, const LightSource& source) {
    Json json = source.light.toJson();
    json["enabled"] = source.enabled;
    return json;
}
void applyLight(World&, entt::entity, LightSource& source, const Json& edit) {
    // The loader reads any type but "spot" as a point light.
    std::string type = "point";
    text(edit, "type", type);
    if (type != "point" && type != "spot") {
        fail("type", "must be point or spot, got '" + type + "'");
    }
    source.light = merged(source.light, edit);
    boolean(edit, "enabled", source.enabled);
}

Json readEmitter(const World& world, entt::entity entity, const Emitter& emitter) {
    Json json = emitter.settings.toJson();
    json["paused"] = !emitter.active;
    json["live"] = world.liveParticles(entity);
    json["burst_done"] = emitter.burstDone;
    return json;
}
void applyEmitter(World&, entt::entity, Emitter& emitter, const Json& edit) {
    emitter.settings = merged(emitter.settings, edit);
    bool paused = !emitter.active;
    boolean(edit, "paused", paused);
    emitter.active = !paused;
}

// ---- collider

Json readCollider(const World&, entt::entity, const Collider& c) {
    return {{"radius", c.radius}, {"height", c.height}, {"solid", c.solid}};
}
void applyCollider(World&, entt::entity, Collider& c, const Json& edit) {
    number(edit, "radius", c.radius, 0.0f, 64.0f);
    number(edit, "height", c.height, 0.0f, 1000.0f);
    boolean(edit, "solid", c.solid);
}

// ---- mover

Json readMover(const World& world, entt::entity, const Mover& m) {
    const Grid& grid = world.map().grid;
    const auto* route = m.route();
    Json jump = false;
    if (m.jump.distance > 0) {
        jump = {{"distance", m.jump.distance},
                {"max_up", m.jump.maxUp},
                {"max_down", m.jump.maxDown},
                {"diagonal", m.jump.diagonal}};
    }
    return {{"speed", m.speed},
            {"max_step", m.maxStep},
            {"radius", m.radius},
            {"collide", m.collide},
            {"climb_speed", m.climbSpeed},
            {"footprint", m.footprint},
            {"fly", m.fly},
            {"altitude", m.altitude},
            {"altitude_speed", m.altitudeSpeed},
            {"blocked_tags", strings(grid.tagsOf(m.blockedTags))},
            {"required_tags", strings(grid.tagsOf(m.requiredTags))},
            {"jump", jump},
            {"lift", m.lift},
            {"motion", MotionNames[static_cast<std::size_t>(m.motion())]},
            {"waypoints", route ? route->waypoints.size() : std::size_t{0}},
            {"drive", toJson(m.drive)},
            {"moved", m.moved},
            {"cell", m.cell.layer < 0 ? Json() : Json{m.cell.x, m.cell.z, m.cell.layer}}};
}
void applyMover(World& world, entt::entity, Mover& m, const Json& edit) {
    number(edit, "speed", m.speed, 0.0f, MaxSpeed);
    number(edit, "max_step", m.maxStep, 0.0f, 1000.0f);
    number(edit, "radius", m.radius, 0.0f, 0.49f);
    boolean(edit, "collide", m.collide);
    number(edit, "climb_speed", m.climbSpeed, 0.0f, MaxSpeed);
    if (m.climbSpeed <= 0) {
        fail("climb_speed", "must be a positive number up to 1000");
    }
    integer(edit, "footprint", m.footprint, 1, MaxFootprint);
    boolean(edit, "fly", m.fly);
    number(edit, "altitude", m.altitude, 0.0f, 1000.0f);
    number(edit, "altitude_speed", m.altitudeSpeed, 0.0f, MaxSpeed);
    gridTags(edit, "blocked_tags", world.grid(), m.blockedTags);
    gridTags(edit, "required_tags", world.grid(), m.requiredTags);
    if (const auto* jump = field(edit, "jump"); jump && jump->is_object()) {
        // A mover without gap jumps starts from the limits a script's table
        // would: its step height both ways.
        JumpRules rules = m.jump.distance > 0 ? m.jump : JumpRules{0, m.maxStep, m.maxStep, false};
        integer(*jump, "distance", rules.distance, 0, 1 << 20);
        number(*jump, "max_up", rules.maxUp);
        number(*jump, "max_down", rules.maxDown);
        boolean(*jump, "diagonal", rules.diagonal);
        StepRules check;
        check.jump = rules;
        validateRules(check); // throws for limits the planner refuses
        m.jump = rules.distance > 0 ? rules : JumpRules{};
    } else if (jump && jump->is_boolean() && !jump->get<bool>()) {
        m.jump = {};
    } else if (jump) {
        fail("jump", "must be an object or false, got " + jump->dump());
    }
}

// ---- platform

Json readPlatform(const World&, entt::entity, const Platform& p) {
    return {{"size", toJson(p.size)}};
}
void applyPlatform(World&, entt::entity, Platform& p, const Json& edit) {
    vector(edit, "size", p.size);
    if (!(p.size.x > 0 && p.size.y > 0 && p.size.x <= 64 && p.size.y <= 64)) {
        fail("size", "must be above 0 and at most 64 on each axis");
    }
}

// ---- audio

Json readAudio(const World&, entt::entity, const AudioEmitter& a) {
    return {{"sound", a.sound}, {"radius", a.radius}, {"volume", a.volume}, {"playing", a.voice >= 0}};
}
void applyAudio(World& world, entt::entity, AudioEmitter& a, const Json& edit) {
    const auto previous = a.sound;
    text(edit, "sound", a.sound);
    if (a.sound.empty()) {
        fail("sound", "must name a sound file");
    }
    resolveInside(world.assets().root(), a.sound); // throws for a path outside the game folder
    number(edit, "radius", a.radius, 0.0f, Bound);
    if (a.radius <= 0) {
        fail("radius", "must be above 0 and at most 1000000");
    }
    number(edit, "volume", a.volume, 0.0f, Bound);
    // Another file: the emitter lets go of its voice, so the audio pass
    // fades the old sound out (no emitter claims it) and starts the new one.
    if (a.sound != previous) {
        a.voice = -1;
    }
}

// ---- shader params (a list of up to 16 numbers; missing entries are 0)

Json readParams(const World&, entt::entity, const MaterialParams& params) {
    Json values = Json::array();
    for (std::size_t i = 0; i < 16; ++i) {
        values.push_back(params.values[i / 4][static_cast<glm::length_t>(i % 4)]);
    }
    return {{"values", values}};
}
void applyParams(World&, entt::entity, MaterialParams& params, const Json& edit) {
    const auto* given = field(edit, "values");
    if (!given) {
        return;
    }
    if (!given->is_array() || given->size() > 16) {
        fail("values", "must be a list of at most 16 numbers");
    }
    MaterialParams read;
    for (std::size_t i = 0; i < given->size(); ++i) {
        const auto& value = (*given)[i];
        if (!value.is_number() || !std::isfinite(value.get<float>())) {
            fail("values", "must be a list of at most 16 numbers, got " + value.dump());
        }
        read.values[i / 4][static_cast<glm::length_t>(i % 4)] = value.get<float>();
    }
    params = read;
}

// ---- props

Json readProps(const World&, entt::entity, const Props& props) {
    return {{"value", props.value}};
}
void applyProps(World&, entt::entity, Props& props, const Json& edit) {
    if (const auto* given = field(edit, "value")) {
        if (!given->is_object()) {
            fail("value", "must be an object");
        }
        // Scripts read props as a Lua table, which nests no deeper.
        if (jsonDepth(*given) > maxJsonDepth) {
            fail("value", "is nested too deeply");
        }
        props.value = *given;
    }
}

// ---- storing: through the setter that keeps derived state right, where there is one

template<class T> void store(World& world, entt::entity entity, const T& component) {
    world.registry().emplace_or_replace<T>(entity, component);
}
// The world's index of ids follows; an id another entity has is refused.
void store(World& world, entt::entity entity, const Name& name) {
    world.rename(entity, name.id);
}
// Replaced through the registry (editCollider), so collision queries reach a
// changed radius at once.
void store(World& world, entt::entity entity, const Collider& collider) {
    if (world.registry().all_of<Collider>(entity)) {
        editCollider(world, entity, collider);
    } else {
        world.registry().emplace<Collider>(entity, collider);
    }
}
// A moved entity is re-indexed for collision and shown where it now is, with
// no interpolation from where it was (a paused game shows the edit).
void store(World& world, entt::entity entity, const Transform& edited) {
    auto& transform = world.registry().get<Transform>(entity);
    const bool moved = edited.position != transform.position || edited.layer != transform.layer;
    const bool turned = edited.yaw != transform.yaw;
    transform = edited;
    if (turned) {
        transform.previousYaw = transform.yaw;
    }
    if (moved) {
        editTransform(world, entity, edited.position, edited.layer);
    }
}

template<class T> bool has(const World& world, entt::entity entity) {
    return find<T>(world, entity) != nullptr;
}

// The info of a component read by `Read(world, entity, component)` and
// edited by `Apply(world, entity, copy, edit)`, which throws for an edit it
// refuses.
template<class T, auto Read, auto Apply>
ComponentInfo info(const char* name, bool removable, std::vector<std::string> readOnly = {}) {
    return {name,
            removable,
            &has<T>,
            [](const World& world, entt::entity entity) -> Json {
                const auto* current = find<T>(world, entity);
                return current ? Read(world, entity, *current) : Json();
            },
            [](World& world, entt::entity entity, const Json& edit) -> std::string {
                if (!world.valid(entity)) {
                    return "The entity no longer exists";
                }
                if (!edit.is_object()) {
                    return "A component is edited with an object of its fields";
                }
                try {
                    const auto* current = find<T>(world, entity);
                    T copy = current ? *current : T{};
                    Apply(world, entity, copy, edit);
                    store(world, entity, copy);
                } catch (const std::exception& error) {
                    return error.what();
                }
                return {};
            },
            std::move(readOnly)};
}

// A component shown but never edited: write() changes nothing.
template<class T> void keep(World&, entt::entity, T&, const Json&) {
}
template<class T, auto Read>
ComponentInfo shown(const char* name, bool removable, std::vector<std::string> readOnly = {}) {
    auto result = info<T, Read, &keep<T>>(name, removable, std::move(readOnly));
    result.write = [](World&, entt::entity, const Json&) -> std::string {
        return {};
    };
    return result;
}

Json readSerial(const World&, entt::entity, const EntitySerial& serial) {
    return {{"value", serial.value}};
}
Json readScriptRef(const World&, entt::entity, const ScriptRef&) {
    return Json::object();
}
} // namespace

const std::vector<ComponentInfo>& componentInfos() {
    static const std::vector<ComponentInfo> infos = [] {
        std::vector<ComponentInfo> list;
        list.push_back(info<Name, &readName, &applyName>("Name", false));
        list.push_back(info<Transform, &readTransform, &applyTransform>("Transform", false, {"parent"}));
        list.push_back(shown<EntitySerial, &readSerial>("EntitySerial", false, {"value"}));
        list.push_back(info<Tags, &readTags, &applyTags>("Tags", true));
        list.push_back(info<Sprite, &readSprite, &applySprite>("Sprite", true, {"time", "finished"}));
        list.push_back(info<MeshInstance, &readMesh, &applyMesh>(
            "MeshInstance", true, {"time", "from_animation", "fade", "fade_elapsed"}));
        list.push_back(info<LightSource, &readLight, &applyLight>("LightSource", true));
        list.push_back(info<Emitter, &readEmitter, &applyEmitter>("Emitter", true, {"live", "burst_done"}));
        list.push_back(info<Collider, &readCollider, &applyCollider>("Collider", true));
        list.push_back(info<Mover, &readMover, &applyMover>(
            "Mover", true, {"lift", "motion", "waypoints", "drive", "moved", "cell"}));
        list.push_back(info<Platform, &readPlatform, &applyPlatform>("Platform", true));
        list.push_back(info<AudioEmitter, &readAudio, &applyAudio>("AudioEmitter", true, {"playing"}));
        list.push_back(info<MaterialParams, &readParams, &applyParams>("MaterialParams", true));
        list.push_back(info<Props, &readProps, &applyProps>("Props", true));
        // Not removable: the reference it holds is released by the script
        // bindings when the entity is destroyed, not by the registry.
        list.push_back(shown<ScriptRef, &readScriptRef>("ScriptRef", false));
        return list;
    }();
    return infos;
}

std::vector<const ComponentInfo*> componentsOf(const World& world, entt::entity entity) {
    std::vector<const ComponentInfo*> present;
    for (const auto& component : componentInfos()) {
        if (component.has(world, entity)) {
            present.push_back(&component);
        }
    }
    return present;
}
} // namespace fe
