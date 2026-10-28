#include "engine/services/SaveStore.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace fe {
namespace {
void validateSlot(const std::string& slot) {
    if (slot.empty() || slot.size() > 32 ||
        slot.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
        throw std::invalid_argument("Save slot names use 1-32 lowercase letters, digits, '_' or '-': " +
                                    slot);
    }
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
    return directory_ / "saves" / (slot + ".png");
}

void SaveStore::write(const std::string& slot, const Json& data, const Json& meta) {
    writeJson(path(slot), {{"format", 1}, {"meta", meta}, {"data", data}});
}

void SaveStore::writeThumbnail(const std::string& slot, const Image& image) {
    image.savePng(thumbnailPath(slot));
}

std::optional<SaveStore::Loaded> SaveStore::read(const std::string& slot) const {
    const auto file = path(slot);
    if (!std::filesystem::is_regular_file(file)) {
        return std::nullopt;
    }
    const auto document = readJson(file);
    if (document.value("format", 0) != 1 || !document.contains("data")) {
        throw std::runtime_error("Unrecognized save file: " + file.string());
    }
    return Loaded{document.at("data"), document.value("meta", Json::object())};
}

std::vector<SaveStore::SlotInfo> SaveStore::list() const {
    std::vector<SlotInfo> slots;
    std::error_code ec;
    const auto folder = directory_ / "saves";
    if (!std::filesystem::is_directory(folder, ec)) {
        return slots;
    }
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        SlotInfo info;
        info.slot = entry.path().stem().string();
        try {
            info.meta = readJson(entry.path()).value("meta", Json::object());
        } catch (const std::exception&) {
            info.meta = {{"corrupt", true}};
        }
        const auto time = std::filesystem::last_write_time(entry.path(), ec);
        // Portable file_clock -> system_clock conversion.
        const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        info.modified = std::chrono::duration<double>(system.time_since_epoch()).count();
        info.thumbnail = std::filesystem::exists(folder / (info.slot + ".png"));
        slots.push_back(std::move(info));
    }
    std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    return slots;
}

void SaveStore::remove(const std::string& slot) {
    std::filesystem::remove(path(slot));
    std::filesystem::remove(thumbnailPath(slot));
}

Json SaveStore::readConfig() const {
    const auto file = directory_ / "config.json";
    if (!std::filesystem::is_regular_file(file)) {
        return Json::object();
    }
    return readJson(file);
}

void SaveStore::writeConfig(const Json& data) {
    writeJson(directory_ / "config.json", data);
}
} // namespace fe
