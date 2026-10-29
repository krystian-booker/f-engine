#include "engine/assets/SpriteAnimation.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace fe {
SpriteAnimation SpriteAnimation::load(const std::filesystem::path& path) {
    return fromJson(readJson(path));
}
SpriteAnimation SpriteAnimation::fromJson(const Json& j) {
    SpriteAnimation a;
    a.atlas = j.at("atlas");
    a.normal = j.value("normal", std::string{});
    a.emissive = j.value("emissive", std::string{});
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
const std::vector<SpriteFrame>& SpriteClip::row(int direction) const {
    return directions[static_cast<std::size_t>(
        std::clamp(direction, 0, static_cast<int>(directions.size()) - 1))];
}

double SpriteClip::length(int direction) const {
    double total = 0;
    for (const auto& f : row(direction)) {
        total += f.duration;
    }
    return total;
}

const SpriteClip& SpriteAnimation::clip(const std::string& name) const {
    auto it = clips.find(name);
    if (it == clips.end()) {
        it = clips.find("idle");
    }
    if (it == clips.end()) {
        it = clips.begin();
    }
    return it->second;
}

const SpriteFrame& SpriteAnimation::frameFor(const std::string& name, int row, double elapsed) const {
    return clip(name).frameAt(row, elapsed);
}

const SpriteFrame& SpriteClip::frameAt(int direction, double elapsed) const {
    const auto& frames = row(direction);
    double time = std::max(0.0, elapsed);
    if (loop) {
        time = std::fmod(time, length(direction));
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
    const int count = static_cast<int>(clip(name).directions.size());
    const float angle = std::atan2(-facing.x, facing.y) + glm::radians(yaw);
    return (static_cast<int>(std::round(angle / (2 * std::numbers::pi_v<float>)*count)) % count + count) %
           count;
}

void SpriteAnimation::events(const std::string& name, int direction, double from, double to,
                             const std::function<void(const std::string&)>& emit) const {
    if (to <= from) {
        return;
    }
    const auto& clip = this->clip(name);
    const double length = clip.length(direction);
    constexpr int MaxLoops = 64; // events of older passes are dropped when a tick spans more
    const int last = clip.loop ? static_cast<int>(std::floor(std::max(0.0, to) / length)) : 0;
    const int first =
        clip.loop ? std::max(static_cast<int>(std::floor(std::max(0.0, from) / length)), last - MaxLoops) : 0;
    for (int loop = first; loop <= last; ++loop) {
        double time = loop * length;
        for (const auto& f : clip.row(direction)) {
            const bool reached = time > from || (time == 0 && from <= 0);
            if (reached && time <= to && !f.event.empty()) {
                emit(f.event);
            }
            time += f.duration;
        }
    }
}
} // namespace fe
