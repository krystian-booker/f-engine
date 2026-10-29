#include "engine/world/Animation.h"
#include <cmath>

namespace fe {
void restartClip(Sprite& sprite, const std::string& clip) {
    sprite.clip = clip;
    sprite.time = sprite.previousTime = 0;
    sprite.finished = false;
}

void advanceAnimations(entt::registry& registry, AssetCache& assets, float cameraYaw, double dt,
                       EventQueue& events) {
    static const std::string Idle = "idle";
    for (const auto entity : registry.view<Sprite>()) {
        auto& sprite = registry.get<Sprite>(entity);
        const auto* transform = registry.try_get<Transform>(entity);
        if (!sprite.moveClip.empty() && registry.all_of<Mover>(entity)) {
            const bool moved = transform && glm::length(transform->position - transform->previous) > 1e-4f;
            const auto& wanted = moved ? sprite.moveClip : (sprite.idleClip.empty() ? Idle : sprite.idleClip);
            if (sprite.clip != wanted &&
                (sprite.clip == sprite.moveClip || sprite.clip == sprite.idleClip || sprite.clip == Idle)) {
                restartClip(sprite, wanted);
            }
        }
        sprite.time += dt * sprite.speed;
        const auto animation = assets.trySprite(sprite.animation);
        if (!animation) {
            continue;
        }
        const int direction =
            sprite.direction >= 0
                ? sprite.direction
                : animation->direction(sprite.clip, transform ? transform->facing : glm::vec2(0, 1),
                                       cameraYaw);
        animation->events(sprite.clip, direction, sprite.previousTime, sprite.time,
                          [&](const std::string& name) {
                              events.push({WorldEvent::Kind::AnimationEvent, entity, entt::null, name});
                          });
        if (sprite.finished) {
            continue;
        }
        const auto& clip = animation->clip(sprite.clip);
        if (!clip.loop && sprite.time >= clip.length(direction)) {
            sprite.finished = true;
            events.push({WorldEvent::Kind::AnimationFinished, entity, entt::null, sprite.clip});
        }
    }
    for (const auto entity : registry.view<MeshInstance>()) {
        auto& mesh = registry.get<MeshInstance>(entity);
        if (!mesh.playing) {
            continue;
        }
        mesh.time += static_cast<float>(dt) * mesh.speed;
        const auto model = assets.tryModel(mesh.model);
        const int index = model ? model->animation(mesh.animation) : -1;
        if (index < 0) {
            continue;
        }
        const float duration = model->animations[static_cast<std::size_t>(index)].duration;
        if (duration <= 0) {
            continue;
        }
        if (mesh.loop) {
            mesh.time = std::fmod(mesh.time, duration);
        } else if (mesh.time >= duration) {
            mesh.time = duration;
            mesh.playing = false;
            events.push({WorldEvent::Kind::AnimationFinished, entity, entt::null, mesh.animation});
        }
    }
}

SpritePlacement placeSprite(const Sprite& s, const SpriteAnimation& animation, glm::vec3 feet,
                            glm::vec2 facing, const Camera& camera, double alpha, int pixelsPerUnit) {
    const int direction =
        s.direction >= 0 ? s.direction : animation.direction(s.clip, facing, camera.interpolatedYaw(alpha));
    SpritePlacement placement;
    placement.frame = &animation.frameFor(s.clip, direction, glm::mix(s.previousTime, s.time, alpha));
    const float ppu =
        static_cast<float>(animation.pixelsPerUnit > 0 ? animation.pixelsPerUnit : pixelsPerUnit);
    placement.size = glm::vec2(placement.frame->rect.z, placement.frame->rect.w) / ppu * s.scale;
    placement.pivot = animation.pivot;
    placement.lying = s.mode == SpriteMode::Flat || s.mode == SpriteMode::Decal;
    const auto forward = camera.forward(alpha);
    glm::vec3 position = feet, right = camera.right(alpha), up(0, 1, 0);
    if (s.mode == SpriteMode::Facing) {
        up = glm::normalize(glm::cross(right, forward));
    } else if (placement.lying) {
        right = glm::vec3(1, 0, 0);
        up = glm::vec3(0, 0, -1);
        position.y += FlatSpriteLift;
    }
    position += right * (s.pixelOffset.x / ppu) + up * (-s.pixelOffset.y / ppu);
    position -= forward * s.depthBias;
    placement.anchor = position;
    placement.right = right;
    placement.up = up;
    return placement;
}

SpriteQuad spriteQuad(const Sprite& s, const SpriteAnimation& animation, glm::vec3 feet, glm::vec2 facing,
                      const Camera& camera, double alpha, int pixelsPerUnit) {
    return placeSprite(s, animation, feet, facing, camera, alpha, pixelsPerUnit).quad();
}

std::optional<float> rayQuad(glm::vec3 origin, glm::vec3 direction, const SpriteQuad& quad) {
    const auto normal = glm::cross(quad.right, quad.up);
    const float facing = glm::dot(normal, direction);
    if (std::abs(facing) < 1e-9f) {
        return std::nullopt;
    }
    const float t = glm::dot(quad.corner - origin, normal) / facing;
    if (t < 0) {
        return std::nullopt;
    }
    const auto local = origin + direction * t - quad.corner;
    const float u = glm::dot(local, quad.right) / glm::dot(quad.right, quad.right);
    const float v = glm::dot(local, quad.up) / glm::dot(quad.up, quad.up);
    if (u < 0 || u > 1 || v < 0 || v > 1) {
        return std::nullopt;
    }
    return t;
}
} // namespace fe
