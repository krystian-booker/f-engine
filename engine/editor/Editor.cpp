#include "engine/editor/Editor.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/editor/MapFiles.h"
#include "engine/editor/SceneEdits.h"
#include "engine/render/FrameRenderer.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <cstdlib>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

Editor::Editor(Engine& engine)
    : engine_(engine), catalog_(engine.lua().gameRoot(), engine.lua().builtinRoot()),
      importQueue_(std::make_shared<ImportQueue>()) {
    // For screenshots and tests: the folder the Assets panel opens on.
    if (const char* folder = std::getenv("FENGINE_EDITOR_ASSET_FOLDER")) {
        assetFolder_ = folder;
    }
    loadScript();
}

void Editor::attach(FrameRenderer& frame) {
    frame_ = &frame;
    gpu_ = &frame.gpu();
}

Editor::~Editor() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    auto& io = ImGui::GetIO();
    if (!layoutFile_.empty() && io.IniFilename == layoutFile_.c_str()) {
        ImGui::SaveIniSettingsToDisk(layoutFile_.c_str());
    }
    io.IniFilename = nullptr;
}

void Editor::toggle() {
    open_ = !open_;
    if (open_) {
        refreshSchemas();
        setMode(mode_);
    } else {
        close();
        if (leavePending_ && mapDialog_.cancelled) {
            // F1 over the leave-map prompt: the same as Cancel, so the waiting warp is answered.
            auto cancelled = std::move(mapDialog_.cancelled);
            mapDialog_ = {};
            cancelled();
        }
    }
}

void Editor::close() {
    for (const auto& entry : engine_.worlds()) {
        auto& w = *entry.second;
        std::erase_if(w.overlays(), [](const auto& overlay) { return overlay.first.starts_with("editor:"); });
        if (cursorShown_) {
            w.cursor().visible = false;
        }
    }
    cursorShown_ = false;
    paused_ = false;
    gridOverlayKey_.clear();
    zoneOverlayKey_.clear();
}

void Editor::setLayoutFile(std::filesystem::path file) {
    layoutFile_ = file.string();
    ImGui::GetIO().IniFilename = layoutFile_.empty() ? nullptr : layoutFile_.c_str();
    if (!layoutFile_.empty()) {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (std::filesystem::exists(file, error)) {
            ImGui::LoadIniSettingsFromDisk(layoutFile_.c_str());
            layoutBuilt_ = ImGui::FindWindowSettingsByID(ImHashStr("Scene")) != nullptr;
        }
    }
}

void Editor::setMode(Mode mode) {
    mode_ = mode;
    if (mode == Mode::Edit) {
        paused_ = pauseInEdit_;
        if (const auto w = world()) {
            if (!cameraAdopted_) {
                editorCamera_.adopt(w->camera());
                cameraAdopted_ = true;
            }
        }
    } else {
        paused_ = false;
        if (const auto w = world()) {
            std::erase_if(w->overlays(), [](const auto& overlay) { return overlay.first.starts_with("editor:"); });
            w->cursor().visible = false;
        }
        gridOverlayKey_.clear();
        zoneOverlayKey_.clear();
    }
}

std::string Editor::requestLeaveMap(const std::string& worldName) {
    if (leavePending_) {
        return "pending";
    }
    const auto w = engine_.world(worldName);
    if (!w || !history_.unsaved(*w)) {
        return "ok";
    }
    // Edits would be lost with the world: ask, in Edit mode (the game pauses
    // meanwhile; a modal only shows there), and go back to playing afterwards
    // unless the author stays to keep editing.
    const bool wasOpen = open_;
    const auto previous = mode_;
    if (!wasOpen) {
        toggle();
    }
    setMode(Mode::Edit);
    leavePending_ = true;
    leaveResult_.reset();
    // Afterwards the author is back where the prompt found them: in the game
    // with the editor closed, playing with it open, or editing.
    const auto restore = [this, wasOpen, previous] {
        leavePending_ = false;
        if (!wasOpen) {
            if (open_) {
                toggle();
            }
        } else if (previous == Mode::Play) {
            setMode(Mode::Play);
        }
    };
    mapDialog_ = {};
    mapDialog_.kind = MapDialog::Kind::Unsaved;
    mapDialog_.what = "The game is leaving this map (a door or a zone). Leave it";
    mapDialog_.then = [this, restore] {
        leaveResult_ = "continue";
        restore();
    };
    mapDialog_.cancelled = [this, restore] {
        leaveResult_ = "cancel";
        status_ = "Stayed on the map with its edits; that warp was dropped";
        restore();
    };
    return "pending";
}

bool Editor::capturesInput() const {
    if (!open_) {
        return false;
    }
    if (mode_ == Mode::Edit) {
        return true;
    }
    const auto& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse;
}

std::shared_ptr<World> Editor::world() {
    if (auto w = engine_.world(worldName_)) {
        return w;
    }
    auto focus = engine_.focusWorld();
    if (!focus && !engine_.worlds().empty()) {
        focus = engine_.worlds().begin()->second;
    }
    if (focus) {
        worldName_ = focus->name();
    }
    return focus;
}

sol::object Editor::devtool(const char* name) {
    // The game's editor hooks: the `editor` table of its entry module, read
    // raw (no metamethod of the game's runs here). A game without the table,
    // or without this hook, simply doesn't offer that action.
    if (!engine_.hasMain()) {
        return sol::lua_nil;
    }
    try {
        const auto module = engine_.lua().require(engine_.config().entry);
        if (module.get_type() == sol::type::table) {
            const auto hooks = module.as<sol::table>().raw_get<sol::object>("editor");
            if (hooks.get_type() == sol::type::table) {
                sol::object fn = hooks.as<sol::table>().raw_get<sol::object>(name);
                if (fn.get_type() == sol::type::function) {
                    return fn;
                }
            }
        }
    } catch (const std::exception&) {
        // The entry module failed to load; the error overlay already says so.
    }
    return sol::lua_nil;
}

bool Editor::succeeded(const sol::protected_function_result& result) {
    if (result.valid()) {
        return true;
    }
    const sol::error error = result;
    status_ = error.what();
    return false;
}

void Editor::refreshSchemas() {
    schemasFetched_ = Clock::now();
    schemas_ = Json::object();
    const auto describe = devtool("describe");
    if (describe == sol::lua_nil) {
        return;
    }
    const auto result = engine_.lua().invoke(describe);
    if (succeeded(result) && result.get_type() == sol::type::string) {
        const auto parsed = Json::parse(result.get<std::string>(), nullptr, false);
        schemas_ = parsed.is_object() ? parsed : Json::object();
    }
    log(LogLevel::Debug, "editor: schemas refreshed: " + std::to_string(jsonObject(schemas_, "entities").size()) +
                             " placement type(s), " + std::to_string(jsonArray(schemas_, "tables").size()) +
                             " table(s)" + (status_.empty() ? "" : "; status: " + status_));
}

std::string Editor::defaultType(const char* group) const {
    const auto types = jsonObject(schemas_, group);
    const auto preferred = jsonString(jsonObject(schemas_, "defaults"), group, {});
    if (!preferred.empty()) {
        return preferred;
    }
    return types.empty() ? std::string{} : types.begin().key();
}

const Json* Editor::fieldsFor(const std::string& type, std::initializer_list<const char*> groups) const {
    for (const auto* group : groups) {
        const auto found = schemas_.find(group);
        if (found != schemas_.end() && found->contains(type)) {
            const auto& spec = found->at(type);
            const auto fields = spec.find("fields");
            return fields == spec.end() ? nullptr : &*fields;
        }
    }
    return nullptr;
}

void Editor::edit(World& w, const std::function<void(MapData&)>& change) {
    if (!gesture_ && !gestureBakeSnapshot_) {
        gestureBakeSnapshot_ = bakedPlacementsJson(w.authored());
    }
    history_.apply(w, change, !gesture_);
    gesture_ = true;
    ++revision_;
}

void Editor::undo(World& w, bool forward) {
    try {
        const MapData before = w.authored();
        if (forward ? history_.redo(w) : history_.undo(w)) {
            ++revision_;
            status_ = forward ? "Redone" : "Undone";
            placementsChanged(w, before);
        }
    } catch (const std::exception& error) {
        status_ = std::string("Cannot restore: ") + error.what();
    }
    gesture_ = false;
}

void Editor::saveMap(World& w) {
    try {
        const auto json = w.authored().toJson();
        MapData::fromJson(json); // validate before writing
        writeJson(resolveInside(engine_.lua().gameRoot(), "maps/" + w.authored().id + "/map.json"), json);
        history_.markSaved(w);
        status_ = "Saved maps/" + w.authored().id + "/map.json";
    } catch (const std::exception& error) {
        status_ = std::string("Not saved: ") + error.what();
    }
}

void Editor::select(const Selection& selection) {
    selection_ = selection;
    extra_.clear();
}

void Editor::placementChanged(World& w, const std::string& id) {
    if (gesture_) {
        // Part of a drag or a typed edit: respawn once when it ends.
        if (std::find(pendingRespawns_.begin(), pendingRespawns_.end(), id) == pendingRespawns_.end()) {
            pendingRespawns_.push_back(id);
        }
        return;
    }
    const auto hook = devtool("placement_changed");
    if (hook == sol::lua_nil) {
        if (w.name() == PreviewWorld) {
            respawnPreview(w, id); // the editor's own stand-in for a map no game code is running
        } else {
            status_ = "Placement edited; the game shows it when the map loads again (its main.lua has no "
                      "editor.placement_changed hook)";
        }
        return;
    }
    // The call-out may replace or destroy entities; nothing here holds one.
    succeeded(engine_.lua().invoke(hook, w.name(), id));
}

void Editor::placementsChanged(World& w, const MapData& before) {
    for (const auto& id : changedPlacementIds(before, w.authored())) {
        placementChanged(w, id);
    }
}

void Editor::deleteOne(World& w, const Selection& s) {
    if (!selectionValid(w, s)) {
        return;
    }
    switch (s.kind) {
    case Selection::Kind::Placement:
        edit(w, [&](MapData& m) {
            std::erase_if(m.placements, [&](const Placement& p) { return p.id == s.id; });
        });
        gesture_ = false;
        placementChanged(w, s.id);
        break;
    case Selection::Kind::Zone:
        edit(w, [&](MapData& m) { std::erase_if(m.zones, [&](const Zone& z) { return z.id == s.id; }); });
        break;
    case Selection::Kind::Point:
        edit(w, [&](MapData& m) { m.points.erase(s.id); });
        break;
    case Selection::Kind::Light:
        edit(w, [&](MapData& m) {
            if (s.index >= 0 && s.index < static_cast<int>(m.lights.size())) {
                m.lights.erase(m.lights.begin() + s.index);
            }
        });
        break;
    case Selection::Kind::Emitter:
        edit(w, [&](MapData& m) {
            if (s.index >= 0 && s.index < static_cast<int>(m.emitters.size())) {
                m.emitters.erase(m.emitters.begin() + s.index);
            }
        });
        break;
    case Selection::Kind::Collider:
        edit(w, [&](MapData& m) {
            if (s.index >= 0 && s.index < static_cast<int>(m.colliders.size())) {
                m.colliders.erase(m.colliders.begin() + s.index);
            }
        });
        break;
    case Selection::Kind::Entity:
        if (w.alive(s.entity, s.serial)) {
            w.destroy(s.entity);
        }
        break;
    case Selection::Kind::None: break;
    }
}

void Editor::deleteSelection(World& w) {
    std::vector<Selection> all = extra_;
    if (!selection_.empty()) {
        all.push_back(selection_);
    }
    if (all.empty()) {
        return;
    }
    // Index-based kinds go highest index first, so earlier indices stay valid.
    std::sort(all.begin(), all.end(), [](const Selection& a, const Selection& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.index > b.index;
    });
    all.erase(std::unique(all.begin(), all.end()), all.end());
    const auto label = all.size() == 1 ? selectionLabel(w, all.front()) : std::to_string(all.size()) + " objects";
    gesture_ = false;
    for (const auto& s : all) {
        deleteOne(w, s);
        gesture_ = true; // one undo step for the whole group
    }
    gesture_ = false;
    status_ = "Deleted " + label;
    selection_ = {};
    extra_.clear();
}

void Editor::toggleExtra(const Selection& s) {
    if (s.empty() || s == selection_) {
        return;
    }
    if (const auto found = std::find(extra_.begin(), extra_.end(), s); found != extra_.end()) {
        extra_.erase(found);
    } else {
        extra_.push_back(s);
    }
}

bool Editor::isSelected(const Selection& s) const {
    return !s.empty() && (s == selection_ || std::find(extra_.begin(), extra_.end(), s) != extra_.end());
}

void Editor::moveExtras(World& w, glm::vec3 delta) {
    if (extra_.empty() || glm::length(delta) < 1e-6f) {
        return;
    }
    for (const auto& s : extra_) {
        if (s.kind == Selection::Kind::Entity) {
            if (w.alive(s.entity, s.serial)) {
                const auto& t = w.registry().get<Transform>(s.entity);
                editTransform(w, s.entity, t.position + delta, t.layer);
            }
            continue;
        }
        auto transform = selectionTransform(w, s);
        if (!transform) {
            continue;
        }
        transform->position += delta;
        edit(w, [&](MapData& m) { applySelectionTransform(m, s, *transform); });
        if (s.kind == Selection::Kind::Placement) {
            if (const auto* p = findPlacement(w.authored(), s.id)) {
                syncPlacementEntity(w, *p);
            }
        }
    }
}

void Editor::duplicateSelection(World& w) {
    const auto s = selection_;
    if (!selectionValid(w, s)) {
        return;
    }
    gesture_ = false;
    const auto& map = w.authored();
    if (!extra_.empty()) {
        // A group: copies of every placement in it, beside the originals.
        std::vector<Selection> group = extra_;
        group.push_back(s);
        std::vector<std::string> copies;
        for (const auto& member : group) {
            if (member.kind != Selection::Kind::Placement || !findPlacement(map, member.id)) {
                continue;
            }
            const auto copy = duplicatePlacement(*findPlacement(map, member.id), map.placements);
            edit(w, [&](MapData& m) { m.placements.push_back(copy); });
            copies.push_back(copy.id);
        }
        gesture_ = false;
        extra_.clear();
        for (const auto& id : copies) {
            placementChanged(w, id);
            if (selection_.kind != Selection::Kind::Placement || selection_.id != copies.front()) {
                selection_ = Selection::placement(copies.front());
            }
            if (id != copies.front()) {
                extra_.push_back(Selection::placement(id));
            }
        }
        status_ = "Duplicated " + std::to_string(copies.size()) + " placement(s)";
        return;
    }
    switch (s.kind) {
    case Selection::Kind::Placement: {
        const auto copy = duplicatePlacement(*findPlacement(map, s.id), map.placements);
        edit(w, [&](MapData& m) { m.placements.push_back(copy); });
        gesture_ = false;
        selection_ = Selection::placement(copy.id);
        placementChanged(w, copy.id);
        break;
    }
    case Selection::Kind::Zone: {
        auto copy = *findZone(map, s.id);
        copy.id = uniqueId(copy.id, map.zones);
        copy.min.x += 1;
        copy.max.x += 1;
        edit(w, [&](MapData& m) { m.zones.push_back(copy); });
        selection_ = Selection::zone(copy.id);
        break;
    }
    case Selection::Kind::Point: {
        auto copy = map.points.at(s.id);
        copy.position.x += 1;
        copy.position = clampToGrid(map.grid, copy.position);
        std::string name = s.id + "_copy";
        for (int n = 2; map.points.contains(name); ++n) {
            name = s.id + "_copy" + std::to_string(n);
        }
        edit(w, [&](MapData& m) { m.points[name] = copy; });
        selection_ = Selection::point(name);
        break;
    }
    case Selection::Kind::Light: {
        auto copy = map.lights[static_cast<std::size_t>(s.index)];
        copy.position.x += 1;
        edit(w, [&](MapData& m) { m.lights.push_back(copy); });
        selection_ = Selection::light(static_cast<int>(map.lights.size()) - 1);
        break;
    }
    case Selection::Kind::Emitter: {
        auto copy = map.emitters[static_cast<std::size_t>(s.index)];
        copy.position.x += 1;
        edit(w, [&](MapData& m) { m.emitters.push_back(copy); });
        selection_ = Selection::emitter(static_cast<int>(map.emitters.size()) - 1);
        break;
    }
    case Selection::Kind::Collider: {
        auto copy = map.colliders[static_cast<std::size_t>(s.index)];
        copy.min.x += 1;
        copy.max.x += 1;
        edit(w, [&](MapData& m) { m.colliders.push_back(copy); });
        selection_ = Selection::collider(static_cast<int>(map.colliders.size()) - 1);
        break;
    }
    case Selection::Kind::Entity:
    case Selection::Kind::None: return;
    }
    gesture_ = false;
    status_ = "Duplicated as " + selectionLabel(w, selection_);
}

bool Editor::editJson(const char* label, Json& value, float height) {
    // While the field is active ImGui keeps the typed text; invalid JSON is
    // not applied, and leaving the field shows the last valid value again.
    const auto id = ImGui::GetID(label);
    auto* invalid = ImGui::GetStateStorage();
    std::string text = value.dump(2);
    bool changed = false;
    if (ImGui::InputTextMultiline(label, &text, ImVec2(-1, height))) {
        auto parsed = Json::parse(text, nullptr, false);
        invalid->SetBool(id, parsed.is_discarded());
        if (!parsed.is_discarded()) {
            value = std::move(parsed);
            changed = true;
        }
    }
    if (!ImGui::IsItemActive()) {
        invalid->SetBool(id, false);
    } else if (invalid->GetBool(id)) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Invalid JSON (not applied)");
    }
    return changed;
}

namespace {
// A JSON value of the type a spec asks for, filled from the spec's default.
Json blankFor(const Json& spec) {
    if (const auto fallback = spec.find("default"); fallback != spec.end()) {
        return *fallback;
    }
    const auto type = jsonString(spec, "type", "any");
    if (type == "number" || type == "integer") {
        return 0;
    }
    if (type == "boolean") {
        return false;
    }
    if (type == "list") {
        return Json::array();
    }
    if (type == "record" || type == "map") {
        Json out = Json::object();
        if (type == "record") {
            // Held in a local: items() keeps a reference to the object it walks.
            const auto fields = jsonObject(spec, "fields");
            for (const auto& [name, field] : fields.items()) {
                if (!jsonBool(field, "optional", false)) {
                    out[name] = blankFor(field);
                }
            }
        }
        return out;
    }
    if (type == "enum") {
        const auto options = jsonArray(spec, "options");
        return options.empty() ? Json("") : options.front();
    }
    return "";
}
AssetKind assetKindNamed(const std::string& kind) {
    return kind == "sprite"  ? AssetKind::Sprite
           : kind == "image" ? AssetKind::Image
           : kind == "model" ? AssetKind::Model
           : kind == "audio" ? AssetKind::Audio
           : kind == "lut"   ? AssetKind::Lut
           : kind == "shader" ? AssetKind::Shader
                              : AssetKind::Other;
}
} // namespace

bool Editor::editValue(const char* label, const Json& spec, Json& value, int depth) {
    const auto type = jsonString(spec, "type", "any");
    const bool optional = jsonBool(spec, "optional", false);
    bool changed = false;
    ImGui::PushID(label);
    if (type == "asset" && (value.is_string() || value.is_null())) {
        std::string text = value.is_string() ? value.get<std::string>() : std::string{};
        if (assetPicker(label, text, assetKindNamed(jsonString(spec, "asset", "sprite")), optional)) {
            value = text;
            changed = true;
        }
    } else if (type == "module" && (value.is_string() || value.is_null())) {
        std::string text = value.is_string() ? value.get<std::string>() : std::string{};
        if (modulePicker(label, text, jsonString(spec, "folder", ""), optional)) {
            value = text;
            changed = true;
        }
    } else if (type == "map_id" && (value.is_string() || value.is_null())) {
        std::string text = value.is_string() ? value.get<std::string>() : std::string{};
        if (mapPicker(label, text, optional)) {
            value = text;
            changed = true;
        }
    } else if (type == "ref" && (value.is_string() || value.is_null())) {
        std::string text = value.is_string() ? value.get<std::string>() : std::string{};
        if (refPicker(label, text, jsonString(spec, "table", {}), optional)) {
            value = text;
            changed = true;
        }
    } else if (type == "string" && (value.is_string() || value.is_null())) {
        std::string text = value.is_string() ? value.get<std::string>() : std::string{};
        if (ImGui::InputText(label, &text)) {
            value = text;
            changed = true;
        }
    } else if ((type == "number" || type == "integer") && (value.is_number() || value.is_null())) {
        double number = value.is_number() ? value.get<double>() : jsonNumber(spec, "default", 0.0);
        if (ImGui::InputDouble(label, &number)) {
            value = type == "integer" ? Json(static_cast<long long>(number)) : Json(number);
            changed = true;
        }
    } else if (type == "boolean" && (value.is_boolean() || value.is_null())) {
        bool flag = value.is_boolean() ? value.get<bool>() : jsonBool(spec, "default", false);
        if (ImGui::Checkbox(label, &flag)) {
            value = flag;
            changed = true;
        }
    } else if (type == "enum" && (value.is_string() || value.is_null())) {
        const std::string current = value.is_string() ? value.get<std::string>() : jsonString(spec, "default", {});
        if (ImGui::BeginCombo(label, current.c_str())) {
            for (const auto& option : jsonArray(spec, "options")) {
                if (!option.is_string()) {
                    continue;
                }
                const auto text = option.get<std::string>();
                if (ImGui::Selectable(text.c_str(), text == current)) {
                    value = text;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } else if (type == "list" && (value.is_array() || value.is_null()) && depth < 6) {
        if (value.is_null()) {
            value = Json::array();
        }
        const auto of = jsonObject(spec, "of");
        const auto ofType = jsonString(of, "type", "any");
        const bool rows = ofType == "record" || ofType == "list" || ofType == "map" || ofType == "any" ||
                          ofType == "table";
        const auto header = std::string(label) + " (" + std::to_string(value.size()) + ")";
        if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            int remove = -1, up = -1;
            for (std::size_t i = 0; i < value.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("x")) {
                    remove = static_cast<int>(i);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Remove");
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton(ICON_FA_ARROW_UP)) {
                    up = static_cast<int>(i);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                const auto name = "#" + std::to_string(i + 1);
                if (rows) {
                    if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        changed |= editValue("##element", of, value[i], depth + 1);
                        ImGui::TreePop();
                    }
                } else {
                    changed |= editValue(name.c_str(), of, value[i], depth + 1);
                }
                ImGui::PopID();
            }
            if (remove >= 0) {
                value.erase(static_cast<std::size_t>(remove));
                changed = true;
            } else if (up > 0) {
                std::swap(value[static_cast<std::size_t>(up)], value[static_cast<std::size_t>(up) - 1]);
                changed = true;
            }
            if (ImGui::SmallButton(ICON_FA_PLUS " Add")) {
                value.push_back(blankFor(of));
                changed = true;
            }
            ImGui::TreePop();
        }
    } else if (type == "record" && (value.is_object() || value.is_null()) && depth < 6) {
        if (value.is_null()) {
            value = Json::object();
        }
        if (depth == 0 || ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            const auto fields = spec.find("fields");
            changed |= editProps(fields != spec.end() ? &*fields : nullptr, value);
            if (depth != 0) {
                ImGui::TreePop();
            }
        }
    } else if (type == "map" && (value.is_object() || value.is_null()) && depth < 6) {
        if (value.is_null()) {
            value = Json::object();
        }
        const auto of = jsonObject(spec, "of");
        const auto header = std::string(label) + " (" + std::to_string(value.size()) + ")";
        if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            std::string remove;
            for (auto& [key, child] : value.items()) {
                ImGui::PushID(key.c_str());
                if (ImGui::SmallButton("x")) {
                    remove = key;
                }
                ImGui::SameLine();
                changed |= editValue(key.c_str(), of, child, depth + 1);
                ImGui::PopID();
            }
            if (!remove.empty()) {
                value.erase(remove);
                changed = true;
            }
            static std::map<ImGuiID, std::string> newKeys;
            auto& newKey = newKeys[ImGui::GetID("newkey")];
            ImGui::SetNextItemWidth(140);
            ImGui::InputTextWithHint("##newkey", "key", &newKey);
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_PLUS " Add") && !newKey.empty() && !value.contains(newKey)) {
                value[newKey] = blankFor(of);
                newKey.clear();
                changed = true;
            }
            ImGui::TreePop();
        }
    } else {
        // Unknown kinds, and values of another type than the schema says.
        if (depth == 0) {
            ImGui::TextUnformatted(label);
        } else {
            ImGui::TextDisabled("%s", label);
        }
        Json copy = value;
        if (editJson("##json", copy, 80)) {
            value = std::move(copy);
            changed = true;
        }
    }
    if (const auto doc = spec.find("doc"); doc != spec.end() && doc->is_string() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", doc->get<std::string>().c_str());
    }
    ImGui::PopID();
    return changed;
}

bool Editor::editProps(const Json* schema, Json& props) {
    if (!schema || !schema->is_object()) {
        return editJson("Props", props);
    }
    const auto& fields = *schema;
    if (!props.is_object()) {
        return editJson("Props", props);
    }
    bool changed = false;
    for (const auto& [name, spec] : fields.items()) {
        if (!spec.is_object()) {
            continue;
        }
        const bool optional = jsonBool(spec, "optional", false);
        const auto label = name + (optional ? "" : " *");
        ImGui::PushID(name.c_str());
        if (optional && !props.contains(name)) {
            // Absent optional fields stay absent until asked for, so the
            // default applies and the file stays small.
            ImGui::TextDisabled("%s", name.c_str());
            if (const auto doc = spec.find("doc"); doc != spec.end() && doc->is_string() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", doc->get<std::string>().c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_PLUS)) {
                props[name] = blankFor(spec);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Set (uses the default until then)");
            }
            ImGui::PopID();
            continue;
        }
        if (!props.contains(name)) {
            // A required field the file lacks: show its default (or a blank)
            // and write it only when the user changes it, so selecting an
            // object never edits the map by itself.
            Json value = blankFor(spec);
            if (editValue(label.c_str(), spec, value)) {
                props[name] = value;
                changed = true;
            }
            ImGui::PopID();
            continue;
        }
        changed |= editValue(label.c_str(), spec, props[name]);
        if (optional) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                props.erase(name);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove (use the default)");
            }
        }
        ImGui::PopID();
    }
    // Keys the schema does not know keep a JSON editor of their own.
    Json others = Json::object();
    for (const auto& [key, child] : props.items()) {
        if (!fields.contains(key)) {
            others[key] = child;
        }
    }
    if (!others.empty() || ImGui::GetIO().KeyAlt) {
        if (ImGui::TreeNode("Other fields (JSON)")) {
            if (editJson("##others", others, 100)) {
                std::vector<std::string> gone;
                for (const auto& [key, child] : props.items()) {
                    if (!fields.contains(key) && !others.contains(key)) {
                        gone.push_back(key);
                    }
                }
                for (const auto& key : gone) {
                    props.erase(key);
                }
                for (const auto& [key, child] : others.items()) {
                    props[key] = child;
                }
                changed = true;
            }
            ImGui::TreePop();
        }
    }
    return changed;
}

void Editor::drawErrors() {
    const auto& errors = engine_.lua().errors();
    if (errors.empty() || errors.size() == errorsSeen_) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Script error")) {
        ImGui::TextWrapped("%s", errors.back().message.c_str());
        if (ImGui::Button("Dismiss")) {
            errorsSeen_ = errors.size();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            engine_.lua().clearErrors();
            errorsSeen_ = 0;
        }
        ImGui::SameLine();
        ImGui::Text("%zu error(s)", errors.size());
    }
    ImGui::End();
}

void Editor::dropFile(const std::filesystem::path& file, glm::vec2 windowPoint) {
    droppedFiles_.emplace_back(file, windowPoint);
}

void Editor::draw(const EditorFrame& frame) {
    if (!open_) {
        droppedFiles_.clear();
        runScript(); // a walkthrough keeps going while the editor is closed (F1, a leave-map prompt's answer)
        return;
    }
    alpha_ = frame.alpha;
    if (gesture_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !gizmoUsing_ &&
        !groundDragging_) {
        gesture_ = false;
        // The stroke or drag is over: record the map it left, once, and
        // respawn the placements whose edits waited for it.
        if (const auto w = world()) {
            history_.settle(*w);
            const auto pending = std::move(pendingRespawns_);
            pendingRespawns_.clear();
            for (const auto& id : pending) {
                placementChanged(*w, id);
            }
        }
    }
    if (!gesture_ && !gizmoUsing_ && !groundDragging_ && gestureBakeSnapshot_) {
        // The edit (or the gesture of edits) is complete: derive walkability
        // again where a placed model changed.
        if (const auto w = world()) {
            autoRebake(*w);
        } else {
            gestureBakeSnapshot_.reset();
        }
    }
    // Games register tables and types while starting; look again until they have.
    if ((jsonArray(schemas_, "tables").empty() || jsonObject(schemas_, "entities").empty()) &&
        Clock::now() - schemasFetched_ > std::chrono::milliseconds(300)) {
        refreshSchemas();
    }
    if (Clock::now() - catalogScanned_ > std::chrono::seconds(2)) {
        const bool first = catalog_.entries().empty();
        catalogScanned_ = Clock::now();
        catalog_.scan();
        if (first && assetFolder_.empty()) {
            // Open the Assets panel on the art, the folder an artist wants first.
            const auto folders = catalog_.folders("");
            if (std::find(folders.begin(), folders.end(), "sprites") != folders.end()) {
                assetFolder_ = "sprites";
            }
        }
    }
    {
        ImportQueue::Purpose purpose;
        std::string folder;
        const auto chosen = takeChosenFiles(purpose, folder);
        if (!chosen.empty()) {
            if (purpose == ImportQueue::Purpose::Assets) {
                importFiles(chosen, folder);
            } else if (purpose == ImportQueue::Purpose::NewMapGeometry) {
                mapDialog_.geometry = chosen.front().string();
            } else if (const auto w = world()) {
                importGeometry(*w, chosen.front());
            }
        }
    }
    const bool cursorWas = cursorShown_;
    cursorShown_ = false;
    // viewportHovered_ keeps last frame's value until drawViewport runs: the
    // panels drawn before it (the Map panel's brush) read it.
    runScript();
    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);
    try {
        ImGuizmo::BeginFrame();
        if (mode_ == Mode::Edit) {
            drawEditMode(frame);
        } else {
            drawPlayBar();
        }
    } catch (const std::exception& error) {
        auto& io = ImGui::GetIO();
        const bool assertOnError = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::ErrorRecoveryTryToRecoverState(&recovery);
        io.ConfigErrorRecoveryEnableAssert = assertOnError;
        status_ = std::string("Editor error: ") + error.what();
    }
    droppedFiles_.clear();
    if (cursorWas && !cursorShown_) {
        if (const auto w = world()) {
            w->cursor().visible = false;
        }
    }
    if (!open_) {
        close(); // closed from the menu
    }
}
} // namespace fe
