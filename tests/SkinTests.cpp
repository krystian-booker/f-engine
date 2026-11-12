#include "engine/assets/Model.h"
#include "engine/runtime/Engine.h"
#include "engine/world/Components.h"
#include "engine/world/World.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <string>

using namespace fe;

namespace {
std::filesystem::path drake() {
    return std::filesystem::path(FENGINE_TEST_GAME) / "models" / "drake.glb";
}

float maxZ(const std::vector<MeshVertex>& vertices) {
    float z = -1e9f;
    for (const auto& v : vertices) {
        z = std::max(z, v.position.z);
    }
    return z;
}
} // namespace

TEST_CASE("a skinned glTF loads its skin, weights and animations") {
    const auto model = Model::load(drake());
    REQUIRE(model.skins.size() == 1);
    CHECK(model.skinned());
    CHECK(model.skins[0].joints.size() == 14);
    CHECK(model.skins[0].inverseBind.size() == 14);
    REQUIRE(model.parts.size() == 1);
    const auto& part = model.parts[0];
    CHECK(part.skin == 0);
    REQUIRE(part.joints.size() == part.vertices.size());
    REQUIRE(part.weights.size() == part.vertices.size());
    bool blended = false;
    for (std::size_t v = 0; v < part.weights.size(); ++v) {
        const auto& w = part.weights[v];
        CHECK(w.x + w.y + w.z + w.w == doctest::Approx(1.0f));
        for (const auto joint : part.joints[v]) {
            CHECK(joint < 14);
        }
        blended = blended || (w.x > 0.01f && w.y > 0.01f);
    }
    CHECK(blended); // the tail's vertices follow two joints each
    for (const char* name : {"idle", "attack", "cast", "hit", "die"}) {
        CHECK(model.animation(name) >= 0);
    }
    // The bounds are those of the skinned rest pose: the creature stands on the ground.
    CHECK(model.min.y == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(model.max.y > 1.5f);
}

TEST_CASE("skinning at rest reproduces the bind pose and an animation moves the joints' vertices") {
    const auto model = Model::load(drake());
    const auto& part = model.parts[0];
    std::vector<glm::mat4> pose, joints;
    std::vector<MeshVertex> skinned;
    model.pose(-1, 0, pose);
    model.jointMatrices(0, pose, joints);
    skinVertices(part, joints, skinned);
    REQUIRE(skinned.size() == part.vertices.size());
    for (std::size_t v = 0; v < skinned.size(); ++v) {
        CHECK(glm::distance(skinned[v].position, part.vertices[v].position) < 1e-4f);
        CHECK(glm::distance(skinned[v].normal, part.vertices[v].normal) < 1e-4f);
    }
    const float restReach = maxZ(skinned);
    // The lunge of "attack" carries the head forwards.
    model.pose(model.animation("attack"), 0.4f, pose);
    model.jointMatrices(0, pose, joints);
    skinVertices(part, joints, skinned);
    CHECK(maxZ(skinned) > restReach + 0.4f);
    for (const auto& v : skinned) {
        CHECK(glm::length(v.normal) == doctest::Approx(1.0f).epsilon(0.001));
    }
}

TEST_CASE("sampled poses blend between animations and compose like pose()") {
    const auto model = Model::load(drake());
    const int attack = model.animation("attack");
    std::vector<NodePose> a, b;
    std::vector<glm::mat4> direct, composed;
    model.sample(attack, 0.4f, a);
    model.compose(a, composed);
    model.pose(attack, 0.4f, direct);
    REQUIRE(composed.size() == direct.size());
    for (std::size_t n = 0; n < direct.size(); ++n) {
        for (int c = 0; c < 4; ++c) {
            CHECK(glm::distance(composed[n][c], direct[n][c]) < 1e-4f);
        }
    }
    model.sample(-1, 0, b);
    const auto rest = b;
    Model::blendPoses(b, a, 0);
    CHECK(glm::distance(b[2].translation, rest[2].translation) < 1e-6f);
    Model::blendPoses(b, a, 1);
    CHECK(glm::distance(b[2].translation, a[2].translation) < 1e-6f);
    auto half = rest;
    Model::blendPoses(half, a, 0.5f);
    CHECK(glm::distance(half[2].translation, (rest[2].translation + a[2].translation) * 0.5f) < 1e-5f);
    CHECK(glm::length(half[3].rotation) == doctest::Approx(1.0f));
}

TEST_CASE("a mesh crossfades into a new animation and reports its end") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = std::filesystem::temp_directory_path() / "fengine-skin-test";
    Engine engine(std::move(options));
    const auto world = engine.loadWorld("skin", "arena");
    const auto entity = world->spawn("drake", {2, 0, 2});
    MeshInstance mesh;
    mesh.model = "models/drake.glb";
    world->registry().emplace<MeshInstance>(entity, mesh);
    world->playMesh(entity, "idle", true);
    for (int i = 0; i < 30; ++i) {
        engine.tick(1.0 / 60.0);
    }
    world->playMesh(entity, "attack", false, 0.2f);
    {
        const auto& m = world->registry().get<MeshInstance>(entity);
        CHECK(m.fromAnimation == "idle");
        CHECK(m.fromTime == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(m.fade == doctest::Approx(0.2f));
        CHECK(m.time == 0.0f);
    }
    bool finished = false;
    for (int i = 0; i < 60 && !finished; ++i) {
        engine.tick(1.0 / 60.0);
        for (const auto& event : world->takeEvents()) {
            finished =
                finished || (event.kind == WorldEvent::Kind::AnimationFinished && event.name == "attack");
        }
    }
    CHECK(finished);
    const auto& m = world->registry().get<MeshInstance>(entity);
    CHECK(m.fade == 0.0f); // the crossfade ended long before the clip
    CHECK(m.fromAnimation.empty());
    CHECK_FALSE(m.playing);
}

TEST_CASE("an accessor wider than its attribute is refused instead of overrunning the reader") {
    // COLOR_0 retyped as MAT4: cgltf accepts it, the loader must not copy 16 floats into 4.
    const auto bytes = [] {
        std::ifstream in(drake(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    auto broken = bytes;
    const auto at = broken.find("\"COLOR_0\":");
    REQUIRE(at != std::string::npos);
    const int accessor = std::stoi(broken.substr(at + 10));
    // Accessors are written in order, each with one "type" key: retype the colour's.
    std::size_t cursor = broken.find("\"accessors\":");
    REQUIRE(cursor != std::string::npos);
    for (int i = 0; i <= accessor; ++i) {
        cursor = broken.find("\"type\":\"", cursor + 1);
        REQUIRE(cursor != std::string::npos);
    }
    REQUIRE(broken.compare(cursor + 8, 4, "VEC4") == 0);
    broken.replace(cursor + 8, 4, "MAT4");
    const auto file = std::filesystem::temp_directory_path() / "fengine-broken-drake.glb";
    {
        std::ofstream out(file, std::ios::binary);
        out << broken;
    }
    CHECK_THROWS(Model::load(file));
    std::filesystem::remove(file);
}
