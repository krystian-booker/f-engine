#include "engine/editor/MapFiles.h"
#include "engine/core/Paths.h"
#include "engine/editor/WalkBake.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace fe {
namespace {
std::string base64(const std::vector<unsigned char>& bytes) {
    static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned int a = bytes[i], b = i + 1 < bytes.size() ? bytes[i + 1] : 0,
                           c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const unsigned int triple = (a << 16) | (b << 8) | c;
        out += Alphabet[(triple >> 18) & 63];
        out += Alphabet[(triple >> 12) & 63];
        out += i + 1 < bytes.size() ? Alphabet[(triple >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? Alphabet[triple & 63] : '=';
    }
    return out;
}

template<class T> void append(std::vector<unsigned char>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
} // namespace

Json groundQuadGltf(float width, float depth) {
    // Four vertices (position, normal, colour), two triangles, one buffer as a data URI.
    std::vector<unsigned char> buffer;
    const float positions[4][3] = {{0, 0, 0}, {width, 0, 0}, {width, 0, depth}, {0, 0, depth}};
    for (const auto& p : positions) {
        append(buffer, p[0]);
        append(buffer, p[1]);
        append(buffer, p[2]);
    }
    for (int i = 0; i < 4; ++i) {
        append(buffer, 0.0f);
        append(buffer, 1.0f);
        append(buffer, 0.0f);
    }
    for (int i = 0; i < 4; ++i) { // a grey-green ground, linear
        append(buffer, 0.3f);
        append(buffer, 0.42f);
        append(buffer, 0.28f);
        append(buffer, 1.0f);
    }
    const std::uint16_t indices[6] = {0, 2, 1, 0, 3, 2};
    for (const auto index : indices) {
        append(buffer, index);
    }
    const auto positionsBytes = 4 * 12, normalsBytes = 4 * 12, coloursBytes = 4 * 16, indicesBytes = 6 * 2;
    return {{"asset", {{"version", "2.0"}, {"generator", "f-engine editor"}}},
            {"scene", 0},
            {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
            {"nodes", Json::array({{{"mesh", 0}, {"name", "ground"}}})},
            {"meshes", Json::array({{{"primitives", Json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"COLOR_0", 2}}},
                                                                   {"indices", 3},
                                                                   {"material", 0}}})}}})},
            {"materials", Json::array({{{"pbrMetallicRoughness", {{"baseColorFactor", {1, 1, 1, 1}}}}, {"doubleSided", true}}})},
            {"buffers", Json::array({{{"byteLength", buffer.size()}, {"uri", "data:application/octet-stream;base64," + base64(buffer)}}})},
            {"bufferViews", Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", positionsBytes}, {"target", 34962}},
                                         {{"buffer", 0}, {"byteOffset", positionsBytes}, {"byteLength", normalsBytes}, {"target", 34962}},
                                         {{"buffer", 0}, {"byteOffset", positionsBytes + normalsBytes}, {"byteLength", coloursBytes}, {"target", 34962}},
                                         {{"buffer", 0}, {"byteOffset", positionsBytes + normalsBytes + coloursBytes}, {"byteLength", indicesBytes}, {"target", 34963}}})},
            {"accessors", Json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {width, 0, depth}}},
                                       {{"bufferView", 1}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}},
                                       {{"bufferView", 2}, {"componentType", 5126}, {"count", 4}, {"type", "VEC4"}},
                                       {{"bufferView", 3}, {"componentType", 5123}, {"count", 6}, {"type", "SCALAR"}}})}};
}

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
    auto map = MapData::fromJson(json);
    // A new map derives its walkability: the editor bakes it as soon as it opens.
    map.bake.options = BakeOptions{}.toJson();
    return map;
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
        // Flat ground the size of the grid: something to stand on and to drop kit
        // pieces onto until a Blender export replaces it.
        auto blank = json;
        blank["geometry"] = "geometry.gltf";
        writeJson(folder / "geometry.gltf", groundQuadGltf(static_cast<float>(map.grid.width()), static_cast<float>(map.grid.depth())));
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
