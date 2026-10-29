#include "engine/world/Particles.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <cmath>

namespace fe {
ParticleSystem::ParticleSystem(entt::registry& registry, AssetCache& assets, Random& random)
    : registry_(registry), assets_(assets), random_(random) {
}

std::uint16_t ParticleSystem::sprite(const std::string& path) {
    const auto found = std::find(sprites_.begin(), sprites_.end(), path);
    if (found != sprites_.end()) {
        return static_cast<std::uint16_t>(found - sprites_.begin());
    }
    sprites_.push_back(path);
    return static_cast<std::uint16_t>(sprites_.size() - 1);
}

int ParticleSystem::spawn(const EmitterSettings& s, glm::vec3 origin, entt::entity emitter, int count,
                          bool weather) {
    auto jitter = [&](float amount) {
        return static_cast<float>(random_.unit() * 2 - 1) * amount;
    };
    const bool attached = emitter != entt::null && registry_.all_of<Emitter>(emitter);
    const auto spriteIndex = sprite(s.sprite);
    int spawned = 0;
    for (; spawned < count && particles_.size() < MaxParticles; ++spawned) {
        Particle particle;
        particle.offset = glm::vec3(jitter(s.area.x), jitter(s.area.y), jitter(s.area.z));
        particle.position = origin + particle.offset;
        particle.velocity = s.velocity + glm::vec3(jitter(s.velocityJitter.x), jitter(s.velocityJitter.y),
                                                   jitter(s.velocityJitter.z));
        particle.lifetime = std::max(0.05f, s.lifetime + jitter(s.lifetimeJitter));
        particle.size = s.size;
        particle.sizeEnd = s.sizeEnd;
        particle.drag = s.drag;
        particle.stretch = s.stretch;
        particle.gravity = s.gravity;
        particle.color = s.color;
        particle.colorEnd = s.colorEnd;
        particle.sprite = spriteIndex;
        particle.blend = s.blend;
        particle.unlit = s.unlit;
        particle.distort = s.distort;
        particle.local = attached && !s.worldSpace;
        particle.weather = weather;
        particle.emitter = emitter;
        particles_.push_back(particle);
    }
    if (attached && spawned > 0) {
        live_[emitter] += spawned;
    }
    if (weather) {
        weatherLive_ += spawned;
    }
    return spawned;
}

int ParticleSystem::live(entt::entity emitter) const {
    const auto found = live_.find(emitter);
    return found == live_.end() ? 0 : found->second;
}

void ParticleSystem::burst(glm::vec3 position, const EmitterSettings& settings, int count) {
    spawn(settings, position, entt::null, std::clamp(count, 0, std::min(4096, settings.maxParticles)), false);
}

void ParticleSystem::update(double dt, const WeatherSettings& weather, glm::vec3 cameraTarget) {
    updateEmitters(dt);
    updateWeather(dt, weather, cameraTarget);
    simulate(dt);
}

void ParticleSystem::updateEmitters(double dt) {
    for (const auto entity : registry_.view<Transform, Emitter>()) {
        auto& emitter = registry_.get<Emitter>(entity);
        const auto origin = registry_.get<Transform>(entity).position;
        const auto room = [&] {
            return std::max(0, emitter.settings.maxParticles - live(entity));
        };
        if (!emitter.burstDone) {
            spawn(emitter.settings, origin, entity, std::min(emitter.settings.burst, room()), false);
            emitter.burstDone = true;
        }
        if (!emitter.active) {
            continue;
        }
        emitter.accumulator += dt * emitter.settings.rate;
        const int due = static_cast<int>(emitter.accumulator);
        emitter.accumulator -= due;
        spawn(emitter.settings, origin, entity, std::min(due, room()), false);
    }
}

void ParticleSystem::updateWeather(double dt, const WeatherSettings& weather, glm::vec3 cameraTarget) {
    if (weather.kind == "none" || weather.intensity <= 0) {
        weatherAccumulator_ = 0;
        return;
    }
    const bool changed = !weatherReady_ || weather.kind != appliedWeather_.kind ||
                         weather.wind != appliedWeather_.wind || weather.color != appliedWeather_.color ||
                         weather.sprite != appliedWeather_.sprite;
    if (changed) {
        appliedWeather_ = weather;
        weatherReady_ = true;
        weatherValid_ = false;
        if (const auto* base = preset(weather.kind)) {
            weatherSettings_ = base->settings;
            weatherOrigin_ = base->origin;
            weatherSettings_.velocity += weather.wind;
            if (!weather.sprite.empty()) {
                weatherSettings_.sprite = weather.sprite;
            }
            if (weather.color != glm::vec4(1)) {
                // A map colour replaces the preset's, keeping the preset's fade.
                const auto& s = base->settings;
                const float fade = s.color.a > 0 ? s.colorEnd.a / s.color.a : 1.0f;
                weatherSettings_.color = weather.color;
                weatherSettings_.colorEnd = glm::vec4(glm::vec3(weather.color), weather.color.a * fade);
            }
            weatherValid_ = true;
        }
    }
    if (!weatherValid_) {
        return;
    }
    weatherAccumulator_ += dt * weatherSettings_.rate * weather.intensity;
    const int due = static_cast<int>(weatherAccumulator_);
    weatherAccumulator_ -= due;
    spawn(weatherSettings_, cameraTarget + weatherOrigin_, entt::null,
          std::min(due, std::max(0, weatherSettings_.maxParticles - weatherLive_)), true);
}

void ParticleSystem::simulate(double dt) {
    const auto delta = static_cast<float>(dt);
    for (auto& p : particles_) {
        p.age += delta;
        p.velocity += p.gravity * delta;
        p.velocity *= std::exp(-p.drag * delta);
        if (!p.local) {
            p.position += p.velocity * delta;
            continue;
        }
        p.offset += p.velocity * delta;
        const auto* anchor = registry_.valid(p.emitter) ? registry_.try_get<Transform>(p.emitter) : nullptr;
        if (anchor) {
            p.position = anchor->position + p.offset;
        } else {
            p.local = false;
            p.position += p.velocity * delta;
        }
    }
    std::erase_if(particles_, [&](const Particle& p) {
        if (p.age < p.lifetime) {
            return false;
        }
        if (p.weather) {
            --weatherLive_;
        } else if (const auto found = live_.find(p.emitter); found != live_.end() && --found->second == 0) {
            live_.erase(found);
        }
        return true;
    });
}

const WeatherPreset* ParticleSystem::preset(const std::string& kind) {
    if (!presets_) {
        presets_.emplace();
        loadPresets("@fe/assets/weather.json", true);
        loadPresets("data/weather.json", false);
    }
    const auto found = presets_->find(kind);
    if (found == presets_->end()) {
        if (unknownKinds_.insert(kind).second) {
            log(LogLevel::Warning, "Unknown weather kind '" + kind + "'");
        }
        return nullptr;
    }
    return &found->second;
}

void ParticleSystem::loadPresets(const std::string& path, bool required) {
    try {
        const auto file = assets_.file(path);
        if (!required && !isFile(file)) {
            return;
        }
        const auto presets = readJson(file);
        for (const auto& [kind, value] : presets.items()) {
            WeatherPreset preset{EmitterSettings::fromJson(value)};
            if (value.contains("origin")) {
                preset.origin = jsonVec3(value.at("origin"));
            }
            (*presets_)[kind] = std::move(preset);
        }
    } catch (const std::exception& error) {
        log(LogLevel::Error, "Weather presets " + path + ": " + error.what());
    }
}
} // namespace fe
