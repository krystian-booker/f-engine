#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fe {
using Json = nlohmann::json;
Json readJson(const std::filesystem::path& path);
// Writes through a temporary file so a crash never leaves a truncated file.
void writeJson(const std::filesystem::path& path, const Json& data);
void writeTextAtomic(const std::filesystem::path& path, const std::string& text);
} // namespace fe
