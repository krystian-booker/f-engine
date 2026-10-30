#include "engine/core/Paths.h"
#include "engine/nav/Query.h"
#include "engine/script/bindings/Convert.h"
#include "engine/script/bindings/WorldTypes.h"
#include <type_traits>

namespace fe {
namespace {
// Movement speeds (units per second) scripts may set: a tick at the largest
// time_scale then moves at most a few thousand collision-checked sub-steps.
constexpr float MaxSpeed = 1000.0f;

constexpr std::pair<const char*, SpriteMode> SpriteModes[] = {{"upright", SpriteMode::Upright},
                                                              {"flat", SpriteMode::Flat},
                                                              {"facing", SpriteMode::Facing},
                                                              {"decal", SpriteMode::Decal}};

const char* spriteModeName(SpriteMode mode) {
    switch (mode) {
    case SpriteMode::Flat:
        return "flat";
    case SpriteMode::Facing:
        return "facing";
    case SpriteMode::Decal:
        return "decal";
    case SpriteMode::Upright:
        break;
    }
    return "upright";
}

// `blend = true|false|"alpha"|"additive"|"multiply"|"premultiplied"|"opaque"`.
BlendMode blendMode(const sol::object& value, BlendMode fallback) {
    if (lua::absent(value)) {
        return fallback;
    }
    if (value.get_type() == sol::type::boolean) {
        return value.as<bool>() ? BlendMode::Alpha : BlendMode::Opaque;
    }
    if (value.get_type() != sol::type::string) {
        throw std::runtime_error("blend must be a boolean or a blend mode name");
    }
    return blendModeFromName(value.as<std::string>());
}

// A game shader name, or "" for the engine's own.
std::string shaderName(std::string name, const char* what) {
    if (!name.empty() && !isShaderName(name)) {
        throw std::runtime_error(std::string(what) + " '" + name +
                                 "' must use a-z, 0-9, _ and - (at most 64)");
    }
    return name;
}

// The entity's component as a copy, or a default one. Every setter below
// validates into such a copy and stores it only when every field passed, so a
// rejected table changes nothing (not even the fields before the bad one).
template<class T> T copyOf(const EntityRef& e) {
    const auto* current = e.find<T>();
    return current ? *current : T{};
}
template<class T> void store(const EntityRef& e, const T& component) {
    e.owner().registry().emplace_or_replace<T>(e.id, component);
}

// Settings read as JSON (lights, emitters) come back the same way; their
// vectors return as {x, y, z} tables like every other vector.
sol::table settingsTable(sol::state_view lua, const Json& json, std::initializer_list<const char*> vectors) {
    sol::table t = jsonToLua(lua, json);
    for (const char* key : vectors) {
        const auto found = json.find(key);
        if (found != json.end() && found->is_array() && found->size() == 3) {
            t[key] = lua::table(lua, glm::vec3((*found)[0].get<float>(), (*found)[1].get<float>(),
                                               (*found)[2].get<float>()));
        }
    }
    return t;
}

// ---- sprite

void applySprite(Sprite& s, const sol::table& t) {
    const auto previousClip = s.clip;
    s.animation = lua::get(t, "animation", s.animation);
    s.clip = lua::get(t, "clip", s.clip);
    s.moveClip = lua::get(t, "move_clip", s.moveClip);
    s.idleClip = lua::get(t, "idle_clip", s.idleClip);
    s.speed = lua::within(lua::get(t, "speed", s.speed), -1000.0f, 1000.0f, "sprite speed");
    s.direction = lua::get(t, "direction", s.direction);
    s.visible = lua::get(t, "visible", s.visible);
    s.blend = blendMode(lua::field(t, "blend"), s.blend);
    s.unlit = lua::get(t, "unlit", s.unlit);
    s.castShadow = lua::get(t, "cast_shadow", s.castShadow);
    if (const auto mode = lua::opt<std::string>(lua::field(t, "mode"), "Field 'mode'")) {
        s.mode = lua::choice(*mode, SpriteModes, "sprite mode");
    }
    s.scale = lua::within(lua::get(t, "scale", s.scale), 0.0f, 1000.0f, "sprite scale");
    s.depthBias = lua::get(t, "depth_bias", s.depthBias);
    s.pixelOffset = lua::vec2(lua::field(t, "offset"), s.pixelOffset);
    s.tint = lua::color(lua::field(t, "tint"), s.tint);
    s.flash = lua::color(lua::field(t, "flash"), s.flash);
    s.outline = lua::color(lua::field(t, "outline"), s.outline);
    s.outlineWidth =
        lua::within(lua::get(t, "outline_width", s.outlineWidth), 0.5f, 4.0f, "sprite outline_width");
    if (const sol::object silhouette = lua::field(t, "silhouette");
        silhouette.get_type() == sol::type::boolean) {
        s.silhouette = silhouette.as<bool>() ? glm::vec4(1, 1, 1, 0.6f) : glm::vec4(0);
    } else if (!lua::absent(silhouette)) {
        s.silhouette = lua::color(silhouette, glm::vec4(glm::vec3(s.silhouette), 1));
    }
    if (const auto snap = lua::opt<bool>(lua::field(t, "pixel_snap"), "Field 'pixel_snap'")) {
        s.pixelSnap = *snap;
    }
    s.palette = lua::get(t, "palette", s.palette);
    s.paletteRow = lua::get(t, "palette_row", s.paletteRow);
    s.distort = lua::atLeast(lua::get(t, "distort", s.distort), 0.0f, "sprite distort");
    s.shader = shaderName(lua::get(t, "shader", s.shader), "sprite shader");
    if (s.clip != previousClip) {
        s.time = s.previousTime = 0;
        s.finished = false;
    }
}

void setSprite(const EntityRef& e, const sol::table& t) {
    auto sprite = copyOf<Sprite>(e);
    applySprite(sprite, t);
    store(e, sprite);
}

sol::table spriteTable(sol::state_view lua, const Sprite& s) {
    auto t = lua.create_table();
    t["animation"] = s.animation;
    t["clip"] = s.clip;
    t["move_clip"] = s.moveClip;
    t["idle_clip"] = s.idleClip;
    t["time"] = s.time;
    t["speed"] = s.speed;
    t["direction"] = s.direction;
    t["visible"] = s.visible;
    t["blend"] = blendModeName(s.blend);
    t["unlit"] = s.unlit;
    t["cast_shadow"] = s.castShadow;
    t["mode"] = spriteModeName(s.mode);
    t["scale"] = s.scale;
    t["depth_bias"] = s.depthBias;
    t["offset"] = lua::table(lua, s.pixelOffset);
    t["tint"] = lua::colorTable(lua, s.tint);
    t["flash"] = lua::colorTable(lua, s.flash);
    t["outline"] = lua::colorTable(lua, s.outline);
    t["outline_width"] = s.outlineWidth;
    t["silhouette"] = lua::colorTable(lua, s.silhouette);
    if (s.pixelSnap) {
        t["pixel_snap"] = *s.pixelSnap;
    }
    t["palette"] = s.palette;
    t["palette_row"] = s.paletteRow;
    t["distort"] = s.distort;
    if (!s.shader.empty()) {
        t["shader"] = s.shader;
    }
    t["finished"] = s.finished;
    return t;
}

// ---- mesh

void setMesh(const EntityRef& e, const sol::table& t) {
    auto m = copyOf<MeshInstance>(e);
    m.model = lua::get(t, "model", m.model);
    m.animation = lua::get(t, "animation", m.animation);
    m.speed = lua::within(lua::get(t, "speed", m.speed), -1000.0f, 1000.0f, "mesh speed");
    m.loop = lua::get(t, "loop", m.loop);
    m.playing = lua::get(t, "playing", m.playing);
    m.visible = lua::get(t, "visible", m.visible);
    m.castShadow = lua::get(t, "cast_shadow", m.castShadow);
    m.tint = lua::color(lua::field(t, "tint"), m.tint);
    if (const sol::object blend = lua::field(t, "blend"); !lua::absent(blend)) {
        m.blend = blendMode(blend, BlendMode::Opaque);
    }
    m.shader = shaderName(lua::get(t, "shader", m.shader), "mesh shader");
    store(e, m);
}

sol::table meshTable(sol::state_view lua, const MeshInstance& m) {
    auto t = lua.create_table_with("model", m.model, "animation", m.animation, "time", m.time, "speed",
                                   m.speed, "loop", m.loop, "playing", m.playing, "visible", m.visible,
                                   "cast_shadow", m.castShadow, "tint", lua::colorTable(lua, m.tint));
    if (m.blend) {
        t["blend"] = blendModeName(*m.blend);
    }
    if (!m.shader.empty()) {
        t["shader"] = m.shader;
    }
    return t;
}

// ---- mover

void setMover(const EntityRef& e, const sol::table& t) {
    auto m = copyOf<Mover>(e);
    m.speed = lua::within(lua::get(t, "speed", m.speed), 0.0f, MaxSpeed, "mover speed");
    m.maxStep = lua::within(lua::get(t, "max_step", m.maxStep), 0.0f, 1000.0f, "mover max_step");
    m.radius = lua::within(lua::get(t, "radius", m.radius), 0.0f, 0.49f, "mover radius");
    m.collide = lua::get(t, "collide", m.collide);
    m.climbSpeed = lua::get(t, "climb_speed", m.climbSpeed);
    if (m.climbSpeed <= 0 || m.climbSpeed > MaxSpeed) {
        throw std::runtime_error("climb_speed must be a positive number up to 1000");
    }
    m.footprint = lua::within(lua::get(t, "footprint", m.footprint), 1, MaxFootprint, "mover footprint");
    m.fly = lua::get(t, "fly", m.fly);
    m.altitude = lua::within(lua::get(t, "altitude", m.altitude), 0.0f, 1000.0f, "mover altitude");
    m.altitudeSpeed =
        lua::within(lua::get(t, "altitude_speed", m.altitudeSpeed), 0.0f, MaxSpeed, "mover altitude_speed");
    const Grid& grid = e.owner().grid();
    if (const auto tags = lua::field(t, "blocked_tags"); !lua::absent(tags)) {
        m.blockedTags = gridTagBits(grid, tags, "mover blocked_tags");
    }
    if (const auto tags = lua::field(t, "required_tags"); !lua::absent(tags)) {
        m.requiredTags = gridTagBits(grid, tags, "mover required_tags");
    }
    const sol::object jumpValue = lua::field(t, "jump");
    if (jumpValue.get_type() == sol::type::table) {
        const sol::table jump = jumpValue;
        JumpRules rules;
        rules.distance = lua::as<int>(lua::field(jump, "distance"), "jump.distance");
        rules.maxUp = lua::get(jump, "max_up", m.maxStep);
        rules.maxDown = lua::get(jump, "max_down", m.maxStep);
        rules.diagonal = lua::get(jump, "diagonal", false);
        StepRules check;
        check.jump = rules;
        try {
            validateRules(check);
        } catch (const std::invalid_argument& error) {
            throw std::runtime_error(error.what());
        }
        m.jump = rules;
    } else if (jumpValue.get_type() == sol::type::boolean && !jumpValue.as<bool>()) {
        m.jump = {};
    } else if (!lua::absent(jumpValue)) {
        throw std::runtime_error("mover jump must be a table or false, got " + lua::describe(jumpValue));
    }
    store(e, m);
}

sol::object moverTable(sol::state_view lua, const EntityRef& e, const Mover& m) {
    const Grid& grid = e.owner().grid();
    auto t = lua.create_table_with("speed", m.speed, "max_step", m.maxStep, "radius", m.radius, "collide",
                                   m.collide, "climb_speed", m.climbSpeed, "footprint", m.footprint, "fly",
                                   m.fly, "altitude", m.altitude, "altitude_speed", m.altitudeSpeed, "lift",
                                   m.lift, "blocked_tags", sol::as_table(grid.tagsOf(m.blockedTags)),
                                   "required_tags", sol::as_table(grid.tagsOf(m.requiredTags)));
    if (m.jump.distance > 0) {
        t["jump"] = lua.create_table_with("distance", m.jump.distance, "max_up", m.jump.maxUp, "max_down",
                                          m.jump.maxDown, "diagonal", m.jump.diagonal);
    } else {
        t["jump"] = false;
    }
    return t;
}

// ---- platform

void setPlatform(const EntityRef& e, const sol::table& t) {
    auto p = copyOf<Platform>(e);
    p.size = lua::vecXZ(lua::field(t, "size"), p.size);
    if (!(p.size.x > 0 && p.size.y > 0 && p.size.x <= 64 && p.size.y <= 64)) {
        throw std::runtime_error("platform size must be above 0 and at most 64 on each axis");
    }
    store(e, p);
}

sol::table platformTable(sol::state_view lua, const Platform& p) {
    return lua.create_table_with("size", lua::tableXZ(lua, p.size));
}

// ---- shader params (a list of up to 16 numbers; missing entries are 0)

void setMaterialParams(const EntityRef& e, const sol::table& t) {
    const auto count = lua_rawlen(t.lua_state(), t.push());
    lua_pop(t.lua_state(), 1);
    if (count > 16) {
        throw std::runtime_error("shader_params takes at most 16 numbers");
    }
    MaterialParams params;
    for (std::size_t i = 0; i < 16; ++i) {
        const auto value = lua::field(t, static_cast<lua_Integer>(i + 1));
        params.values[i / 4][static_cast<glm::length_t>(i % 4)] =
            lua::absent(value) ? 0.0f : lua::as<float>(value, "shader_params value");
    }
    store(e, params);
}

sol::table materialParamsTable(sol::state_view lua, const MaterialParams& params) {
    sol::table t = lua.create_table(16, 0);
    for (std::size_t i = 0; i < 16; ++i) {
        t[i + 1] = params.values[i / 4][static_cast<glm::length_t>(i % 4)];
    }
    return t;
}

// ---- collider

void setCollider(const EntityRef& e, const sol::table& t) {
    auto c = copyOf<Collider>(e);
    c.radius = lua::within(lua::get(t, "radius", c.radius), 0.0f, 64.0f, "collider radius");
    c.height = lua::within(lua::get(t, "height", c.height), 0.0f, 1000.0f, "collider height");
    c.solid = lua::get(t, "solid", c.solid);
    // Through the registry, so the collision system sees the new radius at once.
    store(e, c);
}

sol::table colliderTable(sol::state_view lua, const Collider& c) {
    return lua.create_table_with("radius", c.radius, "height", c.height, "solid", c.solid);
}

// ---- light

void setLight(const EntityRef& e, const sol::table& t) {
    auto settings = LightSettings::fromJson(lua::lightJson(t));
    LightSource source{settings, true};
    if (const auto* existing = e.find<LightSource>()) {
        source.enabled = existing->enabled;
        if (lua::absent(lua::field(t, "position"))) {
            source.light.position = existing->light.position;
        }
    }
    source.enabled = lua::get(t, "enabled", source.enabled);
    store(e, source);
}

sol::table lightTable(sol::state_view lua, const LightSource& source) {
    auto t = settingsTable(lua, source.light.toJson(), {"position", "direction"});
    t["enabled"] = source.enabled;
    return t;
}

// ---- emitter

// Emitter settings; `paused` alone pauses or resumes without changing them.
void setEmitter(const EntityRef& e, const sol::table& t) {
    const auto paused = lua::opt<bool>(lua::field(t, "paused"), "Field 'paused'");
    bool onlyPaused = true;
    t.for_each([&onlyPaused](const sol::object& key, const sol::object&) {
        onlyPaused = onlyPaused && key.get_type() == sol::type::string && key.as<std::string>() == "paused";
    });
    const auto* existing = e.find<Emitter>();
    Emitter emitter = existing && onlyPaused ? *existing : Emitter{EmitterSettings::fromJson(lua::emitterJson(t))};
    if (paused) {
        emitter.active = !*paused;
    }
    store(e, emitter);
}

sol::table emitterTable(sol::state_view lua, const Emitter& emitter) {
    auto t = settingsTable(lua, emitter.settings.toJson(), {"velocity", "velocity_jitter", "gravity", "area"});
    t["paused"] = !emitter.active;
    return t;
}

// ---- audio

void setAudio(const EntityRef& e, const sol::table& t) {
    auto a = copyOf<AudioEmitter>(e);
    const auto sound = lua::get(t, "sound", a.sound);
    if (sound.empty()) {
        throw std::runtime_error("A positional sound needs a `sound` file");
    }
    resolveInside(e.engine->lua().gameRoot(), sound);
    a.radius = lua::get(t, "radius", a.radius);
    if (!(a.radius > 0 && a.radius <= lua::VectorBound)) {
        throw std::runtime_error("A positional sound's radius must be above 0 and at most 1000000");
    }
    a.volume = lua::atLeast(lua::get(t, "volume", a.volume), 0.0f, "volume");
    // Everything validated: a changed file stops the voice playing the old one.
    if (sound != a.sound && a.voice >= 0) {
        e.engine->audio().stopSound(a.voice, 0.3);
        a.voice = -1;
    }
    a.sound = sound;
    store(e, a);
}

sol::table audioTable(sol::state_view lua, const AudioEmitter& a) {
    return lua.create_table_with("sound", a.sound, "radius", a.radius, "volume", a.volume);
}

template<class T> void removeComponent(const EntityRef& e) {
    e.owner().registry().remove<T>(e.id);
}
template<> void removeComponent<AudioEmitter>(const EntityRef& e) {
    if (const auto* audio = e.find<AudioEmitter>(); audio && audio->voice >= 0) {
        e.engine->audio().stopSound(audio->voice, 0.3);
    }
    e.owner().registry().remove<AudioEmitter>(e.id);
}

// The eight component methods (sprite, mesh, mover, light, emitter,
// collider, audio, platform) share one rule: no argument reads the component
// (its settings as a table, nil when the entity has none; nothing is added),
// a settings table adds the component or changes it (and returns its new
// state), and an explicit nil removes it. `set(e, table)` validates and
// stores, `read(lua, component)` (or `read(lua, e, component)` when the table
// needs the world, as the mover's tag names do) builds the table.
template<class T, class Set, class Read> auto component(const char* name, Set set, Read read) {
    return [name, set, read](sol::this_state s, const EntityRef& e, sol::object settings) -> sol::object {
        const auto table = [&](const T& current) {
            if constexpr (std::is_invocable_v<Read, sol::this_state, const EntityRef&, const T&>) {
                return read(s, e, current);
            } else {
                return read(s, current);
            }
        };
        if (lua_gettop(s) >= 2 && lua::absent(settings)) {
            removeComponent<T>(e);
            return sol::lua_nil;
        }
        if (lua::absent(settings)) {
            const auto* current = e.find<T>();
            return current ? sol::object(table(*current)) : sol::object(sol::lua_nil);
        }
        set(e, lua::tableArg(settings, (std::string(name) + " settings").c_str()));
        return table(e.get<T>());
    };
}

// A waypoint from a cell ({x, z, layer?}: its centre, on the grid) or a point
// ({x, y, z}). Cells from world:path keep their jump/climb marks.
Waypoint waypointFrom(const Grid& grid, const sol::object& value, std::optional<int> layer) {
    const auto t = lua::vectorTable(value, "a cell {x, z, layer} or a point {x, y, z}");
    Waypoint waypoint;
    if (lua::get(t, "jump", false)) {
        waypoint.kind = StepKind::Jump;
    } else if (lua::get(t, "climb", false)) {
        waypoint.kind = StepKind::Climb;
    }
    if (const auto own = lua::opt<int>(lua::field(t, "layer"), "Waypoint layer")) {
        waypoint.layer = *own;
    } else if (layer) {
        waypoint.layer = *layer;
    }
    if (waypoint.layer >= grid.layerCount()) {
        throw std::runtime_error("Waypoint layer outside the grid");
    }
    if (!lua::absent(lua::field(t, "y"))) {
        waypoint.position = lua::position(value);
        return waypoint;
    }
    const auto cell = lua::cell(value);
    const glm::vec2 centre(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f);
    waypoint.position = {centre.x, grid.heightAt(centre, std::max(waypoint.layer, 0)), centre.y};
    return waypoint;
}

} // namespace

// Shared with WorldBindings for `world:spawn{...}`.
void configureEntity(EntityRef& e, const sol::table& spec) {
    auto& world = e.owner();
    auto& registry = world.registry();
    auto& transform = registry.get<Transform>(e.id);
    if (const auto facing = lua::field(spec, "facing"); !lua::absent(facing)) {
        const auto direction = lua::vecXZ(facing);
        if (glm::length(direction) > 1e-4f) {
            transform.facing = glm::normalize(direction);
        }
    }
    if (const auto yaw = lua::opt<float>(lua::field(spec, "yaw"), "Field 'yaw'")) {
        transform.yaw = glm::radians(*yaw);
    }
    if (const auto scale = lua::field(spec, "scale"); !lua::absent(scale)) {
        transform.scale = scale.get_type() == sol::type::number
                              ? glm::vec3(lua::within(lua::as<float>(scale, "scale"), -lua::VectorBound,
                                                      lua::VectorBound, "scale"))
                              : lua::vec3(scale, glm::vec3(1));
    }
    if (const auto tags = lua::field(spec, "tags"); !lua::absent(tags)) {
        registry.emplace_or_replace<Tags>(e.id, Tags{lua::strings(tags)});
    }
    // Each component field is a settings table.
    const auto component = [&spec](const char* key) {
        return lua::options(lua::field(spec, key), (std::string("Field '") + key + "'").c_str());
    };
    if (const auto sprite = component("sprite")) {
        setSprite(e, *sprite);
    }
    if (const auto mesh = component("mesh")) {
        setMesh(e, *mesh);
    }
    if (const auto light = component("light")) {
        setLight(e, *light);
    }
    if (const auto emitter = component("emitter")) {
        setEmitter(e, *emitter);
    }
    if (const auto collider = component("collider")) {
        setCollider(e, *collider);
    }
    if (const auto mover = component("mover")) {
        setMover(e, *mover);
    }
    if (const auto platform = component("platform")) {
        setPlatform(e, *platform);
    }
    if (const auto audio = component("audio")) {
        setAudio(e, *audio);
    }
    if (const auto props = lua::field(spec, "props"); !lua::absent(props)) {
        registry.emplace_or_replace<Props>(e.id, Props{luaToJson(props)});
    }
}

void bindEntity(Engine& engine) {
    auto& host = engine.lua();
    TypeBinder<EntityRef> type(
        host, engine.docs(), "Entity",
        "A world entity. Components are optional; a script decides what an entity is.");
    type.op(
            sol::meta_function::equal_to, [](const EntityRef& a, const EntityRef& b) { return a == b; },
            "a == b", "True when both handles refer to the same entity.")
        .op(
            sol::meta_function::to_string,
            [](const EntityRef& e) {
                if (!e.valid()) {
                    return std::string("Entity(destroyed)");
                }
                const auto* name = e.find<Name>();
                return "Entity(" + (name ? name->id : std::to_string(e.serial)) + ")";
            },
            "tostring(e)", "'Entity(<id or key>)', or 'Entity(destroyed)'.")
        .method(
            "valid", [](const EntityRef& e) { return e.valid(); }, "fun(): boolean",
            "False once destroyed (or its world unloaded); a handle never comes to name a later entity.")
        .readonly(
            "key", [](const EntityRef& e) { return static_cast<lua_Integer>(e.serial); }, "integer",
            "Stable number for table keys: unique within its world and never reused, also after the "
            "entity is destroyed.")
        .property(
            "id",
            [](const EntityRef& e) {
                const auto* name = e.find<Name>();
                return name ? name->id : std::string{};
            },
            [](const EntityRef& e, sol::object id) {
                e.owner().rename(e.id, lua::as<std::string>(id, "id"));
            },
            "string", "Unique name within the world ('' = anonymous).")
        .method(
            "world", [](const EntityRef& e) { return WorldRef{e.lock(), e.engine}; }, "fun(): World",
            "Owning world.")
        .readonly(
            "position",
            [](const EntityRef& e) { return lua::table(e.engine->lua().lua(), e.get<Transform>().position); },
            "Vec3",
            "World position (feet); a copy. Move the entity with place, "
            "move_to or jump.")
        .readonly(
            "layer", [](const EntityRef& e) { return e.get<Transform>().layer; }, "integer", "Grid layer.")
        .readonly(
            "facing",
            [](const EntityRef& e) { return lua::tableXZ(e.engine->lua().lua(), e.get<Transform>().facing); },
            "{x: number, z: number}",
            "Facing direction on the ground plane; a copy. Turn the entity with face.")
        .method(
            "cell",
            [](sol::this_state s, const EntityRef& e) {
                const auto& t = e.get<Transform>();
                return lua::table(s, GridPos{floorCell(t.position.x), floorCell(t.position.z), t.layer});
            },
            "fun(): Cell", "Grid cell under the entity.")
        .method(
            "place",
            [](const EntityRef& e, sol::object position, sol::object layer, sol::object keepHeight) {
                auto& world = e.owner();
                world.place(e.id, lua::position(position),
                            lua::opt<int>(layer, "layer").value_or(e.get<Transform>().layer),
                            lua::opt<bool>(keepHeight, "keep_height").value_or(false));
            },
            "fun(position: Vec3, layer?: integer, keep_height?: boolean)",
            "Teleport; height snaps to the grid unless keep_height (then y is kept).")
        .method(
            "face",
            [](const EntityRef& e, sol::object target) {
                auto& t = e.get<Transform>();
                glm::vec2 direction(0);
                if (target.is<EntityRef>()) {
                    const auto p = target.as<EntityRef>().get<Transform>().position;
                    direction = {p.x - t.position.x, p.z - t.position.z};
                } else if (target.get_type() == sol::type::string) {
                    const auto name = target.as<std::string>();
                    const auto compass = rampFromName(name);
                    if (!compass || *compass == Ramp::None) {
                        throw std::runtime_error("Unknown direction '" + name +
                                                 "' (expected 'north', 'east', 'south', 'west')");
                    }
                    direction = glm::vec2(rampDirection(*compass));
                } else {
                    direction =
                        lua::vecXZ(lua::vectorTable(target, "an entity, direction name or {x, z}"), t.facing);
                }
                if (glm::length(direction) > 1e-4f) {
                    t.facing = glm::normalize(direction);
                }
            },
            "fun(target: Entity|string|{x: number, z: number})",
            "Face an entity, a compass direction ('north'...) or a vector.")
        .property(
            "yaw", [](const EntityRef& e) { return glm::degrees(e.get<Transform>().yaw); },
            [](const EntityRef& e, sol::object degrees) {
                e.get<Transform>().yaw = glm::radians(lua::as<float>(degrees, "yaw"));
            },
            "number", "Rotation of mesh entities about the vertical axis in degrees.")
        .property(
            "scale", [](sol::this_state s, const EntityRef& e) { return lua::table(s, e.get<Transform>().scale); },
            [](const EntityRef& e, sol::object scale) {
                e.get<Transform>().scale =
                    scale.get_type() == sol::type::number
                        ? glm::vec3(lua::within(lua::as<float>(scale, "scale"), -lua::VectorBound, lua::VectorBound,
                                                "scale"))
                        : lua::vec3(scale);
            },
            "Vec3", "Scale of mesh entities (set a number for a uniform scale or a vector).")
        .method(
            "configure", [](EntityRef& e, sol::table spec) { configureEntity(e, spec); }, "fun(spec: table)",
            "Add or change components (same fields as world:spawn).")
        .method(
            "move_to",
            [](const EntityRef& e, sol::object target, sol::object settings) {
                auto& world = e.owner();
                const auto options = lua::options(settings, "move_to options");
                std::optional<int> layer;
                std::optional<StepRules> rules;
                sol::object path = sol::make_object(target.lua_state(), sol::lua_nil);
                if (options) {
                    if (const auto speed = lua::opt<float>(lua::field(*options, "speed"), "Field 'speed'")) {
                        auto mover = copyOf<Mover>(e);
                        mover.speed = lua::within(*speed, 0.0f, MaxSpeed, "speed");
                        store(e, mover);
                    }
                    layer = lua::opt<int>(lua::field(*options, "layer"), "Field 'layer'");
                    path = lua::field(*options, "path");
                    if (const auto value = lua::field(*options, "rules"); !lua::absent(value)) {
                        rules = stepRules(e.engine->lua(), world, value);
                    }
                }
                if (path.get_type() == sol::type::table) {
                    if (rules) {
                        throw std::runtime_error("move_to takes either a path to follow or rules to plan with");
                    }
                    std::vector<Waypoint> route;
                    for (const auto& waypoint : lua::elements(path.as<sol::table>())) {
                        route.push_back(waypointFrom(world.grid(), waypoint, std::nullopt));
                    }
                    if (!lua::absent(target)) {
                        route.push_back(waypointFrom(world.grid(), target, layer));
                    } else if (layer && !route.empty()) {
                        route.back().layer = *layer;
                    }
                    if (route.empty()) {
                        throw std::runtime_error("move_to needs a target or a non-empty path");
                    }
                    world.follow(e.id, std::move(route));
                    return;
                }
                world.moveTo(e.id, lua::position(target), lua::opt<bool>(path, "Field 'path'").value_or(true),
                             layer, rules ? &*rules : nullptr);
            },
            "fun(target: Vec3|nil, options?: {path?: boolean|(Cell|Vec3)[], speed?: number, layer?: "
            "integer, rules?: table})",
            "Walk to a position (pathfinding by default; `speed` 0 to 1000 sets the mover's); options come "
            "second, so a planned route is "
            "`move_to(nil, {path = cells})`. The target layer defaults to the current one if "
            "walkable there, else the walkable layer whose surface is nearest the target's y. `path` may "
            "instead be a list of cells ({x, z, layer}: walked through their centres) or points ({x, y, z}) "
            "to follow as given, then `target` if any; cells marked `jump` or `climb` (from world:path) "
            "are jumped to or climbed. `rules` are the planning rules of world:path "
            "(diagonal, blocked_tags, required_tags, blocked, jump, ladders, cost) "
            "for the built-in planner; without them it plans 8-directional with the mover's limits and "
            "its own blocked_tags/required_tags. Emits "
            "'arrived' once at the end, or 'blocked' (at once when no path exists or the search stops at "
            "its 200000-cell cap, without moving); jumps emit 'jump_start'/'landed' and ladder climbs "
            "'climb_start'/'climb_end'.")
        .method(
            "stop", [](const EntityRef& e) { e.owner().stop(e.id); }, "fun()", "Stop moving.")
        .method(
            "drive",
            [](const EntityRef& e, sol::object velocity) {
                const auto drive = lua::vecXZ(velocity);
                if (glm::length(drive) > MaxSpeed) {
                    throw std::runtime_error("drive velocity must be at most 1000 units/second");
                }
                e.owner().registry().get_or_emplace<Mover>(e.id).drive = drive;
            },
            "fun(velocity: {x: number, z: number})",
            "Steer this tick with a ground-plane velocity in units/second, at most 1000 long (player "
            "control).")
        .method(
            "jump",
            [](const EntityRef& e, sol::object target, sol::object settings) {
                const auto& t = e.get<Transform>();
                const auto options = lua::options(settings, "jump options");
                const float height = lua::within(options ? lua::get(*options, "height", 1.0f) : 1.0f,
                                                 -1000.0f, 1000.0f, "jump height");
                const float duration = lua::within(options ? lua::get(*options, "duration", 0.5f) : 0.5f,
                                                   0.0f, 60.0f, "jump duration");
                const int layer = options ? lua::get(*options, "layer", t.layer) : t.layer;
                e.owner().jump(e.id, lua::position(target), height, duration, layer);
            },
            "fun(target: Vec3, options?: {height?: number, duration?: number, layer?: integer})",
            "Arc to a position; `height` is -1000 to 1000 (default 1) and `duration` 0 to 60 seconds "
            "(default 0.5). Emits 'jump_start' and 'landed'.")
        .method(
            "launch",
            [](const EntityRef& e, sol::object velocity) { e.owner().launch(e.id, lua::vec3(velocity)); },
            "fun(velocity: Vec3)",
            "Throw the body with a velocity in units/second (each component within the world bound): it "
            "flies a ballistic arc under the world's `gravity` (set_movement_tuning), keeping its "
            "horizontal speed until a solid entity, a collider box, a cliff face or the grid's edge stops "
            "it (it then drops straight down), and lands on the first surface it falls onto that it may "
            "stand on (any layer; its tags and fly apply), or with none on the lowest layer's surface. A "
            "landing inside a box or a solid body moves to the nearest free spot within `landing_search`; "
            "with none it lands there anyway and 'blocked' follows 'landed'. Emits 'jump_start' and "
            "'landed'. A zero velocity above the ground is a fall; a launch during a jump or launch "
            "replaces it from where the body is; during a ladder climb it is an error.")
        .method(
            "moving", [](const EntityRef& e) { return e.owner().moving(e.id); }, "fun(): boolean",
            "Walking, jumping, climbing or being driven.")
        .method(
            "can_occupy",
            [](const EntityRef& e, sol::object position, sol::object layer) {
                return e.owner().canOccupy(e.id, lua::position(position),
                                           lua::opt<int>(layer, "layer").value_or(e.get<Transform>().layer));
            },
            "fun(position: Vec3, layer?: integer): boolean",
            "Whether the entity would fit there (grid, colliders and solid entities, current as of this "
            "call).")
        .method(
            "mover", component<Mover>("mover", setMover, moverTable),
            "fun(settings?: {speed?: number, max_step?: number, radius?: number, collide?: boolean, "
            "climb_speed?: number, footprint?: integer, fly?: boolean, altitude?: number, altitude_speed?: "
            "number, blocked_tags?: string[], required_tags?: string[], jump?: {distance: integer, max_up?: "
            "number, max_down?: number, diagonal?: boolean}|false}|nil): table?",
            "Movement. A table adds or configures it (and returns the new state), no argument reads it "
            "(nil without a mover), nil removes it. `speed` (units/second) is 0 to 1000, `max_step` 0 "
            "to 1000 and `radius` 0 to 0.49. `jump` lets planned paths jump gaps (see world:path); ladders "
            "are climbed at `climb_speed` (default 1.5, at most 1000), also when steering into one. "
            "`footprint` (1 to 8, default 1) is the square of cells the mover occupies, anchored at its cell "
            "towards +x and +z, and `fly` lets it stand on and path over unwalkable cells and height steps. "
            "`blocked_tags` and `required_tags` (grid tag names from the map's tag list; an unknown name is "
            "an error) are its passability class, flying or not: it never stands on or paths through a "
            "cell carrying a blocked tag or, when `required_tags` is not empty, a cell with none of them "
            "(a boat is `{fly = true, required_tags = {'water'}}`). They are read back as name lists and "
            "plan `move_to(target)`; rules given to move_to replace them. `altitude` (0 to 1000) lifts the "
            "body that far above the grid surface, reached at `altitude_speed` units/second (0, the "
            "default: at once); `lift` reads the height in effect. A lifted body passes over boxes and "
            "bodies whose tops lie below its feet; its cell, zones and tags still apply, it flies past "
            "ladders, and a launch lands it on the surface (the lift eases back up afterwards).")
        .method(
            "platform", component<Platform>("platform", setPlatform, platformTable),
            "fun(settings?: {size?: {x: number, z: number}}|nil): table?",
            "Make the entity a moving surface (rafts, lifts): its position is the top, `size` (default 1 x "
            "1, at most 64 x 64) the footprint. Movers and colliders standing on it ride along; grid cells "
            "whose centre it covers on its layer are walkable at its height while it is there. Move it with "
            "mover{collide = false} or place(). No argument reads it (nil without one); nil removes it.")
        .method(
            "shader_params", component<MaterialParams>("shader_params", setMaterialParams, materialParamsTable),
            "fun(values?: number[]|nil): number[]?",
            "The 16 numbers a game shader (sprite or mesh `shader`) reads as `u_params[4]`: a list of up to "
            "16 numbers in order (`u_params[0].x` first; missing ones are 0), for dissolve amounts, hit "
            "timers and the like. No argument reads them (nil when never set); nil removes them (zeros).")
        .method(
            "sprite", component<Sprite>("sprite", setSprite, spriteTable),
            "fun(settings?: table|nil): table?",
            "Add or change the sprite and return its state; e:sprite() reads it (nil when the entity has no "
            "sprite) and nil removes it.")
        .method(
            "clip",
            [](const EntityRef& e, const std::string& clip, sol::object restart) {
                e.owner().setClip(e.id, clip, lua::opt<bool>(restart, "restart").value_or(true));
            },
            "fun(clip: string, restart?: boolean)", "Play a sprite clip.")
        .method(
            "mesh", component<MeshInstance>("mesh", setMesh, meshTable), "fun(settings?: table|nil): table?",
            "Add or change a glTF mesh (props, doors...) and return its state; e:mesh() reads it (nil "
            "without one) and nil removes it.")
        .method(
            "play",
            [](const EntityRef& e, const std::string& animation, sol::object loop) {
                e.owner().playMesh(e.id, animation, lua::opt<bool>(loop, "loop").value_or(false));
            },
            "fun(animation: string, loop?: boolean)", "Play a mesh animation by name.")
        .method(
            "light", component<LightSource>("light", setLight, lightTable),
            "fun(settings?: table|nil): table?",
            "Attach a light (position is an offset; `enabled = false` keeps it but turns it off) and return "
            "its state; e:light() reads it (nil without one) and nil removes it.")
        .method(
            "emitter", component<Emitter>("emitter", setEmitter, emitterTable),
            "fun(settings?: table|nil): table?",
            "Attach particles (the settings replace the previous ones) and return their state; `paused = "
            "true|false` stops or resumes spawning, and a table with no other field keeps the settings. "
            "e:emitter() reads it (nil without one) and nil removes the emitter.")
        .method(
            "collider", component<Collider>("collider", setCollider, colliderTable),
            "fun(settings?: {radius?: number, height?: number, solid?: boolean}|nil): table?",
            "Contact shape: an upright cylinder, `radius` 0 to 64 (default 0.25) and `height` 0 to 1000 "
            "(default 1.6). Solid colliders block movers; all report contacts. e:collider() reads it (nil "
            "without one) and nil removes it.")
        .method(
            "audio", component<AudioEmitter>("audio", setAudio, audioTable),
            "fun(settings?: {sound: string, radius?: number, volume?: number}|nil): table?",
            "Looping positional sound (waterfalls, fires), heard while its world is drawn. e:audio() reads "
            "it (nil without one) and nil removes it.")
        .method(
            "tags",
            [](const EntityRef& e) {
                const auto* tags = e.find<Tags>();
                return sol::as_table(tags ? tags->values : std::vector<std::string>{});
            },
            "fun(): string[]", "Tags.")
        .method(
            "tag",
            [](const EntityRef& e, const std::string& tag) {
                auto& tags = e.owner().registry().get_or_emplace<Tags>(e.id);
                if (!tags.has(tag)) {
                    tags.values.push_back(tag);
                }
            },
            "fun(tag: string)", "Add a tag.")
        .method(
            "untag",
            [](const EntityRef& e, const std::string& tag) {
                if (auto* tags = e.find<Tags>()) {
                    std::erase(tags->values, tag);
                }
            },
            "fun(tag: string)", "Remove a tag.")
        .method(
            "has_tag",
            [](const EntityRef& e, const std::string& tag) {
                const auto* tags = e.find<Tags>();
                return tags && tags->has(tag);
            },
            "fun(tag: string): boolean", "Whether the entity has a tag.")
        .method(
            "props",
            [](const EntityRef& e) {
                const auto* props = e.find<Props>();
                return jsonToLua(e.engine->lua().lua(), props ? props->value : Json::object());
            },
            "fun(): table", "Authored properties (a copy).")
        .method(
            "data",
            [](sol::this_state s, EntityRef& e) -> sol::table {
                auto& ref = e.owner().registry().get_or_emplace<ScriptRef>(e.id);
                lua_State* L = s;
                if (ref.reference == LUA_NOREF) {
                    lua_newtable(L);
                    ref.reference = luaL_ref(L, LUA_REGISTRYINDEX);
                }
                lua_rawgeti(L, LUA_REGISTRYINDEX, ref.reference);
                sol::table table(L, -1);
                lua_pop(L, 1);
                return table;
            },
            "fun(): table", "Script-side table that lives as long as the entity.")
        .method(
            "attach",
            [](const EntityRef& e, const EntityRef& parent, sol::object offset, sol::object settings) {
                if (!parent.valid() || !e.sameWorld(parent)) {
                    throw std::runtime_error("attach: the parent must be a live entity in the same world");
                }
                if (parent.id == e.id) {
                    throw std::runtime_error("attach: an entity cannot follow itself");
                }
                const auto options = lua::options(settings, "attach options");
                auto& t = e.get<Transform>();
                t.parent = parent.id;
                t.offset = lua::vec3(offset, glm::vec3(0));
                t.ground = options ? lua::get(*options, "ground", false) : false;
            },
            "fun(parent: Entity, offset?: Vec3, options?: {ground?: boolean})",
            "Follow another entity of the same world (emote balloons, carried objects); detaches when the "
            "parent is destroyed. `offset` is turned by the parent's yaw. With `ground = true` the offset "
            "is added to the grid surface under the parent instead of the parent's position, so a flat "
            "sprite attached that way is a blob shadow that stays on the ground however high the parent "
            "flies.")
        .method(
            "detach", [](const EntityRef& e) { e.get<Transform>().parent = entt::null; }, "fun()",
            "Stop following.")
        .method(
            "screen_position",
            [](const EntityRef& e, sol::object lift, sol::object alpha) {
                auto& world = e.owner();
                const double a = lua::opt<double>(alpha, "alpha").value_or(e.engine->drawAlpha());
                return projectToCanvas(*e.engine, world.camera(),
                                       world.interpolated(e.id, a) +
                                           glm::vec3(0, lua::opt<float>(lift, "lift").value_or(0), 0),
                                       a);
            },
            "fun(lift?: number, alpha?: number): number, number, boolean",
            "Canvas position of the entity (plus `lift` units up); third value false when behind the camera. "
            "Pass draw's `alpha` to follow the rendered (interpolated) entity and camera.")
        .method(
            "destroy", [](EntityRef& e) { e.owner().destroy(e.id); }, "fun()",
            "Remove the entity. Like every other method it raises on a handle whose entity is already "
            "gone (check e:valid() first when that is expected).");
}
} // namespace fe
