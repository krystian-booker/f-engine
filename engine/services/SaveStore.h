#pragma once
#include "engine/assets/Image.h"
#include "engine/core/Json.h"
#include <filesystem>
#include <optional>
#include <vector>

namespace fe {
// Schema-free persistence. Scripts decide what a save contains; this class
// only writes JSON atomically into the per-user data folder.
class SaveStore {
public:
    struct SlotInfo {
        std::string slot;
        Json meta;
        double modified = 0; // seconds since epoch
        bool thumbnail = false;
    };
    struct Loaded {
        Json data, meta;
    };
    explicit SaveStore(std::filesystem::path directory);
    void write(const std::string& slot, const Json& data, const Json& meta);
    void writeThumbnail(const std::string& slot, const Image& image);
    [[nodiscard]] std::optional<Loaded> read(const std::string& slot) const;
    [[nodiscard]] std::vector<SlotInfo> list() const;
    void remove(const std::string& slot);
    [[nodiscard]] std::filesystem::path thumbnailPath(const std::string& slot) const;
    [[nodiscard]] Json readConfig() const;
    void writeConfig(const Json& data);
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

private:
    [[nodiscard]] std::filesystem::path path(const std::string& slot) const;
    std::filesystem::path directory_;
};
} // namespace fe
