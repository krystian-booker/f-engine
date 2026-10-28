#pragma once
#include <filesystem>
#include <map>
#include <vector>

namespace fe {
enum class FileChangeKind { Added, Modified, Removed };
struct FileChange { std::filesystem::path path; FileChangeKind kind; };
// Portable polling watcher. Called on the main thread at a low frequency;
// consumers perform transactional reloads so a bad edit keeps old data alive.
class FileWatcher {
public:
    explicit FileWatcher(std::filesystem::path root);
    std::vector<FileChange> poll();
private:
    struct Stamp {
        std::filesystem::file_time_type time;
        std::uintmax_t size;
        bool operator==(const Stamp&) const = default;
    };
    using Snapshot = std::map<std::filesystem::path, Stamp>;
    Snapshot snapshot() const;
    std::filesystem::path root_;
    Snapshot previous_;
};
}
