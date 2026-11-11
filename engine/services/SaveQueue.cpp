#include "engine/services/SaveQueue.h"
#include "engine/core/Log.h"
#include <algorithm>
#include <chrono>

namespace fe {
namespace {
double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

QueuedSaveBackend::QueuedSaveBackend(std::unique_ptr<SaveBackend> inner)
    : inner_(std::move(inner)), concurrentReads_(inner_->concurrentReads()), worker_([this] { run(); }) {
}

QueuedSaveBackend::~QueuedSaveBackend() {
    {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [&] { return waiting_.empty() && runningId_.empty(); });
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void QueuedSaveBackend::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || !waiting_.empty(); });
        if (waiting_.empty()) {
            return; // stop_, with nothing left to write
        }
        running_ = std::move(waiting_.front());
        waiting_.pop_front();
        runningId_ = running_.id;
        lock.unlock();
        std::optional<std::string> failure;
        try {
            const std::scoped_lock io(io_);
            if (running_.blob) {
                inner_->writeBlob(running_.key, running_.name, running_.bytes);
            } else {
                inner_->write(running_.key, running_.bytes, running_.keepBackup);
            }
        } catch (const std::exception& error) {
            failure = (running_.blob ? "Blob '" + running_.name + "' of save '" + running_.key + "'"
                                     : "'" + running_.key + "'") +
                      " was not written: " + error.what();
            log(LogLevel::Error, *failure);
        }
        lock.lock();
        if (failure && !error_) {
            error_ = std::move(failure);
        }
        runningId_.clear();
        running_ = {};
        idle_.notify_all();
    }
}

void QueuedSaveBackend::enqueue(Job job) {
    {
        const std::scoped_lock lock(mutex_);
        job.queued = now();
        const auto newest = std::find_if(waiting_.rbegin(), waiting_.rend(),
                                         [&](const Job& queued) { return queued.id == job.id; });
        if (newest != waiting_.rend() && !newest->keepBackup && !job.keepBackup) {
            *newest = std::move(job); // only the newest version of this key is written
        } else {
            waiting_.push_back(std::move(job));
        }
    }
    wake_.notify_one();
}

const QueuedSaveBackend::Job* QueuedSaveBackend::find(const std::string& id) const {
    const auto newest =
        std::find_if(waiting_.rbegin(), waiting_.rend(), [&](const Job& queued) { return queued.id == id; });
    if (newest != waiting_.rend()) {
        return &*newest;
    }
    return runningId_ == id ? &running_ : nullptr;
}

void QueuedSaveBackend::settle(const std::string& id) {
    std::unique_lock lock(mutex_);
    std::erase_if(waiting_, [&](const Job& queued) { return queued.id == id; });
    idle_.wait(lock, [&] { return runningId_ != id; });
}

void QueuedSaveBackend::write(const std::string& key, const std::string& bytes, bool keepBackup) {
    Job job;
    job.key = key;
    job.bytes = bytes;
    job.keepBackup = keepBackup;
    job.id = keyId(key);
    enqueue(std::move(job));
}

void QueuedSaveBackend::writeBlob(const std::string& slot, const std::string& name,
                                  const std::string& bytes) {
    Job job;
    job.blob = true;
    job.key = slot;
    job.name = name;
    job.bytes = bytes;
    job.id = blobId(slot, name);
    enqueue(std::move(job));
}

std::optional<std::string> QueuedSaveBackend::read(const std::string& key, bool backup) const {
    if (backup) {
        // The backup is what the wrapped backend holds once this key's writes are done.
        std::unique_lock lock(mutex_);
        const_cast<QueuedSaveBackend*>(this)->idle_.wait(lock, [&] { return find(keyId(key)) == nullptr; });
    } else {
        const std::scoped_lock lock(mutex_);
        if (const auto* job = find(keyId(key))) {
            return job->bytes;
        }
    }
    const auto io = readLock();
    return inner_->read(key, backup);
}

std::optional<std::string> QueuedSaveBackend::readBlob(const std::string& slot,
                                                       const std::string& name) const {
    {
        const std::scoped_lock lock(mutex_);
        if (const auto* job = find(blobId(slot, name))) {
            return job->bytes;
        }
    }
    const auto io = readLock();
    return inner_->readBlob(slot, name);
}

std::optional<SaveBackend::Stat> QueuedSaveBackend::stat(const std::string& key, bool backup) const {
    if (backup) {
        std::unique_lock lock(mutex_);
        const_cast<QueuedSaveBackend*>(this)->idle_.wait(lock, [&] { return find(keyId(key)) == nullptr; });
    } else {
        const std::scoped_lock lock(mutex_);
        if (const auto* job = find(keyId(key))) {
            Stat stat;
            stat.modified = job->queued;
            stat.size = job->bytes.size();
            return stat;
        }
    }
    const auto io = readLock();
    return inner_->stat(key, backup);
}

std::vector<std::string> QueuedSaveBackend::list(const std::string& folder) const {
    std::vector<std::string> names;
    {
        const auto io = readLock();
        names = inner_->list(folder);
    }
    const std::scoped_lock lock(mutex_);
    const auto prefix = "k:" + folder + "/";
    const auto add = [&](const std::string& id) {
        if (id.starts_with(prefix) && id.find('/', prefix.size()) == std::string::npos) {
            auto name = id.substr(prefix.size());
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(std::move(name));
            }
        }
    };
    for (const auto& job : waiting_) {
        add(job.id);
    }
    if (!runningId_.empty()) {
        add(runningId_);
    }
    return names;
}

std::vector<std::string> QueuedSaveBackend::listBlobs(const std::string& slot) const {
    std::vector<std::string> names;
    {
        const auto io = readLock();
        names = inner_->listBlobs(slot);
    }
    const std::scoped_lock lock(mutex_);
    const auto add = [&](const Job& job) {
        if (job.blob && job.key == slot && std::find(names.begin(), names.end(), job.name) == names.end()) {
            names.push_back(job.name);
        }
    };
    for (const auto& job : waiting_) {
        add(job);
    }
    if (!runningId_.empty()) {
        add(running_);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void QueuedSaveBackend::remove(const std::string& key) {
    settle(keyId(key));
    const std::scoped_lock io(io_);
    inner_->remove(key);
}

void QueuedSaveBackend::removeBlob(const std::string& slot, const std::string& name) {
    settle(blobId(slot, name));
    const std::scoped_lock io(io_);
    inner_->removeBlob(slot, name);
}

void QueuedSaveBackend::flush() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return waiting_.empty() && runningId_.empty(); });
}

std::size_t QueuedSaveBackend::pending() const {
    const std::scoped_lock lock(mutex_);
    return waiting_.size() + (runningId_.empty() ? 0 : 1);
}

std::optional<std::string> QueuedSaveBackend::takeError() {
    const std::scoped_lock lock(mutex_);
    return std::exchange(error_, std::nullopt);
}
} // namespace fe
