#include "engine/nav/Query.h"
#include <doctest/doctest.h>
#include <limits>
#include <numbers>

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

namespace {
// Ground everywhere except a river at x = 4..6; a bridge deck on layer 1
// spans x = 3..7 at height 1 with ramps at both ends. Directed links at the
// ramp feet climb east at x = 3 and west at x = 7.
Grid bridge() {
    Grid grid(11, 3, 1);
    for (int x = 4; x <= 6; ++x) {
        for (int z = 0; z < 3; ++z) {
            grid.at(x, z).walkable = false;
        }
    }
    grid.addLayer("deck");
    for (int x = 3; x <= 7; ++x) {
        auto& cell = grid.at(x, 1, 1);
        cell.walkable = true;
        cell.height = x == 3 || x == 7 ? 0 : 1;
    }
    grid.at(3, 1, 1).ramp = Ramp::East;
    grid.at(7, 1, 1).ramp = Ramp::West;
    grid.setLinks({{3, 1, 0, 1, {1, 0}}, {7, 1, 0, 1, {-1, 0}}});
    return grid;
}

bool sameCell(GridPos a, GridPos b) {
    return a.x == b.x && a.z == b.z;
}
} // namespace

TEST_CASE("A* crosses a bridge through directed links, never switching layers in place") {
    const auto grid = bridge();
    StepRules rules;
    const auto path = findPath(grid, {1, 1, 0}, {9, 1, 0}, rules);
    REQUIRE_FALSE(path.empty());
    CHECK(path.back() == GridPos{9, 1, 0});
    CHECK(std::find(path.begin(), path.end(), GridPos{3, 1, 1}) != path.end());
    CHECK(std::find(path.begin(), path.end(), GridPos{5, 1, 1}) != path.end());
    CHECK(std::find(path.begin(), path.end(), GridPos{8, 1, 0}) != path.end());
    for (std::size_t i = 1; i < path.size(); ++i) {
        CHECK_FALSE(sameCell(path[i - 1], path[i]));
    }
    // Entering the ramp foot from the north does not climb: the link points east.
    auto north = grid;
    north.at(2, 1).walkable = false;
    CHECK(findPath(north, {3, 0, 0}, {5, 1, 1}, rules).empty());
    // A gap between the surfaces at the ramp foot: the deck cannot be reached.
    auto gap = grid;
    gap.at(3, 1, 1).height = 0.5f;
    CHECK(findPath(gap, {1, 1, 0}, {5, 1, 1}, rules).empty());
}

TEST_CASE("undirected links switch layers where the current layer ends") {
    Grid grid(8, 3, 1);
    for (int x = 4; x < 8; ++x) {
        for (int z = 0; z < 3; ++z) {
            grid.at(x, z).walkable = false;
        }
    }
    grid.addLayer("pier");
    for (int x = 3; x < 8; ++x) {
        grid.at(x, 1, 1).walkable = true;
    }
    grid.setLinks({{3, 1, 0, 1, {0, 0}}});
    StepRules rules;
    const auto out = findPath(grid, {1, 1, 0}, {6, 1, 1}, rules);
    REQUIRE_FALSE(out.empty());
    CHECK(std::find(out.begin(), out.end(), GridPos{4, 1, 1}) != out.end());
    const auto back = findPath(grid, {6, 1, 1}, {1, 1, 0}, rules);
    REQUIRE_FALSE(back.empty());
    CHECK(std::find(back.begin(), back.end(), GridPos{2, 1, 0}) != back.end());
    // The shared rule itself: only where the layer ends ahead.
    CHECK(grid.traverseLink({3.9f, 1.5f}, 0, {1, 0}, 0.6f, 0.6f, true) == 1);
    CHECK(grid.traverseLink({3.5f, 1.5f}, 0, {1, 0}, 0.6f, 0.6f, false) == 0);
}

TEST_CASE("link lookups find every link in a cell") {
    Grid grid(4, 4, 3);
    grid.setLinks({{1, 1, 0, 1, {0, 0}}, {1, 1, 1, 2, {1, 0}}, {2, 2, 0, 2, {0, 0}}});
    REQUIRE(grid.linkAt(1, 1, 2));
    CHECK(grid.linkAt(1, 1, 2)->other(2) == 1);
    CHECK(grid.linkAt(1, 1, 0)->other(0) == 1);
    CHECK(grid.linkAt(2, 2, 1) == nullptr);
    CHECK(grid.linkAt(9, 9, 0) == nullptr);
    std::vector<int> others;
    CHECK_FALSE(grid.anyLinkAt(1, 1, 1, [&](const GridLink& link) {
        others.push_back(link.other(1));
        return false;
    }));
    CHECK(others == std::vector<int>{0, 2});
    CHECK(grid.anyLinkAt(1, 1, 1, [](const GridLink& link) { return link.other(1) == 2; }));
    CHECK_FALSE(grid.anyLinkAt(9, 9, 0, [](const GridLink&) { return true; }));
}

TEST_CASE("ramp names and codes come from one table") {
    for (const auto ramp : {Ramp::None, Ramp::North, Ramp::East, Ramp::South, Ramp::West}) {
        CHECK(rampFromName(rampName(ramp)) == ramp);
        CHECK(rampFromCode(rampCode(ramp)) == ramp);
    }
    CHECK_FALSE(rampFromName("up"));
    CHECK_FALSE(rampFromCode('x'));
}

TEST_CASE("diagonal steps cannot slip between tall pillars") {
    Grid grid(3, 3, 1);
    grid.at(1, 0).height = 2;
    grid.at(0, 1).height = 2;
    StepRules rules;
    rules.diagonal = true;
    CHECK(findPath(grid, {0, 0, 0}, {1, 1, 0}, rules).empty());
    rules.maxUp = rules.maxDown = 2;
    CHECK(findPath(grid, {0, 0, 0}, {1, 1, 0}, rules).size() == 1);
}

TEST_CASE("negative or non-finite tag costs are rejected") {
    Grid grid(2, 1);
    const auto road = grid.tagBit("road", true);
    StepRules rules;
    rules.tagCosts.emplace_back(road, -1.5f);
    CHECK_THROWS_AS(reachable(grid, {0, 0, 0}, 3, rules), std::invalid_argument);
    CHECK_THROWS_AS(findPath(grid, {0, 0, 0}, {1, 0, 0}, rules), std::invalid_argument);
    rules.tagCosts = {{road, std::numeric_limits<float>::quiet_NaN()}};
    CHECK_THROWS_AS(validateRules(rules), std::invalid_argument);
    rules.tagCosts = {{road, 0.5f}};
    CHECK_NOTHROW(validateRules(rules));
}

TEST_CASE("A* breaks ties on the cell key") {
    Grid grid(5, 5, 1);
    StepRules rules;
    const auto first = findPath(grid, {0, 0, 0}, {2, 2, 0}, rules);
    // Every 4-step route is equally short; lower keys (z, then x) are expanded first.
    const std::vector<GridPos> expected{{1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}};
    CHECK(first == expected);
    CHECK(findPath(grid, {0, 0, 0}, {2, 2, 0}, rules) == first);
}

TEST_CASE("line of sight traces every crossed cell, ignores the shooter's cell and sees bridges") {
    Grid grid(10, 6, 1);
    const auto wall = grid.tagBit("wall", true);
    grid.at(1, 1).tags = wall;
    CHECK(lineOfSight(grid, {1.5f, 1, 1.5f}, {8.5f, 1, 1.5f}, 0, wall));
    grid.at(4, 1).tags = wall;
    CHECK_FALSE(lineOfSight(grid, {1.5f, 1, 1.5f}, {8.5f, 1, 1.5f}, 0, wall));
    // A line that only clips the corner of a tall cell (about 0.04 units inside it).
    Grid corner(8, 8, 1);
    corner.at(3, 3).height = 5;
    CHECK_FALSE(lineOfSight(corner, {0.5f, 1, 5.53f}, {5.53f, 1, 0.5f}, 0));
    corner.at(3, 3).height = 0;
    CHECK(lineOfSight(corner, {0.5f, 1, 5.53f}, {5.53f, 1, 0.5f}, 0));
    // Bridge decks block lines through them, not lines passing underneath.
    Grid decks(10, 3, 1);
    decks.addLayer("deck");
    decks.at(4, 1, 1) = {2, true, Ramp::None, 0};
    decks.at(5, 1, 1) = {2, true, Ramp::None, 0};
    CHECK(lineOfSight(decks, {1.5f, 1, 1.5f}, {8.5f, 1, 1.5f}, 0));
    CHECK_FALSE(lineOfSight(decks, {1.5f, 3, 1.5f}, {8.5f, 0.5f, 1.5f}, 0));
    // Ground cliffs block a shooter standing on an upper layer.
    decks.at(1, 1, 1) = {1, true, Ramp::None, 0};
    decks.at(7, 1).height = 3;
    CHECK_FALSE(lineOfSight(decks, {1.5f, 2, 1.5f}, {8.5f, 2, 1.5f}, 1));
}

TEST_CASE("diagonal lines and cones follow the diagonal") {
    const auto line = areaOffsets(AreaShape::Line, 1, 3, {1, 1});
    CHECK(line == std::vector<glm::ivec2>{{1, 1}, {2, 2}, {3, 3}});
    const auto back = areaOffsets(AreaShape::Line, 1, 2, {-1, 1});
    CHECK(back == std::vector<glm::ivec2>{{-1, 1}, {-2, 2}});
    // Same cell count per ring as an orthogonal cone: 1 + 3.
    const auto cone = areaOffsets(AreaShape::Cone, 1, 2, {1, -1});
    CHECK(cone.size() == 4);
    for (const auto offset : cone) {
        CHECK(offset.x >= 1);
        CHECK(offset.y <= -1);
    }
    // Directions snap to the nearest of eight.
    CHECK(areaOffsets(AreaShape::Line, 1, 2, {0.9f, 0.3f}) == std::vector<glm::ivec2>{{1, 0}, {2, 0}});
    CHECK(areaOffsets(AreaShape::Line, 1, 1, {0.45f, 0.89f}) == std::vector<glm::ivec2>{{1, 1}});
}

TEST_CASE("picking stops at cliff faces and hits bridge decks") {
    Grid grid(8, 1, 1);
    grid.at(3, 0).height = 3;
    const auto hit = raycastGrid(grid, {0.5f, 2, 0.5f}, {1, -0.1f, 0});
    REQUIRE(hit);
    CHECK(hit->cell == GridPos{3, 0, 0});
    CHECK(hit->point.x == doctest::Approx(3).epsilon(0.001));
    grid.addLayer("deck");
    grid.at(6, 0, 1) = {2, true, Ramp::None, 0};
    const auto deck = raycastGrid(grid, {6.5f, 10, 0.5f}, {0, -1, 0});
    REQUIRE(deck);
    CHECK(deck->cell == GridPos{6, 0, 1});
    CHECK(deck->point.y == doctest::Approx(2));
    CHECK_FALSE(raycastGrid(grid, {20, 5, 20}, {1, 0, 0}));
}

TEST_CASE("zones of control end movement but stay reachable") {
    // A 7x1 corridor; a foe's zone covers x = 3.
    Grid grid(7, 3, 1);
    StepRules rules;
    rules.zoc.insert(cellKey({3, 1, 0}));
    const auto cells = reachable(grid, {1, 1, 0}, 5, rules);
    const auto at = [&](GridPos p) {
        return std::find_if(cells.begin(), cells.end(), [&](const ReachCell& c) { return c.pos == p; });
    };
    REQUIRE(at({3, 1, 0}) != cells.end());
    CHECK(at({3, 1, 0})->stoppable);
    // (4, 1) is reached around the zone cell, never through it.
    REQUIRE(at({4, 1, 0}) != cells.end());
    CHECK(at({4, 1, 0})->cost == doctest::Approx(5));
    CHECK(cells[static_cast<std::size_t>(at({4, 1, 0})->parent)].pos != GridPos{3, 1, 0});
    // A zone cell only stops movement that enters it, not movement that starts there.
    CHECK(reachable(grid, {3, 1, 0}, 1, rules).size() == 5);
    // A* honours the same rule.
    const auto path = findPath(grid, {1, 1, 0}, {5, 1, 0}, rules);
    REQUIRE_FALSE(path.empty());
    CHECK(std::find(path.begin(), path.end(), GridPos{3, 1, 0}) == path.end());
    rules.zoc = {cellKey({3, 0, 0}), cellKey({3, 1, 0}), cellKey({3, 2, 0})};
    CHECK(findPath(grid, {1, 1, 0}, {5, 1, 0}, rules).empty());
    CHECK(findPath(grid, {1, 1, 0}, {3, 2, 0}, rules).size() == 3);
}

TEST_CASE("a cost function reprices or forbids steps") {
    Grid grid(5, 3, 1);
    StepRules rules;
    int calls = 0;
    rules.cost = [&](GridPos from, GridPos to, float base) -> std::optional<float> {
        ++calls;
        CHECK(std::abs(from.x - to.x) + std::abs(from.z - to.z) == 1);
        if (to.z == 1 && to.x == 2) {
            return std::nullopt; // a wall the grid does not know about
        }
        return to.z == 2 ? base * 0.25f : base; // a cheap road along z = 2
    };
    const auto path = findPath(grid, {0, 1, 0}, {4, 1, 0}, rules);
    CHECK(calls > 0);
    REQUIRE_FALSE(path.empty());
    CHECK(std::find(path.begin(), path.end(), GridPos{2, 1, 0}) == path.end());
    // The cheapest route uses the road even though it is longer.
    CHECK(std::find(path.begin(), path.end(), GridPos{2, 2, 0}) != path.end());
    const auto cells = reachable(grid, {0, 2, 0}, 1, rules);
    CHECK(std::any_of(cells.begin(), cells.end(),
                      [](const ReachCell& c) { return c.pos == GridPos{4, 2, 0}; }));
    rules.cost = [](GridPos, GridPos, float) -> std::optional<float> {
        return -1.0f;
    };
    CHECK_THROWS_AS(reachable(grid, {0, 0, 0}, 3, rules), std::invalid_argument);
}

TEST_CASE("gap jumps cross pits and walls no higher than the lower end") {
    // Row z = 1: ground, a two-cell chasm, ground, a low wall, ground.
    Grid grid(8, 3, 1);
    for (int z = 0; z < 3; ++z) {
        grid.at(2, z).walkable = grid.at(3, z).walkable = false;
    }
    grid.at(5, 1).walkable = false;
    grid.at(5, 1).height = 0.5f; // a wall above the ground
    grid.at(5, 0).walkable = grid.at(5, 2).walkable = false;
    grid.at(5, 0).height = grid.at(5, 2).height = 1; // no way round
    StepRules rules;
    CHECK(findPath(grid, {1, 1, 0}, {4, 1, 0}, rules).empty());
    rules.jump = {1, 0.6f, 0.6f, false};
    CHECK(findPath(grid, {1, 1, 0}, {4, 1, 0}, rules).empty()); // the chasm is two cells wide
    rules.jump.distance = 2;
    const auto route = findRoute(grid, {1, 1, 0}, {4, 1, 0}, rules).steps;
    REQUIRE(route.size() == 1);
    CHECK(route[0].cell == GridPos{4, 1, 0});
    CHECK(route[0].kind == StepKind::Jump);
    const auto cells = reachable(grid, {1, 1, 0}, 3, rules);
    const auto landing = std::find_if(cells.begin(), cells.end(),
                                      [](const ReachCell& c) { return c.pos == GridPos{4, 1, 0}; });
    REQUIRE(landing != cells.end());
    CHECK(landing->kind == StepKind::Jump);
    CHECK(landing->cost == doctest::Approx(3));
    // The raised wall blocks the jump from (4, 1) to (6, 1).
    CHECK(findPath(grid, {4, 1, 0}, {6, 1, 0}, rules).empty());
    grid.at(5, 1).height = 0;
    CHECK(findRoute(grid, {4, 1, 0}, {6, 1, 0}, rules).steps.front().kind == StepKind::Jump);
    // Landing height limits.
    grid.at(6, 1).height = 1;
    rules.jump.maxUp = 0.5f;
    CHECK(findPath(grid, {4, 1, 0}, {6, 1, 0}, rules).empty());
    // A unit in the gap cannot be jumped over.
    grid.at(6, 1).height = 0;
    grid.at(5, 1).walkable = true;
    grid.at(5, 1).height = -3;
    rules.blocked.insert(cellKey({5, 1, 0}));
    CHECK(findPath(grid, {4, 1, 0}, {6, 1, 0}, rules).empty());
    rules.blocked.clear();
    CHECK(findRoute(grid, {4, 1, 0}, {6, 1, 0}, rules).steps.front().kind == StepKind::Jump);
}

TEST_CASE("diagonal gap jumps are opt-in and may not cut past a raised corner") {
    // A pit island: (1, 1) and (3, 3) are ground, everything else a chasm.
    Grid grid(5, 5, 1);
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 5; ++x) {
            grid.at(x, z).walkable = (x == 1 && z == 1) || (x == 3 && z == 3);
        }
    }
    StepRules rules;
    rules.jump = {1, 0.6f, 0.6f, false};
    CHECK(findPath(grid, {1, 1, 0}, {3, 3, 0}, rules).empty()); // only along the axes
    rules.jump.diagonal = true;
    const auto route = findRoute(grid, {1, 1, 0}, {3, 3, 0}, rules).steps;
    REQUIRE(route.size() == 1);
    CHECK(route[0].kind == StepKind::Jump);
    const auto cells = reachable(grid, {1, 1, 0}, 3, rules);
    const auto landing = std::find_if(cells.begin(), cells.end(),
                                      [](const ReachCell& c) { return c.pos == GridPos{3, 3, 0}; });
    REQUIRE(landing != cells.end());
    CHECK(landing->cost == doctest::Approx(2 * std::numbers::sqrt2_v<float>));
    // A pillar beside the line blocks it; one no higher than the ends does not.
    grid.at(2, 1).height = 1;
    CHECK(findPath(grid, {1, 1, 0}, {3, 3, 0}, rules).empty());
    grid.at(2, 1).height = 0;
    grid.at(3, 2).height = 1;
    CHECK(findPath(grid, {1, 1, 0}, {3, 3, 0}, rules).empty());
    grid.at(3, 2).height = 0;
    CHECK_FALSE(findPath(grid, {1, 1, 0}, {3, 3, 0}, rules).empty());
    // A raised landing may take a corner at its own height.
    grid.at(3, 3).height = grid.at(3, 2).height = 0.5f;
    CHECK_FALSE(findPath(grid, {1, 1, 0}, {3, 3, 0}, rules).empty());
}

TEST_CASE("ladders join cells of any height difference, both ways") {
    // A cliff: z = 0 is 4 units up; a ladder at (2, 1) climbs north onto it.
    Grid grid(5, 3, 1);
    for (int x = 0; x < 5; ++x) {
        grid.at(x, 0).height = 4;
    }
    grid.setLinks({{2, 1, 0, 0, {0, -1}, LinkKind::Ladder}});
    CHECK(grid.linkAt(2, 1, 0) == nullptr); // ladders never switch layers in place
    REQUIRE(grid.ladderStep({2, 1, 0}, {0, -1}));
    CHECK(grid.ladderStep({2, 0, 0}, {0, 1}));
    CHECK_FALSE(grid.ladderStep({2, 1, 0}, {0, 1}));
    StepRules rules;
    const auto up = findRoute(grid, {0, 2, 0}, {0, 0, 0}, rules).steps;
    REQUIRE_FALSE(up.empty());
    const auto climb =
        std::find_if(up.begin(), up.end(), [](const PathStep& s) { return s.kind == StepKind::Climb; });
    REQUIRE(climb != up.end());
    CHECK(climb->cell == GridPos{2, 0, 0});
    const auto down = findRoute(grid, {4, 0, 0}, {4, 2, 0}, rules).steps;
    REQUIRE_FALSE(down.empty());
    CHECK(std::any_of(down.begin(), down.end(), [](const PathStep& s) {
        return s.kind == StepKind::Climb && s.cell == GridPos{2, 1, 0};
    }));
    const auto copy = Grid::fromJson(grid.toJson());
    REQUIRE(copy.links().size() == 1);
    CHECK(copy.links()[0].kind == LinkKind::Ladder);
    CHECK(copy.links()[0].top() == GridPos{2, 0, 0});
    auto json = grid.toJson();
    json["links"][0].erase("dir");
    CHECK_THROWS(Grid::fromJson(json));
}

TEST_CASE("rules.ladders = false keeps paths and reach off ladders") {
    Grid grid(5, 3, 1);
    for (int x = 0; x < 5; ++x) {
        grid.at(x, 0).height = 4;
    }
    grid.setLinks({{2, 1, 0, 0, {0, -1}, LinkKind::Ladder}});
    StepRules rules;
    CHECK_FALSE(findRoute(grid, {0, 2, 0}, {0, 0, 0}, rules).steps.empty());
    const auto reachesTop = [&] {
        const auto cells = reachable(grid, {2, 1, 0}, 3, rules);
        return std::any_of(cells.begin(), cells.end(),
                           [](const ReachCell& c) { return c.pos == GridPos{2, 0, 0}; });
    };
    CHECK(reachesTop());
    rules.ladders = false;
    CHECK(findRoute(grid, {0, 2, 0}, {0, 0, 0}, rules).steps.empty());
    CHECK_FALSE(reachesTop());
}

TEST_CASE("an A* search stopped by its visit cap reports a truncated route") {
    Grid grid(40, 40, 1);
    StepRules rules;
    const auto full = findRoute(grid, {0, 0, 0}, {39, 39, 0}, rules);
    CHECK_FALSE(full.truncated);
    CHECK(full.steps.size() == 78);
    // Walls make the heuristic useless, so the search needs more than ten visits.
    for (int z = 0; z < 39; ++z) {
        grid.at(20, z).walkable = false;
    }
    const auto capped = findRoute(grid, {0, 0, 0}, {39, 0, 0}, rules, 10);
    CHECK(capped.truncated);
    CHECK(capped.steps.empty());
    const auto early = findRoute(Grid(3, 1, 1), {0, 0, 0}, {2, 0, 0}, rules, 1);
    CHECK(early.truncated);
    Grid closed(3, 1, 1);
    closed.at(1, 0).walkable = false;
    const auto none = findRoute(closed, {0, 0, 0}, {2, 0, 0}, rules);
    CHECK(none.steps.empty());
    CHECK_FALSE(none.truncated);
}

TEST_CASE("ramps climb by their rise") {
    Grid grid(3, 1, 1);
    grid.at(1, 0).ramp = Ramp::East;
    grid.at(1, 0).rise = 2;
    grid.at(2, 0).height = 2;
    CHECK(grid.heightAt({1.5f, 0.5f}) == doctest::Approx(1));
    CHECK(grid.edgeHeight({1, 0, 0}, 1, 0) == doctest::Approx(grid.edgeHeight({2, 0, 0}, -1, 0)));
    StepRules rules;
    CHECK(findPath(grid, {0, 0, 0}, {2, 0, 0}, rules).size() == 2);
    const auto copy = Grid::fromJson(grid.toJson());
    CHECK(copy.at(1, 0).rise == doctest::Approx(2));
    CHECK(copy.at(0, 0).rise == doctest::Approx(1));
    auto json = grid.toJson();
    json["layers"][0]["rise"] = {"1 -1 1"};
    CHECK_THROWS(Grid::fromJson(json));
}

TEST_CASE("grid raycasts can skip layers") {
    Grid grid(4, 4, 2);
    grid.at(1, 1, 1).walkable = true;
    grid.at(1, 1, 1).height = 2;
    const auto all = raycastGrid(grid, {1.5f, 10, 1.5f}, {0, -1, 0});
    REQUIRE(all);
    CHECK(all->cell.layer == 1);
    const auto ground = raycastGrid(grid, {1.5f, 10, 1.5f}, {0, -1, 0}, 200, 1u);
    REQUIRE(ground);
    CHECK(ground->cell.layer == 0);
}

TEST_CASE("cell coordinates of far-away and non-finite positions saturate instead of overflowing") {
    constexpr float Huge = 3.4e38f, Infinity = std::numeric_limits<float>::infinity();
    CHECK(floorCell(2.5f) == 2);
    CHECK(floorCell(-0.5f) == -1);
    CHECK(ceilCell(-0.5f) == 0);
    CHECK(floorCell(Huge) == CellLimit);
    CHECK(floorCell(-Huge) == -CellLimit);
    CHECK(floorCell(Infinity) == CellLimit);
    CHECK(floorCell(std::numeric_limits<float>::quiet_NaN()) == -CellLimit);
    CHECK(ceilCell(-Huge) == -CellLimit);
    const auto grid = arena();
    CHECK(grid.heightAt({Huge, -Huge}, 0) == 0);
    CHECK(grid.heightAt({std::numeric_limits<float>::quiet_NaN(), 1}, 0) == 0);
    CHECK_FALSE(grid.walkableAt({-Huge, Huge}, 0));
}

TEST_CASE("line of sight traces only the part of a segment over the grid") {
    // The reviewer's probe: a 2e9-unit line took 40 seconds cell by cell, and
    // 1e30 overflowed the cell count. Within the world bound scripts are held
    // to (1e6) the clipped trace is exact.
    Grid grid(10, 3, 1);
    CHECK(lineOfSight(grid, {-1e6f, 1, 1.5f}, {1e6f, 1, 1.5f}, 0));
    CHECK(lineOfSight(grid, {-1e6f, 1, -1e6f}, {1e6f, 1, 1e6f}, 0));
    grid.at(5, 1).height = 3;
    CHECK_FALSE(lineOfSight(grid, {-1e6f, 1, 1.5f}, {1e6f, 1, 1.5f}, 0));
    CHECK_FALSE(lineOfSight(grid, {-1e9f, 1, 1.5f}, {1e9f, 1, 1.5f}, 0));
    CHECK(lineOfSight(grid, {-1e6f, 4, 1.5f}, {1e6f, 4, 1.5f}, 0));
    // Far beyond it the answer is only as precise as the floats, but the
    // trace still ends at once.
    (void)lineOfSight(grid, {-1e30f, 1, -1e30f}, {1e30f, 1, 1e30f}, 0);
    (void)lineOfSight(grid, {-3.4e38f, 1, 1.5f}, {3.4e38f, 1, 1.5f}, 0);
    (void)lineOfSight(grid, {-3.4e38f, -3.4e38f, -3.4e38f}, {3.4e38f, 3.4e38f, 3.4e38f}, 0);
    // Lines that miss the grid see through.
    CHECK(lineOfSight(grid, {-1e9f, 1, 50}, {1e9f, 1, 50}, 0));
    CHECK(lineOfSight(grid, {-5, 1, -5}, {-1, 1, -1}, 0));
    // Clipping keeps the blocking test exact for a segment entering the grid.
    CHECK_FALSE(lineOfSight(grid, {-20, 1, 1.5f}, {8.5f, 1, 1.5f}, 0));
    CHECK(lineOfSight(grid, {-20, 4, 1.5f}, {8.5f, 4, 1.5f}, 0));
    CHECK_FALSE(lineOfSight(grid, {std::numeric_limits<float>::quiet_NaN(), 1, 1}, {1, 1, 1}, 0));
}
