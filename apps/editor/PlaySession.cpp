#include "apps/editor/PlaySession.h"
#include "apps/editor/ViewportWindow.h"
#include "engine/audio/MiniAudio.h"
#include "engine/core/Log.h"
#include "engine/input/SdlInput.h"
#include "engine/render/FrameRenderer.h"
#include "engine/render/Renderer.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/MapData.h"
#include "engine/world/World.h"
#include <QKeyEvent>
#include <SDL3/SDL.h>
#include <chrono>

namespace fe::editor {
namespace {
constexpr const char* SceneTarget = "editor_scene_view";
// The raw input name of a Qt key, as SdlInput names keys ("key:Left",
// "key:A"): by SDL's scancode names. Qt reports keys by the character they
// produce, so on other layouts letters follow the layout, not the position.
std::string keyName(const QKeyEvent& event) {
    const int key = event.key();
    SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        code = static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key - Qt::Key_A));
    } else if (key >= Qt::Key_1 && key <= Qt::Key_9) {
        code = static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (key - Qt::Key_1));
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        code = static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (key - Qt::Key_F1));
    } else {
        static const std::pair<int, SDL_Scancode> table[] = {
            {Qt::Key_0, SDL_SCANCODE_0},
            {Qt::Key_Left, SDL_SCANCODE_LEFT},
            {Qt::Key_Right, SDL_SCANCODE_RIGHT},
            {Qt::Key_Up, SDL_SCANCODE_UP},
            {Qt::Key_Down, SDL_SCANCODE_DOWN},
            {Qt::Key_Space, SDL_SCANCODE_SPACE},
            {Qt::Key_Return, SDL_SCANCODE_RETURN},
            {Qt::Key_Enter, SDL_SCANCODE_KP_ENTER},
            {Qt::Key_Escape, SDL_SCANCODE_ESCAPE},
            {Qt::Key_Tab, SDL_SCANCODE_TAB},
            {Qt::Key_Backspace, SDL_SCANCODE_BACKSPACE},
            {Qt::Key_Delete, SDL_SCANCODE_DELETE},
            {Qt::Key_Insert, SDL_SCANCODE_INSERT},
            {Qt::Key_Home, SDL_SCANCODE_HOME},
            {Qt::Key_End, SDL_SCANCODE_END},
            {Qt::Key_PageUp, SDL_SCANCODE_PAGEUP},
            {Qt::Key_PageDown, SDL_SCANCODE_PAGEDOWN},
            {Qt::Key_Shift, SDL_SCANCODE_LSHIFT},
            {Qt::Key_Control, SDL_SCANCODE_LCTRL},
            {Qt::Key_Alt, SDL_SCANCODE_LALT},
            {Qt::Key_Meta, SDL_SCANCODE_LGUI},
            {Qt::Key_Minus, SDL_SCANCODE_MINUS},
            {Qt::Key_Equal, SDL_SCANCODE_EQUALS},
            {Qt::Key_BracketLeft, SDL_SCANCODE_LEFTBRACKET},
            {Qt::Key_BracketRight, SDL_SCANCODE_RIGHTBRACKET},
            {Qt::Key_Backslash, SDL_SCANCODE_BACKSLASH},
            {Qt::Key_Semicolon, SDL_SCANCODE_SEMICOLON},
            {Qt::Key_Apostrophe, SDL_SCANCODE_APOSTROPHE},
            {Qt::Key_QuoteLeft, SDL_SCANCODE_GRAVE},
            {Qt::Key_Comma, SDL_SCANCODE_COMMA},
            {Qt::Key_Period, SDL_SCANCODE_PERIOD},
            {Qt::Key_Slash, SDL_SCANCODE_SLASH},
            {Qt::Key_CapsLock, SDL_SCANCODE_CAPSLOCK},
        };
        for (const auto& [from, to] : table) {
            if (from == key) {
                code = to;
                break;
            }
        }
    }
    if (code == SDL_SCANCODE_UNKNOWN) {
        return {};
    }
    return std::string("key:") + SDL_GetScancodeName(code);
}
} // namespace

PlaySession::PlaySession(Renderer& renderer, ViewportWindow& view, Options options)
    : renderer_(renderer), view_(view) {
    EngineOptions engine;
    engine.gameRoot = options.game;
    engine.builtinRoot = options.builtin;
    engine.userData = options.userData;
    engine.development = true;
    if (!options.mute) {
        try {
            engine.audio = std::make_unique<MiniAudio>();
        } catch (const std::exception& error) {
            log(LogLevel::Warning, std::string("Playing without sound: ") + error.what());
        }
    }
    engine_ = std::make_unique<Engine>(std::move(engine), options.config);
    frame_ = std::make_unique<FrameRenderer>(renderer_, engine_->assets(), options.config.pixelsPerUnit);
    engine_->setGpuWarm([this](const WarmAsset& asset) { return frame_->warmed(asset); });
    const auto size = view_.pixels();
    engine_->setViewport(std::max(size.x, 1), std::max(size.y, 1));

    // Gamepads come through SDL (no SDL window is involved); keys and the
    // mouse come from the Game view.
    gamepads_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (gamepads_) {
        pads_ = std::make_unique<SdlInput>(engine_->input());
    } else {
        log(LogLevel::Warning, std::string("Gamepads unavailable in Play mode: ") + SDL_GetError());
    }

    if (!engine_->start()) {
        const auto& errors = engine_->lua().errors();
        const std::string why = errors.empty() ? std::string("its init failed") : errors.back().message;
        pads_.reset();
        if (gamepads_) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
        throw std::runtime_error("The game did not start: " + why);
    }
    if (!options.scene.empty()) {
        // Start where the editor is, when the game says how.
        try {
            const auto module = engine_->lua().require(options.config.entry);
            const auto hooks = module.as<sol::table>().raw_get<sol::object>("editor");
            if (hooks.get_type() == sol::type::table) {
                const sol::object playFrom = hooks.as<sol::table>().raw_get<sol::object>("play_from");
                if (playFrom.get_type() == sol::type::function) {
                    const auto result = engine_->lua().invoke(playFrom, options.scene, options.point);
                    if (!result.valid()) {
                        const sol::error error = result;
                        log(LogLevel::Error, std::string("editor.play_from failed: ") + error.what());
                    }
                } else {
                    log(LogLevel::Info, "The game has no editor.play_from hook: playing from its start");
                }
            }
        } catch (const std::exception& error) {
            log(LogLevel::Error, std::string("editor.play_from: ") + error.what());
        }
    }

    view_.onKey = [this](QKeyEvent* event) {
        key(event, true);
    };
    view_.onKeyRelease = [this](QKeyEvent* event) {
        key(event, false);
    };
    const auto mouse = [this](const ViewportWindow::Pointer& pointer) {
        engine_->input().feedMouse(engine_->canvas().fromWindow(pointer.position, view_.pixels()));
    };
    const auto buttons = [this, mouse](const ViewportWindow::Pointer& pointer) {
        mouse(pointer);
        auto& input = engine_->input();
        input.feed("mouse:left", pointer.buttons.testFlag(Qt::LeftButton) ? 1.0f : 0.0f);
        input.feed("mouse:right", pointer.buttons.testFlag(Qt::RightButton) ? 1.0f : 0.0f);
        input.feed("mouse:middle", pointer.buttons.testFlag(Qt::MiddleButton) ? 1.0f : 0.0f);
        view_.requestActivate();
    };
    view_.onPress = buttons;
    view_.onRelease = buttons;
    view_.onMove = mouse;
    view_.onWheel = [this](float notches) {
        engine_->input().feedWheel(notches);
    };
    view_.onLeave = nullptr;
    view_.onDrop = nullptr;
}

PlaySession::~PlaySession() {
    view_.onKey = nullptr;
    view_.onKeyRelease = nullptr;
    view_.onPress = nullptr;
    view_.onRelease = nullptr;
    view_.onMove = nullptr;
    view_.onWheel = nullptr;
    pads_.reset();
    frame_.reset();
    engine_.reset();
    if (gamepads_) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
}

void PlaySession::key(QKeyEvent* event, bool down) {
    if (event->isAutoRepeat()) {
        return;
    }
    auto& input = engine_->input();
    if (const auto name = keyName(*event); !name.empty()) {
        input.feed(name, down ? 1.0f : 0.0f);
    }
    // A text field of the game's is open: what the key types goes to it.
    if (down && input.textEntry().active) {
        const QString typed = event->text();
        if (!typed.isEmpty() && typed.at(0).isPrint()) {
            input.feedText(typed.toStdString());
        }
    }
}

void PlaySession::releaseInput() {
    engine_->input().focusLost();
}

void PlaySession::tick(double dt) {
    engine_->tick(dt);
    ++ticks_;
}

void PlaySession::step() {
    if (paused) {
        tick(fixed_.interval());
    }
}

void PlaySession::advance(double seconds) {
    if (pads_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            pads_->process(event, engine_->canvas(), view_.pixels(), 1.0f, false);
        }
    }
    (void)engine_->input().takeTextEntryChange(); // Qt needs no telling; keys carry their text
    if (!paused) {
        fixed_.advance(seconds, [&](double dt) { tick(dt); });
    }
}

std::uint16_t PlaySession::render(std::uint16_t firstView, const Camera* sceneCamera, float sceneAspect) {
    const auto started = std::chrono::steady_clock::now();
    const bool shown = view_.ready();
    const auto size = shown ? view_.pixels() : glm::max(engine_->viewport(), glm::ivec2(16));
    if (engine_->viewport() != size) {
        engine_->setViewport(size.x, size.y);
    }
    engine_->setGpuWarmActive(true);
    frame_->warm(*engine_);
    const double alpha = paused ? 1.0 : fixed_.alpha();
    engine_->draw(alpha);
    auto& requests = engine_->requests();
    for (const auto& file : requests.sceneCaptures) {
        frame_->captureScene(file);
    }
    requests.sceneCaptures.clear();
    requests.captures.clear();
    requests.display.reset();
    requests.title.reset();
    std::vector<WorldDraw> draws = engine_->drawList();
    if (const auto world = sceneCamera ? shownWorld() : nullptr) {
        // One more draw of the world, into a render target the Scene view
        // shows; sized in canvas units like any world:draw{target=...}.
        WorldDraw scene;
        scene.world = world;
        scene.target = SceneTarget;
        const auto height = static_cast<float>(engine_->canvas().height());
        scene.rect = Rect{0, 0, height * std::max(sceneAspect, 0.1f), height};
        scene.camera = *sceneCamera;
        scene.snapCamera = true;
        draws.push_back(std::move(scene));
        frame_->keepTarget(SceneTarget, SceneTarget);
    }
    FrameOutput out;
    out.target = shown ? view_.target() : bgfx::FrameBufferHandle BGFX_INVALID_HANDLE;
    out.firstView = firstView;
    const auto next = frame_->render(draws, engine_->canvas(), engine_->screen(), size, alpha,
                                     engine_->gameTime(), engine_->ticks(), out);
    frameMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    engine_->stats().drawCalls = frame_->drawCalls();
    return next;
}

bgfx::TextureHandle PlaySession::sceneTexture() const {
    return frame_->keptTarget(SceneTarget);
}

void PlaySession::finishFrame(std::uint32_t frame) {
    frame_->finishFrame(frame);
}

bool PlaySession::wantsQuit() const {
    return engine_->requests().quit;
}

void PlaySession::reload(const std::filesystem::path& file) {
    engine_->reload(file);
    if (file.extension() == ".sc" || file.extension() == ".bin") {
        frame_->reload();
    }
}

std::shared_ptr<World> PlaySession::shownWorld() const {
    if (!chosenWorld_.empty()) {
        if (auto chosen = engine_->world(chosenWorld_)) {
            return chosen;
        }
    }
    if (auto focus = engine_->focusWorld()) {
        return focus;
    }
    const auto& worlds = engine_->worlds();
    return worlds.empty() ? nullptr : worlds.begin()->second;
}

std::vector<std::string> PlaySession::worldNames() const {
    std::vector<std::string> names;
    for (const auto& [name, world] : engine_->worlds()) {
        names.push_back(name);
    }
    return names;
}

std::uint32_t PlaySession::drawCalls() const {
    return frame_->drawCalls();
}
} // namespace fe::editor
