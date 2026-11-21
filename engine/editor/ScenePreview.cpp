#include "engine/editor/ScenePreview.h"
#include "engine/runtime/Engine.h"
#include "engine/world/Components.h"
#include "engine/world/World.h"
#include <algorithm>
#include <glm/glm.hpp>

namespace fe {
std::string spawnPlacementPreview(World& world, const std::string& id) {
    if (const auto existing = world.find(id); existing != entt::null) {
        world.destroy(existing);
    }
    const auto& placements = world.authored().placements;
    const auto found =
        std::find_if(placements.begin(), placements.end(), [&](const Placement& p) { return p.id == id; });
    if (found == placements.end()) {
        return {};
    }
    const auto model = found->props.find("model");
    const auto sprite = found->props.find("sprite");
    const bool hasModel = model != found->props.end() && model->is_string();
    const bool hasSprite = sprite != found->props.end() && sprite->is_string();
    if (!hasModel && !hasSprite) {
        return {};
    }
    try {
        const auto entity = world.spawn(found->id, found->position, found->layer);
        auto& transform = world.registry().get<Transform>(entity);
        transform.yaw = transform.previousYaw = glm::radians(found->yaw);
        transform.scale = found->scale;
        transform.facing = found->facing;
        if (hasModel) {
            MeshInstance mesh;
            mesh.model = model->get<std::string>();
            world.registry().emplace<MeshInstance>(entity, std::move(mesh));
        } else {
            Sprite visual;
            visual.animation = sprite->get<std::string>();
            world.registry().emplace<Sprite>(entity, std::move(visual));
        }
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

std::shared_ptr<World> openPreviewWorld(Engine& engine, const std::string& mapId) {
    if (engine.world(PreviewWorldName)) {
        engine.unloadWorld(PreviewWorldName);
    }
    auto world = engine.loadWorld(PreviewWorldName, mapId);
    for (const auto& placement : world->authored().placements) {
        spawnPlacementPreview(*world, placement.id);
    }
    return world;
}
} // namespace fe
