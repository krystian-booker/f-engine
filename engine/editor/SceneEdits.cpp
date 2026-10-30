#include "engine/editor/SceneEdits.h"
#include "engine/editor/MapEdits.h"
#include "engine/world/World.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace fe {
GridPos snapToCell(const Grid& grid, glm::vec3 position, int layer) {
    return {std::clamp(floorCell(position.x), 0, std::max(0, grid.width() - 1)),
            std::clamp(floorCell(position.z), 0, std::max(0, grid.depth() - 1)),
            std::clamp(layer, 0, std::max(0, grid.layerCount() - 1))};
}

glm::vec3 cellCentre(const Grid& grid, GridPos cell) {
    const glm::vec2 centre(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f);
    const int layer = std::clamp(cell.layer, 0, std::max(0, grid.layerCount() - 1));
    return {centre.x, grid.contains(cell.x, cell.z, layer) ? grid.heightAt(centre, layer) : 0.0f, centre.y};
}

glm::vec3 snapToStep(glm::vec3 position, float step) {
    if (step <= 0) {
        return position;
    }
    return glm::round(position / step) * step;
}

std::string uniquePlacementId(const std::string& prefix, const std::vector<Placement>& placements) {
    const auto taken = [&](const std::string& id) {
        return std::any_of(placements.begin(), placements.end(), [&](const Placement& p) { return p.id == id; });
    };
    const std::string base = prefix.empty() ? std::string("placement") : prefix;
    if (!taken(base)) {
        return base;
    }
    for (std::size_t n = 2;; ++n) {
        const auto id = base + std::to_string(n);
        if (!taken(id)) {
            return id;
        }
    }
}

namespace {
// "sprites/elder.anim.json" -> "elder"; "fx/crate.glb" -> "crate".
std::string stemOf(const std::string& path) {
    auto name = std::filesystem::path(path).filename().string();
    if (const auto dot = name.find('.'); dot != std::string::npos) {
        name.resize(dot);
    }
    std::string id;
    for (const char c : name) {
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        id += std::isalnum(static_cast<unsigned char>(lower)) ? lower : '_';
    }
    return id.empty() ? std::string("placement") : id;
}
} // namespace

std::optional<Placement> placementForAsset(AssetKind kind, const std::string& path, GridPos cell, const Grid& grid,
                                           const std::string& defaultType, const std::vector<Placement>& existing) {
    Placement p;
    switch (kind) {
    case AssetKind::Sprite:
        p.type = defaultType.empty() ? "prop" : defaultType;
        p.props["sprite"] = path;
        break;
    case AssetKind::Model:
        p.type = "prop";
        p.props["model"] = path;
        break;
    default: return std::nullopt;
    }
    p.id = uniquePlacementId(stemOf(path), existing);
    p.position = cellCentre(grid, cell);
    p.layer = std::clamp(cell.layer, 0, std::max(0, grid.layerCount() - 1));
    return p;
}

Placement duplicatePlacement(const Placement& source, const std::vector<Placement>& existing) {
    Placement copy = source;
    auto base = source.id;
    while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) {
        base.pop_back();
    }
    copy.id = uniquePlacementId(base.empty() ? source.id : base, existing);
    copy.position += glm::vec3(1, 0, 0);
    return copy;
}

bool syncPlacementEntity(World& world, const Placement& placement) {
    const auto entity = placementEntity(world, placement.id);
    if (entity == entt::null) {
        return false;
    }
    editTransform(world, entity, placement.position, placement.layer);
    auto& transform = world.registry().get<Transform>(entity);
    if (glm::length(placement.facing) > 1e-4f) {
        transform.facing = glm::normalize(placement.facing);
    }
    if (world.registry().all_of<MeshInstance>(entity)) {
        transform.yaw = glm::radians(placement.yaw);
        transform.previousYaw = transform.yaw;
        transform.scale = placement.scale;
    }
    return true;
}

std::vector<std::string> changedPlacementIds(const MapData& before, const MapData& after) {
    std::map<std::string, Json> was, now;
    for (const auto& p : before.placements) {
        was[p.id] = MapData{.placements = {p}}.toJson().at("placements").at(0);
    }
    for (const auto& p : after.placements) {
        now[p.id] = MapData{.placements = {p}}.toJson().at("placements").at(0);
    }
    std::vector<std::string> ids;
    for (const auto& [id, json] : was) {
        const auto found = now.find(id);
        if (found == now.end() || found->second != json) {
            ids.push_back(id);
        }
    }
    for (const auto& [id, json] : now) {
        if (!was.contains(id)) {
            ids.push_back(id);
        }
    }
    return ids;
}
} // namespace fe
