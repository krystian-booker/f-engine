#include "engine/assets/SpriteSheetBuilder.h"
#include "engine/assets/SpriteAnimation.h"
#include <filesystem>
#include <stdexcept>

namespace fe {
SheetSpec sheetSpecFromJson(const Json& manifest) {
    SheetSpec spec;
    spec.atlas = manifest.at("atlas").get<std::string>();
    spec.frameWidth = manifest.at("frame_width");
    spec.frameHeight = manifest.at("frame_height");
    spec.directions = manifest.value("directions", 4);
    spec.pixelsPerUnit = manifest.value("pixels_per_unit", 32);
    for (const auto& [name, clip] : manifest.at("clips").items()) {
        SheetClip out;
        out.name = name;
        for (const auto& frame : clip.at("frames")) {
            out.frames.push_back(frame.get<int>());
        }
        out.loop = clip.value("loop", true);
        out.duration = clip.value("duration", 0.15);
        out.rowOffset = clip.value("row_offset", 0);
        out.columnOffset = clip.value("column_offset", 0);
        spec.clips.push_back(std::move(out));
    }
    return spec;
}

int sheetColumns(const SheetSpec& spec, int imageWidth) {
    return spec.frameWidth > 0 ? imageWidth / spec.frameWidth : 0;
}
int sheetRows(const SheetSpec& spec, int imageHeight) {
    return spec.frameHeight > 0 ? imageHeight / spec.frameHeight : 0;
}

Json buildSheetAnimation(const SheetSpec& spec, int imageWidth, int imageHeight) {
    const int w = spec.frameWidth, h = spec.frameHeight;
    if (w < 1 || h < 1 || (spec.directions != 1 && spec.directions != 4 && spec.directions != 8)) {
        throw std::runtime_error("Invalid sheet frame size or direction count");
    }
    if (spec.clips.empty()) {
        throw std::runtime_error("A sheet needs at least one clip");
    }
    Json result{{"atlas", std::filesystem::path(spec.atlas).filename().string()},
                {"pixels_per_unit", spec.pixelsPerUnit},
                {"clips", Json::object()}};
    const int columns = sheetColumns(spec, imageWidth), rows = sheetRows(spec, imageHeight);
    for (const auto& clip : spec.clips) {
        if (clip.name.empty()) {
            throw std::runtime_error("A clip needs a name");
        }
        if (clip.frames.empty()) {
            throw std::runtime_error("Clip " + clip.name + " has no frames");
        }
        auto& out = result["clips"][clip.name];
        out["loop"] = clip.loop;
        out["directions"] = Json::array();
        for (int direction = 0; direction < spec.directions; ++direction) {
            Json frames = Json::array();
            for (const int frame : clip.frames) {
                const int column = frame + clip.columnOffset;
                const int row = direction + clip.rowOffset;
                if (column < 0 || row < 0 || column >= columns || row >= rows) {
                    throw std::runtime_error("Frame outside the sprite sheet in clip " + clip.name);
                }
                frames.push_back({{"rect", {column * w, row * h, w, h}}, {"duration", clip.duration}});
            }
            out["directions"].push_back(frames);
        }
    }
    SpriteAnimation::fromJson(result); // the result must load
    return result;
}
} // namespace fe
