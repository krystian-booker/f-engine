#include "engine/core/Json.h"
#include <fstream>
#include <stdexcept>

namespace fe {
Json readJson(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("Cannot open " + path.string());
    }
    try {
        return Json::parse(stream);
    } catch (const Json::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}

void writeTextAtomic(const std::filesystem::path& path, const std::string& text) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc | std::ios::binary);
        stream << text;
        stream.flush();
        if (!stream) {
            throw std::runtime_error("Cannot write " + temporary.string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (!ec) {
        return;
    }
    // Windows cannot rename over an existing file: keep a backup until the
    // replacement is in place.
    auto backup = path;
    backup += ".bak";
    if (std::filesystem::exists(path)) {
        std::filesystem::rename(path, backup);
    }
    try {
        std::filesystem::rename(temporary, path);
    } catch (...) {
        if (std::filesystem::exists(backup)) {
            std::filesystem::rename(backup, path);
        }
        throw;
    }
    std::filesystem::remove(backup);
}

void writeJson(const std::filesystem::path& path, const Json& data) {
    writeTextAtomic(path, data.dump(2) + "\n");
}
} // namespace fe
