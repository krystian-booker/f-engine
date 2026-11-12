#include "engine/editor/ScenePicking.h"
#include "engine/world/Geometry.h"
#include "engine/world/World.h"
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

namespace fe {
ViewRay viewRay(const Camera& camera, const ViewRect& view, glm::vec2 mousePx, double alpha) {
    ViewRay ray;
    camera.ray(view.ndc(mousePx), view.aspect(), ray.origin, ray.direction, alpha);
    return ray;
}

std::optional<GridHit> pickGround(const World& world, const Camera& camera, const ViewRect& view,
                                  glm::vec2 mousePx, double alpha, std::uint32_t layers) {
    const auto ray = viewRay(camera, view, mousePx, alpha);
    return raycastGrid(world.map().grid, ray.origin, ray.direction, camera.farPlane, layers);
}

std::optional<glm::vec2> projectToView(const Camera& camera, const ViewRect& view, glm::vec3 point,
                                       double alpha) {
    const auto ndc = camera.project(point, view.aspect(), alpha);
    if (ndc.z > 1 || !std::isfinite(ndc.x) || !std::isfinite(ndc.y)) {
        return std::nullopt;
    }
    return view.toPixels(glm::vec2(ndc));
}

std::optional<Selection> pickScene(World& world, const ViewRect& view, glm::vec2 mousePx,
                                   const PickOptions& options) {
    if (!view.contains(mousePx)) {
        return std::nullopt;
    }
    const Camera& camera = options.camera ? *options.camera : world.camera();
    const auto& map = world.authored();
    // 1. Handles drawn over the scene: nearest icon within reach wins.
    std::optional<Selection> best;
    float bestDistance = options.handleRadiusPixels;
    float bestDepth = std::numeric_limits<float>::infinity();
    const auto consider = [&](glm::vec3 position, Selection candidate) {
        const auto ndc = camera.project(position, view.aspect(), options.alpha);
        if (ndc.z > 1) {
            return;
        }
        const float distance = glm::length(view.toPixels(glm::vec2(ndc)) - mousePx);
        if (distance < bestDistance - 1e-3f ||
            (std::abs(distance - bestDistance) <= 1e-3f && ndc.z < bestDepth)) {
            bestDistance = distance;
            bestDepth = ndc.z;
            best = std::move(candidate);
        }
    };
    if (options.points) {
        for (const auto& [name, point] : map.points) {
            consider(point.position, Selection::point(name));
        }
    }
    if (options.lights) {
        for (std::size_t i = 0; i < map.lights.size(); ++i) {
            consider(map.lights[i].position, Selection::light(static_cast<int>(i)));
        }
    }
    if (options.emitters) {
        for (std::size_t i = 0; i < map.emitters.size(); ++i) {
            consider(map.emitters[i].position, Selection::emitter(static_cast<int>(i)));
        }
    }
    if (options.placements) {
        for (const auto& p : map.placements) {
            if (placementEntity(world, p.id) == entt::null) {
                consider(p.position, Selection::placement(p.id));
            }
        }
    }
    if (best) {
        return best;
    }
    // 2. The world ray.
    const auto ray = viewRay(camera, view, mousePx, options.alpha);
    RayOptions rayOptions;
    rayOptions.maxDistance = camera.farPlane;
    rayOptions.layers = options.layers;
    rayOptions.entities = options.entities || options.placements;
    rayOptions.solidOnly = false;
    rayOptions.pixelsPerUnit = options.pixelsPerUnit;
    rayOptions.alpha = options.alpha;
    rayOptions.camera = &camera;
    auto hit = world.raycast(ray.origin, ray.direction, rayOptions);
    // Meshes without a collider are not in the collision hash: test their bounds.
    if (rayOptions.entities) {
        auto& registry = world.registry();
        for (const auto [entity, transform, mesh] : registry.view<Transform, MeshInstance>().each()) {
            if (!mesh.visible || registry.all_of<Collider>(entity)) {
                continue;
            }
            const auto model = world.assets().tryModel(mesh.model);
            if (!model) {
                continue;
            }
            const auto m = glm::translate(glm::mat4(1), world.interpolated(entity, options.alpha)) *
                           glm::rotate(glm::mat4(1), transform.yaw, glm::vec3(0, 1, 0)) *
                           glm::scale(glm::mat4(1), transform.scale);
            Box box;
            transformBounds(m, model->min, model->max, box.min, box.max);
            const auto t = rayBox(ray.origin, ray.direction, box, hit ? hit->distance : camera.farPlane);
            if (t && (!hit || *t < hit->distance)) {
                hit = WorldHit{.kind = WorldHit::Kind::Entity,
                               .distance = *t,
                               .point = ray.origin + ray.direction * *t,
                               .cell = {},
                               .collider = -1,
                               .entity = entity};
            }
        }
    }
    if (!hit) {
        return std::nullopt;
    }
    switch (hit->kind) {
    case WorldHit::Kind::Entity: {
        const auto id = entityPlacement(world, hit->entity);
        if (!id.empty() && options.placements) {
            return Selection::placement(id);
        }
        if (options.entities) {
            return Selection::ofEntity(world, hit->entity);
        }
        return std::nullopt;
    }
    case WorldHit::Kind::Collider:
        // Boxes past the authored list come from geometry markers.
        if (options.colliders && hit->collider >= 0 &&
            hit->collider < static_cast<int>(map.colliders.size())) {
            return Selection::collider(hit->collider);
        }
        return std::nullopt;
    case WorldHit::Kind::Grid: {
        if (!options.zones) {
            return std::nullopt;
        }
        const Zone* smallest = nullptr;
        float smallestArea = std::numeric_limits<float>::infinity();
        for (const auto& z : map.zones) {
            if (!z.contains(hit->point, hit->cell.layer)) {
                continue;
            }
            const float area = std::max(0.0f, z.max.x - z.min.x) * std::max(0.0f, z.max.y - z.min.y);
            if (area < smallestArea) {
                smallestArea = area;
                smallest = &z;
            }
        }
        return smallest ? std::optional(Selection::zone(smallest->id)) : std::nullopt;
    }
    }
    return std::nullopt;
}
} // namespace fe
