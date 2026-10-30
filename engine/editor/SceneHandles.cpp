#include "engine/editor/SceneHandles.h"
#include "engine/world/Animation.h"
#include "engine/world/Geometry.h"
#include "engine/world/World.h"
#include <cmath>
#include <functional>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace fe {
namespace {
using namespace handleColors;

void addCircle(HandleBatch& out, glm::vec3 centre, glm::vec3 axisA, glm::vec3 axisB, float radius, glm::vec4 color,
               float thickness, int segments = 32) {
    glm::vec3 previous = centre + axisA * radius;
    for (int i = 1; i <= segments; ++i) {
        const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        const glm::vec3 next = centre + (axisA * std::cos(angle) + axisB * std::sin(angle)) * radius;
        out.lines.push_back({previous, next, color, thickness});
        previous = next;
    }
}

void addCylinder(HandleBatch& out, glm::vec3 feet, float radius, float height, glm::vec4 color, float thickness) {
    const glm::vec3 x(1, 0, 0), z(0, 0, 1), up(0, height, 0);
    addCircle(out, feet, x, z, radius, color, thickness, 24);
    addCircle(out, feet + up, x, z, radius, color, thickness, 24);
    for (int i = 0; i < 4; ++i) {
        const float angle = glm::half_pi<float>() * static_cast<float>(i);
        const glm::vec3 side = (x * std::cos(angle) + z * std::sin(angle)) * radius;
        out.lines.push_back({feet + side, feet + side + up, color, thickness});
    }
}

void addQuadOutline(HandleBatch& out, const SpriteQuad& quad, glm::vec4 color, float thickness) {
    const auto a = quad.corner, b = quad.corner + quad.right, c = quad.corner + quad.right + quad.up,
               d = quad.corner + quad.up;
    out.lines.push_back({a, b, color, thickness});
    out.lines.push_back({b, c, color, thickness});
    out.lines.push_back({c, d, color, thickness});
    out.lines.push_back({d, a, color, thickness});
}

// The facing arrow: a line from the position along the facing with a head.
void addFacing(HandleBatch& out, glm::vec3 position, glm::vec2 facing, float length, glm::vec4 color,
               float thickness) {
    if (glm::length(facing) < 1e-4f) {
        return;
    }
    const auto dir = glm::normalize(glm::vec3(facing.x, 0, facing.y));
    const auto side = glm::vec3(-dir.z, 0, dir.x);
    const auto tip = position + dir * length;
    out.lines.push_back({position, tip, color, thickness});
    out.lines.push_back({tip, tip - dir * (length * 0.3f) + side * (length * 0.2f), color, thickness});
    out.lines.push_back({tip, tip - dir * (length * 0.3f) - side * (length * 0.2f), color, thickness});
}

void outlineEntity(World& world, entt::entity entity, glm::vec4 color, float thickness, double alpha,
                   HandleBatch& out) {
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
            addBoxLines(out, lo, hi, color, thickness);
            return;
        }
    }
    if (const auto* sprite = registry.try_get<Sprite>(entity)) {
        if (const auto animation = world.assets().trySprite(sprite->animation)) {
            addQuadOutline(out,
                           spriteQuad(*sprite, *animation, position, transform.facing, world.camera(), alpha,
                                      animation->pixelsPerUnit),
                           color, thickness);
            return;
        }
    }
    if (const auto* collider = registry.try_get<Collider>(entity)) {
        addCylinder(out, position, collider->radius, collider->height, color, thickness);
        return;
    }
    out.icons.push_back({position, HandleBatch::Icon::Shape::Diamond, color, Selection::ofEntity(world, entity)});
}
} // namespace

void addBoxLines(HandleBatch& out, glm::vec3 min, glm::vec3 max, glm::vec4 color, float thickness) {
    const glm::vec3 c[8] = {{min.x, min.y, min.z}, {max.x, min.y, min.z}, {max.x, min.y, max.z}, {min.x, min.y, max.z},
                            {min.x, max.y, min.z}, {max.x, max.y, min.z}, {max.x, max.y, max.z}, {min.x, max.y, max.z}};
    const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) {
        out.lines.push_back({c[e[0]], c[e[1]], color, thickness});
    }
}

void buildHandles(World& world, const HandleOptions& options, const Selection& primary, const Selection& hovered,
                  double alpha, HandleBatch& out, const std::vector<Selection>& extras) {
    const auto& map = world.authored();
    // Several things can be selected at once; each is emphasised like the primary.
    const auto isSelected = [&](const Selection& self) {
        return self == primary || std::find(extras.begin(), extras.end(), self) != extras.end();
    };
    const auto em = [&](const Selection& self, glm::vec4 base) {
        if (isSelected(self)) {
            return Selected;
        }
        if (self == hovered) {
            return Hovered;
        }
        return base;
    };
    const auto th = [&](const Selection& self) { return isSelected(self) ? 2.5f : 1.0f; };
    const Selection& selected = primary;
    const auto& grid = map.grid;
    const int layers = grid.layerCount();
    const auto heightAt = [&](glm::vec2 p, int layer) {
        return grid.heightAt(p, std::clamp(layer < 0 ? options.layer : layer, 0, layers - 1));
    };
    if (options.zones) {
        for (const auto& z : map.zones) {
            const auto self = Selection::zone(z.id);
            const auto color = em(self, ZoneColor);
            const float t = th(self);
            const float lift = 0.05f;
            const glm::vec2 corners[4] = {z.min, {z.max.x, z.min.y}, z.max, {z.min.x, z.max.y}};
            for (int i = 0; i < 4; ++i) {
                const auto a = corners[i], b = corners[(i + 1) % 4];
                // Follow the ground: split long edges so they ride ramps and steps.
                const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(b - a))));
                for (int s = 0; s < steps; ++s) {
                    const auto p = glm::mix(a, b, static_cast<float>(s) / steps);
                    const auto q = glm::mix(a, b, static_cast<float>(s + 1) / steps);
                    out.lines.push_back({{p.x, heightAt(p, z.layer) + lift, p.y},
                                         {q.x, heightAt(q, z.layer) + lift, q.y},
                                         color,
                                         t});
                }
            }
            const auto centre = (z.min + z.max) * 0.5f;
            const glm::vec3 c(centre.x, heightAt(centre, z.layer) + lift, centre.y);
            out.icons.push_back({c, HandleBatch::Icon::Shape::Square, color, self, 5});
            if (options.labels) {
                out.labels.push_back({c, z.id + " (" + z.type + ")", color});
            }
        }
    }
    if (options.points) {
        for (const auto& [name, point] : map.points) {
            const auto self = Selection::point(name);
            const auto color = em(self, PointColor);
            const float t = th(self);
            const auto top = point.position + glm::vec3(0, 0.9f, 0);
            out.lines.push_back({point.position, top, color, t});
            addFacing(out, point.position + glm::vec3(0, 0.02f, 0), point.facing, 0.6f, color, t);
            out.icons.push_back({top, HandleBatch::Icon::Shape::Flag, color, self});
            if (options.labels) {
                out.labels.push_back({top, name, color});
            }
        }
    }
    if (options.lights) {
        for (std::size_t i = 0; i < map.lights.size(); ++i) {
            const auto& l = map.lights[i];
            const auto self = Selection::light(static_cast<int>(i));
            const auto color = em(self, LightColor);
            const float t = th(self);
            const auto faint = glm::vec4(glm::vec3(color), color.a * 0.35f);
            out.icons.push_back({l.position, HandleBatch::Icon::Shape::Sun, color, self, 7});
            if (self != selected && self != hovered) {
                continue; // the reach is drawn for the light being looked at, not every light
            }
            if (l.type == LightType::Spot) {
                auto axis = glm::length(l.direction) > 1e-4f ? glm::normalize(l.direction) : glm::vec3(0, -1, 0);
                const auto side = std::abs(axis.y) < 0.99f ? glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)))
                                                           : glm::vec3(1, 0, 0);
                const auto other = glm::cross(axis, side);
                const float reach = std::clamp(l.radius, 0.5f, 60.0f);
                const float spread = reach * std::tan(glm::radians(std::clamp(l.outerCone, 1.0f, 89.0f)));
                const auto end = l.position + axis * reach;
                addCircle(out, end, side, other, spread, faint, t, 24);
                for (int k = 0; k < 8; ++k) {
                    const float angle = glm::two_pi<float>() * static_cast<float>(k) / 8;
                    out.lines.push_back(
                        {l.position, end + (side * std::cos(angle) + other * std::sin(angle)) * spread, faint, t});
                }
            } else {
                const float r = std::clamp(l.radius, 0.1f, 60.0f);
                addCircle(out, l.position, {1, 0, 0}, {0, 0, 1}, r, faint, t);
                addCircle(out, l.position, {1, 0, 0}, {0, 1, 0}, r, faint, t);
                addCircle(out, l.position, {0, 0, 1}, {0, 1, 0}, r, faint, t);
            }
        }
    }
    if (options.emitters) {
        for (std::size_t i = 0; i < map.emitters.size(); ++i) {
            const auto& e = map.emitters[i];
            const auto self = Selection::emitter(static_cast<int>(i));
            const auto color = em(self, EmitterColor);
            const float t = th(self);
            if (glm::length(e.settings.area) > 1e-4f) {
                const auto half = glm::abs(e.settings.area) * 0.5f;
                addBoxLines(out, e.position - half, e.position + half, glm::vec4(glm::vec3(color), color.a * 0.5f), t);
            }
            out.icons.push_back({e.position, HandleBatch::Icon::Shape::Diamond, color, self});
            if (options.labels && !e.settings.sprite.empty()) {
                out.labels.push_back({e.position, e.settings.sprite, color});
            }
        }
    }
    if (options.colliders) {
        for (std::size_t i = 0; i < map.colliders.size(); ++i) {
            const auto& b = map.colliders[i];
            const auto self = Selection::collider(static_cast<int>(i));
            addBoxLines(out, b.min, b.max, em(self, ColliderColor), th(self));
        }
    }
    auto& registry = world.registry();
    if (options.triggers) {
        for (const auto [entity, transform, collider] : registry.view<Transform, Collider>().each()) {
            if (collider.solid) {
                continue;
            }
            const auto id = entityPlacement(world, entity);
            const auto self = id.empty() ? Selection::ofEntity(world, entity) : Selection::placement(id);
            addCylinder(out, world.interpolated(entity, alpha), collider.radius, collider.height,
                        em(self, TriggerColor), th(self));
        }
    }
    if (!options.triggerBadges.empty()) {
        // What fires each scripted placement: pages, or the script shorthand.
        for (const auto& p : map.placements) {
            std::vector<std::string> triggers;
            if (const auto pages = p.props.find("pages"); pages != p.props.end() && pages->is_array()) {
                for (const auto& page : *pages) {
                    if (page.is_object()) {
                        const auto trigger = page.value("trigger", std::string("action"));
                        if (std::find(triggers.begin(), triggers.end(), trigger) == triggers.end()) {
                            triggers.push_back(trigger);
                        }
                    }
                }
            } else if (p.props.contains("script")) {
                triggers.push_back(p.props.value("trigger", std::string("action")));
            } else if (p.props.contains("map") && p.props.at("map").is_string()) {
                triggers.push_back(p.props.value("trigger", std::string("touch")));
            }
            if (triggers.empty()) {
                continue;
            }
            std::string badge;
            for (const auto& trigger : triggers) {
                const auto found = options.triggerBadges.find(trigger);
                badge += (found != options.triggerBadges.end() ? found->second : trigger) + " ";
            }
            const auto entity = placementEntity(world, p.id);
            auto position = entity != entt::null ? world.interpolated(entity, alpha) : p.position;
            position.y += 1.25f;
            out.labels.push_back({position, badge, glm::vec4(0.95f, 0.95f, 1, 0.9f)});
        }
    }
    if (options.placements) {
        for (const auto& p : map.placements) {
            if (placementEntity(world, p.id) != entt::null) {
                continue;
            }
            const auto self = Selection::placement(p.id);
            const auto color = em(self, PlacementColor);
            out.icons.push_back({p.position + glm::vec3(0, 0.3f, 0), HandleBatch::Icon::Shape::Diamond, color, self});
            if (options.labels) {
                out.labels.push_back({p.position + glm::vec3(0, 0.3f, 0), p.id + " (" + p.type + ")", color});
            }
        }
    }
    // The selection's and hover's own outline (an entity, or a placement's entity).
    std::vector<const Selection*> outlined{&selected};
    for (const auto& extra : extras) {
        outlined.push_back(&extra);
    }
    outlined.push_back(&hovered);
    for (const auto* s : outlined) {
        if (s->empty() || (s == &hovered && isSelected(hovered))) {
            continue;
        }
        const auto color = s == &hovered ? Hovered : Selected;
        const float t = s == &hovered ? 1.5f : 2.5f;
        entt::entity entity = entt::null;
        if (s->kind == Selection::Kind::Entity && world.alive(s->entity, s->serial)) {
            entity = s->entity;
        } else if (s->kind == Selection::Kind::Placement) {
            entity = placementEntity(world, s->id);
            if (entity == entt::null) {
                if (const auto* p = findPlacement(map, s->id)) {
                    addFacing(out, p->position + glm::vec3(0, 0.02f, 0), p->facing, 0.6f, color, t);
                }
            }
        }
        if (entity != entt::null && registry.all_of<Transform>(entity)) {
            outlineEntity(world, entity, color, t, alpha, out);
            if (const auto* transform = registry.try_get<Transform>(entity);
                transform && !registry.all_of<MeshInstance>(entity)) {
                addFacing(out, world.interpolated(entity, alpha) + glm::vec3(0, 0.02f, 0), transform->facing, 0.6f,
                          color, t);
            }
        }
    }
}
} // namespace fe
