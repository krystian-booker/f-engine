#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fe {
using Json = nlohmann::json;
inline Json readJson(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open " + path.string());
    try { return Json::parse(stream); }
    catch (const Json::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}
inline void writeJson(const std::filesystem::path& path, const Json& data) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << data.dump(2) << '\n';
        stream.flush();
        if (!stream) throw std::runtime_error("Cannot write " + temporary.string());
    }
    // Windows cannot rename over an existing file. Preserve the previous file
    // until the new one has been written in full.
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        auto backup = path; backup += ".bak";
        if (std::filesystem::exists(path)) std::filesystem::rename(path, backup);
        try { std::filesystem::rename(temporary, path); }
        catch (...) {
            if (std::filesystem::exists(backup)) std::filesystem::rename(backup, path);
            throw;
        }
        std::filesystem::remove(backup);
    }
}
}
