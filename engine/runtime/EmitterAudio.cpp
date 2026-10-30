#include "engine/runtime/EmitterAudio.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/runtime/Engine.h"
#include "engine/services/Audio.h"
#include "engine/world/World.h"
#include <set>

namespace fe {
void EmitterAudio::update(Audio& audio, const std::vector<WorldDraw>& drawList,
                          const std::filesystem::path& gameRoot) {
    if (!audio.available()) {
        return;
    }
    std::set<int> heard;
    std::set<const World*> visited;
    std::map<std::string, Listener> listeners;
    for (const auto& draw : drawList) {
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
                    emitter.voice = audio.playSound(resolveInside(gameRoot, emitter.sound), options);
                } catch (const std::exception& error) {
                    log(LogLevel::Error,
                        "Positional sound '" + emitter.sound + "' disabled: " + error.what());
                    emitter.sound.clear();
                }
            } else if (emitter.voice >= 0) {
                audio.setSound(emitter.voice, {.volume = gain, .pan = pan});
            }
            if (emitter.voice >= 0) {
                heard.insert(emitter.voice);
                voices_[emitter.voice] = {world, entity};
            }
        }
    }
    for (auto it = voices_.begin(); it != voices_.end();) {
        if (heard.contains(it->first)) {
            ++it;
            continue;
        }
        audio.stopSound(it->first, 0.3);
        if (const auto world = it->second.world.lock(); world && world->valid(it->second.entity)) {
            if (auto* emitter = world->registry().try_get<AudioEmitter>(it->second.entity);
                emitter && emitter->voice == it->first) {
                emitter->voice = -1;
            }
        }
        it = voices_.erase(it);
    }
    audio.setListeners(std::move(listeners));
}
} // namespace fe
