#include "engine/assets/Image.h"
#include "engine/core/Json.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <array>
#include <cmath>
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

const std::array<float, 256>& srgbToLinear() {
    static const auto table = [] {
        std::array<float, 256> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            values[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return values;
    }();
    return table;
}

// The sRGB byte nearest to a linear value. Mip chains call this for every
// texel of every level, so it is a table lookup and a compare, not a pow():
// `edges[v]` is the linear value where the answer turns from v - 1 to v, and
// a coarse table gives a first guess that is never more than one step off
// above the dark end, where the loop walks the last few values.
std::uint8_t linearToSrgb(float c) {
    constexpr std::size_t Steps = 4096;
    struct Tables {
        std::array<float, 257> edges{};
        std::array<std::uint8_t, Steps + 1> guess{};
    };
    static const Tables tables = [] {
        Tables t;
        const auto decode = [](double encoded) {
            return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        };
        t.edges[0] = -std::numeric_limits<float>::infinity();
        for (std::size_t v = 1; v < 256; ++v) {
            t.edges[v] = static_cast<float>(decode((static_cast<double>(v) - 0.5) / 255.0));
        }
        t.edges[256] = std::numeric_limits<float>::infinity();
        std::size_t v = 0;
        for (std::size_t i = 0; i <= Steps; ++i) {
            const float linear = static_cast<float>(i) / static_cast<float>(Steps);
            while (linear >= t.edges[v + 1]) {
                ++v;
            }
            t.guess[i] = static_cast<std::uint8_t>(v);
        }
        return t;
    }();
    if (!(c > 0)) {
        return 0; // also NaN
    }
    if (c >= 1) {
        return 255;
    }
    std::size_t v = tables.guess[static_cast<std::size_t>(c * static_cast<float>(Steps))];
    while (c >= tables.edges[v + 1]) {
        ++v;
    }
    return static_cast<std::uint8_t>(v);
}
} // namespace
Image Image::load(const std::filesystem::path& path) {
    int w = 0, h = 0, channels = 0;
    const auto bytes = readFile(path);
    auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                         static_cast<int>(bytes.size()), &w, &h, &channels, 4);
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
Image Image::halved(bool srgb) const {
    Image out;
    out.width = std::max(1, width / 2);
    out.height = std::max(1, height / 2);
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    const auto& toLinear = srgbToLinear();
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const int x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
            const int y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
            const std::size_t samples[] = {
                static_cast<std::size_t>(y0 * width + x0) * 4, static_cast<std::size_t>(y0 * width + x1) * 4,
                static_cast<std::size_t>(y1 * width + x0) * 4, static_cast<std::size_t>(y1 * width + x1) * 4};
            auto* target = &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4];
            // Colour is weighted by alpha, so the (often black) colour of
            // transparent texels does not darken the edges of cutouts as the
            // mips get smaller; alpha itself is a plain average.
            float alphaSum = 0;
            for (const auto s : samples) {
                alphaSum += static_cast<float>(rgba[s + 3]) / 255.0f;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                float sum = 0;
                for (const auto s : samples) {
                    const auto value = rgba[s + c];
                    const float linear = srgb ? toLinear[value] : static_cast<float>(value) / 255.0f;
                    sum += alphaSum > 0 ? linear * static_cast<float>(rgba[s + 3]) / 255.0f : linear;
                }
                const float average = alphaSum > 0 ? sum / alphaSum : sum * 0.25f;
                target[c] = srgb ? linearToSrgb(average) : static_cast<std::uint8_t>(average * 255.0f + 0.5f);
            }
            target[3] = static_cast<std::uint8_t>(alphaSum * 0.25f * 255.0f + 0.5f);
        }
    }
    return out;
}

void Image::savePng(const std::filesystem::path& path) const {
    if (width < 1 || height < 1 || rgba.size() != static_cast<std::size_t>(width) * height * 4)
        throw std::runtime_error("Invalid image buffer");
    std::string png;
    const auto append = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                                   static_cast<std::size_t>(size));
    };
    if (!stbi_write_png_to_func(append, &png, width, height, 4, rgba.data(), width * 4))
        throw std::runtime_error("Cannot encode PNG " + path.string());
    writeTextAtomic(path, png);
}
} // namespace fe

namespace fe {
std::optional<KtxInfo> readKtxHeader(std::span<const std::uint8_t> bytes) {
    static constexpr std::uint8_t identifier[] = {0xAB, 'K',  'T',  'X',  ' ',  '1',
                                                  '1',  0xBB, '\r', '\n', 0x1A, '\n'};
    constexpr std::size_t headerSize = 64;
    if (bytes.size() < headerSize ||
        !std::equal(std::begin(identifier), std::end(identifier), bytes.begin())) {
        return std::nullopt;
    }
    auto word = [&](std::size_t index) {
        const auto* p = &bytes[12 + index * 4];
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
               (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
    };
    // endianness, glType, glTypeSize, glFormat, glInternalFormat, glBaseInternalFormat,
    // width, height, depth, array elements, faces, mips, key/value bytes
    if (word(0) != 0x04030201u || word(8) != 0 || word(9) != 0 || word(10) != 1) {
        return std::nullopt;
    }
    KtxInfo info;
    info.width = static_cast<int>(word(6));
    info.height = static_cast<int>(word(7));
    info.mips = static_cast<int>(std::max(1u, word(11)));
    if (info.width < 1 || info.height < 1 || info.width > 16384 || info.height > 16384) {
        return std::nullopt;
    }
    switch (word(4)) {
    case 0x8058: // GL_RGBA8
        info.format = KtxInfo::Format::Rgba8;
        break;
    case 0x8C43: // GL_SRGB8_ALPHA8
        info.format = KtxInfo::Format::Rgba8;
        info.srgb = true;
        break;
    case 0x83F1: // GL_COMPRESSED_RGBA_S3TC_DXT1
        info.format = KtxInfo::Format::Bc1;
        break;
    case 0x8C4D: // GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1
        info.format = KtxInfo::Format::Bc1;
        info.srgb = true;
        break;
    case 0x83F3: // GL_COMPRESSED_RGBA_S3TC_DXT5
        info.format = KtxInfo::Format::Bc3;
        break;
    case 0x8C4F: // GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5
        info.format = KtxInfo::Format::Bc3;
        info.srgb = true;
        break;
    case 0x8E8C: // GL_COMPRESSED_RGBA_BPTC_UNORM
        info.format = KtxInfo::Format::Bc7;
        break;
    case 0x8E8D: // GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
        info.format = KtxInfo::Format::Bc7;
        info.srgb = true;
        break;
    default:
        break;
    }
    return info;
}

std::filesystem::path cookedTexture(const std::filesystem::path& image) {
    if (image.empty() || image.extension() == ".ktx") {
        return {};
    }
    auto cooked = image;
    cooked.replace_extension(".ktx");
    return isFile(cooked) ? cooked : std::filesystem::path{};
}
} // namespace fe
