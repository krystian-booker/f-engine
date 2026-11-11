#pragma once
#include <filesystem>
#include <string>

namespace fe {
// The running executable's absolute path, asked of the operating system
// (argv[0] may be relative to a working directory elsewhere, a symlink or a
// bare name found through PATH); `fallback` when the system cannot say.
std::filesystem::path executablePath(const std::filesystem::path& fallback);
// Locates builtin next to the executable: <exe dir>/builtin, an app bundle's
// Resources/builtin or <exe dir>/share/fengine/builtin. Development builds also
// search the folders above and fall back to the source tree; shipped builds
// look only in those places.
std::filesystem::path findBuiltinRoot(const std::filesystem::path& executable);
// Runs <game>/tests/*.lua with the built-in test runner; returns a process exit code. The
// script watchdog stops a test, or a tick within it, that runs longer than
// `watchdog` seconds.
int runTests(const std::filesystem::path& game, const std::filesystem::path& builtin,
             const std::string& filter, double watchdog = 3.0);
// Loads every map, sprite and model a game references and runs main.validate().
int checkGame(const std::filesystem::path& game, const std::filesystem::path& builtin);
// Bakes every map's walkability from its geometry and placed models and
// writes the maps back (fengine_headless --bake <game>); with `checkOnly`
// nothing is written and a map whose bake is out of date fails the run.
int bakeGame(const std::filesystem::path& game, const std::filesystem::path& builtin, bool checkOnly);
// Writes fengine.d.lua (LuaLS stubs) and api.md into a folder.
int writeApi(const std::filesystem::path& builtin, const std::filesystem::path& folder);
// Shared command-line handling for --test/--check/--bake/--write-api; returns -1
// when none of them was requested.
int runHeadlessCommand(int argc, char** argv);
} // namespace fe
