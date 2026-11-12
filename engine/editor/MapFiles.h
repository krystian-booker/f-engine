#pragma once
#include "engine/world/MapData.h"
#include <filesystem>
#include <string>
#include <vector>

namespace fe {
// Map files as the editor creates, copies, renames and removes them. UI-free.
// Every function throws std::runtime_error with a message for the status
// line when the id is invalid, the map exists (or does not), or a file
// operation fails.

// A map id names its folder: 1 to 64 of a-z, 0-9, '_' and '-'.
[[nodiscard]] bool validMapId(std::string_view id);
[[nodiscard]] std::filesystem::path mapFolder(const std::filesystem::path& gameRoot, std::string_view id);
[[nodiscard]] bool mapExists(const std::filesystem::path& gameRoot, std::string_view id);

// A flat, fully walkable map of width x depth cells with a `start` point at
// its centre, the engine's default environment and a bake section (auto
// bake on, never baked yet), so the editor derives its walkability.
[[nodiscard]] MapData makeBlankMap(const std::string& id, const std::string& name, int width, int depth);
// A glTF scene holding one flat ground quad from (0, 0) to (width, depth):
// what a new map stands on until a Blender export replaces it.
[[nodiscard]] Json groundQuadGltf(float width, float depth);
// Writes maps/<id>/map.json (validated first; the folder is created). With
// `geometry`, that .glb/.gltf file is copied in as the map's geometry;
// without one, a map whose geometry file is missing gets a flat ground quad
// the size of the grid (`geometry.gltf`), which the bake reads as walkable.
void writeMap(const std::filesystem::path& gameRoot, const MapData& map,
              const std::filesystem::path& geometry = {});
// Copies a map folder to a new id (map.json gets the new id; geometry and
// everything else in the folder come along).
void copyMap(const std::filesystem::path& gameRoot, std::string_view from, const std::string& to,
             const std::string& newName = {});
// Moves a map folder to a new id, rewriting the id inside map.json.
void renameMap(const std::filesystem::path& gameRoot, std::string_view from, const std::string& to);
// Removes a map folder and everything in it.
void deleteMap(const std::filesystem::path& gameRoot, std::string_view id);
} // namespace fe
