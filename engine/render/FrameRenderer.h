#pragma once
#include "engine/render/CanvasRenderer.h"
#include "engine/render/FrameMath.h"
#include "engine/render/PostPass.h"
#include "engine/render/WorldRenderer.h"
#include "engine/runtime/Engine.h"
#include <memory>

namespace fe {
class Renderer;

// Draws a whole frame: every requested world in order, each composited over
// the previous ones (full screen, into a screen rect, or into a named render
// target); the canvas UI on top; then one screen pass that applies the
// freeze-frame, letterbox, transitions, radial blur, chromatic aberration,
// fade and flash to everything (so they also cover the UI and work with no
// world drawn) while copying to the back buffer.
class FrameRenderer {
public:
    FrameRenderer(Renderer& renderer, AssetCache& assets, int pixelsPerUnit);
    ~FrameRenderer();
    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;
    // Loads every program first, so a failure keeps the current ones.
    void reload();
    // `tick` counts logic ticks: a draw's camera override is interpolated
    // between the values it had on the previous tick and this one, like the
    // world camera, for each world and target (and the n-th such draw).
    void render(const std::vector<WorldDraw>& draws, const Canvas& canvas, const ScreenFx& screen,
                glm::ivec2 viewport, double alpha, double time, std::uint64_t tick);
    // Writes the next rendered frame's worlds (no canvas, screen effects or
    // editor) to a PNG at most 480 pixels wide, once the GPU has read it back.
    void captureScene(const std::filesystem::path& file) { sceneCaptures_.push_back(file); }
    // After bgfx::frame(): writes the scene captures whose read-back finished.
    void finishFrame(std::uint32_t frame);
    [[nodiscard]] std::uint32_t drawCalls() const { return calls_; }
    // Per-draw parameters (u_params) of an entity's game sprite or mesh
    // shader; see WorldRenderer::setShaderParams. What a script's
    // `entity:shader_params{...}` should reach.
    void setShaderParams(const World& world, entt::entity entity, const ShaderParams& params) {
        worlds_.setShaderParams(world, entity, params);
    }
    void clearShaderParams(const World& world, entt::entity entity) { worlds_.clearShaderParams(world, entity); }
    void clearShaderParams(const World& world) { worlds_.clearShaderParams(world); }

private:
    struct NamedTarget {
        glm::ivec2 size{0};
        Texture color;
        Framebuffer buffer;
        std::uint64_t clearedFrame = 0; // cleared once per frame before its first world
    };
    struct Readback {
        std::vector<std::filesystem::path> files;
        glm::ivec2 size{0};
        Texture texture;
        std::vector<std::uint8_t> rgba;
        std::uint32_t ready = 0;
    };
    Renderer& renderer_;
    GpuAssets gpu_;
    Uniforms uniforms_;
    CustomShaders custom_;
    WorldRenderer worlds_;
    PostPass post_;
    CanvasRenderer canvas_;
    Program present_;
    Uniform freezeSampler_;
    Texture frameColor_, freezeColor_;
    Framebuffer frame_;
    glm::ivec2 frameSize_{0};
    FramePool<std::pair<int, int>, SceneTarget> scenes_;
    FramePool<std::string, NamedTarget> named_;
    FreezeFrame freeze_;
    std::vector<std::filesystem::path> sceneCaptures_;
    std::vector<std::unique_ptr<Readback>> readbacks_;
    DrawCameras drawCameras_;
    std::uint32_t calls_ = 0;
    std::uint64_t frameCount_ = 0;
    bool warnedViews_ = false;
};
} // namespace fe
