#include "engine/services/SaveStore.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <stdexcept>

namespace fe {
namespace {
bool reservedOnWindows(const std::string& slot) {
    static const std::array<std::string, 4> Devices = {"con", "prn", "aux", "nul"};
    if (std::find(Devices.begin(), Devices.end(), slot) != Devices.end()) {
        return true;
    }
    return slot.size() == 4 && (slot.starts_with("com") || slot.starts_with("lpt")) && slot[3] >= '0' &&
           slot[3] <= '9';
}

bool wellFormedSlot(const std::string& slot) {
    return !slot.empty() && slot.size() <= 32 &&
           slot.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}

void validateSlot(const std::string& slot) {
    if (!wellFormedSlot(slot)) {
        throw std::invalid_argument("Save slot names use 1-32 lowercase letters, digits, '_' or '-': " +
                                    slot);
    }
    if (reservedOnWindows(slot)) {
        throw std::invalid_argument("'" + slot + "' is a reserved file name on Windows");
    }
}

void validateBlobName(const std::string& name) {
    if (!wellFormedSlot(name)) {
        throw std::invalid_argument("Blob names use 1-32 lowercase letters, digits, '_' or '-': " + name);
    }
}

constexpr std::string_view SavesFolder = "saves";
constexpr std::string_view SlotSuffix = ".json";
constexpr std::string_view MetaSuffix = ".meta.json";
constexpr std::string_view BackupSuffix = ".json.bak";
constexpr std::string_view BlobSuffix = ".blob";

// A slot's document. Falls back to the backup only when the document is
// missing or cut short (empty, or JSON that ends early, as an interrupted
// write leaves it); any other unreadable document raises.
Json readSlotDocument(const SaveBackend& backend, const std::string& key, bool& fromBackup) {
    fromBackup = false;
    std::string reason;
    const auto text = backend.read(key);
    if (!text) {
        reason = "is missing";
    } else {
        try {
            return parseJson(*text);
        } catch (const Json::parse_error& error) {
            // nlohmann reports an error at the end of the text one byte past it.
            if (text->find_first_not_of(" \t\r\n") != std::string::npos && error.byte <= text->size()) {
                throw std::runtime_error(key + ": " + error.what());
            }
            reason = "is cut short";
        } catch (const std::exception& error) {
            throw std::runtime_error(key + ": " + error.what());
        }
    }
    const auto backup = backend.read(key, true);
    if (!backup) {
        throw std::runtime_error(key + " " + reason + " and has no backup");
    }
    log(LogLevel::Warning, key + " " + reason + "; reading the previous save");
    fromBackup = true;
    try {
        return parseJson(*backup);
    } catch (const std::exception& error) {
        throw std::runtime_error(key + ".bak: " + error.what());
    }
}

// Portable file_clock -> seconds since the epoch.
double secondsSinceEpoch(std::filesystem::file_time_type time) {
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration<double>(system.time_since_epoch()).count();
}
} // namespace

std::vector<std::string> SaveBackend::listBlobs(const std::string& slot) const {
    std::vector<std::string> names;
    const auto prefix = slot + ".";
    for (const auto& entry : list(std::string(SavesFolder))) {
        if (entry.starts_with(prefix) && entry.ends_with(BlobSuffix) &&
            entry.size() > prefix.size() + BlobSuffix.size()) {
            auto name = entry.substr(prefix.size(), entry.size() - prefix.size() - BlobSuffix.size());
            if (wellFormedSlot(name)) {
                names.push_back(std::move(name));
            }
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

FileSaveBackend::FileSaveBackend(std::filesystem::path directory) : directory_(std::move(directory)) {
}

std::filesystem::path FileSaveBackend::path(const std::string& key, bool backup) const {
    const auto file = directory_ / std::filesystem::path(key);
    return backup ? backupPath(file) : file;
}

std::optional<std::string> FileSaveBackend::read(const std::string& key, bool backup) const {
    const auto file = path(key, backup);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return std::nullopt;
    }
    return readFile(file);
}

void FileSaveBackend::write(const std::string& key, const std::string& bytes, bool keepBackup) {
    writeTextAtomic(path(key, false), bytes, keepBackup);
}

std::vector<std::string> FileSaveBackend::list(const std::string& folder) const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory_ / folder, ec)) {
        if (entry.is_regular_file(ec)) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

void FileSaveBackend::remove(const std::string& key) {
    std::error_code ec;
    std::filesystem::remove(path(key, false), ec);
    std::filesystem::remove(path(key, true), ec);
}

std::optional<SaveBackend::Stat> FileSaveBackend::stat(const std::string& key, bool backup) const {
    const auto file = path(key, backup);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return std::nullopt;
    }
    Stat stat;
    const auto time = std::filesystem::last_write_time(file, ec);
    if (!ec) {
        stat.modified = secondsSinceEpoch(time);
    }
    const auto size = std::filesystem::file_size(file, ec);
    if (!ec) {
        stat.size = size;
    }
    return stat;
}

SaveStore::SaveStore(std::filesystem::path directory, std::unique_ptr<SaveBackend> backend)
    : directory_(std::move(directory)),
      backend_(backend ? std::move(backend) : std::make_unique<FileSaveBackend>(directory_)) {
}

std::string SaveStore::key(const std::string& slot) {
    validateSlot(slot);
    return std::string(SavesFolder) + "/" + slot + std::string(SlotSuffix);
}

std::string SaveStore::metaKey(const std::string& slot) {
    validateSlot(slot);
    return std::string(SavesFolder) + "/" + slot + std::string(MetaSuffix);
}

std::filesystem::path SaveStore::thumbnailPath(const std::string& slot) const {
    validateSlot(slot);
    return directory_ / "thumbnails" / (slot + ".png");
}

std::optional<std::string> SaveStore::thumbnailUri(const std::string& slot) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(thumbnailPath(slot), ec)) {
        return std::nullopt;
    }
    return "user://thumbnails/" + slot + ".png";
}

void SaveStore::write(const std::string& slot, const Json& data, const Json& meta) {
    for (const auto* value : {&data, &meta}) {
        if (jsonDepth(*value) > maxJsonDepth - 1) {
            throw std::runtime_error("Save " + std::string(value == &data ? "data" : "metadata") +
                                     " nests deeper than " + std::to_string(maxJsonDepth - 1) +
                                     " levels (the save file wraps it in one more) and could not be read "
                                     "back");
        }
    }
    const auto document = Json{{"format", 1}, {"meta", meta}, {"data", data}}.dump(2) + "\n";
    if (document.size() > MaxSlotBytes) {
        throw std::runtime_error("Save '" + slot + "' is " + std::to_string(document.size()) +
                                 " bytes; a save slot holds at most " + std::to_string(MaxSlotBytes >> 20) +
                                 " MiB");
    }
    backend_->write(key(slot), document, true);
    // The sidecar follows the slot; list() trusts it only while the sizes agree.
    try {
        backend_->write(metaKey(slot), Json{{"meta", meta}, {"size", document.size()}}.dump(2) + "\n", false);
    } catch (const std::exception& error) {
        log(LogLevel::Warning, "Save '" + slot + "' was written but its summary was not: " + error.what());
    }
}

Json SaveStore::readSave(const std::string& slot, bool& fromBackup) const {
    auto document = readSlotDocument(*backend_, key(slot), fromBackup);
    if (!document.is_object() || document.value("format", Json()) != Json(1) || !document.contains("data")) {
        throw std::runtime_error("Unrecognized save file: " + key(slot) + (fromBackup ? ".bak" : ""));
    }
    return document;
}

std::optional<SaveStore::Loaded> SaveStore::read(const std::string& slot) const {
    const auto name = key(slot);
    if (!backend_->stat(name) && !backend_->stat(name, true)) {
        return std::nullopt;
    }
    bool fromBackup = false;
    const auto document = readSave(slot, fromBackup);
    return Loaded{document.at("data"), document.value("meta", Json::object()), fromBackup};
}

std::vector<SaveStore::SlotInfo> SaveStore::list() const {
    std::vector<SlotInfo> slots;
    std::set<std::string> names;
    for (const auto& name : backend_->list(std::string(SavesFolder))) {
        if (name.ends_with(MetaSuffix)) {
            continue;
        }
        for (const auto suffix : {SlotSuffix, BackupSuffix}) {
            if (name.size() > suffix.size() && name.ends_with(suffix)) {
                // Files that are not slots (a sync tool's "slot1 (conflicted copy).json") are skipped.
                auto slot = name.substr(0, name.size() - suffix.size());
                if (wellFormedSlot(slot) && !reservedOnWindows(slot)) {
                    names.insert(std::move(slot));
                }
                break;
            }
        }
    }
    for (const auto& slot : names) {
        SlotInfo info;
        info.slot = slot;
        const auto current = backend_->stat(key(slot));
        const auto stat = current ? current : backend_->stat(key(slot), true);
        info.modified = stat ? stat->modified : 0;
        bool summarised = false;
        if (current && current->size) {
            // The sidecar is cheap; it is trusted while it describes a document of the current size.
            if (const auto sidecar = backend_->read(metaKey(slot))) {
                try {
                    const auto summary = parseJson(*sidecar);
                    if (summary.is_object() && summary.value("size", std::uint64_t{0}) == *current->size &&
                        summary.contains("meta")) {
                        info.meta = summary.at("meta");
                        summarised = true;
                    }
                } catch (const std::exception&) {
                    // Falls back to the full read below.
                }
            }
        }
        if (!summarised) {
            try {
                info.meta = readSave(slot, info.fromBackup).value("meta", Json::object());
            } catch (const std::exception&) {
                info.meta = {{"corrupt", true}};
                info.fromBackup = false;
            }
        }
        std::error_code ec;
        info.thumbnail = std::filesystem::is_regular_file(thumbnailPath(slot), ec);
        slots.push_back(std::move(info));
    }
    return slots;
}

void SaveStore::remove(const std::string& slot) {
    backend_->remove(key(slot));
    backend_->remove(metaKey(slot));
    for (const auto& blob : backend_->listBlobs(slot)) {
        backend_->removeBlob(slot, blob);
    }
    std::error_code ec;
    std::filesystem::remove(thumbnailPath(slot), ec);
}

void SaveStore::writeBlob(const std::string& slot, const std::string& name, const std::string& bytes) {
    validateSlot(slot);
    validateBlobName(name);
    if (bytes.size() > MaxSlotBytes) {
        throw std::runtime_error("Blob '" + name + "' of save '" + slot + "' is " +
                                 std::to_string(bytes.size()) + " bytes; a blob holds at most " +
                                 std::to_string(MaxSlotBytes >> 20) + " MiB");
    }
    backend_->writeBlob(slot, name, bytes);
}

std::optional<std::string> SaveStore::readBlob(const std::string& slot, const std::string& name) const {
    validateSlot(slot);
    validateBlobName(name);
    return backend_->readBlob(slot, name);
}

void SaveStore::removeBlob(const std::string& slot, const std::string& name) {
    validateSlot(slot);
    validateBlobName(name);
    backend_->removeBlob(slot, name);
}

std::vector<std::string> SaveStore::listBlobs(const std::string& slot) const {
    validateSlot(slot);
    return backend_->listBlobs(slot);
}

Json SaveStore::readConfig() const {
    const std::string name = "config.json";
    const auto current = backend_->read(name);
    if (current) {
        try {
            return parseJson(*current);
        } catch (const std::exception& error) {
            if (!backend_->stat(name, true)) {
                throw std::runtime_error(name + ": " + error.what());
            }
            log(LogLevel::Warning, name + ": " + error.what() + "; using the backup");
        }
    }
    const auto backup = backend_->read(name, true);
    if (!backup) {
        return Json::object();
    }
    try {
        return parseJson(*backup);
    } catch (const std::exception& error) {
        throw std::runtime_error(name + ".bak: " + error.what());
    }
}

void SaveStore::writeConfig(const Json& data) {
    if (jsonDepth(data) > maxJsonDepth) {
        throw std::runtime_error("Cannot write config.json: its JSON would nest deeper than " +
                                 std::to_string(maxJsonDepth) + " levels and could not be read back");
    }
    backend_->write("config.json", data.dump(2) + "\n", true);
}
} // namespace fe
