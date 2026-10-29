#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fe {
using Json = nlohmann::json;
// Arrays and objects nest at most this deep in JSON the engine reads, and in
// Lua values converted to or from JSON; deeper values raise an error instead
// of exhausting the stack in code that walks them recursively.
inline constexpr int maxJsonDepth = 128;
// Json::parse after checking the nesting depth.
Json parseJson(std::string_view text);
// The nesting level of the deepest array or object in `value`, counting the
// outermost container as level 0 (as parseJson, luaToJson and jsonToLua do);
// -1 for a plain value. Stops counting past maxJsonDepth + 1.
[[nodiscard]] int jsonDepth(const Json& value);
Json readJson(const std::filesystem::path& path);
// Reads `path`, falling back to the `<path>.bak` copy that writes with
// `keepBackup` leave behind when the file is missing or unreadable.
Json readJsonOrBackup(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path backupPath(const std::filesystem::path& path);

// Durable atomic replacement: the data (any bytes) goes to `<path>.tmp`, is
// flushed to disk, then renamed over `path` (and the folder entry flushed), so
// a crash leaves either the old or the new file, never a truncated one. With
// `keepBackup` the previous version is first copied to `<path>.bak`, which is
// flushed to disk the same way before the rename.
void writeTextAtomic(const std::filesystem::path& path, const std::string& text, bool keepBackup = false);
// Raises instead of writing JSON nested deeper than parseJson reads back.
void writeJson(const std::filesystem::path& path, const Json& data, bool keepBackup = false);
// Moves a finished temporary file over `path` with the same guarantees.
void replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& path);
} // namespace fe
