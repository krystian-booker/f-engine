#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
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
// process (one game per process) and are keyed by archive: mounting the same
// archive again replaces its mount. Several archives may stand in for one
// folder; the one mounted last wins where their entries share a path.
void mountPack(const std::filesystem::path& archive, bool preferLoose = FENGINE_DEV != 0);
// Mounts `<folder>.pak` when it exists; returns whether it does.
bool mountFolderPack(const std::filesystem::path& folder);
// Lays `archive` over `root` (a game folder, loose or itself supplied by an
// archive): its entries win over the folder's files and earlier mounts, also
// over loose files in development builds, so a content pack can be tested
// against an unpacked game. The engine mounts <game>/packs/*.pak and <user
// data>/packs/*.pak this way.
void mountOverlay(const std::filesystem::path& archive, const std::filesystem::path& root);
// mountOverlay for every `*.pak` in `folder`, in name order; an archive that
// does not open (damaged, not a pak) is logged and skipped. Returns the file
// names mounted, in mount order; none when the folder does not exist.
std::vector<std::string> mountOverlayFolder(const std::filesystem::path& folder,
                                            const std::filesystem::path& root);
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
// Where a file's bytes lie when it is an archive entry stored as it is (not
// compressed): the archive file, and the offset and size of the entry in it.
// Nullopt for a loose file, a compressed entry and a missing file. A reader
// that takes a window of the archive needs no copy of the entry. Thread safe.
struct StoredSpan {
    std::filesystem::path archive;
    std::uint64_t offset = 0, size = 0;
};
[[nodiscard]] std::optional<StoredSpan> storedSpan(const std::filesystem::path& path);
// Whether the file is read from an archive entry, stored or compressed.
[[nodiscard]] bool isPacked(const std::filesystem::path& path);
// A path on disk holding the file, for libraries that only open files by
// name: the loose file, or the archive entry extracted once per process into
// a private temporary folder removed at exit.
[[nodiscard]] std::filesystem::path localFile(const std::filesystem::path& path);
} // namespace fe
