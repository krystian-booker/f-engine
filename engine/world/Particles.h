#pragma once
#include "engine/core/AssetCache.h"
#include "engine/core/Random.h"
#include "engine/world/Components.h"
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace fe {
// `position` is always in world space. Particles of a local-space emitter
// (EmitterSettings::worldSpace = false) are simulated as `offset` from their
// `emitter` entity and `position` is rewritten as emitter position + offset
// every tick, so they travel with it; when the emitter disappears they carry
// on in world space.
struct Particle {
    glm::vec3 position{0}, velocity{0};
    float age = 0, lifetime = 1, size = 0.2f, sizeEnd = 0.2f, drag = 0, stretch = 0;
    glm::vec3 gravity{0};
    glm::vec4 color{1}, colorEnd{1};
    std::uint16_t sprite = 0;
    BlendMode blend = BlendMode::Alpha;
    bool unlit = true;
    float distort = 0;    // >0: bends the scene behind it instead of drawing (EmitterSettings::distort)
    bool local = false;   // offset-from-emitter simulation (see above)
    bool weather = false; // counts toward the weather cap
    entt::entity emitter = entt::null;
    glm::vec3 offset{0};
};

// Weather particle settings for one `environment.weather.kind`. `origin` is
// an offset from the camera target.
struct WeatherPreset {
    EmitterSettings settings;
    glm::vec3 origin{0};
};

// Particle simulation for emitters, bursts and weather. Weather presets are
// data: @fe/assets/weather.json, with kinds added or replaced by the game's
// data/weather.json; both are read when a world first shows weather.
class ParticleSystem {
public:
    static constexpr std::size_t MaxParticles = 16384;
    ParticleSystem(entt::registry& registry, AssetCache& assets, Random& random);

    // One-shot particles at a position (at most settings.maxParticles).
    void burst(glm::vec3 position, const EmitterSettings& settings, int count);
    void update(double dt, const WeatherSettings& weather, glm::vec3 cameraTarget);
    [[nodiscard]] const std::vector<Particle>& particles() const { return particles_; }
    [[nodiscard]] const std::vector<std::string>& sprites() const { return sprites_; }
    // Particles alive from an emitter entity (capped by its settings.maxParticles).
    [[nodiscard]] int live(entt::entity emitter) const;

private:
    int spawn(const EmitterSettings& settings, glm::vec3 origin, entt::entity emitter, int count,
              bool weather);
    std::uint16_t sprite(const std::string& path);
    void updateEmitters(double dt);
    void updateWeather(double dt, const WeatherSettings& weather, glm::vec3 cameraTarget);
    void simulate(double dt);
    const WeatherPreset* preset(const std::string& kind);
    void loadPresets(const std::string& path, bool required);

    entt::registry& registry_;
    AssetCache& assets_;
    Random& random_;
    std::vector<Particle> particles_;
    std::vector<std::string> sprites_;
    std::unordered_map<entt::entity, int> live_; // per emitter entity, entries > 0
    std::optional<std::map<std::string, WeatherPreset>> presets_;
    std::set<std::string> unknownKinds_;
    WeatherSettings appliedWeather_; // weather the settings below were built for
    EmitterSettings weatherSettings_;
    glm::vec3 weatherOrigin_{0};
    bool weatherReady_ = false, weatherValid_ = false;
    double weatherAccumulator_ = 0;
    int weatherLive_ = 0;
};
} // namespace fe
