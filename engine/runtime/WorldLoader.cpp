#include "engine/runtime/WorldLoader.h"
#include "engine/core/AssetCache.h"
#include <chrono>
#include <set>
#include <stdexcept>

namespace fe {
int WorldLoader::begin(const std::string& name, const std::string& mapId,
                       const std::filesystem::path& mapFile) {
    Pending load;
    load.name = name;
    load.mapId = mapId;
    load.parse = std::async(std::launch::async, [file = mapFile] { return MapData::load(file); });
    const int ticket = next_++;
    loads_.emplace(ticket, std::move(load));
    return ticket;
}

void WorldLoader::advanceAll() {
    for (auto& [ticket, load] : loads_) {
        advance(load);
    }
    // Cancelled parses are released once their worker is done (waiting on
    // the future here would stall the tick).
    std::erase_if(discarded_, [](std::future<MapData>& parse) {
        if (parse.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return false;
        }
        try {
            static_cast<void>(parse.get());
        } catch (const std::exception&) {}
        return true;
    });
}

void WorldLoader::cancel(int ticket) {
    const auto found = loads_.find(ticket);
    if (found == loads_.end()) {
        throw std::runtime_error("Unknown world load ticket " + std::to_string(ticket));
    }
    if (found->second.stage == Pending::Stage::Parsing) {
        discarded_.push_back(std::move(found->second.parse));
    }
    loads_.erase(found);
}

// Parsing, then the map's models, images and sprite animations, then the
// atlases those animations name.
void WorldLoader::advance(Pending& load) {
    using Stage = Pending::Stage;
    if (load.stage == Stage::Parsing) {
        if (load.parse.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        try {
            load.map = load.parse.get();
        } catch (const std::exception& error) {
            load.error = error.what();
            load.stage = Stage::Failed;
            return;
        }
        const auto& environment = load.map->environment;
        std::set<std::string> paths{"maps/" + load.map->id + "/" + load.map->geometry};
        std::set<std::string> sprites;
        for (const auto& path :
             {environment.sky.panorama, environment.post.lut, environment.post.lutBlendTarget}) {
            if (!path.empty()) {
                paths.insert(path);
            }
        }
        for (const auto& backdrop : environment.sky.backdrops) {
            if (!backdrop.image.empty()) {
                paths.insert(backdrop.image);
            }
        }
        for (const auto& emitter : load.map->emitters) {
            if (!emitter.settings.sprite.empty()) {
                sprites.insert(emitter.settings.sprite);
            }
        }
        if (!environment.weather.sprite.empty()) {
            sprites.insert(environment.weather.sprite);
        }
        paths.insert(sprites.begin(), sprites.end());
        load.sprites.assign(sprites.begin(), sprites.end());
        load.assetCount = paths.size();
        load.ticket = assets_.preload({paths.begin(), paths.end()});
        load.stage = Stage::Assets;
    }
    if (load.stage == Stage::Assets) {
        if (assets_.progress(load.ticket) < 1) {
            return;
        }
        std::vector<std::string> atlases;
        for (const auto& path : load.sprites) {
            if (const auto sprite = assets_.trySprite(path)) {
                atlases.push_back(
                    (std::filesystem::path(path).parent_path() / sprite->atlas).generic_string());
            }
        }
        load.ticket = assets_.preload(atlases);
        load.stage = Stage::Atlases;
    }
    if (load.stage == Stage::Atlases && assets_.progress(load.ticket) >= 1) {
        load.stage = Stage::Ready;
    }
}

float WorldLoader::progress(int ticket) {
    using Stage = Pending::Stage;
    const auto found = loads_.find(ticket);
    if (found == loads_.end()) {
        throw std::runtime_error("Unknown world load ticket " + std::to_string(ticket));
    }
    auto& load = found->second;
    advance(load);
    const auto total = static_cast<float>(1 + load.assetCount + load.sprites.size());
    switch (load.stage) {
    case Stage::Parsing:
        return 0;
    case Stage::Assets:
        return (1 + assets_.progress(load.ticket) * static_cast<float>(load.assetCount)) / total;
    case Stage::Atlases:
        return (1 + static_cast<float>(load.assetCount) +
                assets_.progress(load.ticket) * static_cast<float>(load.sprites.size())) /
               total;
    case Stage::Ready:
    case Stage::Failed:
        break;
    }
    return 1;
}

WorldLoader::Finished WorldLoader::finish(int ticket) {
    const auto found = loads_.find(ticket);
    if (found == loads_.end()) {
        throw std::runtime_error("Unknown world load ticket " + std::to_string(ticket));
    }
    auto load = std::move(found->second);
    loads_.erase(found);
    if (load.stage == Pending::Stage::Parsing) {
        load.parse.wait();
        advance(load);
    }
    if (load.stage == Pending::Stage::Failed) {
        throw std::runtime_error(load.error);
    }
    assets_.collect();
    return {std::move(load.name), std::move(load.mapId), std::move(*load.map)};
}
} // namespace fe
