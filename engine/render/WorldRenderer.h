#pragma once
#include "engine/render/GpuAssets.h"
#include "engine/services/ScreenFx.h"
#include "engine/world/World.h"
#include <filesystem>

namespace fe {
// Draws a World: shadows, sky, map geometry, mesh entities, sprites,
// particles, grid overlays, water and post-processing. One instance draws
// every world of a frame, in order; each gets its own bgfx view range.
class WorldRenderer {
public:
    WorldRenderer(GpuAssets& gpu, std::filesystem::path shaderRoot, int pixelsPerUnit);
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;
    void reloadPrograms();
    // `slot` (0..3) selects the bgfx view range; later slots draw on top.
    void render(World& world, double alpha, glm::ivec2 viewport, const ScreenFx& screen, double time,
                int slot);
    [[nodiscard]] std::uint32_t drawCalls() const { return drawCalls_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint32_t drawCalls_ = 0;
};
} // namespace fe
