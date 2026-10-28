#include "engine/nav/Query.h"
#include "engine/script/bindings/Convert.h"
#include "engine/script/bindings/WorldTypes.h"

namespace fe {
void configureEntity(EntityRef& e, const sol::table& spec);

namespace {
const char* rampName(Ramp ramp) {
    static constexpr const char* Names[] = {"none", "north", "east", "south", "west"};
    return Names[static_cast<int>(ramp)];
}

Ramp rampFromName(const std::string& name) {
    static constexpr const char* Names[] = {"none", "north", "east", "south", "west"};
    for (int i = 0; i < 5; ++i) {
        if (name == Names[i]) {
            return static_cast<Ramp>(i);
        }
    }
    throw std::runtime_error("Unknown ramp '" + name + "'");
}

const char* eventName(WorldEvent::Kind kind) {
    switch (kind) {
    case WorldEvent::Kind::ContactBegin:
        return "contact_begin";
    case WorldEvent::Kind::ContactEnd:
        return "contact_end";
    case WorldEvent::Kind::Arrived:
        return "arrived";
    case WorldEvent::Kind::Blocked:
        return "blocked";
    case WorldEvent::Kind::Landed:
        return "landed";
    case WorldEvent::Kind::AnimationEvent:
        return "anim_event";
    case WorldEvent::Kind::AnimationFinished:
        return "anim_finished";
    case WorldEvent::Kind::ZoneEnter:
        return "zone_enter";
    case WorldEvent::Kind::ZoneExit:
        return "zone_exit";
    }
    return "unknown";
}

StepRules stepRules(World& world, const sol::optional<sol::table>& options) {
    StepRules rules;
    if (!options) {
        return rules;
    }
    const auto& t = *options;
    rules.diagonal = lua::get(t, "diagonal", rules.diagonal);
    const float step = lua::get(t, "max_step", rules.maxUp);
    rules.maxUp = lua::get(t, "max_up", step);
    rules.maxDown = lua::get(t, "max_down", step);
    rules.blockedTags = world.grid().tagMask(lua::strings(t["blocked_tags"]));
    if (t["tag_costs"].valid()) {
        sol::table costs = t["tag_costs"];
        for (const auto& [key, value] : costs) {
            const auto mask = world.grid().tagMask({key.as<std::string>()});
            if (mask) {
                rules.tagCosts.emplace_back(mask, value.as<float>());
            }
        }
    }
    for (const char* key : {"blocked", "no_stop"}) {
        if (!t[key].valid()) {
            continue;
        }
        sol::table cells = t[key];
        auto& set = std::string(key) == "blocked" ? rules.blocked : rules.noStop;
        for (std::size_t i = 1; i <= cells.size(); ++i) {
            set.insert(cellKey(lua::cell(cells[i])));
        }
    }
    return rules;
}

sol::table cellList(sol::state_view lua, const std::vector<GridPos>& cells) {
    auto out = lua.create_table(static_cast<int>(cells.size()), 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        out[i + 1] = lua::table(lua, cells[i]);
    }
    return out;
}

std::vector<GridPos> cellsFrom(const sol::object& value) {
    std::vector<GridPos> cells;
    if (value.get_type() != sol::type::table) {
        return cells;
    }
    const sol::table t = value;
    for (std::size_t i = 1; i <= t.size(); ++i) {
        cells.push_back(lua::cell(t[i]));
    }
    return cells;
}

sol::table cellTable(sol::state_view lua, World& world, GridPos p) {
    const auto& grid = world.grid();
    const auto& cell = grid.at(p);
    auto t = lua.create_table_with("x", p.x, "z", p.z, "layer", p.layer, "height", cell.height, "walkable",
                                   cell.walkable, "ramp", rampName(cell.ramp));
    t["tags"] = sol::as_table(grid.tagsOf(cell.tags));
    if (const auto* props = grid.cellProps(p)) {
        t["props"] = jsonToLua(lua, *props);
    }
    return t;
}

glm::vec2 canvasToNdc(Engine& engine, float x, float y) {
    const auto viewport = engine.viewport();
    const auto window = engine.canvas().toWindow({x, y}, viewport);
    return {window.x / static_cast<float>(viewport.x) * 2 - 1,
            1 - window.y / static_cast<float>(viewport.y) * 2};
}

float aspect(Engine& engine) {
    return static_cast<float>(engine.viewport().x) / static_cast<float>(engine.viewport().y);
}

void bindGrid(TypeBinder<WorldRef>& type) {
    type.method(
            "grid_size",
            [](const WorldRef& w) {
                const auto& g = w.get().grid();
                return std::make_tuple(g.width(), g.depth(), g.layerCount());
            },
            "fun(): integer, integer, integer", "Grid width, depth and layer count.")
        .method(
            "cell",
            [](sol::this_state s, const WorldRef& w, int x, int z, sol::optional<int> layer) -> sol::object {
                const GridPos p{x, z, layer.value_or(0)};
                if (!w.get().grid().contains(p)) {
                    return sol::lua_nil;
                }
                return cellTable(s, w.get(), p);
            },
            "fun(x: integer, z: integer, layer?: integer): {height: number, walkable: boolean, ramp: string, "
            "tags: string[], props?: table}?",
            "Grid cell data, nil outside the map.")
        .method(
            "set_cell",
            [](const WorldRef& w, int x, int z, int layer, sol::table t) {
                auto& grid = w.get().grid();
                auto& cell = grid.at(x, z, layer);
                cell.height = lua::get(t, "height", cell.height);
                cell.walkable = lua::get(t, "walkable", cell.walkable);
                if (t["ramp"].valid()) {
                    cell.ramp = rampFromName(t.get<std::string>("ramp"));
                }
                if (t["tags"].valid()) {
                    cell.tags = 0;
                    for (const auto& tag : lua::strings(t["tags"])) {
                        cell.tags |= grid.tagBit(tag, true);
                    }
                }
                for (const auto& tag : lua::strings(t["add_tags"])) {
                    cell.tags |= grid.tagBit(tag, true);
                }
                for (const auto& tag : lua::strings(t["remove_tags"])) {
                    cell.tags &= ~grid.tagBit(tag, false);
                }
                if (t["props"].valid()) {
                    grid.setCellProps({x, z, layer}, luaToJson(t["props"]));
                }
            },
            "fun(x: integer, z: integer, layer: integer, changes: {height?: number, walkable?: boolean, "
            "ramp?: string, tags?: string[], add_tags?: string[], remove_tags?: string[], props?: table})",
            "Change a cell at runtime (burning grass, frozen water, collapsing bridges).")
        .method(
            "cells_with_tag",
            [](sol::this_state s, const WorldRef& w, const std::string& tag) {
                auto& grid = w.get().grid();
                const auto mask = grid.tagMask({tag});
                std::vector<GridPos> cells;
                for (int l = 0; mask && l < grid.layerCount(); ++l) {
                    for (int z = 0; z < grid.depth(); ++z) {
                        for (int x = 0; x < grid.width(); ++x) {
                            if (grid.at(x, z, l).tags & mask) {
                                cells.push_back({x, z, l});
                            }
                        }
                    }
                }
                return cellList(s, cells);
            },
            "fun(tag: string): {x: integer, z: integer, layer: integer}[]", "All cells carrying a tag.")
        .method(
            "height_at",
            [](const WorldRef& w, float x, float z, sol::optional<int> layer) {
                return w.get().grid().heightAt({x, z}, layer.value_or(0));
            },
            "fun(x: number, z: number, layer?: integer): number", "Ground height at a position.")
        .method(
            "path",
            [](sol::this_state s, const WorldRef& w, sol::object from, sol::object to,
               sol::optional<sol::table> rules) {
                return cellList(
                    s, findPath(w.get().grid(), lua::cell(from), lua::cell(to), stepRules(w.get(), rules)));
            },
            "fun(from: table, to: table, rules?: table): table[]",
            "A* path of cells (excluding the start). Rules: diagonal, max_step, max_up, max_down, "
            "blocked_tags, tag_costs, blocked, no_stop.")
        .method(
            "reachable",
            [](sol::this_state s, const WorldRef& w, sol::object start, float budget,
               sol::optional<sol::table> rules) {
                sol::state_view lua(s);
                const auto cells =
                    reachable(w.get().grid(), lua::cell(start), budget, stepRules(w.get(), rules));
                auto out = lua.create_table(static_cast<int>(cells.size()), 0);
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    const auto& c = cells[i];
                    out[i + 1] =
                        lua.create_table_with("x", c.pos.x, "z", c.pos.z, "layer", c.pos.layer, "cost",
                                              c.cost, "parent", c.parent + 1, "stop", c.stoppable);
                }
                return out;
            },
            "fun(start: table, budget: number, rules?: table): {x: integer, z: integer, layer: integer, "
            "cost: number, parent: integer, stop: boolean}[]",
            "Every cell reachable within a movement budget (tactical move ranges). `parent` indexes the list "
            "(0 = start).")
        .method(
            "line_of_sight",
            [](const WorldRef& w, sol::object from, sol::object to, sol::optional<int> layer,
               sol::object blocking) {
                auto& grid = w.get().grid();
                return lineOfSight(grid, lua::vec3(from), lua::vec3(to), layer.value_or(0),
                                   grid.tagMask(lua::strings(blocking)));
            },
            "fun(from: table, to: table, layer?: integer, blocking_tags?: string[]): boolean",
            "Whether terrain blocks the straight line between two points.")
        .method(
            "area",
            [](sol::this_state s, const WorldRef& w, sol::object centre, const std::string& shape,
               int minRange, int maxRange, sol::object direction) {
                const auto parsed = parseAreaShape(shape);
                if (!parsed) {
                    throw std::runtime_error("Unknown area shape '" + shape + "'");
                }
                const auto origin = lua::cell(centre);
                const auto dir = lua::vecXZ(direction, {0, 1});
                auto& grid = w.get().grid();
                std::vector<GridPos> cells;
                for (const auto offset : areaOffsets(
                         *parsed, minRange, maxRange,
                         {static_cast<int>(std::round(dir.x)), static_cast<int>(std::round(dir.y))})) {
                    const GridPos p{origin.x + offset.x, origin.z + offset.y, origin.layer};
                    if (grid.contains(p)) {
                        cells.push_back(p);
                    }
                }
                return cellList(s, cells);
            },
            "fun(centre: table, shape: 'diamond'|'square'|'cross'|'line'|'cone'|'ring', min: integer, max: "
            "integer, direction?: table): table[]",
            "Cells covered by a shape around a cell (attack ranges, areas of effect).")
        .method(
            "pick",
            [](sol::this_state s, const WorldRef& w, float x,
               float y) -> std::tuple<sol::object, sol::object> {
                auto& world = w.get();
                glm::vec3 origin, direction;
                world.camera().ray(canvasToNdc(*w.engine, x, y), aspect(*w.engine), origin, direction);
                const auto hit = raycastGrid(world.grid(), origin, direction, world.camera().farPlane);
                if (!hit) {
                    return {sol::lua_nil, sol::lua_nil};
                }
                return {lua::table(s, hit->cell), lua::table(s, hit->point)};
            },
            "fun(x: number, y: number): table?, table?",
            "Grid cell and point under a canvas position (mouse picking).");
}

void bindOverlays(TypeBinder<WorldRef>& type) {
    type.method(
            "overlay",
            [](const WorldRef& w, const std::string& name, sol::object cells,
               sol::optional<sol::table> options) {
                auto& layer = w.get().overlays()[name];
                layer.cells = cellsFrom(cells);
                if (options) {
                    layer.color = lua::color((*options)["color"], layer.color);
                    layer.pulse = lua::get(*options, "pulse", layer.pulse);
                    layer.order = lua::get(*options, "order", layer.order);
                    layer.border = lua::get(*options, "border", layer.border);
                    layer.visible = lua::get(*options, "visible", true);
                }
            },
            "fun(name: string, cells: table[], options?: {color?: any, pulse?: number, order?: integer, "
            "border?: boolean, visible?: boolean})",
            "Highlight grid cells (move and attack ranges).")
        .method(
            "clear_overlay",
            [](const WorldRef& w, sol::optional<std::string> name) {
                if (name) {
                    w.get().overlays().erase(*name);
                    w.get().overlayPaths().erase(*name);
                } else {
                    w.get().overlays().clear();
                    w.get().overlayPaths().clear();
                }
            },
            "fun(name?: string)", "Remove one overlay or all of them.")
        .method(
            "overlay_path",
            [](const WorldRef& w, const std::string& name, sol::object cells, sol::object color) {
                auto& path = w.get().overlayPaths()[name];
                path.cells = cellsFrom(cells);
                path.color = lua::color(color, path.color);
            },
            "fun(name: string, cells: table[], color?: any)", "Draw a path arrow through cells.")
        .method(
            "cursor",
            [](const WorldRef& w, sol::object cell, sol::object color) {
                auto& cursor = w.get().cursor();
                cursor.visible = cell.get_type() == sol::type::table;
                if (cursor.visible) {
                    cursor.cell = lua::cell(cell);
                }
                cursor.color = lua::color(color, cursor.color);
            },
            "fun(cell: table|nil, color?: any)", "Show the grid cursor on a cell; nil hides it.")
        .method(
            "burst",
            [](const WorldRef& w, sol::object position, sol::table settings, sol::optional<int> count) {
                const auto parsed = EmitterSettings::fromJson(luaToJson(settings));
                w.get().burst(lua::vec3(position), parsed,
                              count.value_or(parsed.burst > 0 ? parsed.burst : 12));
            },
            "fun(position: table, settings: table, count?: integer)", "One-shot particle burst.");
}

void bindMapData(TypeBinder<WorldRef>& type) {
    type.readonly(
            "name", [](const WorldRef& w) { return w.get().name(); }, "string", "World name.")
        .readonly(
            "map_id", [](const WorldRef& w) { return w.get().map().id; }, "string", "Map folder id.")
        .readonly(
            "map_name", [](const WorldRef& w) { return w.get().map().name; }, "string", "Display name.")
        .method(
            "props", [](sol::this_state s, const WorldRef& w) { return jsonToLua(s, w.get().map().props); },
            "fun(): table", "Map properties (music, encounters, anything a game stores).")
        .method(
            "points",
            [](sol::this_state s, const WorldRef& w) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                for (const auto& [name, p] : w.get().map().points) {
                    out[name] = lua.create_table_with("position", lua::table(lua, p.position), "facing",
                                                      lua::tableXZ(lua, p.facing), "layer", p.layer);
                }
                return out;
            },
            "fun(): table<string, {position: table, facing: table, layer: integer}>",
            "Named points (spawns).")
        .method(
            "placements",
            [](sol::this_state s, const WorldRef& w) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                for (const auto& p : w.get().map().placements) {
                    out.add(lua.create_table_with(
                        "id", p.id, "type", p.type, "position", lua::table(lua, p.position), "facing",
                        lua::tableXZ(lua, p.facing), "layer", p.layer, "props", jsonToLua(lua, p.props)));
                }
                return out;
            },
            "fun(): {id: string, type: string, position: table, facing: table, layer: integer, props: "
            "table}[]",
            "Authored placements; scripts decide what to spawn for each type.")
        .method(
            "zones",
            [](sol::this_state s, const WorldRef& w) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                for (const auto& z : w.get().map().zones) {
                    out.add(lua.create_table_with("id", z.id, "type", z.type, "min", lua::tableXZ(lua, z.min),
                                                  "max", lua::tableXZ(lua, z.max), "layer", z.layer, "props",
                                                  jsonToLua(lua, z.props)));
                }
                return out;
            },
            "fun(): {id: string, type: string, min: table, max: table, layer: integer, props: table}[]",
            "Authored zones (camera regions, encounter areas, triggers).")
        .method(
            "zones_at",
            [](sol::this_state s, const WorldRef& w, sol::object position, sol::optional<int> layer) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                const auto p = lua::vec3(position);
                for (const auto& z : w.get().map().zones) {
                    if (z.contains(p, layer.value_or(0))) {
                        out.add(lua.create_table_with("id", z.id, "type", z.type, "props",
                                                      jsonToLua(lua, z.props)));
                    }
                }
                return out;
            },
            "fun(position: table, layer?: integer): {id: string, type: string, props: table}[]",
            "Zones containing a position.")
        .method(
            "environment",
            [](sol::this_state s, const WorldRef& w) { return jsonToLua(s, w.get().environment().toJson()); },
            "fun(): table", "Lighting, fog, sky, weather and post-processing settings.")
        .method(
            "set_environment",
            [](const WorldRef& w, sol::table changes) {
                auto merged = w.get().environment().toJson();
                merged.merge_patch(luaToJson(changes));
                w.get().environment() = Environment::fromJson(merged);
            },
            "fun(changes: table)",
            "Change environment values (day/night, weather, post effects). Only this world changes; the map "
            "file does not.")
        .method(
            "time", [](const WorldRef& w) { return w.get().time(); }, "fun(): number", "Simulated seconds.");
}

void bindEntities(TypeBinder<WorldRef>& type) {
    type.method(
            "spawn",
            [](const WorldRef& w, sol::table spec) {
                auto& world = w.get();
                const auto position = lua::vec3(spec["position"]);
                const int layer = lua::get(spec, "layer", 0);
                const auto entity = world.spawn(lua::get<std::string>(spec, "id", ""), position, layer);
                if (!lua::get(spec, "keep_height", false) && !spec["position"]["y"].valid()) {
                    world.place(entity, position, layer);
                }
                EntityRef ref{w.world, entity, w.engine};
                configureEntity(ref, spec);
                return ref;
            },
            "fun(spec: {id?: string, position: table, layer?: integer, facing?: table, yaw?: number, scale?: "
            "number|table, tags?: string[], sprite?: table, mesh?: table, light?: table, emitter?: table, "
            "collider?: table, mover?: table, audio?: table, props?: table, keep_height?: boolean}): Entity",
            "Create an entity with the given components.")
        .method(
            "find",
            [](const WorldRef& w, const std::string& id) -> sol::optional<EntityRef> {
                const auto entity = w.get().find(id);
                if (entity == entt::null) {
                    return sol::nullopt;
                }
                return EntityRef{w.world, entity, w.engine};
            },
            "fun(id: string): Entity?", "Entity by id.")
        .method(
            "query",
            [](sol::this_state s, const WorldRef& w, sol::optional<sol::table> filter) {
                sol::state_view lua(s);
                std::string tag;
                std::optional<glm::vec3> centre;
                float radius = 0;
                if (filter) {
                    tag = lua::get<std::string>(*filter, "tag", "");
                    if ((*filter)["near"].valid()) {
                        centre = lua::vec3((*filter)["near"]);
                        radius = lua::get(*filter, "radius", 1.0f);
                    }
                }
                auto out = lua.create_table();
                for (const auto entity : w.get().query(tag, centre, radius)) {
                    out.add(EntityRef{w.world, entity, w.engine});
                }
                return out;
            },
            "fun(filter?: {tag?: string, near?: table, radius?: number}): Entity[]",
            "Entities with a tag and/or within a radius.")
        .method(
            "events",
            [](sol::this_state s, const WorldRef& w) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                for (const auto& event : w.get().takeEvents()) {
                    auto t = lua.create_table_with("type", eventName(event.kind));
                    if (w.get().valid(event.a)) {
                        t["a"] = EntityRef{w.world, event.a, w.engine};
                    }
                    if (w.get().valid(event.b)) {
                        t["b"] = EntityRef{w.world, event.b, w.engine};
                    }
                    if (!event.name.empty()) {
                        t["name"] = event.name;
                    }
                    out.add(t);
                }
                return out;
            },
            "fun(): {type: string, a?: Entity, b?: Entity, name?: string}[]",
            "Events since the last call: contact_begin/end, arrived, blocked, landed, anim_event, "
            "anim_finished, zone_enter/exit.")
        .method(
            "draw",
            [](const WorldRef& w) {
                auto locked = w.world.lock();
                if (!locked) {
                    throw std::runtime_error("This world has been unloaded");
                }
                w.engine->queueDraw(locked);
            },
            "fun()", "Render this world this frame (call from draw; several worlds draw in call order).")
        .method(
            "camera",
            [](const WorldRef& w) {
                w.ensureLoaded();
                return CameraRef{w.world, w.engine};
            },
            "fun(): Camera", "The world's camera.");
}
} // namespace

void bindWorld(Engine& engine) {
    auto& host = engine.lua();
    bindEntity(engine);
    bindCamera(engine);
    TypeBinder<WorldRef> type(host, engine.docs(), "World",
                              "A loaded map: grid, entities, camera, lights and environment.");
    type.raw()[sol::meta_function::equal_to] = [](const WorldRef& a, const WorldRef& b) {
        return !a.world.owner_before(b.world) && !b.world.owner_before(a.world);
    };
    bindMapData(type);
    bindEntities(type);
    bindGrid(type);
    bindOverlays(type);
    ModuleBinder(host, engine.docs(), "fe.world",
                 "Load and manage worlds (maps). Several can be loaded at once.")
        .fn(
            "load",
            [&engine](const std::string& name, const std::string& mapId) {
                auto world = engine.loadWorld(name, mapId);
                auto* raw = world.get();
                lua_State* L = engine.lua().lua().lua_state();
                world->setDestroyHook([raw, L](entt::entity entity) {
                    if (auto* ref = raw->registry().try_get<ScriptRef>(entity); ref && ref->reference >= 0) {
                        luaL_unref(L, LUA_REGISTRYINDEX, ref->reference);
                        ref->reference = LUA_NOREF;
                    }
                });
                return WorldRef{world, &engine};
            },
            "fun(name: string, map: string): World",
            "Load maps/<map>/map.json as a world called `name` (replacing one of that name).")
        .fn(
            "get",
            [&engine](const std::string& name) -> sol::optional<WorldRef> {
                auto world = engine.world(name);
                if (!world) {
                    return sol::nullopt;
                }
                return WorldRef{world, &engine};
            },
            "fun(name: string): World?", "A loaded world by name.")
        .fn(
            "unload", [&engine](const std::string& name) { engine.unloadWorld(name); }, "fun(name: string)",
            "Unload a world.")
        .fn(
            "list",
            [&engine] {
                std::vector<std::string> names;
                for (const auto& [name, world] : engine.worlds()) {
                    (void)world;
                    names.push_back(name);
                }
                return sol::as_table(names);
            },
            "fun(): string[]", "Names of loaded worlds.");
}

void bindCamera(Engine& engine) {
    auto& host = engine.lua();
    TypeBinder<CameraRef>(host, engine.docs(), "Camera",
                          "Orbit camera. Scripts implement follow, zones, rotation and cutscene paths by "
                          "setting these each tick.")
        .property(
            "target", [](const CameraRef& c) { return lua::table(c.engine->lua().lua(), c.get().target); },
            [](const CameraRef& c, sol::object v) { c.get().target = lua::vec3(v, c.get().target); }, "table",
            "Point the camera looks at.")
        .property(
            "yaw", [](const CameraRef& c) { return c.get().yaw; },
            [](const CameraRef& c, float v) { c.get().yaw = v; }, "number",
            "Degrees around the vertical axis.")
        .property(
            "pitch", [](const CameraRef& c) { return c.get().pitch; },
            [](const CameraRef& c, float v) { c.get().pitch = std::clamp(v, -89.0f, 89.9f); }, "number",
            "Degrees above the horizon (90 = top-down).")
        .property(
            "distance", [](const CameraRef& c) { return c.get().distance; },
            [](const CameraRef& c, float v) { c.get().distance = std::max(0.1f, v); }, "number",
            "Distance from the target.")
        .property(
            "fov", [](const CameraRef& c) { return c.get().fov; },
            [](const CameraRef& c, float v) { c.get().fov = std::clamp(v, 1.0f, 170.0f); }, "number",
            "Vertical field of view in degrees.")
        .property(
            "shake", [](const CameraRef& c) { return lua::table(c.engine->lua().lua(), c.get().shake); },
            [](const CameraRef& c, sol::object v) { c.get().shake = lua::vec3(v); }, "table",
            "Visual offset (screen shake), not interpolated.")
        .method(
            "snap", [](const CameraRef& c) { c.get().snap(); }, "fun()", "Skip interpolation after a cut.")
        .method(
            "position",
            [](sol::this_state s, const CameraRef& c) { return lua::table(s, c.get().position()); },
            "fun(): table", "Eye position.")
        .method(
            "project",
            [](const CameraRef& c, sol::object point) {
                auto& engine = *c.engine;
                const auto viewport = engine.viewport();
                const auto ndc = c.get().project(lua::vec3(point), aspect(engine));
                const glm::vec2 window((ndc.x * 0.5f + 0.5f) * static_cast<float>(viewport.x),
                                       (0.5f - ndc.y * 0.5f) * static_cast<float>(viewport.y));
                const auto canvas = engine.canvas().fromWindow(window, viewport);
                return std::make_tuple(canvas.x, canvas.y, ndc.z <= 1);
            },
            "fun(point: table): number, number, boolean", "Canvas position of a world point.")
        .method(
            "ray",
            [](sol::this_state s, const CameraRef& c, float x, float y) {
                glm::vec3 origin, direction;
                c.get().ray(canvasToNdc(*c.engine, x, y), aspect(*c.engine), origin, direction);
                return std::make_tuple(lua::table(s, origin), lua::table(s, direction));
            },
            "fun(x: number, y: number): table, table", "World ray through a canvas position.");
}
} // namespace fe
