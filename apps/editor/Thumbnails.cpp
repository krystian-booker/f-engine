#include "apps/editor/Thumbnails.h"
#include "engine/core/Json.h"
#include "engine/core/Log.h"
#include "engine/editor/MapFiles.h"
#include "engine/render/FrameRenderer.h"
#include "engine/render/Renderer.h"
#include "engine/runtime/Engine.h"
#include "engine/world/Components.h"
#include "engine/world/World.h"
#include <QFileInfo>
#include <QImageReader>
#include <QPixmap>
#include <algorithm>
#include <functional>

namespace fe::editor {
namespace {
constexpr int Size = 192;

QString key(const std::filesystem::path& file) {
    return QString::fromStdString(file.string());
}

bool isModel(const std::filesystem::path& file) {
    const auto extension = file.extension().string();
    return extension == ".glb" || extension == ".gltf";
}

QIcon imageIcon(const QString& file) {
    QImageReader reader(file);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > 256 || size.height() > 256)) {
        reader.setScaledSize(size.scaled(256, 256, Qt::KeepAspectRatio));
    }
    const QImage image = reader.read();
    return image.isNull() ? QIcon() : QIcon(QPixmap::fromImage(image));
}
} // namespace

Thumbnails::Thumbnails(Renderer& renderer, Engine& engine, std::filesystem::path cacheFolder,
                       int pixelsPerUnit)
    : engine_(engine), cache_(std::move(cacheFolder)) {
    std::filesystem::create_directories(cache_);
    // A renderer and a world of its own: nothing here touches the scene
    // being edited or its GPU state.
    frame_ = std::make_unique<FrameRenderer>(renderer, engine.assets(), pixelsPerUnit);
    auto map = makeBlankMap("thumbnails", "Thumbnails", 4, 4);
    map.environment.fogDensity = 0;
    world_ = std::make_unique<World>("editor_thumbnails", std::move(map), engine.assets(), 1);
    color_ = bgfx::createTexture2D(Size, Size, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[] = {color_};
    target_ = bgfx::createFrameBuffer(1, attachments, false);
}

Thumbnails::~Thumbnails() {
    world_.reset();
    frame_.reset();
    if (bgfx::isValid(target_)) {
        bgfx::destroy(target_);
    }
    if (bgfx::isValid(color_)) {
        bgfx::destroy(color_);
    }
}

// Named by the file's path, size and time, so an edited model gets a new picture.
std::filesystem::path Thumbnails::cacheFile(const std::filesystem::path& file) const {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    const auto time = std::filesystem::last_write_time(file, error).time_since_epoch().count();
    const auto hash =
        std::hash<std::string>{}(file.string() + "|" + std::to_string(size) + "|" + std::to_string(time));
    return cache_ / (std::to_string(hash) + ".png");
}

QIcon Thumbnails::icon(const std::filesystem::path& file) {
    const QString name = key(file);
    if (const auto found = icons_.constFind(name); found != icons_.constEnd()) {
        return *found;
    }
    const auto extension = file.extension().string();
    QIcon made;
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga") {
        made = imageIcon(name);
    } else if (file.filename().string().ends_with(".anim.json")) {
        // A sprite: its sheet.
        try {
            const auto sheet = readJson(file);
            if (const auto atlas = sheet.find("atlas"); atlas != sheet.end() && atlas->is_string()) {
                made = imageIcon(key(file.parent_path() / atlas->get<std::string>()));
            }
        } catch (const std::exception&) {
            // Not a sprite sheet after all: the file type's own icon.
        }
    } else if (isModel(file)) {
        const auto cached = cacheFile(file);
        if (std::filesystem::is_regular_file(cached)) {
            made = imageIcon(key(cached));
        } else {
            if (file != rendering_ && std::find(queue_.begin(), queue_.end(), file) == queue_.end()) {
                queue_.push_back(file);
            }
            return {}; // asked again once it is in
        }
    }
    icons_.insert(name, made);
    return made;
}

void Thumbnails::forget(const std::filesystem::path& file) {
    icons_.remove(key(file));
    if (file.extension() == ".png") {
        icons_.clear(); // sprites show their sheets
    }
}

std::uint16_t Thumbnails::render(std::uint16_t firstView) {
    if (!rendering_.empty()) {
        // The picture is written once the GPU has handed the frame back.
        if (std::filesystem::is_regular_file(written_) || ++waited_ > 120) {
            if (waited_ > 120) {
                icons_.insert(key(rendering_), QIcon()); // gave up: the type's own icon
            }
            for (const auto entity : world_->registry().view<const MeshInstance>()) {
                world_->destroy(entity);
                break;
            }
            rendering_.clear();
            if (onReady) {
                onReady();
            }
        }
        return firstView;
    }
    if (queue_.empty()) {
        return firstView;
    }
    const auto file = queue_.front();
    queue_.pop_front();
    std::error_code error;
    const auto relative = std::filesystem::relative(file, engine_.assets().root(), error).generic_string();
    const auto model = error ? nullptr : engine_.assets().tryModel(relative);
    if (!model) {
        icons_.insert(key(file), QIcon());
        return firstView;
    }
    try {
        const auto entity = world_->spawn("subject", glm::vec3(2, 0, 2), 0);
        MeshInstance mesh;
        mesh.model = relative;
        world_->registry().emplace<MeshInstance>(entity, std::move(mesh));
    } catch (const std::exception& problem) {
        log(LogLevel::Debug, "No thumbnail for " + relative + ": " + problem.what());
        icons_.insert(key(file), QIcon());
        return firstView;
    }
    EditorCamera camera;
    camera.frame(model->min + glm::vec3(2, 0, 2), model->max + glm::vec3(2, 0, 2));
    WorldDraw draw;
    draw.world = std::shared_ptr<World>(world_.get(), [](World*) {});
    draw.camera = camera.camera;
    draw.snapCamera = true;
    rendering_ = file;
    written_ = cacheFile(file);
    waited_ = 0;
    frame_->captureScene(written_);
    engine_.canvas().begin();
    FrameOutput out;
    out.target = target_;
    out.firstView = firstView;
    return frame_->render({draw}, engine_.canvas(), ScreenFx{}, glm::ivec2(Size), 1.0, 0.0, ++ticks_, out);
}

void Thumbnails::finishFrame(std::uint32_t frame) {
    frame_->finishFrame(frame);
}
} // namespace fe::editor
