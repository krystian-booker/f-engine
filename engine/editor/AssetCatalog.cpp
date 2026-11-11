#include "engine/editor/AssetCatalog.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/editor/PropLinks.h"
#include <algorithm>
#include <set>

namespace fe {
namespace {
std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// Whether a JSON string looks like a file path an asset field would hold.
bool looksLikeFile(std::string_view text) {
    if (text.empty() || text.size() > 512 || text.find('\n') != std::string_view::npos) {
        return false;
    }
    const auto kind = assetKindOf(text);
    return kind != AssetKind::Other && kind != AssetKind::Script;
}

// A dotted module name ("events.elder") as its file path ("events/elder.lua").
std::string moduleFile(std::string_view module) {
    std::string path(module);
    std::replace(path.begin(), path.end(), '.', '/');
    return path + ".lua";
}

bool looksLikeModule(std::string_view text) {
    if (text.empty() || text.size() > 128) {
        return false;
    }
    return std::all_of(
               text.begin(), text.end(),
               [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }) &&
           text.find('/') == std::string_view::npos && text.front() != '.' && text.back() != '.';
}
} // namespace

const char* assetKindName(AssetKind kind) {
    switch (kind) {
    case AssetKind::Sprite:
        return "sprite";
    case AssetKind::Image:
        return "image";
    case AssetKind::Model:
        return "model";
    case AssetKind::Audio:
        return "audio";
    case AssetKind::Lut:
        return "lut";
    case AssetKind::Script:
        return "script";
    case AssetKind::Map:
        return "map";
    case AssetKind::Data:
        return "data";
    case AssetKind::Shader:
        return "shader";
    case AssetKind::Font:
        return "font";
    case AssetKind::Text:
        return "text";
    case AssetKind::Other:
        break;
    }
    return "other";
}

AssetKind assetKindOf(std::string_view path) {
    const auto name = lower(path);
    if (endsWith(name, ".anim.json")) {
        return AssetKind::Sprite;
    }
    if (endsWith(name, "/map.json") || name == "map.json") {
        return AssetKind::Map;
    }
    const auto dot = name.rfind('.');
    const auto ext = dot == std::string::npos ? std::string() : name.substr(dot);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".ktx") {
        return AssetKind::Image;
    }
    if (ext == ".glb" || ext == ".gltf") {
        return AssetKind::Model;
    }
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
        return AssetKind::Audio;
    }
    if (ext == ".cube") {
        return AssetKind::Lut;
    }
    if (ext == ".lua") {
        return AssetKind::Script;
    }
    if (ext == ".sc" || ext == ".sh" || ext == ".bin") {
        return AssetKind::Shader;
    }
    if (ext == ".ttf" || ext == ".otf" || ext == ".fnt") {
        return AssetKind::Font;
    }
    if (ext == ".json") {
        return name.starts_with("data/") ? AssetKind::Data : AssetKind::Text;
    }
    if (ext == ".txt" || ext == ".md") {
        return AssetKind::Text;
    }
    return AssetKind::Other;
}

AssetCatalog::AssetCatalog(std::filesystem::path gameRoot, std::filesystem::path builtinRoot)
    : gameRoot_(std::move(gameRoot)), builtinRoot_(std::move(builtinRoot)), schemas_(defaultSchemas()) {
}

bool AssetCatalog::exists(std::string_view path) const {
    try {
        if (path.starts_with("@fe/")) {
            return !builtinRoot_.empty() && isFile(resolveInside(builtinRoot_, path.substr(4)));
        }
        return isFile(resolveInside(gameRoot_, path));
    } catch (const std::exception&) {
        return false;
    }
}

void AssetCatalog::scan() {
    std::vector<AssetEntry> entries;
    std::map<std::string, std::vector<AssetReference>, std::less<>> references;
    std::vector<MapInfo> maps;
    std::vector<AudioId> audio;
    std::swap(entries, entries_);
    std::swap(references, references_);
    std::swap(maps, maps_);
    std::swap(audio, audio_);
    entries_.clear();
    references_.clear();
    maps_.clear();
    audio_.clear();
    try {
        for (const auto& relative : listFilesBelow(gameRoot_)) {
            if (relative.starts_with(".") || relative.find("/.") != std::string::npos) {
                continue; // hidden files (.DS_Store, .git)
            }
            AssetEntry entry;
            entry.path = relative;
            entry.kind = assetKindOf(relative);
            std::error_code error;
            entry.size = std::filesystem::file_size(gameRoot_ / relative, error);
            if (error) {
                entry.size = 0;
            }
            entries_.push_back(std::move(entry));
        }
    } catch (const std::exception& error) {
        log(LogLevel::Warning, std::string("Asset catalog: ") + error.what());
    }
    // The engine's own assets (default font, input glyphs and the like), as "@fe/assets/...".
    if (!builtinRoot_.empty()) {
        try {
            const auto folder = builtinRoot_ / "assets";
            if (isFolder(folder)) {
                for (const auto& relative : listFilesBelow(folder)) {
                    AssetEntry entry;
                    entry.path = "@fe/assets/" + relative;
                    entry.kind = assetKindOf(relative);
                    std::error_code error;
                    entry.size = std::filesystem::file_size(folder / relative, error);
                    if (error) {
                        entry.size = 0;
                    }
                    entries_.push_back(std::move(entry));
                }
            }
        } catch (const std::exception& error) {
            log(LogLevel::Debug, std::string("Asset catalog (library): ") + error.what());
        }
    }
    // The audio ids first: maps and tables resolve their strings against them.
    if (exists("data/audio.json")) {
        indexData("data/audio.json");
    }
    for (const auto& entry : std::vector<AssetEntry>(entries_)) {
        if (entry.path == "data/audio.json") {
            continue;
        }
        switch (entry.kind) {
        case AssetKind::Map:
            indexMap(entry.path);
            break;
        case AssetKind::Data:
            indexData(entry.path);
            break;
        case AssetKind::Sprite:
            indexSprite(entry.path);
            break;
        default:
            break;
        }
    }
    std::sort(maps_.begin(), maps_.end(), [](const MapInfo& a, const MapInfo& b) { return a.id < b.id; });
    if (entries != entries_ || references != references_ || maps != maps_ || audio != audio_) {
        ++revision_;
    }
}

void AssetCatalog::addReference(const std::string& target, const std::string& file,
                                const std::string& where) {
    auto& list = references_[target];
    if (std::none_of(list.begin(), list.end(),
                     [&](const AssetReference& r) { return r.file == file && r.where == where; })) {
        list.push_back({file, where});
    }
}

// Walks a JSON document for strings that name things. No key is known by
// name: a string is a reference when it reads like a file path, or when it
// resolves to something that exists (a Lua module, "module.function" of one,
// a map folder, an audio id of data/audio.json). What a field is *meant* to
// name, so that a script or a map that does not exist is reported missing,
// comes from the game's schemas (indexLinks).
void AssetCatalog::indexJson(const Json& value, const std::string& file, const std::string& where,
                             bool isMap) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            const auto path = where.empty() ? key : where + "/" + key;
            if (child.is_string()) {
                indexString(child.get<std::string>(), file, path);
            } else {
                indexJson(child, file, path, isMap);
            }
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto& child = value[i];
            // A placement or zone list: name entries by their id so the
            // reference reads "placements/elder/props/sprite".
            std::string name = std::to_string(i);
            if (isMap && child.is_object() && child.contains("id") && child.at("id").is_string()) {
                name = child.at("id").get<std::string>();
            }
            const auto path = where.empty() ? name : where + "/" + name;
            if (child.is_string()) {
                indexString(child.get<std::string>(), file, path);
            } else {
                indexJson(child, file, path, isMap);
            }
        }
    }
}

void AssetCatalog::indexString(const std::string& text, const std::string& file, const std::string& where) {
    if (text.empty()) {
        return;
    }
    if (looksLikeFile(text)) {
        addReference(text, file, where);
        return;
    }
    if (looksLikeModule(text)) {
        if (exists(moduleFile(text))) {
            addReference(moduleFile(text), file, where);
            return;
        }
        // "module.function"
        if (const auto dot = text.rfind('.');
            dot != std::string::npos && exists(moduleFile(text.substr(0, dot)))) {
            addReference(moduleFile(text.substr(0, dot)), file, where);
            return;
        }
    }
    if (text.find('/') == std::string::npos && exists("maps/" + text + "/map.json")) {
        addReference("maps/" + text + "/map.json", file, where);
        return;
    }
    if (std::any_of(audio_.begin(), audio_.end(), [&](const AudioId& id) { return id.id == text; })) {
        addReference("audio:" + text, file, where);
    }
}

// The fields the game's schemas type as scripts and maps, for the placements
// and zones of a map: referenced whether they exist or not, so a typo shows
// up among the missing.
void AssetCatalog::indexLinks(const Json& objects, const char* group, const std::string& file) {
    if (!objects.is_array()) {
        return;
    }
    for (const auto& object : objects) {
        if (!object.is_object()) {
            continue;
        }
        const auto id = object.value("id", std::string());
        const auto props = object.value("props", Json::object());
        const auto* fields = typeFields(schemas_, group, object.value("type", std::string()));
        const auto list = std::string(group) == "zones" ? "zones" : "placements";
        for (const auto& link : propLinks(fields, props)) {
            const auto where = std::string(list) + "/" + id + "/props/" + link.field;
            if (link.kind == PropLink::Kind::Script) {
                if (looksLikeModule(link.value)) {
                    addReference(moduleFile(link.value), file, where);
                }
            } else {
                addReference("maps/" + link.value + "/map.json", file, where);
            }
        }
    }
}

void AssetCatalog::indexMap(const std::string& path) {
    try {
        const auto json = parseJson(readFile(gameRoot_ / path));
        MapInfo info;
        info.path = path;
        info.id = json.value("id", std::string());
        info.name = json.value("name", info.id);
        if (const auto grid = json.find("grid"); grid != json.end() && grid->is_object()) {
            info.width = grid->value("width", 0);
            info.depth = grid->value("depth", 0);
        }
        maps_.push_back(info);
        const auto folder = std::filesystem::path(path).parent_path().generic_string();
        const auto geometry = json.value("geometry", std::string("geometry.glb"));
        if (!geometry.empty()) {
            addReference(folder + "/" + geometry, path, "geometry");
        }
        auto rest = json;
        rest.erase("geometry"); // referenced above, relative to the map's folder
        indexJson(rest, path, "", true);
        indexLinks(json.value("placements", Json::array()), "entities", path);
        indexLinks(json.value("zones", Json::array()), "zones", path);
    } catch (const std::exception& error) {
        log(LogLevel::Debug, "Asset catalog: " + path + ": " + error.what());
    }
}

void AssetCatalog::indexData(const std::string& path) {
    try {
        const auto json = parseJson(readFile(gameRoot_ / path));
        if (path == "data/audio.json" && json.is_object()) {
            for (const auto& [group, ids] : json.items()) {
                if (!ids.is_object()) {
                    continue;
                }
                for (const auto& [id, spec] : ids.items()) {
                    std::string file;
                    if (spec.is_string()) {
                        file = spec.get<std::string>();
                    } else if (spec.is_object()) {
                        file = spec.value("file", std::string());
                    }
                    audio_.push_back({group, id, file});
                    if (!file.empty()) {
                        addReference(file, path, group + "/" + id);
                    }
                }
            }
            return;
        }
        // Records keyed by id (an object) or an array of records with ids.
        if (json.is_object()) {
            for (const auto& [id, record] : json.items()) {
                indexJson(record, path, id, false);
            }
        } else if (json.is_array()) {
            for (std::size_t i = 0; i < json.size(); ++i) {
                const auto& record = json[i];
                const auto id = record.is_object() && record.contains("id") && record.at("id").is_string()
                                    ? record.at("id").get<std::string>()
                                    : std::to_string(i);
                indexJson(record, path, id, false);
            }
        }
    } catch (const std::exception& error) {
        log(LogLevel::Debug, "Asset catalog: " + path + ": " + error.what());
    }
}

std::filesystem::path AssetCatalog::fileOf(const std::string& path) const {
    if (path.starts_with("@fe/")) {
        return builtinRoot_ / path.substr(4);
    }
    return gameRoot_ / path;
}

void AssetCatalog::indexSprite(const std::string& path) {
    try {
        const auto json = parseJson(readFile(fileOf(path)));
        const auto folder = std::filesystem::path(path).parent_path();
        for (const char* key : {"atlas", "normal", "emissive"}) {
            const auto image = json.value(key, std::string());
            if (image.empty()) {
                continue;
            }
            const auto imagePath = (folder / image).lexically_normal().generic_string();
            addReference(imagePath, path, key);
            for (auto& entry : entries_) {
                if (entry.path == imagePath) {
                    entry.sprite = path;
                }
            }
        }
    } catch (const std::exception& error) {
        log(LogLevel::Debug, "Asset catalog: " + path + ": " + error.what());
    }
}

std::vector<const AssetEntry*> AssetCatalog::ofKind(AssetKind kind) const {
    std::vector<const AssetEntry*> out;
    for (const auto& entry : entries_) {
        if (entry.kind == kind) {
            out.push_back(&entry);
        }
    }
    return out;
}

const AssetEntry* AssetCatalog::find(std::string_view path) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const AssetEntry& entry) { return entry.path == path; });
    return found == entries_.end() ? nullptr : &*found;
}

const std::vector<AssetReference>& AssetCatalog::references(std::string_view path) const {
    static const std::vector<AssetReference> none;
    const auto found = references_.find(path);
    return found == references_.end() ? none : found->second;
}

std::vector<std::pair<std::string, AssetReference>> AssetCatalog::missing() const {
    std::vector<std::pair<std::string, AssetReference>> out;
    std::set<std::string> audioIds;
    for (const auto& id : audio_) {
        audioIds.insert(id.group + "/" + id.id);
        audioIds.insert(id.id);
    }
    for (const auto& [target, users] : references_) {
        bool found;
        if (target.starts_with("audio:")) {
            const auto id = target.substr(6);
            // A path-like id names a file directly (the convention of data/audio.json).
            found = id.find('/') != std::string::npos || id.find('.') != std::string::npos
                        ? exists(id)
                        : audioIds.contains(id);
        } else {
            found = exists(target);
        }
        if (!found) {
            for (const auto& user : users) {
                out.emplace_back(target, user);
            }
        }
    }
    return out;
}

std::vector<std::string> AssetCatalog::modules(std::string_view folder) const {
    std::vector<std::string> out;
    const std::string prefix = folder.empty() ? std::string() : std::string(folder) + "/";
    for (const auto& entry : entries_) {
        if (entry.kind != AssetKind::Script || !entry.path.starts_with(prefix) ||
            entry.path.starts_with("tests/")) {
            continue;
        }
        auto module = entry.path.substr(0, entry.path.size() - 4);
        if (endsWith(module, "/init")) {
            module.resize(module.size() - 5);
        }
        std::replace(module.begin(), module.end(), '/', '.');
        out.push_back(module);
    }
    return out;
}

std::vector<std::string> AssetCatalog::folders(std::string_view parent) const {
    std::set<std::string> names;
    const std::string prefix = parent.empty() ? std::string() : std::string(parent) + "/";
    for (const auto& entry : entries_) {
        if (!entry.path.starts_with(prefix)) {
            continue;
        }
        const auto rest = entry.path.substr(prefix.size());
        if (const auto slash = rest.find('/'); slash != std::string::npos) {
            names.insert(rest.substr(0, slash));
        }
    }
    return {names.begin(), names.end()};
}
} // namespace fe
