#include "engine/script/bindings/Convert.h"
#include "engine/script/bindings/WorldTypes.h"

namespace fe {
namespace {
SpriteMode spriteMode(const std::string& mode) {
    if (mode == "flat") {
        return SpriteMode::Flat;
    }
    if (mode == "facing") {
        return SpriteMode::Facing;
    }
    return SpriteMode::Upright;
}

const char* spriteModeName(SpriteMode mode) {
    switch (mode) {
    case SpriteMode::Flat:
        return "flat";
    case SpriteMode::Facing:
        return "facing";
    case SpriteMode::Upright:
        break;
    }
    return "upright";
}

void applySprite(Sprite& s, const sol::table& t) {
    const auto previousClip = s.clip;
    s.animation = lua::get(t, "animation", s.animation);
    s.clip = lua::get(t, "clip", s.clip);
    s.moveClip = lua::get(t, "move_clip", s.moveClip);
    s.idleClip = lua::get(t, "idle_clip", s.idleClip);
    s.speed = lua::get(t, "speed", s.speed);
    s.direction = lua::get(t, "direction", s.direction);
    s.visible = lua::get(t, "visible", s.visible);
    s.blend = lua::get(t, "blend", s.blend);
    s.unlit = lua::get(t, "unlit", s.unlit);
    s.castShadow = lua::get(t, "cast_shadow", s.castShadow);
    if (t["mode"].valid()) {
        s.mode = spriteMode(t.get<std::string>("mode"));
    }
    s.scale = lua::get(t, "scale", s.scale);
    s.depthBias = lua::get(t, "depth_bias", s.depthBias);
    s.pixelOffset = lua::vec2(t["offset"], s.pixelOffset);
    s.tint = lua::color(t["tint"], s.tint);
    s.flash = lua::color(t["flash"], s.flash);
    s.outline = lua::color(t["outline"], s.outline);
    s.palette = lua::get(t, "palette", s.palette);
    s.paletteRow = lua::get(t, "palette_row", s.paletteRow);
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
    t["blend"] = s.blend;
    t["unlit"] = s.unlit;
    t["cast_shadow"] = s.castShadow;
    t["mode"] = spriteModeName(s.mode);
    t["scale"] = s.scale;
    t["depth_bias"] = s.depthBias;
    t["offset"] = lua::table(lua, s.pixelOffset);
    t["tint"] = lua::colorTable(lua, s.tint);
    t["flash"] = lua::colorTable(lua, s.flash);
    t["outline"] = lua::colorTable(lua, s.outline);
    t["palette"] = s.palette;
    t["palette_row"] = s.paletteRow;
    t["finished"] = s.finished;
    return t;
}

void applyMesh(MeshInstance& m, const sol::table& t) {
    m.model = lua::get(t, "model", m.model);
    m.animation = lua::get(t, "animation", m.animation);
    m.speed = lua::get(t, "speed", m.speed);
    m.loop = lua::get(t, "loop", m.loop);
    m.playing = lua::get(t, "playing", m.playing);
    m.visible = lua::get(t, "visible", m.visible);
    m.castShadow = lua::get(t, "cast_shadow", m.castShadow);
    m.tint = lua::color(t["tint"], m.tint);
}

void applyMover(Mover& m, const sol::table& t) {
    m.speed = lua::get(t, "speed", m.speed);
    m.maxStep = lua::get(t, "max_step", m.maxStep);
    m.radius = std::clamp(lua::get(t, "radius", m.radius), 0.0f, 0.49f);
    m.collide = lua::get(t, "collide", m.collide);
}

void applyCollider(Collider& c, const sol::table& t) {
    c.radius = lua::get(t, "radius", c.radius);
    c.height = lua::get(t, "height", c.height);
    c.solid = lua::get(t, "solid", c.solid);
}

void applyAudio(AudioEmitter& a, const sol::table& t) {
    a.sound = lua::get(t, "sound", a.sound);
    a.radius = lua::get(t, "radius", a.radius);
    a.volume = lua::get(t, "volume", a.volume);
}

void eraseScriptRef(EntityRef& e) {
    auto* ref = e.find<ScriptRef>();
    if (ref && ref->reference != LUA_NOREF) {
        luaL_unref(e.engine->lua().lua().lua_state(), LUA_REGISTRYINDEX, ref->reference);
        ref->reference = LUA_NOREF;
    }
}
} // namespace

// Shared with WorldBindings for `world:spawn{...}`.
void configureEntity(EntityRef& e, const sol::table& spec) {
    auto& world = e.owner();
    auto& registry = world.registry();
    auto& transform = registry.get<Transform>(e.id);
    if (spec["facing"].valid()) {
        const auto facing = lua::vecXZ(spec["facing"], transform.facing);
        if (glm::length(facing) > 1e-4f) {
            transform.facing = glm::normalize(facing);
        }
    }
    transform.yaw = lua::get(spec, "yaw", transform.yaw);
    if (spec["scale"].valid()) {
        transform.scale = spec["scale"].get_type() == sol::type::number
                              ? glm::vec3(spec.get<float>("scale"))
                              : lua::vec3(spec["scale"], glm::vec3(1));
    }
    if (spec["tags"].valid()) {
        registry.emplace_or_replace<Tags>(e.id, Tags{lua::strings(spec["tags"])});
    }
    if (spec["sprite"].valid()) {
        applySprite(registry.get_or_emplace<Sprite>(e.id), spec["sprite"]);
    }
    if (spec["mesh"].valid()) {
        applyMesh(registry.get_or_emplace<MeshInstance>(e.id), spec["mesh"]);
    }
    if (spec["light"].valid()) {
        registry.emplace_or_replace<LightSource>(
            e.id, LightSource{LightSettings::fromJson(luaToJson(spec["light"]))});
    }
    if (spec["emitter"].valid()) {
        registry.emplace_or_replace<Emitter>(e.id,
                                             Emitter{EmitterSettings::fromJson(luaToJson(spec["emitter"]))});
    }
    if (spec["collider"].valid()) {
        applyCollider(registry.get_or_emplace<Collider>(e.id), spec["collider"]);
    }
    if (spec["mover"].valid()) {
        applyMover(registry.get_or_emplace<Mover>(e.id), spec["mover"]);
    }
    if (spec["audio"].valid()) {
        applyAudio(registry.get_or_emplace<AudioEmitter>(e.id), spec["audio"]);
    }
    if (spec["props"].valid()) {
        registry.emplace_or_replace<Props>(e.id, Props{luaToJson(spec["props"])});
    }
}

void bindEntity(Engine& engine) {
    auto& host = engine.lua();
    TypeBinder<EntityRef> type(
        host, engine.docs(), "Entity",
        "A world entity. Components are optional; a script decides what an entity is.");
    type.raw()[sol::meta_function::equal_to] = [](const EntityRef& a, const EntityRef& b) {
        return a == b;
    };
    type.raw()[sol::meta_function::to_string] = [](const EntityRef& e) {
        if (!e.valid()) {
            return std::string("Entity(destroyed)");
        }
        const auto* name = e.find<Name>();
        return "Entity(" + (name ? name->id : std::to_string(static_cast<std::uint32_t>(e.id))) + ")";
    };
    type.method(
            "valid", [](const EntityRef& e) { return e.valid(); }, "fun(): boolean", "False once destroyed.")
        .readonly(
            "key", [](const EntityRef& e) { return static_cast<std::uint32_t>(e.id); }, "integer",
            "Stable number for table keys (unique within its world).")
        .property(
            "id",
            [](const EntityRef& e) {
                const auto* name = e.find<Name>();
                return name ? name->id : std::string{};
            },
            [](const EntityRef& e, const std::string& id) { e.owner().rename(e.id, id); }, "string",
            "Unique name within the world ('' = anonymous).")
        .method(
            "world",
            [](const EntityRef& e) {
                e.ensureValid();
                return WorldRef{e.world, e.engine};
            },
            "fun(): World", "Owning world.")
        .readonly(
            "position",
            [](const EntityRef& e) { return lua::table(e.engine->lua().lua(), e.get<Transform>().position); },
            "{x: number, y: number, z: number}", "World position (feet).")
        .readonly(
            "layer", [](const EntityRef& e) { return e.get<Transform>().layer; }, "integer", "Grid layer.")
        .readonly(
            "facing",
            [](const EntityRef& e) { return lua::tableXZ(e.engine->lua().lua(), e.get<Transform>().facing); },
            "{x: number, z: number}", "Facing direction on the ground plane.")
        .method(
            "cell",
            [](sol::this_state s, const EntityRef& e) {
                const auto& t = e.get<Transform>();
                return lua::table(s, GridPos{static_cast<int>(std::floor(t.position.x)),
                                             static_cast<int>(std::floor(t.position.z)), t.layer});
            },
            "fun(): {x: integer, z: integer, layer: integer}", "Grid cell under the entity.")
        .method(
            "place",
            [](const EntityRef& e, sol::object position, sol::optional<int> layer,
               sol::optional<bool> keepHeight) {
                auto& world = e.owner();
                world.place(e.id, lua::vec3(position), layer.value_or(e.get<Transform>().layer),
                            keepHeight.value_or(false));
            },
            "fun(position: table, layer?: integer, keep_height?: boolean)",
            "Teleport; height snaps to the grid unless keep_height.")
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
                    direction = name == "north"  ? glm::vec2(0, -1)
                                : name == "east" ? glm::vec2(1, 0)
                                : name == "west" ? glm::vec2(-1, 0)
                                                 : glm::vec2(0, 1);
                } else {
                    direction = lua::vecXZ(target, t.facing);
                }
                if (glm::length(direction) > 1e-4f) {
                    t.facing = glm::normalize(direction);
                }
            },
            "fun(target: Entity|string|{x: number, z: number})",
            "Face an entity, a compass direction ('north'...) or a vector.")
        .method(
            "set_yaw", [](const EntityRef& e, float yaw) { e.get<Transform>().yaw = yaw; },
            "fun(radians: number)", "Rotation for mesh entities.")
        .method(
            "configure", [](EntityRef& e, sol::table spec) { configureEntity(e, spec); }, "fun(spec: table)",
            "Add or change components (same fields as world:spawn).")
        .method(
            "move_to",
            [](const EntityRef& e, sol::object target, sol::optional<sol::table> options) {
                const bool path = options ? lua::get(*options, "path", true) : true;
                if (options && (*options)["speed"].valid()) {
                    e.owner().registry().get_or_emplace<Mover>(e.id).speed = options->get<float>("speed");
                }
                e.owner().moveTo(e.id, lua::vec3(target), path);
            },
            "fun(target: table, options?: {path?: boolean, speed?: number})",
            "Walk to a position (pathfinding by default). Emits 'arrived' or 'blocked'.")
        .method(
            "stop", [](const EntityRef& e) { e.owner().stop(e.id); }, "fun()", "Stop moving.")
        .method(
            "drive",
            [](const EntityRef& e, float x, float z) {
                e.owner().registry().get_or_emplace<Mover>(e.id).drive = {x, z};
            },
            "fun(x: number, z: number)", "Steer this tick with a velocity in units/second (player control).")
        .method(
            "jump",
            [](const EntityRef& e, sol::object target, sol::optional<sol::table> options) {
                const auto& t = e.get<Transform>();
                const float height = options ? lua::get(*options, "height", 1.0f) : 1.0f;
                const float duration = options ? lua::get(*options, "duration", 0.5f) : 0.5f;
                const int layer = options ? lua::get(*options, "layer", t.layer) : t.layer;
                e.owner().jump(e.id, lua::vec3(target), height, duration, layer);
            },
            "fun(target: table, options?: {height?: number, duration?: number, layer?: integer})",
            "Arc to a position. Emits 'landed'.")
        .method(
            "moving", [](const EntityRef& e) { return e.owner().moving(e.id); }, "fun(): boolean",
            "Walking, jumping or being driven.")
        .method(
            "can_occupy",
            [](const EntityRef& e, sol::object position, sol::optional<int> layer) {
                return e.owner().canOccupy(e.id, lua::vec3(position),
                                           layer.value_or(e.get<Transform>().layer));
            },
            "fun(position: table, layer?: integer): boolean", "Whether the entity would fit there.")
        .method(
            "mover",
            [](const EntityRef& e, sol::optional<sol::table> t) {
                auto& m = e.owner().registry().get_or_emplace<Mover>(e.id);
                if (t) {
                    applyMover(m, *t);
                }
            },
            "fun(settings?: {speed?: number, max_step?: number, radius?: number, collide?: boolean})",
            "Add or configure movement.")
        .method(
            "sprite",
            [](sol::this_state s, const EntityRef& e, sol::optional<sol::table> t) {
                auto& sprite = e.owner().registry().get_or_emplace<Sprite>(e.id);
                if (t) {
                    applySprite(sprite, *t);
                }
                return spriteTable(s, sprite);
            },
            "fun(settings?: table): table", "Add or change the sprite; returns its current state.")
        .method(
            "clip",
            [](const EntityRef& e, const std::string& clip, sol::optional<bool> restart) {
                e.owner().setClip(e.id, clip, restart.value_or(true));
            },
            "fun(clip: string, restart?: boolean)", "Play a sprite clip.")
        .method(
            "mesh",
            [](const EntityRef& e, sol::optional<sol::table> t) {
                auto& m = e.owner().registry().get_or_emplace<MeshInstance>(e.id);
                if (t) {
                    applyMesh(m, *t);
                }
            },
            "fun(settings?: table)", "Add or change a glTF mesh (props, doors...).")
        .method(
            "play",
            [](const EntityRef& e, const std::string& animation, sol::optional<bool> loop) {
                e.owner().playMesh(e.id, animation, loop.value_or(false));
            },
            "fun(animation: string, loop?: boolean)", "Play a mesh animation by name.")
        .method(
            "light",
            [](const EntityRef& e, sol::object t) {
                auto& registry = e.owner().registry();
                if (t.get_type() != sol::type::table) {
                    registry.remove<LightSource>(e.id);
                    return;
                }
                auto settings = LightSettings::fromJson(luaToJson(t));
                if (const auto* existing = registry.try_get<LightSource>(e.id);
                    existing && !t.as<sol::table>()["position"].valid()) {
                    settings.position = existing->light.position;
                }
                registry.emplace_or_replace<LightSource>(e.id, LightSource{settings});
            },
            "fun(settings: table|nil)", "Attach a light (position is an offset); nil removes it.")
        .method(
            "emitter",
            [](const EntityRef& e, sol::object t) {
                auto& registry = e.owner().registry();
                if (t.get_type() == sol::type::boolean) {
                    if (auto* emitter = registry.try_get<Emitter>(e.id)) {
                        emitter->active = t.as<bool>();
                    }
                    return;
                }
                if (t.get_type() != sol::type::table) {
                    registry.remove<Emitter>(e.id);
                    return;
                }
                registry.emplace_or_replace<Emitter>(e.id, Emitter{EmitterSettings::fromJson(luaToJson(t))});
            },
            "fun(settings: table|boolean|nil)", "Attach particles; a boolean pauses/resumes; nil removes.")
        .method(
            "collider",
            [](const EntityRef& e, sol::object t) {
                auto& registry = e.owner().registry();
                if (t.get_type() != sol::type::table) {
                    registry.remove<Collider>(e.id);
                    return;
                }
                applyCollider(registry.get_or_emplace<Collider>(e.id), t);
            },
            "fun(settings: {radius?: number, height?: number, solid?: boolean}|nil)",
            "Contact shape. Solid colliders block movers; all report contacts.")
        .method(
            "audio",
            [](const EntityRef& e, sol::object t) {
                auto& registry = e.owner().registry();
                if (t.get_type() != sol::type::table) {
                    if (const auto* audio = registry.try_get<AudioEmitter>(e.id);
                        audio && audio->voice >= 0) {
                        e.engine->audio().stopSound(audio->voice, 0.3);
                    }
                    registry.remove<AudioEmitter>(e.id);
                    return;
                }
                applyAudio(registry.get_or_emplace<AudioEmitter>(e.id), t);
            },
            "fun(settings: {sound: string, radius?: number, volume?: number}|nil)",
            "Looping positional sound (waterfalls, fires).")
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
                if (ref.reference == LUA_NOREF || ref.reference == -2) {
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
                auto& t = e.get<Transform>();
                t.parent = parent.id;
                t.offset = lua::vec3(offset);
            },
            "fun(parent: Entity, offset?: table)", "Follow another entity (emote balloons, held items).")
        .method(
            "detach", [](const EntityRef& e) { e.get<Transform>().parent = entt::null; }, "fun()",
            "Stop following.")
        .method(
            "screen_position",
            [](const EntityRef& e, sol::optional<float> lift) {
                auto& world = e.owner();
                const auto viewport = e.engine->viewport();
                const float aspect = static_cast<float>(viewport.x) / static_cast<float>(viewport.y);
                const auto ndc = world.camera().project(
                    e.get<Transform>().position + glm::vec3(0, lift.value_or(0), 0), aspect);
                const glm::vec2 window((ndc.x * 0.5f + 0.5f) * static_cast<float>(viewport.x),
                                       (0.5f - ndc.y * 0.5f) * static_cast<float>(viewport.y));
                const auto canvas = e.engine->canvas().fromWindow(window, viewport);
                return std::make_tuple(canvas.x, canvas.y, ndc.z <= 1);
            },
            "fun(lift?: number): number, number, boolean",
            "Canvas position of the entity (plus `lift` units up); third value false when behind the camera.")
        .method(
            "destroy",
            [](EntityRef& e) {
                if (e.valid()) {
                    eraseScriptRef(e);
                    e.owner().destroy(e.id);
                }
            },
            "fun()", "Remove the entity.");
}
} // namespace fe
