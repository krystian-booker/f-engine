#include "engine/core/GameConfig.h"
#include "engine/core/Json.h"

namespace fe {
namespace {
constexpr std::string_view IdCharacters = "abcdefghijklmnopqrstuvwxyz0123456789_-";
} // namespace

bool validGameId(std::string_view id) {
    return !id.empty() && id.size() <= 64 && id.find_first_not_of(IdCharacters) == std::string_view::npos;
}

std::optional<std::string> derivedGameId(std::string_view title) {
    std::string id;
    for (const char c : title) {
        if (c == ' ') {
            if (!id.empty() && !id.ends_with('-')) {
                id += '-';
            }
        } else if (c >= 'A' && c <= 'Z') {
            id += static_cast<char>(c - 'A' + 'a');
        } else if (IdCharacters.find(c) != std::string_view::npos) {
            id += c;
        } else {
            return std::nullopt;
        }
    }
    while (id.ends_with('-')) {
        id.pop_back();
    }
    if (!validGameId(id)) {
        return std::nullopt;
    }
    return id;
}

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
        config.maxFps = data.value("max_fps", config.maxFps);
        if (data.contains("hdr_output") && !data.at("hdr_output").is_boolean()) {
            throw std::runtime_error("hdr_output must be true or false");
        }
        config.hdrOutput = data.value("hdr_output", config.hdrOutput);
        if (data.contains("hdr_paper_white") && !data.at("hdr_paper_white").is_number()) {
            throw std::runtime_error("hdr_paper_white must be a number of nits");
        }
        config.hdrPaperWhite = data.value("hdr_paper_white", config.hdrPaperWhite);
        if (!(config.hdrPaperWhite >= 80 && config.hdrPaperWhite <= 10000)) {
            throw std::runtime_error("hdr_paper_white is 80 to 10000 nits");
        }
        const auto fit = data.value("canvas_fit", std::string("letterbox"));
        if (fit == "letterbox") {
            config.canvasFit = CanvasFit::Letterbox;
        } else if (fit == "expand") {
            config.canvasFit = CanvasFit::Expand;
        } else {
            throw std::runtime_error("canvas_fit must be \"letterbox\" or \"expand\", got \"" + fit + "\"");
        }
        const auto scale = data.value("canvas_scale", std::string("integer"));
        if (scale != "integer" && scale != "smooth") {
            throw std::runtime_error("canvas_scale must be \"integer\" or \"smooth\", got \"" + scale + "\"");
        }
        config.canvasSmooth = scale == "smooth";
        const bool valid =
            !config.title.empty() && !config.entry.empty() && config.pixelsPerUnit >= 1 &&
            config.pixelsPerUnit <= 4096 && config.windowWidth >= 64 && config.windowHeight >= 64 &&
            config.windowWidth <= 16384 && config.windowHeight <= 16384 && config.virtualWidth >= 16 &&
            config.virtualHeight >= 16 && config.virtualWidth <= 16384 && config.virtualHeight <= 16384;
        if (!valid) {
            throw std::runtime_error("Invalid title, entry, dimensions or pixels_per_unit");
        }
        if (config.maxFps < 0 || config.maxFps > 1000) {
            throw std::runtime_error("max_fps is 0 (uncapped) or 1 to 1000");
        }
        if (data.contains("id")) {
            if (!data.at("id").is_string() || !validGameId(data.at("id").get<std::string>())) {
                throw std::runtime_error("id must be 1 to 64 of a-z, 0-9, '_' and '-'");
            }
            config.id = data.at("id").get<std::string>();
        } else if (const auto derived = derivedGameId(config.title)) {
            config.id = *derived;
        } else {
            throw std::runtime_error("The title \"" + config.title +
                                     "\" has characters outside a-z, 0-9, '_', '-' and spaces, so game.json "
                                     "needs an \"id\" (1 to 64 of a-z, 0-9, '_' and '-') naming the "
                                     "per-user folder");
        }
        return config;
    } catch (const std::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}
} // namespace fe
