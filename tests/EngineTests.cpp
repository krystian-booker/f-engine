// The engine's draw requests, screen effects and camera handles as scripts
// drive them (what the renderer reads, without a GPU).
#include "engine/render/FrameMath.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <cmath>
#include <doctest/doctest.h>

using namespace fe;

namespace {
EngineOptions testEngine() {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = std::filesystem::temp_directory_path() / "fengine-engine-test";
    options.seed = 7;
    return options;
}
} // namespace

TEST_CASE("engine: world draws record rects, render targets and camera overrides") {
    Engine engine(testEngine());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); w:camera().yaw = 30").ok);
    engine.draw(1);
    REQUIRE(
        lua.evaluate("w:draw(); w:draw{rect = {10, 20, 64, 48}}; "
                     "w:draw{target = 'map', rect = {0, 0, 96, 80}, camera = {pitch = 89, distance = 30}}; "
                     "fe.canvas.image('rt:map', 4, 4)")
            .ok);
    const auto& draws = engine.drawList();
    REQUIRE(draws.size() == 3);
    CHECK_FALSE(draws[0].rect);
    REQUIRE(draws[1].rect);
    CHECK(draws[1].rect->w == doctest::Approx(64));
    CHECK(draws[1].target.empty());
    CHECK(draws[2].target == "map");
    REQUIRE(draws[2].camera);
    CHECK(draws[2].camera->pitch == doctest::Approx(89));
    CHECK(draws[2].camera->yaw == doctest::Approx(30)); // unset fields copy the world camera
    CHECK(engine.world("main")->camera().pitch != doctest::Approx(89));
    CHECK(engine.canvas().imageSize("rt:map") == glm::ivec2(96, 80));
    REQUIRE(!engine.canvas().quads().empty());
    CHECK(engine.canvas().quads().back().texture.image == "rt:map");
    CHECK(engine.focusWorld() == engine.world("main"));
    CHECK_FALSE(lua.evaluate("w:draw{target = 'Bad Name'}").ok);
    CHECK_FALSE(lua.evaluate("w:draw{rect = {0, 0, 0, 10}}").ok);
    CHECK(engine.canvas().imageSize("rt:unknown") == glm::ivec2(0));
}


TEST_CASE("engine: a draw's post table overrides the environment's post fields for that draw") {
    Engine engine(testEngine());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); "
                         "w:set_environment{post = {bloom_strength = 0.9, exposure = 1.4, ssao = true}}")
                .ok);
    REQUIRE(lua.evaluate("w:draw(); w:draw{target = 'map', post = {depth_of_field = false, bloom = false, "
                         "custom = {{shader = 'sepia'}}}}")
                .ok);
    const auto& draws = engine.drawList();
    REQUIRE(draws.size() == 2);
    CHECK_FALSE(draws[0].post);
    REQUIRE(draws[1].post);
    CHECK_FALSE(draws[1].post->depthOfField);
    CHECK_FALSE(draws[1].post->bloom);
    CHECK(draws[1].post->exposure == doctest::Approx(1.4f)); // unset fields copy the environment
    CHECK(draws[1].post->ssao);
    REQUIRE(draws[1].post->custom.size() == 1);
    CHECK(draws[1].post->custom[0].shader == "sepia");
    const auto& environment = engine.world("main")->environment().post;
    CHECK(environment.bloom);
    CHECK(environment.depthOfField);
    CHECK(environment.custom.empty());
    CHECK_FALSE(lua.evaluate("w:draw{post = {custom = {{shader = 'Bad Name'}}}}").ok);
}


TEST_CASE("engine: screen freeze, radial blur and world-only thumbnails are requests") {
    Engine engine(testEngine());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("fe.screen.freeze()").ok);
    CHECK(engine.screen().frozen);
    const auto serial = engine.screen().freezeSerial;
    REQUIRE(lua.evaluate("fe.screen.freeze()").ok);
    CHECK(engine.screen().freezeSerial == serial + 1);
    CHECK(lua.evaluate("assert(fe.screen.frozen())").ok);
    REQUIRE(lua.evaluate("fe.screen.unfreeze(); fe.screen.set{radial_blur = 2, radial_centre = {0.25, 0.75}, "
                         "chromatic = 0.5, transition = {kind = 'shatter', progress = 0.5}}")
                .ok);
    CHECK_FALSE(engine.screen().frozen);
    CHECK(engine.screen().radialBlur == doctest::Approx(1)); // clamped
    CHECK(engine.screen().radialCentre == glm::vec2(0.25f, 0.75f));
    CHECK(lua.evaluate("local s = fe.screen.get(); assert(s.chromatic == 0.5 and s.radial_centre.x == 0.25)")
              .ok);
    REQUIRE(lua.evaluate("fe.save.thumbnail('slot1')").ok);
    CHECK(engine.requests().captures.empty());
    REQUIRE(engine.requests().sceneCaptures.size() == 1);
    CHECK(engine.requests().sceneCaptures[0] == engine.saves().thumbnailPath("slot1"));
}


TEST_CASE("engine: distort sprites and emitters are recorded for the renderer") {
    Engine engine(testEngine());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); "
                         "e = w:spawn{position = {x = 2.5, z = 5.5}, sprite = {animation = "
                         "'@fe/assets/ui/spark.anim.json', distort = 2}, "
                         "emitter = {rate = 60, distort = 0.5}}")
                .ok);
    CHECK(lua.evaluate("assert(e:sprite().distort == 2)").ok);
    engine.tick(1.0 / 60);
    engine.tick(1.0 / 60);
    const auto& particles = engine.world("main")->particles();
    REQUIRE(!particles.empty());
    CHECK(particles.front().distort == doctest::Approx(0.5f));
    CHECK_FALSE(lua.evaluate("w:spawn{position = {x = 1, z = 1}, emitter = {distort = -1}}").ok);
}


TEST_CASE("engine: a draw's camera override and the camera handle carry the orthographic fields") {
    Engine engine(testEngine());
    auto& lua = engine.lua();
    REQUIRE(lua.evaluate("w = fe.world.load('main', 'arena'); c = w:camera(); c.distance = 20; c.fov = 30")
                .ok);
    CHECK(lua.console("c.orthographic") == "false");
    REQUIRE(lua.evaluate("c.orthographic = true").ok);
    CHECK(engine.world("main")->camera().orthographic);
    // The default height follows distance and fov; an explicit one sticks.
    CHECK(lua.console("math.floor(c.ortho_height * 1000)") ==
          std::to_string(static_cast<int>(std::floor(2 * 20 * std::tan(glm::radians(15.0f)) * 1000))));
    REQUIRE(lua.evaluate("c.ortho_height = 12").ok);
    CHECK(lua.console("c.ortho_height") == "12.0");
    REQUIRE(lua.evaluate("c.ortho_height = 0").ok);
    CHECK(engine.world("main")->camera().orthoHeight == 0);
    CHECK_FALSE(lua.evaluate("c.ortho_height = 1e9").ok);
    CHECK_FALSE(lua.evaluate("c.orthographic = 'yes'").ok);
    REQUIRE(lua.evaluate("c.orthographic = false").ok);
    REQUIRE(lua.evaluate("w:draw{target = 'map', rect = {0, 0, 64, 64}, camera = {orthographic = true, "
                         "ortho_height = 40}}")
                .ok);
    const auto& draws = engine.drawList();
    REQUIRE(draws.size() == 1);
    REQUIRE(draws[0].camera);
    CHECK(draws[0].camera->orthographic);
    CHECK(draws[0].camera->orthoHeight == doctest::Approx(40));
    CHECK_FALSE(engine.world("main")->camera().orthographic);
    CHECK_FALSE(lua.evaluate("w:draw{camera = {ortho_height = 1e9}}").ok);
    // Draw cameras carry the fields across frames.
    DrawCameras cameras;
    int world = 0;
    const auto& first = cameras.update(&world, "map", *draws[0].camera, 1);
    CHECK(first.orthographic);
    cameras.endFrame();
    Camera next = *draws[0].camera;
    next.orthographic = false;
    next.orthoHeight = 0;
    const auto& second = cameras.update(&world, "map", next, 2);
    CHECK_FALSE(second.orthographic);
    CHECK(second.orthoHeight == 0);
}
