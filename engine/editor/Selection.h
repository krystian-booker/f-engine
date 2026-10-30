#pragma once
#include "engine/world/MapData.h"
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <optional>
#include <string>
#include <utility>

namespace fe {
class World;

// What the editor has selected: an authored map object (by id, or by index
// into the map's list for lights, emitters and colliders) or a live entity
// (with its serial, so a respawned entity in the same slot is not it).
struct Selection {
    enum class Kind { None, Placement, Zone, Point, Light, Emitter, Collider, Entity };
    Kind kind = Kind::None;
    std::string id;                   // Placement, Zone, Point
    int index = -1;                   // Light, Emitter, Collider
    entt::entity entity = entt::null; // Entity
    std::uint64_t serial = ~std::uint64_t{0};
    bool operator==(const Selection&) const = default;
    [[nodiscard]] bool empty() const { return kind == Kind::None; }

    static Selection placement(std::string id) { return {Kind::Placement, std::move(id)}; }
    static Selection zone(std::string id) { return {Kind::Zone, std::move(id)}; }
    static Selection point(std::string name) { return {Kind::Point, std::move(name)}; }
    static Selection light(int index) { return {Kind::Light, {}, index}; }
    static Selection emitter(int index) { return {Kind::Emitter, {}, index}; }
    static Selection collider(int index) { return {Kind::Collider, {}, index}; }
    static Selection ofEntity(const World& world, entt::entity entity);
};
[[nodiscard]] const char* selectionKindName(Selection::Kind kind);

// Whether the selection still names something in the world (its map object
// exists, or its entity is alive with the same serial).
[[nodiscard]] bool selectionValid(const World& world, const Selection& selection);
// A readable name for lists and the status line ("elder (npc)", "Light 2").
[[nodiscard]] std::string selectionLabel(const World& world, const Selection& selection);

// Lookups by id in a map (nullptr / -1 when absent).
[[nodiscard]] const Placement* findPlacement(const MapData& map, std::string_view id);
[[nodiscard]] Placement* findPlacement(MapData& map, std::string_view id);
[[nodiscard]] int placementIndex(const MapData& map, std::string_view id);
[[nodiscard]] const Zone* findZone(const MapData& map, std::string_view id);
[[nodiscard]] Zone* findZone(MapData& map, std::string_view id);
[[nodiscard]] int zoneIndex(const MapData& map, std::string_view id);

// The spawned entity of an authored placement (fe.field names entities by
// their placement id), or null. The reverse: the placement id of an entity,
// or empty when no authored placement has its name.
[[nodiscard]] entt::entity placementEntity(const World& world, std::string_view id);
[[nodiscard]] std::string entityPlacement(const World& world, entt::entity entity);

// What a gizmo edits for a selection, and what it may do with it. Zones and
// colliders expose their centre as the position and their size as the scale.
struct SelectionTransform {
    glm::vec3 position{0};
    glm::vec2 facing{0, 1};
    float yawDegrees = 0;
    glm::vec3 scale{1};
    int layer = 0;
    bool canRotate = false;   // yaw (meshes) or facing (sprites, points)
    bool facingOnly = false;  // rotation writes `facing`, not `yaw`
    bool canScale = false;    // mesh scale, or a zone's or collider's size
    bool uniformScale = false; // sprites scale by one number
    bool snapsToGrid = false; // placements and points sit on cell centres by default
};
[[nodiscard]] std::optional<SelectionTransform> selectionTransform(const World& world, const Selection& selection);
// Writes a transform into the authored map for map kinds (not Entity);
// returns false when the selection is not in the map.
bool applySelectionTransform(MapData& map, const Selection& selection, const SelectionTransform& transform);
// World-space bounds for framing (F): the sprite quad or mesh box of a
// placement's entity, a zone's rectangle, a light's radius, a collider box;
// half a unit around a bare position.
[[nodiscard]] std::optional<std::pair<glm::vec3, glm::vec3>> selectionBounds(World& world, const Selection& selection,
                                                                             double alpha);
} // namespace fe
