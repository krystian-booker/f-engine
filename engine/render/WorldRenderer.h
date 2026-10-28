#pragma once
#include "engine/render/Camera.h"
#include <filesystem>
#include <memory>

namespace fe {
class World;
class WorldRenderer {
public:
    WorldRenderer(std::filesystem::path gameRoot, std::filesystem::path shaderRoot,int pixelsPerUnit=32);
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;
    void updateCamera(const World& world, double dt, bool snap = false);
    void render(World& world, double alpha, int width, int height);
    void reload(const std::filesystem::path& path);
    [[nodiscard]] Camera& camera();
    bool shadows = true;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
