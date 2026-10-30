#include "engine/core/Json.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fe {
namespace {
#ifdef _WIN32
void writeDurably(const std::filesystem::path& path, const std::string& text) {
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        throw std::runtime_error("Cannot write " + path.string());
    }
}

void flushFolder(const std::filesystem::path&) {
    // MOVEFILE_WRITE_THROUGH already waits for the rename to reach the disk.
}

void moveOver(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace " + to.string());
    }
}

// `to` becomes `from`, and what `to` was becomes `backup`: ReplaceFileW does
// both in one step (and keeps `to`'s attributes); when it cannot (another
// volume, a locked file) the two renames run one after the other.
void moveOverKeeping(const std::filesystem::path& from, const std::filesystem::path& to,
                     const std::filesystem::path& backup) {
    if (ReplaceFileW(to.c_str(), from.c_str(), backup.c_str(), REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return;
    }
    if (!MoveFileExW(to.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot keep the previous " + to.string());
    }
    moveOver(from, to);
}
#else
void syncDescriptor(int descriptor) {
#ifdef __APPLE__
    // fsync only reaches the drive cache on macOS.
    if (fcntl(descriptor, F_FULLFSYNC) == 0) {
        return;
    }
#endif
    fsync(descriptor);
}

void writeDurably(const std::filesystem::path& path, const std::string& text) {
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        throw std::runtime_error("Cannot write " + path.string() + ": " + std::strerror(errno));
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const auto count = write(descriptor, text.data() + done, text.size() - done);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(descriptor);
            throw std::runtime_error("Cannot write " + path.string() + ": " + std::strerror(errno));
        }
        done += static_cast<std::size_t>(count);
    }
    syncDescriptor(descriptor);
    close(descriptor);
}

void flushFolder(const std::filesystem::path& folder) {
    const int descriptor = open(folder.empty() ? "." : folder.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor >= 0) {
        syncDescriptor(descriptor);
        close(descriptor);
    }
}

void moveOver(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::rename(from, to);
}

void moveOverKeeping(const std::filesystem::path& from, const std::filesystem::path& to,
                     const std::filesystem::path& backup) {
    std::filesystem::rename(to, backup); // replaces an older backup atomically
    moveOver(from, to);
}
#endif

// The current contents of `path`, for worthBackingUp.
std::string currentBytes(const std::filesystem::path& path) {
    std::ifstream previous(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(previous)), std::istreambuf_iterator<char>());
    if (!previous.good() && !previous.eof()) {
        throw std::runtime_error("Cannot read " + path.string() + " to back it up");
    }
    return bytes;
}
} // namespace

Json parseJson(std::string_view text) {
    int depth = 0;
    bool inString = false, escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            // The outermost container is level 0, as in luaToJson and jsonToLua.
            if (depth++ > maxJsonDepth) {
                throw std::runtime_error("JSON nests deeper than " + std::to_string(maxJsonDepth) +
                                         " levels");
            }
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return Json::parse(text);
}

int jsonDepth(const Json& value) {
    if (!value.is_structured()) {
        return -1;
    }
    // Iterative, so a value too deep to read back cannot exhaust the stack.
    int deepest = 0;
    std::vector<std::pair<const Json*, int>> pending{{&value, 0}};
    while (!pending.empty()) {
        const auto [json, level] = pending.back();
        pending.pop_back();
        deepest = std::max(deepest, level);
        if (deepest > maxJsonDepth) {
            break;
        }
        for (const auto& child : *json) {
            if (child.is_structured()) {
                pending.emplace_back(&child, level + 1);
            }
        }
    }
    return deepest;
}

Json readJson(const std::filesystem::path& path) {
    const auto text = readFile(path);
    try {
        return parseJson(text);
    } catch (const std::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}

std::filesystem::path backupPath(const std::filesystem::path& path) {
    auto backup = path;
    backup += ".bak";
    return backup;
}

Json readJsonOrBackup(const std::filesystem::path& path) {
    const auto backup = backupPath(path);
    try {
        return readJson(path);
    } catch (const std::exception& error) {
        if (!std::filesystem::is_regular_file(backup)) {
            throw;
        }
        log(LogLevel::Warning, std::string(error.what()) + "; using the backup " + backup.string());
        return readJson(backup);
    }
}

void replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& path) {
    moveOver(temporary, path);
    flushFolder(path.parent_path());
}

void writeTextAtomic(const std::filesystem::path& path, const std::string& text, bool keepBackup) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += ".tmp";
    writeDurably(temporary, text); // the one data fsync
    // The previous version is renamed aside, never copied: between its rename
    // and the temporary's, `path` is missing and `.bak` holds the last good
    // version, which readJsonOrBackup and SaveStore read in its place.
    if (keepBackup && std::filesystem::is_regular_file(path) && worthBackingUp(path, currentBytes(path))) {
        moveOverKeeping(temporary, path, backupPath(path));
    } else {
        moveOver(temporary, path); // a blank or truncated file is replaced, not kept
    }
    flushFolder(path.parent_path()); // the one folder fsync covers both renames
}

bool worthBackingUp(const std::filesystem::path& path, const std::string& bytes) {
    if (bytes.find_first_not_of(" \t\r\n") == std::string::npos) {
        return false;
    }
    if (path.extension() != ".json") {
        return true;
    }
    try {
        (void)parseJson(bytes);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void writeJson(const std::filesystem::path& path, const Json& data, bool keepBackup) {
    if (jsonDepth(data) > maxJsonDepth) {
        throw std::runtime_error("Cannot write " + path.string() + ": its JSON would nest deeper than " +
                                 std::to_string(maxJsonDepth) + " levels and could not be read back");
    }
    writeTextAtomic(path, data.dump(2) + "\n", keepBackup);
}
} // namespace fe
