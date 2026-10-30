#include "engine/platform/FileWatcher.h"

namespace fe {
FileWatcher::FileWatcher(std::filesystem::path root, std::size_t entriesPerPoll)
    : root_(std::move(root)), entriesPerPoll_(entriesPerPoll) {
    pending_.push_back(root_);
    walk(0);
    previous_ = std::move(building_);
    building_.clear();
}

void FileWatcher::keepPrevious(const std::filesystem::path& under) {
    for (auto it = previous_.lower_bound(under); it != previous_.end(); ++it) {
        const auto relative = it->first.lexically_relative(under);
        if (relative.empty() || *relative.begin() == "..") {
            break;
        }
        building_.insert(*it);
    }
}

// Walks the tree one directory at a time. Anything that cannot be read this
// time (a directory being replaced, a file mid-save) keeps its previous
// entries, so a transient error never looks like files being removed and
// added again. A budget of 0 finishes the walk; otherwise it stops after the
// directory that exhausts the budget.
bool FileWatcher::walk(std::size_t budget) {
    std::size_t visited = 0;
    while (!pending_.empty()) {
        if (budget > 0 && visited >= budget) {
            return false;
        }
        const auto directory = pending_.back();
        pending_.pop_back();
        std::error_code ec;
        std::filesystem::directory_iterator it(directory, ec);
        if (ec) {
            keepPrevious(directory);
            continue;
        }
        for (const std::filesystem::directory_iterator end; it != end;) {
            ++visited;
            const auto path = it->path();
            std::error_code status;
            const bool directoryEntry = std::filesystem::is_directory(it->symlink_status(status));
            if (!status && directoryEntry) {
                pending_.push_back(path); // directory symlinks are not followed
            } else if (!status && it->is_regular_file(status)) {
                std::error_code timeError, sizeError;
                const auto time = it->last_write_time(timeError);
                const auto size = it->file_size(sizeError);
                if (!timeError && !sizeError) {
                    building_[path] = {time, size};
                } else if (const auto old = previous_.find(path); old != previous_.end()) {
                    building_.insert(*old);
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
    return true;
}

std::vector<FileChange> FileWatcher::poll() {
    if (pending_.empty()) {
        building_.clear();
        pending_.push_back(root_);
    }
    if (!walk(entriesPerPoll_)) {
        return {};
    }
    std::vector<FileChange> changes;
    for (const auto& [path, stamp] : building_) {
        const auto previous = previous_.find(path);
        if (previous == previous_.end()) {
            changes.push_back({path, FileChangeKind::Added});
        } else if (!(previous->second == stamp)) {
            changes.push_back({path, FileChangeKind::Modified});
        }
    }
    for (const auto& [path, stamp] : previous_) {
        if (!building_.contains(path)) {
            changes.push_back({path, FileChangeKind::Removed});
        }
    }
    previous_ = std::move(building_);
    building_.clear();
    return changes;
}
} // namespace fe
