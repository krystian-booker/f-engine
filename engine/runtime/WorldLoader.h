#pragma once
#include "engine/world/MapData.h"
#include <filesystem>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fe {
class AssetCache;

// Background world loading for Engine::loadWorldAsync: the map file is parsed
// on a worker thread, then the models, images and sprite animations it names
// decode through AssetCache's worker, then the atlases those animations name.
// Tickets advance on advanceAll() (every tick) and whenever progress is read.
class WorldLoader {
public:
    struct Finished {
        std::string name, mapId;
        MapData map;
    };
    explicit WorldLoader(AssetCache& assets) : assets_(assets) {}
    int begin(const std::string& name, const std::string& mapId, const std::filesystem::path& mapFile);
    void advanceAll();
    // 0..1; 1 once finish will not wait (or the load failed). Throws for an
    // unknown ticket.
    [[nodiscard]] float progress(int ticket);
    // Waits for the map parse if needed; assets still decoding load on first
    // use. Throws the load's error. The ticket is consumed.
    Finished finish(int ticket);
    // Drops a pending load: a worker still parsing its map finishes in the
    // background and its result is discarded; assets already queued stay in
    // the cache like any other. Throws for an unknown (or finished) ticket.
    void cancel(int ticket);

private:
    struct Pending {
        enum class Stage { Parsing, Assets, Atlases, Ready, Failed };
        std::string name, mapId;
        std::future<MapData> parse;
        std::optional<MapData> map;
        std::string error;
        Stage stage = Stage::Parsing;
        int ticket = 0;
        std::vector<std::string> sprites;
        std::size_t assetCount = 0;
    };
    void advance(Pending& load);
    AssetCache& assets_;
    std::map<int, Pending> loads_;
    std::vector<std::future<MapData>> discarded_; // cancelled parses still running
    int next_ = 1;
};
} // namespace fe
