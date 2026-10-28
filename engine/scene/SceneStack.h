#pragma once
#include <memory>
#include <vector>

namespace fe {
class Scene {
public:
    virtual ~Scene() = default;
    virtual void enter() {}
    virtual void leave() {}
    virtual void update(double dt) = 0;
    virtual void render(double alpha) = 0;
    virtual bool blocksUpdate() const { return true; }
    virtual bool blocksRender() const { return false; }
};
// Mutations are deferred until after traversal so a scene may safely push/pop
// itself during callbacks. New scenes first update on the next tick.
class SceneStack {
public:
    void push(std::unique_ptr<Scene> scene);
    void pop();
    void clear();
    void update(double dt);
    void render(double alpha);
    [[nodiscard]] std::size_t size() const { return scenes_.size(); }
private:
    enum class Operation { Push, Pop, Clear };
    struct Pending { Operation operation; std::unique_ptr<Scene> scene; };
    void flush();
    std::vector<std::unique_ptr<Scene>> scenes_;
    std::vector<Pending> pending_;
};
}
