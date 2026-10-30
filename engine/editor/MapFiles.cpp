#include "engine/editor/MapFiles.h"
#include "engine/core/Paths.h"
#include <algorithm>
#include <stdexcept>

namespace fe {
bool validMapId(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::filesystem::path mapFolder(const std::filesystem::path& gameRoot, std::string_view id) {
    if (!validMapId(id)) {
        throw std::runtime_error("Map ids use 1 to 64 of a-z, 0-9, _ and -: '" + std::string(id) + "'");
    }
    return gameRoot / "maps" / std::string(id);
}

bool mapExists(const std::filesystem::path& gameRoot, std::string_view id) {
    return validMapId(id) && isFile(gameRoot / "maps" / std::string(id) / "map.json");
}

MapData makeBlankMap(const std::string& id, const std::string& name, int width, int depth) {
    if (!validMapId(id)) {
        throw std::runtime_error("Map ids use 1 to 64 of a-z, 0-9, _ and -: '" + id + "'");
    }
    width = std::clamp(width, 1, 1024);
    depth = std::clamp(depth, 1, 1024);
    Json json = {{"format", 2},
                 {"id", id},
                 {"name", name.empty() ? id : name},
                 {"grid",
                  {{"width", width},
                   {"depth", depth},
                   {"layers", Json::array({{{"name", "ground"}, {"height_fill", 0}, {"walk_fill", true}}})}}},
                 {"points",
                  {{"start",
                    {{"position", {static_cast<float>(width) * 0.5f, 0, static_cast<float>(depth) * 0.5f}},
                     {"facing", {0, 1}},
                     {"layer", 0}}}}}};
    return MapData::fromJson(json);
}

void writeMap(const std::filesystem::path& gameRoot, const MapData& map, const std::filesystem::path& geometry) {
    const auto folder = mapFolder(gameRoot, map.id);
    const auto json = map.toJson();
    MapData::fromJson(json); // never write a map that cannot be loaded
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        throw std::runtime_error("Cannot create " + folder.string() + ": " + error.message());
    }
    if (!geometry.empty()) {
        const auto extension = geometry.extension().string();
        if (extension != ".glb" && extension != ".gltf") {
            throw std::runtime_error("Geometry must be a .glb or .gltf file");
        }
        const auto target = folder / map.geometry;
        if (!std::filesystem::equivalent(geometry, target, error)) {
            std::filesystem::copy_file(geometry, target, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                throw std::runtime_error("Cannot copy the geometry: " + error.message());
            }
        }
    }
    if (geometry.empty() && !isFile(folder / map.geometry)) {
        // An empty scene: the map is flat ground until a Blender export replaces it.
        const Json empty = {{"asset", {{"version", "2.0"}, {"generator", "f-engine editor"}}},
                            {"scenes", Json::array({{{"nodes", Json::array()}}})},
                            {"scene", 0}};
        auto blank = json;
        blank["geometry"] = "geometry.gltf";
        writeJson(folder / "geometry.gltf", empty);
        writeJson(folder / "map.json", blank);
        return;
    }
    writeJson(folder / "map.json", json);
}

void copyMap(const std::filesystem::path& gameRoot, std::string_view from, const std::string& to,
             const std::string& newName) {
    const auto source = mapFolder(gameRoot, from);
    const auto target = mapFolder(gameRoot, to);
    if (!isFile(source / "map.json")) {
        throw std::runtime_error("No map '" + std::string(from) + "'");
    }
    if (std::filesystem::exists(target)) {
        throw std::runtime_error("A map '" + to + "' already exists");
    }
    std::error_code error;
    std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
    if (error) {
        throw std::runtime_error("Cannot copy the map folder: " + error.message());
    }
    auto json = readJson(target / "map.json");
    json["id"] = to;
    if (!newName.empty()) {
        json["name"] = newName;
    }
    writeJson(target / "map.json", json);
}

void renameMap(const std::filesystem::path& gameRoot, std::string_view from, const std::string& to) {
    const auto source = mapFolder(gameRoot, from);
    const auto target = mapFolder(gameRoot, to);
    if (!isFile(source / "map.json")) {
        throw std::runtime_error("No map '" + std::string(from) + "'");
    }
    if (std::filesystem::exists(target)) {
        throw std::runtime_error("A map '" + to + "' already exists");
    }
    std::error_code error;
    std::filesystem::rename(source, target, error);
    if (error) {
        throw std::runtime_error("Cannot move the map folder: " + error.message());
    }
    auto json = readJson(target / "map.json");
    json["id"] = to;
    writeJson(target / "map.json", json);
}

void deleteMap(const std::filesystem::path& gameRoot, std::string_view id) {
    const auto folder = mapFolder(gameRoot, id);
    if (!isFile(folder / "map.json")) {
        throw std::runtime_error("No map '" + std::string(id) + "'");
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (error) {
        throw std::runtime_error("Cannot remove the map folder: " + error.message());
    }
}
} // namespace fe
