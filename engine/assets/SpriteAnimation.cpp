#include "engine/assets/SpriteAnimation.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace fe {
const std::vector<SpriteFrame>& SpriteClip::row(int direction) const {
    const int count = static_cast<int>(directions.size());
    return directions[static_cast<std::size_t>((direction % count + count) % count)];
}

SpriteAnimation SpriteAnimation::load(const std::filesystem::path& path) {
    return fromJson(readJson(path));
}
SpriteAnimation SpriteAnimation::fromJson(const Json& j) {
    SpriteAnimation a;
    a.atlas = j.at("atlas");
    a.pixelsPerUnit = j.value("pixels_per_unit", 32);
    if (a.atlas.empty() || a.pixelsPerUnit < 1 || a.pixelsPerUnit > 4096)
        throw std::runtime_error("Invalid sprite atlas/PPU");
    if (j.contains("pivot"))
        a.pivot = {j.at("pivot").at(0).get<float>(), j.at("pivot").at(1).get<float>()};
    for (const auto& [name, c] : j.at("clips").items()) {
        SpriteClip clip;
        clip.loop = c.value("loop", true);
        for (const auto& direction : c.at("directions")) {
            std::vector<SpriteFrame> frames;
            for (const auto& f : direction) {
                SpriteFrame frame;
                const auto& r = f.at("rect");
                if (!r.is_array() || r.size() != 4)
                    throw std::runtime_error("Sprite rect requires x,y,w,h");
                frame.rect = {r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), r[3].get<int>()};
                frame.duration = f.value("duration", 0.15);
                frame.event = f.value("event", "");
                if (!std::isfinite(frame.duration) || frame.duration <= 0 || frame.rect.x < 0 ||
                    frame.rect.y < 0 || frame.rect.z < 1 || frame.rect.w < 1)
                    throw std::runtime_error("Invalid sprite frame");
                frames.push_back(frame);
            }
            if (frames.empty())
                throw std::runtime_error("Sprite direction has no frames");
            clip.directions.push_back(std::move(frames));
        }
        if (clip.directions.size() != 1 && clip.directions.size() != 4 && clip.directions.size() != 8)
            throw std::runtime_error("Sprite clip needs 1, 4, or 8 directions");
        a.clips[name] = std::move(clip);
    }
    if (a.clips.empty())
        throw std::runtime_error("Sprite needs at least one clip");
    return a;
}
const SpriteFrame& SpriteAnimation::frame(const std::string& name, glm::vec2 facing, float yaw,
                                          double elapsed) const {
    // Rows run south, west, north, east (then diagonals for eight directions),
    // clockwise relative to the camera, with south facing the viewer.
    return frameFor(name, direction(name, facing, yaw), elapsed);
}

const SpriteFrame& SpriteAnimation::frameFor(const std::string& name, int row, double elapsed) const {
    auto it = clips.find(name);
    if (it == clips.end()) {
        it = clips.find("idle");
    }
    if (it == clips.end()) {
        it = clips.begin();
    }
    const auto& clip = it->second;
    const auto& frames = clip.row(row);
    double duration = 0;
    for (const auto& f : frames) {
        duration += f.duration;
    }
    double time = std::max(0.0, elapsed);
    if (clip.loop) {
        time = std::fmod(time, duration);
    }
    for (const auto& f : frames) {
        if (time < f.duration) {
            return f;
        }
        time -= f.duration;
    }
    return frames.back();
}

int SpriteAnimation::direction(const std::string& name, glm::vec2 facing, float yaw) const {
    auto it = clips.find(name);
    if (it == clips.end())
        it = clips.find("idle");
    if (it == clips.end())
        it = clips.begin();
    const int count = static_cast<int>(it->second.directions.size());
    const float angle = std::atan2(-facing.x, facing.y) + glm::radians(yaw);
    return (static_cast<int>(std::round(angle / (2 * std::numbers::pi_v<float>)*count)) % count + count) %
           count;
}
std::vector<std::string> SpriteAnimation::events(const std::string& name, int direction, double from,
                                                 double to) const {
    std::vector<std::string> result;
    auto found = clips.find(name);
    if (found == clips.end() || to <= from)
        return result;
    const auto& clip = found->second;
    const auto& frames = clip.row(direction);
    double length = 0;
    for (const auto& f : frames)
        length += f.duration;
    const int first = clip.loop ? static_cast<int>(std::floor(std::max(0.0, from) / length)) : 0;
    const int last = clip.loop ? static_cast<int>(std::floor(std::max(0.0, to) / length)) : 0;
    if (last - first > 1000)
        throw std::runtime_error("Sprite event interval too large");
    for (int loop = first; loop <= last; ++loop) {
        double time = loop * length;
        for (const auto& f : frames) {
            if (time > from && time <= to && !f.event.empty())
                result.push_back(f.event);
            time += f.duration;
        }
    }
    return result;
}
} // namespace fe
