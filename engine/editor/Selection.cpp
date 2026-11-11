#include "engine/editor/Selection.h"
#include "engine/editor/SceneEdits.h"
#include "engine/world/Animation.h"
#include "engine/world/Geometry.h"
#include "engine/world/World.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace fe {
Selection Selection::ofEntity(const World& world, entt::entity entity) {
    Selection s;
    s.kind = Kind::Entity;
    s.entity = entity;
    s.serial = world.serial(entity);
    return s;
}

const char* selectionKindName(Selection::Kind kind) {
    switch (kind) {
    case Selection::Kind::Placement:
        return "placement";
    case Selection::Kind::Zone:
        return "zone";
    case Selection::Kind::Point:
        return "point";
    case Selection::Kind::Light:
        return "light";
    case Selection::Kind::Emitter:
        return "emitter";
    case Selection::Kind::Collider:
        return "collider";
    case Selection::Kind::Entity:
        return "entity";
    case Selection::Kind::None:
        break;
    }
    return "nothing";
}

const Placement* findPlacement(const MapData& map, std::string_view id) {
    const auto found = std::find_if(map.placements.begin(), map.placements.end(),
                                    [&](const Placement& p) { return p.id == id; });
    return found == map.placements.end() ? nullptr : &*found;
}
Placement* findPlacement(MapData& map, std::string_view id) {
    return const_cast<Placement*>(findPlacement(static_cast<const MapData&>(map), id));
}
int placementIndex(const MapData& map, std::string_view id) {
    const auto* p = findPlacement(map, id);
    return p ? static_cast<int>(p - map.placements.data()) : -1;
}
const Zone* findZone(const MapData& map, std::string_view id) {
    const auto found =
        std::find_if(map.zones.begin(), map.zones.end(), [&](const Zone& z) { return z.id == id; });
    return found == map.zones.end() ? nullptr : &*found;
}
Zone* findZone(MapData& map, std::string_view id) {
    return const_cast<Zone*>(findZone(static_cast<const MapData&>(map), id));
}
int zoneIndex(const MapData& map, std::string_view id) {
    const auto* z = findZone(map, id);
    return z ? static_cast<int>(z - map.zones.data()) : -1;
}

entt::entity placementEntity(const World& world, std::string_view id) {
    if (!findPlacement(world.authored(), id)) {
        return entt::null;
    }
    const auto entity = world.find(std::string(id));
    return world.valid(entity) && world.registry().all_of<Transform>(entity) ? entity : entt::null;
}

std::string entityPlacement(const World& world, entt::entity entity) {
    if (!world.valid(entity)) {
        return {};
    }
    const auto* name = world.registry().try_get<Name>(entity);
    if (!name || !findPlacement(world.authored(), name->id)) {
        return {};
    }
    return name->id;
}

bool selectionValid(const World& world, const Selection& s) {
    const auto& map = world.authored();
    switch (s.kind) {
    case Selection::Kind::None:
        return false;
    case Selection::Kind::Placement:
        return findPlacement(map, s.id) != nullptr;
    case Selection::Kind::Zone:
        return findZone(map, s.id) != nullptr;
    case Selection::Kind::Point:
        return map.points.contains(s.id);
    case Selection::Kind::Light:
        return s.index >= 0 && s.index < static_cast<int>(map.lights.size());
    case Selection::Kind::Emitter:
        return s.index >= 0 && s.index < static_cast<int>(map.emitters.size());
    case Selection::Kind::Collider:
        return s.index >= 0 && s.index < static_cast<int>(map.colliders.size());
    case Selection::Kind::Entity:
        return world.alive(s.entity, s.serial) && world.registry().all_of<Transform>(s.entity);
    }
    return false;
}

std::string selectionLabel(const World& world, const Selection& s) {
    const auto& map = world.authored();
    switch (s.kind) {
    case Selection::Kind::Placement:
        if (const auto* p = findPlacement(map, s.id)) {
            return p->id + " (" + p->type + ")";
        }
        return s.id;
    case Selection::Kind::Zone:
        if (const auto* z = findZone(map, s.id)) {
            return z->id + " (" + z->type + " zone)";
        }
        return s.id;
    case Selection::Kind::Point:
        return "Point " + s.id;
    case Selection::Kind::Light:
        return "Light " + std::to_string(s.index);
    case Selection::Kind::Emitter:
        if (s.index >= 0 && s.index < static_cast<int>(map.emitters.size())) {
            return "Emitter " + std::to_string(s.index) + " " +
                   map.emitters[static_cast<std::size_t>(s.index)].settings.sprite;
        }
        return "Emitter " + std::to_string(s.index);
    case Selection::Kind::Collider:
        return "Collider " + std::to_string(s.index);
    case Selection::Kind::Entity:
        if (world.valid(s.entity)) {
            if (const auto* name = world.registry().try_get<Name>(s.entity)) {
                return name->id;
            }
            return "#" + std::to_string(entt::to_entity(s.entity));
        }
        return "(gone)";
    case Selection::Kind::None:
        break;
    }
    return "Nothing";
}

std::optional<SelectionTransform> selectionTransform(const World& world, const Selection& s) {
    const auto& map = world.authored();
    const auto& registry = world.registry();
    SelectionTransform t;
    switch (s.kind) {
    case Selection::Kind::Placement: {
        const auto* p = findPlacement(map, s.id);
        if (!p) {
            return std::nullopt;
        }
        t.position = p->position;
        t.facing = p->facing;
        t.yawDegrees = p->yaw;
        t.scale = p->scale;
        t.layer = p->layer;
        t.snapsToGrid = true;
        t.canRotate = true;
        t.canScale = true;
        const auto entity = placementEntity(world, s.id);
        const bool mesh = entity != entt::null && registry.all_of<MeshInstance>(entity);
        t.facingOnly = !mesh;
        t.uniformScale = !mesh;
        return t;
    }
    case Selection::Kind::Point: {
        const auto found = map.points.find(s.id);
        if (found == map.points.end()) {
            return std::nullopt;
        }
        t.position = found->second.position;
        t.facing = found->second.facing;
        t.layer = found->second.layer;
        t.snapsToGrid = true;
        t.canRotate = true;
        t.facingOnly = true;
        return t;
    }
    case Selection::Kind::Light:
        if (s.index < 0 || s.index >= static_cast<int>(map.lights.size())) {
            return std::nullopt;
        }
        t.position = map.lights[static_cast<std::size_t>(s.index)].position;
        return t;
    case Selection::Kind::Emitter:
        if (s.index < 0 || s.index >= static_cast<int>(map.emitters.size())) {
            return std::nullopt;
        }
        t.position = map.emitters[static_cast<std::size_t>(s.index)].position;
        return t;
    case Selection::Kind::Collider: {
        if (s.index < 0 || s.index >= static_cast<int>(map.colliders.size())) {
            return std::nullopt;
        }
        const auto& box = map.colliders[static_cast<std::size_t>(s.index)];
        t.position = (box.min + box.max) * 0.5f;
        t.scale = glm::max(box.max - box.min, glm::vec3(0.01f));
        t.canScale = true;
        return t;
    }
    case Selection::Kind::Zone: {
        const auto* z = findZone(map, s.id);
        if (!z) {
            return std::nullopt;
        }
        const glm::vec2 centre = (z->min + z->max) * 0.5f;
        const int layer = std::max(0, z->layer);
        t.position = {centre.x, map.grid.heightAt(centre, std::min(layer, map.grid.layerCount() - 1)),
                      centre.y};
        t.scale = {std::max(z->max.x - z->min.x, 0.01f), 1, std::max(z->max.y - z->min.y, 0.01f)};
        t.layer = z->layer;
        t.canScale = true;
        return t;
    }
    case Selection::Kind::Entity: {
        if (!world.alive(s.entity, s.serial) || !registry.all_of<Transform>(s.entity)) {
            return std::nullopt;
        }
        const auto& transform = registry.get<Transform>(s.entity);
        t.position = transform.position;
        t.facing = transform.facing;
        t.yawDegrees = glm::degrees(transform.yaw);
        t.scale = transform.scale;
        t.layer = transform.layer;
        const bool mesh = registry.all_of<MeshInstance>(s.entity);
        t.canRotate = true;
        t.facingOnly = !mesh;
        t.canScale = mesh;
        return t;
    }
    case Selection::Kind::None:
        break;
    }
    return std::nullopt;
}

bool applySelectionTransform(MapData& map, const Selection& s, const SelectionTransform& t) {
    switch (s.kind) {
    case Selection::Kind::Placement:
        if (auto* p = findPlacement(map, s.id)) {
            p->position = t.position;
            p->facing = glm::length(t.facing) > 1e-4f ? glm::normalize(t.facing) : p->facing;
            p->yaw = t.yawDegrees;
            p->scale = t.scale;
            p->layer = std::clamp(t.layer, 0, map.grid.layerCount() - 1);
            return true;
        }
        return false;
    case Selection::Kind::Point:
        if (const auto found = map.points.find(s.id); found != map.points.end()) {
            found->second.position =
                clampToGrid(map.grid, t.position); // a point off the grid cannot be saved
            found->second.facing =
                glm::length(t.facing) > 1e-4f ? glm::normalize(t.facing) : found->second.facing;
            found->second.layer = std::clamp(t.layer, 0, map.grid.layerCount() - 1);
            return true;
        }
        return false;
    case Selection::Kind::Light:
        if (s.index >= 0 && s.index < static_cast<int>(map.lights.size())) {
            map.lights[static_cast<std::size_t>(s.index)].position = t.position;
            return true;
        }
        return false;
    case Selection::Kind::Emitter:
        if (s.index >= 0 && s.index < static_cast<int>(map.emitters.size())) {
            map.emitters[static_cast<std::size_t>(s.index)].position = t.position;
            return true;
        }
        return false;
    case Selection::Kind::Collider:
        if (s.index >= 0 && s.index < static_cast<int>(map.colliders.size())) {
            auto& box = map.colliders[static_cast<std::size_t>(s.index)];
            const auto half = glm::max(glm::abs(t.scale), glm::vec3(0.01f)) * 0.5f;
            box.min = t.position - half;
            box.max = t.position + half;
            return true;
        }
        return false;
    case Selection::Kind::Zone:
        if (auto* z = findZone(map, s.id)) {
            const glm::vec2 half(std::max(std::abs(t.scale.x), 0.01f) * 0.5f,
                                 std::max(std::abs(t.scale.z), 0.01f) * 0.5f);
            z->min = glm::vec2(t.position.x, t.position.z) - half;
            z->max = glm::vec2(t.position.x, t.position.z) + half;
            z->layer = std::clamp(t.layer, -1, map.grid.layerCount() - 1);
            return true;
        }
        return false;
    case Selection::Kind::Entity:
    case Selection::Kind::None:
        break;
    }
    return false;
}

namespace {
std::optional<std::pair<glm::vec3, glm::vec3>> entityBounds(World& world, entt::entity entity, double alpha) {
    auto& registry = world.registry();
    const auto& transform = registry.get<Transform>(entity);
    const auto position = world.interpolated(entity, alpha);
    if (const auto* mesh = registry.try_get<MeshInstance>(entity)) {
        if (const auto model = world.assets().tryModel(mesh->model)) {
            const auto m = glm::translate(glm::mat4(1), position) *
                           glm::rotate(glm::mat4(1), transform.yaw, glm::vec3(0, 1, 0)) *
                           glm::scale(glm::mat4(1), transform.scale);
            glm::vec3 lo, hi;
            transformBounds(m, model->min, model->max, lo, hi);
            return std::pair(lo, hi);
        }
    }
    if (const auto* sprite = registry.try_get<Sprite>(entity)) {
        if (const auto animation = world.assets().trySprite(sprite->animation)) {
            const auto quad = spriteQuad(*sprite, *animation, position, transform.facing, world.camera(),
                                         alpha, animation->pixelsPerUnit);
            glm::vec3 lo = quad.corner, hi = quad.corner;
            for (const auto& corner :
                 {quad.corner + quad.right, quad.corner + quad.up, quad.corner + quad.right + quad.up}) {
                lo = glm::min(lo, corner);
                hi = glm::max(hi, corner);
            }
            return std::pair(lo, hi);
        }
    }
    if (const auto* collider = registry.try_get<Collider>(entity)) {
        const glm::vec3 r(collider->radius, 0, collider->radius);
        return std::pair(position - r, position + r + glm::vec3(0, collider->height, 0));
    }
    return std::pair(position - glm::vec3(0.5f, 0, 0.5f), position + glm::vec3(0.5f, 1, 0.5f));
}
} // namespace

std::optional<std::pair<glm::vec3, glm::vec3>> selectionBounds(World& world, const Selection& s,
                                                               double alpha) {
    const auto& map = world.authored();
    const auto around = [](glm::vec3 p, float r) {
        return std::pair(p - glm::vec3(r), p + glm::vec3(r));
    };
    switch (s.kind) {
    case Selection::Kind::Placement: {
        const auto entity = placementEntity(world, s.id);
        if (entity != entt::null) {
            return entityBounds(world, entity, alpha);
        }
        if (const auto* p = findPlacement(map, s.id)) {
            return std::pair(p->position - glm::vec3(0.5f, 0, 0.5f), p->position + glm::vec3(0.5f, 1, 0.5f));
        }
        return std::nullopt;
    }
    case Selection::Kind::Entity:
        if (world.alive(s.entity, s.serial) && world.registry().all_of<Transform>(s.entity)) {
            return entityBounds(world, s.entity, alpha);
        }
        return std::nullopt;
    case Selection::Kind::Point:
        if (const auto found = map.points.find(s.id); found != map.points.end()) {
            return around(found->second.position, 0.75f);
        }
        return std::nullopt;
    case Selection::Kind::Light:
        if (s.index >= 0 && s.index < static_cast<int>(map.lights.size())) {
            const auto& l = map.lights[static_cast<std::size_t>(s.index)];
            return around(l.position, std::clamp(l.radius, 0.5f, 20.0f));
        }
        return std::nullopt;
    case Selection::Kind::Emitter:
        if (s.index >= 0 && s.index < static_cast<int>(map.emitters.size())) {
            const auto& e = map.emitters[static_cast<std::size_t>(s.index)];
            return around(e.position, std::max(0.75f, glm::length(e.settings.area)));
        }
        return std::nullopt;
    case Selection::Kind::Collider:
        if (s.index >= 0 && s.index < static_cast<int>(map.colliders.size())) {
            const auto& b = map.colliders[static_cast<std::size_t>(s.index)];
            return std::pair(b.min, b.max);
        }
        return std::nullopt;
    case Selection::Kind::Zone:
        if (const auto* z = findZone(map, s.id)) {
            const int layer = std::clamp(std::max(0, z->layer), 0, map.grid.layerCount() - 1);
            const float h = map.grid.heightAt((z->min + z->max) * 0.5f, layer);
            return std::pair(glm::vec3(z->min.x, h - 0.5f, z->min.y), glm::vec3(z->max.x, h + 1, z->max.y));
        }
        return std::nullopt;
    case Selection::Kind::None:
        break;
    }
    return std::nullopt;
}
} // namespace fe
