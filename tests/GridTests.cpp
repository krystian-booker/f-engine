#include "engine/nav/Query.h"
#include <doctest/doctest.h>

using namespace fe;

namespace {
Grid arena() {
    // 8x6, a wall at x=4 for z<4, a raised block reached by a ramp.
    Grid grid(8, 6, 1);
    for (int z = 0; z < 4; ++z) {
        grid.at(4, z).walkable = false;
    }
    grid.at(6, 0).height = 1;
    grid.at(5, 0).ramp = Ramp::East;
    return grid;
}
} // namespace

TEST_CASE("grid JSON round trip keeps heights, walls, ramps, tags and links") {
    auto grid = arena();
    grid.at(1, 1).tags = grid.tagBit("water", true) | grid.tagBit("region:2", true);
    grid.addLayer("bridge");
    grid.at(2, 2, 1).walkable = true;
    grid.setLinks({{2, 2, 0, 1, {1, 0}}});
    grid.setCellProps({3, 3, 0}, {{"treasure", "potion"}});
    const auto copy = Grid::fromJson(grid.toJson());
    CHECK(copy.layerCount() == 2);
    CHECK_FALSE(copy.at(4, 0).walkable);
    CHECK(copy.at(6, 0).height == doctest::Approx(1));
    CHECK(copy.at(5, 0).ramp == Ramp::East);
    CHECK(copy.tagsOf(copy.at(1, 1).tags) == std::vector<std::string>{"water", "region:2"});
    REQUIRE(copy.linkAt(2, 2, 0));
    CHECK(copy.linkAt(2, 2, 0)->direction == glm::ivec2(1, 0));
    REQUIRE(copy.cellProps({3, 3, 0}));
    CHECK(copy.cellProps({3, 3, 0})->at("treasure") == "potion");
}

TEST_CASE("ramps interpolate height and edges meet") {
    const auto grid = arena();
    CHECK(grid.heightAt({5.0f, 0.5f}) == doctest::Approx(0));
    CHECK(grid.heightAt({5.75f, 0.5f}) == doctest::Approx(0.75f));
    CHECK(grid.edgeHeight({5, 0, 0}, 1, 0) ==
          doctest::Approx(grid.edgeHeight({6, 0, 0}, -1, 0)).epsilon(0.01));
}

TEST_CASE("A* goes around walls and respects step height") {
    const auto grid = arena();
    StepRules rules;
    const auto path = findPath(grid, {1, 1, 0}, {6, 1, 0}, rules);
    REQUIRE_FALSE(path.empty());
    CHECK(path.back() == GridPos{6, 1, 0});
    for (const auto& cell : path) {
        CHECK(grid.at(cell).walkable);
    }
    // The raised cell is only reachable by climbing the ramp at (5, 0).
    const auto climb = findPath(grid, {7, 1, 0}, {6, 0, 0}, rules);
    REQUIRE_FALSE(climb.empty());
    CHECK(std::find(climb.begin(), climb.end(), GridPos{5, 0, 0}) != climb.end());
    auto flat = grid;
    flat.at(5, 0).ramp = Ramp::None;
    CHECK(findPath(flat, {7, 1, 0}, {6, 0, 0}, rules).empty());
}

TEST_CASE("reachable honours budget, jump limits, blocked and no-stop cells") {
    Grid grid(9, 9, 1);
    grid.at(5, 4).height = 2; // a tall pillar
    StepRules rules;
    rules.maxUp = 1;
    rules.maxDown = 3;
    rules.blocked.insert(cellKey({4, 3, 0}));
    rules.noStop.insert(cellKey({3, 4, 0}));
    const auto cells = reachable(grid, {4, 4, 0}, 2, rules);
    auto find = [&](GridPos p) {
        return std::find_if(cells.begin(), cells.end(), [&](const ReachCell& c) { return c.pos == p; });
    };
    CHECK(find({4, 4, 0}) != cells.end());
    CHECK(find({5, 4, 0}) == cells.end()); // too high to climb
    CHECK(find({4, 3, 0}) == cells.end()); // blocked by an enemy
    REQUIRE(find({3, 4, 0}) != cells.end());
    CHECK_FALSE(find({3, 4, 0})->stoppable);
    CHECK(find({2, 4, 0}) != cells.end());
    CHECK(find({1, 4, 0}) == cells.end()); // beyond the budget
    const auto path = reachPath(cells, {2, 4, 0});
    REQUIRE(path.size() == 2);
    CHECK(path.back() == GridPos{2, 4, 0});
}

TEST_CASE("line of sight is blocked by higher terrain") {
    Grid grid(10, 3, 1);
    grid.at(5, 1).height = 3;
    CHECK_FALSE(lineOfSight(grid, {1.5f, 1, 1.5f}, {8.5f, 1, 1.5f}, 0));
    CHECK(lineOfSight(grid, {1.5f, 4, 1.5f}, {8.5f, 4, 1.5f}, 0));
}

TEST_CASE("area shapes produce the expected cell counts") {
    CHECK(areaOffsets(AreaShape::Diamond, 1, 2).size() == 12);
    CHECK(areaOffsets(AreaShape::Diamond, 0, 1).size() == 5);
    CHECK(areaOffsets(AreaShape::Square, 0, 1).size() == 9);
    CHECK(areaOffsets(AreaShape::Cross, 1, 2).size() == 8);
    const auto line = areaOffsets(AreaShape::Line, 1, 3, {1, 0});
    REQUIRE(line.size() == 3);
    CHECK(line[0] == glm::ivec2(1, 0));
    CHECK(areaOffsets(AreaShape::Cone, 1, 2, {0, 1}).size() == 4);
}

TEST_CASE("raycast picks the cell under a downward ray") {
    Grid grid(6, 6, 1);
    grid.at(3, 3).height = 1;
    const auto hit = raycastGrid(grid, {3.5f, 10, 3.5f}, {0, -1, 0});
    REQUIRE(hit);
    CHECK(hit->cell == GridPos{3, 3, 0});
    CHECK(hit->point.y == doctest::Approx(1));
}
