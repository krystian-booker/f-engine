#pragma once
#include "engine/editor/EditorCamera.h"
#include <QHash>
#include <QIcon>
#include <bgfx/bgfx.h>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>

namespace fe {
class Engine;
class FrameRenderer;
class Renderer;
class World;
} // namespace fe

namespace fe::editor {
// Pictures of assets for the Project panel: images and sprite sheets as they
// are, models rendered once each (framed by their bounds, in a small world
// of their own) and kept as PNG files in the editor's folder for the project.
class Thumbnails {
public:
    Thumbnails(Renderer& renderer, Engine& engine, std::filesystem::path cacheFolder, int pixelsPerUnit);
    ~Thumbnails();

    // The picture of a file, or a null icon when it has none (yet): a model
    // not rendered so far is queued, and onReady fires when its picture is in.
    [[nodiscard]] QIcon icon(const std::filesystem::path& file);
    // Each frame, between the renderer's beginFrame and endFrame: renders the
    // next queued model. Returns the first bgfx view left unused.
    std::uint16_t render(std::uint16_t firstView);
    void finishFrame(std::uint32_t frame);
    // A file changed: its picture is made again.
    void forget(const std::filesystem::path& file);
    [[nodiscard]] std::size_t pending() const { return queue_.size() + (rendering_.empty() ? 0 : 1); }

    std::function<void()> onReady;

private:
    [[nodiscard]] std::filesystem::path cacheFile(const std::filesystem::path& file) const;

    Engine& engine_;
    std::filesystem::path cache_;
    std::unique_ptr<FrameRenderer> frame_;
    std::unique_ptr<World> world_;
    bgfx::TextureHandle color_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle target_ = BGFX_INVALID_HANDLE;
    QHash<QString, QIcon> icons_;
    std::deque<std::filesystem::path> queue_;
    std::filesystem::path rendering_, written_;
    int waited_ = 0;
    std::uint64_t ticks_ = 0;
};
} // namespace fe::editor
