#pragma once
#include "engine/core/Json.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fe {
class Engine;

// Plays a scripted session: tick-stamped commands that feed input, run Lua,
// assert Lua expressions, wait for conditions, resize and capture. Used for
// end-to-end tests of whole games (CTest runs them with FENGINE_RUN_REPLAYS).
//
//   [{"tick": 10, "input": {"key:Right": 1}},
//    {"tick": 40, "input": {"key:Right": 0}},
//    {"tick": 41, "wait_for": "require('fe.field').world ~= nil", "timeout": 600},
//    {"tick": 42, "assert": "require('fe.state').flag('met_elder')"},
//    {"tick": 60, "capture": "town.png"}]
//
// Ticks after a wait_for are relative to when its condition became true.
class ReplayHarness {
public:
    struct Effects {
        std::optional<std::filesystem::path> capture;
        std::optional<std::pair<int, int>> resize;
    };
    explicit ReplayHarness(const std::filesystem::path& file);
    // Runs commands due at this tick (before the engine ticks).
    Effects apply(std::uint64_t tick, Engine& engine);
    [[nodiscard]] bool finished() const { return next_ >= commands_.size(); }
    [[nodiscard]] const std::vector<std::string>& failures() const { return failures_; }
    [[nodiscard]] std::uint64_t lastTick() const;

private:
    bool evaluate(Engine& engine, const std::string& expression, std::string& detail);
    Json commands_;
    std::size_t next_ = 0;
    std::uint64_t offset_ = 0, waitStarted_ = 0;
    bool waiting_ = false;
    std::vector<std::string> failures_;
};
} // namespace fe
