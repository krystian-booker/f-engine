#pragma once
#include <filesystem>
#include <map>
#include <vector>

namespace fe {
enum class FileChangeKind { Added, Modified, Removed };
struct FileChange {
    std::filesystem::path path;
    FileChangeKind kind;
};
// Portable polling watcher. Called on the main thread at a low frequency;
// consumers perform transactional reloads so a bad edit keeps old data alive.
// With `entriesPerPoll` > 0 each poll() stats at most about that many
// directory entries and carries the walk over to the next call, so a large
// tree costs a bounded slice of each tick; changes are reported once a whole
// walk has finished. 0 walks the whole tree in every poll().
class FileWatcher {
public:
    explicit FileWatcher(std::filesystem::path root, std::size_t entriesPerPoll = 0);
    std::vector<FileChange> poll();

private:
    struct Stamp {
        std::filesystem::file_time_type time;
        std::uintmax_t size;
        bool operator==(const Stamp&) const = default;
    };
    using Snapshot = std::map<std::filesystem::path, Stamp>;
    // Walks pending directories into building_; false until the walk is complete.
    bool walk(std::size_t budget);
    void keepPrevious(const std::filesystem::path& under);
    std::filesystem::path root_;
    std::size_t entriesPerPoll_;
    Snapshot previous_, building_;
    std::vector<std::filesystem::path> pending_;
};
} // namespace fe
