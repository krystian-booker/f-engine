#pragma once
#include "engine/assets/Image.h"
#include "engine/core/Json.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fe {
struct MeshVertex {
    glm::vec3 position{0}, normal{0, 1, 0};
    glm::vec2 uv{0};
    glm::vec4 color{1};
};

enum class MaterialKind { Lit, Unlit, Water };

struct Material {
    glm::vec4 color{1};
    glm::vec3 emissive{0};
    std::shared_ptr<Image> albedo, normal;
    bool transparent = false, doubleSided = false;
    float alphaCutoff = 0.5f;
    MaterialKind kind = MaterialKind::Lit;
};

// Geometry in the owning node's local space.
struct ModelPart {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    Material material;
    int node = 0;
    bool hidden = false; // collision or helper geometry
    glm::vec3 min{0}, max{0};
};

struct ModelNode {
    std::string name;
    int parent = -1;
    glm::vec3 translation{0};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1};
    Json properties = Json::object(); // glTF extras ("fengine" object if present)
};

// Named points and volumes authored in Blender: spawn points, collision
// boxes, lights. The engine reports them; scripts and tools decide meaning.
struct ModelMarker {
    std::string name;
    glm::vec3 position{0}, min{0}, max{0};
    bool hasBounds = false;
    Json properties = Json::object();
};

struct AnimationChannel {
    enum class Target { Translation, Rotation, Scale };
    int node = 0;
    Target target = Target::Translation;
    bool step = false;
    std::vector<float> times;
    std::vector<glm::vec4> values;
};

struct ModelAnimation {
    std::string name;
    float duration = 0;
    std::vector<AnimationChannel> channels;
};

// A static glTF scene: node hierarchy, rigid TRS animations, markers.
// Skinned meshes are rejected; characters are sprites.
struct Model {
    std::vector<ModelNode> nodes;
    std::vector<int> order; // parents before children
    std::vector<ModelPart> parts;
    std::vector<ModelAnimation> animations;
    std::vector<ModelMarker> markers;
    glm::vec3 min{0}, max{0};

    static Model load(const std::filesystem::path& path);
    [[nodiscard]] int animation(const std::string& name) const;
    // World matrices for every node; animation -1 gives the rest pose.
    void pose(int animation, float time, std::vector<glm::mat4>& matrices) const;
};
} // namespace fe
