#pragma once
#include "engine/core/Json.h"
#include <entt/entt.hpp>
#include <string>
#include <vector>

namespace fe {
class World;

// One component type as generic JSON, so a tool (the editor's Inspector) can
// show and edit every component of any live entity without knowing its
// struct. Field names are those of the Lua settings tables (`cast_shadow`,
// `max_step`...); vectors are arrays of numbers, colours [r, g, b, a], enums
// their lowercase names, angles degrees.
struct ComponentInfo {
    const char* name; // the struct's name: "Transform", "Sprite"...
    // False for the components an entity keeps for life (Name, EntitySerial,
    // Transform, and ScriptRef, whose Lua reference only the script bindings
    // release); the others may be removed through the registry.
    bool removable;
    bool (*has)(const World&, entt::entity);
    // The component as an object of its fields, the editable ones and the
    // run-time state listed in `readOnly`; null when the entity has none.
    Json (*read)(const World&, entt::entity);
    // Applies an edited object of the shape read() returns: unknown keys are
    // ignored and missing keys keep their value. The fields are validated
    // into a copy that is stored only when all of them passed, so a refused
    // edit changes nothing. An entity without the component gets one (the
    // defaults plus the edit), as a Lua settings table would add it; the
    // components that are read-only as a whole (EntitySerial, ScriptRef)
    // change nothing. Returns the problem, empty on success.
    std::string (*write)(World&, entt::entity, const Json&);
    // Fields of read() that write() ignores: state shown for information.
    std::vector<std::string> readOnly;
};

// Every public component of Components.h, in a stable display order (Name and
// Transform first).
[[nodiscard]] const std::vector<ComponentInfo>& componentInfos();
// The infos of the components `entity` has, in that order; none for an
// entity that no longer exists.
[[nodiscard]] std::vector<const ComponentInfo*> componentsOf(const World& world, entt::entity entity);
} // namespace fe
