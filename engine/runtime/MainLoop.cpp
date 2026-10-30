#include "engine/runtime/MainLoop.h"
#include "engine/assets/Image.h"
#include "engine/audio/MiniAudio.h"
#include "engine/core/FixedStep.h"
#include "engine/core/Log.h"
#include "engine/input/SdlInput.h"
#include "engine/platform/FileWatcher.h"
#include "engine/platform/Window.h"
#include "engine/render/FrameRenderer.h"
#include "engine/render/Renderer.h"
#include "engine/runtime/Engine.h"
#include "engine/runtime/ReplayHarness.h"
#if FENGINE_DEV
#include "engine/editor/Editor.h"
#endif
#include <SDL3/SDL.h>
#include <chrono>
#include <iostream>
#include <vector>

namespace fe {
namespace {
using Clock = std::chrono::steady_clock;

#if FENGINE_DEV
// Development hot reload: game files, lualib, compiled shaders, and shader
// sources when running from the build tree (rebuilt in the background).
class Reloader {
public:
    // Each watcher stats a bounded number of entries per poll, so a large game
    // folder costs a slice of each poll rather than a stall every quarter second.
    static constexpr std::size_t EntriesPerPoll = 512;
    Reloader(const RuntimeOptions& options, const std::filesystem::path& shaderRoot)
        : game_(std::filesystem::weakly_canonical(options.game), EntriesPerPoll),
          library_(options.lualib, EntriesPerPoll), shaders_(shaderRoot, EntriesPerPoll) {
#ifdef FENGINE_SHADER_SOURCE_DIR
        std::error_code ec;
        if (std::filesystem::equivalent(shaderRoot, FENGINE_SHADER_DIR, ec)) {
            sources_.emplace(FENGINE_SHADER_SOURCE_DIR, EntriesPerPoll);
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

    void poll(Engine& engine, Renderer& renderer) {
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
#endif

// Replay screenshots: each must look like a rendered frame, and one with a
// reference image for the running backend must match it. bgfx writes a
// capture while presenting a later frame, so each one is checked once the
// file exists. With `write` (--write-references) the capture, at reference
// size, becomes the reference instead.
class CaptureChecks {
public:
    explicit CaptureChecks(bool write) : write_(write) {}
    void request(Renderer& renderer, const std::filesystem::path& capture,
                 std::optional<std::filesystem::path> reference, double tolerance) {
        std::error_code ec;
        std::filesystem::remove(capture, ec);
        renderer.capture(capture);
        pending_.push_back({capture, std::move(reference), tolerance, 0});
    }
    void poll(ReplayHarness& replay) {
        std::erase_if(pending_, [&](Pending& check) {
            if (!std::filesystem::exists(check.capture)) {
                if (++check.frames > 10) {
                    replay.fail("Screenshot was never written: " + check.capture.string());
                    return true;
                }
                return false;
            }
            try {
                check.verify(replay, write_);
            } catch (const std::exception& error) {
                replay.fail(std::string("Screenshot check failed: ") + error.what());
            }
            return true;
        });
    }
    [[nodiscard]] bool idle() const { return pending_.empty(); }

private:
    struct Pending {
        std::filesystem::path capture;
        std::optional<std::filesystem::path> reference;
        double tolerance;
        int frames;

        void verify(ReplayHarness& replay, bool write) const {
            const auto image = Image::load(capture);
            if (const auto problem = blankFrameProblem(image); !problem.empty()) {
                replay.fail(capture.string() + " is not a rendered frame: " + problem);
                return;
            }
            if (!reference) {
                return;
            }
            if (write) {
                resizedTo(image, ReferenceWidth).savePng(*reference);
                log(LogLevel::Info, "Wrote reference image " + reference->string());
                return;
            }
            if (!std::filesystem::exists(*reference)) {
                log(LogLevel::Info, "No reference image " + reference->string() + " for this backend; " +
                                        capture.string() + " was only checked for being a rendered frame");
                return;
            }
            const auto difference = compareCapture(image, Image::load(*reference));
            if (difference.mean > tolerance) {
                replay.fail(capture.string() + " differs from " + reference->string() + ": mean " +
                            std::to_string(difference.mean) + " > tolerance " + std::to_string(tolerance) +
                            " (largest " + std::to_string(difference.max) + ")");
            }
        }
    };
    bool write_;
    std::vector<Pending> pending_;
};

std::optional<ReplayHarness> loadReplay(const RuntimeOptions& options) {
    if (options.replay.empty()) {
        return std::nullopt;
    }
    return ReplayHarness(options.replay);
}

// Waits `seconds` before the next frame: sleeps until shortly before the
// deadline, then spins, since a timer sleep alone overshoots by a millisecond
// or more.
void pace(double seconds) {
    if (seconds <= 0) {
        return;
    }
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(seconds));
    constexpr double SpinSeconds = 0.0015;
    if (seconds > SpinSeconds) {
        SDL_DelayNS(static_cast<Uint64>((seconds - SpinSeconds) * 1e9));
    }
    while (Clock::now() < deadline) {
    }
}

// Replays and recordings are deterministic: a fixed seed unless one is given.
std::optional<std::uint64_t> sessionSeed(const RuntimeOptions& options,
                                         const std::optional<ReplayHarness>& replay) {
    if (options.seed) {
        return options.seed;
    }
    if (replay) {
        return replay->seed().value_or(1);
    }
    if (!options.record.empty()) {
        return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    }
    return std::nullopt;
}

// The back buffer the game asks for: HDR from --hdr or game.json, presented
// with the frame's white at the configured paper white.
OutputColor outputRequest(const RuntimeOptions& options, const GameConfig& config) {
    OutputColor output;
    output.encoding = options.hdr || config.hdrOutput ? OutputEncoding::ScRgb : OutputEncoding::Srgb;
    output.paperWhite = config.hdrPaperWhite;
    return output;
}

// One run of a game in a window: owns every subsystem and splits a frame
// into event handling, fixed-step ticks, rendering and app requests.
class Session {
public:
    Session(const RuntimeOptions& options, const GameConfig& config)
        : options_(options), window_(config), renderer_(window_, options.shaders, outputRequest(options, config)),
          replay_(loadReplay(options)), engine_(engineOptions(), config),
          frame_(renderer_, engine_.assets(), config.pixelsPerUnit), input_(engine_.input())
#if FENGINE_DEV
          ,
          editor_(engine_), reloader_(options, options.shaders)
#endif
    {
        if (!options.record.empty()) {
            recorder_.emplace(engine_.seed());
            input_.record(&*recorder_);
        }
#if FENGINE_DEV
        editor_.attach(frame_);
        editor_.setWindow(window_.native());
        engine_.editorHooks().leaveMap = [this](const std::string& world) { return editor_.requestLeaveMap(world); };
        engine_.editorHooks().leaveResult = [this] { return editor_.leaveMapResult(); };
        if (!options.userData.empty()) {
            editor_.setLayoutFile(options.userData / "editor.ini");
        }
        if (options.tools) {
            editor_.toggle();
        }
#endif
        readViewport();
        auto& display = engine_.display();
        display.monitors = Window::monitors();
        window_.describe(display);
        display.hdr = renderer_.output().encoding != OutputEncoding::Srgb;
        refresh_ = window_.refreshRate();
        cap_.setMaxFps(options.maxFps.value_or(config.maxFps));
    }

    int run() {
        if (!engine_.start() && FENGINE_DEV == 0) {
            std::cerr << "The game failed to start; see the log above.\n";
            return 1;
        }
        auto previous = Clock::now();
        while (running_) {
            pumpEvents();
            // The frame cap applies while vsync is off; replays run as fast as
            // they can unless paced in real time.
            if (engine_.display().vsync == VsyncMode::Off && !(replay_ && !options_.realtime)) {
                pace(cap_.wait(std::chrono::duration<double>(Clock::now().time_since_epoch()).count()));
            } else {
                cap_.reset();
            }
            const auto now = Clock::now();
            const double frameSeconds = std::chrono::duration<double>(now - previous).count();
            previous = now;
            advance(replay_ && !options_.realtime ? step_.interval() : frameSeconds);
            if (engine_.display().vsync == VsyncMode::Adaptive) {
                renderer_.setVsync(adaptive_.update(frameSeconds, refresh_, renderer_.vsync()));
            }
            render(frameSeconds * 1000.0);
            applyRequests();
            if (options_.realtime && replay_) {
                const auto spent = std::chrono::duration<double>(Clock::now() - now).count();
                if (spent < step_.interval()) {
                    SDL_DelayNS(static_cast<Uint64>((step_.interval() - spent) * 1e9));
                }
            }
        }
        return finish();
    }

private:
    EngineOptions engineOptions() {
        EngineOptions engine;
        engine.gameRoot = options_.game;
        engine.libraryRoot = options_.lualib;
        engine.userData = options_.userData;
        engine.development = FENGINE_DEV != 0;
        engine.seed = sessionSeed(options_, replay_);
        engine.testHooks = !options_.replay.empty() || !options_.record.empty();
        // Replays and recordings must not depend on the machine's language or
        // clipboard (they keep the engine's in-memory one).
        engine.locale = engine.testHooks ? "en-US" : preferredLocale();
        if (!engine.testHooks) {
            engine.clipboard = std::make_unique<SdlClipboard>();
        }
        if (options_.platform) {
            engine.platform = options_.platform(options_.userData);
        }
        if (!options_.mute) {
            auto audio = std::make_unique<MiniAudio>();
            audio_ = audio.get();
            engine.audio = std::move(audio);
        }
        return engine;
    }

    void readViewport() {
        const auto [width, height] = window_.pixelSize();
        viewport_ = {width, height};
        engine_.setViewport(width, height);
    }

    void pumpEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            renderer_.processEvent(event);
            activity_.handle(event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running_ = false;
            }
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                viewport_ = {event.window.data1, event.window.data2};
            }
            if (event.type >= SDL_EVENT_DISPLAY_FIRST && event.type <= SDL_EVENT_DISPLAY_LAST) {
                monitorsChanged_ = true;
                displayChanged_ = true;
            }
            switch (event.type) {
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_MOVED:
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            case SDL_EVENT_WINDOW_MAXIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                displayChanged_ = true;
                break;
            default:
                break;
            }
#if FENGINE_DEV
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.scancode == SDL_SCANCODE_F1) {
                editor_.toggle();
                continue;
            }
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                // Window coordinates (points); the editor scales them to pixels.
                editor_.dropFile(std::filesystem::path(event.drop.data), {event.drop.x, event.drop.y});
                continue;
            }
            const bool captured = editor_.capturesInput();
#else
            const bool captured = false;
#endif
            if (!replay_) { // replays feed their own input for determinism
                input_.process(event, engine_.canvas(), viewport_,
                               SDL_GetWindowPixelDensity(window_.native()), captured);
            }
        }
        readViewport();
        // The window and monitors are asked only after an event says they
        // changed, not every frame.
        if (displayChanged_) {
            displayChanged_ = false;
            auto& display = engine_.display();
            if (monitorsChanged_) {
                monitorsChanged_ = false;
                display.monitors = Window::monitors();
            }
            window_.describe(display);
            refresh_ = window_.refreshRate();
        }
    }

    // Background windows don't advance the game; replays always run.
    [[nodiscard]] bool suspended() const {
        return options_.pauseOnFocusLoss && !replay_ && !activity_.foreground();
    }

    void advance(double elapsed) {
        const bool pause = suspended();
        if (pause != wasSuspended_) {
            wasSuspended_ = pause;
            if (audio_) {
                audio_->setPaused(pause);
            }
            log(LogLevel::Info, pause ? "Window in background: game paused" : "Game resumed");
        }
#if FENGINE_DEV
        sinceReload_ += elapsed;
        if (sinceReload_ > 0.25) {
            sinceReload_ = 0;
            reloader_.poll(engine_, renderer_);
        }
#endif
        if (!pause) {
            step_.advance(elapsed, [&](double dt) { tick(dt); });
        }
    }

    void tick(double dt) {
        if (replay_) {
            const auto effects = replay_->apply(ticks_ + 1, engine_);
            if (effects.capture) {
                captures_.request(renderer_, *effects.capture,
                                  effects.reference
                                      ? std::optional(referenceFor(*effects.reference, renderer_.name()))
                                      : std::nullopt,
                                  effects.tolerance);
            }
            if (effects.resize) {
                SDL_SetWindowSize(window_.native(), effects.resize->first, effects.resize->second);
            }
        }
#if FENGINE_DEV
        // Replays never pause (like focus loss), so a replay can drive the
        // game under an open editor.
        if (editor_.paused() && !replay_) {
            return;
        }
#endif
        if (recorder_) {
            recorder_->stamp(ticks_ + 1);
        }
        engine_.tick(dt);
        ++ticks_;
    }

    void render(double frameMilliseconds) {
        if (!renderer_.beginFrame()) {
            SDL_Delay(16); // minimised
            return;
        }
        engine_.draw(step_.alpha());
        auto& requests = engine_.requests();
        for (const auto& file : requests.sceneCaptures) {
            frame_.captureScene(file);
        }
        requests.sceneCaptures.clear();
#if FENGINE_DEV
        // The editor runs before the frame renders: the Scene viewport it
        // lays out this frame is where the world is drawn this frame. ImGui's
        // own draw data is submitted in endFrame, after everything else.
        editor_.drawErrors();
        editor_.draw({frameMilliseconds, engine_.stats().drawCalls, step_.alpha(), viewport_});
        std::vector<WorldDraw> draws = engine_.drawList();
        editor_.adjustDraws(draws);
        frame_.render(draws, engine_.canvas(), engine_.screen(), viewport_, step_.alpha(), engine_.gameTime(),
                      engine_.ticks());
#else
        frame_.render(engine_.drawList(), engine_.canvas(), engine_.screen(), viewport_, step_.alpha(),
                      engine_.gameTime(), engine_.ticks());
#endif
        engine_.stats().frame = frameMilliseconds;
        engine_.stats().drawCalls = frame_.drawCalls();
        for (const auto& file : requests.captures) {
            renderer_.capture(file);
        }
        requests.captures.clear();
        if (frames_ + 1 == 30 && !options_.capture.empty()) {
            renderer_.capture(options_.capture);
        }
        frame_.finishFrame(renderer_.endFrame());
        ++frames_;
        if (replay_) {
            captures_.poll(*replay_);
        }
    }

    void applyRequests() {
        auto& requests = engine_.requests();
        if (requests.title) {
            SDL_SetWindowTitle(window_.native(), requests.title->c_str());
            requests.title.reset();
        }
        if (requests.display) {
            applyDisplay(*requests.display);
            requests.display.reset();
        }
        input_.apply(window_.native(), engine_.canvas(), viewport_,
                     SDL_GetWindowPixelDensity(window_.native()));
        running_ = running_ && !requests.quit && !(options_.frameLimit && frames_ >= options_.frameLimit) &&
                   !(replay_ && replay_->done(ticks_) && captures_.idle());
    }

    void applyDisplay(const DisplayRequest& request) {
        auto& display = engine_.display();
        DisplayState target = display;
        window_.describe(target);
        request.applyTo(target);
        window_.apply(target);
        if (request.vsync) {
            adaptive_.reset();
            renderer_.setVsync(*request.vsync != VsyncMode::Off);
        }
        display.vsync = target.vsync;
        window_.describe(display);
        refresh_ = window_.refreshRate();
        readViewport();
    }

    int finish() {
        int code = 0;
        if (replay_) {
            code = replay_->report(engine_);
        }
        if (recorder_) {
            writeJson(options_.record, recorder_->toJson(ticks_));
            log(LogLevel::Info,
                "Recorded " + std::to_string(ticks_) + " ticks to " + options_.record.string());
        }
        log(LogLevel::Info, "Stopped after " + std::to_string(frames_) + " frames");
        return code;
    }

    const RuntimeOptions& options_;
    Window window_;
    Renderer renderer_;
    std::optional<ReplayHarness> replay_;
    MiniAudio* audio_ = nullptr; // owned by the engine
    Engine engine_;
    FrameRenderer frame_;
    SdlInput input_;
    WindowActivity activity_;
    std::optional<ReplayRecorder> recorder_;
    CaptureChecks captures_{options_.writeReferences};
#if FENGINE_DEV
    Editor editor_;
    Reloader reloader_;
    double sinceReload_ = 0;
#endif
    FixedStep step_;
    AdaptiveVsync adaptive_;
    FrameCap cap_;
    float refresh_ = 60;
    bool monitorsChanged_ = false, displayChanged_ = false;
    glm::ivec2 viewport_{1};
    std::uint64_t frames_ = 0, ticks_ = 0;
    bool running_ = true, wasSuspended_ = false;
};
} // namespace

MainLoop::MainLoop(RuntimeOptions options, GameConfig config)
    : options_(std::move(options)), config_(std::move(config)) {
}

int MainLoop::run() {
    Session session(options_, config_);
    return session.run();
}
} // namespace fe
