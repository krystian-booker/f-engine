#include "engine/core/App.h"
#include "engine/core/FixedStep.h"
#include "engine/core/Log.h"
#include "engine/platform/FileWatcher.h"
#include "engine/platform/Window.h"
#include "engine/render/Renderer.h"
#include "engine/scene/GameSession.h"
#include "engine/rpg/GameState.h"
#include "engine/script/ScriptEngine.h"
#include "engine/world/World.h"
#if FENGINE_DEV
#include "engine/editor/Editor.h"
#endif
#include <SDL3/SDL.h>
#include <imgui.h>
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace fe {
#if FENGINE_DEV
namespace {
struct ShaderBuildProcess {
    SDL_Process* process=nullptr;
    ~ShaderBuildProcess() {if (process) SDL_DestroyProcess(process);}
};
}
#endif
namespace {
struct ReplayGamepad {
    SDL_JoystickID id=0;
    SDL_Joystick* joystick=nullptr;
    void disconnect() {
        if (joystick) SDL_CloseJoystick(joystick);
        joystick=nullptr;
        if (id) SDL_DetachVirtualJoystick(id);
        id=0;
    }
    ~ReplayGamepad() {disconnect();}
    void connect() {
        if (id) throw std::runtime_error("Replay gamepad already connected");
        SDL_VirtualJoystickDesc desc{};SDL_INIT_INTERFACE(&desc);
        desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.name="f-engine replay controller";
        desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
        id=SDL_AttachVirtualJoystick(&desc);
        if (!id || !(joystick=SDL_OpenJoystick(id))) throw std::runtime_error(SDL_GetError());
    }
};
}
App::App(AppOptions options) : options_(std::move(options)), config_(GameConfig::load(options_.gameFolder)) {}
int App::run() {
    Window window(config_);
    auto shaderRoot = options_.shaderFolder;
    if (shaderRoot.empty()) {
        const auto installed = std::filesystem::path(SDL_GetBasePath()) / "shaders";
        shaderRoot = std::filesystem::exists(installed) ? installed : std::filesystem::path(FENGINE_SHADER_DIR);
    }
    Renderer renderer(window, shaderRoot);
    auto userData=options_.userDataFolder;
    if (userData.empty()) {
        auto* path=SDL_GetPrefPath("fengine",config_.title.c_str());
        if (!path) throw std::runtime_error(std::string("User data path: ")+SDL_GetError());
        userData=path;SDL_free(path);
    }
    GameSession game(options_.gameFolder,userData,shaderRoot,config_,options_.mute,options_.skipTitle);
    if (!options_.startBattle.empty()) game.startBattle(options_.startBattle);
    auto module=options_.moduleFactory?options_.moduleFactory():nullptr;
    if (module) module->onStart(game);
#if FENGINE_DEV
    Editor editor(game);
    if (options_.showTools) editor.toggle();
#endif
#if FENGINE_DEV
    FileWatcher watcher(std::filesystem::weakly_canonical(options_.gameFolder));
    FileWatcher shaderWatcher(shaderRoot);
    std::optional<FileWatcher> shaderSources;
    std::error_code shaderPathError;
    if (std::filesystem::equivalent(shaderRoot,FENGINE_SHADER_DIR,shaderPathError) && std::filesystem::is_directory(FENGINE_SHADER_SOURCE_DIR)) shaderSources.emplace(FENGINE_SHADER_SOURCE_DIR);
    ShaderBuildProcess shaderBuild;bool shaderBuildQueued=false;
#endif
    Json replay=Json::array();std::size_t replayIndex=0;
    if (!options_.replayPath.empty()) {
        replay=readJson(options_.replayPath);
        if (!replay.is_array()) throw std::runtime_error("Replay must be an array of tick-stamped commands");
        std::uint64_t previousTick=0;
        for (const auto& command:replay) {
            const auto tick=command.at("tick").get<std::uint64_t>();
            if (tick<previousTick) throw std::runtime_error("Replay commands must be sorted by tick");previousTick=tick;
            for (const auto& [key,value]:command.items()) {
                (void)value;
                if (key!="tick" && key!="key" && key!="down" && key!="capture" && key!="script" && key!="resize" && key!="assert_map" && key!="assert_flags" && key!="assert_gold" && key!="assert_inventory" && key!="gamepad" && key!="button" && key!="axis" && key!="value" && key!="assert_position") throw std::runtime_error("Unknown replay command: "+key);
            }
        }
        if (!options_.frameLimit) options_.frameLimit=previousTick+30;
        if (options_.frameLimit<=previousTick) throw std::runtime_error("Replay frame limit must allow all commands to execute");
    }
    ReplayGamepad replayPad;
    std::filesystem::path pendingCapture;
    FixedStep loop;
    bool running = true;
    std::uint64_t frames = 0, ticks = 0;
    double watchElapsed = 0;
    std::map<SDL_JoystickID, SDL_Gamepad*> gamepads;
    auto openGamepad = [&](SDL_JoystickID id) {
        if (gamepads.contains(id)) return;
        if (auto* pad = SDL_OpenGamepad(id)) {
            gamepads.emplace(id, pad);
            const auto* name=SDL_GetGamepadName(pad);log(LogLevel::Info,std::string("Gamepad connected: ")+(name?name:"Unnamed controller"));
        }
    };
    int padCount = 0;
    auto* pads = SDL_GetGamepads(&padCount);
    for (int i = 0; i < padCount; ++i) openGamepad(pads[i]);
    SDL_free(pads);
    auto previous = std::chrono::steady_clock::now();
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            renderer.processEvent(event);
            if (options_.replayPath.empty() || (event.type>=SDL_EVENT_GAMEPAD_AXIS_MOTION && event.type<=SDL_EVENT_GAMEPAD_REMAPPED)) game.processEvent(event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F1)
                {
#if FENGINE_DEV
                    editor.toggle();
#endif
                }
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) openGamepad(event.gdevice.which);
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                auto found = gamepads.find(event.gdevice.which);
                if (found != gamepads.end()) { SDL_CloseGamepad(found->second); gamepads.erase(found); }
                log(LogLevel::Info, "Gamepad disconnected");
            }
        }
        if (!running) break;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = options_.replayPath.empty()?std::chrono::duration<double>(now - previous).count():1.0/60;
        previous = now;
        loop.advance(elapsed, [&](double dt) {
            while (replayIndex<replay.size() && replay[replayIndex].at("tick").get<std::uint64_t>()<=ticks+1) {
                const auto& command=replay[replayIndex++];
                if (command.contains("gamepad")) {
                    const auto operation=command.at("gamepad").get<std::string>();
                    if (operation=="connect") replayPad.connect();else if (operation=="disconnect") replayPad.disconnect();
                    else throw std::runtime_error("Unknown replay gamepad operation");
                }
                if (command.contains("button")) {
                    const auto button=SDL_GetGamepadButtonFromString(command.at("button").get<std::string>().c_str());
                    if (!replayPad.joystick || button==SDL_GAMEPAD_BUTTON_INVALID || !SDL_SetJoystickVirtualButton(replayPad.joystick,button,command.value("down",true))) throw std::runtime_error("Invalid replay gamepad button");
                }
                if (command.contains("axis")) {
                    const auto axis=SDL_GetGamepadAxisFromString(command.at("axis").get<std::string>().c_str());const float value=command.at("value");
                    if (!replayPad.joystick || axis==SDL_GAMEPAD_AXIS_INVALID || !std::isfinite(value) || std::abs(value)>1 || !SDL_SetJoystickVirtualAxis(replayPad.joystick,axis,static_cast<Sint16>(value*32767))) throw std::runtime_error("Invalid replay gamepad axis");
                }
                if (command.contains("key")) {
                    SDL_Event key{};key.type=command.value("down",true)?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
                    key.key.scancode=SDL_GetScancodeFromName(command.at("key").get<std::string>().c_str());key.key.down=command.value("down",true);
                    if (key.key.scancode==SDL_SCANCODE_UNKNOWN) throw std::runtime_error("Unknown replay key");
                    game.processEvent(key);
#if FENGINE_DEV
                    if (key.key.scancode==SDL_SCANCODE_F1 && key.key.down) editor.toggle();
#endif
                }
                if (command.contains("capture")) pendingCapture=command.at("capture").get<std::string>();
                if (command.contains("script")) game.scripts().start(command.at("script").get<std::string>());
                if (command.contains("resize")) {
                    const auto& size=command.at("resize");
                    if (!SDL_SetWindowSize(window.native(),size.at(0).get<int>(),size.at(1).get<int>())) throw std::runtime_error(SDL_GetError());
                }
                if (command.contains("assert_map") && game.state().map!=command.at("assert_map").get<std::string>()) throw std::runtime_error("Replay map assertion failed");
                if (command.contains("assert_flags")) for (const auto& [flag,value]:command.at("assert_flags").items())
                    if (game.state().flag(flag)!=value.get<bool>()) throw std::runtime_error("Replay flag assertion failed: "+flag);
                if (command.contains("assert_gold") && game.state().gold!=command.at("assert_gold").get<int>()) throw std::runtime_error("Replay gold assertion failed");
                if (command.contains("assert_inventory")) for (const auto& [id,count]:command.at("assert_inventory").items())
                    if (game.state().itemCount(id)!=count.get<int>()) throw std::runtime_error("Replay inventory assertion failed: "+id);
                if (command.contains("assert_position")) {
                    const auto& position=command.at("assert_position");
                    if (glm::distance(game.world().playerPosition(),glm::vec3(position.at(0),position.at(1),position.at(2)))>0.15f) throw std::runtime_error("Replay position assertion failed");
                }
            }
#if FENGINE_DEV
            game.update(dt,editor.paused());
            if (module && !editor.paused()) module->onFixedUpdate(game,dt);
#else
            game.update(dt);
            if (module) module->onFixedUpdate(game,dt);
#endif
            ++ticks;
        });
        watchElapsed += elapsed;
#if FENGINE_DEV
        if (watchElapsed >= 0.25) {
            watchElapsed = 0;
            for (const auto& change : watcher.poll()) {
                log(LogLevel::Info,"Asset changed: "+change.path.string());
                if (change.kind!=FileChangeKind::Removed) game.reload(change.path);
            }
            const auto shaderChanges=shaderWatcher.poll();
            const auto changed=std::find_if(shaderChanges.begin(),shaderChanges.end(),[](const auto& change){return change.kind!=FileChangeKind::Removed;});
            if (changed!=shaderChanges.end()) {
                game.reload(changed->path);
                try {renderer.reloadShaders();} catch (const std::exception& error) {log(LogLevel::Error,error.what());}
            }
            if (shaderSources) for (const auto& change:shaderSources->poll()) if (change.path.extension()==".sc" || change.path.extension()==".sh") shaderBuildQueued=true;
        }
        if (shaderBuild.process) {
            int exitCode=0;
            if (SDL_WaitProcess(shaderBuild.process,false,&exitCode)) {
                SDL_DestroyProcess(shaderBuild.process);shaderBuild.process=nullptr;
                log(exitCode==0?LogLevel::Info:LogLevel::Error,exitCode==0?"Shaders rebuilt":"Shader build failed; keeping last valid programs");
            }
        }
        if (shaderBuildQueued && !shaderBuild.process) {
            const char* arguments[]{FENGINE_CMAKE,"--build",FENGINE_BUILD_DIR,"--target","fengine_shaders",nullptr};
            shaderBuild.process=SDL_CreateProcess(arguments,false);shaderBuildQueued=false;
            if (!shaderBuild.process) log(LogLevel::Error,std::string("Cannot start shader compiler: ")+SDL_GetError());
        }
#endif
        if (renderer.beginFrame()) {
            const auto [width,height]=window.pixelSize();
            game.render(loop.alpha(),width,height);
#if FENGINE_DEV
            editor.draw(ticks);
#endif
            const auto captureFrame=options_.frameLimit?std::min<std::uint64_t>(30,std::max<std::uint64_t>(1,options_.frameLimit/2)):30;
            if (!options_.capturePath.empty() && frames+1==captureFrame) {
                if (options_.capturePath.has_parent_path()) std::filesystem::create_directories(options_.capturePath.parent_path());
                bgfx::requestScreenShot(BGFX_INVALID_HANDLE,options_.capturePath.string().c_str());
            }
            if (!pendingCapture.empty()) {
                if (pendingCapture.has_parent_path()) std::filesystem::create_directories(pendingCapture.parent_path());
                bgfx::requestScreenShot(BGFX_INVALID_HANDLE,pendingCapture.string().c_str());pendingCapture.clear();
            }
            renderer.endFrame();
            ++frames;
            if (options_.frameLimit && frames >= options_.frameLimit) running = false;
        } else SDL_Delay(16);
        if (options_.replayRealtime && !options_.replayPath.empty()) {
            const auto spent=std::chrono::duration<double>(std::chrono::steady_clock::now()-now).count();
            if (spent<1.0/60) SDL_DelayNS(static_cast<Uint64>((1.0/60-spent)*1e9));
        }
    }
    if (replayIndex!=replay.size()) throw std::runtime_error("Replay stopped before all commands executed");
    for (auto [id, pad] : gamepads) { (void)id; SDL_CloseGamepad(pad); }
    if (!options_.dumpStatePath.empty()) writeJson(options_.dumpStatePath,game.state().toJson());
    if (module) module->onStop(game);
    log(LogLevel::Info, "Runtime stopped after " + std::to_string(frames) + " frames");
    return 0;
}
}
