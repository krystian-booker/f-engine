#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace fe {
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    static Image load(const std::filesystem::path& path);
    static Image decode(std::span<const std::uint8_t> bytes);
    static Image solid(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255);
    // The next mip level (each side halved, at least 1) with a 2x2 box filter.
    // `srgb` averages colour in linear light; alpha is always linear.
    [[nodiscard]] Image halved(bool srgb) const;
    // Writes a PNG with writeTextAtomic: flushed to disk, then renamed over `path`.
    void savePng(const std::filesystem::path& path) const;
};

// Header of a KTX 1 texture, as `fengine_assetc cook` writes them.
struct KtxInfo {
    enum class Format { Rgba8, Bc1, Bc3, Bc7, Other };
    Format format = Format::Other;
    bool srgb = false;
    int width = 0, height = 0, mips = 0;
};
// Nullopt unless `bytes` start with a little-endian KTX 1 header of a single 2D texture.
std::optional<KtxInfo> readKtxHeader(std::span<const std::uint8_t> bytes);
// The cooked `.ktx` next to an image file (same name, `.ktx` extension; loose
// or in a mounted pak), or an empty path when there is none.
std::filesystem::path cookedTexture(const std::filesystem::path& image);
} // namespace fe
