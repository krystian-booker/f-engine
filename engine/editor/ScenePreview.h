#pragma once
#include <memory>
#include <string>

namespace fe {
class Engine;
class World;

// The world an editor loads a map into when no game code is running it.
inline constexpr const char* PreviewWorldName = "editor_preview";

// (Re)creates the stand-in entity of the authored placement `id`: a mesh or
// a sprite for a placement whose props name a "model" or a "sprite", nothing
// for any other (its handle still marks the spot). Returns what went wrong,
// empty when nothing did.
std::string spawnPlacementPreview(World& world, const std::string& id);

// Loads map `mapId` into the preview world (replacing the one loaded) and
// stands entities in for its placements, so a scene can be laid out before
// any game code exists. Throws what loading the map throws.
std::shared_ptr<World> openPreviewWorld(Engine& engine, const std::string& mapId);
} // namespace fe
