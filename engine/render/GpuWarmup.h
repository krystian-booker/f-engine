#pragma once
#include "engine/render/GpuAssets.h"
#include "engine/runtime/WarmAssets.h"
#include <chrono>
#include <set>
#include <string>
#include <vector>

namespace fe {
// Uploads assets before they are drawn, a few milliseconds per frame, so the
// frame that first shows a map, a mesh or a sprite finds it on the GPU
// instead of decoding, building mip chains and filling buffers on the spot.
// What to upload comes from the engine (Engine::warmAssets): the assets of
// worlds still loading in the background and of preloads. An asset the cache
// has not decoded is queued on its worker thread, never decoded here.
class GpuWarmup {
public:
    explicit GpuWarmup(GpuAssets& gpu) : gpu_(gpu), revision_(gpu.revision()) {}
    // Works through `wanted` in order until `budget` has passed. One step (a
    // texture, a model's buffers, a sprite sheet) always runs, so the queue
    // drains even when a step is longer than the budget.
    void run(const std::vector<WarmAsset>& wanted, std::chrono::duration<double> budget);
    // True once an asset is uploaded, or will never be (it failed to load;
    // drawing it reports that).
    [[nodiscard]] bool ready(const WarmAsset& asset) const { return done_.contains(asset); }
    // How many of the assets last asked for are still to upload.
    [[nodiscard]] std::size_t backlog() const { return backlog_; }

private:
    enum class Step { Done, Waiting, OutOfTime };
    Step step(const WarmAsset& asset, std::chrono::steady_clock::time_point deadline);
    // True once `path` is in the asset cache or its background decode has
    // ended without it (a failure, which the upload then reports).
    bool decoded(const std::string& path);
    GpuAssets& gpu_;
    std::uint64_t revision_;
    std::set<WarmAsset> done_;
    std::set<std::string> requested_; // background decodes asked for here
    std::size_t backlog_ = 0;
};
} // namespace fe
