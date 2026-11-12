#include "engine/runtime/WarmAssets.h"
#include "engine/world/MapData.h"
#include <set>

namespace fe {
void mapWarmAssets(const MapData& map, std::vector<WarmAsset>& out) {
    using Kind = WarmAsset::Kind;
    std::set<WarmAsset> seen;
    const auto add = [&](Kind kind, const std::string& path) {
        if (!path.empty() && seen.insert({kind, path}).second) {
            out.push_back({kind, path});
        }
    };
    add(Kind::Geometry, "maps/" + map.id + "/" + map.geometry);
    const auto& environment = map.environment;
    add(Kind::Panorama, environment.sky.panorama);
    for (const auto& backdrop : environment.sky.backdrops) {
        add(Kind::Scenery, backdrop.image);
    }
    add(Kind::Lut, environment.post.lut);
    add(Kind::Lut, environment.post.lutBlendTarget);
    add(Kind::Sprite, environment.weather.sprite);
    for (const auto& emitter : map.emitters) {
        add(Kind::Sprite, emitter.settings.sprite);
    }
}

std::optional<WarmAsset> preloadWarmAsset(const std::string& path) {
    if (path.ends_with(".glb") || path.ends_with(".gltf")) {
        return WarmAsset{WarmAsset::Kind::Mesh, path};
    }
    if (path.ends_with(".anim.json")) {
        return WarmAsset{WarmAsset::Kind::Sprite, path};
    }
    return std::nullopt;
}
} // namespace fe
