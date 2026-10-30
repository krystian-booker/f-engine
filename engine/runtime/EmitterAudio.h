#pragma once
#include <entt/entt.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace fe {
class Audio;
class World;
struct WorldDraw;

// Looping positional sounds of entities with an AudioEmitter: one voice per
// emitter, heard from its world's camera, started, re-placed and stopped as
// the worlds are drawn (see Engine::tick step 6).
class EmitterAudio {
public:
    // Plays for the worlds in `drawList` (last frame's world:draw calls),
    // each heard from its own camera. Voices of entities that were destroyed,
    // lost their emitter, or whose world was unloaded or not drawn are
    // stopped. Each drawn world's camera is also published as the listener
    // for positional one-shots.
    void update(Audio& audio, const std::vector<WorldDraw>& drawList, const std::filesystem::path& gameRoot);

private:
    struct Voice {
        std::weak_ptr<World> world;
        entt::entity entity = entt::null;
    };
    std::map<int, Voice> voices_;
};
} // namespace fe
