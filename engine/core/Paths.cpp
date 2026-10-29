#include "engine/core/Paths.h"
#include "engine/core/Pack.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fe {
std::filesystem::path resolveInside(const std::filesystem::path& root, std::string_view relative) {
    if (relative.empty()) {
        throw std::runtime_error("Empty path");
    }
    // The file system would stop at the NUL, so "x.lua\0.png" names x.lua.
    if (relative.find('\0') != std::string_view::npos) {
        throw std::runtime_error("Path contains a NUL character: " +
                                 std::string(relative.substr(0, relative.find('\0'))) + "\\0...");
    }
    const auto base = std::filesystem::weakly_canonical(root);
    const auto candidate = std::filesystem::weakly_canonical(base / std::filesystem::path(relative));
    const auto inside = candidate.lexically_relative(base);
    if (inside.empty() || inside.is_absolute() || *inside.begin() == "..") {
        throw std::runtime_error("Path escapes the game folder: " + std::string(relative));
    }
    return candidate;
}

std::string relativeTo(const std::filesystem::path& root, const std::filesystem::path& file) {
    const auto inside =
        std::filesystem::weakly_canonical(file).lexically_relative(std::filesystem::weakly_canonical(root));
    if (inside.empty() || *inside.begin() == "..") {
        return {};
    }
    return inside.generic_string();
}

namespace {
struct Mount {
    // The folder the archive stands in for, as given and with links resolved.
    std::filesystem::path root, canonicalRoot;
    PackArchive archive;
    bool preferLoose = true;
};

// Extracted entries live in one private folder per process.
class Extracted {
public:
    ~Extracted() {
        if (!folder_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(folder_, error);
        }
    }
    std::filesystem::path file(const Mount& mount, const PackArchive::Entry& entry) {
        std::lock_guard lock(mutex_);
        const auto key = mount.archive.file().string() + '\n' + entry.name;
        if (const auto found = files_.find(key); found != files_.end()) {
            return found->second;
        }
        if (folder_.empty()) {
            create();
        }
        const auto target = folder_ / std::to_string(files_.size()) / std::filesystem::path(entry.name);
        std::filesystem::create_directories(target.parent_path());
        const auto bytes = mount.archive.read(entry);
        std::ofstream stream(target, std::ios::binary | std::ios::trunc);
        if (!stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            throw std::runtime_error("Cannot extract " + entry.name + " to " + target.string());
        }
        files_[key] = target;
        return target;
    }

private:
    void create() {
        const auto base = std::filesystem::temp_directory_path();
        std::random_device entropy;
        for (int attempt = 0; attempt < 16 && folder_.empty(); ++attempt) {
            const auto candidate = base / ("fengine-pak-" + std::to_string(entropy()));
            if (std::filesystem::create_directory(candidate)) {
                std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace);
                folder_ = candidate;
            }
        }
        if (folder_.empty()) {
            throw std::runtime_error("Cannot create an extraction folder in " + base.string());
        }
    }
    std::mutex mutex_;
    std::filesystem::path folder_;
    std::map<std::string, std::filesystem::path> files_;
};

std::shared_mutex mountsMutex;
std::vector<std::shared_ptr<const Mount>> mounts;
Extracted extracted;

// The mount whose folder holds `path`, with the entry name inside it ("" for
// the folder itself).
std::pair<std::shared_ptr<const Mount>, std::string> locate(const std::filesystem::path& path) {
    std::shared_lock lock(mountsMutex);
    if (mounts.empty()) {
        return {};
    }
    const auto normal = std::filesystem::absolute(path).lexically_normal();
    for (const auto& mount : mounts) {
        for (const auto* root : {&mount->root, &mount->canonicalRoot}) {
            const auto inside = normal.lexically_relative(*root);
            if (!inside.empty() && *inside.begin() != "..") {
                auto name = inside.generic_string();
                if (name.ends_with('/')) {
                    name.pop_back();
                }
                if (name == ".") {
                    name.clear();
                }
                return {mount, std::move(name)};
            }
        }
    }
    return {};
}

struct Packed {
    std::shared_ptr<const Mount> mount;
    const PackArchive::Entry* entry = nullptr;
};

// The archive entry `path` reads from; empty when it reads the loose file.
Packed packed(const std::filesystem::path& path) {
    auto [mount, name] = locate(path);
    if (!mount || name.empty()) {
        return {};
    }
    const auto* entry = mount->archive.find(name);
    std::error_code error;
    if (!entry || (mount->preferLoose && std::filesystem::is_regular_file(path, error))) {
        return {};
    }
    return {std::move(mount), entry};
}
} // namespace

void mountPack(const std::filesystem::path& archive, bool preferLoose) {
    const auto absolute = std::filesystem::absolute(archive).lexically_normal();
    auto mount = std::make_shared<Mount>(
        Mount{absolute.parent_path() / absolute.stem(),
              std::filesystem::weakly_canonical(absolute.parent_path()) / absolute.stem(),
              PackArchive(absolute), preferLoose});
    std::unique_lock lock(mountsMutex);
    std::erase_if(mounts, [&](const auto& existing) { return existing->root == mount->root; });
    mounts.push_back(std::move(mount));
}

bool mountFolderPack(const std::filesystem::path& folder) {
    auto archive = std::filesystem::absolute(folder).lexically_normal();
    if (!archive.has_filename()) {
        archive = archive.parent_path();
    }
    archive += ".pak";
    std::error_code error;
    if (!std::filesystem::is_regular_file(archive, error)) {
        return false;
    }
    {
        std::shared_lock lock(mountsMutex);
        const auto same = std::find_if(mounts.begin(), mounts.end(),
                                       [&](const auto& mount) { return mount->archive.file() == archive; });
        if (same != mounts.end()) {
            return true;
        }
    }
    mountPack(archive);
    return true;
}

std::filesystem::path openGame(const std::filesystem::path& folderOrArchive) {
    if (folderOrArchive.extension() == ".pak") {
        mountPack(folderOrArchive);
        const auto absolute = std::filesystem::absolute(folderOrArchive).lexically_normal();
        return absolute.parent_path() / absolute.stem();
    }
    mountFolderPack(folderOrArchive);
    return folderOrArchive;
}

void unmountPacks() {
    std::unique_lock lock(mountsMutex);
    mounts.clear();
}

bool isFile(const std::filesystem::path& path) {
    std::error_code error;
    return packed(path).entry || std::filesystem::is_regular_file(path, error);
}

bool isFolder(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return true;
    }
    const auto [mount, name] = locate(path);
    return mount && mount->archive.hasFolder(name);
}

std::string readFile(const std::filesystem::path& path) {
    if (const auto source = packed(path); source.entry) {
        return source.mount->archive.read(*source.entry);
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::vector<std::string> listFolder(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        names.push_back(entry.path().filename().string() + (entry.is_directory(error) ? "/" : ""));
    }
    if (const auto [mount, name] = locate(folder); mount) {
        const auto inside = mount->archive.list(name);
        names.insert(names.end(), inside.begin(), inside.end());
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::vector<std::string> listFilesBelow(const std::filesystem::path& folder) {
    std::vector<std::string> files;
    std::vector<std::string> pending = {""};
    while (!pending.empty()) {
        const auto prefix = std::move(pending.back());
        pending.pop_back();
        for (const auto& name : listFolder(prefix.empty() ? folder : folder / prefix)) {
            (name.ends_with('/') ? pending : files).push_back(prefix + name);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::filesystem::path localFile(const std::filesystem::path& path) {
    if (const auto source = packed(path); source.entry) {
        return extracted.file(*source.mount, *source.entry);
    }
    return path;
}
} // namespace fe
