#include "engine/render/FrameRenderer.h"
#include "engine/assets/Image.h"
#include "engine/core/Log.h"
#include "engine/render/Renderer.h"
#include <algorithm>
#include <utility>

namespace fe {
namespace {
constexpr int ThumbnailWidth = 480;

float transitionKind(const std::string& kind) {
    return kind == "swirl"     ? 1.0f
           : kind == "mosaic"  ? 2.0f
           : kind == "wipe"    ? 3.0f
           : kind == "iris"    ? 4.0f
           : kind == "fade"    ? 5.0f
           : kind == "shatter" ? 6.0f
                               : 0.0f;
}

Texture colorTexture(glm::ivec2 size, std::uint64_t flags) {
    return Texture(bgfx::createTexture2D(static_cast<std::uint16_t>(size.x),
                                         static_cast<std::uint16_t>(size.y), false, 1,
                                         bgfx::TextureFormat::RGBA8, flags));
}

// Draws a world with another camera and post-processing, restoring the
// world's own afterwards.
class DrawOverride {
public:
    DrawOverride(World& world, const Camera* camera, const std::optional<PostSettings>& post)
        : world_(world) {
        if (camera) {
            camera_ = world.camera();
            world.camera() = *camera;
        }
        if (post) {
            post_ = std::exchange(world.environment().post, *post);
        }
    }
    ~DrawOverride() {
        if (camera_) {
            world_.camera() = *camera_;
        }
        if (post_) {
            world_.environment().post = std::move(*post_);
        }
    }
    DrawOverride(const DrawOverride&) = delete;
    DrawOverride& operator=(const DrawOverride&) = delete;

private:
    World& world_;
    std::optional<Camera> camera_;
    std::optional<PostSettings> post_;
};
} // namespace

FrameRenderer::FrameRenderer(Renderer& renderer, AssetCache& assets, int pixelsPerUnit)
    : renderer_(renderer), gpu_(assets), custom_(assets.root(), renderer.shaderRoot()),
      worlds_(gpu_, uniforms_, custom_, renderer.shaderRoot(), pixelsPerUnit),
      post_(gpu_, uniforms_, custom_, renderer.shaderRoot()), canvas_(gpu_, renderer.shaderRoot()),
      present_(loadProgram(renderer.shaderRoot(), "vs_screen", "fs_present")),
      freezeSampler_(bgfx::createUniform("s_freeze", bgfx::UniformType::Sampler)) {
    renderer_.attach(this);
}

FrameRenderer::~FrameRenderer() {
    renderer_.attach(nullptr);
}

void FrameRenderer::reload() {
    const auto& root = renderer_.shaderRoot();
    Program present(loadProgram(root, "vs_screen", "fs_present"));
    worlds_.reload(root);
    post_.reload(root);
    canvas_.reload(root);
    present_ = std::move(present);
    custom_.reload(root);
}

void FrameRenderer::render(const std::vector<WorldDraw>& draws, const Canvas& canvas, const ScreenFx& screen,
                           glm::ivec2 viewport, double alpha, double time, std::uint64_t tick) {
    gpu_.sync();
    calls_ = 0;
    ++frameCount_;
    viewport = glm::max(viewport, glm::ivec2(1));
    const bool resized = viewport != frameSize_;
    if (resized) {
        frameSize_ = viewport;
        frameColor_ = colorTexture(viewport, BGFX_TEXTURE_RT | LinearClamp);
        const bgfx::TextureHandle attachments[] = {frameColor_.get()};
        frame_ = Framebuffer(bgfx::createFrameBuffer(1, attachments, false));
    }
    // View 0 belongs to the Renderer and the last view to the editor UI.
    const auto last = static_cast<std::uint32_t>(bgfx::getCaps()->limits.maxViews - 1);
    std::uint16_t next = 1;
    const auto take = [&] {
        bgfx::resetView(next);
        return next++;
    };
    const auto blit = [&](const char* name, bgfx::TextureHandle destination) {
        const auto view = take();
        bgfx::setViewName(view, name);
        bgfx::blit(view, {.handle = destination}, {.handle = frameColor_.get()});
        bgfx::touch(view);
    };

    // The freeze-frame holds what was on screen: last frame's image, still in
    // the frame texture, unless the window size just changed.
    const auto freeze = freeze_.update(screen.frozen, screen.freezeSerial);
    if (freeze == FreezeFrame::Action::Release) {
        freezeColor_.reset();
    } else if (freeze == FreezeFrame::Action::Capture) {
        freezeColor_ = colorTexture(viewport, BGFX_TEXTURE_BLIT_DST | LinearClamp);
        if (!resized) {
            blit("Freeze frame", freezeColor_.get());
        }
    }

    const auto clear = take();
    bgfx::setViewName(clear, "Frame");
    bgfx::setViewFrameBuffer(clear, frame_.get());
    bgfx::setViewRect(clear, 0, 0, static_cast<std::uint16_t>(viewport.x),
                      static_cast<std::uint16_t>(viewport.y));
    bgfx::setViewClear(clear, BGFX_CLEAR_COLOR, 0x000000ff);
    bgfx::touch(clear);

    const auto presentation = canvas.present(viewport);
    for (std::size_t slot = 0; slot < draws.size(); ++slot) {
        const auto& draw = draws[slot];
        auto& world = *draw.world;
        const auto& settings = draw.post ? *draw.post : world.environment().post;
        if (next + SceneViews::Count + PostPass::viewsNeeded(settings) + 1u + 3u > last) {
            if (!warnedViews_) {
                log(LogLevel::Warning, "Too many worlds drawn in one frame; the rest are skipped");
                warnedViews_ = true;
            }
            break;
        }
        PostOutput output;
        if (!draw.target.empty()) {
            const glm::vec2 canvasSize = draw.rect ? glm::vec2(draw.rect->w, draw.rect->h)
                                                   : glm::vec2(canvas.width(), canvas.height());
            const auto size = glm::clamp(glm::ivec2(glm::round(canvasSize * presentation.scale)),
                                         glm::ivec2(1), glm::ivec2(8192));
            auto& named = named_.acquire(
                draw.target,
                [&] {
                    NamedTarget made;
                    made.size = size;
                    made.color = colorTexture(size, BGFX_TEXTURE_RT | LinearClamp);
                    const bgfx::TextureHandle attachments[] = {made.color.get()};
                    made.buffer = Framebuffer(bgfx::createFrameBuffer(1, attachments, false));
                    return made;
                },
                [&](const NamedTarget& existing) { return existing.size == size; });
            if (named.clearedFrame != frameCount_) {
                const auto view = take();
                bgfx::setViewName(view, "Render target");
                bgfx::setViewFrameBuffer(view, named.buffer.get());
                bgfx::setViewRect(view, 0, 0, static_cast<std::uint16_t>(size.x),
                                  static_cast<std::uint16_t>(size.y));
                bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x00000000);
                bgfx::touch(view);
                named.clearedFrame = frameCount_;
            }
            output = {named.buffer.get(), glm::ivec4(0, 0, size)};
        } else if (draw.pixelRect) {
            const auto& r = *draw.pixelRect;
            const auto from = glm::clamp(glm::ivec2(r.x, r.y), glm::ivec2(0), viewport);
            const auto to = glm::clamp(glm::ivec2(r.x + r.z, r.y + r.w), glm::ivec2(0), viewport);
            if (to.x <= from.x || to.y <= from.y) {
                continue;
            }
            output = {frame_.get(), glm::ivec4(from, to - from)};
        } else if (draw.rect) {
            const auto& r = *draw.rect;
            const auto from = glm::clamp(
                glm::ivec2(glm::round(presentation.offset + glm::vec2(r.x, r.y) * presentation.scale)),
                glm::ivec2(0), viewport);
            const auto to =
                glm::clamp(glm::ivec2(glm::round(presentation.offset +
                                                 glm::vec2(r.x + r.w, r.y + r.h) * presentation.scale)),
                           glm::ivec2(0), viewport);
            if (to.x <= from.x || to.y <= from.y) {
                continue;
            }
            output = {frame_.get(), glm::ivec4(from, to - from)};
        } else {
            output = {frame_.get(), glm::ivec4(0, 0, viewport)};
        }
        const glm::ivec2 outSize(output.rect.z, output.rect.w);
        const auto sceneSize = sceneSizeFor(outSize, settings.pixelScale);
        auto& scene = scenes_.acquire({sceneSize.x, sceneSize.y}, [&] { return SceneTarget(sceneSize); });
        const float aspect = static_cast<float>(outSize.x) / static_cast<float>(outSize.y);
        const auto* camera =
            draw.camera ? &drawCameras_.update(&world, draw.target, *draw.camera, tick, draw.snapCamera) : nullptr;
        const DrawOverride drawn(world, camera, draw.post);
        const SceneViews views(next);
        for (std::uint16_t i = 0; i < SceneViews::Count; ++i) {
            take();
        }
        worlds_.render(world, slot, alpha, time, aspect, views, scene, calls_);
        post_.render(world, screen, scene, output, alpha, time, take, calls_);
    }

    for (const auto& [target, key] : keepRequests_) {
        const auto* named = named_.find(target);
        if (!named || next + 1 >= last) {
            continue;
        }
        auto copy = colorTexture(named->size, BGFX_TEXTURE_BLIT_DST | LinearClamp);
        const auto view = take();
        bgfx::setViewName(view, "Kept target");
        bgfx::blit(view, {.handle = copy.get()}, {.handle = named->color.get()});
        bgfx::touch(view);
        kept_[key] = std::move(copy);
    }
    keepRequests_.clear();

    if (!sceneCaptures_.empty()) {
        auto readback = std::make_unique<Readback>();
        readback->files = std::move(sceneCaptures_);
        sceneCaptures_.clear();
        readback->size = viewport;
        readback->texture = colorTexture(viewport, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        readback->rgba.resize(static_cast<std::size_t>(viewport.x) * static_cast<std::size_t>(viewport.y) *
                              4);
        blit("Scene capture", readback->texture.get());
        readback->ready = bgfx::read({.handle = readback->texture.get()}, readback->rgba.data());
        readbacks_.push_back(std::move(readback));
    }

    canvas_.render(
        canvas, viewport, take(), frame_.get(), time,
        [this](std::string_view name) -> bgfx::TextureHandle {
            const auto* target = named_.find(name); // no string built per quad
            return target ? target->color.get() : bgfx::TextureHandle BGFX_INVALID_HANDLE;
        },
        calls_);
    if (freeze == FreezeFrame::Action::Capture && resized) {
        blit("Freeze frame", freezeColor_.get());
    }
    drawCameras_.endFrame();
    scenes_.endFrame();
    named_.endFrame();
    post_.endFrame();
    custom_.poll();

    const auto view = take();
    bgfx::setViewName(view, "Screen effects");
    bgfx::setViewRect(view, 0, 0, static_cast<std::uint16_t>(viewport.x),
                      static_cast<std::uint16_t>(viewport.y));
    const auto& t = screen.transition;
    const float aspect = static_cast<float>(viewport.x) / static_cast<float>(viewport.y);
    const bool frozen = freeze_.held() && freezeColor_.valid();
    const glm::vec4 present[Uniforms::PresentCount] = {
        glm::vec4(transitionKind(t.kind), t.progress, t.centre),
        t.color,
        screen.fade,
        screen.flash,
        glm::vec4(std::clamp(screen.letterbox, 0.0f, 1.0f) * screen.letterboxHeight, aspect,
                  bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f, frozen ? 1.0f : 0.0f),
        glm::vec4(screen.radialBlur, screen.radialCentre, screen.chromatic),
        packOutput(renderer_.output())};
    bgfx::setUniform(uniforms_.present.get(), present, Uniforms::PresentCount);
    bgfx::setTexture(0, uniforms_.frame.get(), frameColor_.get(), PointClamp);
    bgfx::setTexture(1, freezeSampler_.get(), frozen ? freezeColor_.get() : frameColor_.get(), LinearClamp);
    fullscreen(view, present_.get(), BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, calls_);
}

bgfx::TextureHandle FrameRenderer::keptTarget(std::string_view key) const {
    const auto found = kept_.find(key);
    return found == kept_.end() ? bgfx::TextureHandle BGFX_INVALID_HANDLE : found->second.get();
}

void FrameRenderer::finishFrame(std::uint32_t frame) {
    std::erase_if(readbacks_, [&](const std::unique_ptr<Readback>& readback) {
        if (frame < readback->ready) {
            return false;
        }
        const auto size = readback->size;
        if (bgfx::getCaps()->originBottomLeft) {
            const auto row = static_cast<std::size_t>(size.x) * 4;
            for (int y = 0; y < size.y / 2; ++y) {
                std::swap_ranges(
                    readback->rgba.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(y)),
                    readback->rgba.begin() +
                        static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(y + 1)),
                    readback->rgba.begin() +
                        static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(size.y - 1 - y)));
            }
        }
        Image image;
        glm::ivec2 scaled;
        image.rgba = downscaleRgba(readback->rgba, size, thumbnailDivisor(size, ThumbnailWidth), scaled);
        image.width = scaled.x;
        image.height = scaled.y;
        for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
            image.rgba[i] = 255;
        }
        for (const auto& file : readback->files) {
            try {
                image.savePng(file);
                log(LogLevel::Info, "Wrote scene capture " + file.string());
            } catch (const std::exception& error) {
                log(LogLevel::Error, error.what());
            }
        }
        return true;
    });
}
} // namespace fe
