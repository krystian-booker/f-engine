#include "engine/runtime/ReplayHarness.h"
#include "engine/runtime/Engine.h"
#include <set>

namespace fe {
namespace {
const std::set<std::string> Known = {"tick",     "input",   "lua",     "assert", "message",
                                     "wait_for", "timeout", "capture", "resize"};
}

ReplayHarness::ReplayHarness(const std::filesystem::path& file) : commands_(readJson(file)) {
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
        for (const auto& [key, value] : command.items()) {
            (void)value;
            if (!Known.contains(key)) {
                throw std::runtime_error("Unknown replay field '" + key + "' in " + file.string());
            }
        }
    }
}

std::uint64_t ReplayHarness::lastTick() const {
    return commands_.empty() ? 0 : commands_.back().at("tick").get<std::uint64_t>() + offset_;
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
            if (!evaluate(engine, command.at("wait_for").get<std::string>(), detail)) {
                if (tick - waitStarted_ > command.value("timeout", 600u)) {
                    failures_.push_back("Timed out waiting for: " +
                                        command.at("wait_for").get<std::string>() + " (" + detail + ")");
                    waiting_ = false;
                    ++next_;
                }
                break;
            }
            offset_ += tick - waitStarted_;
            waiting_ = false;
        }
        if (command.contains("input")) {
            for (const auto& [input, value] : command.at("input").items()) {
                engine.input().feed(input, value.get<float>());
            }
        }
        if (command.contains("lua")) {
            const auto code = command.at("lua").get<std::string>();
            const auto before = engine.lua().errors().size();
            engine.lua().console(code);
            if (engine.lua().errors().size() > before) {
                failures_.push_back("Replay Lua failed: " + code);
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
        if (command.contains("capture")) {
            effects.capture = command.at("capture").get<std::string>();
        }
        if (command.contains("resize")) {
            effects.resize =
                std::make_pair(command.at("resize").at(0).get<int>(), command.at("resize").at(1).get<int>());
        }
        ++next_;
    }
    return effects;
}
} // namespace fe
