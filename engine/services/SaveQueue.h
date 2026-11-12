#pragma once
#include "engine/services/SaveStore.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace fe {
// Writes saves on a worker thread. A durable write is slow (a data flush, two
// renames, a folder flush; tens of milliseconds on a hard disk or a console's
// save volume), and a game calls it while it runs: an autosave on a door, a
// settings change. This backend wraps another one: write and writeBlob return
// once the bytes are queued, and the worker hands them to the wrapped backend
// in order.
//
// The game cannot tell the difference except by the clock:
//   * read, stat and list answer from the queue for anything not yet written,
//     so a slot reads back the moment write returns;
//   * a later write of the same key replaces a queued one that keeps no
//     backup (only the newest version is written); writes that keep a
//     backup, slots, are all carried out, so the backup is always the save
//     before the last one;
//   * remove drops what is queued for the key and waits for a write of it in
//     progress before removing;
//   * flush() waits until everything is written; the destructor flushes.
// A write that fails is logged, and reported once by takeError() (which
// fe.save.flush raises). Writes and removes of the wrapped backend never
// overlap each other; its reads wait for a write in progress too unless it
// says they need not (SaveBackend::concurrentReads, as the file backend
// does), in which case nothing the game calls waits on storage except
// remove and flush.
class QueuedSaveBackend final : public SaveBackend {
public:
    explicit QueuedSaveBackend(std::unique_ptr<SaveBackend> inner);
    ~QueuedSaveBackend() override;
    QueuedSaveBackend(const QueuedSaveBackend&) = delete;
    QueuedSaveBackend& operator=(const QueuedSaveBackend&) = delete;

    [[nodiscard]] std::string name() const override { return inner_->name(); }
    [[nodiscard]] std::optional<std::string> read(const std::string& key, bool backup) const override;
    void write(const std::string& key, const std::string& bytes, bool keepBackup) override;
    [[nodiscard]] std::vector<std::string> list(const std::string& folder) const override;
    void remove(const std::string& key) override;
    [[nodiscard]] std::optional<Stat> stat(const std::string& key, bool backup) const override;
    void writeBlob(const std::string& slot, const std::string& name, const std::string& bytes) override;
    [[nodiscard]] std::optional<std::string> readBlob(const std::string& slot,
                                                      const std::string& name) const override;
    void removeBlob(const std::string& slot, const std::string& name) override;
    [[nodiscard]] std::vector<std::string> listBlobs(const std::string& slot) const override;

    void flush() override;
    [[nodiscard]] std::size_t pending() const override;
    [[nodiscard]] std::optional<std::string> takeError() override;

private:
    struct Job {
        std::string id;
        bool blob = false;
        std::string key;  // a key, or for a blob its slot
        std::string name; // a blob's name
        std::string bytes;
        bool keepBackup = false;
        double queued = 0; // seconds since epoch
    };
    [[nodiscard]] static std::string keyId(const std::string& key) { return "k:" + key; }
    [[nodiscard]] static std::string blobId(const std::string& slot, const std::string& name) {
        return "b:" + slot + "/" + name;
    }
    void enqueue(Job job);
    // The newest queued job with this id, else the one in progress (mutex_ held), or null.
    [[nodiscard]] const Job* find(const std::string& id) const;
    // Drops a queued job and waits for one in progress.
    void settle(const std::string& id);
    void run();

    std::unique_ptr<SaveBackend> inner_;
    mutable std::mutex mutex_; // the queue
    mutable std::mutex io_;    // calls into inner_ (reads only when it cannot take them concurrently)
    // Held for a read of inner_: io_, or nothing when reads may overlap writes.
    [[nodiscard]] std::unique_lock<std::mutex> readLock() const {
        return concurrentReads_ ? std::unique_lock<std::mutex>() : std::unique_lock<std::mutex>(io_);
    }
    bool concurrentReads_ = false;
    std::condition_variable wake_, idle_;
    std::deque<Job> waiting_; // oldest first
    std::string runningId_;   // the job the worker is writing, "" for none
    Job running_;
    std::optional<std::string> error_;
    bool stop_ = false;
    std::thread worker_;
};
} // namespace fe
