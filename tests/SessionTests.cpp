#include "engine/editor/EditorSession.h"
#include "engine/editor/SceneEdits.h"
#include "engine/runtime/Engine.h"
#include "engine/world/World.h"
#include <doctest/doctest.h>
#include <filesystem>

using namespace fe;

namespace {
// An editing session over a scratch copy of the test game, so saving and
// scene files never touch the repository.
struct Project {
    std::filesystem::path root;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<EditorSession> session;
    unsigned changes = 0;
    std::string status;

    explicit Project(const char* name) {
        root = std::filesystem::temp_directory_path() / (std::string("fengine-session-") + name);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::copy(FENGINE_TEST_GAME, root / "game", std::filesystem::copy_options::recursive);
        EngineOptions options;
        options.gameRoot = root / "game";
        options.builtinRoot = FENGINE_BUILTIN;
        options.userData = root / "user";
        options.backgroundSaves = false;
        engine = std::make_unique<Engine>(std::move(options));
        session = std::make_unique<EditorSession>(*engine);
        session->onChanged = [this](unsigned what) {
            changes |= what;
        };
        session->onStatus = [this](const std::string& text) {
            status = text;
        };
    }
    ~Project() {
        session.reset();
        engine.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
} // namespace

TEST_CASE("a session opens a scene as a preview world and lists the project's scenes") {
    Project project("open");
    auto& session = *project.session;
    const auto scenes = session.scenes();
    CHECK(std::find(scenes.begin(), scenes.end(), "arena") != scenes.end());
    CHECK(session.map() == nullptr);
    REQUIRE(session.openScene("arena"));
    REQUIRE(session.map() != nullptr);
    CHECK(session.sceneId() == "arena");
    CHECK_FALSE(session.unsaved());
    CHECK((project.changes & EditorSession::SceneChanged) != 0);
    CHECK_FALSE(session.openScene("no_such_scene"));
    CHECK(session.map() == nullptr);
    CHECK(project.status.find("Cannot open") != std::string::npos);
}

TEST_CASE("edits are undo steps, a gesture is one step, and undoing to the saved state is saved") {
    Project project("undo");
    auto& session = *project.session;
    REQUIRE(session.openScene("arena"));
    const Json before = session.map()->toJson();
    const auto point = session.add(Selection::Kind::Point, {2, 0, 2}, "probe");
    REQUIRE(point.kind == Selection::Kind::Point);
    CHECK(session.selection() == point);
    CHECK(session.map()->points.contains(point.id));
    CHECK(session.unsaved());

    // A drag: many edits, one step.
    auto transform = selectionTransform(*session.world(), point);
    REQUIRE(transform.has_value());
    const auto start = transform->position;
    for (int i = 1; i <= 5; ++i) {
        transform->position = start + glm::vec3(0.5f * static_cast<float>(i), 0, 0);
        CHECK(session.setTransform(point, *transform, true));
    }
    session.endGesture();
    CHECK(session.map()->points.at(point.id).position.x == doctest::Approx(start.x + 2.5f));
    session.undo();
    CHECK(session.map()->points.at(point.id).position.x == doctest::Approx(start.x));
    session.undo();
    CHECK_FALSE(session.map()->points.contains(point.id));
    CHECK(session.selection().empty()); // what it named is gone
    CHECK(session.map()->toJson() == before);
    CHECK_FALSE(session.unsaved());
    CHECK_FALSE(session.canUndo());
    session.redo();
    session.redo();
    CHECK(session.map()->points.at(point.id).position.x == doctest::Approx(start.x + 2.5f));
}

TEST_CASE("objects are added, duplicated and deleted, a group as one step") {
    Project project("objects");
    auto& session = *project.session;
    REQUIRE(session.openScene("arena"));
    const auto* map = session.map();
    const auto lights = map->lights.size(), zones = map->zones.size(), colliders = map->colliders.size();
    const auto light = session.add(Selection::Kind::Light, {3, 0, 3});
    const auto zone = session.add(Selection::Kind::Zone, {3, 0, 3});
    const auto collider = session.add(Selection::Kind::Collider, {3, 0, 3});
    CHECK(map->lights.size() == lights + 1);
    CHECK(map->zones.size() == zones + 1);
    CHECK(map->colliders.size() == colliders + 1);

    session.selectAll({light, zone, collider});
    CHECK(session.selected().size() == 3);
    session.duplicateSelection();
    CHECK(map->lights.size() == lights + 2);
    CHECK(map->zones.size() == zones + 2);
    CHECK(map->colliders.size() == colliders + 2);
    CHECK(session.selected().size() == 3); // the copies
    session.deleteSelection();
    CHECK(map->lights.size() == lights + 1);
    CHECK(map->zones.size() == zones + 1);
    CHECK(session.selection().empty());
    session.undo(); // the delete, as one step
    CHECK(map->lights.size() == lights + 2);
    CHECK(map->colliders.size() == colliders + 2);

    const auto placement = session.addPlacement("crate", {4, 0, 4});
    REQUIRE(placement.kind == Selection::Kind::Placement);
    CHECK(findPlacement(*map, placement.id)->type == "crate");
    session.toggle(light);
    CHECK(session.isSelected(light));
    CHECK(session.isSelected(placement));
    session.toggle(light);
    CHECK_FALSE(session.isSelected(light));
}

TEST_CASE("typed edits replace an object only when the map still loads") {
    Project project("typed");
    auto& session = *project.session;
    REQUIRE(session.openScene("arena"));
    const auto zone = session.add(Selection::Kind::Zone, {3, 0, 3});
    Json value = session.objectJson(zone);
    REQUIRE(value.is_object());
    value["props"]["note"] = "hello";
    value["id"] = "renamed_zone";
    CHECK(session.setObjectJson(zone, value).empty());
    CHECK(session.selection() == Selection::zone("renamed_zone")); // the rename follows the selection
    CHECK(findZone(*session.map(), "renamed_zone")->props.at("note") == "hello");

    const Json good = session.objectJson(session.selection());
    Json bad = good;
    bad["min"] = "not a point";
    CHECK_FALSE(session.setObjectJson(session.selection(), bad).empty());
    CHECK(session.objectJson(session.selection()) == good);

    CHECK(session.setMapJson("/name", "A New Name").empty());
    CHECK(session.map()->name == "A New Name");
    CHECK_FALSE(session.setMapJson("/id", "other").empty()); // a scene's id is its folder
    CHECK_FALSE(session.setMapJson("/environment/sun_color", "bright").empty());

    const auto point = session.add(Selection::Kind::Point, {1, 0, 1}, "door");
    CHECK(session.renamePoint(point.id, "gate"));
    CHECK(session.selection() == Selection::point("gate"));
    CHECK_FALSE(session.renamePoint("gate", "gate"));
    CHECK_FALSE(session.renamePoint("gate", ""));
}

TEST_CASE("scenes are created, saved, copied, renamed and deleted on disk") {
    Project project("files");
    auto& session = *project.session;
    const auto maps = project.root / "game" / "maps";
    REQUIRE(session.createScene("fresh", "Fresh", 8, 6));
    CHECK(std::filesystem::is_regular_file(maps / "fresh" / "map.json"));
    CHECK(session.map()->grid.width() == 8);
    CHECK_FALSE(session.createScene("fresh", "Again", 4, 4));
    CHECK_FALSE(session.createScene("Bad Id", "x", 4, 4));

    session.add(Selection::Kind::Point, {2, 0, 2}, "marker");
    CHECK(session.unsaved());
    REQUIRE(session.save());
    CHECK_FALSE(session.unsaved());
    CHECK(MapData::load(maps / "fresh" / "map.json").points.contains("marker"));

    session.add(Selection::Kind::Point, {3, 0, 3}, "unsaved_marker");
    REQUIRE(session.saveSceneAs("fresh_copy"));
    CHECK(session.sceneId() == "fresh_copy");
    CHECK(session.map()->points.contains("unsaved_marker")); // the copy has the edits
    CHECK_FALSE(
        MapData::load(maps / "fresh" / "map.json").points.contains("unsaved_marker")); // the original not

    REQUIRE(session.renameScene("fresh_two"));
    CHECK(session.sceneId() == "fresh_two");
    CHECK_FALSE(std::filesystem::exists(maps / "fresh_copy"));
    REQUIRE(session.deleteScene("fresh_two"));
    CHECK(session.map() == nullptr);
    CHECK_FALSE(std::filesystem::exists(maps / "fresh_two"));
}

TEST_CASE("painting walkability on a scene that was never baked writes the grid, as one stroke") {
    Project project("paint");
    auto& session = *project.session;
    REQUIRE(session.createScene("flat", "Flat", 8, 8));
    const auto& grid = session.map()->grid;
    REQUIRE(grid.walkableAt({2.5f, 2.5f}, 0));
    GridBrush blocked;
    blocked.walkable = false;
    session.paintWalkability({2, 2, 0}, 0, blocked, false, true);
    session.paintWalkability({3, 2, 0}, 0, blocked, false, true);
    session.endGesture();
    CHECK_FALSE(grid.walkableAt({2.5f, 2.5f}, 0));
    CHECK_FALSE(grid.walkableAt({3.5f, 2.5f}, 0));
    session.undo();
    CHECK(grid.walkableAt({2.5f, 2.5f}, 0));
    CHECK(grid.walkableAt({3.5f, 2.5f}, 0));
}

TEST_CASE("rectangles, flood fills, ramps and tags paint the grid, each as one undo step") {
    Project project("shapes");
    auto& session = *project.session;
    REQUIRE(session.createScene("flat", "Flat", 8, 8));
    const auto& grid = session.map()->grid;
    const auto walkable = [&](int x, int z) {
        return grid.walkableAt({static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f}, 0);
    };
    GridBrush blocked;
    blocked.walkable = false;
    // A wall across the map, corner to corner in either order.
    session.paintWalkabilityRect({7, 3, 0}, {0, 3, 0}, blocked, false);
    for (int x = 0; x < 8; ++x) {
        CHECK_FALSE(walkable(x, 3));
    }
    CHECK(walkable(0, 2));
    CHECK(walkable(0, 4));

    // A flood fill stops at the wall.
    GridBrush raised;
    raised.mode = GridBrush::Mode::Height;
    raised.height = 2;
    session.floodWalkability({1, 1, 0}, raised);
    CHECK(grid.heightAt({1.5f, 1.5f}, 0) == doctest::Approx(2));
    CHECK(grid.heightAt({6.5f, 2.5f}, 0) == doctest::Approx(2));
    CHECK(grid.heightAt({1.5f, 5.5f}, 0) == doctest::Approx(0));
    session.undo(); // the fill
    CHECK(grid.heightAt({1.5f, 1.5f}, 0) == doctest::Approx(0));
    session.undo(); // the wall
    CHECK(walkable(4, 3));

    GridBrush ramp;
    ramp.mode = GridBrush::Mode::Ramp;
    ramp.ramp = Ramp::North;
    session.paintWalkability({2, 2, 0}, 0, ramp, false, false);
    CHECK(grid.at(2, 2, 0).ramp == Ramp::North);
    project.status.clear();
    session.floodWalkability({1, 1, 0}, ramp);
    CHECK(project.status.find("Ramps") != std::string::npos); // refused, said why

    GridBrush tag;
    tag.mode = GridBrush::Mode::AddTag;
    tag.tag = "mud";
    session.paintWalkabilityRect({0, 0, 0}, {1, 1, 0}, tag, false);
    CHECK(session.map()->toJson().at("grid").at("tags").dump().find("mud") != std::string::npos);
}

TEST_CASE("bake settings and layer links are edited as parts of the map file") {
    Project project("parts");
    auto& session = *project.session;
    REQUIRE(session.openScene("arena"));
    CHECK(session.setMapJson("/bake/auto", false).empty());
    CHECK_FALSE(session.map()->bake.autoBake);
    CHECK(session.setMapJson("/bake/options", Json{{"max_slope", 20.0}}).empty());
    CHECK(session.map()->bake.options.at("max_slope") == 20.0);
    CHECK(session.mapJson("/grid/links").is_array());
    CHECK_FALSE(session.setMapJson("/grid/links", "none").empty()); // refused: the map would not load
    session.undo();
    session.undo();
    CHECK(session.map()->bake.autoBake);
}

TEST_CASE("the default description gives forms when the game describes nothing") {
    Project project("schemas");
    auto& session = *project.session;
    session.refreshSchemas();
    CHECK((project.changes & EditorSession::SchemasChanged) != 0);
    CHECK_FALSE(session.typeNames("entities").empty());
    CHECK_FALSE(session.defaultType("entities").empty());
    CHECK(session.fieldsFor(session.defaultType("entities"), {"entities"}) != nullptr);
    CHECK(session.fieldsFor("no_such_type", {"entities", "zones"}) == nullptr);
}
