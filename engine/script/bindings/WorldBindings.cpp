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
    WorldDraw draw{world, {}, {}, {}, {}};
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
    if (!draw.target.empty()) {
        const auto& canvas = engine.canvas();
        const glm::vec2 size =
            draw.rect ? glm::vec2(draw.rect->w, draw.rect->h) : glm::vec2(canvas.width(), canvas.height());
        engine.canvas().defineTarget(draw.target, glm::ivec2(glm::round(size)));
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
        camera.shake = lua::vec3(lua::field(c, "shake"), glm::vec3(0));
        camera.snap();
        draw.camera = camera;
    }
    if (const auto post = lua::options(lua::field(t, "post"), "Field 'post'")) {
        auto environment = world->environment().toJson();
        environment.merge_patch(Json{{"post", luaToJson(*post)}});
        draw.post = Environment::fromJson(environment).post;
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
    }
    return "unknown";
}

StepRules stepRules(LuaHost& host, World& world, const sol::object& value) {
    StepRules rules;
    const auto options = lua::options(value, "rules");
    if (!options) {
        return rules;
    }
    const auto& t = *options;
    rules.diagonal = lua::get(t, "diagonal", rules.diagonal);
    rules.ladders = lua::get(t, "ladders", rules.ladders);
    const float step = lua::get(t, "max_step", rules.maxUp);
    rules.maxUp = lua::get(t, "max_up", step);
    rules.maxDown = lua::get(t, "max_down", step);
    if (rules.maxUp < 0 || rules.maxDown < 0) {
        throw std::runtime_error("max_step, max_up and max_down must be non-negative numbers");
    }
    rules.blockedTags = world.grid().tagMask(lua::strings(lua::field(t, "blocked_tags")));
    if (const auto costs = lua::options(lua::field(t, "tag_costs"), "Field 'tag_costs'")) {
        for (const auto& [key, value] : *costs) {
            if (key.get_type() != sol::type::string) {
                throw std::runtime_error("tag_costs keys must be tag names, got " + lua::describe(key));
            }
            const auto tag = key.as<std::string>();
            const auto cost = lua::convert<float>(value).value_or(-1.0f);
            if (cost < 0) {
                throw std::runtime_error("tag_costs['" + tag + "'] must be a non-negative number");
            }
            if (const auto mask = world.grid().tagMask({tag})) {
                rules.tagCosts.emplace_back(mask, cost);
            }
        }
        // Deterministic order whatever the Lua table iteration order was.
        std::sort(rules.tagCosts.begin(), rules.tagCosts.end());
    }
    for (const char* key : {"blocked", "no_stop", "zoc"}) {
        const auto cells = lua::field(t, key);
        if (lua::absent(cells)) {
            continue;
        }
        const std::string name = key;
        auto& set = name == "blocked" ? rules.blocked : name == "no_stop" ? rules.noStop : rules.zoc;
        for (const auto& cell : lua::cells(cells, key)) {
            set.insert(cellKey(cell));
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
    return rules;
}

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

// Overlays are drawn every frame, so their size is bounded.
constexpr std::size_t MaxOverlays = 256;
constexpr std::size_t MaxOverlayCells = 65536;
std::vector<GridPos> overlayCells(const sol::object& value, const char* what) {
    if (lua::length(lua::tableArg(value, what)) > MaxOverlayCells) {
        throw std::runtime_error(std::string(what) + ": at most 65536 cells");
    }
    return lua::cells(value, what);
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
            "fun(tag: string): Cell[]", "All cells carrying a tag.")
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
            "A* path of cells (excluding the start). Rules: diagonal, max_step, max_up, max_down, "
            "blocked_tags, tag_costs (non-negative extra cost per tag), blocked, no_stop, zoc (cells that "
            "end movement), jump = {distance, max_up?, max_down?, diagonal?} (gap jumps along the axes, and "
            "with diagonal = true the diagonals, over up to `distance` unwalkable, blocked-tag or sunken "
            "cells), ladders (default true; false never climbs ladder links), cost = fun(from, to, base): "
            "number|false (per step; false = impassable; the two cell tables are reused between calls). "
            "Layer changes follow links and ladders the way movers do; steps that jump or climb a ladder are "
            "marked `jump`/`climb`. The search expands at most 200000 cells; when it stops there before "
            "reaching the goal the list has `truncated = true` and is empty or a route that may not be the "
            "cheapest.")
        .method(
            "reachable",
            [](sol::this_state s, const WorldRef& w, sol::object start, sol::object budget,
               sol::object rules) {
                sol::state_view lua(s);
                const auto world = w.lock();
                const auto from = lua::cell(start);
                const auto limit = lua::atLeast(lua::as<float>(budget, "budget"), 0.0f, "budget");
                const auto parsed = stepRules(w.engine->lua(), *world, rules);
                const auto cells = reachable(world->grid(), from, limit, parsed);
                auto out = lua.create_table(static_cast<int>(cells.size()), 0);
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    const auto& c = cells[i];
                    auto entry = lua.create_table_with("x", c.pos.x, "z", c.pos.z, "layer", c.pos.layer,
                                                       "cost", c.cost, "stop", c.stoppable);
                    if (c.parent >= 0) {
                        entry["parent"] = c.parent + 1;
                    }
                    markStep(entry, c.kind);
                    out[i + 1] = entry;
                }
                return out;
            },
            "fun(start: Cell, budget: number, rules?: table): {x: integer, z: integer, layer: integer, "
            "cost: number, parent?: integer, stop: boolean, jump?: true, climb?: true}[]",
            "Every cell reachable within a movement budget (tactical move ranges), with the rules of "
            "world:path. The first entry is the start cell, which has no `parent`; every other entry's "
            "`parent` is the index in this list of the cell it is reached from, so `while c.parent do c = "
            "list[c.parent] end` walks back to the start. `jump`/`climb` mark how the step from the parent "
            "enters the cell. Zone-of-control cells are reachable but nothing is reached through them.")
        .method(
            "line_of_sight",
            [](const WorldRef& w, sol::object from, sol::object to, sol::object layer, sol::object blocking) {
                auto& grid = w.get().grid();
                return lineOfSight(grid, lua::position(from), lua::position(to),
                                   lua::opt<int>(layer, "layer").value_or(0),
                                   grid.tagMask(lua::strings(blocking)));
            },
            "fun(from: Vec3, to: Vec3, layer?: integer, blocking_tags?: string[]): boolean",
            "Whether terrain blocks the straight line between two points: ground cells, walkable upper-layer "
            "surfaces it passes through and cells with a blocking tag. `layer` is the shooter's layer; its "
            "own "
            "cell is ignored.")
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
            "pick",
            [](sol::this_state s, const WorldRef& w, float x, float y,
               sol::object alpha) -> std::tuple<sol::object, sol::object> {
                auto& world = w.get();
                glm::vec3 origin, direction;
                world.camera().ray(canvasToNdc(*w.engine, x, y), aspect(*w.engine), origin, direction,
                                   lua::opt<double>(alpha, "alpha").value_or(w.engine->drawAlpha()));
                const auto hit = raycastGrid(world.grid(), origin, direction, world.camera().farPlane);
                if (!hit) {
                    return {sol::lua_nil, sol::lua_nil};
                }
                return {lua::table(s, hit->cell), lua::table(s, hit->point)};
            },
            "fun(x: number, y: number, alpha?: number): Cell?, Vec3?",
            "Grid cell and point under a canvas position (mouse picking). Cliff faces stop the ray. Pass "
            "draw's `alpha` to pick against the camera as rendered.")
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

void bindOverlays(TypeBinder<WorldRef>& type) {
    type.method(
            "overlay",
            [](const WorldRef& w, const std::string& name, sol::object cells, sol::object settings) {
                const auto options = lua::options(settings, "overlay options");
                auto list = overlayCells(cells, "overlay cells");
                auto& overlays = w.get().overlays();
                if (!overlays.contains(name) && overlays.size() >= MaxOverlays) {
                    throw std::runtime_error("A world shows at most 256 overlays; clear some first");
                }
                auto& layer = overlays[name];
                layer.cells = std::move(list);
                if (options) {
                    layer.color = lua::color(lua::field(*options, "color"), layer.color);
                    layer.pulse = lua::get(*options, "pulse", layer.pulse);
                    layer.order = lua::get(*options, "order", layer.order);
                    layer.border = lua::get(*options, "border", layer.border);
                    layer.visible = lua::get(*options, "visible", true);
                }
            },
            "fun(name: string, cells: Cell[], options?: {color?: Color, pulse?: number, order?: integer, "
            "border?: boolean, visible?: boolean})",
            "Highlight grid cells (move and attack ranges): at most 65536 cells, in at most 256 overlays "
            "per world.")
        .method(
            "clear_overlay",
            [](const WorldRef& w, sol::object overlay) {
                if (const auto name = lua::opt<std::string>(overlay, "name")) {
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
            [](const WorldRef& w, const std::string& name, sol::object cells, sol::object settings) {
                const auto options = lua::options(settings, "overlay_path options");
                auto list = overlayCells(cells, "overlay_path cells");
                auto& paths = w.get().overlayPaths();
                if (!paths.contains(name) && paths.size() >= MaxOverlays) {
                    throw std::runtime_error("A world shows at most 256 overlay paths; clear some first");
                }
                auto& path = paths[name];
                path.cells = std::move(list);
                if (options) {
                    path.color = lua::color(lua::field(*options, "color"), path.color);
                }
            },
            "fun(name: string, cells: Cell[], options?: {color?: Color})",
            "Draw a path arrow through cells (at most 65536, in at most 256 paths per world); the colour is "
            "kept until an options table changes it.")
        .method(
            "cursor",
            [](const WorldRef& w, sol::object cell, sol::object settings) {
                const auto options = lua::options(settings, "cursor options");
                auto& cursor = w.get().cursor();
                if (!lua::absent(cell)) {
                    cursor.cell = lua::cell(cell);
                }
                cursor.visible = !lua::absent(cell);
                if (options) {
                    cursor.color = lua::color(lua::field(*options, "color"), cursor.color);
                }
            },
            "fun(cell: Cell|nil, options?: {color?: Color})",
            "Show the grid cursor on a cell; nil hides it. The colour is kept until an options table changes "
            "it.")
        .method(
            "burst",
            [](const WorldRef& w, sol::object position, sol::table settings, sol::object count) {
                const auto parsed = EmitterSettings::fromJson(lua::emitterJson(settings));
                w.get().burst(
                    lua::position(position), parsed,
                    lua::within(lua::opt<int>(count, "count").value_or(parsed.burst > 0 ? parsed.burst : 12),
                                0, 4096, "count"));
            },
            "fun(position: Vec3, settings: table, count?: integer)", "One-shot particle burst (up to 4096).");
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
                    out.add(lua.create_table_with(
                        "id", p.id, "type", p.type, "position", lua::table(lua, p.position), "facing",
                        lua::tableXZ(lua, p.facing), "layer", p.layer, "props", jsonToLua(lua, p.props)));
                }
                return out;
            },
            "fun(): {id: string, type: string, position: Vec3, facing: {x: number, z: number}, layer: "
            "integer, "
            "props: table}[]",
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
            "fun(): {id: string, type: string, min: {x: number, z: number}, max: {x: number, z: number}, "
            "layer: integer, props: table}[]",
            "Authored zones (camera regions, trigger areas, anything a game defines).")
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
            "time", [](const WorldRef& w) { return w.get().time(); }, "fun(): number", "Simulated seconds.")
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
                if (!keepHeight && !hasHeight) {
                    world.place(entity, position, layer);
                }
                EntityRef ref{w.world, entity, w.engine};
                configureEntity(ref, spec);
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
                if (const auto filter = lua::options(options, "query filter")) {
                    tag = lua::get<std::string>(*filter, "tag", "");
                    if (const auto near = lua::field(*filter, "near"); !lua::absent(near)) {
                        centre = lua::position(near);
                        radius = lua::atLeast(lua::get(*filter, "radius", 1.0f), 0.0f, "radius");
                    }
                }
                auto out = lua.create_table();
                for (const auto entity : w.get().query(tag, centre, radius)) {
                    out.add(EntityRef{w.world, entity, w.engine});
                }
                return out;
            },
            "fun(filter?: {tag?: string, near?: Vec3, radius?: number}): Entity[]",
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
            "Events since the last call: contact_begin/end, arrived, blocked, jump_start, landed, "
            "climb_start, climb_end, anim_event, "
            "anim_finished, zone_enter/exit (entities with a mover or collider). Events naming an entity "
            "destroyed before they are read are dropped. At most 4096 are kept; older ones are dropped "
            "with a warning when nobody reads them.")
        .method(
            "draw",
            [](const WorldRef& w, sol::object options) {
                w.engine->queueDraw(worldDraw(*w.engine, w.lock(), options));
            },
            "fun(options?: {rect?: Rect, target?: string, camera?: {target?: Vec3, yaw?: number, "
            "pitch?: number, distance?: number, fov?: number, shake?: Vec3}, post?: table})",
            "Render this world this frame (call from draw; several worlds draw in call order). `rect` = "
            "{x, y, w, h} in canvas pixels draws it into that part of the screen (picture-in-picture). "
            "`target` draws it off-screen instead, at the rect's size (default: the whole canvas; x and y "
            "are "
            "unused), for fe.canvas.image('rt:<target>') to show (minimaps, 3D portraits). `camera` replaces "
            "the world's camera for this draw only; unset fields copy the world camera, and it does not "
            "shake unless `shake` is given. Like the world camera it is interpolated between the values of "
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
    bindOverlays(type);
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
