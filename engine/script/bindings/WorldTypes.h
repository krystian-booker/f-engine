#pragma once
#include "engine/runtime/Engine.h"
#include <memory>

namespace fe {
// Script handles. Both hold weak references, so a script that keeps a handle
// after its world is unloaded gets a clear error instead of a dangling pointer.
struct WorldRef {
    std::weak_ptr<World> world;
    Engine* engine = nullptr;
    [[nodiscard]] World& get() const {
        auto locked = world.lock();
        if (!locked) {
            throw std::runtime_error("This world has been unloaded");
        }
        return *locked;
    }
    void ensureLoaded() const {
        if (world.expired()) {
            throw std::runtime_error("This world has been unloaded");
        }
    }
};

struct EntityRef {
    std::weak_ptr<World> world;
    entt::entity id = entt::null;
    Engine* engine = nullptr;
    [[nodiscard]] bool valid() const {
        const auto locked = world.lock();
        return locked && locked->valid(id);
    }
    [[nodiscard]] World& owner() const {
        auto locked = world.lock();
        if (!locked || !locked->valid(id)) {
            throw std::runtime_error("This entity no longer exists");
        }
        return *locked;
    }
    void ensureValid() const {
        if (!valid()) {
            throw std::runtime_error("This entity no longer exists");
        }
    }
    template<class T> T& get() const { return owner().registry().get<T>(id); }
    template<class T> T* find() const { return owner().registry().try_get<T>(id); }
    bool operator==(const EntityRef& other) const {
        return id == other.id && !world.owner_before(other.world) && !other.world.owner_before(world);
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

void bindEntity(Engine& engine);
void bindCamera(Engine& engine);
} // namespace fe
