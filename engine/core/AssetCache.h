#pragma once
#include "engine/assets/Image.h"
#include "engine/assets/Model.h"
#include "engine/assets/SpriteAnimation.h"
#include "engine/text/Font.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <variant>

namespace fe {
// CPU-side asset cache shared by the world, scripts, the canvas and the
// renderer. Keys are game-relative paths. A worker thread decodes preload
// requests; everything else loads on first use. The generation number
// increases on every reload so GPU caches know to rebuild.
class AssetCache {
public:
    // Paths beginning with "@fe/" resolve inside lualib/fe (shared fonts and
    // UI art); all others inside the game folder.
    AssetCache(std::filesystem::path root, std::filesystem::path libraryRoot);
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    // These throw on failure; `tryX` variants log once and return null.
    std::shared_ptr<const Model> model(const std::string& path);
    std::shared_ptr<const SpriteAnimation> sprite(const std::string& path);
    std::shared_ptr<const Image> image(const std::string& path);
    std::shared_ptr<const Model> tryModel(const std::string& path);
    std::shared_ptr<const SpriteAnimation> trySprite(const std::string& path);
    std::shared_ptr<const Image> tryImage(const std::string& path);

    // Fonts are named sets of faces, e.g. {"files":["ui/font.fnt","ui/noto.ttf"],"size":12}.
    void defineFont(const std::string& name, const Json& spec);
    std::shared_ptr<FontSet> font(const std::string& name);
    [[nodiscard]] bool hasFont(const std::string& name) const { return fonts_.contains(name); }

    // Starts background decoding; returns a ticket for progress().
    int preload(const std::vector<std::string>& paths);
    [[nodiscard]] float progress(int ticket);
    // Moves finished background results into the cache (main thread).
    void collect();

    // A file changed on disk. Returns true when a cached asset was replaced.
    bool reload(const std::filesystem::path& absolute);
    [[nodiscard]] std::uint64_t generation() const { return generation_; }

private:
    using Decoded = std::variant<std::monostate, std::shared_ptr<const Model>,
                                 std::shared_ptr<const SpriteAnimation>, std::shared_ptr<const Image>>;
    static Decoded decode(const std::filesystem::path& file);

public:
    [[nodiscard]] std::filesystem::path file(const std::string& relative) const;

private:
    void worker();
    std::filesystem::path root_, libraryRoot_;
    std::map<std::string, std::shared_ptr<const Model>> models_;
    std::map<std::string, std::shared_ptr<const SpriteAnimation>> sprites_;
    std::map<std::string, std::shared_ptr<const Image>> images_;
    std::map<std::string, Json> fontSpecs_;
    std::map<std::string, std::shared_ptr<FontSet>> fonts_;
    std::set<std::string> failed_;
    std::uint64_t generation_ = 1;

    struct Ticket {
        std::size_t total = 0, done = 0;
    };
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<int, std::string>> queue_;
    std::vector<std::pair<std::string, Decoded>> finished_;
    std::map<int, Ticket> tickets_;
    int nextTicket_ = 1;
    bool stopping_ = false;
    std::thread thread_;
};
} // namespace fe
