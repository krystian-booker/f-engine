#include "engine/runtime/Engine.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <chrono>

namespace fe {
Engine::Engine(EngineOptions options)
    : config_(GameConfig::load(options.gameRoot)), development_(options.development),
      seed_(options.seed.value_or(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()))),
      assets_(std::make_unique<AssetCache>(options.gameRoot, options.libraryRoot)),
      text_(std::make_unique<TextSystem>(*assets_)),
      canvas_(std::make_unique<Canvas>(*text_, config_.virtualWidth, config_.virtualHeight)),
      audio_(options.audio ? std::move(options.audio) : std::make_unique<NullAudio>()),
      saves_(options.userData),
      lua_(std::make_unique<LuaHost>(options.gameRoot, options.libraryRoot, options.development)) {
    if (!std::filesystem::is_directory(options.libraryRoot)) {
        throw std::runtime_error("Lua library folder not found: " + options.libraryRoot.string());
    }
    assets_->defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    bind();
}

Engine::~Engine() {
    // Worlds release their script references while the Lua state is alive.
    main_ = sol::object();
    drawList_.clear();
    worlds_.clear();
    text_->setResolver({}); // holds a Lua function
    lua_.reset();
}

bool Engine::hasMain() const {
    return main_.valid() && main_.get_type() == sol::type::table;
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
}

bool Engine::start() {
    lua_->beginFrame();
    try {
        main_ = lua_->require(config_.entry);
    } catch (const std::exception& error) {
        lua_->report(std::string("Cannot load entry module '") + config_.entry + "':\n" + error.what());
        return false;
    }
    if (!hasMain()) {
        lua_->report("The entry module must return a table with init/update/draw functions");
        return false;
    }
    sol::table main = main_;
    if (main["init"].valid()) {
        return lua_->call(main["init"]);
    }
    return true;
}

void Engine::tick(double dt) {
    ++ticks_;
    time_ += dt;
    assets_->collect();
    input_.tick(dt);
    for (const auto& [name, world] : worlds_) {
        (void)name;
        world->beginTick();
    }
    lua_->beginFrame();
    const auto start = std::chrono::steady_clock::now();
    if (hasMain()) {
        sol::table main = main_;
        lua_->call(main["update"], dt);
    }
    stats_.luaUpdate =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // Copy: scripts may load or unload worlds during the tick.
    const auto worlds = worlds_;
    for (const auto& [name, world] : worlds) {
        try {
            world->tick(dt);
        } catch (const std::exception& error) {
            // A simulation fault is shown like a script error instead of closing the game.
            lua_->reportOnce("World '" + name + "' tick failed: " + error.what());
        }
    }
    updateAudioEmitters();
    audio_->update(dt);
}

void Engine::draw(double alpha) {
    canvas_->begin();
    drawList_.clear();
    lua_->beginFrame();
    const auto start = std::chrono::steady_clock::now();
    if (hasMain()) {
        sol::table main = main_;
        lua_->call(main["draw"], alpha);
    }
    stats_.luaDraw =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!drawList_.empty()) {
        focus_ = drawList_.back();
    }
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
    auto map = MapData::load(resolveMap(mapId));
    if (map.id != mapId) {
        throw std::runtime_error("Map folder '" + mapId + "' contains map id '" + map.id + "'");
    }
    auto world = std::make_shared<World>(name, std::move(map), *assets_, nextSeed());
    worlds_[name] = world;
    return world;
}

std::shared_ptr<World> Engine::world(const std::string& name) const {
    const auto found = worlds_.find(name);
    return found == worlds_.end() ? nullptr : found->second;
}

void Engine::unloadWorld(const std::string& name) {
    worlds_.erase(name);
    std::erase_if(drawList_, [&](const auto& w) { return w->name() == name; });
}

std::shared_ptr<World> Engine::focusWorld() const {
    if (auto world = focus_.lock()) {
        return world;
    }
    return worlds_.empty() ? nullptr : worlds_.begin()->second;
}

void Engine::reload(const std::filesystem::path& file) {
    const auto extension = file.extension().string();
    if (extension == ".lua") {
        lua_->reloadFile(file);
        return;
    }
    assets_->reload(file);
    audio_->reload(file);
    if (hasMain()) {
        sol::table main = main_;
        const auto relative = relativeTo(lua_->gameRoot(), file);
        if (!relative.empty()) {
            lua_->call(main["file_changed"], relative);
        }
    }
}

void Engine::updateAudioEmitters() {
    const auto world = focusWorld();
    if (!world || !audio_->available()) {
        return;
    }
    const auto listener = world->camera().target;
    const auto right = world->camera().right();
    auto& registry = world->registry();
    for (const auto entity : registry.view<Transform, AudioEmitter>()) {
        auto& emitter = registry.get<AudioEmitter>(entity);
        const auto offset = registry.get<Transform>(entity).position - listener;
        const float distance = glm::length(offset);
        const float gain = emitter.volume * std::max(0.0f, 1 - distance / std::max(0.1f, emitter.radius));
        const float pan =
            distance > 0.01f ? std::clamp(glm::dot(offset / distance, right), -1.0f, 1.0f) : 0.0f;
        if (emitter.voice < 0 && gain > 0) {
            SoundOptions options;
            options.loop = true;
            options.volume = gain;
            options.pan = pan;
            try {
                emitter.voice = audio_->playSound(resolveInside(lua_->gameRoot(), emitter.sound), options);
            } catch (const std::exception& error) {
                log(LogLevel::Error, error.what());
                emitter.sound.clear();
            }
        } else if (emitter.voice >= 0) {
            audio_->setSound(emitter.voice, gain, pan);
        }
    }
}

void Engine::writeApi(const std::filesystem::path& stubs, const std::filesystem::path& markdown) const {
    docs_.writeStubs(stubs);
    docs_.writeMarkdown(markdown);
}
} // namespace fe
