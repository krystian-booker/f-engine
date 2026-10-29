#pragma once
#include <filesystem>
#include <string>

namespace fe {
// Locates lualib next to the executable: <exe dir>/lualib, an app bundle's
// Resources/lualib or <exe dir>/share/fengine/lualib. Development builds also
// search the folders above and fall back to the source tree; shipped builds
// look only in those places.
std::filesystem::path findLibraryRoot(const std::filesystem::path& executable);
// Runs <game>/tests/*.lua with fe.test; returns a process exit code. The
// script watchdog stops a test, or a tick within it, that runs longer than
// `watchdog` seconds.
int runTests(const std::filesystem::path& game, const std::filesystem::path& lualib,
             const std::string& filter, double watchdog = 3.0);
// Loads every map, sprite and model a game references and runs main.validate().
int checkGame(const std::filesystem::path& game, const std::filesystem::path& lualib);
// Writes fengine.d.lua (LuaLS stubs) and api.md into a folder.
int writeApi(const std::filesystem::path& lualib, const std::filesystem::path& folder);
// Shared command-line handling for --test/--check/--write-api; returns -1
// when none of them was requested.
int runHeadlessCommand(int argc, char** argv);
} // namespace fe
