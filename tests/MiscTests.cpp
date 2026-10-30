#include "engine/core/GameConfig.h"
#include "engine/platform/FileWatcher.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fe;

namespace {
void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text;
}
} // namespace

TEST_CASE("FileWatcher: survives unreadable folders without reporting removals") {
    const auto root = std::filesystem::temp_directory_path() / "fengine-watch-test";
    std::filesystem::remove_all(root);
    write(root / "a.txt", "a");
    write(root / "sub" / "b.txt", "b");
    FileWatcher watcher(root);
    CHECK(watcher.poll().empty());
    write(root / "a.txt", "changed");
    write(root / "c.txt", "c");
    auto changes = watcher.poll();
    REQUIRE(changes.size() == 2);
    std::filesystem::permissions(root / "sub", std::filesystem::perms::none);
    changes = watcher.poll();
    std::filesystem::permissions(root / "sub", std::filesystem::perms::owner_all);
    CHECK(changes.empty());
    std::filesystem::remove(root / "c.txt");
    changes = watcher.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].kind == FileChangeKind::Removed);
    CHECK(changes[0].path.filename() == "c.txt");
}

TEST_CASE("GameConfig: hdr_output and hdr_paper_white are read and checked") {
    const auto base = GameConfig::load(FENGINE_TEST_GAME);
    CHECK_FALSE(base.hdrOutput);
    CHECK(base.hdrPaperWhite == doctest::Approx(200));
    const auto root = std::filesystem::temp_directory_path() / "fengine-config-hdr";
    write(root / "game.json", R"({"title": "Bright", "hdr_output": true, "hdr_paper_white": 300})");
    const auto config = GameConfig::load(root);
    CHECK(config.hdrOutput);
    CHECK(config.hdrPaperWhite == doctest::Approx(300));
    write(root / "game.json", R"({"title": "Bright", "hdr_output": "yes"})");
    CHECK_THROWS_WITH(GameConfig::load(root), doctest::Contains("hdr_output"));
    write(root / "game.json", R"({"title": "Bright", "hdr_paper_white": 20})");
    CHECK_THROWS_WITH(GameConfig::load(root), doctest::Contains("80 to 10000"));
    write(root / "game.json", R"({"title": "Bright", "hdr_paper_white": "bright"})");
    CHECK_THROWS_WITH(GameConfig::load(root), doctest::Contains("hdr_paper_white"));
}

TEST_CASE("GameConfig: game.json sets whether the game pauses when the window loses focus") {
    CHECK(GameConfig::load(FENGINE_TEST_GAME).pauseOnFocusLoss);
    const auto root = std::filesystem::temp_directory_path() / "fengine-config-focus";
    write(root / "game.json", R"({"title": "Focus", "pause_on_focus_loss": false})");
    CHECK_FALSE(GameConfig::load(root).pauseOnFocusLoss);
}
