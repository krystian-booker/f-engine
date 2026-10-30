#include "engine/core/AssetCache.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <sstream>

namespace fe {
namespace {
constexpr std::string_view UserPrefix = "user://";

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The page images a bitmap font names (`page id=0 file="pixel.png"`), as
// paths relative to the same root as the font's own path.
std::set<std::string> bitmapFontPages(const std::string& fontRelative, const std::filesystem::path& file) {
    std::set<std::string> pages;
    std::istringstream lines(readFile(file));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("page")) {
            continue;
        }
        const auto at = line.find("file=\"");
        if (at == std::string::npos) {
            continue;
        }
        const auto end = line.find('"', at + 6);
        if (end == std::string::npos) {
            continue;
        }
        const auto folder = std::filesystem::path(fontRelative).parent_path();
        pages.insert((folder / line.substr(at + 6, end - at - 6)).lexically_normal().generic_string());
    }
    return pages;
}
} // namespace

std::size_t decodedBytes(const Image& image) {
    return image.rgba.size();
}

std::size_t decodedBytes(const Model& model) {
    std::size_t bytes = 0;
    std::set<const Image*> images;
    for (const auto& part : model.parts) {
        bytes += part.vertices.size() * sizeof(MeshVertex) + part.indices.size() * sizeof(std::uint32_t);
        for (const auto* texture : {&part.material.albedo, &part.material.normal, &part.material.emission}) {
            if (texture->image && images.insert(texture->image.get()).second) {
                bytes += decodedBytes(*texture->image);
            }
        }
    }
    for (const auto& animation : model.animations) {
        for (const auto& channel : animation.channels) {
            bytes += channel.times.size() * sizeof(float) + channel.values.size() * sizeof(glm::vec4);
        }
    }
    return bytes;
}

std::size_t decodedBytes(const SpriteAnimation& sprite) {
    std::size_t bytes = sizeof(SpriteAnimation);
    for (const auto& [name, clip] : sprite.clips) {
        for (const auto& row : clip.directions) {
            bytes += row.size() * sizeof(SpriteFrame);
        }
    }
    return bytes;
}

AssetCache::AssetCache(std::filesystem::path root, std::filesystem::path libraryRoot,
                       std::filesystem::path userRoot, std::size_t budget)
    : root_(std::filesystem::weakly_canonical(root)),
      libraryRoot_(std::filesystem::weakly_canonical(libraryRoot)),
      userRoot_(userRoot.empty() ? userRoot : std::filesystem::weakly_canonical(userRoot)), budget_(budget),
      thread_([this] { worker(); }) {
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
    if (relative.starts_with(UserPrefix)) {
        if (userRoot_.empty()) {
            throw std::runtime_error("No user data folder for " + relative);
        }
        return resolveInside(userRoot_, relative.substr(UserPrefix.size()));
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
    touch(path);
    if (const auto found = models_.find(path); found != models_.end()) {
        return found->second;
    }
    auto value = std::make_shared<const Model>(Model::load(file(path)));
    models_[path] = value;
    return value;
}

std::shared_ptr<const SpriteAnimation> AssetCache::sprite(const std::string& path) {
    collect();
    touch(path);
    if (const auto found = sprites_.find(path); found != sprites_.end()) {
        return found->second;
    }
    auto value = std::make_shared<const SpriteAnimation>(SpriteAnimation::load(file(path)));
    sprites_[path] = value;
    return value;
}

void AssetCache::refreshUserImage(const std::string& path) {
    if (!path.starts_with(UserPrefix)) {
        return;
    }
    std::filesystem::path absolute;
    try {
        absolute = file(path);
    } catch (const std::exception&) {
        return; // the load itself reports the bad path
    }
    Stamp stamp;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(absolute, error);
    if (!error) {
        const auto size = std::filesystem::file_size(absolute, error);
        if (!error) {
            stamp.emplace(time, size);
        }
    }
    const auto [found, added] = userStamps_.try_emplace(path, stamp);
    if (!added && found->second != stamp) {
        found->second = stamp;
        images_.erase(path);
        failed_.erase(path);
        ++generation_;
    }
}

std::shared_ptr<const Image> AssetCache::image(const std::string& path) {
    collect();
    refreshUserImage(path);
    touch(path);
    if (const auto found = images_.find(path); found != images_.end()) {
        return found->second;
    }
    auto value = std::make_shared<const Image>(Image::load(file(path)));
    images_[path] = value;
    return value;
}

bool AssetCache::loaded(const std::string& path) const {
    return models_.contains(path) || sprites_.contains(path) || images_.contains(path);
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
    if (failed_.contains(path)) {
        refreshUserImage(path);
    }
    return tryLoad<Image>(failed_, path, [this](const std::string& p) { return image(p); });
}

std::size_t AssetCache::bytesOf(const std::string& path) const {
    if (const auto found = images_.find(path); found != images_.end()) {
        return decodedBytes(*found->second);
    }
    if (const auto found = models_.find(path); found != models_.end()) {
        return decodedBytes(*found->second);
    }
    if (const auto found = sprites_.find(path); found != sprites_.end()) {
        return decodedBytes(*found->second);
    }
    return 0;
}

std::size_t AssetCache::cachedBytes() const {
    std::size_t bytes = 0;
    for (const auto& [path, image] : images_) {
        bytes += decodedBytes(*image);
    }
    for (const auto& [path, model] : models_) {
        bytes += decodedBytes(*model);
    }
    for (const auto& [path, sprite] : sprites_) {
        bytes += decodedBytes(*sprite);
    }
    return bytes;
}

bool AssetCache::erase(const std::string& path) {
    const bool had = images_.erase(path) + models_.erase(path) + sprites_.erase(path) > 0;
    lastUse_.erase(path);
    failed_.erase(path);
    return had;
}

bool AssetCache::release(const std::string& path) {
    collect();
    if (!erase(path)) {
        return false;
    }
    ++generation_;
    return true;
}

std::size_t AssetCache::releaseUnused(const std::set<std::string>& referenced) {
    collect();
    std::size_t total = cachedBytes();
    if (total <= budget_) {
        return 0;
    }
    // Oldest use first; assets never touched (preloaded, not yet used) count as oldest.
    std::vector<std::pair<std::uint64_t, std::string>> candidates;
    const auto consider = [&](const auto& map) {
        for (const auto& [path, value] : map) {
            if (!referenced.contains(path)) {
                const auto use = lastUse_.find(path);
                candidates.emplace_back(use == lastUse_.end() ? 0 : use->second, path);
            }
        }
    };
    consider(images_);
    consider(models_);
    consider(sprites_);
    std::sort(candidates.begin(), candidates.end());
    std::size_t evicted = 0;
    for (const auto& [use, path] : candidates) {
        if (total <= budget_) {
            break;
        }
        const auto bytes = bytesOf(path);
        erase(path);
        total -= std::min(total, bytes);
        ++evicted;
    }
    if (evicted > 0) {
        ++generation_;
    }
    return evicted;
}

void AssetCache::defineFont(const std::string& name, const Json& spec) {
    const auto files = spec.at("files").get<std::vector<std::string>>();
    if (files.empty()) {
        throw std::runtime_error("Font '" + name + "' needs at least one file");
    }
    const float size = spec.value("size", 12.0f);
    const bool pixel = spec.value("pixel", true);
    std::vector<std::shared_ptr<Font>> faces;
    std::set<std::string> used;
    for (const auto& relative : files) {
        const auto path = file(relative);
        const auto extension = path.extension().string();
        used.insert(relative);
        if (extension == ".fnt") {
            faces.push_back(loadBitmapFont(path));
            used.merge(bitmapFontPages(relative, path));
        } else if (extension == ".ttf" || extension == ".otf") {
            faces.push_back(loadTrueTypeFont(path, size, pixel));
        } else {
            throw std::runtime_error("Unsupported font file " + relative + " (use .fnt, .ttf or .otf)");
        }
    }
    fontSpecs_[name] = spec;
    fontFiles_[name] = std::move(used);
    auto& slot = fonts_[name];
    if (slot) {
        retiredFonts_.push_back(std::move(slot));
    }
    slot = std::make_shared<FontSet>(std::move(faces));
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
    if (found == tickets_.end()) {
        return 1;
    }
    if (found->second.done >= found->second.total) {
        tickets_.erase(found);
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
        hasFinished_.store(true, std::memory_order_release);
        ++tickets_[job.first].done;
    }
}

void AssetCache::collect() {
    if (!hasFinished_.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<std::string, Decoded>> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(finished_);
        hasFinished_.store(false, std::memory_order_relaxed);
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
    ++fileChanges_;
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
        const auto files = fontFiles_.find(name);
        if (files == fontFiles_.end() || !files->second.contains(relative)) {
            continue;
        }
        try {
            defineFont(name, spec);
            changed = true;
        } catch (const std::exception& error) {
            log(LogLevel::Error, std::string("Font reload failed: ") + error.what());
        }
    }
    if (changed) {
        ++generation_;
    }
    return changed;
}
} // namespace fe
