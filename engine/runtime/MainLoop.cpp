#include "engine/runtime/MainLoop.h"
#include "engine/audio/MiniAudio.h"
#include "engine/core/FixedStep.h"
#include "engine/core/Log.h"
#include "engine/input/SdlInput.h"
#include "engine/platform/FileWatcher.h"
#include "engine/platform/Window.h"
#include "engine/render/CanvasRenderer.h"
#include "engine/render/Renderer.h"
#include "engine/render/WorldRenderer.h"
#include "engine/runtime/Engine.h"
#include "engine/runtime/ReplayHarness.h"
#if FENGINE_DEV
#include "engine/editor/Editor.h"
#endif
#include <SDL3/SDL.h>
#include <chrono>
#include <imgui.h>
#include <iostream>

namespace fe {
namespace {
// Development hot reload: game files, lualib, compiled shaders, and shader
// sources when running from the build tree (rebuilt in the background).
class Reloader {
public:
    Reloader(const RuntimeOptions& options, const std::filesystem::path& shaderRoot)
        : game_(std::filesystem::weakly_canonical(options.game)), library_(options.lualib),
          shaders_(shaderRoot) {
#ifdef FENGINE_SHADER_SOURCE_DIR
        std::error_code ec;
        if (std::filesystem::equivalent(shaderRoot, FENGINE_SHADER_DIR, ec)) {
            sources_.emplace(FENGINE_SHADER_SOURCE_DIR);
        }
#endif
    }
    ~Reloader() {
        if (build_) {
            SDL_DestroyProcess(build_);
        }
    }
    Reloader(const Reloader&) = delete;
    Reloader& operator=(const Reloader&) = delete;

    void poll(Engine& engine, Renderer& renderer, WorldRenderer& worlds, CanvasRenderer& canvas) {
        for (auto* watcher : {&game_, &library_}) {
            for (const auto& change : watcher->poll()) {
                if (change.kind != FileChangeKind::Removed) {
                    engine.reload(change.path);
                }
            }
        }
        bool shadersChanged = false;
        for (const auto& change : shaders_.poll()) {
            shadersChanged = shadersChanged || change.kind != FileChangeKind::Removed;
        }
        if (shadersChanged) {
            try {
                worlds.reloadPrograms();
                canvas.reload(renderer.shaderRoot());
                renderer.reloadShaders();
            } catch (const std::exception& error) {
                log(LogLevel::Error,
                    std::string("Shader reload failed; keeping previous programs: ") + error.what());
            }
        }
        if (sources_) {
            for (const auto& change : sources_->poll()) {
                const auto extension = change.path.extension();
                queued_ = queued_ || extension == ".sc" || extension == ".sh";
            }
        }
        rebuildShaders();
    }

private:
    void rebuildShaders() {
#ifdef FENGINE_BUILD_DIR
        if (build_) {
            int exitCode = 0;
            if (SDL_WaitProcess(build_, false, &exitCode)) {
                SDL_DestroyProcess(build_);
                build_ = nullptr;
                log(exitCode == 0 ? LogLevel::Info : LogLevel::Error,
                    exitCode == 0 ? "Shaders rebuilt"
                                  : "Shader build failed; keeping the last working programs");
            }
        }
        if (queued_ && !build_) {
            const char* arguments[] = {FENGINE_CMAKE, "--build",         FENGINE_BUILD_DIR,
                                       "--target",    "fengine_shaders", nullptr};
            build_ = SDL_CreateProcess(arguments, false);
            queued_ = false;
        }
#endif
    }
    FileWatcher game_, library_, shaders_;
    std::optional<FileWatcher> sources_;
    SDL_Process* build_ = nullptr;
    bool queued_ = false;
};
} // namespace

MainLoop::MainLoop(RuntimeOptions options) : options_(std::move(options)) {
}

int MainLoop::run() {
    const auto config = GameConfig::load(options_.game);
    Window window(config);
    Renderer renderer(window, options_.shaders);
    EngineOptions engineOptions;
    engineOptions.gameRoot = options_.game;
    engineOptions.libraryRoot = options_.lualib;
    engineOptions.userData = options_.userData;
    engineOptions.development = FENGINE_DEV != 0;
    engineOptions.seed = options_.seed;
    if (!options_.mute) {
        engineOptions.audio = std::make_unique<MiniAudio>();
    }
    Engine engine(std::move(engineOptions));
    GpuAssets gpu(engine.assets());
    WorldRenderer worlds(gpu, options_.shaders, config.pixelsPerUnit);
    CanvasRenderer canvas(gpu, options_.shaders);
    SdlInput input(engine.input());
    std::optional<ReplayHarness> replay;
    if (!options_.replay.empty()) {
        replay.emplace(options_.replay);
    }
#if FENGINE_DEV
    Editor editor(engine);
    if (options_.tools) {
        editor.toggle();
    }
    Reloader reloader(options_, options_.shaders);
#endif
    {
        const auto [w, h] = window.pixelSize();
        engine.setViewport(w, h);
    }
    if (!engine.start() && FENGINE_DEV == 0) {
        std::cerr << "The game failed to start; see the log above.\n";
        return 1;
    }
    FixedStep step;
    std::uint64_t frames = 0, ticks = 0;
    double sinceReload = 0, frameMs = 0;
    bool running = true;
    auto previous = std::chrono::steady_clock::now();
    while (running) {
        SDL_Event event;
        const auto [width, height] = window.pixelSize();
        const glm::ivec2 viewport(width, height);
        while (SDL_PollEvent(&event)) {
            renderer.processEvent(event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
#if FENGINE_DEV
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.scancode == SDL_SCANCODE_F1) {
                editor.toggle();
                continue;
            }
            const bool captured = editor.capturesInput();
#else
            const bool captured = false;
#endif
            if (!replay) { // replays feed their own input for determinism
                input.process(event, engine.canvas(), viewport, SDL_GetWindowPixelDensity(window.native()),
                              captured);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = replay && !options_.realtime
                                   ? step.interval()
                                   : std::chrono::duration<double>(now - previous).count();
        frameMs = std::chrono::duration<double, std::milli>(now - previous).count();
        previous = now;
        engine.setViewport(width, height);
        step.advance(elapsed, [&](double dt) {
            if (replay) {
                const auto effects = replay->apply(ticks + 1, engine);
                if (effects.capture) {
                    renderer.capture(*effects.capture);
                }
                if (effects.resize) {
                    SDL_SetWindowSize(window.native(), effects.resize->first, effects.resize->second);
                }
            }
#if FENGINE_DEV
            if (editor.paused()) {
                return;
            }
#endif
            engine.tick(dt);
            ++ticks;
        });
#if FENGINE_DEV
        sinceReload += elapsed;
        if (sinceReload > 0.25) {
            sinceReload = 0;
            reloader.poll(engine, renderer, worlds, canvas);
        }
#endif
        if (renderer.beginFrame()) {
            gpu.sync();
            engine.draw(step.alpha());
            int slot = 0;
            std::uint32_t calls = 0;
            for (const auto& world : engine.drawList()) {
                worlds.render(*world, step.alpha(), viewport, engine.screen(), engine.gameTime(), slot++);
                calls += worlds.drawCalls();
            }
            canvas.render(engine.canvas(), engine.screen(), viewport);
#if FENGINE_DEV
            editor.drawErrors();
            editor.draw(frameMs, calls);
#endif
            auto& requests = engine.requests();
            for (const auto& file : requests.captures) {
                renderer.capture(file);
            }
            requests.captures.clear();
            if (frames + 1 == 30 && !options_.capture.empty()) {
                renderer.capture(options_.capture);
            }
            renderer.endFrame();
            ++frames;
        } else {
            SDL_Delay(16);
        }
        auto& requests = engine.requests();
        if (requests.title) {
            SDL_SetWindowTitle(window.native(), requests.title->c_str());
            requests.title.reset();
        }
        if (requests.fullscreen) {
            SDL_SetWindowFullscreen(window.native(), *requests.fullscreen);
            requests.fullscreen.reset();
        }
        if (requests.quit) {
            running = false;
        }
        if (options_.frameLimit && frames >= options_.frameLimit) {
            running = false;
        }
        if (replay && replay->finished() && ticks > replay->lastTick() + 30) {
            running = false;
        }
        if (options_.realtime && replay) {
            const auto spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
            if (spent < step.interval()) {
                SDL_DelayNS(static_cast<Uint64>((step.interval() - spent) * 1e9));
            }
        }
    }
    int code = 0;
    if (replay) {
        for (const auto& failure : replay->failures()) {
            std::cerr << "REPLAY FAILURE: " << failure << "\n";
            code = 1;
        }
        if (!replay->finished()) {
            std::cerr << "REPLAY FAILURE: stopped before every command ran\n";
            code = 1;
        }
        if (!engine.lua().errors().empty()) {
            std::cerr << "REPLAY FAILURE: " << engine.lua().errors().size() << " script error(s)\n";
            code = 1;
        }
    }
    log(LogLevel::Info, "Stopped after " + std::to_string(frames) + " frames");
    return code;
}
} // namespace fe
