#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>

namespace fe {
struct RuntimeOptions {
    std::filesystem::path game, lualib, shaders, userData;
    std::filesystem::path replay, capture;
    std::optional<std::uint64_t> seed;
    std::uint64_t frameLimit = 0;
    bool mute = false, tools = false, realtime = false;
};

// The windowed game loop: SDL events, fixed-step ticks, rendering of the
// worlds and canvas scripts drew, the editor and development hot reload.
class MainLoop {
public:
    explicit MainLoop(RuntimeOptions options);
    int run();

private:
    RuntimeOptions options_;
};
} // namespace fe
