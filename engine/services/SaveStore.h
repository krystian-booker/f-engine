#pragma once
#include "engine/core/Json.h"
#include <filesystem>
#include <optional>
#include <vector>

namespace fe {
// Schema-free persistence. Scripts decide what a save contains; this class
// writes JSON durably and atomically into the per-user data folder and keeps
// the previous version of each file as `<file>.bak`. A slot read falls back
// to the `.bak` only when the slot's file is missing or cut short (empty, or
// its JSON ends early, as an interrupted write leaves it), and says so; a
// slot file that is complete but unreadable (invalid JSON, nested too deep,
// not a save) raises an error rather than silently loading the previous
// save. The settings file falls back to its `.bak` when damaged in any way.
class SaveStore {
public:
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
    explicit SaveStore(std::filesystem::path directory);
    // Raises when the save could not be read back: `data` and `meta` may nest
    // at most maxJsonDepth - 1 levels, since the save file wraps them.
    void write(const std::string& slot, const Json& data, const Json& meta);
    [[nodiscard]] std::optional<Loaded> read(const std::string& slot) const;
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
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    // Where fe.app.capture writes screenshots.
    [[nodiscard]] std::filesystem::path screenshotFolder() const { return directory_ / "screenshots"; }

private:
    [[nodiscard]] std::filesystem::path path(const std::string& slot) const;
    std::filesystem::path directory_;
};
} // namespace fe
