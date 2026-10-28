#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>

namespace fe {
struct Particle {
    glm::vec3 velocity{0};
    glm::vec4 color{1};
    double age=0,lifetime=.7;
};
struct ParticleBurst {
    std::string sprite;
    glm::vec3 position{0};
    glm::vec4 color{1.5f,1,.4f,1};
    int count=12;
    float speed=1.5f,size=.25f;
    double lifetime=.7;
};
void emitParticles(entt::registry& registry,const ParticleBurst& burst,std::uint32_t seed=1);
void updateParticles(entt::registry& registry,double dt);
}
