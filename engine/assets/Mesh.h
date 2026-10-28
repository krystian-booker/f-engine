#pragma once
#include "engine/assets/Image.h"
#include "engine/core/Json.h"
#include <glm/glm.hpp>
#include <memory>

namespace fe {
struct MeshVertex { glm::vec3 position{0}, normal{0,1,0}; glm::vec2 uv{0}; glm::vec4 color{1}; };
struct Material {
    glm::vec4 color{1};
    std::shared_ptr<Image> albedo, normal;
    bool transparent = false, doubleSided = false;
    float alphaCutoff = 0.5f;
};
struct MeshPart { std::vector<MeshVertex> vertices; std::vector<std::uint32_t> indices; Material material; };
struct MeshMarker {
    std::string name;glm::vec3 position{0},min{0},max{0};bool hasBounds=false;Json properties=Json::object();
};
struct Mesh {
    std::vector<MeshPart> parts;
    std::vector<MeshMarker> markers;
    glm::vec3 min{0}, max{0};
    static Mesh load(const std::filesystem::path& path);
};
}
