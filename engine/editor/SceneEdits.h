#pragma once
#include "engine/editor/AssetCatalog.h"
#include "engine/editor/Selection.h"
#include "engine/nav/Grid.h"
#include <string>
#include <vector>

namespace fe {
class World;
struct Model;

// Grid snapping: the cell under a position and that cell's centre at the
// grid's height there (what "sits on the grid" means for placements and points).
[[nodiscard]] GridPos snapToCell(const Grid& grid, glm::vec3 position, int layer);
[[nodiscard]] glm::vec3 cellCentre(const Grid& grid, GridPos cell);
// `position` moved inside the grid's rectangle (x and z; y is kept): where a
// map's points must lie, or the map no longer loads (MapData::fromJson).
[[nodiscard]] glm::vec3 clampToGrid(const Grid& grid, glm::vec3 position);
// A position snapped to a step (0.5, 0.25) on every axis.
[[nodiscard]] glm::vec3 snapToStep(glm::vec3 position, float step);
// The cells per side a placed model covers (at least 1 each), from its box at
// the placement's yaw and scale.
[[nodiscard]] glm::ivec2 footprintCells(const Model& model, const Placement& placement);
// Where a model's origin goes for a drop or a drag at `point`: on each axis a
// footprint of an even number of cells snaps to a cell corner and an odd one
// to a cell centre, so whole-cell footprints line up with the grid; y is the
// grid's height there.
[[nodiscard]] glm::vec3 snapForFootprint(const Grid& grid, glm::vec3 point, glm::ivec2 footprint, int layer);

// `prefix` followed by the first number no placement uses as an id ("chest",
// "chest2"...); the prefix itself when free.
[[nodiscard]] std::string uniquePlacementId(const std::string& prefix,
                                            const std::vector<Placement>& placements);
// The placement to make when an asset is dropped on a cell: a sprite becomes
// the game's default placement type (`defaultType`, from the game's editor.describe hook) with
// `props.sprite`, a model a "prop" with `props.model`. A model given with the
// drop `point` snaps by its footprint (snapForFootprint); otherwise the
// placement stands at the cell's centre. Nullopt for assets that cannot stand
// in a scene (audio, scripts).
[[nodiscard]] std::optional<Placement>
placementForAsset(AssetKind kind, const std::string& path, GridPos cell, const Grid& grid,
                  const std::string& defaultType, const std::vector<Placement>& existing,
                  const Model* model = nullptr, std::optional<glm::vec3> point = std::nullopt);
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
