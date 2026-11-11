#pragma once
#include "engine/assets/Image.h"
#include "engine/assets/Model.h"
#include "engine/assets/SpriteAnimation.h"
#include "engine/text/Font.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>

namespace fe {
// CPU-side asset cache shared by the world, scripts, the canvas and the
// renderer. Keys are game-relative paths. A worker thread decodes preload
// requests; everything else loads on first use. Files are read through
// engine/core/Paths.h, so they may come from mounted .pak archives. The
// generation number increases on every reload and eviction so GPU caches know
// to rebuild or drop their copies.
//
// Memory: decoded images, models and sprite animations count towards a byte
// budget (default 512 MiB). releaseUnused() evicts the least recently used
// assets outside a caller-supplied referenced set (the loaded worlds' assets)
// until the cache fits; release() drops one asset at once. An evicted asset
// loads again on next use.
class AssetCache {
public:
    static constexpr std::size_t DefaultBudget = std::size_t{512} << 20;
    // Paths beginning with "@fe/" resolve inside the builtin folder (shared fonts and
    // UI art), "user://" ones inside the per-user data folder (read only,
    // e.g. save thumbnails), all others inside the game folder.
    AssetCache(std::filesystem::path root, std::filesystem::path builtinRoot,
               std::filesystem::path userRoot = {}, std::size_t budget = DefaultBudget);
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    // These throw on failure; `tryX` variants log once and return null.
    // A "user://" image is reloaded when its file changes or reappears.
    std::shared_ptr<const Model> model(const std::string& path);
    std::shared_ptr<const SpriteAnimation> sprite(const std::string& path);
    std::shared_ptr<const Image> image(const std::string& path);
    std::shared_ptr<const Model> tryModel(const std::string& path);
    std::shared_ptr<const SpriteAnimation> trySprite(const std::string& path);
    std::shared_ptr<const Image> tryImage(const std::string& path);
    // Whether `path` is decoded and cached right now (no load is started).
    [[nodiscard]] bool loaded(const std::string& path) const;
    // The cached asset, or null: nothing is loaded and, unlike the accessors
    // above, the asset does not count as used (the renderer compares its GPU
    // copies with these without making every asset look recently used).
    [[nodiscard]] std::shared_ptr<const Model> cachedModel(const std::string& path) const;
    [[nodiscard]] std::shared_ptr<const SpriteAnimation> cachedSprite(const std::string& path) const;
    [[nodiscard]] std::shared_ptr<const Image> cachedImage(const std::string& path) const;

    // Fonts are named sets of faces, e.g. {"files":["ui/font.fnt","ui/noto.ttf"],"size":12}.
    // Redefining a font keeps the previous faces alive until
    // releaseRetiredFonts(), because canvas quads recorded this frame point
    // at them.
    void defineFont(const std::string& name, const Json& spec);
    std::shared_ptr<FontSet> font(const std::string& name);
    [[nodiscard]] bool hasFont(const std::string& name) const { return fonts_.contains(name); }
    // Call once nothing refers to the faces of redefined fonts (frame start).
    void releaseRetiredFonts() { retiredFonts_.clear(); }

    // Starts background decoding; returns a ticket for progress().
    int preload(const std::vector<std::string>& paths);
    // 0..1; a finished ticket is forgotten and reads as 1 from then on.
    [[nodiscard]] float progress(int ticket);
    // Moves finished background results into the cache (main thread). Cheap
    // when nothing finished: no lock is taken.
    void collect();

    // Eviction. Bytes are the decoded sizes: an image's pixels, a model's
    // vertices, indices and textures, a sprite animation's frames.
    void setBudget(std::size_t bytes) { budget_ = bytes; }
    [[nodiscard]] std::size_t budget() const { return budget_; }
    [[nodiscard]] std::size_t cachedBytes() const;
    [[nodiscard]] std::size_t cachedCount() const {
        return models_.size() + sprites_.size() + images_.size();
    }
    // Drops the cached asset at `path` (loaded again on next use); true when
    // one was cached. Bumps the generation.
    bool release(const std::string& path);
    // While the cache is over budget, evicts the least recently used assets
    // whose paths are not in `referenced`. Returns how many were evicted;
    // bumps the generation when any was.
    std::size_t releaseUnused(const std::set<std::string>& referenced);
    // The decoded size of one cached asset (0 when not cached).
    [[nodiscard]] std::size_t bytesOf(const std::string& path) const;

    // A file changed on disk. Returns true when a cached asset was replaced.
    bool reload(const std::filesystem::path& absolute);
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
    // Counts reload() calls for files of the game or library, cached or not
    // (LUTs, cooked textures), so GPU caches look at files only after one.
    [[nodiscard]] std::uint64_t fileChanges() const { return fileChanges_; }

private:
    using Decoded = std::variant<std::monostate, std::shared_ptr<const Model>,
                                 std::shared_ptr<const SpriteAnimation>, std::shared_ptr<const Image>>;
    static Decoded decode(const std::filesystem::path& file);

public:
    [[nodiscard]] std::filesystem::path file(const std::string& relative) const;

private:
    using Stamp = std::optional<std::pair<std::filesystem::file_time_type, std::uintmax_t>>;
    void refreshUserImage(const std::string& path);
    void touch(const std::string& path) { lastUse_[path] = ++useCounter_; }
    bool erase(const std::string& path);
    void worker();
    std::filesystem::path root_, builtinRoot_, userRoot_;
    std::map<std::string, Stamp> userStamps_;
    std::map<std::string, std::shared_ptr<const Model>> models_;
    std::map<std::string, std::shared_ptr<const SpriteAnimation>> sprites_;
    std::map<std::string, std::shared_ptr<const Image>> images_;
    std::map<std::string, std::uint64_t> lastUse_;
    std::uint64_t useCounter_ = 0;
    std::size_t budget_;
    std::map<std::string, Json> fontSpecs_;
    // The files each font reads: its faces and, for bitmap fonts, their pages.
    std::map<std::string, std::set<std::string>> fontFiles_;
    std::map<std::string, std::shared_ptr<FontSet>> fonts_;
    std::vector<std::shared_ptr<FontSet>> retiredFonts_;
    std::set<std::string> failed_;
    std::uint64_t generation_ = 1, fileChanges_ = 0;

    struct Ticket {
        std::size_t total = 0, done = 0;
    };
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<int, std::string>> queue_;
    std::vector<std::pair<std::string, Decoded>> finished_;
    std::atomic<bool> hasFinished_ = false;
    std::map<int, Ticket> tickets_;
    int nextTicket_ = 1;
    bool stopping_ = false;
    std::thread thread_;
};

// Decoded sizes, as the cache's budget counts them.
[[nodiscard]] std::size_t decodedBytes(const Image& image);
[[nodiscard]] std::size_t decodedBytes(const Model& model);
[[nodiscard]] std::size_t decodedBytes(const SpriteAnimation& sprite);
} // namespace fe
