#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
// A .pak archive holds a folder's files in one read-only file. Layout, little
// endian:
//   header  "FPAK", u32 version (1), u64 index offset, u64 index size,
//           u32 entry count, u32 CRC-32 of the index
//   data    each entry's bytes, stored or as a zlib stream
//   index   per entry, sorted by name: u16 name length, name (UTF-8, '/'
//           separated), u8 method (0 stored, 1 zlib), u64 offset,
//           u64 stored size, u64 size, u32 CRC-32 of the original bytes
// Names are relative paths without "." or ".." parts, so no entry can name a
// file outside the folder the archive stands in for.
[[nodiscard]] bool validPackName(std::string_view name);

class PackArchive {
public:
    struct Entry {
        std::string name;
        std::uint8_t method = 0;
        std::uint64_t offset = 0, storedSize = 0, size = 0;
        std::uint32_t crc = 0;
    };
    // Reads and validates the index; throws on a damaged or foreign file.
    explicit PackArchive(std::filesystem::path file);

    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
    [[nodiscard]] const Entry* find(std::string_view name) const;
    // Whether any entry lies below `folder` ("" is the archive root).
    [[nodiscard]] bool hasFolder(std::string_view folder) const;
    // Sorted names directly inside `folder`; sub-folders end with '/'.
    [[nodiscard]] std::vector<std::string> list(std::string_view folder) const;
    // The entry's original bytes, checked against its CRC. Thread safe.
    [[nodiscard]] std::string read(const Entry& entry) const;

private:
    std::filesystem::path file_;
    std::vector<Entry> entries_;
};

class PackWriter {
public:
    enum class Compression { Auto, Store, Zlib };
    // Writes to `<output>.tmp`; finish() moves it over `output`.
    explicit PackWriter(std::filesystem::path output);
    ~PackWriter();
    PackWriter(const PackWriter&) = delete;
    PackWriter& operator=(const PackWriter&) = delete;

    // Auto compresses unless the format is already compressed or zlib saves
    // less than a tenth.
    void add(const std::string& name, std::string_view bytes, Compression compression = Compression::Auto);
    void finish();

private:
    std::filesystem::path output_, temporary_;
    std::ofstream stream_;
    std::vector<PackArchive::Entry> entries_;
    std::set<std::string> names_;
    bool finished_ = false;
};

// Packs every regular file below `folder`, in name order, into `output`.
// Returns the number of files.
std::size_t packFolder(const std::filesystem::path& folder, const std::filesystem::path& output);
} // namespace fe
