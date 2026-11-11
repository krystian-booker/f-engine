#include "engine/editor/MapEdits.h"
#include "engine/assets/Model.h"
#include "engine/world/World.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fe {
void applyEdit(World& world, const std::function<void(MapData&)>& edit) {
    edit(world.authored());
    edit(world.map());
    world.rebuildStatic();
}

void editCollider(World& world, entt::entity entity, const Collider& collider) {
    world.registry().replace<Collider>(entity, collider);
}

void editTransform(World& world, entt::entity entity, glm::vec3 position, int layer) {
    auto& transform = world.registry().get<Transform>(entity);
    transform.position = position;
    transform.previous = position; // no interpolation from where it was: a paused game shows the move
    transform.layer = layer;
    // The hash is no longer rebuilt per tick: a transform written directly
    // must be re-indexed, so queries (and a paused game) see it at once.
    world.refreshCollision();
}

std::string MapEditHistory::key(const World& world) {
    return world.name() + "\n" + world.authored().id;
}

void MapEditHistory::sync(const World& world) {
    const auto found = stacks_.find(key(world));
    if (found != stacks_.end() && found->second.head != world.authored().toJson()) {
        stacks_.erase(found);
    }
}

MapEditHistory::Stacks& MapEditHistory::stacksFor(World& world, bool check) {
    if (check) {
        sync(world);
    }
    auto& stacks = stacks_[key(world)];
    stacks.world = &world;
    return stacks;
}

void MapEditHistory::apply(World& world, const std::function<void(MapData&)>& edit, bool step) {
    // A gesture continues only on the world object it started on; any other
    // world starts a step, which checks the map the history left.
    const auto found = stacks_.find(key(world));
    const bool continues =
        !step && found != stacks_.end() && found->second.world == &world && !found->second.undo.empty();
    auto& stacks = stacksFor(world, !continues);
    if (!continues) {
        stacks.undo.push_back(world.authored().toJson());
        if (stacks.undo.size() > Limit) {
            stacks.undo.pop_front();
        }
        stacks.redo.clear();
    }
    applyEdit(world, edit);
    if (continues) {
        stacks.headStale = true;
    } else {
        stacks.head = world.authored().toJson();
        stacks.headStale = false;
    }
}

void MapEditHistory::settle(const World& world) {
    const auto found = stacks_.find(key(world));
    if (found != stacks_.end() && found->second.headStale && found->second.world == &world) {
        found->second.head = world.authored().toJson();
        found->second.headStale = false;
    }
}

void MapEditHistory::restore(World& world, const Json& snapshot) {
    auto restored = MapData::fromJson(snapshot);
    const auto current = world.authored().toJson();
    auto changed = [&](const char* section) {
        return snapshot.value(section, Json()) != current.value(section, Json());
    };
    auto& live = world.map();
    if (changed("name")) {
        live.name = restored.name;
    }
    if (changed("geometry")) {
        live.geometry = restored.geometry;
    }
    if (changed("grid")) {
        live.grid = restored.grid;
    }
    if (changed("points")) {
        live.points = restored.points;
    }
    if (changed("placements")) {
        live.placements = restored.placements;
    }
    if (changed("zones")) {
        live.zones = restored.zones;
    }
    if (changed("lights")) {
        live.lights = restored.lights;
    }
    if (changed("emitters")) {
        live.emitters = restored.emitters;
    }
    if (changed("colliders")) {
        live.colliders = restored.colliders;
    }
    if (changed("bake")) {
        live.bake = restored.bake;
    }
    if (changed("environment")) {
        live.environment = restored.environment;
    }
    if (changed("props")) {
        live.props = restored.props;
    }
    world.authored() = std::move(restored);
    world.rebuildStatic();
}

bool MapEditHistory::undo(World& world) {
    settle(world);
    sync(world);
    const auto found = stacks_.find(key(world));
    if (found == stacks_.end() || found->second.undo.empty()) {
        return false;
    }
    auto& stacks = found->second;
    stacks.world = &world;
    auto current = world.authored().toJson();
    restore(world, stacks.undo.back());
    stacks.head = world.authored().toJson();
    stacks.headStale = false;
    stacks.undo.pop_back();
    stacks.redo.push_back(std::move(current));
    return true;
}

bool MapEditHistory::redo(World& world) {
    settle(world);
    sync(world);
    const auto found = stacks_.find(key(world));
    if (found == stacks_.end() || found->second.redo.empty()) {
        return false;
    }
    auto& stacks = found->second;
    stacks.world = &world;
    auto current = world.authored().toJson();
    restore(world, stacks.redo.back());
    stacks.head = world.authored().toJson();
    stacks.headStale = false;
    stacks.redo.pop_back();
    stacks.undo.push_back(std::move(current));
    return true;
}

void MapEditHistory::markSaved(World& world) {
    auto& stacks = stacksFor(world, false);
    stacks.saved = world.authored().toJson();
    if (stacks.undo.empty() && stacks.redo.empty()) {
        stacks.head = *stacks.saved; // a fresh stack: sync() compares against it
        stacks.headStale = false;
    }
}

bool MapEditHistory::unsaved(const World& world) const {
    const auto found = stacks_.find(key(world));
    if (found == stacks_.end()) {
        return false;
    }
    const auto& stacks = found->second;
    if (!stacks.saved) {
        return !stacks.undo.empty();
    }
    return stacks.headStale || stacks.head != *stacks.saved;
}

std::size_t MapEditHistory::undoCount(const World& world) const {
    const auto found = stacks_.find(key(world));
    return found == stacks_.end() ? 0 : found->second.undo.size();
}

std::size_t MapEditHistory::redoCount(const World& world) const {
    const auto found = stacks_.find(key(world));
    return found == stacks_.end() ? 0 : found->second.redo.size();
}

std::string brushProblem(const Grid& grid, const GridBrush& brush, int layer) {
    if (layer < 0 || layer >= grid.layerCount()) {
        return "Layer " + std::to_string(layer) + " does not exist";
    }
    const bool tagging = brush.mode == GridBrush::Mode::AddTag || brush.mode == GridBrush::Mode::RemoveTag;
    if (tagging && brush.tag.empty()) {
        return "Enter a tag name to paint";
    }
    if (brush.mode == GridBrush::Mode::AddTag && grid.tagMask({brush.tag}) == 0 &&
        grid.tagNames().size() >= 32) {
        return "The grid already uses 32 tags";
    }
    return {};
}

int paint(Grid& grid, const GridBrush& brush, GridPos centre, int radius) {
    if (!brushProblem(grid, brush, centre.layer).empty()) {
        return 0;
    }
    const bool tagging = brush.mode == GridBrush::Mode::AddTag || brush.mode == GridBrush::Mode::RemoveTag;
    const auto bit = tagging ? grid.tagBit(brush.tag, brush.mode == GridBrush::Mode::AddTag) : 0u;
    int changed = 0;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const GridPos p{centre.x + dx, centre.z + dz, centre.layer};
            if (!grid.inRange(p)) {
                continue;
            }
            auto& cell = grid.at(p.x, p.z, p.layer);
            const auto before = cell;
            switch (brush.mode) {
            case GridBrush::Mode::Walkable:
                cell.walkable = brush.walkable;
                break;
            case GridBrush::Mode::Height:
                cell.height = brush.height;
                break;
            case GridBrush::Mode::Ramp:
                cell.ramp = brush.ramp;
                break;
            case GridBrush::Mode::AddTag:
                cell.tags |= bit;
                break;
            case GridBrush::Mode::RemoveTag:
                cell.tags &= ~bit;
                break;
            }
            const bool same = before.walkable == cell.walkable && before.height == cell.height &&
                              before.ramp == cell.ramp && before.tags == cell.tags;
            changed += same ? 0 : 1;
        }
    }
    return changed;
}

int fillRect(Grid& grid, const GridBrush& brush, GridPos a, GridPos b) {
    if (!brushProblem(grid, brush, a.layer).empty()) {
        return 0;
    }
    const int x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), z0 = std::min(a.z, b.z),
              z1 = std::max(a.z, b.z);
    int changed = 0;
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            changed += paint(grid, brush, {x, z, a.layer}, 0);
        }
    }
    return changed;
}

int floodFill(Grid& grid, const GridBrush& brush, GridPos start) {
    if (!grid.inRange(start) || brush.mode == GridBrush::Mode::Ramp ||
        !brushProblem(grid, brush, start.layer).empty()) {
        return 0;
    }
    const bool seed = grid.at(start).walkable;
    std::vector<GridPos> stack{start};
    std::vector<char> seen(static_cast<std::size_t>(grid.width() * grid.depth()), 0);
    std::vector<GridPos> region;
    while (!stack.empty()) {
        const auto p = stack.back();
        stack.pop_back();
        if (!grid.inRange(p)) {
            continue;
        }
        auto& mark = seen[static_cast<std::size_t>(p.z * grid.width() + p.x)];
        if (mark || grid.at(p).walkable != seed) {
            continue;
        }
        mark = 1;
        region.push_back(p);
        stack.push_back({p.x + 1, p.z, p.layer});
        stack.push_back({p.x - 1, p.z, p.layer});
        stack.push_back({p.x, p.z + 1, p.layer});
        stack.push_back({p.x, p.z - 1, p.layer});
    }
    int changed = 0;
    for (const auto& p : region) {
        changed += paint(grid, brush, p, 0);
    }
    return changed;
}

namespace {
bool near(float a, float b) {
    return std::abs(a - b) < 1e-4f;
}
// Joins two boxes when they share every extent but one axis, along which
// they touch or overlap.
bool tryMerge(const Box& a, const Box& b, Box& out) {
    for (int axis = 0; axis < 3; ++axis) {
        bool same = true;
        for (int other = 0; other < 3; ++other) {
            if (other != axis && (!near(a.min[other], b.min[other]) || !near(a.max[other], b.max[other]))) {
                same = false;
            }
        }
        if (!same) {
            continue;
        }
        if (a.max[axis] >= b.min[axis] - 1e-4f && b.max[axis] >= a.min[axis] - 1e-4f) {
            out = a;
            out.min[axis] = std::min(a.min[axis], b.min[axis]);
            out.max[axis] = std::max(a.max[axis], b.max[axis]);
            return true;
        }
    }
    return false;
}
} // namespace

std::vector<Box> mergeBoxes(std::vector<Box> boxes) {
    for (auto& box : boxes) {
        const auto lo = glm::min(box.min, box.max), hi = glm::max(box.min, box.max);
        box.min = lo;
        box.max = hi;
    }
    bool merged = true;
    while (merged) {
        merged = false;
        for (std::size_t i = 0; i < boxes.size() && !merged; ++i) {
            for (std::size_t j = i + 1; j < boxes.size(); ++j) {
                Box joined;
                if (tryMerge(boxes[i], boxes[j], joined)) {
                    boxes[i] = joined;
                    boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
    }
    return boxes;
}

std::vector<Box> cutBox(const Box& box, glm::vec2 lo, glm::vec2 hi) {
    std::vector<Box> pieces;
    if (!(box.min.x < hi.x - 1e-4f && box.max.x > lo.x + 1e-4f && box.min.z < hi.y - 1e-4f &&
          box.max.z > lo.y + 1e-4f)) {
        pieces.push_back(box); // beside the rectangle: whole
        return pieces;
    }
    // Left and right of the rectangle (full depth), then front and back (within its x span).
    if (box.min.x < lo.x - 1e-4f) {
        pieces.push_back({box.min, {lo.x, box.max.y, box.max.z}});
    }
    if (box.max.x > hi.x + 1e-4f) {
        pieces.push_back({{hi.x, box.min.y, box.min.z}, box.max});
    }
    const float x0 = std::max(box.min.x, lo.x), x1 = std::min(box.max.x, hi.x);
    if (box.min.z < lo.y - 1e-4f) {
        pieces.push_back({{x0, box.min.y, box.min.z}, {x1, box.max.y, lo.y}});
    }
    if (box.max.z > hi.y + 1e-4f) {
        pieces.push_back({{x0, box.min.y, hi.y}, {x1, box.max.y, box.max.z}});
    }
    return pieces;
}

Box boxForModel(const Model& model, const Placement& placement, float margin) {
    const float yaw = glm::radians(placement.yaw);
    const float c = std::cos(yaw), s = std::sin(yaw);
    Box box{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())};
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner =
            glm::vec3((i & 1) ? model.max.x : model.min.x, (i & 2) ? model.max.y : model.min.y,
                      (i & 4) ? model.max.z : model.min.z) *
            placement.scale;
        // glm::rotate about +y: x' = x cos + z sin, z' = -x sin + z cos.
        const glm::vec3 turned(corner.x * c + corner.z * s, corner.y, -corner.x * s + corner.z * c);
        const auto p = placement.position + turned;
        box.min = glm::min(box.min, p);
        box.max = glm::max(box.max, p);
    }
    const float shrink =
        std::min(margin, (std::min(box.max.x - box.min.x, box.max.z - box.min.z) - BlockStep) * 0.5f);
    if (shrink > 0) {
        box.min.x += shrink;
        box.max.x -= shrink;
        box.min.z += shrink;
        box.max.z -= shrink;
    }
    return box;
}

namespace {
template<class Settings> std::string roundTripProblem(const Settings& settings) {
    try {
        Settings::fromJson(settings.toJson());
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}
} // namespace

std::string editProblem(const LightSettings& light) {
    return roundTripProblem(light);
}

std::string editProblem(const EmitterSettings& emitter) {
    return roundTripProblem(emitter);
}

std::string editProblem(const Environment& environment) {
    return roundTripProblem(environment);
}

std::string editProblem(const Grid& grid, const GridLink& link) {
    if (link.kind == LinkKind::Ladder) {
        if (link.direction == glm::ivec2(0)) {
            return "A ladder needs a direction (the way up)";
        }
        if (!grid.inRange(link.foot()) || !grid.inRange(link.top())) {
            return "Both ends of a ladder must lie inside the grid";
        }
        return {};
    }
    if (!grid.inRange(link.x, link.z, link.from) || !grid.inRange(link.x, link.z, link.to)) {
        return "A link must lie inside the grid on existing layers";
    }
    if (link.from == link.to) {
        return "A step link joins two different layers";
    }
    return {};
}

std::string editProblem(const std::vector<Zone>& zones, const Zone& zone, std::size_t index) {
    if (zone.id.empty()) {
        return "Zone ids must be unique and not empty";
    }
    for (std::size_t i = 0; i < zones.size(); ++i) {
        if (i != index && zones[i].id == zone.id) {
            return "Zone ids must be unique and not empty";
        }
    }
    if (zone.shape == ZoneShape::Polygon &&
        (zone.points.size() < 3 || zone.points.size() > Zone::MaxPoints)) {
        return "A polygon zone needs 3 to 64 corners";
    }
    return {};
}

void addFlickerKey(LightSettings& light) {
    const auto time = light.flickerCurve.empty() ? 0.0f : light.flickerCurve.back().x + 0.5f;
    light.flickerCurve.emplace_back(time, 1.0f);
}

Environment withEnvironmentJson(const Environment& environment, const std::string& pointer,
                                const Json& value) {
    auto json = environment.toJson();
    json[Json::json_pointer(pointer)] = value;
    return Environment::fromJson(json);
}

std::vector<GridPos> zoneCells(const Zone& zone, const Grid& grid, int fallbackLayer) {
    const int layer = zone.layer >= 0 ? zone.layer : fallbackLayer;
    std::vector<GridPos> cells;
    for (int z = std::max(0, floorCell(zone.min.y)); z < std::min(grid.depth(), ceilCell(zone.max.y)); ++z) {
        for (int x = std::max(0, floorCell(zone.min.x)); x < std::min(grid.width(), ceilCell(zone.max.x));
             ++x) {
            const glm::vec3 centre(static_cast<float>(x) + 0.5f, 0, static_cast<float>(z) + 0.5f);
            if (grid.inRange(x, z, layer) && zone.contains(centre, zone.layer < 0 ? -1 : layer)) {
                cells.push_back({x, z, layer});
            }
        }
    }
    return cells;
}

int importMarkers(MapData& map, const Model& model, std::vector<std::string>& problems) {
    auto& grid = map.grid;
    int imported = 0;
    for (const auto& marker : model.markers) {
        const auto& props = marker.properties;
        // `collision` nodes are the walkability bake's business (WalkBake.h), not a marker.
        if (props.contains("spawn")) {
            const auto name =
                props.at("spawn").is_string() ? props.at("spawn").get<std::string>() : marker.name;
            const int x = floorCell(marker.position.x);
            const int z = floorCell(marker.position.z);
            if (!grid.inRange(x, z, 0)) {
                problems.push_back(marker.name + ": spawn point lies outside the grid");
            } else {
                map.points[name] =
                    MapPoint{{marker.position.x, grid.heightAt({marker.position.x, marker.position.z}),
                              marker.position.z},
                             {0, 1},
                             0};
                ++imported;
            }
        }
        if (props.contains("light") && props.at("light").is_object() &&
            props.at("light").value("type", std::string("point")) != "directional") {
            auto light = LightSettings::fromJson(props.at("light"));
            light.position = marker.position;
            map.lights.push_back(light);
            ++imported;
        }
    }
    return imported;
}

namespace editorui {
namespace {
const Json* typed(const Json& object, const char* key, bool (Json::*check)() const) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(key);
    return found != object.end() && (*found.*check)() ? &*found : nullptr;
}
} // namespace

std::string jsonString(const Json& object, const char* key, std::string fallback) {
    const auto* value = typed(object, key, &Json::is_string);
    return value ? value->get<std::string>() : fallback;
}
double jsonNumber(const Json& object, const char* key, double fallback) {
    const auto* value = typed(object, key, &Json::is_number);
    return value ? value->get<double>() : fallback;
}
bool jsonBool(const Json& object, const char* key, bool fallback) {
    const auto* value = typed(object, key, &Json::is_boolean);
    return value ? value->get<bool>() : fallback;
}
Json jsonObject(const Json& object, const char* key) {
    const auto* value = typed(object, key, &Json::is_object);
    return value ? *value : Json::object();
}
Json jsonArray(const Json& object, const char* key) {
    const auto* value = typed(object, key, &Json::is_array);
    return value ? *value : Json::array();
}
} // namespace editorui
} // namespace fe
