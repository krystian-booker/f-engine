#pragma once
#include "engine/assets/Image.h"
#include "engine/core/Json.h"
#include <array>
#include <cstdint>
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
    glm::vec3 min{0}, max{0}; // bounds in node space (a skinned part: model space, at rest)
    // A skinned part (skin >= 0) carries, per vertex, up to four joints of
    // its skin (indices into ModelSkin::joints) and their weights, which sum
    // to 1. Its vertices are in the mesh's bind space and are placed by the
    // joints alone: the owning node's transform does not apply (glTF).
    int skin = -1;
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<glm::vec4> weights;
};

// A glTF skin: the nodes that are its joints and, per joint, the matrix that
// takes a vertex from bind space into that joint's space.
struct ModelSkin {
    std::vector<int> joints;
    std::vector<glm::mat4> inverseBind;
};

// A node's local transform, the unit animations are sampled and blended in.
struct NodePose {
    glm::vec3 translation{0};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1};
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

// A glTF scene: node hierarchy, TRS animations of its nodes, skins (skeletal
// characters and creatures) and markers. Images a .gltf references by URI
// must lie in the model's folder (or below it); a node matrix with a negative
// determinant imports as a mirrored scale. Morph targets are not read.
struct Model {
    std::vector<ModelNode> nodes;
    std::vector<int> order; // parents before children
    std::vector<ModelPart> parts;
    std::vector<ModelSkin> skins;
    std::vector<ModelAnimation> animations; // each one's channels sorted by node
    std::vector<ModelMarker> markers;
    glm::vec3 min{0}, max{0};
    // Baked at load: each node's local matrix at rest and the rest pose (the
    // world matrix of every node), so posing a model without an animation
    // is a copy.
    std::vector<glm::mat4> restLocal, restPose;

    static Model load(const std::filesystem::path& path);
    [[nodiscard]] int animation(const std::string& name) const;
    // World matrices for every node; animation -1 gives the rest pose. Writes
    // into `matrices` only (sized to the nodes), so a frame allocates nothing
    // here once it is warm.
    void pose(int animation, float time, std::vector<glm::mat4>& matrices) const;
    // The same in two steps, for blending between animations: every node's
    // local transform under an animation (-1: at rest), then world matrices
    // from local transforms. blendPoses moves `into` towards `other` by
    // `weight` (0 keeps it, 1 replaces it), the short way round for rotations.
    void sample(int animation, float time, std::vector<NodePose>& locals) const;
    void compose(const std::vector<NodePose>& locals, std::vector<glm::mat4>& matrices) const;
    static void blendPoses(std::vector<NodePose>& into, const std::vector<NodePose>& other, float weight);
    // Joint matrices of a skin under a pose (world matrices of every node):
    // matrices[i] takes a bind-space vertex of joint i to model space.
    void jointMatrices(int skin, const std::vector<glm::mat4>& pose, std::vector<glm::mat4>& matrices) const;
    [[nodiscard]] bool skinned() const { return !skins.empty(); }
    // Fills restLocal and restPose from the nodes (load() does this).
    void bakeRestPose();
};

// A skinned part's vertices under joint matrices (Model::jointMatrices): each
// position, normal and tangent is the weighted sum of its joints' transforms.
// `out` is resized to the part's vertex count; uv and colour are copied.
void skinVertices(const ModelPart& part, const std::vector<glm::mat4>& joints, std::vector<MeshVertex>& out);

// True when two materials draw identically: same factors, images (the same
// decoded image, not merely equal pixels), samplers, flags and shader. Parts
// with such materials can share one draw call.
[[nodiscard]] bool sameMaterial(const Material& a, const Material& b);

// Static geometry ready to draw as a map: a model's visible parts at rest, in
// model space, merged by material.
struct BakedMesh {
    const Material* material = nullptr; // of the first part merged into it
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Every visible part is transformed by its node (a skinned part by its
// joints at rest; a mirrored node's triangles are turned to face outwards
// again), then parts whose materials are the same are joined into one mesh,
// in the order their materials first appear. A map built from many small
// pieces (one node per wall, floor tile or prop) becomes one mesh per
// material rather than one per piece. Transparent and water materials are
// not joined: their parts are sorted by distance, each on its own.
[[nodiscard]] std::vector<BakedMesh> bakeGeometry(const Model& model);
} // namespace fe
