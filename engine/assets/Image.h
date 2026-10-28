#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace fe {
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    static Image load(const std::filesystem::path& path);
    static Image decode(std::span<const std::uint8_t> bytes);
    static Image solid(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255);
    void savePng(const std::filesystem::path& path) const;
};
} // namespace fe
