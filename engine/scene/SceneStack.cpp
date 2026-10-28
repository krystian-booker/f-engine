#include "engine/scene/SceneStack.h"
#include <stdexcept>

namespace fe {
void SceneStack::push(std::unique_ptr<Scene> scene) {
    if (!scene) throw std::invalid_argument("Cannot push a null scene");
    pending_.push_back({Operation::Push, std::move(scene)});
}
void SceneStack::pop() { pending_.push_back({Operation::Pop, {}}); }
void SceneStack::clear() { pending_.push_back({Operation::Clear, {}}); }
void SceneStack::flush() {
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& change : pending) {
        switch (change.operation) {
        case Operation::Push:
            scenes_.push_back(std::move(change.scene)); scenes_.back()->enter(); break;
        case Operation::Pop:
            if (!scenes_.empty()) { scenes_.back()->leave(); scenes_.pop_back(); } break;
        case Operation::Clear:
            while (!scenes_.empty()) { scenes_.back()->leave(); scenes_.pop_back(); } break;
        }
    }
}
void SceneStack::update(double dt) {
    flush();
    std::size_t first = scenes_.size();
    while (first > 0) { --first; if (scenes_[first]->blocksUpdate()) break; }
    for (std::size_t i = first; i < scenes_.size(); ++i) scenes_[i]->update(dt);
    flush();
}
void SceneStack::render(double alpha) {
    flush();
    std::size_t first = scenes_.size();
    while (first > 0) { --first; if (scenes_[first]->blocksRender()) break; }
    for (std::size_t i = first; i < scenes_.size(); ++i) scenes_[i]->render(alpha);
    flush();
}
}
