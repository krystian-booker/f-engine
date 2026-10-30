#pragma once
#include "engine/core/Json.h"
#include <string>
#include <vector>

namespace fe {
struct Image;

// Describes a uniform sprite sheet (frames on a grid of equal cells) as an
// .anim.json document. Shared by `fengine_assetc sheet` (from a manifest)
// and the editor's sprite sheet wizard (from a form).
struct SheetClip {
    std::string name;
    std::vector<int> frames; // column indices
    bool loop = true;
    double duration = 0.15;
    int rowOffset = 0, columnOffset = 0;
};
struct SheetSpec {
    std::string atlas; // the image's file name, as written into the .anim.json
    int frameWidth = 32, frameHeight = 48;
    int directions = 4; // 1, 4 or 8 rows per clip
    int pixelsPerUnit = 32;
    std::vector<SheetClip> clips;
};
// The manifest form `fengine_assetc sheet` reads: {atlas, frame_width,
// frame_height, directions, pixels_per_unit, clips{name = {frames, loop,
// duration, row_offset, column_offset}}}.
[[nodiscard]] SheetSpec sheetSpecFromJson(const Json& manifest);
// The .anim.json for a spec over an image of `imageWidth` x `imageHeight`
// pixels; throws std::runtime_error for frames outside the image or an
// invalid frame size or direction count.
[[nodiscard]] Json buildSheetAnimation(const SheetSpec& spec, int imageWidth, int imageHeight);
// How many columns and rows of frames an image holds for a spec.
[[nodiscard]] int sheetColumns(const SheetSpec& spec, int imageWidth);
[[nodiscard]] int sheetRows(const SheetSpec& spec, int imageHeight);
} // namespace fe
