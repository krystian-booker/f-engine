#include "engine/render/FrameMath.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <doctest/doctest.h>

using namespace fe;

TEST_CASE("bloom mip chain halves the scene and stops before 2 pixels") {
    const auto sizes = bloomMipSizes({1280, 720}, 6);
    REQUIRE(sizes.size() == 6);
    CHECK(sizes[0] == glm::ivec2(640, 360));
    CHECK(sizes[5] == glm::ivec2(20, 11));
    CHECK(bloomMipSizes({1280, 720}, 3).size() == 3);
    const auto small = bloomMipSizes({20, 12}, 8);
    REQUIRE(small.size() == 2); // 10x6, 5x3; 2x1 would be too small
    CHECK(small.back() == glm::ivec2(5, 3));
    CHECK(bloomMipSizes({3, 3}, 6) == std::vector<glm::ivec2>{glm::ivec2(1, 1)});
}

TEST_CASE("bloom normalisation and soft knee") {
    CHECK(bloomNormalisation(1, 0.5f) == doctest::Approx(1));
    CHECK(bloomNormalisation(3, 0.5f) == doctest::Approx(1.75f));
    CHECK(bloomNormalisation(4, 1.0f) == doctest::Approx(4));
    // Well below threshold - knee nothing passes; well above, the excess passes.
    CHECK(bloomPass(0.2f, 1.0f, 0.5f) == doctest::Approx(0));
    CHECK(bloomPass(3.0f, 1.0f, 0.5f) == doctest::Approx(2.0f / 3.0f));
    // Inside the knee the curve is soft but positive and grows with brightness.
    const float a = bloomPass(0.8f, 1.0f, 0.5f), b = bloomPass(1.2f, 1.0f, 0.5f);
    CHECK(a > 0);
    CHECK(b > a);
    // No knee: a hard threshold.
    CHECK(bloomPass(0.99f, 1.0f, 0.0f) == doctest::Approx(0));
    // The settings' knee is a fraction of the threshold; the shader gets it absolute.
    PostSettings post;
    post.bloomThreshold = 0.8f;
    post.bloomKnee = 0.5f;
    CHECK(bloomKnee(post) == doctest::Approx(0.4f));
    CHECK(bloomPass(0.8f, post.bloomThreshold, bloomKnee(post)) == doctest::Approx(0.4f * 0.4f / 1.6f / 0.8f).epsilon(1e-3));
}

TEST_CASE("particles fade out as they near the camera") {
    CHECK(particleNearFade(18, 18) == doctest::Approx(1));
    CHECK(particleNearFade(5.4f, 18) == doctest::Approx(1));
    CHECK(particleNearFade(3.6f, 18) == doctest::Approx(0.5f));
    CHECK(particleNearFade(1.8f, 18) == doctest::Approx(0));
    CHECK(particleNearFade(-2, 18) == doctest::Approx(0));
    CHECK(particleNearFade(1, 0) == doctest::Approx(1));
}

TEST_CASE("depth linearisation matches both depth conventions") {
    for (const bool homogeneous : {false, true}) {
        CAPTURE(homogeneous);
        CHECK(linearDepth(0, 0.1f, 300, homogeneous) == doctest::Approx(0.1f));
        CHECK(linearDepth(1, 0.1f, 300, homogeneous) == doctest::Approx(300).epsilon(1e-3));
    }
    // A point 17 units away, projected with [0, 1] depth.
    const float n = 0.1f, f = 300, z = 17;
    const float device = (f / (f - n)) * (1 - n / z);
    CHECK(linearDepth(device, n, f, false) == doctest::Approx(z).epsilon(1e-3));
}

TEST_CASE("circle of confusion separates near and far fields") {
    FocusSettings focus;
    focus.mode = FocusMode::Depth;
    focus.distance = 10;
    focus.aperture = 2;
    CHECK(circleOfConfusion(10, 0.5f, focus) == doctest::Approx(0));
    CHECK(circleOfConfusion(20, 0.5f, focus) == doctest::Approx(1));           // far, clamped
    CHECK(circleOfConfusion(15, 0.5f, focus) == doctest::Approx(2.0f / 3.0f)); // 2 * 5 / 15
    CHECK(circleOfConfusion(8, 0.5f, focus) == doctest::Approx(-0.5f));        // near is negative
    focus.mode = FocusMode::Tilt;
    focus.line = 0.5f;
    focus.range = 0.2f;
    CHECK(circleOfConfusion(50, 0.6f, focus) == doctest::Approx(0));     // inside the sharp band
    CHECK(circleOfConfusion(50, 0.1f, focus) == doctest::Approx(1));     // above: far
    CHECK(circleOfConfusion(50, 0.8f, focus) == doctest::Approx(-0.5f)); // below: near
    focus.mode = FocusMode::Both;
    // The stronger term wins with its own sign.
    CHECK(circleOfConfusion(8, 0.8f, focus) == doctest::Approx(-0.5f));
    CHECK(circleOfConfusion(30, 0.8f, focus) == doctest::Approx(1));
}

TEST_CASE("frame pools keep what a frame used and rebuild what changed") {
    FramePool<std::string, int> pool;
    int made = 0;
    auto make = [&] {
        return ++made;
    };
    CHECK(pool.acquire("a", make) == 1);
    CHECK(pool.acquire("a", make) == 1);
    CHECK(pool.acquire("b", make) == 2);
    pool.endFrame();
    CHECK(pool.size() == 2);
    CHECK(pool.acquire("a", make) == 1); // reused across frames
    pool.endFrame();
    CHECK(pool.size() == 1); // "b" went unused for a frame
    CHECK(pool.find("b") == nullptr);
    CHECK(pool.acquire("a", make, [](int value) { return value != 1; }) == 3); // rejected: rebuilt
    CHECK(*pool.find("a") == 3);
}

TEST_CASE("freeze frames capture once per freeze call and release on unfreeze") {
    FreezeFrame freeze;
    using Action = FreezeFrame::Action;
    CHECK(freeze.update(false, 0) == Action::None);
    CHECK(freeze.update(true, 1) == Action::Capture);
    CHECK(freeze.held());
    CHECK(freeze.update(true, 1) == Action::None);
    CHECK(freeze.update(true, 2) == Action::Capture); // freeze() again recaptures
    CHECK(freeze.update(false, 2) == Action::Release);
    CHECK_FALSE(freeze.held());
    CHECK(freeze.update(false, 2) == Action::None);
    CHECK(freeze.update(true, 2) == Action::Capture); // a later freeze with the same serial
}

TEST_CASE("custom post params pack in order into four vectors") {
    const auto packed = packCustomParams({1, 2, 3, 4, 5, 6});
    CHECK(packed[0] == glm::vec4(1, 2, 3, 4));
    CHECK(packed[1] == glm::vec4(5, 6, 0, 0));
    CHECK(packed[3] == glm::vec4(0));
    CHECK_THROWS(packCustomParams(std::vector<float>(17, 1.0f)));
}

TEST_CASE("glyph effects move by whole pixels") {
    CHECK(glyphEffectOffset(GlyphEffectNone, 3, 1.25) == glm::vec2(0));
    for (int frame = 0; frame < 30; ++frame) {
        const double time = frame / 60.0;
        const auto shake = glyphEffectOffset(GlyphEffectShake, 4, time);
        CHECK(std::abs(shake.x) <= 1);
        CHECK(std::abs(shake.y) <= 1);
        const auto wave = glyphEffectOffset(GlyphEffectWave, 4, time);
        CHECK(wave.x == 0);
        CHECK(std::abs(wave.y) <= 2);
        CHECK(wave.y == std::round(wave.y));
    }
    // Shake holds still within a step and differs between glyphs over time.
    CHECK(glyphEffectOffset(GlyphEffectShake, 1, 0.001) == glyphEffectOffset(GlyphEffectShake, 1, 0.06));
    bool differs = false;
    for (int step = 0; step < 20; ++step) {
        differs = differs || glyphEffectOffset(GlyphEffectShake, 1, step / 15.0) !=
                                 glyphEffectOffset(GlyphEffectShake, 2, step / 15.0);
    }
    CHECK(differs);
    // Wave runs along the text: neighbours are out of phase.
    CHECK(glyphEffectOffset(GlyphEffectWave, 0, 0.2) != glyphEffectOffset(GlyphEffectWave, 3, 0.2));
}

TEST_CASE("thumbnails box-filter down to the maximum width") {
    CHECK(thumbnailDivisor({1920, 1080}, 480) == 4);
    CHECK(thumbnailDivisor({2560, 1440}, 480) == 6);
    CHECK(thumbnailDivisor({320, 240}, 480) == 1);
    std::vector<std::uint8_t> rgba = {0, 0, 0, 255, 100, 200, 50, 255, 20, 40, 60, 255, 80, 0, 10, 255};
    glm::ivec2 size;
    const auto out = downscaleRgba(rgba, {2, 2}, 2, size);
    CHECK(size == glm::ivec2(1, 1));
    CHECK(out == std::vector<std::uint8_t>{50, 60, 30, 255});
}

TEST_CASE("post settings parse, round-trip and validate") {
    auto json = Environment{}.toJson();
    json["post"]["bloom_levels"] = 4;
    json["post"]["dof_mode"] = "depth";
    json["post"]["ssao"] = true;
    json["post"]["antialias"] = "none";
    json["post"]["custom"] = Json::array({{{"shader", "sepia"}, {"params", {1, 0.5}}}});
    const auto env = Environment::fromJson(json);
    CHECK(env.post.bloomLevels == 4);
    CHECK(env.post.dofMode == "depth");
    CHECK(env.post.ssao);
    CHECK_FALSE(env.post.fxaa());
    REQUIRE(env.post.custom.size() == 1);
    CHECK(env.post.custom[0].shader == "sepia");
    CHECK(env.post.custom[0].params == std::vector<float>{1, 0.5f});
    CHECK(Environment::fromJson(env.toJson()).post.custom[0].params.size() == 2);
    // FXAA by default, but not over a deliberately pixelated scene.
    PostSettings post;
    CHECK(post.fxaa());
    post.pixelScale = 3;
    CHECK_FALSE(post.fxaa());
    post.antialias = "fxaa";
    CHECK(post.fxaa());
    for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{{"bloom_levels", 9},
                                                                              {"bloom_radius", 1.5},
                                                                              {"dof_mode", "lens"},
                                                                              {"antialias", "msaa"},
                                                                              {"dof_radius", 40}}) {
        CAPTURE(key);
        auto bad = Environment{}.toJson();
        bad["post"][key] = value;
        CHECK_THROWS(Environment::fromJson(bad));
    }
    auto badShader = Environment{}.toJson();
    badShader["post"]["custom"] = Json::array({{{"shader", "../evil"}}});
    CHECK_THROWS(Environment::fromJson(badShader));
    badShader["post"]["custom"] = Json::array({{{"shader", "x"}, {"params", std::vector<float>(17, 0.0f)}}});
    CHECK_THROWS(Environment::fromJson(badShader));
}

TEST_CASE("draw camera overrides interpolate between ticks per world, target and order") {
    DrawCameras cameras;
    int world = 0, other = 0;
    Camera camera;
    camera.target = {0, 0, 0};
    camera.yaw = 0;
    // First frame: snapped.
    CHECK(cameras.update(&world, "map", camera, 1).focus(0.5).x == doctest::Approx(0));
    cameras.endFrame();
    // A tick later the override moved: the frame between ticks shows the blend.
    camera.target.x = 4;
    camera.yaw = 90;
    const auto& moved = cameras.update(&world, "map", camera, 2);
    CHECK(moved.focus(0.5).x == doctest::Approx(2));
    CHECK(moved.interpolatedYaw(0.25) == doctest::Approx(22.5f));
    CHECK(moved.focus(1).x == doctest::Approx(4));
    // Another draw of the same world and target in the same frame is its own camera.
    CHECK(cameras.update(&world, "map", camera, 2).focus(0.5).x == doctest::Approx(4));
    CHECK(cameras.update(&other, "map", camera, 2).focus(0.5).x == doctest::Approx(4));
    cameras.endFrame();
    CHECK(cameras.size() == 3);
    // A second frame in the same tick keeps the interpolation start.
    CHECK(cameras.update(&world, "map", camera, 2).focus(0.5).x == doctest::Approx(2));
    cameras.endFrame();
    CHECK(cameras.size() == 1); // draws not made for a frame are forgotten
    // The next tick starts from this tick's values.
    camera.target.x = 6;
    CHECK(cameras.update(&world, "map", camera, 3).focus(0.5).x == doctest::Approx(5));
    cameras.endFrame();
    cameras.endFrame();
    CHECK(cameras.update(&world, "map", camera, 4).focus(0).x == doctest::Approx(6)); // snapped again
}

TEST_CASE("post and screen chromatic aberration share one shift") {
    CHECK(chromaticShift({0.5f, 0.5f}, 1) == glm::vec2(0));
    const auto edge = chromaticShift({1, 0.5f}, 1);
    CHECK(edge.x == doctest::Approx(0.015f));
    CHECK(edge.y == doctest::Approx(0));
    CHECK(chromaticShift({0, 1}, 0.5f).x == doctest::Approx(-0.0075f));
    CHECK(chromaticShift({0, 1}, 0.5f).y == doctest::Approx(0.0075f));
}

TEST_CASE("the composite pass packs every post field it reads into four vectors") {
    PostSettings s;
    s.exposure = 1.5f;
    s.chromaticAberration = 0.4f;
    s.distortion = 2;
    s.vignette = false;
    s.vignetteStrength = 0.3f;
    s.lutBlendTarget = "night.cube";
    s.lutBlend = 0.25f;
    s.tonemap = "none";
    s.ssao = true;
    PostFrame frame;
    frame.bloomStrength = 0.7f;
    frame.depthOfField = true;
    frame.distorted = true;
    frame.blurRadius = 6;
    frame.tint = {0.5f, 0.6f, 0.7f, 0.8f};
    const auto post = packPost(s, frame);
    CHECK(post[0] == glm::vec4(0.7f, 1.5f, 0.4f, 2));
    CHECK(post[1] == glm::vec4(1, 0, 1, 1));
    CHECK(post[2] == glm::vec4(0.3f, 0.25f, 0, 6));
    CHECK(post[3] == frame.tint);
    frame.distorted = false;
    s.lutBlendTarget.clear();
    s.tonemap = "aces";
    const auto plain = packPost(s, frame);
    CHECK(plain[0].w == 0);
    CHECK(plain[2].y == 0);
    CHECK(plain[2].z == 1);
}

TEST_CASE("window depth linearises the same for both clip conventions, so water is right on OpenGL") {
    // fs_water.sc linearises without the convention flag; OpenGL's -1..1
    // projection stores the same window depth as the 0..1 one.
    const float n = 0.5f, f = 80;
    for (const float z : {0.7f, 10.0f, 63.0f}) {
        const float ndc = (f + n - 2 * f * n / z) / (f - n); // OpenGL
        const float window = ndc * 0.5f + 0.5f;
        const float direct = f * (z - n) / (z * (f - n)); // 0..1 clip range
        CHECK(window == doctest::Approx(direct));
        CHECK(linearDepth(window, n, f, true) == doctest::Approx(z).epsilon(1e-3));
        CHECK(linearDepth(window, n, f, false) == doctest::Approx(z).epsilon(1e-3));
    }
    WaterSettings w;
    WaterFrame frame;
    frame.nearPlane = n;
    frame.farPlane = f;
    frame.sceneCopy = true;
    const auto water = packWater(w, frame);
    CHECK(water[3] == glm::vec4(n, f, 1, 0));
    CHECK(water[4].z == 1);
}

TEST_CASE("render targets show premultiplied: canvas colours are premultiplied to match") {
    CHECK(premultiplyRgba(0xffffffffu) == 0xffffffffu);
    CHECK(premultiplyRgba(0xff804000u) == 0x00000000u);
    CHECK(premultiplyRgba(0xff804080u) == 0x80402080u);
    CHECK(premultiplyRgba(0x10203040u) == 0x04080c40u);
}

TEST_CASE("pixel_scale takes fractions from 1 to 8 and keeps whole numbers crisp") {
    auto json = Environment{}.toJson();
    json["post"]["pixel_scale"] = 1.5;
    const auto env = Environment::fromJson(json);
    CHECK(env.post.pixelScale == doctest::Approx(1.5f));
    CHECK_FALSE(env.post.fxaa());     // auto antialiasing stays off below full resolution
    CHECK_FALSE(env.post.pixelated()); // a fraction is filtered up
    CHECK(env.toJson()["post"]["pixel_scale"] == 1.5);
    json["post"]["pixel_scale"] = 3;
    CHECK(Environment::fromJson(json).post.pixelated());
    CHECK(Environment::fromJson(json).toJson()["post"]["pixel_scale"].is_number_integer());
    PostSettings post;
    CHECK_FALSE(post.pixelated());
    CHECK(Environment{}.toJson()["post"]["pixel_scale"] == 1);
    for (const auto value : {0.5, 0.0, 8.5, -2.0}) {
        CAPTURE(value);
        json["post"]["pixel_scale"] = value;
        CHECK_THROWS_WITH(Environment::fromJson(json), doctest::Contains("pixel_scale"));
    }
}

TEST_CASE("sprite protection defaults off and survives environment serialization") {
    auto map = MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json").toJson();
    map["environment"]["post"] = {{"protect_sprites", true}, {"antialias", "fxaa"}, {"depth_of_field", true}};
    const auto parsed = MapData::fromJson(map);
    CHECK(parsed.environment.post.protectSprites);
    CHECK(parsed.environment.post.fxaa());
    CHECK(parsed.environment.post.depthOfField);
    CHECK(MapData::fromJson(parsed.toJson()).environment.post.protectSprites);
    map["environment"]["post"].erase("protect_sprites");
    CHECK_FALSE(MapData::fromJson(map).environment.post.protectSprites);
}
