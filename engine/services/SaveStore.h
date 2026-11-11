#pragma once
#include "engine/core/Json.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace fe {
// Where save documents live. SaveStore keeps slots and settings as text under
// keys such as "saves/slot1.json", "saves/slot1.meta.json" and "config.json";
// a backend stores those bytes. The default writes files in the user-data
// folder; a port (cloud saves, a console's save API) implements this and
// hands it to the engine (EngineOptions::saveBackend). Thumbnails are PNG
// files the asset cache reads as `user://` images, so they stay in the
// user-data folder whatever the backend.
class SaveBackend {
public:
    struct Stat {
        double modified = 0; // seconds since epoch, 0 when unknown
        std::optional<std::uint64_t> size;
    };
    virtual ~SaveBackend() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    // The bytes under `key`, or with `backup` the version a write with
    // `keepBackup` set aside; nullopt when there is none.
    [[nodiscard]] virtual std::optional<std::string> read(const std::string& key,
                                                          bool backup = false) const = 0;
    // Replaces `key` atomically (a failure leaves the old version). With
    // `keepBackup` the previous version stays readable as the backup.
    virtual void write(const std::string& key, const std::string& bytes, bool keepBackup) = 0;
    // Names directly below `folder` ("saves"): current versions as their key
    // name ("slot1.json") and backups with ".bak" appended.
    [[nodiscard]] virtual std::vector<std::string> list(const std::string& folder) const = 0;
    // Removes the key and its backup.
    virtual void remove(const std::string& key) = 0;
    // Cheap facts about a key (no read); nullopt when it does not exist.
    [[nodiscard]] virtual std::optional<Stat> stat(const std::string& key, bool backup = false) const = 0;

    // Binary blobs stored next to a slot (snapshots, screenshots a port's
    // save API keeps with the save): `name` is a slot-like token. The
    // defaults keep them as the key "saves/<slot>.<name>.blob" through the
    // functions above, so a port only overrides them to use its own blob
    // storage. SaveStore removes a slot's blobs with the slot.
    virtual void writeBlob(const std::string& slot, const std::string& name, const std::string& bytes) {
        write(blobKey(slot, name), bytes, false);
    }
    [[nodiscard]] virtual std::optional<std::string> readBlob(const std::string& slot,
                                                              const std::string& name) const {
        return read(blobKey(slot, name));
    }
    virtual void removeBlob(const std::string& slot, const std::string& name) { remove(blobKey(slot, name)); }
    // Names of the slot's blobs, sorted.
    [[nodiscard]] virtual std::vector<std::string> listBlobs(const std::string& slot) const;
    [[nodiscard]] static std::string blobKey(const std::string& slot, const std::string& name) {
        return "saves/" + slot + "." + name + ".blob";
    }

    // For a backend that writes in the background (QueuedSaveBackend): wait
    // until every write is done; how many are not; and the first failure
    // since the last call, once. A backend that writes as it is called has
    // nothing to wait for.
    virtual void flush() {}
    [[nodiscard]] virtual std::size_t pending() const { return 0; }
    [[nodiscard]] virtual std::optional<std::string> takeError() { return std::nullopt; }
};

// The default backend: files below a folder, written with writeTextAtomic;
// backups are `<file>.bak`.
class FileSaveBackend final : public SaveBackend {
public:
    explicit FileSaveBackend(std::filesystem::path directory);
    [[nodiscard]] std::string name() const override { return "file"; }
    [[nodiscard]] std::optional<std::string> read(const std::string& key, bool backup) const override;
    void write(const std::string& key, const std::string& bytes, bool keepBackup) override;
    [[nodiscard]] std::vector<std::string> list(const std::string& folder) const override;
    void remove(const std::string& key) override;
    [[nodiscard]] std::optional<Stat> stat(const std::string& key, bool backup) const override;

private:
    [[nodiscard]] std::filesystem::path path(const std::string& key, bool backup) const;
    std::filesystem::path directory_;
};

// Schema-free persistence. Scripts decide what a save contains; this class
// writes JSON durably and atomically through a SaveBackend and keeps the
// previous version of each document as its backup. A slot read falls back to
// the backup only when the slot's document is missing or cut short (empty, or
// its JSON ends early, as an interrupted write leaves it), and says so; a
// slot document that is complete but unreadable (invalid JSON, nested too
// deep, not a save) raises an error rather than silently loading the previous
// save. The settings document falls back to its backup when damaged in any
// way. Each slot's `meta` is also kept in a sidecar (`<slot>.meta.json`) so
// list() needs no full parse; a sidecar that is missing or does not match the
// slot's size falls back to the full read.
class SaveStore {
public:
    // A slot document larger than this is refused.
    static constexpr std::size_t MaxSlotBytes = std::size_t{16} << 20;
    struct SlotInfo {
        std::string slot;
        Json meta;
        double modified = 0; // seconds since epoch
        bool thumbnail = false;
        bool fromBackup = false; // meta comes from the .bak (see read)
    };
    struct Loaded {
        Json data, meta;
        bool fromBackup = false; // the slot's file was missing or cut short; this is the .bak
    };
    // Files below `directory` (thumbnails always; saves and settings unless
    // `backend` stores them elsewhere).
    explicit SaveStore(std::filesystem::path directory, std::unique_ptr<SaveBackend> backend = nullptr);
    // Raises when the save could not be read back: `data` and `meta` may nest
    // at most maxJsonDepth - 1 levels, since the save file wraps them, and the
    // document may not exceed MaxSlotBytes.
    void write(const std::string& slot, const Json& data, const Json& meta);
    [[nodiscard]] std::optional<Loaded> read(const std::string& slot) const;
    // Blobs next to a slot (names follow slot rules; at most MaxSlotBytes
    // each); the slot itself need not exist. remove(slot) removes them too.
    void writeBlob(const std::string& slot, const std::string& name, const std::string& bytes);
    [[nodiscard]] std::optional<std::string> readBlob(const std::string& slot, const std::string& name) const;
    void removeBlob(const std::string& slot, const std::string& name);
    [[nodiscard]] std::vector<std::string> listBlobs(const std::string& slot) const;
    // Slots in the saves folder, sorted by name; files whose names are not
    // valid slot names are ignored.
    [[nodiscard]] std::vector<SlotInfo> list() const;
    void remove(const std::string& slot);
    // Thumbnails are <user data>/thumbnails/<slot>.png; the URI is the
    // "user://thumbnails/<slot>.png" path images load from, when one exists.
    [[nodiscard]] std::filesystem::path thumbnailPath(const std::string& slot) const;
    [[nodiscard]] std::optional<std::string> thumbnailUri(const std::string& slot) const;
    [[nodiscard]] Json readConfig() const;
    void writeConfig(const Json& data);
    // Writes still on their way to storage (0 with a backend that writes at
    // once), and waiting for them: flush() returns when all are written and
    // raises the first failure among them.
    [[nodiscard]] std::size_t pending() const { return backend_->pending(); }
    void flush();
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    [[nodiscard]] SaveBackend& backend() { return *backend_; }
    // Where fe.app.capture writes screenshots.
    [[nodiscard]] std::filesystem::path screenshotFolder() const { return directory_ / "screenshots"; }

private:
    [[nodiscard]] static std::string key(const std::string& slot);
    [[nodiscard]] static std::string metaKey(const std::string& slot);
    [[nodiscard]] Json readSave(const std::string& slot, bool& fromBackup) const;
    std::filesystem::path directory_;
    std::unique_ptr<SaveBackend> backend_;
};
} // namespace fe
