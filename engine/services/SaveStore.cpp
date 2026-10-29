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

constexpr std::string_view SlotSuffix = ".json";
constexpr std::string_view BackupSuffix = ".json.bak";

// A slot file's document. Falls back to the `.bak` only when the file is
// missing or cut short (empty, or JSON that ends early, as an interrupted
// write leaves it); any other unreadable file raises.
Json readSlotFile(const std::filesystem::path& file, bool& fromBackup) {
    const auto backup = backupPath(file);
    fromBackup = false;
    std::string reason;
    if (!std::filesystem::is_regular_file(file)) {
        reason = "is missing";
    } else {
        const auto text = readFile(file);
        try {
            return parseJson(text);
        } catch (const Json::parse_error& error) {
            // nlohmann reports an error at the end of the text one byte past it.
            if (text.find_first_not_of(" \t\r\n") != std::string::npos && error.byte <= text.size()) {
                throw std::runtime_error(file.string() + ": " + error.what());
            }
            reason = "is cut short";
        } catch (const std::exception& error) {
            throw std::runtime_error(file.string() + ": " + error.what());
        }
    }
    if (!std::filesystem::is_regular_file(backup)) {
        throw std::runtime_error(file.string() + " " + reason + " and has no backup");
    }
    log(LogLevel::Warning, file.string() + " " + reason + "; reading the previous save " + backup.string());
    fromBackup = true;
    return readJson(backup);
}

// A slot file's document, checked to be a save.
Json readSave(const std::filesystem::path& file, bool& fromBackup) {
    auto document = readSlotFile(file, fromBackup);
    if (!document.is_object() || document.value("format", Json()) != Json(1) || !document.contains("data")) {
        throw std::runtime_error("Unrecognized save file: " +
                                 (fromBackup ? backupPath(file) : file).string());
    }
    return document;
}
} // namespace

SaveStore::SaveStore(std::filesystem::path directory) : directory_(std::move(directory)) {
}

std::filesystem::path SaveStore::path(const std::string& slot) const {
    validateSlot(slot);
    return directory_ / "saves" / (slot + ".json");
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
    writeJson(path(slot), {{"format", 1}, {"meta", meta}, {"data", data}}, true);
}

std::optional<SaveStore::Loaded> SaveStore::read(const std::string& slot) const {
    const auto file = path(slot);
    if (!std::filesystem::is_regular_file(file) && !std::filesystem::is_regular_file(backupPath(file))) {
        return std::nullopt;
    }
    bool fromBackup = false;
    const auto document = readSave(file, fromBackup);
    return Loaded{document.at("data"), document.value("meta", Json::object()), fromBackup};
}

std::vector<SaveStore::SlotInfo> SaveStore::list() const {
    std::vector<SlotInfo> slots;
    std::error_code ec;
    const auto folder = directory_ / "saves";
    if (!std::filesystem::is_directory(folder, ec)) {
        return slots;
    }
    std::set<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const auto name = entry.path().filename().string();
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
        auto file = folder / (slot + ".json");
        if (!std::filesystem::is_regular_file(file, ec)) {
            file = backupPath(file);
        }
        SlotInfo info;
        info.slot = slot;
        try {
            info.meta = readSave(folder / (slot + ".json"), info.fromBackup).value("meta", Json::object());
        } catch (const std::exception&) {
            info.meta = {{"corrupt", true}};
            info.fromBackup = false;
        }
        const auto time = std::filesystem::last_write_time(file, ec);
        // Portable file_clock -> system_clock conversion.
        const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        info.modified = std::chrono::duration<double>(system.time_since_epoch()).count();
        info.thumbnail = std::filesystem::is_regular_file(thumbnailPath(slot), ec);
        slots.push_back(std::move(info));
    }
    return slots;
}

void SaveStore::remove(const std::string& slot) {
    std::filesystem::remove(path(slot));
    std::filesystem::remove(backupPath(path(slot)));
    std::filesystem::remove(thumbnailPath(slot));
}

Json SaveStore::readConfig() const {
    const auto file = directory_ / "config.json";
    if (!std::filesystem::is_regular_file(file) && !std::filesystem::is_regular_file(backupPath(file))) {
        return Json::object();
    }
    return readJsonOrBackup(file);
}

void SaveStore::writeConfig(const Json& data) {
    writeJson(directory_ / "config.json", data, true);
}
} // namespace fe
