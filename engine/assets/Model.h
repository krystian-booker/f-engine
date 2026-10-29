#pragma once
#include "engine/assets/Image.h"
#include "engine/core/Json.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
struct MeshVertex {
    glm::vec3 position{0}, normal{0, 1, 0};
    glm::vec4 tangent{1, 0, 0, 1}; // xyz along +u, w = handedness of the bitangent (+v)
    glm::vec2 uv{0};
    glm::vec4 color{1};
};

// Per-vertex tangents for normal mapping: each triangle's tangent and
// bitangent from its UV gradients, weighted by triangle area and summed per
// vertex (so shared vertices are smoothed), then made orthogonal to the
// vertex normal. Vertices without usable UVs get any tangent perpendicular to
// their normal.
void generateTangents(std::vector<MeshVertex>& vertices, const std::vector<std::uint32_t>& indices);

enum class MaterialKind { Lit, Unlit, Water };

// glTF sampler state of one texture reference.
struct TextureSampler {
    enum class Wrap { Repeat, Clamp, Mirror };
    enum class Mips { None, Nearest, Linear };
    Wrap wrapU = Wrap::Repeat, wrapV = Wrap::Repeat;
    bool nearestMag = false, nearestMin = false;
    Mips mips = Mips::Linear;
};

// The file an image came from (empty when it is embedded in a .glb), so a
// cooked .ktx next to it can replace it on the GPU.
struct ModelImage {
    std::shared_ptr<Image> image;
    std::filesystem::path file;
    explicit operator bool() const { return image != nullptr; }
};

// Names of game shaders (shaders/fs_<name>.sc): a-z, 0-9, _ and -, at most 64 characters.
[[nodiscard]] bool isShaderName(std::string_view name);

struct Material {
    glm::vec4 color{1};
    glm::vec3 emissive{0}; // emissiveFactor times KHR_materials_emissive_strength
    ModelImage albedo, normal, emission;
    TextureSampler albedoSampler, normalSampler, emissionSampler;
    float normalScale = 1;
    bool transparent = false, doubleSided = false;
    float alphaCutoff = 0.5f;
    MaterialKind kind = MaterialKind::Lit;
    // A game fragment shader in place of the engine's (glTF material extras
    // or node custom property "shader"); empty = the engine's.
    std::string shader;
};

// Geometry in the owning node's local space.
struct ModelPart {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    Material material;
    int node = 0;
    bool hidden = false;      // collision or helper geometry, or a node outside the shown scene
    glm::vec3 min{0}, max{0}; // bounds in node space
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
