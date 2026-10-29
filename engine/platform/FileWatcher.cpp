#include "engine/platform/FileWatcher.h"

namespace fe {
FileWatcher::FileWatcher(std::filesystem::path root) : root_(std::move(root)) {
    previous_ = snapshot();
}

// Walks the tree one directory at a time. Anything that cannot be read this
// time (a directory being replaced, a file mid-save) keeps its previous
// entries, so a transient error never looks like files being removed and
// added again.
FileWatcher::Snapshot FileWatcher::snapshot() const {
    Snapshot result;
    std::vector<std::filesystem::path> pending{root_};
    auto keepPrevious = [&](const std::filesystem::path& under) {
        for (auto it = previous_.lower_bound(under); it != previous_.end(); ++it) {
            const auto relative = it->first.lexically_relative(under);
            if (relative.empty() || *relative.begin() == "..") {
                break;
            }
            result.insert(*it);
        }
    };
    while (!pending.empty()) {
        const auto directory = pending.back();
        pending.pop_back();
        std::error_code ec;
        std::filesystem::directory_iterator it(directory, ec);
        if (ec) {
            keepPrevious(directory);
            continue;
        }
        for (const std::filesystem::directory_iterator end; it != end;) {
            const auto path = it->path();
            std::error_code status;
            const bool directoryEntry = std::filesystem::is_directory(it->symlink_status(status));
            if (!status && directoryEntry) {
                pending.push_back(path); // directory symlinks are not followed
            } else if (!status && it->is_regular_file(status)) {
                std::error_code timeError, sizeError;
                const auto time = it->last_write_time(timeError);
                const auto size = it->file_size(sizeError);
                if (!timeError && !sizeError) {
                    result[path] = {time, size};
                } else if (const auto old = previous_.find(path); old != previous_.end()) {
                    result.insert(*old);
                }
            } else if (status) {
                keepPrevious(path);
            }
            it.increment(ec);
            if (ec) {
                keepPrevious(directory);
                break;
            }
        }
    }
    return result;
}

std::vector<FileChange> FileWatcher::poll() {
    auto current = snapshot();
    std::vector<FileChange> changes;
    for (const auto& [path, stamp] : current) {
        const auto previous = previous_.find(path);
        if (previous == previous_.end()) {
            changes.push_back({path, FileChangeKind::Added});
        } else if (!(previous->second == stamp)) {
            changes.push_back({path, FileChangeKind::Modified});
        }
    }
    for (const auto& [path, stamp] : previous_) {
        if (!current.contains(path)) {
            changes.push_back({path, FileChangeKind::Removed});
        }
    }
    previous_ = std::move(current);
    return changes;
}
} // namespace fe
