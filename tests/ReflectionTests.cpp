#include "engine/runtime/Engine.h"
#include "engine/script/Binder.h"
#include "engine/script/LuaHost.h"
#include "engine/world/ComponentInfo.h"
#include "engine/world/World.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <fstream>
#include <glm/gtc/constants.hpp>

using namespace fe;

namespace {
// The arena map (12 x 10 cells, two layers, grid tags water and deploy) with one entity, "subject".
struct Reflected {
    AssetCache assets{FENGINE_TEST_GAME, FENGINE_BUILTIN};
    World world{"reflect", MapData::load(std::filesystem::path(FENGINE_TEST_GAME) / "maps/arena/map.json"),
                assets, 5};
    entt::entity entity = world.spawn("subject", {2.5f, 0, 5.5f});

    template<class T> T& add(T component = {}) { return world.registry().emplace<T>(entity, component); }
    template<class T> [[nodiscard]] const T& get() const { return world.registry().get<T>(entity); }
};

const ComponentInfo& infoOf(const std::string& name) {
    const auto& infos = componentInfos();
    const auto found =
        std::find_if(infos.begin(), infos.end(), [&](const ComponentInfo& c) { return name == c.name; });
    REQUIRE(found != infos.end());
    return *found;
}

std::vector<std::string> names(const std::vector<const ComponentInfo*>& infos) {
    std::vector<std::string> result;
    for (const auto* component : infos) {
        result.emplace_back(component->name);
    }
    return result;
}

// Every edit in `edits` is refused with a problem and leaves read() as it was.
void checkRefused(Reflected& f, const ComponentInfo& component, const std::vector<Json>& edits) {
    const Json before = component.read(f.world, f.entity);
    for (const auto& edit : edits) {
        CAPTURE(edit.dump());
        CHECK_FALSE(component.write(f.world, f.entity, edit).empty());
        CHECK(component.read(f.world, f.entity) == before);
    }
}
} // namespace

TEST_CASE("component infos cover every component, in display order, and list what an entity has") {
    CHECK(names([] {
              std::vector<const ComponentInfo*> all;
              for (const auto& component : componentInfos()) {
                  all.push_back(&component);
              }
              return all;
          }()) == std::vector<std::string>{"Name", "Transform", "EntitySerial", "Tags", "Sprite",
                                           "MeshInstance", "LightSource", "Emitter", "Collider", "Mover",
                                           "Platform", "AudioEmitter", "MaterialParams", "Props",
                                           "ScriptRef"});
    for (const auto& component : componentInfos()) {
        CAPTURE(component.name);
        const std::string name = component.name;
        CHECK(component.removable ==
              (name != "Name" && name != "Transform" && name != "EntitySerial" && name != "ScriptRef"));
    }

    Reflected f;
    CHECK(names(componentsOf(f.world, f.entity)) ==
          std::vector<std::string>{"Name", "Transform", "EntitySerial"});
    // Added out of order; listed in display order.
    f.add<Props>();
    f.add<Collider>();
    f.add<Sprite>();
    f.add<Tags>();
    CHECK(names(componentsOf(f.world, f.entity)) == std::vector<std::string>{"Name", "Transform",
                                                                             "EntitySerial", "Tags", "Sprite",
                                                                             "Collider", "Props"});
    const auto unnamed = f.world.spawn("", {1.5f, 0, 1.5f});
    CHECK(names(componentsOf(f.world, unnamed)) == std::vector<std::string>{"Transform", "EntitySerial"});

    // A component the entity lacks reads as null; a destroyed entity has none and refuses edits.
    CHECK(infoOf("Mover").read(f.world, f.entity).is_null());
    CHECK_FALSE(infoOf("Mover").has(f.world, f.entity));
    f.world.destroy(unnamed);
    CHECK(componentsOf(f.world, unnamed).empty());
    CHECK(infoOf("Transform").read(f.world, unnamed).is_null());
    CHECK_FALSE(infoOf("Transform").write(f.world, unnamed, Json::object()).empty());
    // An edit is an object.
    CHECK_FALSE(infoOf("Transform").write(f.world, f.entity, Json::array()).empty());
}

TEST_CASE("Name reads its id and renames through the world's index") {
    Reflected f;
    const auto& name = infoOf("Name");
    CHECK(name.read(f.world, f.entity) == Json{{"id", "subject"}});
    CHECK(name.write(f.world, f.entity, {{"id", "renamed"}}).empty());
    CHECK(f.get<Name>().id == "renamed");
    CHECK(f.world.find("renamed") == f.entity);
    CHECK(f.world.find("subject") == entt::entity{entt::null});

    f.world.spawn("other", {1.5f, 0, 1.5f});
    checkRefused(f, name, {{{"id", "other"}}, {{"id", ""}}, {{"id", 7}}});
    CHECK(f.world.find("renamed") == f.entity);

    // An entity spawned without an id gets one.
    const auto unnamed = f.world.spawn("", {3.5f, 0, 1.5f});
    CHECK_FALSE(name.has(f.world, unnamed));
    CHECK_FALSE(name.write(f.world, unnamed, Json::object()).empty());
    CHECK(name.write(f.world, unnamed, {{"id", "late"}}).empty());
    CHECK(f.world.find("late") == unnamed);
}

TEST_CASE("EntitySerial and ScriptRef are shown and never written") {
    Reflected f;
    const auto& serial = infoOf("EntitySerial");
    const auto value = f.world.serial(f.entity);
    CHECK(serial.read(f.world, f.entity) == Json{{"value", value}});
    CHECK(serial.readOnly == std::vector<std::string>{"value"});
    CHECK(serial.write(f.world, f.entity, {{"value", value + 100}}).empty());
    CHECK(f.world.serial(f.entity) == value);

    const auto& script = infoOf("ScriptRef");
    CHECK_FALSE(script.has(f.world, f.entity));
    CHECK(script.write(f.world, f.entity, Json::object()).empty());
    CHECK_FALSE(script.has(f.world, f.entity)); // not added either
    f.add<ScriptRef>({41});
    CHECK(script.read(f.world, f.entity) == Json::object());
    CHECK(script.write(f.world, f.entity, {{"reference", 3}}).empty());
    CHECK(f.get<ScriptRef>().reference == 41);
}

TEST_CASE("Tags read and write a list of names") {
    Reflected f;
    const auto& tags = infoOf("Tags");
    f.add<Tags>({{"door", "locked"}});
    CHECK(tags.read(f.world, f.entity) == Json{{"values", {"door", "locked"}}});
    CHECK(tags.write(f.world, f.entity, {{"values", {"door", "open", "door"}}}).empty());
    CHECK(f.get<Tags>().values == std::vector<std::string>{"door", "open"});
    CHECK(tags.write(f.world, f.entity, {{"unknown", 1}}).empty()); // unknown keys are ignored
    CHECK(f.get<Tags>().values == std::vector<std::string>{"door", "open"});
    checkRefused(f, tags, {{{"values", "door"}}, {{"values", {"door", 3}}}});
}

TEST_CASE("Transform edits move the entity in the collision hash and keep what they do not name") {
    Reflected f;
    const auto& transform = infoOf("Transform");
    const auto parent = f.world.spawn("carrier", {4.5f, 0, 4.5f});
    f.world.registry().get<Transform>(f.entity).parent = parent;
    f.add<Collider>();

    const Json read = transform.read(f.world, f.entity);
    CHECK(read.at("position") == Json{2.5, 0.0, 5.5});
    CHECK(read.at("layer") == 0);
    CHECK(read.at("facing") == Json{0.0, 1.0});
    CHECK(read.at("yaw") == 0.0);
    CHECK(read.at("scale") == Json{1.0, 1.0, 1.0});
    CHECK(read.at("parent") == "carrier");
    CHECK(read.at("offset") == Json{0.0, 0.0, 0.0});
    CHECK(read.at("ground") == false);
    CHECK(transform.readOnly == std::vector<std::string>{"parent"});

    // What read() returned is accepted back and changes nothing.
    CHECK(transform.write(f.world, f.entity, read).empty());
    CHECK(transform.read(f.world, f.entity) == read);

    RayOptions options;
    const auto hitAt = [&](float x) {
        const auto hit = f.world.raycast({x, 0.5f, 0.5f}, {0, 0, 1}, options);
        return hit && hit->kind == WorldHit::Kind::Entity && hit->entity == f.entity;
    };
    CHECK(hitAt(2.5f));
    CHECK(transform
              .write(f.world, f.entity,
                     {{"position", {3.5, 0.0, 5.5}},
                      {"layer", 1},
                      {"facing", {3.0, 0.0}},
                      {"yaw", 90.0},
                      {"scale", {2.0, 2.0, 2.0}},
                      {"offset", {0.0, 1.0, 0.0}},
                      {"ground", true},
                      {"parent", "nobody"}})
              .empty());
    const auto& t = f.get<Transform>();
    CHECK(t.position == glm::vec3(3.5f, 0, 5.5f));
    CHECK(t.previous == t.position); // shown where it is, also while paused
    CHECK(t.layer == 1);
    CHECK(t.facing == glm::vec2(1, 0)); // normalised
    CHECK(t.yaw == doctest::Approx(glm::half_pi<float>()));
    CHECK(t.previousYaw == t.yaw);
    CHECK(t.scale == glm::vec3(2));
    CHECK(t.offset == glm::vec3(0, 1, 0));
    CHECK(t.ground);
    CHECK(t.parent == parent); // read-only
    CHECK(f.world.registry().get<Transform>(f.entity).layer == 1);

    // Missing keys keep their value.
    CHECK(transform.write(f.world, f.entity, {{"layer", 0}}).empty());
    CHECK(f.get<Transform>().position == glm::vec3(3.5f, 0, 5.5f));
    CHECK(f.get<Transform>().layer == 0);
    CHECK_FALSE(hitAt(2.5f)); // re-indexed without a tick
    CHECK(hitAt(3.5f));

    checkRefused(f, transform,
                 {{{"position", {1.0, 2.0}}},
                  {{"position", "here"}},
                  {{"position", {1.0, "y", 2.0}}},
                  {{"position", {1e9, 0.0, 0.0}}},
                  {{"layer", 2}},
                  {{"layer", -1}},
                  {{"layer", 0.5}},
                  {{"facing", {0.0, 0.0}}},
                  {{"yaw", "north"}},
                  {{"scale", 2.0}},
                  {{"ground", 1}},
                  // Nothing of an edit is applied when one field is refused.
                  {{"position", {1.5, 0.0, 1.5}}, {"ground", "yes"}}});
}

TEST_CASE("Sprite fields use the Lua table's names; a new clip starts from its beginning") {
    Reflected f;
    const auto& sprite = infoOf("Sprite");
    auto& added = f.add<Sprite>();
    added.animation = "sprites/hero.anim.json";
    added.time = added.previousTime = 0.75;
    added.finished = true;

    const Json read = sprite.read(f.world, f.entity);
    CHECK(read.at("animation") == "sprites/hero.anim.json");
    CHECK(read.at("clip") == "idle");
    CHECK(read.at("move_clip") == "");
    CHECK(read.at("idle_clip") == "");
    CHECK(read.at("time") == 0.75);
    CHECK(read.at("speed") == 1.0);
    CHECK(read.at("direction") == -1);
    CHECK(read.at("visible") == true);
    CHECK(read.at("blend") == "opaque");
    CHECK(read.at("unlit") == false);
    CHECK(read.at("cast_shadow") == true);
    CHECK(read.at("mode") == "upright");
    CHECK(read.at("scale") == 1.0);
    CHECK(read.at("depth_bias") == 0.0);
    CHECK(read.at("offset") == Json{0.0, 0.0});
    CHECK(read.at("tint") == Json{1.0, 1.0, 1.0, 1.0});
    CHECK(read.at("flash") == Json{1.0, 1.0, 1.0, 0.0});
    CHECK(read.at("outline") == Json{0.0, 0.0, 0.0, 0.0});
    CHECK(read.at("outline_width") == 1.0);
    CHECK(read.at("silhouette") == Json{0.0, 0.0, 0.0, 0.0});
    CHECK(read.at("pixel_snap").is_null());
    CHECK(read.at("palette") == "");
    CHECK(read.at("palette_row") == 0);
    CHECK(read.at("distort") == 0.0);
    CHECK(read.at("shader") == "");
    CHECK(read.at("finished") == true);
    CHECK(read.size() == 26);
    CHECK(sprite.readOnly == std::vector<std::string>{"time", "finished"});

    // Read-only state is ignored, and an unchanged clip keeps its time.
    CHECK(sprite.write(f.world, f.entity, read).empty());
    CHECK(sprite.read(f.world, f.entity) == read);
    CHECK(sprite.write(f.world, f.entity, {{"time", 9.0}, {"finished", false}, {"speed", 2.0}}).empty());
    CHECK(f.get<Sprite>().time == 0.75);
    CHECK(f.get<Sprite>().finished);
    CHECK(f.get<Sprite>().speed == 2.0f);

    CHECK(sprite
              .write(f.world, f.entity,
                     {{"clip", "walk"},
                      {"move_clip", "walk"},
                      {"idle_clip", "idle"},
                      {"direction", 2},
                      {"visible", false},
                      {"blend", "additive"},
                      {"unlit", true},
                      {"cast_shadow", false},
                      {"mode", "decal"},
                      {"scale", 1.5},
                      {"depth_bias", 0.25},
                      {"offset", {4.0, -2.0}},
                      {"tint", {0.5, 0.25, 0.125}},
                      {"flash", {1.0, 0.0, 0.0, 0.5}},
                      {"outline", {0.0, 0.0, 0.0, 1.0}},
                      {"outline_width", 2.0},
                      {"silhouette", {1.0, 1.0, 1.0, 0.5}},
                      {"pixel_snap", true},
                      {"palette", "sprites/palette.png"},
                      {"palette_row", 3},
                      {"distort", 0.5},
                      {"shader", "glow"}})
              .empty());
    const auto& s = f.get<Sprite>();
    CHECK(s.clip == "walk");
    CHECK(s.time == 0); // the new clip starts over
    CHECK_FALSE(s.finished);
    CHECK(s.moveClip == "walk");
    CHECK(s.idleClip == "idle");
    CHECK(s.direction == 2);
    CHECK_FALSE(s.visible);
    CHECK(s.blend == BlendMode::Additive);
    CHECK(s.unlit);
    CHECK_FALSE(s.castShadow);
    CHECK(s.mode == SpriteMode::Decal);
    CHECK(s.scale == 1.5f);
    CHECK(s.depthBias == 0.25f);
    CHECK(s.pixelOffset == glm::vec2(4, -2));
    CHECK(s.tint == glm::vec4(0.5f, 0.25f, 0.125f, 1)); // [r, g, b]: opaque
    CHECK(s.flash == glm::vec4(1, 0, 0, 0.5f));
    CHECK(s.outline == glm::vec4(0, 0, 0, 1));
    CHECK(s.outlineWidth == 2.0f);
    CHECK(s.silhouette == glm::vec4(1, 1, 1, 0.5f));
    CHECK(s.pixelSnap == std::optional<bool>(true));
    CHECK(s.palette == "sprites/palette.png");
    CHECK(s.paletteRow == 3);
    CHECK(s.distort == 0.5f);
    CHECK(s.shader == "glow");
    CHECK(s.animation == "sprites/hero.anim.json"); // not named, kept
    CHECK(sprite.read(f.world, f.entity).at("mode") == "decal");
    CHECK(sprite.write(f.world, f.entity, {{"pixel_snap", nullptr}}).empty());
    CHECK_FALSE(f.get<Sprite>().pixelSnap.has_value());

    checkRefused(f, sprite,
                 {{{"speed", 1001.0}},
                  {{"speed", "fast"}},
                  {{"scale", -1.0}},
                  {{"direction", -2}},
                  {{"direction", 1.5}},
                  {{"visible", "no"}},
                  {{"blend", "shiny"}},
                  {{"blend", 3}},
                  {{"mode", "sideways"}},
                  {{"mode", 1}},
                  {{"tint", {1.0, 1.0}}},
                  {{"tint", {1.0, 1.0, "blue"}}},
                  {{"outline_width", 0.25}},
                  {{"outline_width", 4.5}},
                  {{"pixel_snap", "on"}},
                  {{"palette_row", -1}},
                  {{"distort", -0.5}},
                  {{"shader", "Not A Name"}},
                  {{"clip", 4}},
                  {{"clip", "run"}, {"speed", 5000.0}}});
}

TEST_CASE("MeshInstance fields and its optional blend") {
    Reflected f;
    const auto& mesh = infoOf("MeshInstance");
    auto& added = f.add<MeshInstance>();
    added.model = "models/crate.glb";
    added.time = 0.5f;

    const Json read = mesh.read(f.world, f.entity);
    CHECK(read.at("model") == "models/crate.glb");
    CHECK(read.at("animation") == "");
    CHECK(read.at("time") == 0.5);
    CHECK(read.at("speed") == 1.0);
    CHECK(read.at("loop") == true);
    CHECK(read.at("playing") == false);
    CHECK(read.at("visible") == true);
    CHECK(read.at("cast_shadow") == true);
    CHECK(read.at("tint") == Json{1.0, 1.0, 1.0, 1.0});
    CHECK(read.at("blend").is_null());
    CHECK(read.at("shader") == "");
    CHECK(read.at("from_animation") == "");
    CHECK(read.at("fade") == 0.0);
    CHECK(read.at("fade_elapsed") == 0.0);
    CHECK(mesh.readOnly == std::vector<std::string>{"time", "from_animation", "fade", "fade_elapsed"});
    CHECK(mesh.write(f.world, f.entity, read).empty());
    CHECK(mesh.read(f.world, f.entity) == read);

    CHECK(mesh.write(f.world, f.entity,
                     {{"model", "models/door.glb"},
                      {"animation", "open"},
                      {"time", 3.0},
                      {"speed", -2.0},
                      {"loop", false},
                      {"playing", true},
                      {"visible", false},
                      {"cast_shadow", false},
                      {"tint", {1.0, 0.5, 0.5, 0.5}},
                      {"blend", "alpha"},
                      {"shader", "dissolve"}})
              .empty());
    const auto& m = f.get<MeshInstance>();
    CHECK(m.model == "models/door.glb");
    CHECK(m.animation == "open");
    CHECK(m.time == 0.5f); // read-only
    CHECK(m.speed == -2.0f);
    CHECK_FALSE(m.loop);
    CHECK(m.playing);
    CHECK_FALSE(m.visible);
    CHECK_FALSE(m.castShadow);
    CHECK(m.tint == glm::vec4(1, 0.5f, 0.5f, 0.5f));
    CHECK(m.blend == std::optional<BlendMode>(BlendMode::Alpha));
    CHECK(m.shader == "dissolve");
    CHECK(mesh.read(f.world, f.entity).at("blend") == "alpha");
    CHECK(mesh.write(f.world, f.entity, {{"blend", nullptr}}).empty());
    CHECK_FALSE(f.get<MeshInstance>().blend.has_value());

    checkRefused(f, mesh,
                 {{{"speed", -1001.0}},
                  {{"model", 3}},
                  {{"loop", "yes"}},
                  {{"tint", "red"}},
                  {{"blend", "glossy"}},
                  {{"shader", "bad name!"}}});
}

TEST_CASE("LightSource is its light settings plus enabled, checked by the map loader") {
    Reflected f;
    const auto& light = infoOf("LightSource");
    f.add<LightSource>();

    const Json read = light.read(f.world, f.entity);
    CHECK(read.at("type") == "point");
    CHECK(read.at("position") == Json{0.0, 2.0, 0.0});
    CHECK(read.at("color").size() == 3);
    CHECK(read.at("direction") == Json{0.0, -1.0, 0.0});
    CHECK(read.at("radius") == 5.0);
    CHECK(read.at("intensity") == 2.0);
    CHECK(read.at("inner_cone") == 20.0);
    CHECK(read.at("outer_cone") == 35.0);
    CHECK(read.at("flicker") == 0.0);
    CHECK(read.at("flicker_speed") == 8.0);
    CHECK(read.at("flicker_curve") == Json::array());
    CHECK(read.at("shadows") == false);
    CHECK(read.at("enabled") == true);
    CHECK(light.readOnly.empty());
    CHECK(light.write(f.world, f.entity, read).empty());
    CHECK(light.read(f.world, f.entity) == read);

    CHECK(light
              .write(f.world, f.entity,
                     {{"type", "spot"},
                      {"position", {0.0, 3.0, 0.0}},
                      {"color", {1.0, 0.0, 0.0}},
                      {"direction", {1.0, 0.0, 0.0}},
                      {"radius", 9.0},
                      {"intensity", 4.0},
                      {"inner_cone", 10.0},
                      {"outer_cone", 30.0},
                      {"flicker", 0.5},
                      {"flicker_speed", 2.0},
                      {"flicker_curve", {{0.0, 1.0}, {0.5, 0.25}}},
                      {"shadows", true},
                      {"enabled", false}})
              .empty());
    const auto& source = f.get<LightSource>();
    CHECK(source.light.type == LightType::Spot);
    CHECK(source.light.position == glm::vec3(0, 3, 0));
    CHECK(source.light.color == glm::vec3(1, 0, 0));
    CHECK(source.light.direction == glm::vec3(1, 0, 0));
    CHECK(source.light.radius == 9.0f);
    CHECK(source.light.intensity == 4.0f);
    CHECK(source.light.innerCone == 10.0f);
    CHECK(source.light.outerCone == 30.0f);
    CHECK(source.light.flicker == 0.5f);
    CHECK(source.light.flickerSpeed == 2.0f);
    CHECK(source.light.flickerCurve == std::vector<glm::vec2>{{0, 1}, {0.5f, 0.25f}});
    CHECK(source.light.castShadows);
    CHECK_FALSE(source.enabled);
    // One field alone keeps the rest.
    CHECK(light.write(f.world, f.entity, {{"enabled", true}}).empty());
    CHECK(f.get<LightSource>().enabled);
    CHECK(f.get<LightSource>().light.radius == 9.0f);

    checkRefused(f, light,
                 {{{"radius", 0.0}},
                  {{"radius", 5000.0}},
                  {{"radius", "wide"}},
                  {{"intensity", -1.0}},
                  {{"type", "laser"}},
                  {{"type", 2}},
                  {{"direction", {0.0, 0.0, 0.0}}},
                  {{"position", {1.0, 2.0}}},
                  {{"flicker_curve", {{1.0, 1.0}, {0.5, 1.0}}}},
                  {{"enabled", "on"}},
                  {{"radius", 3.0}, {"enabled", 0}}});
}

TEST_CASE("Emitter is its settings plus paused, with its live particle count for information") {
    Reflected f;
    const auto& emitter = infoOf("Emitter");
    f.add<Emitter>();

    const Json read = emitter.read(f.world, f.entity);
    for (const char* key :
         {"sprite",          "rate",    "burst",     "lifetime", "lifetime_jitter", "velocity",
          "velocity_jitter", "gravity", "area",      "size",     "size_end",        "drag",
          "stretch",         "color",   "color_end", "blend",    "unlit",           "world_space",
          "distort",         "max",     "paused",    "live",     "burst_done"}) {
        CAPTURE(key);
        CHECK(read.contains(key));
    }
    CHECK(read.size() == 23);
    CHECK(read.at("rate") == 10.0);
    CHECK(read.at("velocity") == Json{0.0, 1.0, 0.0});
    CHECK(read.at("color") == Json{1.0, 1.0, 1.0, 1.0});
    CHECK(read.at("blend") == "alpha");
    CHECK(read.at("max") == 512);
    CHECK(read.at("paused") == false);
    CHECK(read.at("live") == 0);
    CHECK(read.at("burst_done") == false);
    CHECK(emitter.readOnly == std::vector<std::string>{"live", "burst_done"});
    CHECK(emitter.write(f.world, f.entity, read).empty());
    CHECK(emitter.read(f.world, f.entity) == read);

    f.world.registry().get<Emitter>(f.entity).accumulator = 0.5;
    CHECK(emitter
              .write(f.world, f.entity,
                     {{"sprite", "sprites/spark.png"},
                      {"rate", 40.0},
                      {"burst", 8},
                      {"lifetime", 2.0},
                      {"velocity", {0.0, 3.0, 0.0}},
                      {"gravity", {0.0, -9.0, 0.0}},
                      {"size", 0.5},
                      {"color_end", {1.0, 0.0, 0.0, 0.0}},
                      {"blend", "additive"},
                      {"world_space", false},
                      {"max", 64},
                      {"paused", true},
                      {"live", 99},
                      {"burst_done", true}})
              .empty());
    const auto& e = f.get<Emitter>();
    CHECK(e.settings.sprite == "sprites/spark.png");
    CHECK(e.settings.rate == 40.0f);
    CHECK(e.settings.burst == 8);
    CHECK(e.settings.lifetime == 2.0f);
    CHECK(e.settings.velocity == glm::vec3(0, 3, 0));
    CHECK(e.settings.gravity == glm::vec3(0, -9, 0));
    CHECK(e.settings.size == 0.5f);
    CHECK(e.settings.colorEnd == glm::vec4(1, 0, 0, 0));
    CHECK(e.settings.blend == BlendMode::Additive);
    CHECK_FALSE(e.settings.worldSpace);
    CHECK(e.settings.maxParticles == 64);
    CHECK(e.settings.lifetimeJitter == 0.25f); // not named, kept
    CHECK_FALSE(e.active);
    CHECK_FALSE(e.burstDone);    // read-only
    CHECK(e.accumulator == 0.5); // run-time state survives an edit
    CHECK(emitter.write(f.world, f.entity, {{"paused", false}}).empty());
    CHECK(f.get<Emitter>().active);
    CHECK(f.get<Emitter>().settings.rate == 40.0f);

    checkRefused(f, emitter,
                 {{{"rate", -1.0}},
                  {{"rate", 6000.0}},
                  {{"rate", "many"}},
                  {{"lifetime", 0.0}},
                  {{"size", 0.0}},
                  {{"burst", 5000}},
                  {{"max", 0}},
                  {{"max", 1.5}},
                  {{"blend", "sparkly"}},
                  {{"velocity", {1.0, 2.0}}},
                  {{"paused", "yes"}},
                  {{"rate", 5.0}, {"paused", 1}}});
}

TEST_CASE("Collider edits are range-checked and reach collision queries at once") {
    Reflected f;
    const auto& collider = infoOf("Collider");
    f.add<Collider>();
    CHECK(collider.read(f.world, f.entity) ==
          Json{{"radius", 0.25}, {"height", Json(1.6f)}, {"solid", true}});
    CHECK(collider.readOnly.empty());

    RayOptions options;
    // A ray along z, 0.6 to the side of the entity: outside the radius until it grows.
    const auto grazes = [&] {
        const auto hit = f.world.raycast({3.1f, 0.5f, 0.5f}, {0, 0, 1}, options);
        return hit && hit->kind == WorldHit::Kind::Entity && hit->entity == f.entity;
    };
    CHECK_FALSE(grazes());
    CHECK(collider.write(f.world, f.entity, {{"radius", 1.0}, {"height", 2.0}}).empty());
    CHECK(f.get<Collider>().radius == 1.0f);
    CHECK(f.get<Collider>().height == 2.0f);
    CHECK(f.get<Collider>().solid);
    CHECK(grazes());
    CHECK(collider.write(f.world, f.entity, {{"solid", false}}).empty());
    CHECK_FALSE(f.get<Collider>().solid);
    CHECK(f.get<Collider>().radius == 1.0f);

    checkRefused(f, collider,
                 {{{"radius", -0.1}},
                  {{"radius", 65.0}},
                  {{"radius", "big"}},
                  {{"height", 1001.0}},
                  {{"solid", 1}},
                  {{"radius", 0.5}, {"height", -1.0}}});

    // Writing to an entity without the component adds it, from the defaults.
    const auto bare = f.world.spawn("bare", {6.5f, 0, 2.5f});
    CHECK(collider.write(f.world, bare, {{"radius", 0.5}}).empty());
    CHECK(f.world.registry().get<Collider>(bare).radius == 0.5f);
    CHECK(f.world.registry().get<Collider>(bare).height == 1.6f);
    // A refused edit adds nothing.
    const auto other = f.world.spawn("untouched", {7.5f, 0, 2.5f});
    CHECK_FALSE(collider.write(f.world, other, {{"radius", 100.0}}).empty());
    CHECK_FALSE(collider.has(f.world, other));
}

TEST_CASE("Mover settings are editable, its motion is shown, and grid tags are names") {
    Reflected f;
    const auto& mover = infoOf("Mover");
    f.add<Mover>();

    const Json read = mover.read(f.world, f.entity);
    CHECK(read.at("speed") == 2.5);
    CHECK(read.at("max_step") == Json(0.6f));
    CHECK(read.at("radius") == Json(0.2f));
    CHECK(read.at("collide") == true);
    CHECK(read.at("climb_speed") == 1.5);
    CHECK(read.at("footprint") == 1);
    CHECK(read.at("fly") == false);
    CHECK(read.at("altitude") == 0.0);
    CHECK(read.at("altitude_speed") == 0.0);
    CHECK(read.at("blocked_tags") == Json::array());
    CHECK(read.at("required_tags") == Json::array());
    CHECK(read.at("jump") == false);
    CHECK(read.at("lift") == 0.0);
    CHECK(read.at("motion") == "none");
    CHECK(read.at("waypoints") == 0);
    CHECK(read.at("drive") == Json{0.0, 0.0});
    CHECK(read.at("moved") == false);
    CHECK(read.at("cell").is_null());
    CHECK(read.size() == 18);
    CHECK(mover.readOnly ==
          std::vector<std::string>{"lift", "motion", "waypoints", "drive", "moved", "cell"});
    CHECK(mover.write(f.world, f.entity, read).empty());
    CHECK(mover.read(f.world, f.entity) == read);

    CHECK(mover
              .write(f.world, f.entity,
                     {{"speed", 4.0},
                      {"max_step", 1.0},
                      {"radius", 0.25},
                      {"collide", false},
                      {"climb_speed", 2.0},
                      {"footprint", 2},
                      {"fly", true},
                      {"altitude", 1.5},
                      {"altitude_speed", 3.0},
                      {"blocked_tags", {"deploy"}},
                      {"required_tags", {"water"}},
                      {"jump", {{"distance", 2}, {"diagonal", true}}},
                      {"lift", 7.0},
                      {"motion", "launch"}})
              .empty());
    const auto& m = f.get<Mover>();
    CHECK(m.speed == 4.0f);
    CHECK(m.maxStep == 1.0f);
    CHECK(m.radius == 0.25f);
    CHECK_FALSE(m.collide);
    CHECK(m.climbSpeed == 2.0f);
    CHECK(m.footprint == 2);
    CHECK(m.fly);
    CHECK(m.altitude == 1.5f);
    CHECK(m.altitudeSpeed == 3.0f);
    CHECK(m.blockedTags == f.world.grid().tagMask({"deploy"}));
    CHECK(m.requiredTags == f.world.grid().tagMask({"water"}));
    CHECK(m.jump.distance == 2);
    CHECK(m.jump.maxUp == 1.0f); // from max_step, as a script's jump table
    CHECK(m.jump.maxDown == 1.0f);
    CHECK(m.jump.diagonal);
    CHECK(m.lift == 0.0f); // read-only
    CHECK(m.idle());
    const Json edited = mover.read(f.world, f.entity);
    CHECK(edited.at("blocked_tags") == Json{"deploy"});
    CHECK(edited.at("required_tags") == Json{"water"});
    CHECK(edited.at("jump") == Json{{"distance", 2}, {"max_up", 1.0}, {"max_down", 1.0}, {"diagonal", true}});
    // One jump limit keeps the others; false (or distance 0) removes gap jumps.
    CHECK(mover.write(f.world, f.entity, {{"jump", {{"max_down", 3.0}}}}).empty());
    CHECK(f.get<Mover>().jump.distance == 2);
    CHECK(f.get<Mover>().jump.maxDown == 3.0f);
    CHECK(mover.write(f.world, f.entity, {{"jump", false}, {"blocked_tags", Json::array()}}).empty());
    CHECK(f.get<Mover>().jump.distance == 0);
    CHECK(f.get<Mover>().blockedTags == 0u);
    CHECK(mover.read(f.world, f.entity).at("jump") == false);

    // A route in progress survives an edit of the settings and is reported.
    CHECK(mover.write(f.world, f.entity, {{"fly", false}, {"required_tags", Json::array()}}).empty());
    f.world.follow(f.entity, {Waypoint{{4.5f, 0, 5.5f}}, Waypoint{{5.5f, 0, 5.5f}}});
    CHECK(mover.write(f.world, f.entity, {{"speed", 1.0}}).empty());
    const Json walking = mover.read(f.world, f.entity);
    CHECK(walking.at("motion") == "route");
    CHECK(walking.at("waypoints") == 2);

    checkRefused(f, mover,
                 {{{"speed", -1.0}},
                  {{"speed", 1001.0}},
                  {{"speed", "quick"}},
                  {{"radius", 0.5}},
                  {{"climb_speed", 0.0}},
                  {{"footprint", 0}},
                  {{"footprint", 9}},
                  {{"footprint", 1.5}},
                  {{"altitude", -1.0}},
                  {{"blocked_tags", {"lava"}}},
                  {{"required_tags", "water"}},
                  {{"jump", true}},
                  {{"jump", 2}},
                  {{"jump", {{"distance", -1}}}},
                  {{"jump", {{"distance", 2}, {"max_up", -1.0}}}},
                  {{"collide", "no"}},
                  {{"speed", 3.0}, {"fly", 1}}});
}

TEST_CASE("Platform size is two positive numbers up to 64") {
    Reflected f;
    const auto& platform = infoOf("Platform");
    f.add<Platform>();
    CHECK(platform.read(f.world, f.entity) == Json{{"size", {1.0, 1.0}}});
    CHECK(platform.write(f.world, f.entity, {{"size", {3.0, 2.0}}}).empty());
    CHECK(f.get<Platform>().size == glm::vec2(3, 2));
    checkRefused(
        f, platform,
        {{{"size", {0.0, 1.0}}}, {{"size", {1.0, 65.0}}}, {{"size", 2.0}}, {{"size", {1.0, 2.0, 3.0}}}});
}

TEST_CASE("AudioEmitter needs a sound inside the game folder and lets go of its voice for a new one") {
    Reflected f;
    const auto& audio = infoOf("AudioEmitter");
    f.add<AudioEmitter>({"audio/tone.ogg", 8, 1, 4});
    CHECK(audio.read(f.world, f.entity) ==
          Json{{"sound", "audio/tone.ogg"}, {"radius", 8.0}, {"volume", 1.0}, {"playing", true}});
    CHECK(audio.readOnly == std::vector<std::string>{"playing"});

    CHECK(audio.write(f.world, f.entity, {{"radius", 12.0}, {"volume", 0.5}, {"playing", false}}).empty());
    CHECK(f.get<AudioEmitter>().radius == 12.0f);
    CHECK(f.get<AudioEmitter>().volume == 0.5f);
    CHECK(f.get<AudioEmitter>().voice == 4); // the same sound keeps playing
    CHECK(audio.write(f.world, f.entity, {{"sound", "audio/other.ogg"}}).empty());
    CHECK(f.get<AudioEmitter>().sound == "audio/other.ogg");
    CHECK(f.get<AudioEmitter>().voice == -1);
    CHECK(audio.read(f.world, f.entity).at("playing") == false);

    checkRefused(f, audio,
                 {{{"sound", ""}},
                  {{"sound", "../outside.ogg"}},
                  {{"sound", 3}},
                  {{"radius", 0.0}},
                  {{"radius", "far"}},
                  {{"volume", -1.0}}});
    // The defaults have no sound: an empty edit cannot add the component.
    const auto bare = f.world.spawn("silent", {6.5f, 0, 2.5f});
    CHECK_FALSE(audio.write(f.world, bare, Json::object()).empty());
    CHECK_FALSE(audio.has(f.world, bare));
}

TEST_CASE("MaterialParams are sixteen numbers; a shorter list leaves the rest zero") {
    Reflected f;
    const auto& params = infoOf("MaterialParams");
    f.add<MaterialParams>();
    const Json read = params.read(f.world, f.entity);
    CHECK(read.at("values").size() == 16);
    CHECK(read.at("values") == Json(std::vector<double>(16, 0.0)));

    CHECK(params.write(f.world, f.entity, {{"values", {1.0, 2.0, 3.0, 4.0, 5.0}}}).empty());
    const auto& values = f.get<MaterialParams>().values;
    CHECK(values[0] == glm::vec4(1, 2, 3, 4));
    CHECK(values[1] == glm::vec4(5, 0, 0, 0));
    CHECK(values[3] == glm::vec4(0));
    CHECK(params.write(f.world, f.entity, {{"values", {9.0}}}).empty());
    CHECK(f.get<MaterialParams>().values[0] == glm::vec4(9, 0, 0, 0));
    CHECK(f.get<MaterialParams>().values[1] == glm::vec4(0));

    checkRefused(f, params,
                 {{{"values", std::vector<double>(17, 1.0)}}, {{"values", {1.0, "two"}}}, {{"values", 3.0}}});
}

TEST_CASE("Props hold one JSON object") {
    Reflected f;
    const auto& props = infoOf("Props");
    f.add<Props>({Json{{"opens", "gate"}, {"cost", 3}}});
    CHECK(props.read(f.world, f.entity) == Json{{"value", {{"opens", "gate"}, {"cost", 3}}}});
    const Json edited = {{"opens", "cellar"}, {"lines", {"one", "two"}}, {"nested", {{"deep", true}}}};
    CHECK(props.write(f.world, f.entity, {{"value", edited}}).empty());
    CHECK(f.get<Props>().value == edited);

    Json deep = Json::object();
    for (int i = 0; i < maxJsonDepth + 2; ++i) {
        deep = Json{{"inner", deep}};
    }
    checkRefused(f, props,
                 {{{"value", "text"}}, {{"value", {1, 2, 3}}}, {{"value", nullptr}}, {{"value", deep}}});
}

TEST_CASE("the Lua host lists the modules it has loaded, by name, with their files") {
    const auto root = std::filesystem::temp_directory_path() / "fengine-reflection-modules";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "util");
    std::ofstream(root / "zeta.lua") << "local helper = require('util.helper') return {value = helper.value}";
    std::ofstream(root / "util" / "helper.lua") << "return {value = 4}";
    std::ofstream(root / "alpha.lua") << "return {}";
    std::ofstream(root / "broken.lua") << "error('no')";
    std::ofstream(root / "unused.lua") << "return {}";

    LuaHost host(root, FENGINE_BUILTIN, true);
    const auto before = host.loadedModules();
    const auto listed = [&](const std::string& name) {
        const auto modules = host.loadedModules();
        const auto found = std::find_if(modules.begin(), modules.end(),
                                        [&](const LuaHost::LoadedModule& m) { return m.name == name; });
        return found == modules.end() ? std::filesystem::path() : found->file;
    };
    CHECK(listed("zeta").empty());

    host.require("zeta");
    host.require("alpha");
    host.require("alpha"); // cached: listed once
    CHECK_THROWS(host.require("broken"));
    const auto modules = host.loadedModules();
    CHECK(modules.size() == before.size() + 3);
    CHECK(std::is_sorted(modules.begin(), modules.end(),
                         [](const auto& a, const auto& b) { return a.name < b.name; }));
    CHECK(std::filesystem::equivalent(listed("zeta"), root / "zeta.lua"));
    CHECK(std::filesystem::equivalent(listed("util.helper"), root / "util" / "helper.lua"));
    CHECK(std::filesystem::equivalent(listed("alpha"), root / "alpha.lua"));
    CHECK(listed("broken").empty()); // failed to load
    CHECK(listed("unused").empty()); // never required
    std::filesystem::remove_all(root);
}

TEST_CASE("binding docs expose their entries for completion and hover text") {
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.builtinRoot = FENGINE_BUILTIN;
    options.userData = std::filesystem::temp_directory_path() / "fengine-reflection-user";
    options.seed = 1;
    Engine engine(std::move(options));
    const auto& entries = engine.docs().entries();
    CHECK(entries.size() > 100);
    const auto find = [&](const std::string& owner, const std::string& name) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const BindingDocs::Entry& e) {
            return e.owner == owner && e.name == name;
        });
        return found == entries.end() ? nullptr : &*found;
    };
    const auto* load = find("fe.world", "load");
    REQUIRE(load);
    CHECK(load->kind == BindingDocs::Kind::Function);
    CHECK(load->signature == "fun(name: string, map: string): World");
    CHECK_FALSE(load->doc.empty());
    CHECK(load->arity == 2);
    const auto* spawn = find("World", "spawn");
    REQUIRE(spawn);
    CHECK(spawn->kind == BindingDocs::Kind::Method);
    CHECK(find("fe.world", "no_such_binding") == nullptr);
}
