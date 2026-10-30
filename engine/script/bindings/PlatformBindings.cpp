#include "engine/runtime/Engine.h"
#include "engine/script/bindings/Convert.h"
#include "engine/script/bindings/WorldTypes.h"

namespace fe {
void bindPlatform(Engine& engine) {
    auto& host = engine.lua();
    auto& platform = engine.platform();
    ModuleBinder(host, engine.docs(), "fe.platform",
                 "Storefront services: unlockable milestones and stats. A port or game module can supply a "
                 "store backend (Steam, a console SDK); the default keeps them in the user-data folder "
                 "(platform.json).")
        .fn(
            "name", [&platform] { return platform.name(); }, "fun(): string",
            "The backend: 'local' (user-data folder) or the store a port plugged in.")
        .fn(
            "unlock", [&platform](const std::string& id) { platform.unlock(id); }, "fun(id: string)",
            "Unlock a milestone by its store id; unlocking twice does nothing.")
        .fn(
            "unlocked", [&platform](const std::string& id) { return platform.unlocked(id); },
            "fun(id: string): boolean", "Whether a milestone is unlocked.")
        .fn(
            "set_stat", [&platform](const std::string& name, double value) { platform.setStat(name, value); },
            "fun(name: string, value: number)", "Set a stat by its store name.")
        .fn(
            "stat",
            [&platform](const std::string& name) -> sol::optional<double> {
                if (const auto value = platform.stat(name)) {
                    return *value;
                }
                return sol::nullopt;
            },
            "fun(name: string): number?", "A stat's value, nil when never set.");
    ModuleBinder(host, engine.docs(), "fe.world", "")
        .fn(
            "load_async",
            [&engine](const std::string& name, const std::string& mapId) {
                return engine.loadWorldAsync(name, mapId);
            },
            "fun(name: string, map: string): integer",
            "Start loading maps/<map>/map.json in the background: the map file is parsed and the models, "
            "images and sprites it names are decoded on worker threads. Returns a ticket for load_progress "
            "and finish_load.")
        .fn(
            "load_progress", [&engine](int ticket) { return engine.loadProgress(ticket); },
            "fun(ticket: integer): number",
            "Background load progress 0..1; 1 when finish_load won't wait (or the load failed).")
        .fn(
            "finish_load", [&engine](int ticket) { return WorldRef{engine.finishLoad(ticket), &engine}; },
            "fun(ticket: integer): World",
            "Create the world from a background load, replacing a world of that name. Before progress "
            "reaches 1 it waits for the map file and leaves undecoded assets to load on first use. Raises "
            "the "
            "load's error (a missing or invalid map). The ticket is used up.")
        .fn(
            "cancel_load", [&engine](int ticket) { engine.cancelLoad(ticket); }, "fun(ticket: integer)",
            "Drop a background load that will not be finished (a cancelled warp): the ticket stops "
            "advancing and becomes unknown; a worker still parsing the map finishes in the background and "
            "its result is discarded, and assets already decoded stay in the cache like any other.");
}
} // namespace fe
