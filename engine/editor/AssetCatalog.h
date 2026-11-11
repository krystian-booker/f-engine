#pragma once
#include "engine/core/Json.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
// What a file in the game folder is, judged by its name.
enum class AssetKind { Sprite, Image, Model, Audio, Lut, Script, Map, Data, Shader, Font, Text, Other };
[[nodiscard]] const char* assetKindName(AssetKind kind);
[[nodiscard]] AssetKind assetKindOf(std::string_view path);

struct AssetEntry {
    std::string path; // game-relative generic path, or "@fe/..." for a library asset
    AssetKind kind = AssetKind::Other;
    std::uintmax_t size = 0;
    // For an image that an .anim.json names as its atlas, normal or emissive
    // map: that sprite's path (the image is part of the sprite, not loose art).
    std::string sprite;
    bool operator==(const AssetEntry&) const = default;
};

// Who uses an asset: the file (and a locator inside it) holding the reference.
struct AssetReference {
    std::string file;  // "maps/town/map.json", "data/actors.json"
    std::string where; // "placements/elder/props/sprite", "aster/portrait"
    bool operator==(const AssetReference&) const = default;
};

struct MapInfo {
    std::string id, name;
    int width = 0, depth = 0;
    std::string path; // "maps/town/map.json"
    bool operator==(const MapInfo&) const = default;
};

struct AudioId {
    std::string group, id, file; // "music", "vale", "audio/vale.wav"
    bool operator==(const AudioId&) const = default;
};

// The editor's view of a game folder's files: every asset by kind, who
// references what, which references point at nothing, and the ids and
// modules pickers offer. UI-free; scan() reads the folder (and mounted
// archives) through engine/core/Paths.h, so it runs headless.
class AssetCatalog {
public:
    AssetCatalog(std::filesystem::path gameRoot, std::filesystem::path builtinRoot);
    // Reads the folder again. Cheap enough to call after the file watcher
    // reports a change; the revision moves when the result differs.
    void scan();
    [[nodiscard]] std::uint64_t revision() const { return revision_; }

    [[nodiscard]] const std::vector<AssetEntry>& entries() const { return entries_; }
    [[nodiscard]] std::vector<const AssetEntry*> ofKind(AssetKind kind) const;
    [[nodiscard]] const AssetEntry* find(std::string_view path) const;
    // Everything that names the asset (maps, data tables, sprites, audio ids).
    [[nodiscard]] const std::vector<AssetReference>& references(std::string_view path) const;
    // Referenced paths no file supplies, with who references them.
    [[nodiscard]] std::vector<std::pair<std::string, AssetReference>> missing() const;

    [[nodiscard]] const std::vector<MapInfo>& maps() const { return maps_; }
    [[nodiscard]] const std::vector<AudioId>& audioIds() const { return audio_; }
    // Dotted module names of the Lua files under a folder ("events" ->
    // "events.elder"); an empty prefix lists every module of the game.
    [[nodiscard]] std::vector<std::string> modules(std::string_view folder) const;
    // Names of the folders at the top of the game (for the asset browser's tree).
    [[nodiscard]] std::vector<std::string> folders(std::string_view parent) const;

    [[nodiscard]] const std::filesystem::path& gameRoot() const { return gameRoot_; }
    // The file behind a catalog path (game-relative, or "@fe/..." in the library).
    [[nodiscard]] std::filesystem::path fileOf(const std::string& path) const;
    // Whether a game-relative (or @fe/) path names a file.
    [[nodiscard]] bool exists(std::string_view path) const;

private:
    void addReference(const std::string& target, const std::string& file, const std::string& where);
    void indexJson(const Json& value, const std::string& file, const std::string& where, bool isMap);
    void indexMap(const std::string& path);
    void indexData(const std::string& path);
    void indexSprite(const std::string& path);
    std::filesystem::path gameRoot_, builtinRoot_;
    std::vector<AssetEntry> entries_;
    std::map<std::string, std::vector<AssetReference>, std::less<>> references_;
    std::vector<MapInfo> maps_;
    std::vector<AudioId> audio_;
    std::uint64_t revision_ = 0;
};
} // namespace fe
