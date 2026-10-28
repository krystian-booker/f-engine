#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace fe {
template <class T> struct Handle {
    std::uint32_t index = UINT32_MAX;
    std::uint32_t generation = 0;
    bool operator==(const Handle&) const = default;
};
// One registry per asset type. Reload replaces the payload in the same slot;
// release invalidates old handles. A failed reload preserves the working asset.
template <class T> class AssetRegistry {
public:
    using Loader = std::function<std::shared_ptr<T>(const std::filesystem::path&)>;
    explicit AssetRegistry(Loader loader) : loader_(std::move(loader)) {}
    Handle<T> load(const std::filesystem::path& path) {
        const auto key = path.lexically_normal().generic_string();
        if (auto found = paths_.find(key); found != paths_.end()) return found->second;
        auto value = loader_(path);
        if (!value) throw std::runtime_error("Loader returned null for " + key);
        std::uint32_t index = 0;
        while (index < slots_.size() && slots_[index].value) ++index;
        if (index == slots_.size()) slots_.emplace_back();
        auto& slot = slots_[index];
        slot.path = path; slot.value = std::move(value);
        Handle<T> handle{index, slot.generation};
        paths_[key] = handle;
        return handle;
    }
    std::shared_ptr<T> get(Handle<T> handle) const {
        if (handle.index >= slots_.size() || slots_[handle.index].generation != handle.generation) return {};
        return slots_[handle.index].value;
    }
    void reload(Handle<T> handle) {
        if (!get(handle)) throw std::runtime_error("Stale asset handle");
        auto& slot = slots_[handle.index];
        auto replacement = loader_(slot.path);
        if (!replacement) throw std::runtime_error("Loader returned null");
        slot.value = std::move(replacement);
    }
    bool reloadPath(const std::filesystem::path& path) {
        const auto found = paths_.find(path.lexically_normal().generic_string());
        if (found == paths_.end()) return false;
        reload(found->second); return true;
    }
    void release(Handle<T> handle) {
        if (!get(handle)) return;
        auto& slot = slots_[handle.index];
        paths_.erase(slot.path.lexically_normal().generic_string());
        slot.value.reset(); ++slot.generation;
    }
private:
    struct Slot { std::filesystem::path path; std::shared_ptr<T> value; std::uint32_t generation = 1; };
    Loader loader_;
    std::vector<Slot> slots_;
    std::unordered_map<std::string, Handle<T>> paths_;
};
}
