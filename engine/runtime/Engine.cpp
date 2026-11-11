#include "engine/runtime/Engine.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/runtime/EmitterAudio.h"
#include "engine/runtime/WorldLoader.h"
#include "engine/script/Binder.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace fe {
// The engine's references into the Lua state, kept out of Engine.h so that
// its consumers do not compile sol2.
struct Engine::Script {
    std::shared_ptr<lua_State*> state; // null once the Lua state is closed
    sol::object main;                  // the entry module, once start() loaded it
    // Calls an optional function of the entry module; false on script error.
    template<class... Args> bool call(LuaHost& lua, const char* name, Args&&... args) {
        if (!main.valid() || main.get_type() != sol::type::table) {
            return true;
        }
        sol::object function;
        try {
            function = field(main.as<sol::table>(), name);
        } catch (const std::exception& error) {
            lua.report(std::string("Reading main.") + name + ": " + error.what());
            return false;
        }
        return function.get_type() != sol::type::function || lua.call(function, std::forward<Args>(args)...);
    }
};

Engine::Engine(EngineOptions options) : Engine(std::move(options), std::nullopt) {
}

Engine::Engine(EngineOptions&& options, std::optional<GameConfig> config)
    : config_(config ? std::move(*config) : GameConfig::load(options.gameRoot)),
      development_(options.development), testHooks_(options.testHooks),
      seed_(options.seed.value_or(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()))),
      assets_(std::make_unique<AssetCache>(options.gameRoot, options.builtinRoot, options.userData,
                                           options.assetBudget)),
      text_(std::make_unique<TextSystem>(*assets_)),
      canvas_(std::make_unique<Canvas>(*text_, config_.virtualWidth, config_.virtualHeight)),
      audio_(options.audio ? std::move(options.audio) : std::make_unique<NullAudio>()),
      saves_(options.userData, std::move(options.saveBackend)),
      platform_(options.platform ? std::move(options.platform)
                                 : std::make_unique<LocalPlatformServices>(
                                       options.userData.empty() ? std::filesystem::path()
                                                                : options.userData / "platform.json")),
      clipboard_(options.clipboard ? std::move(options.clipboard) : std::make_unique<MemoryClipboard>()),
      locale_(options.locale.empty() ? "en-US" : options.locale),
      loader_(std::make_unique<WorldLoader>(*assets_)), emitters_(std::make_unique<EmitterAudio>()),
      docs_(std::make_unique<BindingDocs>()),
      lua_(std::make_unique<LuaHost>(options.gameRoot, options.builtinRoot, options.development)),
      script_(std::make_unique<Script>()), ownsLogSink_(static_cast<bool>(options.logSink)) {
    if (ownsLogSink_) {
        setLogSink(std::move(options.logSink));
    }
    if (!isFolder(options.builtinRoot)) {
        throw std::runtime_error("Engine asset folder (builtin) not found: " + options.builtinRoot.string());
    }
    // Content packs go over the game folder once game.json is loaded, so a
    // pack cannot change the game's configuration, only its files.
    for (const auto& name : mountOverlayFolder(options.gameRoot / "packs", options.gameRoot)) {
        packs_.push_back("packs/" + name);
    }
    if (!options.userData.empty()) {
        for (const auto& name : mountOverlayFolder(options.userData / "packs", options.gameRoot)) {
            packs_.push_back("user://packs/" + name);
        }
    }
    for (const auto& pack : packs_) {
        log(LogLevel::Info, "Mounted content pack " + pack);
    }
    assets_->defineFont("default", {{"files", {"@fe/assets/fonts/pixel.fnt"}}});
    display_.width = config_.windowWidth;
    display_.height = config_.windowHeight;
    const DisplayMode window{config_.windowWidth, config_.windowHeight, 60};
    display_.monitors.push_back({"Display", window, {window}});
    script_->state = std::make_shared<lua_State*>(lua_->lua().lua_state());
    bind();
}

Engine::~Engine() {
    // From here on every engine binding refuses, so the finalizers that run
    // while the state closes cannot load worlds, spawn entities or keep new
    // references into the state.
    lua_->beginShutdown();
    // Release every reference into the Lua state while it is alive: worlds
    // unreference their entities' data tables, the resolver holds a function.
    script_->main = sol::object();
    drawList_.clear();
    worlds_.clear();
    text_->setResolver({});
    // Close the state while the rest of the engine is still whole.
    *script_->state = nullptr;
    lua_->close();
    if (ownsLogSink_) {
        setLogSink({});
    }
}

bool Engine::hasMain() const {
    return script_->main.valid() && script_->main.get_type() == sol::type::table;
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
        script_->main = lua_->require(config_.entry);
    } catch (const std::exception& error) {
        lua_->report(std::string("Cannot load entry module '") + config_.entry + "':\n" + error.what());
        return false;
    }
    if (script_->main.get_type() != sol::type::table) {
        script_->main = sol::object();
        lua_->report("The entry module must return a table with init/update/draw functions");
        return false;
    }
    return script_->call(*lua_, "init");
}

void Engine::tick(double dt) {
    ++ticks_;
    time_ += dt;
    assets_->collect();
    loader_->advanceAll();
    input_.tick(dt);
    for (const auto& entry : worlds_) {
        entry.second->beginTick();
    }
    lua_->beginFrame();
    const auto start = std::chrono::steady_clock::now();
    script_->call(*lua_, "update", dt);
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
    script_->call(*lua_, "post_update", dt);
    emitters_->update(*audio_, drawList_, lua_->gameRoot());
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
    script_->call(*lua_, "draw", alpha);
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
    if (config_.canvasFit == CanvasFit::Expand) {
        const double aspect = static_cast<double>(viewport_.x) / static_cast<double>(viewport_.y);
        // Rounded down, so the integer presentation scale of the height still fits the width.
        const int expanded = static_cast<int>(std::floor(config_.virtualHeight * aspect));
        canvas_->resize(std::clamp(expanded, config_.virtualWidth, 16384), config_.virtualHeight);
    }
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
    return loader_->begin(name, mapId, resolveMap(mapId));
}

float Engine::loadProgress(int ticket) {
    return loader_->progress(ticket);
}

std::shared_ptr<World> Engine::finishLoad(int ticket) {
    auto finished = loader_->finish(ticket);
    return addWorld(finished.name, finished.mapId, std::move(finished.map));
}

void Engine::cancelLoad(int ticket) {
    loader_->cancel(ticket);
}

std::shared_ptr<World> Engine::addWorld(const std::string& name, const std::string& mapId, MapData map) {
    if (map.id != mapId) {
        throw std::runtime_error("Map folder '" + mapId + "' contains map id '" + map.id + "'");
    }
    auto world = std::make_shared<World>(name, std::move(map), *assets_, nextSeed());
    // The hook may outlive the Lua state (a world something else still holds
    // when the engine shuts down); the shared cell is cleared when it closes.
    world->setDestroyHook([raw = world.get(), state = script_->state](entt::entity entity) {
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
    releaseUnusedAssets();
}

std::set<std::string> Engine::referencedAssets() const {
    std::set<std::string> paths, sprites;
    for (const auto& [name, world] : worlds_) {
        paths.insert(world->geometryPath());
        const auto& environment = world->map().environment;
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
        if (!environment.weather.sprite.empty()) {
            sprites.insert(environment.weather.sprite);
        }
        for (const auto& emitter : world->map().emitters) {
            if (!emitter.settings.sprite.empty()) {
                sprites.insert(emitter.settings.sprite);
            }
        }
        const auto& registry = world->registry();
        for (const auto entity : registry.view<Sprite>()) {
            const auto& sprite = registry.get<Sprite>(entity);
            if (!sprite.animation.empty()) {
                sprites.insert(sprite.animation);
            }
            if (!sprite.palette.empty()) {
                paths.insert(sprite.palette);
            }
        }
        for (const auto entity : registry.view<Emitter>()) {
            const auto& emitter = registry.get<Emitter>(entity);
            if (!emitter.settings.sprite.empty()) {
                sprites.insert(emitter.settings.sprite);
            }
        }
        for (const auto entity : registry.view<MeshInstance>()) {
            const auto& mesh = registry.get<MeshInstance>(entity);
            if (!mesh.model.empty()) {
                paths.insert(mesh.model);
            }
        }
    }
    for (const auto& path : sprites) {
        paths.insert(path);
        if (!assets_->loaded(path)) {
            continue;
        }
        const auto animation = assets_->trySprite(path);
        if (!animation) {
            continue;
        }
        const auto folder = std::filesystem::path(path).parent_path();
        for (const auto* image : {&animation->atlas, &animation->normal, &animation->emissive}) {
            if (!image->empty()) {
                paths.insert((folder / *image).generic_string());
            }
        }
    }
    return paths;
}

std::size_t Engine::releaseUnusedAssets() {
    const auto evicted = assets_->releaseUnused(referencedAssets());
    if (evicted > 0) {
        log(LogLevel::Debug, "Evicted " + std::to_string(evicted) + " cached asset(s); " +
                                 std::to_string(assets_->cachedBytes() >> 20) + " MiB remain cached");
    }
    return evicted;
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
        script_->call(*lua_, "file_changed", relative);
    }
}

void Engine::writeApi(const std::filesystem::path& stubs, const std::filesystem::path& markdown) const {
    docs_->writeStubs(stubs);
    docs_->writeMarkdown(markdown);
}
} // namespace fe
