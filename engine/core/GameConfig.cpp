#include "engine/core/GameConfig.h"
#include "engine/core/Json.h"

namespace fe {
GameConfig GameConfig::load(const std::filesystem::path& folder) {
    const auto path = folder / "game.json";
    try {
        const auto data = readJson(path);
        GameConfig config;
        config.title = data.value("title", config.title);
        auto window = data.value("window", Json::object());
        config.windowWidth = window.value("width", config.windowWidth);
        config.windowHeight = window.value("height", config.windowHeight);
        auto resolution = data.value("virtual_resolution", Json::object());
        config.virtualWidth = resolution.value("width", config.virtualWidth);
        config.virtualHeight = resolution.value("height", config.virtualHeight);
        config.pixelsPerUnit = data.value("pixels_per_unit", config.pixelsPerUnit);
        config.startMap = data.value("start_map", config.startMap);
        config.startSpawn = data.value("start_spawn", config.startSpawn);
        config.battleEffectSprite=data.value("battle_effect_sprite",std::string{});
        if (config.title.empty() || config.startMap.empty() || config.pixelsPerUnit < 1 ||
            config.pixelsPerUnit > 4096 || config.windowWidth < 64 || config.windowHeight < 64 ||
            config.windowWidth > 16384 || config.windowHeight > 16384 ||
            config.virtualWidth < 1 || config.virtualHeight < 1 ||
            config.virtualWidth > 16384 || config.virtualHeight > 16384)
            throw std::runtime_error("Invalid title, dimensions, pixels_per_unit, or start_map");
        return config;
    } catch (const std::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}
}
