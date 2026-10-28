#include "engine/core/AssetCache.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>

namespace fe {
namespace {
bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
} // namespace

AssetCache::AssetCache(std::filesystem::path root, std::filesystem::path libraryRoot)
    : root_(std::filesystem::weakly_canonical(root)),
      libraryRoot_(std::filesystem::weakly_canonical(libraryRoot)), thread_([this] { worker(); }) {
}

AssetCache::~AssetCache() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

std::filesystem::path AssetCache::file(const std::string& relative) const {
    if (relative.starts_with("@fe/")) {
        return resolveInside(libraryRoot_ / "fe", relative.substr(4));
    }
    return resolveInside(root_, relative);
}

AssetCache::Decoded AssetCache::decode(const std::filesystem::path& file) {
    const auto name = file.filename().string();
    const auto extension = file.extension().string();
    if (extension == ".glb" || extension == ".gltf") {
        return std::make_shared<const Model>(Model::load(file));
    }
    if (endsWith(name, ".anim.json")) {
        return std::make_shared<const SpriteAnimation>(SpriteAnimation::load(file));
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga") {
        return std::make_shared<const Image>(Image::load(file));
    }
    return {};
}

std::shared_ptr<const Model> AssetCache::model(const std::string& path) {
    collect();
    if (const auto found = models_.find(path); found != models_.end()) {
        return found->second;
    }
    auto value = std::make_shared<const Model>(Model::load(file(path)));
    models_[path] = value;
    return value;
}

std::shared_ptr<const SpriteAnimation> AssetCache::sprite(const std::string& path) {
    collect();
    if (const auto found = sprites_.find(path); found != sprites_.end()) {
        return found->second;
    }
    auto value = std::make_shared<const SpriteAnimation>(SpriteAnimation::load(file(path)));
    sprites_[path] = value;
    return value;
}

std::shared_ptr<const Image> AssetCache::image(const std::string& path) {
    collect();
    if (const auto found = images_.find(path); found != images_.end()) {
        return found->second;
    }
    auto value = std::make_shared<const Image>(Image::load(file(path)));
    images_[path] = value;
    return value;
}

template<class T, class Load>
std::shared_ptr<const T> tryLoad(std::set<std::string>& failed, const std::string& path, Load&& load) {
    if (path.empty() || failed.contains(path)) {
        return {};
    }
    try {
        return load(path);
    } catch (const std::exception& error) {
        failed.insert(path);
        log(LogLevel::Error, error.what());
        return {};
    }
}

std::shared_ptr<const Model> AssetCache::tryModel(const std::string& path) {
    return tryLoad<Model>(failed_, path, [this](const std::string& p) { return model(p); });
}
std::shared_ptr<const SpriteAnimation> AssetCache::trySprite(const std::string& path) {
    return tryLoad<SpriteAnimation>(failed_, path, [this](const std::string& p) { return sprite(p); });
}
std::shared_ptr<const Image> AssetCache::tryImage(const std::string& path) {
    return tryLoad<Image>(failed_, path, [this](const std::string& p) { return image(p); });
}

void AssetCache::defineFont(const std::string& name, const Json& spec) {
    const auto files = spec.at("files").get<std::vector<std::string>>();
    if (files.empty()) {
        throw std::runtime_error("Font '" + name + "' needs at least one file");
    }
    const float size = spec.value("size", 12.0f);
    const bool pixel = spec.value("pixel", true);
    std::vector<std::shared_ptr<Font>> faces;
    for (const auto& relative : files) {
        const auto path = file(relative);
        const auto extension = path.extension().string();
        if (extension == ".fnt") {
            faces.push_back(loadBitmapFont(path));
        } else if (extension == ".ttf" || extension == ".otf") {
            faces.push_back(loadTrueTypeFont(path, size, pixel));
        } else {
            throw std::runtime_error("Unsupported font file " + relative + " (use .fnt, .ttf or .otf)");
        }
    }
    fontSpecs_[name] = spec;
    fonts_[name] = std::make_shared<FontSet>(std::move(faces));
    ++generation_;
}

std::shared_ptr<FontSet> AssetCache::font(const std::string& name) {
    const auto found = fonts_.find(name);
    if (found == fonts_.end()) {
        throw std::runtime_error("Unknown font '" + name + "'");
    }
    return found->second;
}

int AssetCache::preload(const std::vector<std::string>& paths) {
    std::lock_guard lock(mutex_);
    const int ticket = nextTicket_++;
    auto& entry = tickets_[ticket];
    for (const auto& path : paths) {
        if (models_.contains(path) || sprites_.contains(path) || images_.contains(path)) {
            continue;
        }
        queue_.emplace_back(ticket, path);
        ++entry.total;
    }
    wake_.notify_one();
    return ticket;
}

float AssetCache::progress(int ticket) {
    collect();
    std::lock_guard lock(mutex_);
    const auto found = tickets_.find(ticket);
    if (found == tickets_.end() || found->second.total == 0) {
        return 1;
    }
    return static_cast<float>(found->second.done) / static_cast<float>(found->second.total);
}

void AssetCache::worker() {
    for (;;) {
        std::pair<int, std::string> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        Decoded decoded;
        try {
            decoded = decode(file(job.second));
        } catch (const std::exception& error) {
            log(LogLevel::Error, error.what());
        }
        std::lock_guard lock(mutex_);
        finished_.emplace_back(job.second, std::move(decoded));
        ++tickets_[job.first].done;
    }
}

void AssetCache::collect() {
    std::vector<std::pair<std::string, Decoded>> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(finished_);
    }
    for (auto& [path, value] : ready) {
        if (auto* m = std::get_if<std::shared_ptr<const Model>>(&value)) {
            models_.try_emplace(path, *m);
        } else if (auto* s = std::get_if<std::shared_ptr<const SpriteAnimation>>(&value)) {
            sprites_.try_emplace(path, *s);
        } else if (auto* i = std::get_if<std::shared_ptr<const Image>>(&value)) {
            images_.try_emplace(path, *i);
        }
    }
}

bool AssetCache::reload(const std::filesystem::path& absolute) {
    collect();
    auto relative = relativeTo(root_, absolute);
    if (relative.empty()) {
        relative = relativeTo(libraryRoot_ / "fe", absolute);
        if (relative.empty()) {
            return false;
        }
        relative = "@fe/" + relative;
    }
    failed_.erase(relative);
    bool changed = false;
    try {
        if (models_.contains(relative)) {
            models_[relative] = std::make_shared<const Model>(Model::load(absolute));
            changed = true;
        }
        if (sprites_.contains(relative)) {
            sprites_[relative] = std::make_shared<const SpriteAnimation>(SpriteAnimation::load(absolute));
            changed = true;
        }
        if (images_.contains(relative)) {
            images_[relative] = std::make_shared<const Image>(Image::load(absolute));
            changed = true;
        }
    } catch (const std::exception& error) {
        log(LogLevel::Error, std::string("Reload kept the previous asset: ") + error.what());
        return false;
    }
    // Sprite atlases and font pages are separate files: rebuild dependants.
    for (auto& [path, sprite] : sprites_) {
        const auto atlas = std::filesystem::path(path).parent_path() / sprite->atlas;
        if (atlas.generic_string() == relative) {
            images_.erase(atlas.generic_string());
            changed = true;
        }
    }
    for (const auto& [name, spec] : fontSpecs_) {
        const auto files = spec.at("files").get<std::vector<std::string>>();
        const bool uses = std::any_of(files.begin(), files.end(), [&](const std::string& f) {
            return f == relative ||
                   std::filesystem::path(f).parent_path() == std::filesystem::path(relative).parent_path();
        });
        if (uses) {
            try {
                defineFont(name, spec);
                changed = true;
            } catch (const std::exception& error) {
                log(LogLevel::Error, std::string("Font reload failed: ") + error.what());
            }
        }
    }
    if (changed) {
        ++generation_;
    }
    return changed;
}
} // namespace fe
