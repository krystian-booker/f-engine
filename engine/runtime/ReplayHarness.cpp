#include "engine/runtime/ReplayHarness.h"
#include "engine/assets/Image.h"
#include "engine/core/FixedStep.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iostream>
#include <set>

namespace fe {
namespace {
const std::set<std::string> Known = {"tick",    "input",   "lua",       "assert", "message", "wait_for",
                                     "timeout", "mouse",   "wheel",     "text",   "cursor",  "pads",
                                     "capture", "compare", "tolerance", "resize"};

// A `pads` record entry, as ReplayRecorder writes it.
Json padJson(const InputState::PadEvent& event) {
    return {{"connected", event.connected}, {"pad", event.pad.number},
            {"name", event.pad.name},       {"type", event.pad.type},
            {"rumble", event.pad.rumble},   {"trigger_rumble", event.pad.triggerRumble}};
}

void applyPads(Engine& engine, const Json& pads) {
    for (const auto& entry : pads) {
        const int number = entry.at("pad").get<int>();
        if (number < 1 || number > 64) {
            throw std::runtime_error("Replay pads: gamepad numbers are 1 to 64, got " + std::to_string(number));
        }
        if (!entry.value("connected", true)) {
            engine.input().disconnectPad(number);
            continue;
        }
        InputState::Pad pad;
        pad.number = number;
        pad.name = entry.value("name", std::string("Gamepad"));
        pad.type = entry.value("type", std::string("generic"));
        pad.rumble = entry.value("rumble", false);
        pad.triggerRumble = entry.value("trigger_rumble", false);
        engine.input().connectPad(std::move(pad));
    }
}
} // namespace

ReplayHarness::ReplayHarness(const std::filesystem::path& file) : folder_(file.parent_path()) {
    auto document = readJson(file);
    if (document.is_object()) {
        if (document.contains("seed")) {
            seed_ = document.at("seed").get<std::uint64_t>();
        }
        commands_ = document.value("commands", Json());
    } else {
        commands_ = std::move(document);
    }
    if (!commands_.is_array()) {
        throw std::runtime_error("A replay is an array of tick-stamped commands: " + file.string());
    }
    std::uint64_t previous = 0;
    for (const auto& command : commands_) {
        const auto tick = command.at("tick").get<std::uint64_t>();
        if (tick < previous) {
            throw std::runtime_error("Replay commands must be sorted by tick: " + file.string());
        }
        previous = tick;
        for (auto field = command.begin(); field != command.end(); ++field) {
            if (!Known.contains(field.key())) {
                throw std::runtime_error("Unknown replay field '" + field.key() + "' in " + file.string());
            }
        }
        if (command.contains("compare") && !command.contains("capture")) {
            throw std::runtime_error("A replay 'compare' needs a 'capture' in the same command: " +
                                     file.string());
        }
        if (command.contains("cursor") && !command.contains("text")) {
            throw std::runtime_error("A replay 'cursor' needs a 'text' in the same command: " +
                                     file.string());
        }
    }
}

std::uint64_t ReplayHarness::lastTick() const {
    return commands_.empty() ? 0 : commands_.back().at("tick").get<std::uint64_t>() + offset_;
}

void ReplayHarness::feedInputs(Engine& engine, const Json& inputs) {
    if (inputs.is_array()) {
        for (const auto& pair : inputs) {
            engine.input().feed(pair.at(0).get<std::string>(), pair.at(1).get<float>());
        }
        return;
    }
    for (const auto& [input, value] : inputs.items()) {
        engine.input().feed(input, value.get<float>());
    }
}

bool ReplayHarness::evaluate(Engine& engine, const std::string& expression, std::string& detail) {
    detail = engine.lua().console("(" + expression + ") == true");
    return detail == "true";
}

ReplayHarness::Effects ReplayHarness::apply(std::uint64_t tick, Engine& engine) {
    Effects effects;
    while (next_ < commands_.size()) {
        const auto& command = commands_[next_];
        if (command.at("tick").get<std::uint64_t>() + offset_ > tick) {
            break;
        }
        if (command.contains("wait_for")) {
            if (!waiting_) {
                waiting_ = true;
                waitStarted_ = tick;
            }
            std::string detail;
            const bool satisfied = evaluate(engine, command.at("wait_for").get<std::string>(), detail);
            if (!satisfied && tick - waitStarted_ <= command.value("timeout", 600u)) {
                break;
            }
            if (!satisfied) {
                // Timed out: the failure is recorded and the command's other
                // fields still run, as they would have after the wait.
                failures_.push_back("Timed out waiting for: " + command.at("wait_for").get<std::string>() +
                                    " (" + detail + ")");
            }
            offset_ += tick - waitStarted_;
            waiting_ = false;
        }
        if (command.contains("pads")) {
            applyPads(engine, command.at("pads"));
        }
        if (command.contains("input")) {
            feedInputs(engine, command.at("input"));
        }
        if (command.contains("lua")) {
            const auto code = command.at("lua").get<std::string>();
            const auto before = engine.lua().errors().size();
            const auto result = engine.lua().evaluate(code);
            if (!result.ok || engine.lua().errors().size() > before) {
                failures_.push_back("Replay Lua failed: " + code + "\n" + result.output);
            }
        }
        if (command.contains("assert")) {
            std::string detail;
            const auto expression = command.at("assert").get<std::string>();
            if (!evaluate(engine, expression, detail)) {
                failures_.push_back(command.value("message", "Assertion failed") + ": " + expression + " (" +
                                    detail + ")");
            }
        }
        if (command.contains("mouse")) {
            const auto& position = command.at("mouse");
            engine.input().feedMouse({position.at(0).get<float>(), position.at(1).get<float>()});
        }
        if (command.contains("wheel")) {
            engine.input().feedWheel(command.at("wheel").get<float>());
        }
        if (command.contains("text")) {
            auto text = command.at("text").get<std::string>();
            if (command.contains("cursor")) {
                engine.input().feedComposition(std::move(text), command.at("cursor").get<int>());
            } else {
                engine.input().feedText(text);
            }
        }
        if (command.contains("capture")) {
            effects.capture = command.at("capture").get<std::string>();
        }
        if (command.contains("compare")) {
            effects.reference = folder_ / command.at("compare").get<std::string>();
            effects.tolerance = command.value("tolerance", effects.tolerance);
        }
        if (command.contains("resize")) {
            effects.resize =
                std::make_pair(command.at("resize").at(0).get<int>(), command.at("resize").at(1).get<int>());
        }
        ++next_;
    }
    return effects;
}

int ReplayHarness::report(Engine& engine) const {
    int code = 0;
    for (const auto& failure : failures_) {
        std::cerr << "REPLAY FAILURE: " << failure << "\n";
        code = 1;
    }
    if (!finished()) {
        std::cerr << "REPLAY FAILURE: stopped before every command ran\n";
        code = 1;
    }
    if (!engine.lua().errors().empty()) {
        std::cerr << "REPLAY FAILURE: " << engine.lua().errors().size() << " script error(s)\n";
        code = 1;
    }
    return code;
}

ImageDifference compareImages(const Image& actual, const Image& reference) {
    if (actual.width != reference.width || actual.height != reference.height ||
        actual.rgba.size() != reference.rgba.size() || actual.rgba.empty()) {
        return {1, 1};
    }
    double total = 0;
    int largest = 0;
    for (std::size_t i = 0; i < actual.rgba.size(); ++i) {
        const int difference = std::abs(int(actual.rgba[i]) - int(reference.rgba[i]));
        total += difference;
        largest = std::max(largest, difference);
    }
    return {total / (255.0 * static_cast<double>(actual.rgba.size())), largest / 255.0};
}

Image resizedTo(const Image& image, int width) {
    if (image.width < 1 || image.height < 1) {
        return image;
    }
    Image out;
    out.width = width;
    out.height =
        std::max(1, static_cast<int>(std::lround(static_cast<double>(image.height) * width / image.width)));
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) {
        const int y0 = y * image.height / out.height;
        const int y1 = std::max(y0 + 1, (y + 1) * image.height / out.height);
        for (int x = 0; x < out.width; ++x) {
            const int x0 = x * image.width / out.width;
            const int x1 = std::max(x0 + 1, (x + 1) * image.width / out.width);
            std::array<double, 4> sum{};
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const auto* pixel = &image.rgba[(static_cast<std::size_t>(sy) * image.width + sx) * 4];
                    for (std::size_t c = 0; c < 4; ++c) {
                        sum[c] += pixel[c];
                    }
                }
            }
            const double count = static_cast<double>(y1 - y0) * (x1 - x0);
            auto* target = &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4];
            for (std::size_t c = 0; c < 4; ++c) {
                target[c] = static_cast<std::uint8_t>(std::lround(sum[c] / count));
            }
        }
    }
    return out;
}

ImageDifference compareCapture(const Image& capture, const Image& reference) {
    return compareImages(resizedTo(capture, std::max(1, reference.width)), reference);
}

std::string blankFrameProblem(const Image& image) {
    const auto pixels = image.rgba.size() / 4;
    if (pixels == 0) {
        return "the capture is empty";
    }
    std::array<std::size_t, 64> levels{};
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto* p = &image.rgba[i * 4];
        ++levels[(p[0] * 54u + p[1] * 183u + p[2] * 19u) >> 10u];
    }
    const auto most = std::max_element(levels.begin(), levels.end());
    const double share = static_cast<double>(*most) / static_cast<double>(pixels);
    if (share > 0.95) {
        return "one brightness (level " + std::to_string(most - levels.begin()) + " of 63) covers " +
               std::to_string(std::lround(share * 100)) + "% of the frame";
    }
    const auto used =
        std::count_if(levels.begin(), levels.end(), [pixels](std::size_t n) { return n * 200 >= pixels; });
    if (used < 4) {
        return "only " + std::to_string(used) + " of 64 brightness levels hold 0.5% of the frame or more";
    }
    return {};
}

std::string backendFolder(std::string_view rendererName) {
    std::string folder;
    for (const char c : rendererName) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            folder += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return folder;
}

std::filesystem::path referenceFor(const std::filesystem::path& compare, std::string_view rendererName) {
    auto text = compare.generic_string();
    constexpr std::string_view placeholder = "{backend}";
    for (auto at = text.find(placeholder); at != std::string::npos; at = text.find(placeholder, at)) {
        const auto folder = backendFolder(rendererName);
        text.replace(at, placeholder.size(), folder);
        at += folder.size();
    }
    return text;
}

void ReplayRecorder::input(const std::string& name, float value) {
    pendingInput_.push_back(Json::array({name, value}));
}

void ReplayRecorder::mouse(glm::vec2 canvasPosition) {
    pendingMouse_ = canvasPosition;
}

void ReplayRecorder::wheel(float delta) {
    pendingWheel_ += delta;
}

void ReplayRecorder::text(std::string_view utf8) {
    pendingText_ += utf8;
}

void ReplayRecorder::composition(const std::string& text, int cursor) {
    pendingComposition_.emplace(text, cursor);
}

void ReplayRecorder::pad(const InputState::PadEvent& event) {
    pendingPads_.push_back(padJson(event));
}

void ReplayRecorder::stamp(std::uint64_t tick) {
    if (pendingMouse_) {
        commands_.push_back({{"tick", tick}, {"mouse", {pendingMouse_->x, pendingMouse_->y}}});
    }
    if (pendingWheel_ != 0) {
        commands_.push_back({{"tick", tick}, {"wheel", pendingWheel_}});
    }
    if (!pendingPads_.empty()) {
        commands_.push_back({{"tick", tick}, {"pads", std::move(pendingPads_)}});
        pendingPads_ = Json::array();
    }
    if (!pendingInput_.empty()) {
        commands_.push_back({{"tick", tick}, {"input", std::move(pendingInput_)}});
        pendingInput_ = Json::array();
    }
    if (!pendingText_.empty()) {
        commands_.push_back({{"tick", tick}, {"text", std::move(pendingText_)}});
    }
    if (pendingComposition_) {
        commands_.push_back({{"tick", tick},
                             {"text", std::move(pendingComposition_->first)},
                             {"cursor", pendingComposition_->second}});
    }
    pendingMouse_.reset();
    pendingWheel_ = 0;
    pendingText_.clear();
    pendingComposition_.reset();
}

Json ReplayRecorder::toJson(std::uint64_t lastTick) const {
    auto commands = commands_;
    if (commands.empty() || commands.back().at("tick").get<std::uint64_t>() < lastTick) {
        commands.push_back({{"tick", lastTick}});
    }
    return {{"seed", seed_}, {"commands", std::move(commands)}};
}

int runHeadlessReplay(const std::filesystem::path& game, const std::filesystem::path& builtin,
                      const std::filesystem::path& file, std::filesystem::path userData,
                      std::optional<std::uint64_t> seed) {
    ReplayHarness replay(file);
    const bool temporary = userData.empty();
    if (temporary) {
        userData =
            std::filesystem::temp_directory_path() /
            ("fengine-replay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    int code = 0;
    {
        EngineOptions options;
        options.gameRoot = game;
        options.builtinRoot = builtin;
        options.userData = userData;
        options.development = true;
        options.testHooks = true;
        options.seed = seed ? seed : replay.seed() ? replay.seed() : std::optional<std::uint64_t>(1);
        Engine engine(std::move(options));
        engine.setViewport(engine.config().windowWidth, engine.config().windowHeight);
        const FixedStep step;
        if (engine.start()) {
            std::uint64_t ticks = 0;
            while (!replay.done(ticks) && !engine.requests().quit) {
                const auto effects = replay.apply(ticks + 1, engine);
                if (effects.resize) {
                    engine.setViewport(effects.resize->first, effects.resize->second);
                }
                engine.tick(step.interval());
                engine.draw(1.0);
                engine.requests().captures.clear();
                engine.requests().sceneCaptures.clear();
                ++ticks;
            }
        }
        code = replay.report(engine);
    }
    if (temporary) {
        std::error_code ec;
        std::filesystem::remove_all(userData, ec);
    }
    return code;
}
} // namespace fe
