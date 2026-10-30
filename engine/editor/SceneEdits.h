#pragma once
#include "engine/editor/AssetCatalog.h"
#include "engine/editor/Selection.h"
#include "engine/nav/Grid.h"
#include <string>
#include <vector>

namespace fe {
class World;

// Grid snapping: the cell under a position and that cell's centre at the
// grid's height there (what "sits on the grid" means for placements and points).
[[nodiscard]] GridPos snapToCell(const Grid& grid, glm::vec3 position, int layer);
[[nodiscard]] glm::vec3 cellCentre(const Grid& grid, GridPos cell);
// A position snapped to a step (0.5, 0.25) on every axis.
[[nodiscard]] glm::vec3 snapToStep(glm::vec3 position, float step);

// `prefix` followed by the first number no placement uses as an id ("chest",
// "chest2"...); the prefix itself when free.
[[nodiscard]] std::string uniquePlacementId(const std::string& prefix, const std::vector<Placement>& placements);
// The placement to make when an asset is dropped on a cell: a sprite becomes
// the game's default placement type (`defaultType`, from fe.devtools) with
// `props.sprite`, a model a "prop" with `props.model`. Nullopt for assets
// that cannot stand in a scene (audio, scripts).
[[nodiscard]] std::optional<Placement> placementForAsset(AssetKind kind, const std::string& path, GridPos cell,
                                                         const Grid& grid, const std::string& defaultType,
                                                         const std::vector<Placement>& existing);
// A copy of a placement beside the original with a fresh id.
[[nodiscard]] Placement duplicatePlacement(const Placement& source, const std::vector<Placement>& existing);

// Moves a placement's live entity to where the authored placement now is
// (position, layer, facing, yaw, scale) without respawning it; a no-op when
// no entity has the placement's name. Returns whether one was moved.
bool syncPlacementEntity(World& world, const Placement& placement);
// Ids of placements that differ between two maps (changed, added or removed),
// so each can be respawned once after an undo or redo.
[[nodiscard]] std::vector<std::string> changedPlacementIds(const MapData& before, const MapData& after);
} // namespace fe
