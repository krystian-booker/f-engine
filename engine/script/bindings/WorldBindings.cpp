#include "engine/nav/Query.h"
#include "engine/script/bindings/Convert.h"
#include "engine/script/bindings/WorldTypes.h"

namespace fe {
void configureEntity(EntityRef& e, const sol::table& spec);

std::tuple<float, float, bool> projectToCanvas(Engine& engine, const Camera& camera, glm::vec3 point,
                                               double alpha) {
    const auto viewport = engine.viewport();
    const float aspect = static_cast<float>(viewport.x) / static_cast<float>(viewport.y);
    const auto ndc = camera.project(point, aspect, alpha);
    const glm::vec2 window((ndc.x * 0.5f + 0.5f) * static_cast<float>(viewport.x),
                           (0.5f - ndc.y * 0.5f) * static_cast<float>(viewport.y));
    const auto canvas = engine.canvas().fromWindow(window, viewport);
    return {canvas.x, canvas.y, ndc.z <= 1};
}

namespace {
WorldDraw worldDraw(Engine& engine, const std::shared_ptr<World>& world, const sol::object& options) {
    WorldDraw draw;
    draw.world = world;
    const auto spec = lua::options(options, "world:draw options");
    if (!spec) {
        return draw;
    }
    const auto& t = *spec;
    if (const auto rect = lua::field(t, "rect"); !lua::absent(rect)) {
        const auto r = lua::rect(rect);
        draw.rect = Rect{r.x, r.y, r.z, r.w};
        if (!(draw.rect->w >= 1 && draw.rect->h >= 1 && draw.rect->w <= 8192 && draw.rect->h <= 8192)) {
            throw std::runtime_error("world:draw rect needs a width and height of 1 to 8192 canvas pixels");
        }
    }
    if (const auto target = lua::opt<std::string>(lua::field(t, "target"), "Field 'target'")) {
        if (target->empty() ||
            target->find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
            throw std::runtime_error("world:draw target names use a-z, 0-9, _ and -");
        }
        draw.target = *target;
    }
    if (const auto override = lua::options(lua::field(t, "camera"), "Field 'camera'")) {
        const auto& c = *override;
        Camera camera = world->camera();
        if (const auto target = lua::field(c, "target"); !lua::absent(target)) {
            camera.target = lua::position(target);
        }
        camera.yaw = lua::get(c, "yaw", camera.yaw);
        camera.pitch = std::clamp(lua::get(c, "pitch", camera.pitch), -89.0f, 89.9f);
        camera.distance = std::clamp(lua::get(c, "distance", camera.distance), 0.1f, 10000.0f);
        camera.fov = std::clamp(lua::get(c, "fov", camera.fov), 1.0f, 170.0f);
        camera.orthographic = lua::get(c, "orthographic", camera.orthographic);
        if (const auto height = lua::opt<float>(lua::field(c, "ortho_height"), "Field 'ortho_height'")) {
            camera.orthoHeight = *height <= 0 ? 0.0f : lua::within(*height, 0.01f, 100000.0f, "ortho_height");
        }
        camera.shake = lua::vec3(lua::field(c, "shake"), glm::vec3(0));
        camera.snap();
        draw.camera = camera;
    }
    if (const auto post = lua::options(lua::field(t, "post"), "Field 'post'")) {
        auto environment = world->environment().toJson();
        environment.merge_patch(Json{{"post", luaToJson(*post)}});
        draw.post = Environment::fromJson(environment).post;
    }
    // Everything validated: only now does a target get (re)defined.
    if (!draw.target.empty()) {
        const auto& canvas = engine.canvas();
        const glm::vec2 size =
            draw.rect ? glm::vec2(draw.rect->w, draw.rect->h) : glm::vec2(canvas.width(), canvas.height());
        engine.canvas().defineTarget(draw.target, glm::ivec2(glm::round(size)));
    }
    return draw;
}

Ramp parseRamp(const std::string& name) {
    const auto ramp = fe::rampFromName(name);
    if (!ramp) {
        throw std::runtime_error("Unknown ramp '" + name + "'");
    }
    return *ramp;
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
    case WorldEvent::Kind::JumpStart:
        return "jump_start";
    case WorldEvent::Kind::ClimbStart:
        return "climb_start";
    case WorldEvent::Kind::ClimbEnd:
        return "climb_end";
    case WorldEvent::Kind::AnimationEvent:
        return "anim_event";
    case WorldEvent::Kind::AnimationFinished:
        return "anim_finished";
    case WorldEvent::Kind::ZoneEnter:
        return "zone_enter";
    case WorldEvent::Kind::ZoneExit:
        return "zone_exit";
    case WorldEvent::Kind::CellEnter:
        return "cell_enter";
    }
    return "unknown";
}
} // namespace

std::uint32_t gridTagBits(const Grid& grid, const sol::object& names, const char* what) {
    std::uint32_t mask = 0;
    for (const auto& name : lua::strings(names)) {
        const auto bit = grid.tagMask({name});
        if (!bit) {
            throw std::runtime_error(std::string(what) + ": the map grid has no tag '" + name + "'");
        }
        mask |= bit;
    }
    return mask;
}

StepRules stepRules(LuaHost& host, World& world, const sol::object& value) {
    StepRules rules;
    const auto options = lua::options(value, "rules");
    if (!options) {
        const auto& boxed = world.boxBlockedCells();
        rules.blocked.insert(boxed.begin(), boxed.end());
        return rules;
    }
    const auto& t = *options;
    rules.diagonal = lua::get(t, "diagonal", rules.diagonal);
    rules.ladders = lua::get(t, "ladders", rules.ladders);
    rules.footprint =
        lua::within(lua::get(t, "footprint", rules.footprint), 1, MaxFootprint, "rules.footprint");
    rules.fly = lua::get(t, "fly", rules.fly);
    const float step = lua::get(t, "max_step", rules.maxUp);
    rules.maxUp = lua::get(t, "max_up", step);
    rules.maxDown = lua::get(t, "max_down", step);
    if (rules.maxUp < 0 || rules.maxDown < 0) {
        throw std::runtime_error("max_step, max_up and max_down must be non-negative numbers");
    }
    rules.blockedTags = world.grid().tagMask(lua::strings(lua::field(t, "blocked_tags")));
    rules.requiredTags = gridTagBits(world.grid(), lua::field(t, "required_tags"), "rules.required_tags");
    if (const auto cells = lua::field(t, "blocked"); !lua::absent(cells)) {
        for (const auto& cell : lua::cells(cells, "blocked")) {
            rules.blocked.insert(cellKey(cell));
        }
    }
    if (const auto jumpRules = lua::options(lua::field(t, "jump"), "Field 'jump'")) {
        const auto& jump = *jumpRules;
        const auto distance = lua::integer(lua::field(jump, "distance"), "jump.distance");
        if (distance < 0 || distance > 16) {
            throw std::runtime_error("jump.distance must be an integer from 0 to 16");
        }
        rules.jump.distance = static_cast<int>(distance);
        rules.jump.maxUp = lua::get(jump, "max_up", rules.maxUp);
        rules.jump.maxDown = lua::get(jump, "max_down", rules.maxDown);
        rules.jump.diagonal = lua::get(jump, "diagonal", false);
        if (rules.jump.maxUp < 0 || rules.jump.maxDown < 0) {
            throw std::runtime_error("jump.max_up and jump.max_down must be non-negative numbers");
        }
    }
    if (const auto costValue = lua::field(t, "cost"); !lua::absent(costValue)) {
        if (costValue.get_type() != sol::type::function) {
            throw std::runtime_error("rules.cost must be a function");
        }
        // With the host's message handler, so any error object becomes a string.
        sol::protected_function cost(costValue, host.errorHandler());
        // Two tables reused for every call: no garbage per expanded step.
        sol::state_view lua(t.lua_state());
        sol::table from = lua.create_table_with("x", 0, "z", 0, "layer", 0);
        sol::table to = lua.create_table_with("x", 0, "z", 0, "layer", 0);
        // The search only reads the grid, which no script call can reshape, so
        // the cost function may do anything (even unload the world: the
        // binding holds it until the search ends).
        rules.cost = [&host, cost, from, to](GridPos a, GridPos b,
                                             float base) mutable -> std::optional<float> {
            from.raw_set("x", a.x, "z", a.z, "layer", a.layer);
            to.raw_set("x", b.x, "z", b.z, "layer", b.layer);
            const auto result = [&] {
                const LuaHost::Callout callout(host);
                return cost(from, to, base);
            }();
            if (!result.valid()) {
                const sol::error error = result;
                throw std::runtime_error(std::string("path cost function: ") + error.what());
            }
            const sol::object value = result;
            if (value.get_type() == sol::type::boolean && !value.as<bool>()) {
                return std::nullopt;
            }
            const auto number = lua::convert<float>(value);
            if (!number || *number < 0) {
                throw std::runtime_error(
                    "A path cost function must return a non-negative number or false, got " +
                    lua::describe(value));
            }
            return *number;
        };
    }
    // Static boxes block the cells they stand on for every planned route.
    const auto& boxed = world.boxBlockedCells();
    rules.blocked.insert(boxed.begin(), boxed.end());
    return rules;
}

namespace {
void markStep(sol::table& t, StepKind kind) {
    if (kind == StepKind::Jump) {
        t["jump"] = true;
    } else if (kind == StepKind::Climb) {
        t["climb"] = true;
    }
}

sol::table routeList(sol::state_view lua, const Route& route) {
    const auto& steps = route.steps;
    auto out = lua.create_table(static_cast<int>(steps.size()), 0);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        auto cell = lua::table(lua, steps[i].cell);
        markStep(cell, steps[i].kind);
        out[i + 1] = cell;
    }
    if (route.truncated) {
        out["truncated"] = true;
    }
    return out;
}

sol::table cellList(sol::state_view lua, const std::vector<GridPos>& cells) {
    auto out = lua.create_table(static_cast<int>(cells.size()), 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        out[i + 1] = lua::table(lua, cells[i]);
    }
    return out;
}

sol::table cellTable(sol::state_view lua, World& world, GridPos p) {
    const auto& grid = world.grid();
    const auto& cell = grid.at(p);
    auto t = lua.create_table_with("x", p.x, "z", p.z, "layer", p.layer, "height", cell.height, "walkable",
                                   cell.walkable, "ramp", rampName(cell.ramp), "rise", cell.rise);
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
} // namespace

namespace {
void bindGrid(TypeBinder<WorldRef>& type) {
    type.method(
            "grid_size",
            [](const WorldRef& w) {
                const auto& g = w.get().grid();
                return std::make_tuple(g.width(), g.depth(), g.layerCount());
            },
            "fun(): integer, integer, integer", "Grid width, depth and layer count.")
        .method(
            "wraps", [](const WorldRef& w) { return w.get().grid().wraps(); }, "fun(): boolean",
            "Whether the map is toroidal (`grid.wrap`): its east edge joins its west and its south its "
            "north. Positions stay in [0, width) x [0, depth); use `wrap_delta` for differences between "
            "them.")
        .method(
            "wrap_delta",
            [](sol::this_state s, const WorldRef& w, sol::object a, sol::object b) {
                const auto from = lua::position(a), to = lua::position(b);
                const auto d = w.get().grid().delta({from.x, from.z}, {to.x, to.z});
                return lua::table(s, glm::vec3(d.x, to.y - from.y, d.y));
            },
            "fun(a: Vec3, b: Vec3): Vec3",
            "`b - a`, the short way round on a wrapping map (the plain difference otherwise).")
        .method(
            "wrap_point",
            [](sol::this_state s, const WorldRef& w, sol::object p) {
                const auto v = lua::position(p);
                const auto flat = w.get().grid().wrapPoint({v.x, v.z});
                return lua::table(s, glm::vec3(flat.x, v.y, flat.y));
            },
            "fun(p: Vec3): Vec3",
            "The position folded into the map's range on a wrapping map; `p` itself otherwise.")
        .method(
            "cell",
            [](sol::this_state s, const WorldRef& w, sol::object cell) -> sol::object {
                const auto p = lua::cell(cell);
                if (!w.get().grid().contains(p)) {
                    return sol::lua_nil;
                }
                return cellTable(s, w.get(), p);
            },
            "fun(cell: Cell): {x: integer, z: integer, layer: integer, height: number, walkable: boolean, "
            "ramp: string, rise: number, tags: string[], props?: table}?",
            "Grid cell data, nil outside the map.")
        .method(
            "set_cell",
            [](const WorldRef& w, sol::object where, sol::object changes) {
                auto& grid = w.get().grid();
                const auto p = lua::cell(where);
                if (!grid.contains(p)) {
                    throw std::runtime_error("set_cell: cell (" + std::to_string(p.x) + ", " +
                                             std::to_string(p.z) + ", layer " + std::to_string(p.layer) +
                                             ") is outside the grid");
                }
                const auto t = lua::tableArg(changes, "set_cell changes");
                auto& cell = grid.at(p);
                cell.height = lua::within(lua::get(t, "height", cell.height), -lua::VectorBound,
                                          lua::VectorBound, "height");
                cell.walkable = lua::get(t, "walkable", cell.walkable);
                if (const auto ramp = lua::opt<std::string>(lua::field(t, "ramp"), "Field 'ramp'")) {
                    cell.ramp = parseRamp(*ramp);
                }
                cell.rise = lua::within(lua::get(t, "rise", cell.rise), 0.0f, lua::VectorBound, "rise");
                if (const auto tags = lua::field(t, "tags"); !lua::absent(tags)) {
                    cell.tags = 0;
                    for (const auto& tag : lua::strings(tags)) {
                        cell.tags |= grid.tagBit(tag, true);
                    }
                }
                for (const auto& tag : lua::strings(lua::field(t, "add_tags"))) {
                    cell.tags |= grid.tagBit(tag, true);
                }
                for (const auto& tag : lua::strings(lua::field(t, "remove_tags"))) {
                    cell.tags &= ~grid.tagBit(tag, false);
                }
                if (const auto props = lua::field(t, "props"); !lua::absent(props)) {
                    grid.setCellProps(p, luaToJson(props));
                }
            },
            "fun(cell: Cell, changes: {height?: number, walkable?: boolean, ramp?: string, rise?: number, "
            "tags?: string[], add_tags?: string[], remove_tags?: string[], props?: table})",
            "Change a cell at runtime (burning grass, frozen water, collapsing bridges); a cell outside the "
            "grid is an error. `height` is within -1000000 to 1000000 and `rise` 0 to 1000000.")
        .method(
            "height_at",
            [](const WorldRef& w, sol::object position, sol::object layer) {
                const auto p = lua::position(position);
                return w.get().grid().heightAt({p.x, p.z}, lua::opt<int>(layer, "layer").value_or(0));
            },
            "fun(position: Vec3, layer?: integer): number",
            "Ground height at a position on a layer (default 0); y is ignored.")
        .method(
            "path",
            [](sol::this_state s, const WorldRef& w, sol::object from, sol::object to, sol::object rules) {
                const auto world = w.lock();
                const auto start = lua::cell(from);
                const auto goal = lua::cell(to);
                const auto parsed = stepRules(w.engine->lua(), *world, rules);
                return routeList(s, findRoute(world->grid(), start, goal, parsed));
            },
            "fun(from: Cell, to: Cell, rules?: table): {truncated?: true, [integer]: {x: integer, z: "
            "integer, "
            "layer: integer, jump?: true, climb?: true}}",
            "A* path of cells (excluding the start; empty when none exists, found without a search when the "
            "goal cannot be entered or, without fly or jump, lies in another connected part of the grid). "
            "Rules: diagonal, max_step, max_up, max_down, "
            "footprint (1 to 8 cells per side, anchored at the cell towards +x and +z), fly (unwalkable "
            "cells and height steps are ignored), "
            "blocked_tags, required_tags (cells with none of them cannot be entered; every name must be "
            "in the map's tag list), blocked (cells that cannot be entered), jump = {distance, max_up?, "
            "max_down?, diagonal?} (gap jumps along the axes, and "
            "with diagonal = true the diagonals, over up to `distance` unwalkable, blocked-tag or sunken "
            "cells), ladders (default true; false never climbs ladder links), cost = fun(from, to, base): "
            "number|false (per step; false = impassable; the two cell tables are reused between calls; it "
            "runs under a C call, inside the search, so it must not yield: a coroutine.yield in it raises "
            "'attempt to yield across a C-call boundary'). "
            "Layer changes follow links and ladders the way movers do; steps that jump or climb a ladder are "
            "marked `jump`/`climb`. The search expands at most 200000 cells; when it stops there before "
            "reaching the goal the list has `truncated = true` and is empty or a route that may not be the "
            "cheapest.")
        .method(
            "area",
            [](sol::this_state s, const WorldRef& w, sol::object centre, const std::string& shape,
               int minRange, int maxRange, sol::object direction) {
                const auto parsed = parseAreaShape(shape);
                if (!parsed) {
                    throw std::runtime_error(
                        "Unknown area shape '" + shape +
                        "' (expected 'diamond', 'square', 'cross', 'line', 'cone' or 'ring')");
                }
                lua::within(minRange, 0, 64, "area min");
                lua::within(maxRange, minRange, 64, "area max");
                const auto origin = lua::cell(centre);
                auto& grid = w.get().grid();
                std::vector<GridPos> cells;
                for (const auto offset :
                     areaOffsets(*parsed, minRange, maxRange, lua::vecXZ(direction, {0, 1}))) {
                    const GridPos p{origin.x + offset.x, origin.z + offset.y, origin.layer};
                    if (grid.contains(p)) {
                        cells.push_back(p);
                    }
                }
                return cellList(s, cells);
            },
            "fun(centre: Cell, shape: 'diamond'|'square'|'cross'|'line'|'cone'|'ring', min: integer, max: "
            "integer, direction?: {x: number, z: number}): Cell[]",
            "Cells covered by a shape around a cell (attack ranges, areas of effect), within the grid. "
            "0 <= min <= max <= 64. `direction` (default south, {x = 0, z = 1}) snaps to the nearest of 8 "
            "directions; diagonal lines and cones follow the diagonal.")
        .method(
            "pick_entity",
            [](const WorldRef& w, float x, float y, sol::object alpha) -> sol::optional<EntityRef> {
                auto& world = w.get();
                const double a = lua::opt<double>(alpha, "alpha").value_or(w.engine->drawAlpha());
                glm::vec3 origin, direction;
                world.camera().ray(canvasToNdc(*w.engine, x, y), aspect(*w.engine), origin, direction, a);
                const auto entity = world.pickSprite(origin, direction, a, w.engine->config().pixelsPerUnit);
                if (entity == entt::null) {
                    return sol::nullopt;
                }
                return EntityRef{w.world, entity, w.engine};
            },
            "fun(x: number, y: number, alpha?: number): Entity?",
            "The visible sprite under a canvas position, using its frame's bounds; the one drawn on top when "
            "sprites overlap.")
        .method(
            "raycast",
            [](sol::this_state s, const WorldRef& w, sol::object origin, sol::object direction,
               sol::object settings) -> sol::object {
                sol::state_view lua(s);
                auto& world = w.get();
                RayOptions ray;
                ray.pixelsPerUnit = w.engine->config().pixelsPerUnit;
                ray.alpha = w.engine->drawAlpha();
                if (const auto options = lua::options(settings, "raycast options")) {
                    ray.maxDistance = lua::atLeast(lua::get(*options, "max", ray.maxDistance), 0.0f, "max");
                    ray.entities = lua::get(*options, "entities", ray.entities);
                    ray.solidOnly = lua::get(*options, "solid_only", ray.solidOnly);
                    ray.tags = lua::strings(lua::field(*options, "tags"));
                    if (const auto layers = lua::field(*options, "layers"); !lua::absent(layers)) {
                        ray.layers = 0;
                        for (const auto& layer : lua::elements(lua::tableArg(layers, "Field 'layers'"))) {
                            ray.layers |= 1u << static_cast<unsigned>(
                                              lua::within(lua::as<int>(layer, "layer"), 0, 31, "layer"));
                        }
                    }
                }
                const auto hit = world.raycast(lua::position(origin), lua::vec3(direction), ray);
                if (!hit) {
                    return sol::lua_nil;
                }
                static constexpr const char* Kinds[] = {"grid", "collider", "entity"};
                auto out = lua.create_table_with("kind", Kinds[static_cast<int>(hit->kind)], "point",
                                                 lua::table(lua, hit->point), "distance", hit->distance);
                if (hit->kind == WorldHit::Kind::Grid) {
                    out["cell"] = lua::table(lua, hit->cell);
                } else if (hit->kind == WorldHit::Kind::Collider) {
                    out["collider"] = hit->collider + 1;
                } else {
                    out["entity"] = EntityRef{w.world, hit->entity, w.engine};
                }
                return out;
            },
            "fun(origin: Vec3, direction: Vec3, options?: {max?: number, layers?: integer[], tags?: "
            "string[], entities?: boolean, solid_only?: boolean}): {kind: 'grid'|'collider'|'entity', "
            "point: Vec3, distance: number, cell?: Cell, collider?: integer, entity?: Entity}?",
            "First thing along a ray within `max` (default 200): grid surfaces and cliff faces (only "
            "`layers` "
            "when given), map colliders, and entities (their collider cylinder, or their sprite's quad as "
            "drawn "
            "when they have no collider; only entities with one of `tags` when given; none with entities = "
            "false). Entities whose collider is not solid (triggers) are passed through unless solid_only = "
            "false. `collider` is the 1-based index of the map collider hit.");
}

void bindParticles(TypeBinder<WorldRef>& type) {
    type.method(
        "burst",
        [](const WorldRef& w, sol::object position, sol::table settings, sol::object count) {
            const auto parsed = EmitterSettings::fromJson(lua::emitterJson(settings));
            w.get().burst(
                lua::position(position), parsed,
                lua::within(lua::opt<int>(count, "count").value_or(parsed.burst > 0 ? parsed.burst : 12), 0,
                            4096, "count"));
        },
        "fun(position: Vec3, settings: table, count?: integer)", "One-shot particle burst (up to 4096).");
}

// A placement as scripts read it (world:placements(), world:placement(id)).
sol::table placementTable(sol::state_view lua, const Placement& p) {
    return lua.create_table_with("id", p.id, "type", p.type, "position", lua::table(lua, p.position),
                                 "facing", lua::tableXZ(lua, p.facing), "layer", p.layer, "yaw", p.yaw,
                                 "scale", lua::table(lua, p.scale), "props", jsonToLua(lua, p.props));
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
            "fun(): table", "Map properties (music or anything else a game stores).")
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
            "fun(): table<string, {position: Vec3, facing: {x: number, z: number}, layer: integer}>",
            "Named points (spawns).")
        .method(
            "placements",
            [](sol::this_state s, const WorldRef& w) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                for (const auto& p : w.get().map().placements) {
                    out.add(placementTable(lua, p));
                }
                return out;
            },
            "fun(): {id: string, type: string, position: Vec3, facing: {x: number, z: number}, layer: "
            "integer, yaw: number, scale: Vec3, props: table}[]",
            "Authored placements; scripts decide what to spawn for each type.")
        .method(
            "placement",
            [](sol::this_state s, const WorldRef& w, const std::string& id) {
                sol::state_view lua(s);
                const auto& placements = w.get().map().placements;
                const auto found = std::find_if(placements.begin(), placements.end(),
                                                [&](const Placement& p) { return p.id == id; });
                return found == placements.end() ? sol::object(lua, sol::lua_nil)
                                                 : sol::object(placementTable(lua, *found));
            },
            "fun(id: string): {id: string, type: string, position: Vec3, facing: {x: number, z: number}, "
            "layer: integer, yaw: number, scale: Vec3, props: table}|nil",
            "The authored placement with that id, or nil (the live map: editor edits included).")
        .method(
            "zones",
            [](sol::this_state s, const WorldRef& w) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                for (const auto& z : w.get().map().zones) {
                    auto zone =
                        lua.create_table_with("id", z.id, "type", z.type, "min", lua::tableXZ(lua, z.min),
                                              "max", lua::tableXZ(lua, z.max), "layer", z.layer, "shape",
                                              zoneShapeName(z.shape), "props", jsonToLua(lua, z.props));
                    if (z.shape == ZoneShape::Polygon) {
                        auto points = lua.create_table();
                        for (const auto& point : z.points) {
                            points.add(lua::tableXZ(lua, point));
                        }
                        zone["points"] = points;
                    }
                    out.add(zone);
                }
                return out;
            },
            "fun(): {id: string, type: string, shape: 'box'|'ellipse'|'polygon', min: {x: number, z: "
            "number}, "
            "max: {x: number, z: number}, points?: {x: number, z: number}[], layer: integer, props: table}[]",
            "Authored zones (camera regions, trigger areas, anything a game defines). A zone is a box "
            "(`min` to `max`), the ellipse inscribed in that box, or a polygon whose corners are `points` "
            "(`min` and `max` are then its bounds). Entities with a mover or a collider entering and leaving "
            "one are reported by world:events() as zone_enter and zone_exit, with the zone's id as `name`.")
        .method(
            "zones_at",
            [](sol::this_state s, const WorldRef& w, sol::object position, sol::object layerArg) {
                sol::state_view lua(s);
                auto out = lua.create_table();
                const auto p = lua::position(position);
                const int layer = lua::opt<int>(layerArg, "layer").value_or(0);
                for (const auto& z : w.get().map().zones) {
                    if (z.contains(p, layer)) {
                        out.add(lua.create_table_with("id", z.id, "type", z.type, "props",
                                                      jsonToLua(lua, z.props)));
                    }
                }
                return out;
            },
            "fun(position: Vec3, layer?: integer): {id: string, type: string, props: table}[]",
            "Zones containing a position.")
        .method(
            "environment",
            [](sol::this_state s, const WorldRef& w) { return jsonToLua(s, w.get().environment().toJson()); },
            "fun(): table", "Lighting, fog, sky, weather and post-processing settings.")
        .method(
            "set_environment",
            [](const WorldRef& w, sol::table changes) {
                auto merged = w.get().environment().toJson();
                merged.merge_patch(lua::environmentJson(changes));
                w.get().environment() = Environment::fromJson(merged);
            },
            "fun(changes: table)",
            "Change environment values (day/night, weather, post effects). Only this world changes; the map "
            "file does not.")
        .method(
            "movement_tuning",
            [](sol::this_state s, const WorldRef& w) {
                const auto& t = w.get().movementTuning();
                return sol::state_view(s).create_table_with(
                    "arrive_distance", t.arriveDistance, "snap_distance", t.snapDistance, "stuck_share",
                    t.stuckShare, "stuck_seconds", t.stuckSeconds, "jump_arc_base", t.jumpArcBase,
                    "jump_arc_per_unit", t.jumpArcPerUnit, "jump_arc_per_rise", t.jumpArcPerRise,
                    "jump_minimum_seconds", t.jumpMinimumSeconds, "landing_search", t.landingSearch,
                    "gravity", t.gravity);
            },
            "fun(): {arrive_distance: number, snap_distance: number, stuck_share: number, stuck_seconds: "
            "number, jump_arc_base: number, jump_arc_per_unit: number, jump_arc_per_rise: number, "
            "jump_minimum_seconds: number, landing_search: number, gravity: number}",
            "The movers' thresholds and route-jump shape for this world (see set_movement_tuning).")
        .method(
            "set_movement_tuning",
            [](const WorldRef& w, sol::table t) {
                auto tuning = w.get().movementTuning();
                tuning.arriveDistance = lua::within(lua::get(t, "arrive_distance", tuning.arriveDistance),
                                                    0.001f, 1.0f, "arrive_distance");
                tuning.snapDistance = lua::within(lua::get(t, "snap_distance", tuning.snapDistance), 0.0f,
                                                  1.0f, "snap_distance");
                tuning.stuckShare =
                    lua::within(lua::get(t, "stuck_share", tuning.stuckShare), 0.0f, 1.0f, "stuck_share");
                tuning.stuckSeconds = lua::within(lua::get(t, "stuck_seconds", tuning.stuckSeconds), 0.05,
                                                  60.0, "stuck_seconds");
                tuning.jumpArcBase = lua::within(lua::get(t, "jump_arc_base", tuning.jumpArcBase), 0.0f,
                                                 16.0f, "jump_arc_base");
                tuning.jumpArcPerUnit = lua::within(lua::get(t, "jump_arc_per_unit", tuning.jumpArcPerUnit),
                                                    0.0f, 16.0f, "jump_arc_per_unit");
                tuning.jumpArcPerRise = lua::within(lua::get(t, "jump_arc_per_rise", tuning.jumpArcPerRise),
                                                    0.0f, 16.0f, "jump_arc_per_rise");
                tuning.jumpMinimumSeconds =
                    lua::within(lua::get(t, "jump_minimum_seconds", tuning.jumpMinimumSeconds), 0.01f, 10.0f,
                                "jump_minimum_seconds");
                tuning.landingSearch = lua::within(lua::get(t, "landing_search", tuning.landingSearch), 0.0f,
                                                   16.0f, "landing_search");
                tuning.gravity =
                    lua::within(lua::get(t, "gravity", tuning.gravity), 0.1f, 1000.0f, "gravity");
                w.get().movementTuning() = tuning;
            },
            "fun(changes: {arrive_distance?: number, snap_distance?: number, stuck_share?: number, "
            "stuck_seconds?: number, jump_arc_base?: number, jump_arc_per_unit?: number, jump_arc_per_rise?: "
            "number, jump_minimum_seconds?: number, landing_search?: number, gravity?: number})",
            "Tune this world's movers; omitted fields keep their value. `arrive_distance` (0.001-1, default "
            "0.05): a waypoint counts as reached within it, stopping short is a block; `snap_distance` (0-1, "
            "0.02): a waypoint this close is reached even when the tick's step is shorter; `stuck_share` "
            "(0-1, 0.1) and `stuck_seconds` (0.05-60, 1): a route whose ticks cover less than that share of "
            "their step for that long is `blocked`; a route jump's apex is `jump_arc_base` + "
            "`jump_arc_per_unit` x horizontal length + `jump_arc_per_rise` x |height difference| (each 0-16; "
            "0.4, 0.2, 0.5) and lasts at least `jump_minimum_seconds` (0.01-10, 0.3); `landing_search` "
            "(0-16, 1) is how far around a blocked landing the nearest free spot is looked for; `gravity` "
            "(0.1-1000, 20 units/second^2) pulls down bodies in flight after entity:launch.")
        .readonly(
            "time", [](const WorldRef& w) { return w.get().time(); }, "number",
            "Simulated seconds of this world (advanced by its ticks at its time_scale, not while paused).")
        .property(
            "paused", [](const WorldRef& w) { return w.get().paused; },
            [](const WorldRef& w, sol::object paused) { w.get().paused = lua::as<bool>(paused, "paused"); },
            "boolean",
            "When true the world stops simulating (movers, animation, particles, events) but still renders "
            "and accepts changes.")
        .property(
            "time_scale", [](const WorldRef& w) { return w.get().timeScale; },
            [](const WorldRef& w, sol::object scale) {
                const auto value = lua::as<double>(scale, "time_scale");
                if (value < 0 || value > 10) {
                    throw std::runtime_error("time_scale must be 0 to 10, got " + std::to_string(value));
                }
                w.get().timeScale = static_cast<float>(value);
            },
            "number",
            "Multiplies the tick length for this world, 0 to 10 (0 = frozen, slow motion and hit-stop below "
            "1, "
            "1 = normal, fast-forward above 1). Larger steps would let movers pass through walls.");
}

void bindEntities(TypeBinder<WorldRef>& type) {
    type.method(
            "spawn",
            [](const WorldRef& w, sol::table spec) {
                auto& world = w.get();
                const auto positionValue = lua::field(spec, "position");
                const auto position = lua::position(positionValue);
                const int layer = lua::get(spec, "layer", 0);
                const bool keepHeight = lua::get(spec, "keep_height", false);
                const bool hasHeight = !lua::absent(lua::field(positionValue.as<sol::table>(), "y")) ||
                                       !lua::absent(lua::field(positionValue.as<sol::table>(), 2));
                const auto entity = world.spawn(lua::get<std::string>(spec, "id", ""), position, layer);
                EntityRef ref{w.world, entity, w.engine};
                try {
                    if (!keepHeight && !hasHeight) {
                        world.place(entity, position, layer);
                    }
                    configureEntity(ref, spec);
                } catch (...) {
                    // A rejected component leaves no entity (and frees its id).
                    world.destroy(entity);
                    throw;
                }
                return ref;
            },
            "fun(spec: {id?: string, position: Vec3, layer?: integer, facing?: {x: number, z: number}, yaw?: "
            "number, scale?: number|Vec3, tags?: string[], sprite?: table, mesh?: table, light?: table, "
            "emitter?: table, collider?: table, mover?: table, platform?: table, audio?: table, props?: "
            "table, "
            "keep_height?: boolean}): Entity",
            "Create an entity with the given components. Without a `y` in `position` (or with keep_height) "
            "it stands on the grid; `yaw` is in degrees.")
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
            [](sol::this_state s, const WorldRef& w, sol::object options) {
                sol::state_view lua(s);
                std::string tag;
                std::optional<glm::vec3> centre;
                float radius = 0;
                std::vector<std::string> fields;
                if (const auto filter = lua::options(options, "query filter")) {
                    tag = lua::get<std::string>(*filter, "tag", "");
                    if (const auto near = lua::field(*filter, "near"); !lua::absent(near)) {
                        centre = lua::position(near);
                        radius = lua::atLeast(lua::get(*filter, "radius", 1.0f), 0.0f, "radius");
                    }
                    fields = lua::strings(lua::field(*filter, "fields"));
                }
                auto& world = w.get();
                const auto found = world.query(tag, centre, radius);
                auto out = lua.create_table();
                if (fields.empty()) {
                    for (const auto entity : found) {
                        out.add(EntityRef{w.world, entity, w.engine});
                    }
                    return out;
                }
                // Struct of arrays: one table per field, each with n entries.
                out["n"] = static_cast<lua_Integer>(found.size());
                const auto& registry = world.registry();
                for (const auto& name : fields) {
                    auto column = lua.create_table(static_cast<int>(found.size()), 0);
                    lua_Integer i = 0;
                    if (name == "entity") {
                        for (const auto entity : found) {
                            column.raw_set(++i, EntityRef{w.world, entity, w.engine});
                        }
                    } else if (name == "id") {
                        for (const auto entity : found) {
                            const auto* named = registry.try_get<Name>(entity);
                            column.raw_set(++i, named ? named->id : std::string());
                        }
                    } else if (name == "x" || name == "y" || name == "z" || name == "layer" ||
                               name == "cell_x" || name == "cell_z") {
                        for (const auto entity : found) {
                            const auto& t = registry.get<Transform>(entity);
                            if (name == "x") {
                                column.raw_set(++i, t.position.x);
                            } else if (name == "y") {
                                column.raw_set(++i, t.position.y);
                            } else if (name == "z") {
                                column.raw_set(++i, t.position.z);
                            } else if (name == "layer") {
                                column.raw_set(++i, t.layer);
                            } else if (name == "cell_x") {
                                column.raw_set(++i, floorCell(t.position.x));
                            } else {
                                column.raw_set(++i, floorCell(t.position.z));
                            }
                        }
                    } else {
                        throw std::runtime_error("Unknown query field '" + name +
                                                 "' (expected entity, x, y, z, layer, id, cell_x or cell_z)");
                    }
                    out[name] = column;
                }
                return out;
            },
            "fun(filter?: {tag?: string, near?: Vec3, radius?: number, fields?: string[]}): Entity[]|{n: "
            "integer, entity?: Entity[], x?: number[], y?: number[], z?: number[], layer?: integer[], id?: "
            "string[], cell_x?: integer[], cell_z?: integer[]}",
            "Entities with a tag and/or within a radius (XZ distance from `near`). With `fields` the result "
            "is a struct of arrays instead of a list of handles: `n` entities and one array per requested "
            "field (`entity` handles, `x`, `y`, `z` positions, `layer`, `id`, `cell_x`, `cell_z`), so a HUD "
            "or a game system reads two hundred positions a tick with a handful of tables rather than a "
            "handle and a table per entity.")
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
                    if (event.kind == WorldEvent::Kind::CellEnter) {
                        t["cell"] = lua::table(lua, event.cell);
                    }
                    out.add(t);
                }
                return out;
            },
            "fun(): {type: string, a?: Entity, b?: Entity, name?: string, cell?: Cell}[]",
            "Events since the last call: contact_begin/end, arrived, blocked, jump_start, landed, "
            "climb_start, climb_end, anim_event, "
            "anim_finished, zone_enter/exit (entities with a mover or collider) and cell_enter (a mover "
            "entered a new grid cell; `cell` is that cell). Events naming an entity "
            "destroyed before they are read are dropped. At most 4096 are kept; older ones are dropped "
            "with a warning when nobody reads them.")
        .method(
            "draw",
            [](const WorldRef& w, sol::object options) {
                w.engine->queueDraw(worldDraw(*w.engine, w.lock(), options));
            },
            "fun(options?: {rect?: Rect, target?: string, camera?: {target?: Vec3, yaw?: number, "
            "pitch?: number, distance?: number, fov?: number, orthographic?: boolean, ortho_height?: "
            "number, shake?: Vec3}, post?: table})",
            "Render this world this frame (call from draw; several worlds draw in call order). `rect` = "
            "{x, y, w, h} in canvas pixels draws it into that part of the screen (picture-in-picture). "
            "`target` draws it off-screen instead, at the rect's size (default: the whole canvas; x and y "
            "are "
            "unused), for fe.canvas.image('rt:<target>') to show (minimaps, 3D portraits). `camera` replaces "
            "the world's camera for this draw only (`orthographic` and `ortho_height` as on Camera); unset "
            "fields copy the world camera, and it does not shake unless `shake` is given. Like the world "
            "camera it is interpolated between the values of "
            "the last two ticks (per world, target and order of such draws), so pass tick values such as "
            "entity.position. `post` overrides environment post fields for this draw only (a minimap "
            "without depth of field or bloom: post = {depth_of_field = false, bloom = false}).")
        .method(
            "camera", [](const WorldRef& w) { return CameraRef{w.lock(), w.engine}; }, "fun(): Camera",
            "The world's camera.");
}
} // namespace

void bindWorld(Engine& engine) {
    auto& host = engine.lua();
    bindEntity(engine);
    bindCamera(engine);
    TypeBinder<WorldRef> type(host, engine.docs(), "World",
                              "A loaded map: grid, entities, camera, lights and environment.");
    type.op(
            sol::meta_function::equal_to,
            [](const WorldRef& a, const WorldRef& b) {
                return !a.world.owner_before(b.world) && !b.world.owner_before(a.world);
            },
            "a == b", "True when both handles refer to the same loaded world.")
        .op(
            sol::meta_function::to_string,
            [](const WorldRef& w) {
                const auto world = w.world.lock();
                return world ? "World(" + world->name() + ")" : std::string("World(unloaded)");
            },
            "tostring(w)", "'World(<name>)', or 'World(unloaded)'.");
    bindMapData(type);
    bindEntities(type);
    bindGrid(type);
    bindParticles(type);
    ModuleBinder(host, engine.docs(), "fe.world",
                 "Load and manage worlds (maps). Several can be loaded at once.")
        .fn(
            "load",
            [&engine](const std::string& name, const std::string& mapId) {
                return WorldRef{engine.loadWorld(name, mapId), &engine};
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
                for (const auto& entry : engine.worlds()) {
                    names.push_back(entry.first);
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
            [](const CameraRef& c, sol::object v) { c.get().target = lua::position(v); }, "Vec3",
            "Point the camera looks at. Reading returns a copy, so `camera.target.x = 5` changes nothing: "
            "assign a whole table (`camera.target = {x = 5, y = 0, z = 3}`).")
        .property(
            "yaw", [](const CameraRef& c) { return c.get().yaw; },
            [](const CameraRef& c, sol::object v) { c.get().yaw = lua::as<float>(v, "yaw"); }, "number",
            "Degrees around the vertical axis.")
        .property(
            "pitch", [](const CameraRef& c) { return c.get().pitch; },
            [](const CameraRef& c, sol::object v) {
                c.get().pitch = std::clamp(lua::as<float>(v, "pitch"), -89.0f, 89.9f);
            },
            "number", "Degrees above the horizon, held within -89 to 89.9 (89.9 is top-down).")
        .property(
            "distance", [](const CameraRef& c) { return c.get().distance; },
            [](const CameraRef& c, sol::object v) {
                c.get().distance = lua::within(lua::as<float>(v, "distance"), 0.1f, 10000.0f, "distance");
            },
            "number", "Distance from the target (0.1 to 10000).")
        .property(
            "fov", [](const CameraRef& c) { return c.get().fov; },
            [](const CameraRef& c, sol::object v) {
                c.get().fov = lua::within(lua::as<float>(v, "fov"), 1.0f, 170.0f, "fov");
            },
            "number", "Vertical field of view in degrees (1 to 170).")
        .property(
            "near", [](const CameraRef& c) { return c.get().nearPlane; },
            [](const CameraRef& c, sol::object v) {
                auto& camera = c.get();
                const float near = lua::within(lua::as<float>(v, "near"), 0.001f, 100.0f, "near");
                if (near >= camera.farPlane) {
                    throw std::runtime_error("near must be below far (" + lua::format(camera.farPlane) + ")");
                }
                camera.nearPlane = near;
            },
            "number",
            "Near clip plane in world units (0.001 to 100, below `far`; default 0.1). Depth precision falls "
            "as far/near grows, so raise `near` before stretching `far` a long way.")
        .property(
            "far", [](const CameraRef& c) { return c.get().farPlane; },
            [](const CameraRef& c, sol::object v) {
                auto& camera = c.get();
                const float far = lua::within(lua::as<float>(v, "far"), 1.0f, 100000.0f, "far");
                if (far <= camera.nearPlane) {
                    throw std::runtime_error("far must be above near (" + lua::format(camera.nearPlane) +
                                             ")");
                }
                camera.farPlane = far;
            },
            "number",
            "Far clip plane in world units (1 to 100000, above `near`; default 300). Raise it before a "
            "`distance` that would clip an overworld (an airship camera at 400); fog and depth-of-field "
            "fall off relative to the planes, so retune them when the planes move a long way.")
        .property(
            "orthographic", [](const CameraRef& c) { return c.get().orthographic; },
            [](const CameraRef& c, sol::object v) {
                c.get().orthographic = lua::as<bool>(v, "orthographic");
            },
            "boolean",
            "Parallel projection from the same orbit position (pixel-exact 2D views; default false). The "
            "view covers `ortho_height` world units vertically; distance then only sets the near and far "
            "planes. Shadows, picking, `project` and `ray` follow it.")
        .property(
            "ortho_height", [](const CameraRef& c) { return c.get().effectiveOrthoHeight(); },
            [](const CameraRef& c, sol::object v) {
                const auto height = lua::as<float>(v, "ortho_height");
                c.get().orthoHeight =
                    height <= 0 ? 0.0f : lua::within(height, 0.01f, 100000.0f, "ortho_height");
            },
            "number",
            "World units visible vertically when orthographic (0.01 to 100000). 0 (the default) derives it "
            "from distance and fov, the height a perspective view covers at the target, so toggling "
            "`orthographic` keeps the framing; reading returns the height in effect.")
        .property(
            "shake", [](const CameraRef& c) { return lua::table(c.engine->lua().lua(), c.get().shake); },
            [](const CameraRef& c, sol::object v) { c.get().shake = lua::vec3(v); }, "Vec3",
            "Visual offset (screen shake), not interpolated. Reading returns a copy; assign a whole table.")
        .method(
            "snap", [](const CameraRef& c) { c.get().snap(); }, "fun()", "Skip interpolation after a cut.")
        .method(
            "position",
            [](sol::this_state s, const CameraRef& c, sol::object alpha) {
                return lua::table(
                    s, c.get().position(lua::opt<double>(alpha, "alpha").value_or(c.engine->drawAlpha())));
            },
            "fun(alpha?: number): Vec3",
            "Eye position, interpolated at `alpha` (inside draw it defaults to the frame's alpha, otherwise "
            "1).")
        .method(
            "project",
            [](const CameraRef& c, sol::object point, sol::object alpha) {
                return projectToCanvas(*c.engine, c.get(), lua::position(point),
                                       lua::opt<double>(alpha, "alpha").value_or(c.engine->drawAlpha()));
            },
            "fun(point: Vec3, alpha?: number): number, number, boolean",
            "Canvas position of a world point; false when behind the camera. Inside draw it matches the "
            "rendered frame; pass `alpha` to choose another interpolation.")
        .method(
            "ray",
            [](sol::this_state s, const CameraRef& c, float x, float y, sol::object alpha) {
                glm::vec3 origin, direction;
                c.get().ray(canvasToNdc(*c.engine, x, y), aspect(*c.engine), origin, direction,
                            lua::opt<double>(alpha, "alpha").value_or(c.engine->drawAlpha()));
                return std::make_tuple(lua::table(s, origin), lua::table(s, direction));
            },
            "fun(x: number, y: number, alpha?: number): Vec3, Vec3",
            "World ray through a canvas position (camera interpolated at `alpha`, default 1).");
}
} // namespace fe
