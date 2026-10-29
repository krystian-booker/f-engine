#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
// Resolves a game-relative path and rejects anything that escapes the root
// or contains a NUL character. Every file a script names goes through here.
std::filesystem::path resolveInside(const std::filesystem::path& root, std::string_view relative);
// Returns the root-relative generic path ("maps/town/map.json") or empty when
// the file lies outside the root.
std::string relativeTo(const std::filesystem::path& root, const std::filesystem::path& file);

// Packed folders: `<dir>/<name>.pak` (see engine/core/Pack.h) stands in for
// the folder `<dir>/<name>`. Once it is mounted, the functions below see its
// entries as files under that folder. Where both exist, development builds
// read the loose file and ship builds the archive's copy. Mounts last for the
// process (one game per process).
void mountPack(const std::filesystem::path& archive, bool preferLoose = FENGINE_DEV != 0);
// Mounts `<folder>.pak` when it exists; returns whether it does.
bool mountFolderPack(const std::filesystem::path& folder);
// The game folder a command line names: a folder, which `<folder>.pak` may
// supply, or the archive itself. Mounts the archive.
std::filesystem::path openGame(const std::filesystem::path& folderOrArchive);
void unmountPacks();

// Loose files and mounted archives alike.
[[nodiscard]] bool isFile(const std::filesystem::path& path);
[[nodiscard]] bool isFolder(const std::filesystem::path& path);
// The file's bytes; throws when it is missing or unreadable.
[[nodiscard]] std::string readFile(const std::filesystem::path& path);
// Sorted names inside a folder; sub-folders end with '/'.
[[nodiscard]] std::vector<std::string> listFolder(const std::filesystem::path& folder);
// Sorted folder-relative generic paths of every file below `folder`.
[[nodiscard]] std::vector<std::string> listFilesBelow(const std::filesystem::path& folder);
// A path on disk holding the file, for libraries that only open files by
// name: the loose file, or the archive entry extracted once per process into
// a private temporary folder removed at exit.
[[nodiscard]] std::filesystem::path localFile(const std::filesystem::path& path);
} // namespace fe
