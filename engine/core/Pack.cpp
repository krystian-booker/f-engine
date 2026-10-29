#include "engine/core/Pack.h"
#include "engine/core/Json.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stb_image.h>
#include <stdexcept>

// Defined with the stb_image_write implementation (engine/assets/Image.cpp).
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace fe {
namespace {
constexpr std::array<char, 4> Magic = {'F', 'P', 'A', 'K'};
constexpr std::uint32_t Version = 1;
constexpr std::uint64_t HeaderSize = 32;
constexpr std::uint64_t MaxEntrySize = std::numeric_limits<int>::max();
constexpr std::uint64_t MaxIndexSize = 256ull * 1024 * 1024;
constexpr std::uint8_t Stored = 0, Zlib = 1;

std::uint32_t crc32(std::string_view bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            values[i] = c;
        }
        return values;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (const char byte : bytes) {
        crc = table[(crc ^ static_cast<std::uint8_t>(byte)) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

template<class T> void put(std::string& out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xffu));
    }
}

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}
    template<class T> T get() {
        need(sizeof(T));
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes_[at_ + i])) << (8 * i);
        }
        at_ += sizeof(T);
        return static_cast<T>(value);
    }
    std::string text(std::size_t size) {
        need(size);
        std::string value(bytes_.substr(at_, size));
        at_ += size;
        return value;
    }
    [[nodiscard]] bool done() const { return at_ == bytes_.size(); }

private:
    void need(std::size_t size) const {
        if (bytes_.size() - at_ < size) {
            throw std::runtime_error("truncated index");
        }
    }
    std::string_view bytes_;
    std::size_t at_ = 0;
};

std::string readRange(const std::filesystem::path& file, std::uint64_t offset, std::uint64_t size) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open " + file.string());
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.seekg(static_cast<std::streamoff>(offset));
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("Cannot read " + file.string());
    }
    return bytes;
}

bool alreadyCompressed(const std::string& name) {
    const auto extension = std::filesystem::path(name).extension().string();
    for (const char* known : {".png", ".jpg", ".jpeg", ".ogg", ".mp3", ".flac", ".ktx", ".dds", ".zip"}) {
        if (extension == known) {
            return true;
        }
    }
    return false;
}

std::string_view folderPrefix(std::string_view folder, std::string& storage) {
    storage = folder;
    if (!storage.empty() && storage.back() != '/') {
        storage.push_back('/');
    }
    return storage;
}
} // namespace

bool validPackName(std::string_view name) {
    if (name.empty() || name.size() > std::numeric_limits<std::uint16_t>::max() || name.front() == '/' ||
        name.back() == '/' || name.find_first_of(std::string_view("\\:\0", 3)) != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto end = std::min(name.find('/', start), name.size());
        const auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

PackArchive::PackArchive(std::filesystem::path file) : file_(std::move(file)) {
    const auto fail = [this](const std::string& why) {
        return std::runtime_error("Invalid pak archive " + file_.string() + ": " + why);
    };
    std::error_code error;
    const auto fileSize = std::filesystem::file_size(file_, error);
    if (error) {
        throw std::runtime_error("Cannot open " + file_.string());
    }
    if (fileSize < HeaderSize) {
        throw fail("too small");
    }
    const auto headerBytes = readRange(file_, 0, HeaderSize);
    if (!std::equal(Magic.begin(), Magic.end(), headerBytes.begin())) {
        throw fail("not a pak file");
    }
    Reader header(std::string_view(headerBytes).substr(Magic.size()));
    if (header.get<std::uint32_t>() != Version) {
        throw fail("unsupported version");
    }
    const auto indexOffset = header.get<std::uint64_t>();
    const auto indexSize = header.get<std::uint64_t>();
    const auto count = header.get<std::uint32_t>();
    const auto indexCrc = header.get<std::uint32_t>();
    if (indexOffset < HeaderSize || indexOffset > fileSize || indexSize > fileSize - indexOffset ||
        indexSize > MaxIndexSize) {
        throw fail("index out of range");
    }
    const auto index = readRange(file_, indexOffset, indexSize);
    if (crc32(index) != indexCrc) {
        throw fail("index checksum mismatch");
    }
    try {
        Reader reader(index);
        entries_.reserve(std::min<std::uint64_t>(count, indexSize / 31));
        for (std::uint32_t i = 0; i < count; ++i) {
            Entry entry;
            entry.name = reader.text(reader.get<std::uint16_t>());
            entry.method = reader.get<std::uint8_t>();
            entry.offset = reader.get<std::uint64_t>();
            entry.storedSize = reader.get<std::uint64_t>();
            entry.size = reader.get<std::uint64_t>();
            entry.crc = reader.get<std::uint32_t>();
            if (!validPackName(entry.name)) {
                throw std::runtime_error("unsafe entry name '" + entry.name + "'");
            }
            if (!entries_.empty() && entries_.back().name >= entry.name) {
                throw std::runtime_error("entries are not sorted and unique at '" + entry.name + "'");
            }
            const bool stored = entry.method == Stored && entry.storedSize == entry.size;
            if (!(stored || entry.method == Zlib) || entry.size > MaxEntrySize ||
                entry.storedSize > MaxEntrySize || entry.offset < HeaderSize || entry.offset > indexOffset ||
                entry.storedSize > indexOffset - entry.offset) {
                throw std::runtime_error("bad entry '" + entry.name + "'");
            }
            entries_.push_back(std::move(entry));
        }
        if (!reader.done()) {
            throw std::runtime_error("trailing index bytes");
        }
    } catch (const std::runtime_error& error) {
        throw fail(error.what());
    }
}

const PackArchive::Entry* PackArchive::find(std::string_view name) const {
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), name,
                         [](const Entry& entry, std::string_view key) { return entry.name < key; });
    return found != entries_.end() && found->name == name ? &*found : nullptr;
}

bool PackArchive::hasFolder(std::string_view folder) const {
    std::string storage;
    const auto prefix = folderPrefix(folder, storage);
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), prefix,
                         [](const Entry& entry, std::string_view key) { return entry.name < key; });
    return found != entries_.end() && found->name.starts_with(prefix);
}

std::vector<std::string> PackArchive::list(std::string_view folder) const {
    std::string storage;
    const auto prefix = folderPrefix(folder, storage);
    std::vector<std::string> names;
    auto it = std::lower_bound(entries_.begin(), entries_.end(), prefix,
                               [](const Entry& entry, std::string_view key) { return entry.name < key; });
    for (; it != entries_.end() && it->name.starts_with(prefix); ++it) {
        const auto rest = std::string_view(it->name).substr(prefix.size());
        const auto slash = rest.find('/');
        names.emplace_back(rest.substr(0, slash == std::string_view::npos ? rest.size() : slash + 1));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string PackArchive::read(const Entry& entry) const {
    auto stored = readRange(file_, entry.offset, entry.storedSize);
    std::string bytes;
    if (entry.method == Stored) {
        bytes = std::move(stored);
    } else {
        bytes.resize(static_cast<std::size_t>(entry.size));
        const int written = stbi_zlib_decode_buffer(bytes.data(), static_cast<int>(bytes.size()),
                                                    stored.data(), static_cast<int>(stored.size()));
        if (written != static_cast<int>(entry.size)) {
            throw std::runtime_error("Cannot decompress " + entry.name + " in " + file_.string());
        }
    }
    if (crc32(bytes) != entry.crc) {
        throw std::runtime_error("Checksum mismatch for " + entry.name + " in " + file_.string());
    }
    return bytes;
}

PackWriter::PackWriter(std::filesystem::path output)
    : output_(std::move(output)), temporary_(std::filesystem::path(output_) += ".tmp") {
    if (output_.has_parent_path()) {
        std::filesystem::create_directories(output_.parent_path());
    }
    stream_.open(temporary_, std::ios::binary | std::ios::trunc);
    if (!stream_) {
        throw std::runtime_error("Cannot write " + temporary_.string());
    }
    stream_.write(std::string(HeaderSize, '\0').data(), HeaderSize);
}

PackWriter::~PackWriter() {
    if (!finished_) {
        stream_.close();
        std::error_code error;
        std::filesystem::remove(temporary_, error);
    }
}

void PackWriter::add(const std::string& name, std::string_view bytes, Compression compression) {
    if (finished_) {
        throw std::logic_error("PackWriter already finished");
    }
    if (!validPackName(name)) {
        throw std::runtime_error("Unsafe pak entry name: " + name);
    }
    if (bytes.size() > MaxEntrySize) {
        throw std::runtime_error("Pak entries are limited to 2 GiB: " + name);
    }
    if (!names_.insert(name).second) {
        throw std::runtime_error("Duplicate pak entry: " + name);
    }
    PackArchive::Entry entry;
    entry.name = name;
    entry.offset = static_cast<std::uint64_t>(stream_.tellp());
    entry.size = bytes.size();
    entry.crc = crc32(bytes);
    std::string_view data = bytes;
    std::unique_ptr<unsigned char, decltype(&std::free)> compressed(nullptr, std::free);
    const bool tryZlib = compression == Compression::Zlib ||
                         (compression == Compression::Auto && bytes.size() >= 64 && !alreadyCompressed(name));
    if (tryZlib) {
        int size = 0;
        // stb takes a mutable pointer but only reads the input.
        auto* input = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
        compressed.reset(stbi_zlib_compress(input, static_cast<int>(bytes.size()), &size, 8));
        const bool smaller = static_cast<std::uint64_t>(size) * 10 <= bytes.size() * 9;
        if (compressed && (compression == Compression::Zlib || smaller)) {
            entry.method = Zlib;
            data = {reinterpret_cast<const char*>(compressed.get()), static_cast<std::size_t>(size)};
        }
    }
    entry.storedSize = data.size();
    if (!stream_.write(data.data(), static_cast<std::streamsize>(data.size()))) {
        throw std::runtime_error("Cannot write " + temporary_.string());
    }
    entries_.push_back(std::move(entry));
}

void PackWriter::finish() {
    std::sort(entries_.begin(), entries_.end(),
              [](const PackArchive::Entry& a, const PackArchive::Entry& b) { return a.name < b.name; });
    std::string index;
    for (const auto& entry : entries_) {
        put<std::uint16_t>(index, entry.name.size());
        index += entry.name;
        put<std::uint8_t>(index, entry.method);
        put<std::uint64_t>(index, entry.offset);
        put<std::uint64_t>(index, entry.storedSize);
        put<std::uint64_t>(index, entry.size);
        put<std::uint32_t>(index, entry.crc);
    }
    const auto indexOffset = static_cast<std::uint64_t>(stream_.tellp());
    std::string header(Magic.begin(), Magic.end());
    put<std::uint32_t>(header, Version);
    put<std::uint64_t>(header, indexOffset);
    put<std::uint64_t>(header, index.size());
    put<std::uint32_t>(header, entries_.size());
    put<std::uint32_t>(header, crc32(index));
    stream_.write(index.data(), static_cast<std::streamsize>(index.size()));
    stream_.seekp(0);
    stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
    stream_.close();
    if (!stream_) {
        throw std::runtime_error("Cannot write " + temporary_.string());
    }
    replaceFile(temporary_, output_);
    finished_ = true;
}

std::size_t packFolder(const std::filesystem::path& folder, const std::filesystem::path& output) {
    if (!std::filesystem::is_directory(folder)) {
        throw std::runtime_error("Not a folder: " + folder.string());
    }
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            files.emplace_back(entry.path().lexically_relative(folder).generic_string(), entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    PackWriter writer(output);
    for (const auto& [name, path] : files) {
        writer.add(name, readRange(path, 0, std::filesystem::file_size(path)));
    }
    writer.finish();
    return files.size();
}
} // namespace fe
