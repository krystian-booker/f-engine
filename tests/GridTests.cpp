#include "engine/nav/Query.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

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

TEST_CASE("A* breaks ties on the cell key") {
    Grid grid(5, 5, 1);
    StepRules rules;
    const auto first = findPath(grid, {0, 0, 0}, {2, 2, 0}, rules);
    // Every 4-step route is equally short; lower keys (z, then x) are expanded first.
    const std::vector<GridPos> expected{{1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}};
    CHECK(first == expected);
    CHECK(findPath(grid, {0, 0, 0}, {2, 2, 0}, rules) == first);
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
    // Along the road the whole row is a plain four-step route.
    CHECK(findPath(grid, {0, 2, 0}, {4, 2, 0}, rules).size() == 4);
    rules.cost = [](GridPos, GridPos, float) -> std::optional<float> {
        return -1.0f;
    };
    CHECK_THROWS_AS(findPath(grid, {0, 0, 0}, {3, 0, 0}, rules), std::invalid_argument);
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

TEST_CASE("rules.ladders = false keeps paths off ladders") {
    Grid grid(5, 3, 1);
    for (int x = 0; x < 5; ++x) {
        grid.at(x, 0).height = 4;
    }
    grid.setLinks({{2, 1, 0, 0, {0, -1}, LinkKind::Ladder}});
    StepRules rules;
    CHECK_FALSE(findRoute(grid, {0, 2, 0}, {0, 0, 0}, rules).steps.empty());
    const auto reachesTop = [&] {
        return !findRoute(grid, {2, 1, 0}, {2, 0, 0}, rules).steps.empty();
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

TEST_CASE("walk rows accept only 1, 0 and #, and tag masks must fit the tag list") {
    Grid grid(3, 1, 1);
    grid.at(1, 0).walkable = false;
    auto json = grid.toJson();
    json["layers"][0]["walk"] = {"1#0"};
    CHECK_FALSE(Grid::fromJson(json).at(1, 0).walkable);
    json["layers"][0]["walk"] = {"1x0"};
    CHECK_THROWS_WITH(Grid::fromJson(json), doctest::Contains("walk rows use 1, 0 or #"));
    json["layers"][0]["walk"] = {"110"};
    json["tags"] = {"water"};
    json["layers"][0]["tags"] = {"0 1 0"};
    CHECK(Grid::fromJson(json).at(1, 0).tags == 1);
    json["layers"][0]["tags"] = {"0 2 0"};
    CHECK_THROWS_WITH(Grid::fromJson(json), doctest::Contains("tag bits"));
    json.erase("tags");
    json["layers"][0]["tags"] = {"0 1 0"};
    CHECK_THROWS(Grid::fromJson(json));
}

TEST_CASE("a footprint wider than one cell needs every cell of it, and skips ladders") {
    // A corridor three cells deep with a pillar at (3, 1).
    Grid grid(6, 3, 1);
    grid.at(3, 1).walkable = false;
    StepRules rules;
    CHECK_FALSE(findPath(grid, {0, 0, 0}, {5, 0, 0}, rules).empty());
    rules.footprint = 2;
    CHECK(findPath(grid, {0, 0, 0}, {4, 0, 0}, rules).empty()); // rows 0-1 and 1-2 both meet the pillar
    CHECK(findPath(grid, {0, 0, 0}, {4, 2, 0}, rules).empty()); // the goal hangs over the edge
    grid.at(3, 1).walkable = true;
    const auto path = findPath(grid, {0, 0, 0}, {4, 0, 0}, rules);
    REQUIRE_FALSE(path.empty());
    for (const auto& cell : path) {
        CHECK(cell.z <= 1);
    }
    CHECK(findPath(grid, {0, 0, 0}, {5, 0, 0}, rules).empty()); // the body would hang past x = 5
    CHECK(findPath(grid, {0, 0, 0}, {0, 2, 0}, rules).empty()); // and past z = 2
    CHECK(grid.canStand({2.5f, 0.5f}, 0.2f, 0, 2));
    CHECK(grid.canStand({4.5f, 1.5f}, 0.2f, 0, 2));       // cells (4..5, 1..2) all exist
    CHECK_FALSE(grid.canStand({4.5f, 2.5f}, 0.2f, 0, 2)); // the second row hangs over the edge
    CHECK_FALSE(grid.canStand({5.5f, 0.5f}, 0.2f, 0, 2));
    rules.footprint = 0;
    CHECK_THROWS_AS(validateRules(rules), std::invalid_argument);
    rules.footprint = MaxFootprint + 1;
    CHECK_THROWS_AS(validateRules(rules), std::invalid_argument);
    // A ladder up a cliff carries one-cell bodies only.
    Grid cliff(5, 3, 1);
    for (int x = 0; x < 5; ++x) {
        cliff.at(x, 0).height = 4;
    }
    cliff.setLinks({{2, 1, 0, 0, {0, -1}, LinkKind::Ladder}});
    rules.footprint = 1;
    CHECK_FALSE(findRoute(cliff, {0, 2, 0}, {0, 0, 0}, rules).steps.empty());
    rules.footprint = 2;
    CHECK(findRoute(cliff, {0, 1, 0}, {0, 0, 0}, rules).steps.empty());
}

TEST_CASE("flying rules cross unwalkable cells and height steps but not blocked tags") {
    Grid grid(8, 3, 1);
    for (int z = 0; z < 3; ++z) {
        grid.at(3, z).walkable = false;
        grid.at(5, z).height = 3;
    }
    const auto water = grid.tagBit("water", true);
    StepRules rules;
    CHECK(findPath(grid, {0, 1, 0}, {7, 1, 0}, rules).empty());
    rules.fly = true;
    const auto path = findPath(grid, {0, 1, 0}, {7, 1, 0}, rules);
    REQUIRE(path.size() == 7);
    CHECK(std::find(path.begin(), path.end(), GridPos{3, 1, 0}) != path.end());
    CHECK(std::find(path.begin(), path.end(), GridPos{5, 1, 0}) != path.end());
    CHECK(grid.canStand({3.5f, 1.5f}, 0.2f, 0, 1, true));
    CHECK_FALSE(grid.canStand({3.5f, 1.5f}, 0.2f, 0));
    CHECK_FALSE(grid.canStand({8.5f, 1.5f}, 0.2f, 0, 1, true));
    for (int z = 0; z < 3; ++z) {
        grid.at(3, z).tags = water;
    }
    rules.blockedTags = water;
    CHECK(findPath(grid, {0, 1, 0}, {7, 1, 0}, rules).empty());
    CHECK(findPath(grid, {0, 1, 0}, {2, 1, 0}, rules).size() == 2); // up to the water's edge
    CHECK(findPath(grid, {0, 1, 0}, {4, 1, 0}, rules).empty());     // nothing crosses it
}

TEST_CASE("a search to a goal that cannot be entered ends without expanding anything") {
    Grid grid(20, 20, 1);
    const auto lava = grid.tagBit("lava", true);
    grid.at(19, 19).tags = lava;
    StepRules rules;
    rules.blockedTags = lava;
    int calls = 0;
    rules.cost = [&](GridPos, GridPos, float base) -> std::optional<float> {
        ++calls;
        return base;
    };
    CHECK(findRoute(grid, {0, 0, 0}, {19, 19, 0}, rules).steps.empty());
    CHECK(calls == 0);
    rules.blockedTags = 0;
    rules.blocked.insert(cellKey({19, 19, 0}));
    CHECK(findRoute(grid, {0, 0, 0}, {19, 19, 0}, rules).steps.empty());
    CHECK(calls == 0);
    rules.blocked.clear();
    CHECK_FALSE(findRoute(grid, {0, 0, 0}, {19, 19, 0}, rules).steps.empty());
    CHECK(calls > 0);
}

TEST_CASE("required and blocked tags are a passability class for standing and planning") {
    // A river of water cells down the middle, a lava cell on the far bank.
    Grid grid(8, 3, 1);
    const auto water = grid.tagBit("water", true);
    const auto lava = grid.tagBit("lava", true);
    for (int z = 0; z < 3; ++z) {
        grid.at(3, z).tags = grid.at(4, z).tags = water;
    }
    grid.at(6, 1).tags = lava;
    // A boat (flying, water only) stands on water alone; a walker stands anywhere but lava.
    CHECK(grid.canStand({3.5f, 1.5f}, 0.2f, 0, 1, true, 0, water));
    CHECK_FALSE(grid.canStand({2.5f, 1.5f}, 0.2f, 0, 1, true, 0, water));
    CHECK_FALSE(grid.canStand({3.4f, 1.5f}, 0.49f, 0, 1, true, 0, water)); // its disc touches the bank
    CHECK(grid.canStand({6.5f, 1.5f}, 0.2f, 0));
    CHECK_FALSE(grid.canStand({6.5f, 1.5f}, 0.2f, 0, 1, false, lava));
    CHECK_FALSE(grid.canStand({6.5f, 1.5f}, 0.2f, 0, 2, false, lava)); // one cell of the footprint is lava
    StepRules boat;
    boat.fly = true;
    boat.requiredTags = water;
    const auto along = findPath(grid, {3, 0, 0}, {4, 2, 0}, boat);
    REQUIRE_FALSE(along.empty());
    for (const auto& cell : along) {
        CHECK((grid.at(cell).tags & water) != 0u);
    }
    CHECK(findPath(grid, {3, 0, 0}, {5, 2, 0}, boat).empty()); // the goal is land
    // Either tag of a required set will do.
    boat.requiredTags = water | lava;
    CHECK(findPath(grid, {3, 1, 0}, {6, 1, 0}, boat).empty()); // (5, 1) is plain land in between
    boat.jump = {1, 0, 0, false};
    CHECK_FALSE(findPath(grid, {3, 1, 0}, {6, 1, 0}, boat).empty()); // a tagless cell is a gap to jump
    StepRules walker;
    walker.blockedTags = lava;
    const auto around = findPath(grid, {0, 1, 0}, {7, 1, 0}, walker);
    REQUIRE_FALSE(around.empty());
    CHECK(std::find(around.begin(), around.end(), GridPos{6, 1, 0}) == around.end());
}

TEST_CASE("a goal in another connected component is rejected before the search expands anything") {
    // A wall splits the grid; the goal is behind it.
    Grid grid(20, 20, 1);
    for (int z = 0; z < 20; ++z) {
        grid.at(10, z).walkable = false;
    }
    StepRules rules;
    int calls = 0;
    rules.cost = [&](GridPos, GridPos, float base) -> std::optional<float> {
        ++calls;
        return base;
    };
    CHECK(findRoute(grid, {0, 0, 0}, {19, 19, 0}, rules).steps.empty());
    CHECK(calls == 0);
    CHECK(grid.component({0, 0, 0}) != grid.component({19, 19, 0}));
    CHECK(grid.component({10, 5, 0}) == -1);
    CHECK(grid.component({20, 0, 0}) == -1);
    // Opening the wall is seen whichever way the cell is written.
    grid.at(10, 7).walkable = true;
    CHECK_FALSE(findRoute(grid, {0, 0, 0}, {19, 19, 0}, rules).steps.empty());
    CHECK(calls > 0);
    auto closed = grid.at(10, 7);
    closed.walkable = false;
    grid.setCell({10, 7, 0}, closed);
    calls = 0;
    CHECK(findRoute(grid, {0, 0, 0}, {19, 19, 0}, rules).steps.empty());
    CHECK(calls == 0);
    // Writing the same cell again changes nothing; a change bumps the revision.
    const auto revision = grid.revision();
    grid.setCell({10, 7, 0}, closed);
    CHECK(grid.revision() == revision);
    grid.setCell({10, 8, 0}, GridCell{});
    CHECK(grid.revision() > revision);
    CHECK(grid.component({0, 0, 0}) == grid.component({19, 19, 0}));
    // Flying and gap jumps leave a component, so they search as usual.
    grid.setCell({10, 8, 0}, closed);
    StepRules fly = rules;
    fly.fly = true;
    calls = 0;
    CHECK_FALSE(findRoute(grid, {0, 0, 0}, {19, 19, 0}, fly).steps.empty());
    CHECK(calls > 0);
    StepRules hop = rules;
    hop.jump = {1, 0, 0, false};
    calls = 0;
    CHECK_FALSE(findRoute(grid, {0, 0, 0}, {19, 19, 0}, hop).steps.empty());
    CHECK(calls > 0);
    // A start that is not walkable itself has no component and still searches.
    grid.at(0, 0).walkable = false;
    grid.setCell({10, 8, 0}, GridCell{});
    calls = 0;
    CHECK_FALSE(findRoute(grid, {0, 0, 0}, {19, 19, 0}, rules).steps.empty());
    CHECK(calls > 0);
}

TEST_CASE("links and ladders join components; a cell edit splits them again") {
    // Two islands on the ground joined by nothing but a bridge layer, then a ladder.
    Grid grid(7, 3, 2);
    for (int z = 0; z < 3; ++z) {
        grid.at(3, z).walkable = false;
    }
    CHECK(grid.component({0, 1, 0}) != grid.component({6, 1, 0}));
    for (int x = 2; x <= 4; ++x) {
        grid.at(x, 1, 1) = {0, true, Ramp::None, 1};
    }
    grid.setLinks({{2, 1, 0, 1, {1, 0}}, {4, 1, 1, 0, {1, 0}}});
    CHECK(grid.component({0, 1, 0}) == grid.component({6, 1, 0}));
    CHECK(grid.component({3, 1, 1}) == grid.component({0, 1, 0}));
    StepRules rules;
    int calls = 0;
    rules.cost = [&](GridPos, GridPos, float base) -> std::optional<float> {
        ++calls;
        return base;
    };
    CHECK_FALSE(findRoute(grid, {0, 1, 0}, {6, 1, 0}, rules).steps.empty());
    CHECK(calls > 0);
    // Breaking the deck splits them; a ladder up the gap joins them again.
    grid.at(3, 1, 1).walkable = false;
    calls = 0;
    CHECK(findRoute(grid, {0, 1, 0}, {6, 1, 0}, rules).steps.empty());
    CHECK(calls == 0);
    CHECK(grid.component({0, 1, 0}) != grid.component({6, 1, 0}));
    grid.setLinks({{2, 1, 0, 0, {1, 0}, LinkKind::Ladder}}); // foot (2, 1), top (3, 1): still unwalkable
    CHECK(grid.component({0, 1, 0}) != grid.component({6, 1, 0}));
    grid.setLinks({{2, 1, 0, 1, {1, 0}, LinkKind::Ladder}, {4, 1, 1, 0, {1, 0}}}); // up onto the deck's end
    grid.at(3, 1, 1).walkable = true;
    grid.at(2, 1, 1).walkable = false;
    CHECK(grid.component({0, 1, 0}) == grid.component({6, 1, 0}));
    calls = 0;
    const auto route = findRoute(grid, {0, 1, 0}, {6, 1, 0}, rules);
    REQUIRE_FALSE(route.steps.empty());
    CHECK(std::any_of(route.steps.begin(), route.steps.end(),
                      [](const PathStep& s) { return s.kind == StepKind::Climb; }));
    // Components survive copies, revision and all.
    const Grid copy = grid;
    CHECK(copy.component({0, 1, 0}) == copy.component({6, 1, 0}));
    CHECK(copy.revision() == grid.revision());
}

TEST_CASE("a wrapping grid folds cells, points and differences round the map") {
    Grid grid(10, 6, 1);
    // Not wrapping: every helper is the identity and the edges are edges.
    CHECK_FALSE(grid.wraps());
    CHECK(grid.wrapCell({-1, 7, 0}) == GridPos{-1, 7, 0});
    CHECK(grid.wrapPoint({-0.5f, 6.5f}) == glm::vec2(-0.5f, 6.5f));
    CHECK(grid.delta({9.5f, 0.5f}, {0.5f, 0.5f}) == glm::vec2(-9, 0));
    CHECK_FALSE(grid.contains(-1, 0));
    CHECK_FALSE(grid.contains(10, 0));
    CHECK_THROWS_AS((void)grid.at(-1, 0), std::out_of_range);
    CHECK_FALSE(grid.canStand({-0.5f, 1.5f}));
    CHECK(grid.heightAt({-0.5f, 1.5f}) == 0);
    const auto revision = grid.revision();
    grid.setWrap(true);
    CHECK(grid.wraps());
    CHECK(grid.revision() > revision);
    CHECK(grid.wrapCell({-1, 7, 0}) == GridPos{9, 1, 0});
    CHECK(grid.wrapCell({10, -1, 2}) == GridPos{0, 5, 2});
    CHECK(grid.wrapPoint({-0.5f, 6.5f}) == glm::vec2(9.5f, 0.5f));
    CHECK(grid.wrapPoint({10.25f, -0.25f}) == glm::vec2(0.25f, 5.75f));
    CHECK(grid.wrapPoint({-1e-9f, 3}) == glm::vec2(0, 3)); // never `width` itself
    CHECK(grid.delta({9.5f, 0.5f}, {0.5f, 0.5f}) == glm::vec2(1, 0));
    CHECK(grid.delta({0.5f, 0.5f}, {9.5f, 0.5f}) == glm::vec2(-1, 0));
    CHECK(grid.delta({0.5f, 0.5f}, {0.5f, 5.5f}) == glm::vec2(0, -1));
    CHECK(grid.delta({2, 2}, {4, 3}) == glm::vec2(2, 1));
    CHECK(wrapDifference(5, 10) == -5); // half way round: the range is [-size/2, size/2)
    CHECK(wrapDifference(-5, 10) == -5);
    // Lookups wrap; only a bad layer is outside.
    CHECK(grid.contains(-1, 0));
    CHECK(grid.contains(10, 60));
    CHECK_FALSE(grid.contains(0, 0, 1));
    CHECK(grid.inRange(9, 5));
    CHECK_FALSE(grid.inRange(10, 0));
    CHECK_FALSE(grid.inRange(-1, 0));
    grid.at(9, 1).height = 2;
    grid.at(9, 2).ramp = Ramp::East;
    CHECK(grid.heightAt({-0.5f, 1.5f}) == doctest::Approx(2));
    CHECK(grid.heightAt({10.5f, 1.5f}) == doctest::Approx(0));
    CHECK(grid.heightAt({-0.25f, 2.5f}) == doctest::Approx(0.75f)); // three quarters up the ramp
    CHECK(std::as_const(grid).at(-1, 1).height == doctest::Approx(2));
    CHECK(std::as_const(grid).at(19, 7).height == doctest::Approx(2));
    grid.at(9, 3).walkable = false;
    CHECK_FALSE(grid.canStand({-0.5f, 3.5f}, 0.2f));
    CHECK_FALSE(grid.canStand({0.1f, 3.5f}, 0.2f)); // the disc crosses the seam into the wall
    CHECK(grid.canStand({0.1f, 4.5f}, 0.2f));
    CHECK(grid.canStand({-0.5f, 4.5f}, 0.2f));
    grid.setCellProps({-1, 4, 0}, {{"note", "west"}});
    REQUIRE(grid.cellProps({9, 4, 0}));
    CHECK(grid.cellProps({9, 4, 0})->at("note") == "west");
    CHECK(grid.cellProps({19, 4, 0}) == grid.cellProps({9, 4, 0}));
    // The flag survives the file and the file keeps its validation strict.
    const auto copy = Grid::fromJson(grid.toJson());
    CHECK(copy.wraps());
    CHECK(copy.heightAt({-0.5f, 1.5f}) == doctest::Approx(2));
    auto json = grid.toJson();
    json["cell_props"] = Json::array({{{"x", 10}, {"z", 0}, {"props", {{"a", 1}}}}});
    CHECK_THROWS(Grid::fromJson(json));
    json = grid.toJson();
    json["layers"].push_back({{"name", "top"}});
    Json ladder{{"x", 9}, {"z", 0}, {"from", 0}, {"to", 1}, {"kind", "ladder"}, {"dir", "e"}};
    json["links"] = Json::array({ladder});
    CHECK_THROWS(Grid::fromJson(json)); // a ladder may not straddle the seam
    ladder["x"] = 8;
    json["links"] = Json::array({ladder});
    CHECK(Grid::fromJson(json).links().size() == 1);
}

TEST_CASE("paths and components cross a wrapping grid's seam") {
    Grid grid(10, 6, 1);
    StepRules rules;
    // Flat: the long way round is the only way.
    CHECK(findPath(grid, {9, 1, 0}, {2, 1, 0}, rules).size() == 7);
    CHECK(findPath(grid, {9, 1, 0}, {-8, 1, 0}, rules).empty()); // outside a plain grid
    grid.setWrap(true);
    auto path = findPath(grid, {9, 1, 0}, {2, 1, 0}, rules);
    REQUIRE(path.size() == 3);
    CHECK(path[0] == GridPos{0, 1, 0});
    CHECK(path[2] == GridPos{2, 1, 0});
    // Any representative of a cell is that cell, and the route comes back canonical.
    path = findPath(grid, {-1, 7, 0}, {-8, 1, 0}, rules);
    REQUIRE(path.size() == 3);
    CHECK(path[2] == GridPos{2, 1, 0});
    CHECK(findPath(grid, {9, 1, 0}, {2, 1, 0}, rules).size() == 3);
    StepRules diagonal = rules;
    diagonal.diagonal = true;
    path = findPath(grid, {9, 0, 0}, {1, 2, 0}, diagonal);
    REQUIRE(path.size() == 2); // two diagonal steps over the corner of the seam
    CHECK(path[0] == GridPos{0, 1, 0});
    // One step over the seam, keyed on the canonical cell.
    path = findPath(grid, {0, 1, 0}, {9, 1, 0}, rules);
    REQUIRE(path.size() == 1);
    CHECK(path[0] == GridPos{9, 1, 0});
    // Occupancy across the seam counts: a body on (9, 1) stops the short route.
    StepRules occupied = rules;
    occupied.blocked.insert(cellKey({9, 1, 0}));
    CHECK(findPath(grid, {1, 1, 0}, {8, 1, 0}, occupied).size() > 3);
    // A wall down x = 0 forces the long way; a gap jump clears it.
    for (int z = 0; z < 6; ++z) {
        grid.at(0, z).walkable = false;
    }
    CHECK(findPath(grid, {9, 1, 0}, {2, 1, 0}, rules).size() == 7);
    StepRules hop = rules;
    hop.jump = {1, 0, 0, false};
    const auto route = findRoute(grid, {9, 1, 0}, {1, 1, 0}, hop);
    REQUIRE(route.steps.size() == 1);
    CHECK(route.steps[0].cell == GridPos{1, 1, 0});
    CHECK(route.steps[0].kind == StepKind::Jump);
    // Components join across the seam and lookups of any representative agree.
    for (int z = 0; z < 6; ++z) {
        grid.at(0, z).walkable = true;
        grid.at(5, z).walkable = false;
    }
    CHECK(grid.component({1, 1, 0}) == grid.component({9, 1, 0}));
    CHECK(grid.component({-1, 1, 0}) == grid.component({9, 1, 0}));
    CHECK(grid.component({5, 1, 0}) == -1);
    grid.setWrap(false);
    CHECK(grid.component({1, 1, 0}) != grid.component({9, 1, 0}));
    CHECK(grid.component({-1, 1, 0}) == -1);
}
