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

std::string MapEditHistory::key(const World& world) {
    return world.name() + "\n" + world.authored().id;
}

void MapEditHistory::sync(const World& world) {
    const auto found = stacks_.find(key(world));
    if (found != stacks_.end() && found->second.head != world.authored().toJson()) {
        stacks_.erase(found);
    }
}

void MapEditHistory::apply(World& world, const std::function<void(MapData&)>& edit, bool step) {
    sync(world);
    auto& stacks = stacks_[key(world)];
    if (step || stacks.undo.empty()) {
        stacks.undo.push_back(world.authored().toJson());
        if (stacks.undo.size() > Limit) {
            stacks.undo.pop_front();
        }
        stacks.redo.clear();
    }
    applyEdit(world, edit);
    stacks.head = world.authored().toJson();
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
    sync(world);
    const auto found = stacks_.find(key(world));
    if (found == stacks_.end() || found->second.undo.empty()) {
        return false;
    }
    auto& stacks = found->second;
    auto current = world.authored().toJson();
    restore(world, stacks.undo.back());
    stacks.head = world.authored().toJson();
    stacks.undo.pop_back();
    stacks.redo.push_back(std::move(current));
    return true;
}

bool MapEditHistory::redo(World& world) {
    sync(world);
    const auto found = stacks_.find(key(world));
    if (found == stacks_.end() || found->second.redo.empty()) {
        return false;
    }
    auto& stacks = found->second;
    auto current = world.authored().toJson();
    restore(world, stacks.redo.back());
    stacks.head = world.authored().toJson();
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
} // namespace fe
