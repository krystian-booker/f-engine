#include "engine/render/GpuWarmup.h"
#include <filesystem>

namespace fe {
bool GpuWarmup::decoded(const std::string& path) {
    auto& assets = gpu_.cpu();
    if (assets.loaded(path)) {
        return true;
    }
    if (assets.decoding(path)) {
        return false;
    }
    if (assets.loaded(path)) {
        return true; // its decode ended just now
    }
    if (requested_.insert(path).second) {
        assets.decodeInBackground(path);
        return false;
    }
    return true; // asked for, and its decode ended without it
}

GpuWarmup::Step GpuWarmup::step(const WarmAsset& asset, std::chrono::steady_clock::time_point deadline) {
    using Kind = WarmAsset::Kind;
    const auto& path = asset.path;
    switch (asset.kind) {
    case Kind::Geometry:
    case Kind::Mesh: {
        const bool baked = asset.kind == Kind::Geometry;
        if (gpu_.hasModel(path, baked)) {
            return Step::Done;
        }
        if (!decoded(path)) {
            return Step::Waiting;
        }
        // Textures first, one per step: they are the slow part of a map
        // (a mip chain each), and model() then only fills buffers.
        while (gpu_.uploadNextModelTexture(path)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return Step::OutOfTime;
            }
        }
        gpu_.model(path, baked);
        return Step::Done;
    }
    case Kind::Sprite: {
        if (!decoded(path)) {
            return Step::Waiting;
        }
        // The animation names its images; they decode in the background too.
        if (const auto animation = gpu_.cpu().cachedSprite(path)) {
            const auto folder = std::filesystem::path(path).parent_path();
            bool waiting = false;
            for (const auto* name : {&animation->atlas, &animation->normal, &animation->emissive}) {
                if (!name->empty() && !decoded((folder / *name).generic_string())) {
                    waiting = true;
                }
            }
            if (waiting) {
                return Step::Waiting;
            }
        }
        gpu_.spriteSheet(path);
        return Step::Done;
    }
    case Kind::Scenery:
    case Kind::Panorama:
        if (!decoded(path)) {
            return Step::Waiting;
        }
        gpu_.image(path, asset.kind == Kind::Scenery ? TextureUse::Scenery : TextureUse::Panorama);
        return Step::Done;
    case Kind::Lut:
        gpu_.lut(path);
        return Step::Done;
    }
    return Step::Done;
}

void GpuWarmup::run(const std::vector<WarmAsset>& wanted, std::chrono::duration<double> budget) {
    if (gpu_.revision() != revision_) {
        // Assets were reloaded or evicted: what was uploaded may be gone.
        revision_ = gpu_.revision();
        done_.clear();
        requested_.clear();
    }
    if (wanted.empty()) {
        backlog_ = 0;
        if (!done_.empty()) {
            done_.clear();
            requested_.clear();
        }
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(budget);
    bool working = true;
    backlog_ = 0;
    for (const auto& asset : wanted) {
        if (done_.contains(asset)) {
            continue;
        }
        if (working) {
            const auto result = step(asset, deadline);
            if (result == Step::Done) {
                done_.insert(asset);
                working = std::chrono::steady_clock::now() < deadline;
                continue;
            }
            // A waiting asset does not hold up the ones after it.
            working = result == Step::Waiting;
        }
        ++backlog_;
    }
}
} // namespace fe
