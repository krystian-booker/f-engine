#pragma once
#include "engine/render/GpuAssets.h"
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fe {
// The engine vertex shader a game fragment shader is paired with: full-screen
// post passes, map and mesh parts (one draw, or instanced), and sprites.
enum class ShaderVertex { Screen, World, WorldInstanced, Sprite };
constexpr std::size_t ShaderVertexCount = 4;
[[nodiscard]] const char* shaderVertexName(ShaderVertex vertex);

// Game fragment shaders: <game>/shaders/fs_<name>.sc. Development builds
// compile a changed loose source with shaderc into a cache folder (with the
// engine's shader folder on the include path, so lighting.sh is available);
// every build otherwise loads <game>/shaders/<backend>/fs_<name>.bin (what
// tools/package_game.py produces), a loose file or an entry of game.pak, read
// through fe::readFile. A program pairs the fragment shader with an engine
// vertex shader, whose outputs its inputs must match. A shader that cannot be
// compiled, loaded or paired is reported once, with the error, and its users
// fall back to the engine's own shader (post passes are skipped).
class CustomShaders {
public:
    CustomShaders(std::filesystem::path gameRoot, std::filesystem::path shaderRoot);
    // A stable index for a shader name (a-z, 0-9, _ and -), so draws resolve
    // their shader without a string lookup each frame.
    [[nodiscard]] int slot(const std::string& name);
    // Invalid when the shader is unavailable for that vertex shader.
    bgfx::ProgramHandle program(int slot, ShaderVertex vertex);
    bgfx::ProgramHandle program(const std::string& name, ShaderVertex vertex) {
        return program(slot(name), vertex);
    }
    // Development builds: forgets programs whose source changed (checked once a second).
    void poll();
    // Engine shaders were rebuilt: programs are loaded again on next use.
    void reload(std::filesystem::path shaderRoot);

private:
    struct Entry {
        std::string name;
        std::array<Program, ShaderVertexCount> programs;
        std::array<bool, ShaderVertexCount> tried{};
        std::filesystem::file_time_type source{};
        bool reported = false;
    };
    void forget(Entry& entry);
    [[nodiscard]] std::filesystem::path binary(const std::string& name,
                                               std::filesystem::file_time_type source);
    std::filesystem::path gameRoot_, shaderRoot_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, int> slots_;
#if FENGINE_DEV
    std::uint32_t polls_ = 0;
#endif
};
} // namespace fe
