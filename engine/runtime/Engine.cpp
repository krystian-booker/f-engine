#include "engine/runtime/Engine.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace fe {
Engine::Engine(EngineOptions options) : Engine(std::move(options), std::nullopt) {
}

Engine::Engine(EngineOptions&& options, std::optional<GameConfig> config)
    : config_(config ? std::move(*config) : GameConfig::load(options.gameRoot)),
      development_(options.development), testHooks_(options.testHooks),
      seed_(options.seed.value_or(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()))),
      assets_(std::make_unique<AssetCache>(options.gameRoot, options.libraryRoot, options.userData)),
      text_(std::make_unique<TextSystem>(*assets_)),
      canvas_(std::make_unique<Canvas>(*text_, config_.virtualWidth, config_.virtualHeight)),
      audio_(options.audio ? std::move(options.audio) : std::make_unique<NullAudio>()),
      saves_(options.userData),
      platform_(options.platform ? std::move(options.platform)
                                 : std::make_unique<LocalPlatformServices>(
                                       options.userData.empty() ? std::filesystem::path()
                                                                : options.userData / "platform.json")),
      locale_(options.locale.empty() ? "en-US" : options.locale),
      lua_(std::make_unique<LuaHost>(options.gameRoot, options.libraryRoot, options.development)) {
    if (!isFolder(options.libraryRoot)) {
        throw std::runtime_error("Lua library folder not found: " + options.libraryRoot.string());
    }
    assets_->defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    display_.width = config_.windowWidth;
    display_.height = config_.windowHeight;
    const DisplayMode window{config_.windowWidth, config_.windowHeight, 60};
    display_.monitors.push_back({"Display", window, {window}});
    scriptState_ = std::make_shared<lua_State*>(lua_->lua().lua_state());
    bind();
}

Engine::~Engine() {
    // From here on every engine binding refuses, so the finalizers that run
    // while the state closes cannot load worlds, spawn entities or keep new
    // references into the state.
    lua_->beginShutdown();
    // Release every reference into the Lua state while it is alive: worlds
    // unreference their entities' data tables, the resolver holds a function.
    main_ = sol::object();
    drawList_.clear();
    worlds_.clear();
    text_->setResolver({});
    // Close the state while the rest of the engine is still whole.
    *scriptState_ = nullptr;
    lua_->close();
}

template<class... Args> bool Engine::callMain(const char* name, Args&&... args) {
    if (!main_.valid() || main_.get_type() != sol::type::table) {
        return true;
    }
    sol::object function;
    try {
        function = field(main_.as<sol::table>(), name);
    } catch (const std::exception& error) {
        lua_->report(std::string("Reading main.") + name + ": " + error.what());
        return false;
    }
    return function.get_type() != sol::type::function || lua_->call(function, std::forward<Args>(args)...);
}

void Engine::bind() {
    bindCore(*this);
    bindSave(*this);
    bindInput(*this);
    bindText(*this);
    bindCanvas(*this);
    bindAudio(*this);
    bindScreen(*this);
    bindWorld(*this);
    bindPlatform(*this);
    lua_->sealTypes();
    lua_->lua()["math"]["randomseed"](static_cast<lua_Integer>(seed_ >> 1u));
}

bool Engine::start() {
    lua_->beginFrame();
    try {
        main_ = lua_->require(config_.entry);
    } catch (const std::exception& error) {
        lua_->report(std::string("Cannot load entry module '") + config_.entry + "':\n" + error.what());
        return false;
    }
    if (main_.get_type() != sol::type::table) {
        lua_->report("The entry module must return a table with init/update/draw functions");
        return false;
    }
    return callMain("init");
}

void Engine::tick(double dt) {
    ++ticks_;
    time_ += dt;
    assets_->collect();
    for (auto& [ticket, load] : loads_) {
        advance(load);
    }
    input_.tick(dt);
    for (const auto& entry : worlds_) {
        entry.second->beginTick();
    }
    lua_->beginFrame();
    const auto start = std::chrono::steady_clock::now();
    callMain("update", dt);
    stats_.luaUpdate =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // Copy: scripts may load or unload worlds during the tick.
    const auto worlds = worlds_;
    for (const auto& entry : worlds) {
        auto& world = *entry.second;
        if (!world.paused) {
            world.tick(dt * world.timeScale);
        }
    }
    callMain("post_update", dt);
    updateAudioEmitters();
    audio_->update(dt);
    try {
        platform_->update();
    } catch (const std::exception& error) {
        log(LogLevel::Error, std::string("Platform services: ") + error.what());
    }
}

void Engine::requestDisplay(const DisplayRequest& request) {
    if (request.monitor &&
        (*request.monitor < 1 || *request.monitor > static_cast<int>(display_.monitors.size()))) {
        throw std::runtime_error("Monitor " + std::to_string(*request.monitor) +
                                 " does not exist (there are " + std::to_string(display_.monitors.size()) +
                                 ")");
    }
    for (const auto& size : {request.width, request.height}) {
        if (size && (*size < 64 || *size > 16384)) {
            throw std::runtime_error("Window sizes are 64 to 16384");
        }
    }
    if (requests_.display) {
        requests_.display->merge(request);
    } else {
        requests_.display = request;
    }
    request.applyTo(display_);
}

void Engine::draw(double alpha) {
    drawAlpha_ = alpha;
    canvas_->begin();
    assets_->releaseRetiredFonts();
    drawList_.clear();
    lua_->beginFrame();
    const auto start = std::chrono::steady_clock::now();
    callMain("draw", alpha);
    stats_.luaDraw =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // The editor follows the last world drawn full screen.
    for (auto draw = drawList_.rbegin(); draw != drawList_.rend(); ++draw) {
        if (!draw->rect && draw->target.empty()) {
            focus_ = draw->world;
            break;
        }
    }
    drawAlpha_ = 1;
}

std::filesystem::path Engine::resolveMap(const std::string& mapId) const {
    if (mapId.empty() ||
        mapId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
        throw std::runtime_error("Invalid map id '" + mapId + "'");
    }
    return resolveInside(lua_->gameRoot(), "maps/" + mapId + "/map.json");
}

void Engine::setViewport(int width, int height) {
    viewport_ = {std::max(1, width), std::max(1, height)};
}

std::shared_ptr<World> Engine::loadWorld(const std::string& name, const std::string& mapId) {
    if (name.empty()) {
        throw std::runtime_error("Worlds need a name");
    }
    return addWorld(name, mapId, MapData::load(resolveMap(mapId)));
}

int Engine::loadWorldAsync(const std::string& name, const std::string& mapId) {
    if (name.empty()) {
        throw std::runtime_error("Worlds need a name");
    }
    PendingWorld load;
    load.name = name;
    load.mapId = mapId;
    load.parse = std::async(std::launch::async, [file = resolveMap(mapId)] { return MapData::load(file); });
    const int ticket = nextLoad_++;
    loads_.emplace(ticket, std::move(load));
    return ticket;
}

// Parsing, then the map's models, images and sprite animations, then the
// atlases those animations name.
void Engine::advance(PendingWorld& load) {
    using Stage = PendingWorld::Stage;
    if (load.stage == Stage::Parsing) {
        if (load.parse.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        try {
            load.map = load.parse.get();
        } catch (const std::exception& error) {
            load.error = error.what();
            load.stage = Stage::Failed;
            return;
        }
        const auto& environment = load.map->environment;
        std::set<std::string> paths{"maps/" + load.map->id + "/" + load.map->geometry};
        std::set<std::string> sprites;
        for (const auto& path :
             {environment.sky.panorama, environment.post.lut, environment.post.lutBlendTarget}) {
            if (!path.empty()) {
                paths.insert(path);
            }
        }
        for (const auto& backdrop : environment.sky.backdrops) {
            if (!backdrop.image.empty()) {
                paths.insert(backdrop.image);
            }
        }
        for (const auto& emitter : load.map->emitters) {
            if (!emitter.settings.sprite.empty()) {
                sprites.insert(emitter.settings.sprite);
            }
        }
        if (!environment.weather.sprite.empty()) {
            sprites.insert(environment.weather.sprite);
        }
        paths.insert(sprites.begin(), sprites.end());
        load.sprites.assign(sprites.begin(), sprites.end());
        load.assetCount = paths.size();
        load.ticket = assets_->preload({paths.begin(), paths.end()});
        load.stage = Stage::Assets;
    }
    if (load.stage == Stage::Assets) {
        if (assets_->progress(load.ticket) < 1) {
            return;
        }
        std::vector<std::string> atlases;
        for (const auto& path : load.sprites) {
            if (const auto sprite = assets_->trySprite(path)) {
                atlases.push_back(
                    (std::filesystem::path(path).parent_path() / sprite->atlas).generic_string());
            }
        }
        load.ticket = assets_->preload(atlases);
        load.stage = Stage::Atlases;
    }
    if (load.stage == Stage::Atlases && assets_->progress(load.ticket) >= 1) {
        load.stage = Stage::Ready;
    }
}

float Engine::loadProgress(int ticket) {
    using Stage = PendingWorld::Stage;
    const auto found = loads_.find(ticket);
    if (found == loads_.end()) {
        throw std::runtime_error("Unknown world load ticket " + std::to_string(ticket));
    }
    auto& load = found->second;
    advance(load);
    const auto total = static_cast<float>(1 + load.assetCount + load.sprites.size());
    switch (load.stage) {
    case Stage::Parsing:
        return 0;
    case Stage::Assets:
        return (1 + assets_->progress(load.ticket) * static_cast<float>(load.assetCount)) / total;
    case Stage::Atlases:
        return (1 + static_cast<float>(load.assetCount) +
                assets_->progress(load.ticket) * static_cast<float>(load.sprites.size())) /
               total;
    case Stage::Ready:
    case Stage::Failed:
        break;
    }
    return 1;
}

std::shared_ptr<World> Engine::finishLoad(int ticket) {
    const auto found = loads_.find(ticket);
    if (found == loads_.end()) {
        throw std::runtime_error("Unknown world load ticket " + std::to_string(ticket));
    }
    auto load = std::move(found->second);
    loads_.erase(found);
    if (load.stage == PendingWorld::Stage::Parsing) {
        load.parse.wait();
        advance(load);
    }
    if (load.stage == PendingWorld::Stage::Failed) {
        throw std::runtime_error(load.error);
    }
    assets_->collect();
    return addWorld(load.name, load.mapId, std::move(*load.map));
}

std::shared_ptr<World> Engine::addWorld(const std::string& name, const std::string& mapId, MapData map) {
    if (map.id != mapId) {
        throw std::runtime_error("Map folder '" + mapId + "' contains map id '" + map.id + "'");
    }
    auto world = std::make_shared<World>(name, std::move(map), *assets_, nextSeed());
    // The hook may outlive the Lua state (a world something else still holds
    // when the engine shuts down); the shared cell is cleared when it closes.
    world->setDestroyHook([raw = world.get(), state = scriptState_](entt::entity entity) {
        auto& ref = raw->registry().get<ScriptRef>(entity);
        if (ref.reference != LUA_NOREF && *state) {
            luaL_unref(*state, LUA_REGISTRYINDEX, ref.reference);
        }
        ref.reference = LUA_NOREF;
    });
    worlds_[name] = world;
    return world;
}

std::shared_ptr<World> Engine::world(const std::string& name) const {
    const auto found = worlds_.find(name);
    return found == worlds_.end() ? nullptr : found->second;
}

void Engine::unloadWorld(const std::string& name) {
    worlds_.erase(name);
    std::erase_if(drawList_, [&](const auto& draw) { return draw.world->name() == name; });
}

std::shared_ptr<World> Engine::focusWorld() const {
    if (auto world = focus_.lock()) {
        return world;
    }
    return worlds_.empty() ? nullptr : worlds_.begin()->second;
}

void Engine::reload(const std::filesystem::path& file) {
    if (file.extension() == ".lua") {
        lua_->reloadFile(file);
        return;
    }
    assets_->reload(file);
    audio_->reload(file);
    const auto relative = relativeTo(lua_->gameRoot(), file);
    if (!relative.empty()) {
        callMain("file_changed", relative);
    }
}

// Looping positional sounds play for the worlds drawn last frame, each heard
// from its own camera. Voices of entities that were destroyed, lost their
// emitter, or whose world was unloaded or not drawn are stopped. Each drawn
// world's camera is also published as the listener for positional one-shots.
void Engine::updateAudioEmitters() {
    if (!audio_->available()) {
        return;
    }
    std::set<int> heard;
    std::set<const World*> visited;
    std::map<std::string, Listener> listeners;
    for (const auto& draw : drawList_) {
        const auto& world = draw.world;
        if (!visited.insert(world.get()).second) {
            continue;
        }
        const Listener listener{world->camera().target, world->camera().right()};
        listeners.emplace(world->name(), listener);
        auto& registry = world->registry();
        for (const auto entity : registry.view<Transform, AudioEmitter>()) {
            auto& emitter = registry.get<AudioEmitter>(entity);
            if (emitter.sound.empty()) {
                continue;
            }
            const auto& position = registry.get<Transform>(entity).position;
            const auto spatial = spatialize(listener, position, emitter.radius);
            const float gain = emitter.volume * spatial.gain;
            const float pan = spatial.pan;
            if (emitter.voice < 0 && gain > 0) {
                SoundOptions options;
                options.loop = true;
                options.volume = gain;
                options.pan = pan;
                try {
                    emitter.voice =
                        audio_->playSound(resolveInside(lua_->gameRoot(), emitter.sound), options);
                } catch (const std::exception& error) {
                    log(LogLevel::Error,
                        "Positional sound '" + emitter.sound + "' disabled: " + error.what());
                    emitter.sound.clear();
                }
            } else if (emitter.voice >= 0) {
                audio_->setSound(emitter.voice, {.volume = gain, .pan = pan});
            }
            if (emitter.voice >= 0) {
                heard.insert(emitter.voice);
                emitterVoices_[emitter.voice] = {world, entity};
            }
        }
    }
    for (auto it = emitterVoices_.begin(); it != emitterVoices_.end();) {
        if (heard.contains(it->first)) {
            ++it;
            continue;
        }
        audio_->stopSound(it->first, 0.3);
        if (const auto world = it->second.world.lock(); world && world->valid(it->second.entity)) {
            if (auto* emitter = world->registry().try_get<AudioEmitter>(it->second.entity);
                emitter && emitter->voice == it->first) {
                emitter->voice = -1;
            }
        }
        it = emitterVoices_.erase(it);
    }
    audio_->setListeners(std::move(listeners));
}

void Engine::writeApi(const std::filesystem::path& stubs, const std::filesystem::path& markdown) const {
    docs_.writeStubs(stubs);
    docs_.writeMarkdown(markdown);
}
} // namespace fe
