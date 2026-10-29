#include "engine/core/Paths.h"
#include "engine/script/bindings/Convert.h"
#include "engine/script/bindings/WorldTypes.h"

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

void applyMesh(MeshInstance& m, const sol::table& t) {
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
}

void applyMover(Mover& m, const sol::table& t) {
    m.speed = lua::within(lua::get(t, "speed", m.speed), 0.0f, MaxSpeed, "mover speed");
    m.maxStep = lua::within(lua::get(t, "max_step", m.maxStep), 0.0f, 1000.0f, "mover max_step");
    m.radius = lua::within(lua::get(t, "radius", m.radius), 0.0f, 0.49f, "mover radius");
    m.collide = lua::get(t, "collide", m.collide);
    m.climbSpeed = lua::get(t, "climb_speed", m.climbSpeed);
    if (m.climbSpeed <= 0 || m.climbSpeed > MaxSpeed) {
        throw std::runtime_error("climb_speed must be a positive number up to 1000");
    }
    const sol::object jumpValue = lua::field(t, "jump");
    if (jumpValue.get_type() == sol::type::table) {
        const sol::table jump = jumpValue;
        JumpRules rules;
        rules.distance = static_cast<int>(lua::integer(lua::field(jump, "distance"), "jump.distance"));
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
}

void applyPlatform(Platform& p, const sol::table& t) {
    p.size = lua::vecXZ(lua::field(t, "size"), p.size);
    if (!(p.size.x > 0 && p.size.y > 0 && p.size.x <= 64 && p.size.y <= 64)) {
        throw std::runtime_error("platform size must be above 0 and at most 64 on each axis");
    }
}

// A waypoint from a cell ({x, z, layer?}: its centre, on the grid) or a point
// ({x, y, z}). Cells from world:path/reachable keep their jump/climb marks.
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

// Validates a changed copy, then stores it through the registry, so the
// collision system sees the new radius at once.
void setCollider(entt::registry& registry, entt::entity entity, const sol::table& t) {
    const auto* current = registry.try_get<Collider>(entity);
    auto c = current ? *current : Collider{};
    c.radius = lua::within(lua::get(t, "radius", c.radius), 0.0f, 64.0f, "collider radius");
    c.height = lua::within(lua::get(t, "height", c.height), 0.0f, 1000.0f, "collider height");
    c.solid = lua::get(t, "solid", c.solid);
    registry.emplace_or_replace<Collider>(entity, c);
}

void applyAudio(Engine& engine, AudioEmitter& a, const sol::table& t) {
    const auto sound = lua::get(t, "sound", a.sound);
    if (sound.empty()) {
        throw std::runtime_error("A positional sound needs a `sound` file");
    }
    resolveInside(engine.lua().gameRoot(), sound);
    if (sound != a.sound && a.voice >= 0) {
        engine.audio().stopSound(a.voice, 0.3);
        a.voice = -1;
    }
    a.sound = sound;
    a.radius = lua::get(t, "radius", a.radius);
    if (!(a.radius > 0 && a.radius <= lua::VectorBound)) {
        throw std::runtime_error("A positional sound's radius must be above 0 and at most 1000000");
    }
    a.volume = lua::atLeast(lua::get(t, "volume", a.volume), 0.0f, "volume");
}

// Component methods (sprite, mesh, mover, light, emitter, collider, audio,
// platform) share one rule: a settings table adds the component or changes
// it, no argument adds it with its defaults, and an explicit nil removes it.
bool removes(lua_State* state, const sol::object& settings) {
    return lua_gettop(state) >= 2 && lua::absent(settings);
}

// Emitter settings; `paused` alone pauses or resumes without changing them.
void applyEmitter(entt::registry& registry, entt::entity id, const sol::table& t) {
    const auto paused = lua::opt<bool>(lua::field(t, "paused"), "Field 'paused'");
    bool onlyPaused = true;
    t.for_each([&onlyPaused](const sol::object& key, const sol::object&) {
        onlyPaused = onlyPaused && key.get_type() == sol::type::string && key.as<std::string>() == "paused";
    });
    auto* emitter = registry.try_get<Emitter>(id);
    if (!emitter || !onlyPaused) {
        emitter = &registry.emplace_or_replace<Emitter>(
            id, Emitter{EmitterSettings::fromJson(lua::emitterJson(t))});
    }
    if (paused) {
        emitter->active = !*paused;
    }
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
        applySprite(registry.get_or_emplace<Sprite>(e.id), *sprite);
    }
    if (const auto mesh = component("mesh")) {
        applyMesh(registry.get_or_emplace<MeshInstance>(e.id), *mesh);
    }
    if (const auto light = component("light")) {
        registry.emplace_or_replace<LightSource>(
            e.id, LightSource{LightSettings::fromJson(lua::lightJson(*light))});
    }
    if (const auto emitter = component("emitter")) {
        applyEmitter(registry, e.id, *emitter);
    }
    if (const auto collider = component("collider")) {
        setCollider(registry, e.id, *collider);
    }
    if (const auto mover = component("mover")) {
        applyMover(registry.get_or_emplace<Mover>(e.id), *mover);
    }
    if (const auto platform = component("platform")) {
        applyPlatform(registry.get_or_emplace<Platform>(e.id), *platform);
    }
    if (const auto audio = component("audio")) {
        applyAudio(*e.engine, registry.get_or_emplace<AudioEmitter>(e.id), *audio);
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
        .method(
            "configure", [](EntityRef& e, sol::table spec) { configureEntity(e, spec); }, "fun(spec: table)",
            "Add or change components (same fields as world:spawn).")
        .method(
            "move_to",
            [](const EntityRef& e, sol::object target, sol::object settings) {
                auto& world = e.owner();
                const auto options = lua::options(settings, "move_to options");
                std::optional<int> layer;
                sol::object path = sol::make_object(target.lua_state(), sol::lua_nil);
                if (options) {
                    if (const auto speed = lua::opt<float>(lua::field(*options, "speed"), "Field 'speed'")) {
                        world.registry().get_or_emplace<Mover>(e.id).speed =
                            lua::within(*speed, 0.0f, MaxSpeed, "speed");
                    }
                    layer = lua::opt<int>(lua::field(*options, "layer"), "Field 'layer'");
                    path = lua::field(*options, "path");
                }
                if (path.get_type() == sol::type::table) {
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
                             layer);
            },
            "fun(target: Vec3|nil, options?: {path?: boolean|(Cell|Vec3)[], speed?: number, layer?: "
            "integer})",
            "Walk to a position (pathfinding by default; `speed` 0 to 1000 sets the mover's); options come "
            "second, so a planned route is "
            "`move_to(nil, {path = cells})`. The target layer defaults to the current one if "
            "walkable there, else the walkable layer whose surface is nearest the target's y. `path` may "
            "instead be a list of cells ({x, z, layer}: walked through their centres) or points ({x, y, z}) "
            "to follow as given, then `target` if any; cells marked `jump` or `climb` (from world:path or "
            "world:reachable) are jumped to or climbed. Emits 'arrived' once at the end, or 'blocked' (at "
            "once when no path exists or the search stops at its 200000-cell cap, without moving); jumps "
            "emit 'jump_start'/'landed' and ladder climbs "
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
            "mover",
            [](sol::this_state s, const EntityRef& e, sol::object settings) {
                if (removes(s, settings)) {
                    e.owner().registry().remove<Mover>(e.id);
                    return;
                }
                const auto t = lua::options(settings, "mover settings");
                auto& m = e.owner().registry().get_or_emplace<Mover>(e.id);
                if (t) {
                    applyMover(m, *t);
                }
            },
            "fun(settings?: {speed?: number, max_step?: number, radius?: number, collide?: boolean, "
            "climb_speed?: number, jump?: {distance: integer, max_up?: number, max_down?: number, diagonal?: "
            "boolean}|false}|nil)",
            "Add or configure movement; nil removes it. `speed` (units/second) is 0 to 1000, `max_step` 0 "
            "to 1000 and `radius` 0 to 0.49. `jump` lets planned paths jump gaps (see world:path); ladders "
            "are climbed at `climb_speed` (default 1.5, at most 1000), also when steering into one.")
        .method(
            "platform",
            [](sol::this_state s, const EntityRef& e, sol::object settings) {
                auto& registry = e.owner().registry();
                if (removes(s, settings)) {
                    registry.remove<Platform>(e.id);
                    return;
                }
                const auto t = lua::options(settings, "platform settings");
                auto& platform = registry.get_or_emplace<Platform>(e.id);
                if (t) {
                    applyPlatform(platform, *t);
                }
            },
            "fun(settings?: {size?: {x: number, z: number}}|nil)",
            "Make the entity a moving surface (rafts, lifts): its position is the top, `size` (default 1 x "
            "1, at most 64 x 64) the footprint. Movers and colliders standing on it ride along; grid cells "
            "whose centre it "
            "covers on its layer are walkable at its height while it is there. Move it with "
            "mover{collide = false} or place(); nil removes it.")
        .method(
            "sprite",
            [](sol::this_state s, const EntityRef& e, sol::object settings) -> sol::object {
                if (removes(s, settings)) {
                    e.owner().registry().remove<Sprite>(e.id);
                    return sol::lua_nil;
                }
                const auto t = lua::options(settings, "sprite settings");
                auto& sprite = e.owner().registry().get_or_emplace<Sprite>(e.id);
                if (t) {
                    applySprite(sprite, *t);
                }
                return spriteTable(s, sprite);
            },
            "fun(settings?: table|nil): table?",
            "Add or change the sprite and return its current state (e:sprite() reads it); nil removes it.")
        .method(
            "clip",
            [](const EntityRef& e, const std::string& clip, sol::object restart) {
                e.owner().setClip(e.id, clip, lua::opt<bool>(restart, "restart").value_or(true));
            },
            "fun(clip: string, restart?: boolean)", "Play a sprite clip.")
        .method(
            "mesh",
            [](sol::this_state s, const EntityRef& e, sol::object settings) {
                if (removes(s, settings)) {
                    e.owner().registry().remove<MeshInstance>(e.id);
                    return;
                }
                const auto t = lua::options(settings, "mesh settings");
                auto& m = e.owner().registry().get_or_emplace<MeshInstance>(e.id);
                if (t) {
                    applyMesh(m, *t);
                }
            },
            "fun(settings?: table|nil)", "Add or change a glTF mesh (props, doors...); nil removes it.")
        .method(
            "play",
            [](const EntityRef& e, const std::string& animation, sol::object loop) {
                e.owner().playMesh(e.id, animation, lua::opt<bool>(loop, "loop").value_or(false));
            },
            "fun(animation: string, loop?: boolean)", "Play a mesh animation by name.")
        .method(
            "light",
            [](sol::this_state s, const EntityRef& e, sol::object value) {
                auto& registry = e.owner().registry();
                if (removes(s, value)) {
                    registry.remove<LightSource>(e.id);
                    return;
                }
                const auto t = lua::options(value, "light settings");
                auto settings = LightSettings::fromJson(t ? lua::lightJson(*t) : Json::object());
                if (const auto* existing = registry.try_get<LightSource>(e.id);
                    existing && (!t || lua::absent(lua::field(*t, "position")))) {
                    settings.position = existing->light.position;
                }
                registry.emplace_or_replace<LightSource>(e.id, LightSource{settings});
            },
            "fun(settings?: table|nil)", "Attach a light (position is an offset); nil removes it.")
        .method(
            "emitter",
            [](sol::this_state s, const EntityRef& e, sol::object value) {
                auto& registry = e.owner().registry();
                if (removes(s, value)) {
                    registry.remove<Emitter>(e.id);
                    return;
                }
                const auto t = lua::options(value, "emitter settings");
                applyEmitter(registry, e.id, t ? *t : sol::state_view(s).create_table());
            },
            "fun(settings?: table|nil)",
            "Attach particles (the settings replace the previous ones); `paused = true|false` stops or "
            "resumes "
            "spawning, and a table with no other field keeps the settings. nil removes the emitter.")
        .method(
            "collider",
            [](sol::this_state s, const EntityRef& e, sol::object value) {
                auto& registry = e.owner().registry();
                if (removes(s, value)) {
                    registry.remove<Collider>(e.id);
                    return;
                }
                const auto t = lua::options(value, "collider settings");
                setCollider(registry, e.id, t ? *t : sol::state_view(s).create_table());
            },
            "fun(settings?: {radius?: number, height?: number, solid?: boolean}|nil)",
            "Contact shape: an upright cylinder, `radius` 0 to 64 (default 0.25) and `height` 0 to 1000 "
            "(default 1.6). Solid colliders block movers; all report contacts. nil removes it.")
        .method(
            "audio",
            [](sol::this_state s, const EntityRef& e, sol::object value) {
                auto& registry = e.owner().registry();
                if (removes(s, value)) {
                    if (const auto* audio = registry.try_get<AudioEmitter>(e.id);
                        audio && audio->voice >= 0) {
                        e.engine->audio().stopSound(audio->voice, 0.3);
                    }
                    registry.remove<AudioEmitter>(e.id);
                    return;
                }
                const auto t = lua::options(value, "audio settings");
                const auto* existing = registry.try_get<AudioEmitter>(e.id);
                auto audio = existing ? *existing : AudioEmitter{};
                applyAudio(*e.engine, audio, t ? *t : sol::state_view(s).create_table());
                registry.emplace_or_replace<AudioEmitter>(e.id, audio);
            },
            "fun(settings?: {sound: string, radius?: number, volume?: number}|nil)",
            "Looping positional sound (waterfalls, fires), heard while its world is drawn; nil removes it.")
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
            [](const EntityRef& e, const EntityRef& parent, sol::object offset) {
                if (!parent.valid() || !e.sameWorld(parent)) {
                    throw std::runtime_error("attach: the parent must be a live entity in the same world");
                }
                if (parent.id == e.id) {
                    throw std::runtime_error("attach: an entity cannot follow itself");
                }
                auto& t = e.get<Transform>();
                t.parent = parent.id;
                t.offset = lua::vec3(offset, glm::vec3(0));
            },
            "fun(parent: Entity, offset?: Vec3)",
            "Follow another entity of the same world (emote balloons, carried objects); detaches when the "
            "parent "
            "is destroyed.")
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
            "destroy",
            [](EntityRef& e) {
                if (e.valid()) {
                    e.owner().destroy(e.id);
                }
            },
            "fun()", "Remove the entity.");
}
} // namespace fe
