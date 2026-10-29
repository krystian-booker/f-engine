#pragma once
#include "engine/core/Json.h"
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace fe {
struct SpriteFrame {
    glm::ivec4 rect{0, 0, 32, 48};
    double duration = 0.15;
    std::string event;
};
// Direction rows, clockwise relative to the camera with row 0 facing the
// viewer: 4 rows are south, west, north, east; 8 rows are south, south-west,
// west, north-west, north, north-east, east, south-east.
struct SpriteClip {
    bool loop = true;
    std::vector<std::vector<SpriteFrame>> directions;
    // Frames for a direction row; out-of-range rows clamp to the nearest one.
    [[nodiscard]] const std::vector<SpriteFrame>& row(int direction) const;
    // Seconds one pass through a direction row takes.
    [[nodiscard]] double length(int direction) const;
    // The frame showing `elapsed` seconds into the clip (looping clips wrap,
    // others hold their last frame).
    [[nodiscard]] const SpriteFrame& frameAt(int direction, double elapsed) const;
};
struct SpriteAnimation {
    std::string atlas;
    // Optional images laid out like the atlas: a tangent-space normal map
    // (x right, y up, z towards the viewer) and an emissive colour mask.
    std::string normal, emissive;
    glm::vec2 pivot{0.5f, 1};
    int pixelsPerUnit = 32;
    std::map<std::string, SpriteClip> clips;
    static SpriteAnimation load(const std::filesystem::path& file);
    static SpriteAnimation fromJson(const Json& data);
    // Every lookup by clip name goes through clip(), so an unknown name plays,
    // fires the events of and finishes like the clip it falls back to.
    // Frame for an explicit direction row (fixed-direction sprites).
    [[nodiscard]] const SpriteFrame& frameFor(const std::string& clip, int direction, double elapsed) const;
    [[nodiscard]] int direction(const std::string& clip, glm::vec2 facing, float cameraYaw) const;
    // Calls `emit` for every frame event reached in (from, to]; a frame that
    // starts at time 0 also fires when `from` is 0 (the clip just started).
    void events(const std::string& clip, int direction, double from, double to,
                const std::function<void(const std::string&)>& emit) const;
    // Clip by name, falling back to "idle" and then to the first clip.
    [[nodiscard]] const SpriteClip& clip(const std::string& name) const;
};
} // namespace fe
