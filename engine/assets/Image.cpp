#include "engine/assets/Image.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <limits>
#include <memory>
#include <stb_image_write.h>
#include <stdexcept>

namespace fe {
namespace {
Image copyPixels(stbi_uc* raw, int width, int height) {
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(raw, stbi_image_free);
    if (!pixels)
        throw std::runtime_error(std::string("Image decode: ") +
                                 (stbi_failure_reason() ? stbi_failure_reason() : "unknown format error"));
    if (width < 1 || height < 1 || width > 16384 || height > 16384)
        throw std::runtime_error("Image dimensions exceed 16384");
    Image result;
    result.width = width;
    result.height = height;
    result.rgba.assign(pixels.get(), pixels.get() + static_cast<std::size_t>(width) * height * 4);
    return result;
}
} // namespace
Image Image::load(const std::filesystem::path& path) {
    int w = 0, h = 0, channels = 0;
    auto* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
    try {
        return copyPixels(pixels, w, h);
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
}
Image Image::decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Image buffer too large");
    int w = 0, h = 0, channels = 0;
    auto* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    return copyPixels(pixels, w, h);
}
Image Image::solid(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return {1, 1, {r, g, b, a}};
}
void Image::savePng(const std::filesystem::path& path) const {
    if (width < 1 || height < 1 || rgba.size() != static_cast<std::size_t>(width) * height * 4)
        throw std::runtime_error("Invalid image buffer");
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    if (!stbi_write_png(path.string().c_str(), width, height, 4, rgba.data(), width * 4))
        throw std::runtime_error("Cannot write PNG " + path.string());
}
} // namespace fe
