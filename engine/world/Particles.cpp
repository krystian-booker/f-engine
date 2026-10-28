#include "engine/world/Particles.h"
#include "engine/world/World.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace fe {
void emitParticles(entt::registry& registry,const ParticleBurst& burst,std::uint32_t seed) {
    if (burst.sprite.empty()) return;
    if (burst.count<1 || burst.count>4096 || !std::isfinite(burst.lifetime) || burst.lifetime<=0 || burst.lifetime>60 || burst.size<=0 || burst.speed<0)
        throw std::runtime_error("Invalid particle burst");
    std::mt19937 random(seed);
    auto unit=[&] {return float(double(random())/4294967296.0);};
    for (int i=0;i<burst.count;++i) {
        const auto e=registry.create();
        registry.emplace<Transform>(e,burst.position,burst.position);
        auto& sprite=registry.emplace<SpriteRenderer>(e,burst.sprite);sprite.scale=burst.size;sprite.tint=burst.color;sprite.blend=true;sprite.unlit=true;sprite.castShadow=false;
        glm::vec3 direction(unit()*2-1,unit()*1.5f-.25f,unit()*2-1);
        if (glm::length(direction)<.001f) direction={0,1,0};
        registry.emplace<Particle>(e,glm::normalize(direction)*burst.speed,burst.color,0.0,burst.lifetime);
    }
}
void updateParticles(entt::registry& registry,double dt) {
    if (!std::isfinite(dt) || dt<=0) return;
    std::vector<entt::entity> expired;
    for (auto e:registry.view<Particle,Transform,SpriteRenderer>()) {
        auto& particle=registry.get<Particle>(e);auto& transform=registry.get<Transform>(e);auto& sprite=registry.get<SpriteRenderer>(e);
        particle.age+=dt;
        if (particle.age>=particle.lifetime) {expired.push_back(e);continue;}
        transform.position+=particle.velocity*static_cast<float>(dt);particle.velocity*=std::exp(-static_cast<float>(dt)*1.8f);
        sprite.tint=particle.color;sprite.tint.a*=static_cast<float>(1-particle.age/particle.lifetime);
    }
    for (auto e:expired) registry.destroy(e);
}
}
