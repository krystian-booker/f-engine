#include "engine/editor/EditorSession.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/editor/MapFiles.h"
#include "engine/editor/PropLinks.h"
#include "engine/editor/SceneEdits.h"
#include "engine/editor/ScenePreview.h"
#include "engine/editor/WalkBake.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <algorithm>

namespace fe {
namespace {
using editorui::jsonObject;
using editorui::jsonString;

template<class Record> std::string freshId(const std::string& prefix, const std::vector<Record>& records) {
    for (std::size_t n = records.size() + 1;; ++n) {
        const auto id = prefix + std::to_string(n);
        if (std::none_of(records.begin(), records.end(), [&](const auto& r) { return r.id == id; })) {
            return id;
        }
    }
}

template<class T> bool validIndex(int index, const std::vector<T>& list) {
    return index >= 0 && index < static_cast<int>(list.size());
}
} // namespace

EditorSession::EditorSession(Engine& engine) : engine_(engine), schemas_(defaultSchemas()) {
}

EditorSession::~EditorSession() = default;

const std::filesystem::path& EditorSession::gameRoot() const {
    return engine_.lua().gameRoot();
}

void EditorSession::changed(unsigned changes) {
    if (changes & ~static_cast<unsigned>(SelectionChanged)) {
        ++revision_;
    }
    if (onChanged) {
        onChanged(changes);
    }
}

void EditorSession::status(const std::string& text) const {
    if (onStatus) {
        onStatus(text);
    }
}

std::vector<std::string> EditorSession::scenes() const {
    std::vector<std::string> ids;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(gameRoot() / "maps", error)) {
        if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "map.json")) {
            ids.push_back(entry.path().filename().string());
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool EditorSession::openScene(const std::string& id) {
    try {
        world_.reset();
        world_ = openPreviewWorld(engine_, id);
    } catch (const std::exception& error) {
        world_.reset();
        selection_ = {};
        extras_.clear();
        changed(SceneChanged | StructureChanged | SelectionChanged | SavedChanged);
        status("Cannot open " + id + ": " + error.what());
        log(LogLevel::Error, "Cannot open scene " + id + ": " + error.what());
        return false;
    }
    history_.sync(*world_);
    selection_ = {};
    extras_.clear();
    gesture_ = false;
    layer = 0;
    bakedSignature_ = bakedSignature();
    changed(SceneChanged | StructureChanged | SelectionChanged | SavedChanged);
    status("Opened " + id);
    return true;
}

void EditorSession::closeScene() {
    if (!world_) {
        return;
    }
    world_.reset();
    if (engine_.world(PreviewWorldName)) {
        engine_.unloadWorld(PreviewWorldName);
    }
    selection_ = {};
    extras_.clear();
    changed(SceneChanged | StructureChanged | SelectionChanged | SavedChanged);
}

const MapData* EditorSession::map() const {
    return world_ ? &world_->authored() : nullptr;
}

std::string EditorSession::sceneId() const {
    return world_ ? world_->authored().id : std::string();
}

bool EditorSession::unsaved() const {
    return world_ && history_.unsaved(*world_);
}

bool EditorSession::save() {
    if (!world_) {
        return false;
    }
    try {
        endGesture();
        const auto json = world_->authored().toJson();
        MapData::fromJson(json); // never write a map that cannot be loaded
        writeJson(resolveInside(gameRoot(), "maps/" + world_->authored().id + "/map.json"), json);
        history_.markSaved(*world_);
        changed(SavedChanged);
        status("Saved maps/" + world_->authored().id + "/map.json");
        return true;
    } catch (const std::exception& error) {
        status(std::string("Not saved: ") + error.what());
        return false;
    }
}

bool EditorSession::createScene(const std::string& id, const std::string& name, int width, int depth) {
    try {
        if (mapExists(gameRoot(), id)) {
            throw std::runtime_error("a scene named '" + id + "' exists already");
        }
        writeMap(gameRoot(), makeBlankMap(id, name.empty() ? id : name, width, depth));
    } catch (const std::exception& error) {
        status(std::string("Scene not created: ") + error.what());
        return false;
    }
    return openScene(id);
}

bool EditorSession::saveSceneAs(const std::string& id) {
    if (!world_) {
        return false;
    }
    try {
        endGesture();
        copyMap(gameRoot(), world_->authored().id, id);
        auto copy = world_->authored();
        copy.id = id;
        writeMap(gameRoot(), copy);
    } catch (const std::exception& error) {
        status(std::string("Not saved: ") + error.what());
        return false;
    }
    return openScene(id);
}

bool EditorSession::renameScene(const std::string& id) {
    if (!world_) {
        return false;
    }
    if (unsaved() && !save()) {
        return false;
    }
    try {
        renameMap(gameRoot(), world_->authored().id, id);
    } catch (const std::exception& error) {
        status(std::string("Not renamed: ") + error.what());
        return false;
    }
    return openScene(id);
}

bool EditorSession::deleteScene(const std::string& id) {
    try {
        if (sceneId() == id) {
            closeScene();
        }
        deleteMap(gameRoot(), id);
        changed(SceneChanged);
        status("Deleted scene " + id);
        return true;
    } catch (const std::exception& error) {
        status(std::string("Not deleted: ") + error.what());
        return false;
    }
}

void EditorSession::refreshSchemas() {
    // A game that describes nothing still gets forms: the editor's own
    // minimal description, replaced by the game's as a whole.
    schemas_ = defaultSchemas();
    try {
        const auto module = engine_.lua().require(engine_.config().entry);
        if (module.get_type() == sol::type::table) {
            const auto hooks = module.as<sol::table>().raw_get<sol::object>("editor");
            if (hooks.get_type() == sol::type::table) {
                const sol::object describe = hooks.as<sol::table>().raw_get<sol::object>("describe");
                if (describe.get_type() == sol::type::function) {
                    const auto result = engine_.lua().invoke(describe);
                    if (!result.valid()) {
                        const sol::error error = result;
                        status(std::string("editor.describe failed: ") + error.what());
                    } else if (result.get_type() == sol::type::string) {
                        const auto parsed = Json::parse(result.get<std::string>(), nullptr, false);
                        if (parsed.is_object()) {
                            schemas_ = parsed;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        log(LogLevel::Warning,
            std::string("The game's entry module did not load; using default types: ") + error.what());
    }
    changed(SchemasChanged);
}

const Json* EditorSession::fieldsFor(const std::string& type,
                                     std::initializer_list<const char*> groups) const {
    for (const auto* group : groups) {
        const auto found = schemas_.find(group);
        if (found != schemas_.end() && found->is_object() && found->contains(type)) {
            const auto& spec = found->at(type);
            const auto fields = spec.find("fields");
            return fields == spec.end() ? nullptr : &*fields;
        }
    }
    return nullptr;
}

std::vector<std::string> EditorSession::typeNames(const char* group) const {
    std::vector<std::string> names;
    const auto types = jsonObject(schemas_, group);
    for (auto type = types.begin(); type != types.end(); ++type) {
        names.push_back(type.key());
    }
    return names;
}

std::string EditorSession::defaultType(const char* group) const {
    const auto preferred = jsonString(jsonObject(schemas_, "defaults"), group, {});
    if (!preferred.empty()) {
        return preferred;
    }
    if (std::string_view(group) == "models") {
        return defaultType("entities");
    }
    const auto types = jsonObject(schemas_, group);
    return types.empty() ? std::string{} : types.begin().key();
}

std::vector<Selection> EditorSession::selected() const {
    auto all = extras_;
    if (!selection_.empty()) {
        all.push_back(selection_);
    }
    return all;
}

bool EditorSession::isSelected(const Selection& selection) const {
    return !selection.empty() &&
           (selection == selection_ || std::find(extras_.begin(), extras_.end(), selection) != extras_.end());
}

void EditorSession::select(const Selection& selection) {
    if (selection == selection_ && extras_.empty()) {
        return;
    }
    selection_ = selection;
    extras_.clear();
    changed(SelectionChanged);
}

void EditorSession::toggle(const Selection& selection) {
    if (selection.empty()) {
        return;
    }
    if (selection_.empty()) {
        selection_ = selection;
    } else if (selection == selection_) {
        // The primary steps down to the last extra, if there is one.
        if (extras_.empty()) {
            selection_ = {};
        } else {
            selection_ = extras_.back();
            extras_.pop_back();
        }
    } else if (const auto found = std::find(extras_.begin(), extras_.end(), selection);
               found != extras_.end()) {
        extras_.erase(found);
    } else {
        extras_.push_back(selection_);
        selection_ = selection;
    }
    changed(SelectionChanged);
}

void EditorSession::selectAll(const std::vector<Selection>& all) {
    extras_ = all;
    selection_ = {};
    if (!extras_.empty()) {
        selection_ = extras_.back();
        extras_.pop_back();
    }
    changed(SelectionChanged);
}

void EditorSession::dropInvalidSelection() {
    if (!world_) {
        selection_ = {};
        extras_.clear();
        return;
    }
    std::erase_if(extras_, [&](const Selection& s) { return !selectionValid(*world_, s); });
    if (!selection_.empty() && !selectionValid(*world_, selection_)) {
        selection_ = {};
        if (!extras_.empty()) {
            selection_ = extras_.back();
            extras_.pop_back();
        }
    }
}

void EditorSession::respawnChanged(const MapData& before) {
    for (const auto& id : changedPlacementIds(before, world_->authored())) {
        if (const auto problem = spawnPlacementPreview(*world_, id); !problem.empty()) {
            status("Preview of " + id + ": " + problem);
        }
    }
}

void EditorSession::edit(const std::function<void(MapData&)>& change, bool gesture, unsigned what) {
    if (!world_) {
        return;
    }
    const bool wasUnsaved = unsaved();
    history_.apply(*world_, change, !gesture_);
    gesture_ = gesture;
    if (!gesture) {
        autoRebake();
    }
    changed(what | (wasUnsaved ? 0u : static_cast<unsigned>(SavedChanged)));
}

// The placed models the bake reads, with where they stand: when this
// changes, the walkability under them is out of date.
std::string EditorSession::bakedSignature() const {
    std::string signature;
    if (const auto* authored = map()) {
        for (const auto& placement : authored->placements) {
            if (const auto* model = bakedModelOf(placement)) {
                signature += *model + toJson(placement.position).dump() + std::to_string(placement.yaw) +
                             toJson(placement.scale).dump() + ";";
            }
        }
    }
    return signature;
}

// Keeps a baked scene's walkability in step with its models, inside the undo
// step of the edit that moved them. A hand-painted scene is left alone.
void EditorSession::autoRebake() {
    const auto signature = bakedSignature();
    if (signature == bakedSignature_) {
        return;
    }
    bakedSignature_ = signature;
    const auto& authored = world_->authored();
    if (authored.bake.autoBake && !authored.bake.empty()) {
        bake(std::nullopt, true);
    }
}

bool EditorSession::bake(std::optional<glm::ivec4> region, bool join) {
    if (!world_) {
        return false;
    }
    try {
        const auto& authored = world_->authored();
        auto options = BakeOptions::fromJson(authored.bake.options);
        options.layer = std::clamp(options.layer, 0, authored.grid.layerCount() - 1);
        options.region = region;
        const auto scene = sceneForMap(authored, engine_.assets(), world_->geometryPath());
        const auto result = bakeWalkability(scene.pieces, authored.grid, options);
        const Json optionsJson = authored.bake.options.empty() ? options.toJson() : authored.bake.options;
        MapData fingerprinted = authored;
        fingerprinted.bake.options = optionsJson;
        const auto fingerprint =
            bakeFingerprint(fingerprinted, world_->geometryPath(), gameFileReader(gameRoot()));
        int cells = 0;
        const auto apply = [&](MapData& m) {
            cells = applyBake(m, result, options);
            m.bake.fingerprint = fingerprint;
            m.bake.options = optionsJson;
        };
        if (join) {
            history_.apply(*world_, apply, false);
        } else {
            endGesture();
            history_.apply(*world_, apply, true);
            bakedSignature_ = bakedSignature();
        }
        if (!gesture_) {
            history_.settle(*world_);
        }
        std::string text = (region ? "Re-baked walkability: " : "Baked walkability: ") +
                           std::to_string(cells) + " cell(s) changed, " +
                           std::to_string(world_->authored().bake.colliders.size()) + " box(es)";
        for (const auto& problem : scene.problems) {
            text += "; " + problem;
        }
        if (!join) {
            changed(ValuesChanged | SavedChanged);
        }
        status(text);
        return true;
    } catch (const std::exception& error) {
        status(std::string("Bake failed: ") + error.what());
        return false;
    }
}

void EditorSession::paintWalkabilityRect(GridPos a, GridPos b, const GridBrush& brush, bool clear) {
    if (!world_) {
        return;
    }
    endGesture();
    const auto& grid = world_->authored().grid;
    const int x0 = std::max(0, std::min(a.x, b.x)), x1 = std::min(grid.width() - 1, std::max(a.x, b.x));
    const int z0 = std::max(0, std::min(a.z, b.z)), z1 = std::min(grid.depth() - 1, std::max(a.z, b.z));
    if (x1 < x0 || z1 < z0) {
        return;
    }
    const bool derived = !world_->authored().bake.empty();
    if (clear) {
        bool cleared = false;
        edit(
            [&](MapData& m) {
                for (int z = z0; z <= z1; ++z) {
                    for (int x = x0; x <= x1; ++x) {
                        cleared = clearOverride(m, {x, z, a.layer}) || cleared;
                    }
                }
            },
            true);
        if (cleared) {
            bake(glm::ivec4(x0, z0, x1 - x0 + 1, z1 - z0 + 1), true);
        }
        endGesture();
        return;
    }
    if (const auto problem = brushProblem(grid, brush, a.layer); !problem.empty()) {
        status(problem);
        return;
    }
    edit([&](MapData& m) {
        if (derived) {
            setOverrideRect(m, {x0, z0, a.layer}, {x1, z1, a.layer}, brush);
        } else {
            fillRect(m.grid, brush, {x0, z0, a.layer}, {x1, z1, a.layer});
        }
    });
}

void EditorSession::floodWalkability(GridPos start, const GridBrush& brush) {
    if (!world_) {
        return;
    }
    endGesture();
    const auto& authored = world_->authored();
    if (!authored.grid.inRange(start.x, start.z, start.layer)) {
        return;
    }
    if (brush.mode == GridBrush::Mode::Ramp) {
        status("Ramps cannot be flood-filled");
        return;
    }
    if (const auto problem = brushProblem(authored.grid, brush, start.layer); !problem.empty()) {
        status(problem);
        return;
    }
    const bool derived = !authored.bake.empty();
    // Which cells the fill reaches: the ones it changes on a copy of the grid.
    std::vector<GridPos> reached;
    if (derived) {
        Grid filled = authored.grid;
        floodFill(filled, brush, start);
        for (int z = 0; z < filled.depth(); ++z) {
            for (int x = 0; x < filled.width(); ++x) {
                const GridPos cell{x, z, start.layer};
                const glm::vec2 centre(static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f);
                if (filled.walkableAt(centre, start.layer) != authored.grid.walkableAt(centre, start.layer) ||
                    filled.heightAt(centre, start.layer) != authored.grid.heightAt(centre, start.layer)) {
                    reached.push_back(cell);
                }
            }
        }
        if (reached.empty()) {
            reached.push_back(start);
        }
    }
    edit([&](MapData& m) {
        if (derived) {
            for (const auto& cell : reached) {
                setOverride(m, cell, brush);
            }
        } else {
            floodFill(m.grid, brush, start);
        }
    });
}

bool EditorSession::importGeometry(const std::filesystem::path& file) {
    if (!world_) {
        return false;
    }
    const auto id = world_->authored().id;
    try {
        endGesture();
        auto next = world_->authored();
        next.geometry = "geometry" + file.extension().string();
        writeMap(gameRoot(), next, file);
        engine_.assets().reload(gameRoot() / "maps" / id / next.geometry);
    } catch (const std::exception& error) {
        status(std::string("Geometry not imported: ") + error.what());
        return false;
    }
    if (!openScene(id)) {
        return false;
    }
    bake();
    return save();
}

int EditorSession::importGeometryMarkers() {
    if (!world_) {
        return 0;
    }
    const auto model = engine_.assets().tryModel(world_->geometryPath());
    if (!model) {
        status("The scene's geometry could not be read");
        return 0;
    }
    endGesture();
    std::vector<std::string> problems;
    int imported = 0;
    edit(
        [&](MapData& m) {
            problems.clear();
            imported = importMarkers(m, *model, problems);
        },
        false, StructureChanged);
    std::string text = "Imported " + std::to_string(imported) + " marker(s)";
    for (const auto& problem : problems) {
        text += "; " + problem;
    }
    status(text);
    return imported;
}

void EditorSession::paintWalkability(GridPos centre, int radius, const GridBrush& brush, bool clear,
                                     bool gesture) {
    if (!world_) {
        return;
    }
    const auto& grid = world_->authored().grid;
    std::vector<GridPos> cells;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (grid.inRange(centre.x + dx, centre.z + dz, centre.layer)) {
                cells.push_back({centre.x + dx, centre.z + dz, centre.layer});
            }
        }
    }
    if (cells.empty()) {
        return;
    }
    const bool derived = !world_->authored().bake.empty();
    if (clear) {
        std::optional<glm::ivec4> region;
        edit(
            [&](MapData& m) {
                region.reset();
                for (const auto& cell : cells) {
                    if (clearOverride(m, cell)) {
                        const glm::ivec4 one(cell.x, cell.z, 1, 1);
                        region = region ? unionRegion(*region, one) : one;
                    }
                }
            },
            true);
        if (region) {
            bake(region, true); // the bake's own values come back
        }
    } else {
        if (const auto problem = brushProblem(grid, brush, centre.layer); !problem.empty()) {
            status(problem);
            return;
        }
        edit(
            [&](MapData& m) {
                for (const auto& cell : cells) {
                    if (derived) {
                        setOverride(m, cell, brush);
                    } else {
                        paint(m.grid, brush, cell, 0);
                    }
                }
            },
            true);
    }
    if (!gesture) {
        endGesture();
    }
}

void EditorSession::endGesture() {
    if (!gesture_ || !world_) {
        gesture_ = false;
        return;
    }
    gesture_ = false;
    autoRebake();
    history_.settle(*world_);
    changed(ValuesChanged | SavedChanged);
}

void EditorSession::undo() {
    if (!world_) {
        return;
    }
    endGesture();
    try {
        const MapData before = world_->authored();
        if (history_.undo(*world_)) {
            respawnChanged(before);
            bakedSignature_ = bakedSignature();
            dropInvalidSelection();
            changed(StructureChanged | ValuesChanged | SelectionChanged | SavedChanged);
            status("Undone");
        }
    } catch (const std::exception& error) {
        status(std::string("Cannot undo: ") + error.what());
    }
}

void EditorSession::redo() {
    if (!world_) {
        return;
    }
    endGesture();
    try {
        const MapData before = world_->authored();
        if (history_.redo(*world_)) {
            respawnChanged(before);
            bakedSignature_ = bakedSignature();
            dropInvalidSelection();
            changed(StructureChanged | ValuesChanged | SelectionChanged | SavedChanged);
            status("Redone");
        }
    } catch (const std::exception& error) {
        status(std::string("Cannot redo: ") + error.what());
    }
}

bool EditorSession::canUndo() const {
    return world_ && history_.undoCount(*world_) > 0;
}

bool EditorSession::canRedo() const {
    return world_ && history_.redoCount(*world_) > 0;
}

Selection EditorSession::addPlacement(const std::string& type, glm::vec3 at) {
    if (!world_) {
        return {};
    }
    endGesture();
    const auto& authored = world_->authored();
    Placement placement;
    placement.type = type.empty() ? defaultType("entities") : type;
    placement.id =
        uniquePlacementId(placement.type.empty() ? "placement" : placement.type, authored.placements);
    placement.position = cellCentre(authored.grid, snapToCell(authored.grid, at, layer));
    placement.layer = layer;
    edit([&](MapData& m) { m.placements.push_back(placement); }, false, StructureChanged);
    spawnPlacementPreview(*world_, placement.id);
    select(Selection::placement(placement.id));
    status("Added " + placement.id);
    return selection_;
}

Selection EditorSession::place(const Placement& placement) {
    if (!world_) {
        return {};
    }
    endGesture();
    edit([&](MapData& m) { m.placements.push_back(placement); }, false, StructureChanged);
    if (const auto problem = spawnPlacementPreview(*world_, placement.id); !problem.empty()) {
        status("Preview of " + placement.id + ": " + problem);
    }
    select(Selection::placement(placement.id));
    return selection_;
}

Selection EditorSession::add(Selection::Kind kind, glm::vec3 at, const std::string& name) {
    if (!world_) {
        return {};
    }
    endGesture();
    const auto& authored = world_->authored();
    const auto ground = cellCentre(authored.grid, snapToCell(authored.grid, at, layer));
    Selection made;
    switch (kind) {
    case Selection::Kind::Placement:
        return addPlacement({}, at);
    case Selection::Kind::Zone: {
        const auto type = defaultType("zones");
        const Zone zone{freshId("zone", authored.zones),
                        type.empty() ? "zone" : type,
                        {ground.x - 2, ground.z - 2},
                        {ground.x + 2, ground.z + 2},
                        -1,
                        Json::object()};
        edit([&](MapData& m) { m.zones.push_back(zone); }, false, StructureChanged);
        made = Selection::zone(zone.id);
        break;
    }
    case Selection::Kind::Point: {
        const std::string base = name.empty() ? "point" : name;
        auto unique = base;
        for (int n = 2; authored.points.contains(unique); ++n) {
            unique = base + std::to_string(n);
        }
        const MapPoint point{ground, {0, 1}, layer};
        edit([&](MapData& m) { m.points[unique] = point; }, false, StructureChanged);
        made = Selection::point(unique);
        break;
    }
    case Selection::Kind::Light: {
        LightSettings light;
        light.position = ground + glm::vec3(0, 2, 0);
        edit([&](MapData& m) { m.lights.push_back(light); }, false, StructureChanged);
        made = Selection::light(static_cast<int>(world_->authored().lights.size()) - 1);
        break;
    }
    case Selection::Kind::Emitter: {
        const MapEmitter emitter{ground, EmitterSettings{}};
        edit([&](MapData& m) { m.emitters.push_back(emitter); }, false, StructureChanged);
        made = Selection::emitter(static_cast<int>(world_->authored().emitters.size()) - 1);
        break;
    }
    case Selection::Kind::Collider: {
        const Box box{ground - glm::vec3(0.5f, 0, 0.5f), ground + glm::vec3(0.5f, 1, 0.5f)};
        edit([&](MapData& m) { m.colliders.push_back(box); }, false, StructureChanged);
        made = Selection::collider(static_cast<int>(world_->authored().colliders.size()) - 1);
        break;
    }
    case Selection::Kind::Entity:
    case Selection::Kind::None:
        return {};
    }
    select(made);
    status("Added " + selectionLabel(*world_, made));
    return made;
}

void EditorSession::deleteSelection() {
    if (!world_) {
        return;
    }
    endGesture();
    auto all = selected();
    std::erase_if(all, [&](const Selection& s) {
        return s.kind == Selection::Kind::Entity || !selectionValid(*world_, s);
    });
    if (all.empty()) {
        return;
    }
    // Index-based kinds go highest index first, so earlier indices stay valid.
    std::sort(all.begin(), all.end(), [](const Selection& a, const Selection& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.index > b.index;
    });
    all.erase(std::unique(all.begin(), all.end()), all.end());
    const auto label =
        all.size() == 1 ? selectionLabel(*world_, all.front()) : std::to_string(all.size()) + " objects";
    const MapData before = world_->authored();
    edit(
        [&](MapData& m) {
            for (const auto& s : all) {
                switch (s.kind) {
                case Selection::Kind::Placement:
                    std::erase_if(m.placements, [&](const Placement& p) { return p.id == s.id; });
                    break;
                case Selection::Kind::Zone:
                    std::erase_if(m.zones, [&](const Zone& z) { return z.id == s.id; });
                    break;
                case Selection::Kind::Point:
                    m.points.erase(s.id);
                    break;
                case Selection::Kind::Light:
                    if (validIndex(s.index, m.lights)) {
                        m.lights.erase(m.lights.begin() + s.index);
                    }
                    break;
                case Selection::Kind::Emitter:
                    if (validIndex(s.index, m.emitters)) {
                        m.emitters.erase(m.emitters.begin() + s.index);
                    }
                    break;
                case Selection::Kind::Collider:
                    if (validIndex(s.index, m.colliders)) {
                        m.colliders.erase(m.colliders.begin() + s.index);
                    }
                    break;
                case Selection::Kind::Entity:
                case Selection::Kind::None:
                    break;
                }
            }
        },
        false, StructureChanged);
    respawnChanged(before);
    selection_ = {};
    extras_.clear();
    changed(SelectionChanged);
    status("Deleted " + label);
}

void EditorSession::duplicateSelection() {
    if (!world_) {
        return;
    }
    endGesture();
    const auto all = selected();
    std::vector<Selection> copies;
    const MapData before = world_->authored();
    edit(
        [&](MapData& m) {
            copies.clear(); // the edit runs once per map copy
            for (const auto& s : all) {
                switch (s.kind) {
                case Selection::Kind::Placement:
                    if (const auto* source = findPlacement(m, s.id)) {
                        const auto copy = duplicatePlacement(*source, m.placements);
                        m.placements.push_back(copy);
                        copies.push_back(Selection::placement(copy.id));
                    }
                    break;
                case Selection::Kind::Zone:
                    if (const auto* source = findZone(m, s.id)) {
                        auto copy = *source;
                        copy.id = freshId(copy.id, m.zones);
                        copy.min.x += 1;
                        copy.max.x += 1;
                        for (auto& corner : copy.points) {
                            corner.x += 1;
                        }
                        m.zones.push_back(copy);
                        copies.push_back(Selection::zone(copy.id));
                    }
                    break;
                case Selection::Kind::Point:
                    if (const auto found = m.points.find(s.id); found != m.points.end()) {
                        auto copy = found->second;
                        copy.position.x += 1;
                        copy.position = clampToGrid(m.grid, copy.position);
                        std::string name = s.id + "_copy";
                        for (int n = 2; m.points.contains(name); ++n) {
                            name = s.id + "_copy" + std::to_string(n);
                        }
                        m.points[name] = copy;
                        copies.push_back(Selection::point(name));
                    }
                    break;
                case Selection::Kind::Light:
                    if (validIndex(s.index, m.lights)) {
                        auto copy = m.lights[static_cast<std::size_t>(s.index)];
                        copy.position.x += 1;
                        m.lights.push_back(copy);
                        copies.push_back(Selection::light(static_cast<int>(m.lights.size()) - 1));
                    }
                    break;
                case Selection::Kind::Emitter:
                    if (validIndex(s.index, m.emitters)) {
                        auto copy = m.emitters[static_cast<std::size_t>(s.index)];
                        copy.position.x += 1;
                        m.emitters.push_back(copy);
                        copies.push_back(Selection::emitter(static_cast<int>(m.emitters.size()) - 1));
                    }
                    break;
                case Selection::Kind::Collider:
                    if (validIndex(s.index, m.colliders)) {
                        auto copy = m.colliders[static_cast<std::size_t>(s.index)];
                        copy.min.x += 1;
                        copy.max.x += 1;
                        m.colliders.push_back(copy);
                        copies.push_back(Selection::collider(static_cast<int>(m.colliders.size()) - 1));
                    }
                    break;
                case Selection::Kind::Entity:
                case Selection::Kind::None:
                    break;
                }
            }
        },
        false, StructureChanged);
    if (copies.empty()) {
        return;
    }
    respawnChanged(before);
    selectAll(copies);
    status(copies.size() == 1 ? "Duplicated as " + selectionLabel(*world_, copies.front())
                              : "Duplicated " + std::to_string(copies.size()) + " objects");
}

bool EditorSession::setTransform(const Selection& selection, const SelectionTransform& transform,
                                 bool gesture) {
    if (!world_ || !selectionValid(*world_, selection) || selection.kind == Selection::Kind::Entity) {
        return false;
    }
    const auto current = selectionTransform(*world_, selection);
    if (!current) {
        return false;
    }
    const glm::vec3 delta = transform.position - current->position;
    const auto others = selection == selection_ ? extras_ : std::vector<Selection>{};
    edit(
        [&](MapData& m) {
            applySelectionTransform(m, selection, transform);
            for (const auto& other : others) {
                if (auto moved = selectionTransform(*world_, other);
                    moved && other.kind != Selection::Kind::Entity) {
                    moved->position += delta;
                    applySelectionTransform(m, other, *moved);
                }
            }
        },
        gesture);
    // Stand-in entities follow without being respawned.
    for (const auto& moved : selection == selection_ ? selected() : std::vector<Selection>{selection}) {
        if (moved.kind == Selection::Kind::Placement) {
            if (const auto* placement = findPlacement(world_->authored(), moved.id)) {
                syncPlacementEntity(*world_, *placement);
            }
        }
    }
    return true;
}

bool EditorSession::renamePoint(const std::string& from, const std::string& to) {
    const auto* authored = map();
    if (!authored || !authored->points.contains(from) || from == to) {
        return false;
    }
    if (to.empty() || authored->points.contains(to)) {
        status(to.empty() ? "A point needs a name" : "A point named '" + to + "' exists already");
        return false;
    }
    endGesture();
    edit(
        [&](MapData& m) {
            auto node = m.points.extract(from);
            if (!node.empty()) {
                node.key() = to;
                m.points.insert(std::move(node));
            }
        },
        false, StructureChanged);
    if (selection_ == Selection::point(from)) {
        selection_ = Selection::point(to);
    }
    for (auto& extra : extras_) {
        if (extra == Selection::point(from)) {
            extra = Selection::point(to);
        }
    }
    changed(SelectionChanged);
    return true;
}

std::string EditorSession::pointerFor(const Selection& selection) const {
    const auto* authored = map();
    if (!authored) {
        return {};
    }
    switch (selection.kind) {
    case Selection::Kind::Placement: {
        const int index = placementIndex(*authored, selection.id);
        return index < 0 ? std::string() : "/placements/" + std::to_string(index);
    }
    case Selection::Kind::Zone: {
        const int index = zoneIndex(*authored, selection.id);
        return index < 0 ? std::string() : "/zones/" + std::to_string(index);
    }
    case Selection::Kind::Point:
        return authored->points.contains(selection.id)
                   ? (Json::json_pointer("/points") / selection.id).to_string()
                   : std::string();
    case Selection::Kind::Light:
        return validIndex(selection.index, authored->lights) ? "/lights/" + std::to_string(selection.index)
                                                             : std::string();
    case Selection::Kind::Emitter:
        return validIndex(selection.index, authored->emitters)
                   ? "/emitters/" + std::to_string(selection.index)
                   : std::string();
    case Selection::Kind::Collider:
        return validIndex(selection.index, authored->colliders)
                   ? "/colliders/" + std::to_string(selection.index)
                   : std::string();
    case Selection::Kind::Entity:
    case Selection::Kind::None:
        break;
    }
    return {};
}

Json EditorSession::objectJson(const Selection& selection) const {
    const auto pointer = pointerFor(selection);
    return pointer.empty() ? Json() : mapJson(pointer);
}

std::string EditorSession::setObjectJson(const Selection& selection, const Json& value) {
    const auto pointer = pointerFor(selection);
    if (pointer.empty()) {
        return "Nothing is selected";
    }
    const bool hasId =
        selection.kind == Selection::Kind::Placement || selection.kind == Selection::Kind::Zone;
    const std::string newId = hasId ? jsonString(value, "id", selection.id) : selection.id;
    const bool renamed = hasId && newId != selection.id;
    if (const auto problem =
            setMapJson(pointer, value, renamed ? StructureChanged | ValuesChanged : ValuesChanged);
        !problem.empty()) {
        return problem;
    }
    if (renamed) {
        const auto renamedTo = [&](Selection& s) {
            if (s == selection) {
                s.id = newId;
            }
        };
        renamedTo(selection_);
        std::for_each(extras_.begin(), extras_.end(), renamedTo);
        changed(SelectionChanged);
    }
    return {};
}

Json EditorSession::mapJson(const std::string& pointer) const {
    const auto* authored = map();
    if (!authored) {
        return {};
    }
    const auto whole = authored->toJson();
    const Json::json_pointer at(pointer);
    return whole.contains(at) ? whole.at(at) : Json();
}

std::string EditorSession::setMapJson(const std::string& pointer, const Json& value, unsigned what) {
    if (!world_) {
        return "No scene is open";
    }
    MapData next;
    try {
        auto whole = world_->authored().toJson();
        whole[Json::json_pointer(pointer)] = value;
        next = MapData::fromJson(whole);
    } catch (const std::exception& error) {
        return error.what();
    }
    if (next.id != world_->authored().id) {
        return "A scene's id is its folder name; use Rename Scene";
    }
    endGesture();
    const MapData before = world_->authored();
    edit([&](MapData& m) { m = next; }, false, what);
    respawnChanged(before);
    return {};
}
} // namespace fe
