#pragma once
#include <filesystem>
#include <string>

namespace fe {
// Locates lualib next to the executable (installed) or in the source tree.
std::filesystem::path findLibraryRoot(const std::filesystem::path& executable);
// Runs <game>/tests/*.lua with fe.test; returns a process exit code.
int runTests(const std::filesystem::path& game, const std::filesystem::path& lualib,
             const std::string& filter);
// Loads every map, sprite and model a game references and runs main.validate().
int checkGame(const std::filesystem::path& game, const std::filesystem::path& lualib);
// Writes fengine.d.lua (LuaLS stubs) and api.md into a folder.
int writeApi(const std::filesystem::path& lualib, const std::filesystem::path& folder);
// Shared command-line handling for --test/--check/--write-api; returns -1
// when none of them was requested.
int runHeadlessCommand(int argc, char** argv);
} // namespace fe
