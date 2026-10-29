#pragma once
#include "engine/core/AssetCache.h"
#include "engine/world/Camera.h"
#include "engine/world/Components.h"
#include "engine/world/Events.h"
#include <optional>

namespace fe {
void restartClip(Sprite& sprite, const std::string& clip);
// Advances sprite and mesh clips, switches automatic move/idle clips and
// reports frame events and finished clips. Fixed sprite directions outside a
// clip's rows clamp to the nearest row.
void advanceAnimations(entt::registry& registry, AssetCache& assets, float cameraYaw, double dt,
                       EventQueue& events);

// The world-space quad of a sprite's current frame: `corner` is the
// bottom-left corner, `right` and `up` span the full width and height.
struct SpriteQuad {
    glm::vec3 corner{0}, right{0}, up{0};
};
constexpr float FlatSpriteLift = 0.02f;
// A sprite's current frame and where it is drawn: `anchor` is the pivot
// point, `right` and `up` unit axes (camera-facing, upright, or lying on the
// ground for flat sprites and decals, which are lifted by FlatSpriteLift) and
// `size` the frame's size in world units. The renderer, picking and raycasts
// all place sprites through it.
struct SpritePlacement {
    const SpriteFrame* frame = nullptr;
    glm::vec3 anchor{0}, right{0}, up{0};
    glm::vec2 size{0}, pivot{0};
    bool lying = false;
    [[nodiscard]] SpriteQuad quad() const {
        return {anchor - right * (size.x * pivot.x) + up * (size.y * (pivot.y - 1)), right * size.x,
                up * size.y};
    }
};
[[nodiscard]] SpritePlacement placeSprite(const Sprite& sprite, const SpriteAnimation& animation,
                                          glm::vec3 feet, glm::vec2 facing, const Camera& camera,
                                          double alpha, int pixelsPerUnit);
[[nodiscard]] SpriteQuad spriteQuad(const Sprite& sprite, const SpriteAnimation& animation, glm::vec3 feet,
                                    glm::vec2 facing, const Camera& camera, double alpha, int pixelsPerUnit);
// Distance along a ray (unit `direction`) to where it crosses a quad, if it does.
[[nodiscard]] std::optional<float> rayQuad(glm::vec3 origin, glm::vec3 direction, const SpriteQuad& quad);
} // namespace fe
