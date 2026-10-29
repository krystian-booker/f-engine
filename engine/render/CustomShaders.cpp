#include "engine/render/CustomShaders.h"
#include "engine/core/Log.h"
#include <SDL3/SDL_process.h>

namespace fe {
namespace {
#if FENGINE_DEV && defined(FENGINE_SHADERC)
// shaderc's --platform and --profile for each backend folder.
std::pair<const char*, const char*> shaderTarget(std::string_view backend) {
    if (backend == "metal") {
        return {"osx", "metal"};
    }
    if (backend == "dx11") {
        return {"windows", "s_5_0"};
    }
    if (backend == "spirv") {
        return {"linux", "spirv"};
    }
    return {"linux", "330"};
}

bool compileShader(const std::filesystem::path& source, const std::filesystem::path& output) {
    std::filesystem::create_directories(output.parent_path());
    const auto [platform, profile] = shaderTarget(shaderBackend());
    const std::string input = source.string(), out = output.string(),
                      varyings = std::string(FENGINE_SHADER_SOURCE_DIR) + "/varying.def.sc";
    const char* arguments[] = {FENGINE_SHADERC,
                               "-f",
                               input.c_str(),
                               "-o",
                               out.c_str(),
                               "--type",
                               "fragment",
                               "--platform",
                               platform,
                               "--profile",
                               profile,
                               "--varyingdef",
                               varyings.c_str(),
                               "-i",
                               FENGINE_BGFX_SHADER_INCLUDE,
                               "-i",
                               FENGINE_SHADER_SOURCE_DIR,
                               nullptr};
    auto* process = SDL_CreateProcess(arguments, true);
    if (!process) {
        log(LogLevel::Error, std::string("Cannot run shaderc: ") + SDL_GetError());
        return false;
    }
    std::size_t size = 0;
    int exitCode = 1;
    void* text = SDL_ReadProcess(process, &size, &exitCode);
    if (exitCode != 0) {
        log(LogLevel::Error, "Compiling " + input + " failed:\n" +
                                 (text ? std::string(static_cast<const char*>(text), size) : std::string()));
    }
    SDL_free(text);
    SDL_DestroyProcess(process);
    return exitCode == 0;
}
#endif

std::filesystem::path sourceOf(const std::filesystem::path& gameRoot, const std::string& name) {
    return gameRoot / "shaders" / ("fs_" + name + ".sc");
}
} // namespace

const char* shaderVertexName(ShaderVertex vertex) {
    switch (vertex) {
    case ShaderVertex::Screen:
        return "vs_screen";
    case ShaderVertex::World:
        return "vs_world";
    case ShaderVertex::WorldInstanced:
        return "vs_world_instanced";
    case ShaderVertex::Sprite:
        break;
    }
    return "vs_sprite";
}

CustomShaders::CustomShaders(std::filesystem::path gameRoot, std::filesystem::path shaderRoot)
    : gameRoot_(std::move(gameRoot)), shaderRoot_(std::move(shaderRoot)) {
}

int CustomShaders::slot(const std::string& name) {
    const auto [found, added] = slots_.try_emplace(name, static_cast<int>(entries_.size()));
    if (added) {
        Entry entry;
        entry.name = name;
        std::error_code ec;
        entry.source = std::filesystem::last_write_time(sourceOf(gameRoot_, name), ec);
        entries_.push_back(std::move(entry));
    }
    return found->second;
}

std::filesystem::path CustomShaders::binary(const std::string& name,
                                            [[maybe_unused]] std::filesystem::file_time_type source) {
    const auto shipped = gameRoot_ / "shaders" / shaderBackend() / ("fs_" + name + ".bin");
#if FENGINE_DEV && defined(FENGINE_SHADERC)
    std::error_code ec;
    const auto sourceFile = sourceOf(gameRoot_, name);
    if (std::filesystem::is_regular_file(sourceFile, ec)) {
        if (std::filesystem::is_regular_file(shipped, ec) &&
            std::filesystem::last_write_time(shipped, ec) >= source) {
            return shipped;
        }
        const auto cache = std::filesystem::temp_directory_path() / "fengine-shaders" /
                           std::to_string(std::hash<std::string>{}(gameRoot_.string())) / shaderBackend() /
                           ("fs_" + name + ".bin");
        if (!std::filesystem::exists(cache, ec) || std::filesystem::last_write_time(cache, ec) < source) {
            if (!compileShader(sourceFile, cache)) {
                return {};
            }
            log(LogLevel::Info, "Compiled game shader " + sourceFile.string());
        }
        return cache;
    }
#endif
    return shipped;
}

bgfx::ProgramHandle CustomShaders::program(int slot, ShaderVertex vertex) {
    auto& entry = entries_.at(static_cast<std::size_t>(slot));
    const auto v = static_cast<std::size_t>(vertex);
    if (entry.tried[v]) {
        return entry.programs[v].get();
    }
    entry.tried[v] = true;
    std::string error = "compiling it failed";
    try {
        if (const auto file = binary(entry.name, entry.source); !file.empty()) {
            entry.programs[v] = Program(loadProgram(
                shaderRoot_ / shaderBackend() / (std::string(shaderVertexName(vertex)) + ".bin"), file));
        }
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    if (!entry.programs[v].valid() && !entry.reported) {
        entry.reported = true;
        log(LogLevel::Error,
            "Game shader '" + entry.name + "' unavailable, the engine's shader is used: " + error);
    }
    return entry.programs[v].get();
}

void CustomShaders::forget(Entry& entry) {
    for (auto& program : entry.programs) {
        program.reset();
    }
    entry.tried = {};
    entry.reported = false;
}

void CustomShaders::reload(std::filesystem::path shaderRoot) {
    shaderRoot_ = std::move(shaderRoot);
    for (auto& entry : entries_) {
        forget(entry);
    }
}

void CustomShaders::poll() {
#if FENGINE_DEV
    if (++polls_ % 60 != 0) {
        return;
    }
    for (auto& entry : entries_) {
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(sourceOf(gameRoot_, entry.name), ec);
        if (!ec && time != entry.source) {
            entry.source = time;
            forget(entry);
        }
    }
#endif
}
} // namespace fe
