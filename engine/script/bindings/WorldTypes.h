#pragma once
#include "engine/runtime/Engine.h"
#include "engine/script/Binder.h"
#include "engine/world/World.h"
#include <memory>
#include <tuple>

namespace fe {
// Script handles. All hold weak references, so a script that keeps a handle
// after its world is unloaded gets a clear error instead of a dangling pointer.
struct WorldRef {
    std::weak_ptr<World> world;
    Engine* engine = nullptr;
    [[nodiscard]] std::shared_ptr<World> lock() const {
        auto locked = world.lock();
        if (!locked) {
            throw std::runtime_error("This world has been unloaded");
        }
        return locked;
    }
    [[nodiscard]] World& get() const { return *lock(); }
};

// An entity handle names the entity by its slot and its EntitySerial, so
// once the entity is destroyed the handle stays invalid even when entt hands
// the slot (with a wrapped version) to a later entity.
struct EntityRef {
    std::weak_ptr<World> world;
    entt::entity id = entt::null;
    Engine* engine = nullptr;
    std::uint64_t serial = World::NoSerial;
    EntityRef() = default;
    // A handle to a live entity of `owner`.
    EntityRef(std::weak_ptr<World> owner, entt::entity entity, Engine* host)
        : world(std::move(owner)), id(entity), engine(host) {
        if (const auto locked = world.lock()) {
            serial = locked->serial(entity);
        }
    }
    [[nodiscard]] bool valid() const {
        const auto locked = world.lock();
        return locked && locked->alive(id, serial);
    }
    [[nodiscard]] std::shared_ptr<World> lock() const {
        auto locked = world.lock();
        if (!locked || !locked->alive(id, serial)) {
            throw std::runtime_error("This entity no longer exists");
        }
        return locked;
    }
    [[nodiscard]] World& owner() const { return *lock(); }
    template<class T> T& get() const { return owner().registry().get<T>(id); }
    template<class T> T* find() const { return owner().registry().try_get<T>(id); }
    [[nodiscard]] bool sameWorld(const EntityRef& other) const {
        return !world.owner_before(other.world) && !other.world.owner_before(world);
    }
    bool operator==(const EntityRef& other) const {
        return id == other.id && serial == other.serial && sameWorld(other);
    }
};

struct CameraRef {
    std::weak_ptr<World> world;
    Engine* engine = nullptr;
    [[nodiscard]] Camera& get() const {
        auto locked = world.lock();
        if (!locked) {
            throw std::runtime_error("This world has been unloaded");
        }
        return locked->camera();
    }
};

// Canvas position of a world point seen through `camera` as rendered at
// `alpha`; the flag is false when the point is behind the camera.
std::tuple<float, float, bool> projectToCanvas(Engine& engine, const Camera& camera, glm::vec3 point,
                                               double alpha = 1);

// The planning rules of world:path and entity:move_to from their `rules`
// table (an options table; nil gives the defaults).
struct StepRules;
StepRules stepRules(LuaHost& host, World& world, const sol::object& value);
// The tag bits of a list of tag names (nil = none); a name the grid's tag
// list lacks is an error naming `what`.
std::uint32_t gridTagBits(const Grid& grid, const sol::object& names, const char* what);

void bindEntity(Engine& engine);
void bindCamera(Engine& engine);
} // namespace fe
