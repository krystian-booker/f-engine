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
// its centre and the engine's default environment.
[[nodiscard]] MapData makeBlankMap(const std::string& id, const std::string& name, int width, int depth);
// Writes maps/<id>/map.json (validated first; the folder is created). With
// `geometry`, that .glb/.gltf file is copied in as the map's geometry;
// without one, a map whose geometry file is missing gets an empty glTF
// scene (`geometry.gltf`) so the engine has something to load.
void writeMap(const std::filesystem::path& gameRoot, const MapData& map, const std::filesystem::path& geometry = {});
// Copies a map folder to a new id (map.json gets the new id; geometry and
// everything else in the folder come along).
void copyMap(const std::filesystem::path& gameRoot, std::string_view from, const std::string& to,
             const std::string& newName = {});
// Moves a map folder to a new id, rewriting the id inside map.json.
void renameMap(const std::filesystem::path& gameRoot, std::string_view from, const std::string& to);
// Removes a map folder and everything in it.
void deleteMap(const std::filesystem::path& gameRoot, std::string_view id);
} // namespace fe
