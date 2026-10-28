#pragma once
#include "engine/core/Json.h"
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace fe {
struct SpriteFrame { glm::ivec4 rect{0,0,32,48}; double duration = 0.15; std::string event; };
struct SpriteClip { bool loop = true; std::vector<std::vector<SpriteFrame>> directions; };
struct SpriteAnimation {
    std::string atlas;
    glm::vec2 pivot{0.5f,1};
    int pixelsPerUnit = 32;
    std::map<std::string,SpriteClip> clips;
    static SpriteAnimation load(const std::filesystem::path& file);
    static SpriteAnimation fromJson(const Json& data);
    [[nodiscard]] const SpriteFrame& frame(const std::string& clip, glm::vec2 facing, float cameraYaw, double elapsed) const;
    [[nodiscard]] int direction(const std::string& clip,glm::vec2 facing,float cameraYaw) const;
    [[nodiscard]] std::vector<std::string> events(const std::string& clip, int direction, double from, double to) const;
};
}
