#include "engine/editor/MapEdits.h"
#include "engine/assets/Model.h"
#include "engine/world/World.h"
#include <algorithm>
#include <cmath>

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
    const bool continues = !step && found != stacks_.end() && found->second.world == &world &&
                           !found->second.undo.empty();
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
        return snapshot.at(section) != current.at(section);
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
            if (!grid.contains(p)) {
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
        if (!grid.contains(link.foot()) || !grid.contains(link.top())) {
            return "Both ends of a ladder must lie inside the grid";
        }
        return {};
    }
    if (!grid.contains(link.x, link.z, link.from) || !grid.contains(link.x, link.z, link.to)) {
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
            if (grid.contains(x, z, layer)) {
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
        if (props.contains("collision") && marker.hasBounds) {
            const auto config = props.at("collision").is_object() ? props.at("collision") : Json::object();
            const auto layer = config.value("layer", 0);
            if (layer < 0 || layer >= grid.layerCount()) {
                problems.push_back(marker.name + ": collision layer " + std::to_string(layer) +
                                   " does not exist");
            } else {
                const bool walkable = config.value("walkable", false);
                const float height = config.value("height", marker.max.y);
                for (int z = std::max(0, floorCell(marker.min.z));
                     z < std::min(grid.depth(), ceilCell(marker.max.z)); ++z) {
                    for (int x = std::max(0, floorCell(marker.min.x));
                         x < std::min(grid.width(), ceilCell(marker.max.x)); ++x) {
                        auto& cell = grid.at(x, z, layer);
                        cell.walkable = walkable;
                        if (walkable) {
                            cell.height = height;
                        }
                    }
                }
                ++imported;
            }
        }
        if (props.contains("spawn")) {
            const auto name =
                props.at("spawn").is_string() ? props.at("spawn").get<std::string>() : marker.name;
            const int x = floorCell(marker.position.x);
            const int z = floorCell(marker.position.z);
            if (!grid.contains(x, z, 0)) {
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
