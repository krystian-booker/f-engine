#include "engine/core/AssetCache.h"
#include "engine/core/Pack.h"
#include "engine/core/Paths.h"
#include "engine/render/CustomShaders.h"
#include "engine/render/GpuAssets.h"
#include "engine/render/SpriteBatch.h"
#include <doctest/doctest.h>
#include <fstream>

using namespace fe;

TEST_CASE("compiled game shaders are read from the game's pak") {
    const auto root =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) / "fengine-shader-pak";
    std::filesystem::remove_all(root);
    const std::string binary = "FSH\x0b" + std::string(28, 'x');
    std::filesystem::create_directories(root / "source" / "shaders" / "metal");
    std::ofstream(root / "source" / "shaders" / "metal" / "fs_sepia.bin", std::ios::binary) << binary;
    std::ofstream(root / "source" / "shaders" / "metal" / "fs_short.bin", std::ios::binary) << "FSH";
    REQUIRE(packFolder(root / "source", root / "game.pak") == 2);
    const auto game = root / "game";
    CHECK_THROWS(static_cast<void>(readShaderBinary(game / "shaders" / "metal" / "fs_sepia.bin")));
    mountPack(root / "game.pak", false);
    CHECK(readShaderBinary(game / "shaders" / "metal" / "fs_sepia.bin") == binary);
    CHECK_THROWS(static_cast<void>(readShaderBinary(game / "shaders" / "metal" / "fs_short.bin")));
    CHECK_THROWS(static_cast<void>(readShaderBinary(game / "shaders" / "metal" / "fs_missing.bin")));
    unmountPacks();
    std::filesystem::remove_all(root);
}

TEST_CASE("game shaders pair only with the vertex shader whose outputs they read") {
    int backends = 0;
    for (const auto& folder : std::filesystem::directory_iterator(FENGINE_ENGINE_SHADERS)) {
        const auto bin = [&](const char* name) {
            return readShaderBinary(folder.path() / (std::string(name) + ".bin"));
        };
        ++backends;
        CHECK(shaderInterfacesMatch(bin("vs_world"), bin("fs_world")));
        CHECK(shaderInterfacesMatch(bin("vs_world_instanced"), bin("fs_world")));
        CHECK(shaderInterfacesMatch(bin("vs_world_instanced"), bin("fs_water")));
        CHECK(shaderInterfacesMatch(bin("vs_shadow_instanced"), bin("fs_shadow")));
        CHECK(shaderInterfacesMatch(bin("vs_sprite"), bin("fs_sprite")));
        CHECK_FALSE(shaderInterfacesMatch(bin("vs_world"), bin("fs_sprite")));
        CHECK_FALSE(shaderInterfacesMatch(bin("vs_sprite"), bin("fs_world")));
        CHECK_FALSE(shaderInterfacesMatch(bin("vs_screen"), bin("fs_world")));
        CHECK_FALSE(shaderInterfacesMatch(bin("fs_world"), bin("fs_world")));
        CHECK_THROWS_WITH(
            static_cast<void>(loadProgram(folder.path() / "vs_world.bin", folder.path() / "fs_sprite.bin")),
            doctest::Contains("do not match"));
    }
    CHECK(backends > 0);
}

TEST_CASE("game shader names map to stable slots with one program per engine vertex shader") {
    CustomShaders shaders(FENGINE_TEST_GAME, FENGINE_ENGINE_SHADERS);
    const int glint = shaders.slot("glint");
    CHECK(shaders.slot("hologram") == glint + 1);
    CHECK(shaders.slot("glint") == glint);
    shaders.reload(FENGINE_ENGINE_SHADERS);
    CHECK(shaders.slot("glint") == glint);
    // Without a renderer there is no shader backend; naming a shader must still work.
    CHECK(shaders.slot("glint_shadow") == glint + 2);
    // The variants are looked up through the colour shader's slot, not slots of their own.
    CHECK(shaders.slot("glint_distort") == glint + 3);
    CHECK_FALSE(bgfx::isValid(shaders.distortProgram(glint, ShaderVertex::Sprite)));
    CHECK_FALSE(bgfx::isValid(shaders.shadowProgram(glint, ShaderVertex::Sprite)));
    CHECK(shaders.slot("glint") == glint);
    CHECK(std::string(shaderVertexName(ShaderVertex::World)) == "vs_world");
    CHECK(std::string(shaderVertexName(ShaderVertex::WorldInstanced)) == "vs_world_instanced");
    CHECK(std::string(shaderVertexName(ShaderVertex::Sprite)) == "vs_sprite");
    CHECK(std::string(shaderVertexName(ShaderVertex::Screen)) == "vs_screen");
}

namespace {
// bgfx with its no-op renderer: GPU objects are created and destroyed without a device.
struct NoopGpu {
    NoopGpu() {
        bgfx::Init init;
        init.type = bgfx::RendererType::Noop;
        ok = bgfx::init(init);
    }
    ~NoopGpu() {
        if (ok) {
            bgfx::shutdown();
        }
    }
    bool ok = false;
};
} // namespace

TEST_CASE("GPU assets look at files only after a file change, and bake chunks per square") {
    NoopGpu gpu;
    REQUIRE(gpu.ok);
    const auto root = std::filesystem::temp_directory_path() / "fengine-gpu-assets";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "strip.gltf")
        << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
      "nodes": [{"mesh": 0}], "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "buffers": [{"byteLength": 72, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AABAQQAAAAAAAAAAAABQQQAAAAAAAAAAAABAQQAAAAAAAIA/"}],
      "bufferViews": [{"buffer": 0, "byteLength": 72}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 6, "type": "VEC3",
                     "min": [0, 0, 0], "max": [13, 0, 1]}]})";
    {
        AssetCache cache(root, FENGINE_LUALIB);
        GpuAssets assets(cache);
        const auto* whole = assets.model("strip.gltf", false);
        const auto* baked = assets.model("strip.gltf", true);
        REQUIRE(whole);
        REQUIRE(baked);
        CHECK(whole->parts.size() == 1);
        REQUIRE(baked->parts.size() == 2); // one per 8x8 chunk
        CHECK(baked->parts[0].max.x == doctest::Approx(1));
        CHECK(baked->parts[1].min.x == doctest::Approx(12));
        assets.sync();
        const auto first = assets.revision();
        assets.sync();
        CHECK(assets.revision() == first); // nothing changed: no work, no file looked at
        const auto changes = cache.fileChanges();
        CHECK_FALSE(cache.reload(root / "colors.cube")); // not a cached asset, still a file change
        CHECK(cache.fileChanges() == changes + 1);
        assets.sync();
        const auto second = assets.revision();
        CHECK(second != first);
        assets.sync();
        CHECK(assets.revision() == second);
        GpuAssets other(cache);
        CHECK(other.revision() != second); // revisions never repeat across caches
        // A game shader that cannot be loaded (here: no shader backend for the
        // no-op renderer) falls back to the engine's for every vertex shader.
        CustomShaders shaders(root, FENGINE_ENGINE_SHADERS);
        CHECK_FALSE(bgfx::isValid(shaders.program("glint", ShaderVertex::World)));
        CHECK_FALSE(bgfx::isValid(shaders.program("glint", ShaderVertex::WorldInstanced)));
        // No fs_glint_shadow.sc or fs_glint_distort.sc: the variants are simply absent.
        CHECK_FALSE(bgfx::isValid(shaders.shadowProgram(shaders.slot("glint"), ShaderVertex::World)));
        CHECK_FALSE(bgfx::isValid(shaders.distortProgram(shaders.slot("glint"), ShaderVertex::Sprite)));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("sprite batches find a quad's material by hash and keep game shaders apart") {
    SpriteBatch batch;
    const auto material = [](std::uint16_t atlas, std::uint16_t program) {
        SpriteBatch::Material m;
        m.atlas.idx = atlas;
        m.palette.idx = 0;
        m.texel = glm::vec2(1.0f / 64);
        m.program.idx = program;
        return m;
    };
    const std::array<SpriteVertex, 4> quad{};
    for (int round = 0; round < 3; ++round) {
        for (std::uint16_t atlas = 1; atlas <= 200; ++atlas) {
            batch.add(material(atlas, bgfx::kInvalidHandle), quad, {});
        }
    }
    batch.add(material(7, 3), quad, {});
    const auto& quads = batch.quads();
    REQUIRE(quads.size() == 601);
    CHECK(quads[6].material == 6);
    CHECK(quads[206].material == 6);
    CHECK(quads[599].material == 199);
    CHECK(quads[600].material == 200); // same textures, its own shader
    batch.clear();
    batch.add(material(9, bgfx::kInvalidHandle), quad, {});
    CHECK(batch.quads().front().material == 0);
}
