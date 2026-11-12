#pragma once
#include <compare>
#include <optional>
#include <string>
#include <vector>

namespace fe {
struct MapData;

// An asset the renderer will draw, and as what: the same file uploads
// differently as map geometry (baked, merged, chunked) and as an entity's
// mesh, and an image differently as a backdrop and as a palette. The engine
// lists these for worlds still loading and for preloaded assets, and the
// runtime uploads them a little each frame, so the first frame that draws
// them finds them on the GPU.
struct WarmAsset {
    enum class Kind {
        Geometry, // a map's model
        Mesh,     // an entity's model
        Sprite,   // a sprite animation with its atlas (and normal and emissive images)
        Scenery,  // a backdrop image (mipmapped)
        Panorama, // a sky panorama
        Lut,      // a colour grading .cube
    };
    Kind kind = Kind::Mesh;
    std::string path;
    auto operator<=>(const WarmAsset&) const = default;
};

// What drawing a world of `map` needs before any entity is spawned: its
// geometry, sky, colour grading and the sprites of its emitters and weather.
void mapWarmAssets(const MapData& map, std::vector<WarmAsset>& out);

// How a preloaded path is drawn when its file type says so: a model is an
// entity's mesh, a sprite animation a sprite. An image has many uses (and is
// cheap to upload), so it has none here.
[[nodiscard]] std::optional<WarmAsset> preloadWarmAsset(const std::string& path);
} // namespace fe
