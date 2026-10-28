#include "engine/assets/Image.h"
#include "engine/assets/SpriteAnimation.h"
#include "engine/core/Json.h"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <string_view>

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
    fe::writeJson(output, result);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            std::cout << "Usage: fengine_assetc atlas manifest.json output-directory\n"
                         "       fengine_assetc sheet sheet.json output.anim.json\n"
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
