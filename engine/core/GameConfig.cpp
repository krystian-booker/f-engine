#include "engine/core/GameConfig.h"
#include "engine/core/Json.h"

namespace fe {
GameConfig GameConfig::load(const std::filesystem::path& folder) {
    const auto path = folder / "game.json";
    try {
        const auto data = readJson(path);
        GameConfig config;
        config.title = data.value("title", config.title);
        config.entry = data.value("entry", config.entry);
        const auto window = data.value("window", Json::object());
        config.windowWidth = window.value("width", config.windowWidth);
        config.windowHeight = window.value("height", config.windowHeight);
        const auto resolution = data.value("virtual_resolution", Json::object());
        config.virtualWidth = resolution.value("width", config.virtualWidth);
        config.virtualHeight = resolution.value("height", config.virtualHeight);
        config.pixelsPerUnit = data.value("pixels_per_unit", config.pixelsPerUnit);
        config.pauseOnFocusLoss = data.value("pause_on_focus_loss", config.pauseOnFocusLoss);
        const bool valid =
            !config.title.empty() && !config.entry.empty() && config.pixelsPerUnit >= 1 &&
            config.pixelsPerUnit <= 4096 && config.windowWidth >= 64 && config.windowHeight >= 64 &&
            config.windowWidth <= 16384 && config.windowHeight <= 16384 && config.virtualWidth >= 16 &&
            config.virtualHeight >= 16 && config.virtualWidth <= 16384 && config.virtualHeight <= 16384;
        if (!valid) {
            throw std::runtime_error("Invalid title, entry, dimensions or pixels_per_unit");
        }
        return config;
    } catch (const std::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}
} // namespace fe
