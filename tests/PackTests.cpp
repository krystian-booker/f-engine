#include "engine/core/AssetCache.h"
#include "engine/core/Pack.h"
#include "engine/core/Paths.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include <doctest/doctest.h>
#include <fstream>
#include <random>

using namespace fe;

namespace {
void write(const std::filesystem::path& file, std::string_view bytes) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string slurp(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

std::string noise(std::size_t size) {
    std::mt19937 random(7);
    std::string bytes(size, '\0');
    for (auto& byte : bytes) {
        byte = static_cast<char>(random() & 0xffu);
    }
    return bytes;
}

std::uint32_t crc32(std::string_view bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const char byte : bytes) {
        crc ^= static_cast<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
        }
    }
    return crc ^ 0xffffffffu;
}

// A fresh scratch folder; packs mounted by a test are unmounted afterwards.
struct Scratch {
    std::filesystem::path root;
    explicit Scratch(const std::string& name)
        : root(std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
               ("fengine-pack-" + name)) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~Scratch() {
        unmountPacks();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
} // namespace

TEST_CASE("pak archives round-trip files and compress only when it pays") {
    Scratch scratch("roundtrip");
    const auto archive = scratch.root / "data.pak";
    const std::string text(4000, 'a');
    const auto random = noise(3000);
    {
        PackWriter writer(archive);
        writer.add("maps/town/map.json", text);
        writer.add("audio/noise.bin", random);
        writer.add("ui/icon.png", text);
        writer.add("empty.txt", "");
        writer.add("main.lua", "return {}");
        CHECK_THROWS(writer.add("main.lua", "again"));
        writer.finish();
    }
    CHECK_FALSE(std::filesystem::exists(scratch.root / "data.pak.tmp"));
    const PackArchive pak(archive);
    REQUIRE(pak.entries().size() == 5);
    const auto* map = pak.find("maps/town/map.json");
    REQUIRE(map);
    CHECK(map->method == 1);
    CHECK(map->storedSize < map->size);
    CHECK(pak.read(*map) == text);
    CHECK(pak.find("audio/noise.bin")->method == 0);
    CHECK(pak.read(*pak.find("audio/noise.bin")) == random);
    CHECK(pak.find("ui/icon.png")->method == 0);
    CHECK(pak.read(*pak.find("empty.txt")).empty());
    CHECK_FALSE(pak.find("maps/town"));
    CHECK(pak.hasFolder(""));
    CHECK(pak.hasFolder("maps/town"));
    CHECK_FALSE(pak.hasFolder("map"));
    CHECK(pak.list("") == std::vector<std::string>{"audio/", "empty.txt", "main.lua", "maps/", "ui/"});
    CHECK(pak.list("maps") == std::vector<std::string>{"town/"});
    CHECK(pak.list("maps/town/") == std::vector<std::string>{"map.json"});
    CHECK(pak.list("missing").empty());
}

TEST_CASE("pak names are relative paths that cannot leave their folder") {
    for (const char* bad : {"", "../x", "/abs", "a/../b", "a/./b", "a\\b", "C:x", "a//b", "a/", ".."}) {
        CAPTURE(bad);
        CHECK_FALSE(validPackName(bad));
    }
    CHECK(validPackName("a/b.c"));
    CHECK(validPackName("..hidden/x"));
    Scratch scratch("names");
    PackWriter writer(scratch.root / "bad.pak");
    CHECK_THROWS(writer.add("../escape.lua", "x"));
    CHECK_THROWS(writer.add("/etc/passwd", "x"));
}

TEST_CASE("pak readers reject traversal names, damage and foreign files") {
    Scratch scratch("damaged");
    const auto archive = scratch.root / "data.pak";
    {
        PackWriter writer(archive);
        writer.add("xx/evil.lua", std::string(500, 'z'));
        writer.finish();
    }
    const auto good = slurp(archive);
    const auto indexOffset = 32 + PackArchive(archive).entries()[0].storedSize;

    // A hand-made index naming "../evil.lua", with a correct checksum.
    auto traversal = good;
    traversal.replace(indexOffset + 2, 2, "..");
    const auto index = std::string_view(traversal).substr(indexOffset);
    const auto crc = crc32(index);
    for (int i = 0; i < 4; ++i) {
        traversal[28 + i] = static_cast<char>((crc >> (8 * i)) & 0xffu);
    }
    write(archive, traversal);
    CHECK_THROWS_WITH(PackArchive{archive}, doctest::Contains("unsafe entry name"));

    auto corruptIndex = good;
    corruptIndex[indexOffset + 3] ^= 1;
    write(archive, corruptIndex);
    CHECK_THROWS_WITH(PackArchive{archive}, doctest::Contains("checksum"));

    auto corruptData = good;
    corruptData[40] ^= 1;
    write(archive, corruptData);
    const PackArchive damaged(archive);
    CHECK_THROWS(static_cast<void>(damaged.read(damaged.entries()[0])));

    write(archive, "PK\x03\x04 not a pak at all, just some bytes");
    CHECK_THROWS_WITH(PackArchive{archive}, doctest::Contains("not a pak"));
    write(archive, good.substr(0, good.size() - 3));
    CHECK_THROWS(PackArchive{archive});
}

TEST_CASE("a mounted pak stands in for its folder") {
    Scratch scratch("mount");
    write(scratch.root / "source" / "game.json", "{}");
    write(scratch.root / "source" / "maps" / "a" / "map.json", "{\"id\": \"a\"}");
    write(scratch.root / "source" / "sounds" / "step.wav", "RIFF....");
    write(scratch.root / "secret.txt", "outside");
    CHECK(packFolder(scratch.root / "source", scratch.root / "dist" / "game.pak") == 3);
    const auto game = scratch.root / "dist" / "game";
    CHECK_FALSE(isFile(game / "game.json"));
    CHECK(openGame(scratch.root / "dist" / "game.pak") == game);
    CHECK(isFile(game / "game.json"));
    CHECK(isFolder(game));
    CHECK(isFolder(game / "maps" / "a"));
    CHECK_FALSE(isFile(game / "maps" / "a"));
    CHECK_FALSE(isFolder(game / "maps" / "b"));
    CHECK(readFile(game / "maps" / "a" / "map.json") == "{\"id\": \"a\"}");
    CHECK(readJson(resolveInside(game, "maps/a/map.json")).at("id") == "a");
    CHECK(listFolder(game) == std::vector<std::string>{"game.json", "maps/", "sounds/"});
    CHECK(listFilesBelow(game) ==
          std::vector<std::string>{"game.json", "maps/a/map.json", "sounds/step.wav"});
    CHECK_THROWS(static_cast<void>(resolveInside(game, "../secret.txt")));
    CHECK_THROWS(static_cast<void>(resolveInside(game, "maps/../../secret.txt")));
    CHECK_THROWS(static_cast<void>(readFile(game / "missing.json")));

    const auto extracted = localFile(game / "sounds" / "step.wav");
    CHECK(extracted != game / "sounds" / "step.wav");
    CHECK(extracted.filename() == "step.wav");
    CHECK(slurp(extracted) == "RIFF....");
    CHECK(localFile(game / "sounds" / "step.wav") == extracted);
    CHECK(localFile(scratch.root / "secret.txt") == scratch.root / "secret.txt");

    CHECK(mountFolderPack(game));
    CHECK_FALSE(mountFolderPack(scratch.root / "source"));
}

TEST_CASE("loose files win in development mounts and the archive in ship mounts") {
    Scratch scratch("precedence");
    write(scratch.root / "source" / "data.txt", "packed");
    write(scratch.root / "source" / "only_packed.txt", "packed");
    Image::solid(1, 1, 1).savePng(scratch.root / "source" / "art.png");
    packFolder(scratch.root / "source", scratch.root / "game.pak");
    const auto game = scratch.root / "game";
    write(game / "data.txt", "loose");
    write(game / "only_loose.txt", "loose");
    Image::solid(2, 2, 2).savePng(game / "art.png");

    mountPack(scratch.root / "game.pak", true);
    CHECK(readFile(game / "data.txt") == "loose");
    CHECK(localFile(game / "data.txt") == game / "data.txt");
    CHECK(readFile(game / "only_packed.txt") == "packed");
    CHECK(readFile(game / "only_loose.txt") == "loose");
    CHECK(listFolder(game) ==
          std::vector<std::string>{"art.png", "data.txt", "only_loose.txt", "only_packed.txt"});
    CHECK(AssetCache(game, FENGINE_LUALIB).image("art.png")->rgba[0] == 2);

    mountPack(scratch.root / "game.pak", false);
    CHECK(readFile(game / "data.txt") == "packed");
    CHECK(slurp(localFile(game / "data.txt")) == "packed");
    CHECK(AssetCache(game, FENGINE_LUALIB).image("art.png")->rgba[0] == 1);
    CHECK(readFile(game / "only_loose.txt") == "loose");

    unmountPacks();
    CHECK(readFile(game / "data.txt") == "loose");
    CHECK_FALSE(isFile(game / "only_packed.txt"));
}

TEST_CASE("the asset cache, scripts and fe.fs read a game from its pak") {
    Scratch scratch("engine");
    const auto source = scratch.root / "source";
    std::filesystem::create_directories(source);
    std::filesystem::copy(std::filesystem::path(FENGINE_TEST_GAME) / "maps", source / "maps",
                          std::filesystem::copy_options::recursive);
    std::filesystem::copy_file(std::filesystem::path(FENGINE_TEST_GAME) / "game.json", source / "game.json");
    write(source / "main.lua", "return {helper = require('scripts.helper').name}");
    write(source / "scripts" / "helper.lua", "return {name = 'from pak'}");
    write(source / "data" / "items.json", "{\"potion\": 3}");
    Image::solid(10, 20, 30).savePng(source / "art" / "dot.png");
    write(source / "art" / "dot.anim.json",
          R"({"atlas": "dot.png", "clips": {"idle": {"directions": [[{"rect": [0, 0, 1, 1]}]]}}})");
    packFolder(source, scratch.root / "dist" / "game.pak");
    const auto game = openGame(scratch.root / "dist" / "game.pak");
    REQUIRE_FALSE(std::filesystem::exists(game));

    AssetCache assets(game, FENGINE_LUALIB);
    CHECK(assets.image("art/dot.png")->rgba[1] == 20);
    CHECK(assets.sprite("art/dot.anim.json")->atlas == "dot.png");
    CHECK_THROWS(assets.image("../source/art/dot.png"));

    EngineOptions options;
    options.gameRoot = game;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = scratch.root / "user";
    options.seed = 1;
    Engine engine(std::move(options));
    REQUIRE(engine.start());
    auto& lua = engine.lua();
    CHECK(lua.require(engine.config().entry).as<sol::table>().get<std::string>("helper") == "from pak");
    CHECK(lua.console("fe.fs.read('data/items.json')") == "{\"potion\": 3}");
    CHECK(lua.console("fe.fs.exists('data/items.json')") == "true");
    CHECK(lua.console("fe.fs.exists('data')") == "true");
    CHECK(lua.console("fe.fs.exists('nothing.txt')") == "false");
    CHECK(lua.console("table.concat(fe.fs.list('data'), ',')") == "items.json");
    CHECK(lua.console("table.concat(fe.fs.list(''), ',')") == "art/,data/,game.json,main.lua,maps/,scripts/");
    CHECK_FALSE(lua.evaluate("fe.fs.read('../game.pak')").ok);
    REQUIRE(engine.loadWorld("main", "arena"));
}

TEST_CASE("content packs lay over the game in name order, user packs last, damaged ones skipped") {
    Scratch scratch("overlay");
    const auto source = scratch.root / "source";
    std::filesystem::create_directories(source);
    std::filesystem::copy(std::filesystem::path(FENGINE_TEST_GAME) / "maps", source / "maps",
                          std::filesystem::copy_options::recursive);
    std::filesystem::copy_file(std::filesystem::path(FENGINE_TEST_GAME) / "game.json", source / "game.json");
    write(source / "main.lua", "return {}");
    write(source / "data" / "items.json", "{\"from\": \"game\"}");
    write(source / "data" / "base_only.json", "{}");
    write(source / "scripts" / "greeting.lua", "return 'game'");
    // A second map that only pack b supplies: arena's files under a new id.
    const auto arena = std::filesystem::path(FENGINE_TEST_GAME) / "maps" / "arena";
    auto arena2 = readJson(arena / "map.json");
    arena2["id"] = "arena2";

    const auto user = scratch.root / "user";
    {
        PackWriter a(source / "packs" / "a.pak");
        a.add("data/items.json", "{\"from\": \"a\"}");
        a.add("data/a_only.json", "{}");
        a.add("scripts/greeting.lua", "return 'a'");
        a.finish();
        PackWriter b(source / "packs" / "b.pak");
        b.add("data/items.json", "{\"from\": \"b\"}");
        b.add("scripts/greeting.lua", "return 'b'");
        b.add("maps/arena2/map.json", arena2.dump());
        for (const auto& entry : std::filesystem::directory_iterator(arena)) {
            if (entry.path().filename() != "map.json") {
                b.add("maps/arena2/" + entry.path().filename().string(), slurp(entry.path()));
            }
        }
        b.finish();
        write(source / "packs" / "z.pak", "not a pak at all");
        write(source / "packs" / "notes.txt", "ignored");
        PackWriter c(user / "packs" / "c.pak");
        c.add("data/items.json", "{\"from\": \"c\"}");
        c.add("data/c_only.json", "{}");
        c.finish();
    }

    SUBCASE("over a packed game") {
        packFolder(source, scratch.root / "dist" / "game.pak");
        const auto game = openGame(scratch.root / "dist" / "game.pak");
        std::filesystem::create_directories(game / "packs");
        for (const char* name : {"a.pak", "b.pak", "z.pak"}) {
            std::filesystem::copy_file(source / "packs" / name, game / "packs" / name);
        }
        EngineOptions options;
        options.gameRoot = game;
        options.libraryRoot = FENGINE_LUALIB;
        options.userData = user;
        options.seed = 1;
        Engine engine(std::move(options));
        CHECK(engine.packs() == std::vector<std::string>{"packs/a.pak", "packs/b.pak", "user://packs/c.pak"});
        auto& lua = engine.lua();
        CHECK(lua.console("table.concat(fe.app.packs(), ',')") ==
              "packs/a.pak,packs/b.pak,user://packs/c.pak");
        CHECK(lua.console("fe.fs.read('data/items.json')") == "{\"from\": \"c\"}");
        CHECK(lua.console("require('scripts.greeting')") == "b");
        CHECK(lua.console("table.concat(fe.fs.list('data'), ',')") ==
              "a_only.json,base_only.json,c_only.json,items.json");
        CHECK(lua.console("fe.fs.exists('maps/arena2')") == "true");
        REQUIRE(engine.loadWorld("second", "arena2")); // the map id inside is checked against the folder
    }

    SUBCASE("over a loose development folder") {
        // A pack still overrides a loose file, while a loose file beats game.pak.
        packFolder(source, scratch.root / "source.pak");
        write(source / "data" / "items.json", "{\"from\": \"loose\"}");
        write(source / "data" / "base_only.json", "{\"loose\": true}");
        std::filesystem::remove(source / "scripts" / "greeting.lua");
        mountPack(scratch.root / "source.pak", true);
        CHECK(readFile(source / "data" / "base_only.json") == "{\"loose\": true}");
        EngineOptions options;
        options.gameRoot = source;
        options.libraryRoot = FENGINE_LUALIB;
        options.userData = scratch.root / "other-user";
        options.seed = 1;
        Engine engine(std::move(options));
        CHECK(engine.packs() == std::vector<std::string>{"packs/a.pak", "packs/b.pak"});
        auto& lua = engine.lua();
        CHECK(lua.console("fe.fs.read('data/items.json')") == "{\"from\": \"b\"}");
        CHECK(lua.console("fe.fs.read('data/base_only.json')") == "{\"loose\": true}");
        CHECK(lua.console("require('scripts.greeting')") == "b");
        CHECK(lua.console("fe.fs.exists('data/a_only.json')") == "true");
    }
}

TEST_CASE("user:// images load read-only from the user data folder and follow changes") {
    Scratch scratch("user");
    const auto user = scratch.root / "user";
    write(scratch.root / "outside.png", "");
    AssetCache assets(FENGINE_TEST_GAME, FENGINE_LUALIB, user);
    const std::string path = "user://thumbnails/slot1.png";
    CHECK_FALSE(assets.tryImage(path));
    CHECK_THROWS(assets.image("user://../outside.png"));
    CHECK_THROWS(AssetCache(FENGINE_TEST_GAME, FENGINE_LUALIB).image(path));

    std::filesystem::create_directories(user / "thumbnails");
    Image::solid(1, 2, 3).savePng(user / "thumbnails" / "slot1.png");
    const auto first = assets.tryImage(path);
    REQUIRE(first);
    CHECK(first->width == 1);
    const auto generation = assets.generation();
    CHECK(assets.tryImage(path) == first);
    CHECK(assets.generation() == generation);

    const Image bigger{2, 1, {4, 5, 6, 255, 4, 5, 6, 255}};
    bigger.savePng(user / "thumbnails" / "slot1.png");
    const auto second = assets.tryImage(path);
    REQUIRE(second);
    CHECK(second->width == 2);
    CHECK(assets.generation() > generation);

    std::filesystem::remove(user / "thumbnails" / "slot1.png");
    CHECK_FALSE(assets.tryImage(path));
    Image::solid(7, 8, 9).savePng(user / "thumbnails" / "slot1.png");
    const auto third = assets.tryImage(path);
    REQUIRE(third);
    CHECK(third->rgba[0] == 7);
}

TEST_CASE("fe.save.thumbnail_path names a slot's thumbnail once it exists") {
    Scratch scratch("thumbnail");
    EngineOptions options;
    options.gameRoot = FENGINE_TEST_GAME;
    options.libraryRoot = FENGINE_LUALIB;
    options.userData = scratch.root / "user";
    options.seed = 1;
    Engine engine(std::move(options));
    auto& lua = engine.lua();
    CHECK(lua.console("fe.save.thumbnail_path('slot1')") == "nil");
    CHECK_FALSE(lua.evaluate("fe.save.thumbnail_path('../x')").ok);
    lua.evaluate("fe.save.write('slot1', {gold = 1})");
    const auto file = engine.saves().thumbnailPath("slot1");
    CHECK(file == scratch.root / "user" / "thumbnails" / "slot1.png");
    std::filesystem::create_directories(file.parent_path());
    Image::solid(9, 9, 9).savePng(file);
    CHECK(lua.console("fe.save.thumbnail_path('slot1')") == "user://thumbnails/slot1.png");
    CHECK(lua.console("fe.save.list()[1].thumbnail") == "true");
    CHECK(engine.assets().tryImage("user://thumbnails/slot1.png"));
    lua.evaluate("fe.save.delete('slot1')");
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(lua.console("fe.save.thumbnail_path('slot1')") == "nil");
}
