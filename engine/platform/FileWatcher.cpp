#include "engine/platform/FileWatcher.h"

namespace fe {
FileWatcher::FileWatcher(std::filesystem::path root) : root_(std::move(root)), previous_(snapshot()) {}
FileWatcher::Snapshot FileWatcher::snapshot() const {
    Snapshot result;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root_,
        std::filesystem::directory_options::skip_permission_denied, ec), end;
    while (it != end) {
        if (it->is_regular_file(ec) && !ec) {
            const auto time = it->last_write_time(ec);
            if (!ec) {
                const auto size = it->file_size(ec);
                if (!ec) result[it->path()] = {time, size};
            }
        }
        it.increment(ec);
        if (ec) break;
    }
    return result;
}
std::vector<FileChange> FileWatcher::poll() {
    auto current = snapshot();
    std::vector<FileChange> changes;
    for (const auto& [path, stamp] : current) {
        const auto previous = previous_.find(path);
        if (previous == previous_.end()) changes.push_back({path, FileChangeKind::Added});
        else if (!(previous->second == stamp)) changes.push_back({path, FileChangeKind::Modified});
    }
    for (const auto& [path, stamp] : previous_)
        if (!current.contains(path)) changes.push_back({path, FileChangeKind::Removed});
    previous_ = std::move(current);
    return changes;
}
}
