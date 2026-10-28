#pragma once
#include <filesystem>
#include <string>

namespace fe {
struct GameConfig {
    std::string title = "f-engine";
    int windowWidth = 1280, windowHeight = 720;
    int virtualWidth = 480, virtualHeight = 270;
    int pixelsPerUnit = 32;
    std::string startMap = "town";
    std::string startSpawn = "entry";
    std::string battleEffectSprite;
    static GameConfig load(const std::filesystem::path& gameFolder);
};
}
