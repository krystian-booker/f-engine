#pragma once
#include <filesystem>
#include <string_view>

namespace fe {
// Resolves a game-relative path and rejects anything that escapes the root.
// Every file a script names goes through here.
std::filesystem::path resolveInside(const std::filesystem::path& root, std::string_view relative);
// Returns the root-relative generic path ("maps/town/map.json") or empty when
// the file lies outside the root.
std::string relativeTo(const std::filesystem::path& root, const std::filesystem::path& file);
} // namespace fe
