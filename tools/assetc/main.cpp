#include "engine/assets/Image.h"
#include "engine/assets/SpriteAnimation.h"
#include "engine/core/Json.h"
#include "engine/core/Pack.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <string_view>
#ifdef FENGINE_ASSETC_COOK
#include <bimg/bimg.h>
#include <bimg/encode.h>
#include <bx/allocator.h>
#include <bx/readerwriter.h>
#endif

// Asset authoring helpers. Game validation lives in `fengine_headless --check`.
namespace {
using fe::Json;

struct Packed {
    std::vector<glm::ivec4> rectangles;
    fe::Image atlas;
};

Packed pack(const std::vector<fe::Image>& images, int width, int height, int padding) {
    std::vector<std::size_t> order(images.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](auto a, auto b) { return images[a].height > images[b].height; });
    Packed result;
    result.rectangles.resize(images.size());
    result.atlas.width = width;
    result.atlas.height = height;
    result.atlas.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    int x = 0, y = 0, rowHeight = 0;
    for (const auto i : order) {
        const auto& image = images[i];
        const int w = image.width + padding * 2;
        const int h = image.height + padding * 2;
        if (w > width || h > height) {
            throw std::runtime_error("A frame is larger than the atlas");
        }
        if (x + w > width) {
            x = 0;
            y += rowHeight;
            rowHeight = 0;
        }
        if (y + h > height) {
            throw std::runtime_error("Atlas is full; increase its dimensions");
        }
        result.rectangles[i] = {x + padding, y + padding, image.width, image.height};
        // Edge pixels are repeated into the padding to stop filtering bleed.
        for (int py = -padding; py < image.height + padding; ++py) {
            for (int px = -padding; px < image.width + padding; ++px) {
                const auto source =
                    (static_cast<std::size_t>(std::clamp(py, 0, image.height - 1)) * image.width +
                     static_cast<std::size_t>(std::clamp(px, 0, image.width - 1))) *
                    4;
                const auto target =
                    (static_cast<std::size_t>(y + padding + py) * width + x + padding + px) * 4;
                std::copy_n(image.rgba.data() + source, 4, result.atlas.rgba.data() + target);
            }
        }
        x += w;
        rowHeight = std::max(rowHeight, h);
    }
    return result;
}

// Packs individual frame images into atlas.png + atlas.anim.json.
void atlas(const std::filesystem::path& manifest, const std::filesystem::path& output) {
    const auto data = fe::readJson(manifest);
    const int width = data.value("width", 512), height = data.value("height", 512);
    const int padding = data.value("padding", 1), directions = data.value("directions", 4);
    if (width < 1 || height < 1 || width > 8192 || height > 8192 || padding < 0 || padding > 16 ||
        (directions != 1 && directions != 4 && directions != 8)) {
        throw std::runtime_error("Invalid atlas dimensions, padding or directions");
    }
    const auto& frames = data.at("frames");
    if (!frames.is_array() || frames.empty() || frames.size() > 4096) {
        throw std::runtime_error("An atlas needs 1..4096 frames");
    }
    // Optional per-clip settings: {"clips": {"attack": {"loop": false}}}.
    const auto clipSettings = data.value("clips", Json::object());
    std::vector<fe::Image> images;
    for (const auto& frame : frames) {
        images.push_back(fe::Image::load(manifest.parent_path() / frame.at("file").get<std::string>()));
    }
    const auto packed = pack(images, width, height, padding);
    Json animation{{"atlas", "atlas.png"},
                   {"pixels_per_unit", data.value("pixels_per_unit", 32)},
                   {"clips", Json::object()}};
    Json regions = Json::object();
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        const auto clip = frame.value("clip", std::string("idle"));
        const int direction = frame.value("direction", 0);
        if (direction < 0 || direction >= directions) {
            throw std::runtime_error("Frame direction out of range");
        }
        auto& target = animation["clips"][clip];
        if (!target.contains("loop")) {
            target["loop"] = clipSettings.value(clip, Json::object()).value("loop", true);
        }
        if (!target.contains("directions")) {
            target["directions"] = Json::array();
            for (int d = 0; d < directions; ++d) {
                target["directions"].push_back(Json::array());
            }
        }
        const auto r = packed.rectangles[i];
        Json record{{"rect", {r.x, r.y, r.z, r.w}}, {"duration", frame.value("duration", 0.15)}};
        if (frame.contains("event")) {
            record["event"] = frame.at("event");
        }
        target["directions"][static_cast<std::size_t>(direction)].push_back(record);
        regions[frame.value("id", std::to_string(i))] = record["rect"];
    }
    fe::SpriteAnimation::fromJson(animation);
    std::filesystem::create_directories(output);
    packed.atlas.savePng(output / "atlas.png");
    fe::writeJson(output / "atlas.anim.json", animation);
    fe::writeJson(output / "regions.json", regions);
}

// Describes a grid-aligned sprite sheet as a .anim.json file.
void sheet(const std::filesystem::path& manifest, const std::filesystem::path& output) {
    const auto source = fe::readJson(manifest);
    const auto atlasName = source.at("atlas").get<std::string>();
    const auto image = fe::Image::load(manifest.parent_path() / atlasName);
    const int w = source.at("frame_width"), h = source.at("frame_height");
    const int directions = source.value("directions", 4);
    if (w < 1 || h < 1 || (directions != 1 && directions != 4 && directions != 8)) {
        throw std::runtime_error("Invalid sheet frame size or direction count");
    }
    Json result{{"atlas", std::filesystem::path(atlasName).filename().string()},
                {"pixels_per_unit", source.value("pixels_per_unit", 32)},
                {"clips", Json::object()}};
    for (const auto& [name, clip] : source.at("clips").items()) {
        auto& out = result["clips"][name];
        out["loop"] = clip.value("loop", true);
        out["directions"] = Json::array();
        for (int direction = 0; direction < directions; ++direction) {
            Json frames = Json::array();
            for (const auto& frame : clip.at("frames")) {
                const int column = frame.get<int>() + clip.value("column_offset", 0);
                const int row = direction + clip.value("row_offset", 0);
                if (column < 0 || row < 0 || column >= image.width / w || row >= image.height / h) {
                    throw std::runtime_error("Frame outside the sprite sheet in clip " + name);
                }
                frames.push_back(
                    {{"rect", {column * w, row * h, w, h}}, {"duration", clip.value("duration", 0.15)}});
            }
            out["directions"].push_back(frames);
        }
    }
    fe::SpriteAnimation::fromJson(result);
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path());
    }
    fe::writeJson(output, result);
}

// `pack <folder> <output.pak>` and `list <archive.pak>`; -1 for other commands.
int packCommand(int argc, char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "pack" && argc == 4) {
        const auto files = fe::packFolder(argv[2], argv[3]);
        std::cout << "Packed " << files << " files into " << argv[3] << '\n';
        return 0;
    }
    if (command == "list" && argc == 3) {
        const fe::PackArchive archive(argv[2]);
        for (const auto& entry : archive.entries()) {
            std::cout << entry.name << '\t' << entry.size << '\t' << entry.storedSize << '\t'
                      << (entry.method == 0 ? "stored" : "zlib") << '\n';
        }
        return 0;
    }
    return -1;
}

#ifdef FENGINE_ASSETC_COOK
// The JSON of a .gltf, or the JSON chunk of a .glb.
Json gltfJson(const std::filesystem::path& file) {
    if (file.extension() == ".gltf") {
        return fe::readJson(file);
    }
    std::ifstream stream(file, std::ios::binary);
    std::array<std::uint32_t, 5> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), sizeof(header)) || header[0] != 0x46546C67u ||
        header[4] != 0x4E4F534Au) {
        throw std::runtime_error("Not a binary glTF: " + file.string());
    }
    std::string json(header[3], '\0');
    if (!stream.read(json.data(), static_cast<std::streamsize>(json.size()))) {
        throw std::runtime_error("Truncated binary glTF: " + file.string());
    }
    return Json::parse(json);
}

std::string decodeUri(const std::string& uri) {
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

// World and mesh textures under `root`: external images of glTF materials
// (normal maps are linear, the rest sRGB) and map backdrops and panoramas.
// Value: whether the image holds sRGB colour.
std::map<std::filesystem::path, bool> worldTextures(const std::filesystem::path& root) {
    std::map<std::filesystem::path, bool> found;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto& file = entry.path();
        const auto extension = file.extension();
        if (extension == ".gltf" || extension == ".glb") {
            const auto gltf = gltfJson(file);
            const auto images = gltf.value("images", Json::array());
            const auto textures = gltf.value("textures", Json::array());
            auto add = [&](const Json& reference, bool srgb) {
                if (!reference.is_object()) {
                    return;
                }
                const auto texture = reference.value("index", -1);
                if (texture < 0 || texture >= static_cast<int>(textures.size())) {
                    return;
                }
                const auto image = textures[static_cast<std::size_t>(texture)].value("source", -1);
                if (image < 0 || image >= static_cast<int>(images.size())) {
                    return;
                }
                const auto uri = images[static_cast<std::size_t>(image)].value("uri", std::string{});
                if (!uri.empty() && !uri.starts_with("data:")) {
                    found[(file.parent_path() / decodeUri(uri)).lexically_normal()] = srgb;
                }
            };
            for (const auto& material : gltf.value("materials", Json::array())) {
                add(material.value("pbrMetallicRoughness", Json::object()).value("baseColorTexture", Json()),
                    true);
                add(material.value("emissiveTexture", Json()), true);
                add(material.value("normalTexture", Json()), false);
            }
        } else if (file.filename() == "map.json") {
            const auto sky =
                fe::readJson(file).value("environment", Json::object()).value("sky", Json::object());
            auto add = [&](const std::string& image) {
                if (!image.empty() && !image.starts_with("@")) {
                    found[(root / image).lexically_normal()] = true;
                }
            };
            add(sky.value("panorama", std::string{}));
            for (const auto& backdrop : sky.value("backdrops", Json::array())) {
                add(backdrop.value("image", std::string{}));
            }
        }
    }
    return found;
}

// BC7 with a full mip chain (each level halved in linear light for sRGB colour).
void writeBc7(const fe::Image& source, bool srgb, const std::filesystem::path& output) {
    bx::DefaultAllocator allocator;
    std::vector<std::uint8_t> blocks;
    int levels = 0;
    for (fe::Image level = source;; level = level.halved(srgb)) {
        // The encoder reads whole 4x4 blocks: pad by repeating the edge.
        const int pw = (level.width + 3) / 4 * 4, ph = (level.height + 3) / 4 * 4;
        std::vector<float> padded(static_cast<std::size_t>(pw) * ph * 4);
        for (int y = 0; y < ph; ++y) {
            for (int x = 0; x < pw; ++x) {
                const auto* texel =
                    &level.rgba[(static_cast<std::size_t>(std::min(y, level.height - 1)) * level.width +
                                 std::min(x, level.width - 1)) *
                                4];
                for (int c = 0; c < 4; ++c) {
                    padded[(static_cast<std::size_t>(y) * pw + x) * 4 + c] = texel[c] / 255.0f;
                }
            }
        }
        const auto offset = blocks.size();
        blocks.resize(offset + static_cast<std::size_t>(pw / 4) * (ph / 4) * 16);
        bx::Error error;
        bimg::imageEncodeFromRgba32f(&allocator, blocks.data() + offset, padded.data(),
                                     static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph), 1,
                                     bimg::TextureFormat::BC7, bimg::Quality::Default, &error);
        if (!error.isOk()) {
            throw std::runtime_error("BC7 encoding failed");
        }
        ++levels;
        if (level.width == 1 && level.height == 1) {
            break;
        }
    }
    bx::MemoryBlock memory(&allocator);
    bx::MemoryWriter writer(&memory);
    bx::Error error;
    bimg::imageWriteKtx(&writer, bimg::TextureFormat::BC7, false, static_cast<std::uint32_t>(source.width),
                        static_cast<std::uint32_t>(source.height), 0, static_cast<std::uint8_t>(levels), 1,
                        srgb, blocks.data(), &error);
    if (!error.isOk()) {
        throw std::runtime_error("Cannot write " + output.string());
    }
    const auto size = static_cast<std::size_t>(writer.seek(0, bx::Whence::Current));
    auto temporary = output;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.write(static_cast<const char*>(memory.more(0)), static_cast<std::streamsize>(size))) {
            throw std::runtime_error("Cannot write " + temporary.string());
        }
    }
    fe::replaceFile(temporary, output);
}

void cook(const std::filesystem::path& root, bool force) {
    int written = 0, current = 0;
    for (const auto& [image, srgb] : worldTextures(root)) {
        if (image.extension() != ".png" || !std::filesystem::is_regular_file(image)) {
            continue;
        }
        auto output = image;
        output.replace_extension(".ktx");
        std::error_code error;
        if (!force &&
            std::filesystem::last_write_time(output, error) >= std::filesystem::last_write_time(image)) {
            ++current;
            continue;
        }
        writeBc7(fe::Image::load(image), srgb, output);
        std::cout << output.generic_string() << (srgb ? " (BC7 sRGB)\n" : " (BC7 linear)\n");
        ++written;
    }
    std::cout << written << " cooked, " << current << " up to date\n";
}
#endif

// `cook <folder> [--force]`; -1 for other commands.
int cookCommand([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
#ifdef FENGINE_ASSETC_COOK
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "cook" && (argc == 3 || (argc == 4 && std::string_view(argv[3]) == "--force"))) {
        cook(argv[2], argc == 4);
        return 0;
    }
#endif
    return -1;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (const int packed = packCommand(argc, argv); packed >= 0) {
            return packed;
        }
        if (const int cooked = cookCommand(argc, argv); cooked >= 0) {
            return cooked;
        }
        if (argc != 4) {
            std::cout << "Usage: fengine_assetc atlas manifest.json output-directory\n"
                         "       fengine_assetc sheet sheet.json output.anim.json\n"
                         "       fengine_assetc pack folder output.pak\n"
                         "       fengine_assetc list archive.pak   (name, size, stored size, method)\n"
#ifdef FENGINE_ASSETC_COOK
                         "       fengine_assetc cook game-folder [--force]\n"
#endif
                         "Validate a game with: fengine_headless --check <game>\n";
            return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 1;
        }
        const std::string_view command = argv[1];
        if (command == "atlas") {
            atlas(argv[2], argv[3]);
        } else if (command == "sheet") {
            sheet(argv[2], argv[3]);
        } else {
            throw std::runtime_error("Unknown command: " + std::string(command));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
