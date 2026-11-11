#include "engine/core/Paths.h"
#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/editor/MapFiles.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

namespace {
constexpr float StatusBarHeight = 26;
}

void Editor::buildLayout(unsigned int dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) |
                                             ImGuiDockNodeFlags_PassthruCentralNode);
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
    ImGuiID centre = dockspace, left, right, bottom;
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, &left, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.30f, &right, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.28f, &bottom, &centre);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Map", right);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Data", bottom);
    ImGui::DockBuilderDockWindow("Scripts", bottom);
    ImGui::DockBuilderDockWindow("Profiler", bottom);
    ImGui::DockBuilderDockWindow("Launch", bottom);
    ImGui::DockBuilderFinish(dockspace);
    layoutBuilt_ = true;
}

void Editor::drawEditMode(const EditorFrame& frame) {
    if (handles_.triggerBadges.empty()) {
        handles_.triggerBadges = {{"action", ICON_FA_COMMENT " talk"},
                                  {"touch", ICON_FA_SHOE_PRINTS " touch"},
                                  {"auto", ICON_FA_BOLT " auto"},
                                  {"parallel", ICON_FA_REPEAT " loop"}};
    }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
    ImGui::Begin("##fe_editor_host", nullptr, flags);
    ImGui::PopStyleVar(3);
    drawMenuBar(frame);
    dockspaceId_ = ImGui::GetID("fe_dockspace");
    if (!layoutBuilt_) {
        buildLayout(dockspaceId_);
    }
    auto size = ImGui::GetContentRegionAvail();
    size.y = std::max(1.0f, size.y - StatusBarHeight);
    ImGui::DockSpace(dockspaceId_, size,
                     ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode);
    drawStatusBar();
    ImGui::End();

    const auto w = world(); // held for the frame (see Editor::world)
    if (ImGui::Begin("Hierarchy")) {
        drawHierarchy();
    }
    ImGui::End();
    if (ImGui::Begin("Inspector")) {
        drawInspector();
    }
    ImGui::End();
    if (ImGui::Begin("Map")) {
        drawMapTab();
    }
    ImGui::End();
    if (ImGui::Begin("Assets")) {
        drawAssetsPanel();
    }
    ImGui::End();
    if (ImGui::Begin("Data")) {
        drawDataTab();
    }
    ImGui::End();
    if (ImGui::Begin("Console")) {
        drawConsoleTab();
    }
    ImGui::End();
    if (ImGui::Begin("Scripts")) {
        drawScriptsPanel();
    }
    ImGui::End();
    if (ImGui::Begin("Profiler")) {
        drawProfilerTab(frame.frameMilliseconds, frame.drawCalls);
    }
    ImGui::End();
    if (ImGui::Begin("Launch")) {
        drawLaunchTab();
    }
    ImGui::End();
    if (w) {
        updateZoneOverlay(*w);
    }
    drawViewport(frame);
    drawMapDialogs(w.get());
    handleShortcuts(w.get());
}

void Editor::confirmDiscard(World* w, const char* what, std::function<void()> action) {
    if (w && history_.unsaved(*w)) {
        mapDialog_ = {};
        mapDialog_.kind = MapDialog::Kind::Unsaved;
        mapDialog_.what = what;
        mapDialog_.then = std::move(action);
        return;
    }
    action();
}

void Editor::openMap(const std::string& id) {
    const auto playFrom = devtool("play_from");
    if (playFrom == sol::lua_nil) {
        openPreview(id);
        return;
    }
    const auto points = mapPoints(id);
    const auto point = std::find(points.begin(), points.end(), "start") != points.end() ? std::string("start")
                       : points.empty()                                                 ? std::string()
                                                                                        : points.front();
    if (point.empty()) {
        status_ = "The map '" + id + "' has no points to start from";
        return;
    }
    if (succeeded(engine_.lua().invoke(playFrom, id, point))) {
        selection_ = {};
        extra_.clear();
        cameraAdopted_ = false; // frame the new map from its game view
        status_ = "Opened " + id;
    }
}

// A game with no `editor.play_from` hook (a new project with an empty
// main.lua, say) still opens its maps: the editor loads the map into a world
// of its own and stands simple entities in for the placements that name a
// model or a sprite, so the scene can be laid out before any game code exists.
void Editor::openPreview(const std::string& id) {
    try {
        if (engine_.world(PreviewWorld)) {
            engine_.unloadWorld(PreviewWorld);
        }
        const auto w = engine_.loadWorld(PreviewWorld, id);
        for (const auto& placement : w->authored().placements) {
            respawnPreview(*w, placement.id);
        }
        worldName_ = PreviewWorld;
        selection_ = {};
        extra_.clear();
        cameraAdopted_ = false;
        status_ = "Opened " + id + " (preview: this game defines no editor.play_from hook)";
    } catch (const std::exception& error) {
        status_ = std::string("Cannot open ") + id + ": " + error.what();
    }
}

void Editor::respawnPreview(World& w, const std::string& id) {
    if (const auto existing = w.find(id); existing != entt::null) {
        w.destroy(existing);
    }
    const auto& placements = w.authored().placements;
    const auto found =
        std::find_if(placements.begin(), placements.end(), [&](const Placement& p) { return p.id == id; });
    if (found == placements.end()) {
        return;
    }
    const auto model = found->props.find("model");
    const auto sprite = found->props.find("sprite");
    const bool hasModel = model != found->props.end() && model->is_string();
    const bool hasSprite = sprite != found->props.end() && sprite->is_string();
    if (!hasModel && !hasSprite) {
        return; // nothing to show; its handle still marks the spot
    }
    try {
        const auto entity = w.spawn(found->id, found->position, found->layer);
        auto& transform = w.registry().get<Transform>(entity);
        transform.yaw = transform.previousYaw = glm::radians(found->yaw);
        transform.scale = found->scale;
        transform.facing = found->facing;
        if (hasModel) {
            MeshInstance mesh;
            mesh.model = model->get<std::string>();
            w.registry().emplace<MeshInstance>(entity, std::move(mesh));
        } else {
            Sprite visual;
            visual.animation = sprite->get<std::string>();
            w.registry().emplace<Sprite>(entity, std::move(visual));
        }
    } catch (const std::exception& error) {
        status_ = std::string("Preview of ") + id + ": " + error.what();
    }
}

void Editor::newMap() {
    mapDialog_ = {};
    mapDialog_.kind = MapDialog::Kind::New;
    mapDialog_.id = "new_map";
    mapDialog_.name = "New Map";
    for (int n = 2; mapExists(engine_.lua().gameRoot(), mapDialog_.id); ++n) {
        mapDialog_.id = "new_map" + std::to_string(n);
    }
}

void Editor::drawMapDialogs(World* w) {
    auto& d = mapDialog_;
    if (d.kind == MapDialog::Kind::None) {
        return;
    }
    const auto& root = engine_.lua().gameRoot();
    const char* title = d.kind == MapDialog::Kind::New      ? "New map"
                        : d.kind == MapDialog::Kind::SaveAs ? "Save map as"
                        : d.kind == MapDialog::Kind::Rename ? "Rename map"
                        : d.kind == MapDialog::Kind::Delete ? "Delete map"
                                                            : "Unsaved edits";
    ImGui::OpenPopup(title);
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!open) {
            auto cancelled = std::move(d.cancelled);
            d = {};
            if (cancelled) {
                cancelled();
            }
        }
        return;
    }
    const auto close = [&] {
        d = {};
        ImGui::CloseCurrentPopup();
    };
    const auto idField = [&] {
        ImGui::InputText("Id (folder name)", &d.id);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("a-z, 0-9, _ and -; doors and scripts name the map by it");
        }
        if (!validMapId(d.id)) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Use a-z, 0-9, _ and -");
            return false;
        }
        return true;
    };
    const auto usedBy = [&](const std::string& id) {
        const auto& users = catalog_.references("maps/" + id + "/map.json");
        if (!users.empty()) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1),
                               ICON_FA_TRIANGLE_EXCLAMATION " Named by %zu place(s):", users.size());
            for (std::size_t i = 0; i < users.size() && i < 8; ++i) {
                ImGui::BulletText("%s  %s", users[i].file.c_str(), users[i].where.c_str());
            }
        }
    };
    switch (d.kind) {
    case MapDialog::Kind::New: {
        const bool valid = idField() && !mapExists(root, d.id);
        if (validMapId(d.id) && mapExists(root, d.id)) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "A map with this id exists");
        }
        ImGui::InputText("Name", &d.name);
        ImGui::DragInt("Width (cells)", &d.width, 0.5f, 1, 1024);
        ImGui::DragInt("Depth (cells)", &d.depth, 0.5f, 1, 1024);
        ImGui::InputTextWithHint("Geometry", "(none: flat ground)", &d.geometry);
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse...")) {
            openImportDialog(ImportQueue::Purpose::NewMapGeometry);
        }
        ImGui::TextDisabled(
            "A flat, walkable map with a start point in the middle. Add a Blender export as its\n"
            "geometry now or later (File > Import geometry), then bake walkability from it.");
        if (!d.problem.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", d.problem.c_str());
        }
        ImGui::BeginDisabled(!valid);
        const bool create = ImGui::Button(ICON_FA_CHECK " Create and open");
        ImGui::EndDisabled();
        if (create) {
            try {
                auto map = makeBlankMap(d.id, d.name, d.width, d.depth);
                writeMap(root, map,
                         d.geometry.empty() ? std::filesystem::path() : std::filesystem::path(d.geometry));
                catalog_.scan();
                const auto id = d.id;
                close();
                openMap(id);
                return ImGui::EndPopup();
            } catch (const std::exception& error) {
                d.problem = error.what();
            }
        }
        break;
    }
    case MapDialog::Kind::SaveAs: {
        if (!w) {
            close();
            break;
        }
        const bool valid = idField() && !mapExists(root, d.id);
        if (validMapId(d.id) && mapExists(root, d.id)) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "A map with this id exists");
        }
        ImGui::InputText("Name", &d.name);
        ImGui::TextDisabled("Writes the map as you see it (unsaved edits included) under the new id,\n"
                            "copies its geometry, and opens it. The current map's file is left as it was.");
        if (!d.problem.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", d.problem.c_str());
        }
        ImGui::BeginDisabled(!valid);
        const bool save = ImGui::Button(ICON_FA_CHECK " Save as and open");
        ImGui::EndDisabled();
        if (save) {
            try {
                MapData copy = w->authored();
                copy.id = d.id;
                copy.name = d.name.empty() ? d.id : d.name;
                const auto geometry = root / "maps" / w->authored().id / w->authored().geometry;
                writeMap(root, copy, isFile(geometry) ? geometry : std::filesystem::path());
                catalog_.scan();
                const auto id = d.id;
                close();
                openMap(id);
                return ImGui::EndPopup();
            } catch (const std::exception& error) {
                d.problem = error.what();
            }
        }
        break;
    }
    case MapDialog::Kind::Rename: {
        if (!w) {
            close();
            break;
        }
        const auto from = w->authored().id;
        const bool valid = idField() && (d.id == from || !mapExists(root, d.id));
        ImGui::InputText("Name", &d.name);
        usedBy(from);
        ImGui::TextDisabled("Doors and props that name '%s' are not rewritten; the Assets panel lists them.",
                            from.c_str());
        if (!d.problem.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", d.problem.c_str());
        }
        ImGui::BeginDisabled(!valid || history_.unsaved(*w));
        const bool rename = ImGui::Button(ICON_FA_CHECK " Rename");
        ImGui::EndDisabled();
        if (rename) {
            try {
                if (d.id != from) {
                    renameMap(root, from, d.id);
                }
                if (d.name != w->authored().name) {
                    auto json = readJson(root / "maps" / d.id / "map.json");
                    json["name"] = d.name;
                    writeJson(root / "maps" / d.id / "map.json", json);
                }
                catalog_.scan();
                const auto id = d.id;
                close();
                openMap(id);
                return ImGui::EndPopup();
            } catch (const std::exception& error) {
                d.problem = error.what();
            }
        }
        if (history_.unsaved(*w)) {
            ImGui::SameLine();
            ImGui::TextDisabled("Save the map first");
        }
        break;
    }
    case MapDialog::Kind::Delete: {
        ImGui::Text("Delete the map '%s' and everything in its folder?", d.id.c_str());
        ImGui::TextDisabled("This cannot be undone from the editor.");
        usedBy(d.id);
        if (!d.problem.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", d.problem.c_str());
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1));
        if (ImGui::Button(ICON_FA_TRASH " Delete")) {
            try {
                deleteMap(root, d.id);
                catalog_.scan();
                status_ = "Deleted map " + d.id;
                close();
                ImGui::PopStyleColor();
                return ImGui::EndPopup();
            } catch (const std::exception& error) {
                d.problem = error.what();
            }
        }
        ImGui::PopStyleColor();
        break;
    }
    case MapDialog::Kind::Unsaved: {
        ImGui::Text("The map has unsaved edits. %s anyway?", d.what.c_str());
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save, then continue")) {
            if (w) {
                saveMap(*w);
            }
            auto then = std::move(d.then);
            close();
            if (then) {
                then();
            }
            return ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard edits and continue")) {
            auto then = std::move(d.then);
            close();
            if (then) {
                then();
            }
            return ImGui::EndPopup();
        }
        break;
    }
    case MapDialog::Kind::None:
        break;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        auto cancelled = std::move(d.cancelled);
        close();
        if (cancelled) {
            cancelled();
        }
    }
    ImGui::EndPopup();
}

void Editor::drawPlayBar() {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 6), ImGuiCond_Always,
        ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##fe_playbar", nullptr, flags)) {
        if (ImGui::Button(ICON_FA_PEN_TO_SQUARE " Edit (F5)")) {
            setMode(Mode::Edit);
        }
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_PAUSE " Pause", &paused_);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK " Close (F1)")) {
            open_ = false;
        }
        if (!status_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status_.c_str());
        }
    }
    ImGui::End();
    handleShortcuts(nullptr);
}

void Editor::drawMenuBar(const EditorFrame& /*frame*/) {
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    const auto w = world();
    const bool hasWorld = w != nullptr;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(ICON_FA_FILE " New map...")) {
            confirmDiscard(w.get(), "make a new map", [this] { newMap(); });
        }
        if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " Open map")) {
            for (const auto& map : catalog_.maps()) {
                const bool current = hasWorld && map.id == w->authored().id;
                const auto label = map.id + "  (" + map.name + ")";
                if (ImGui::MenuItem(label.c_str(), nullptr, current, !current)) {
                    const auto id = map.id;
                    confirmDiscard(w.get(), "open another map", [this, id] { openMap(id); });
                }
            }
            if (catalog_.maps().empty()) {
                ImGui::TextDisabled("No maps in the game folder");
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Save map", "Ctrl+S", false, hasWorld)) {
            saveMap(*w);
        }
        if (ImGui::MenuItem("Save map as...", nullptr, false, hasWorld)) {
            mapDialog_ = {};
            mapDialog_.kind = MapDialog::Kind::SaveAs;
            mapDialog_.id = w->authored().id + "_copy";
            mapDialog_.name = w->authored().name + " (copy)";
        }
        if (ImGui::MenuItem("Rename map...", nullptr, false, hasWorld)) {
            mapDialog_ = {};
            mapDialog_.kind = MapDialog::Kind::Rename;
            mapDialog_.id = w->authored().id;
            mapDialog_.name = w->authored().name;
        }
        if (ImGui::BeginMenu(ICON_FA_TRASH " Delete map")) {
            for (const auto& map : catalog_.maps()) {
                const bool current = hasWorld && map.id == w->authored().id;
                if (ImGui::MenuItem(map.id.c_str(), nullptr, false, !current)) {
                    mapDialog_ = {};
                    mapDialog_.kind = MapDialog::Kind::Delete;
                    mapDialog_.id = map.id;
                }
            }
            if (hasWorld) {
                ImGui::TextDisabled("(open another map to delete %s)", w->authored().id.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CUBES " Import geometry (.glb)...", nullptr, false, hasWorld)) {
            openImportDialog(ImportQueue::Purpose::Geometry);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Copies a Blender export in as maps/%s/geometry.glb and imports its markers",
                              hasWorld ? w->authored().id.c_str() : "<map>");
        }
        if (ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import assets...")) {
            openImportDialog(ImportQueue::Purpose::Assets);
        }
        if (ImGui::MenuItem("Rescan assets")) {
            catalog_.scan();
            status_ = "Scanned " + std::to_string(catalog_.entries().size()) + " files";
        }
        if (ImGui::MenuItem("Reload schemas")) {
            refreshSchemas();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close editor", "F1")) {
            open_ = false;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool canUndo = hasWorld && history_.undoCount(*w) > 0;
        const bool canRedo = hasWorld && history_.redoCount(*w) > 0;
        if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT " Undo", "Ctrl+Z", false, canUndo)) {
            undo(*w, false);
        }
        if (ImGui::MenuItem(ICON_FA_ROTATE_RIGHT " Redo", "Ctrl+Y", false, canRedo)) {
            undo(*w, true);
        }
        ImGui::Separator();
        const bool selected = hasWorld && selectionValid(*w, selection_);
        if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate", "Ctrl+D", false, selected)) {
            duplicateSelection(*w);
        }
        if (ImGui::MenuItem(ICON_FA_TRASH " Delete", "Del", false, selected)) {
            deleteSelection(*w);
        }
        if (ImGui::MenuItem("Deselect", "Esc", false, !selection_.empty())) {
            selection_ = {};
        }
        ImGui::Separator();
        ImGui::MenuItem("Snap to grid", "", &snap_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Map", hasWorld)) {
        if (ImGui::MenuItem(ICON_FA_PERSON " Add placement")) {
            addPlacement(*w);
        }
        if (ImGui::MenuItem(ICON_FA_VECTOR_SQUARE " Add zone")) {
            addZone(*w);
        }
        if (ImGui::MenuItem(ICON_FA_FLAG " Add point")) {
            addPoint(*w, newPoint_);
        }
        if (ImGui::MenuItem(ICON_FA_LIGHTBULB " Add light")) {
            addLight(*w);
        }
        if (ImGui::MenuItem(ICON_FA_FIRE " Add emitter")) {
            addEmitter(*w);
        }
        if (ImGui::MenuItem(ICON_FA_CUBE " Add collider")) {
            addCollider(*w);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES " Bake walkability now")) {
            rebake(*w, std::nullopt, false);
        }
        if (ImGui::MenuItem("Import Blender markers")) {
            importMarkers(*w);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Grid overlay (walkability)", "", &showGrid_);
        ImGui::MenuItem("Zones", "", &handles_.zones);
        ImGui::MenuItem("Points", "", &handles_.points);
        ImGui::MenuItem("Lights", "", &handles_.lights);
        ImGui::MenuItem("Emitters", "", &handles_.emitters);
        ImGui::MenuItem("Colliders", "", &handles_.colliders);
        ImGui::MenuItem("Baked boxes (walkability)", "", &handles_.bakedBoxes);
        ImGui::MenuItem("Triggers (non-solid colliders)", "", &handles_.triggers);
        ImGui::MenuItem("Unspawned placements", "", &handles_.placements);
        ImGui::MenuItem("Labels", "", &handles_.labels);
        bool badges = !handles_.triggerBadges.empty();
        if (ImGui::MenuItem("Script badges (talk, touch, auto, loop)", "", &badges)) {
            if (!badges) {
                handles_.triggerBadges.clear();
            } else {
                handles_.triggerBadges = {{"action", ICON_FA_COMMENT " talk"},
                                          {"touch", ICON_FA_SHOE_PRINTS " touch"},
                                          {"auto", ICON_FA_BOLT " auto"},
                                          {"parallel", ICON_FA_REPEAT " loop"}};
            }
        }
        ImGui::Separator();
        ImGui::MenuItem("Game camera", "", &gameCamera_);
        bool ortho = editorCamera_.camera.orthographic;
        if (ImGui::MenuItem("Orthographic", "O", &ortho)) {
            editorCamera_.setOrthographic(ortho);
        }
        if (ImGui::MenuItem("Frame selection", "F", false, hasWorld)) {
            frameSelection(*w, alpha_);
        }
        if (ImGui::MenuItem("Frame map", "Home", false, hasWorld)) {
            const auto& g = w->authored().grid;
            editorCamera_.frame({0, 0, 0}, {static_cast<float>(g.width()), 2, static_cast<float>(g.depth())});
        }
        if (ImGui::MenuItem("Camera from game view", "", false, hasWorld)) {
            editorCamera_.adopt(w->camera());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            layoutBuilt_ = false;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        if (ImGui::MenuItem(ICON_FA_PLAY " Play", "F5")) {
            setMode(Mode::Play);
        }
        ImGui::MenuItem("Pause game while editing", "", &pauseInEdit_);
        if (ImGui::MenuItem(paused_ ? "Resume game (keep editing)" : "Pause game", "", false)) {
            paused_ = !paused_;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("Scene: right drag orbits, middle drag (or Shift+right) pans, wheel zooms");
        ImGui::TextDisabled("Click selects; drag a selected object along the ground (Ctrl: no snap)");
        ImGui::TextDisabled("Q select, W move, E rotate, R scale, F frame, Del delete, Ctrl+D duplicate");
        ImGui::TextDisabled("F5 play/edit, F1 closes the editor");
        ImGui::EndMenu();
    }
    // Tool buttons on the right of the menu bar.
    ImGui::SameLine(ImGui::GetWindowWidth() - 330);
    const auto toolButton = [&](Tool tool, const char* icon, const char* tip) {
        const bool on = tool_ == tool;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(icon)) {
            tool_ = tool;
        }
        if (on) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::SameLine();
    };
    toolButton(Tool::Select, ICON_FA_ARROW_POINTER, "Select (Q)");
    toolButton(Tool::Translate, ICON_FA_UP_DOWN_LEFT_RIGHT, "Move (W)");
    toolButton(Tool::Rotate, ICON_FA_ROTATE, "Rotate (E)");
    toolButton(Tool::Scale, ICON_FA_MAXIMIZE, "Scale (R)");
    toolButton(Tool::Paint, ICON_FA_PAINTBRUSH, "Paint the grid (see the Map panel)");
    if (ImGui::SmallButton(snap_ ? ICON_FA_MAGNET " Snap" : ICON_FA_MAGNET " Free")) {
        snap_ = !snap_;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(paused_ ? ICON_FA_PLAY " Run" : ICON_FA_PAUSE " Pause")) {
        paused_ = !paused_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", paused_ ? "The game is paused while you edit; run it in place"
                                        : "The game is running; pause it");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_GAMEPAD " Play (F5)")) {
        setMode(Mode::Play);
    }
    ImGui::EndMenuBar();
}

void Editor::drawStatusBar() {
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    const auto w = world();
    if (w) {
        const auto& map = w->authored();
        ImGui::Text("  %s (%s)  %dx%d", map.name.c_str(), map.id.c_str(), map.grid.width(), map.grid.depth());
        ImGui::SameLine();
        if (history_.unsaved(*w)) {
            ImGui::TextDisabled("  unsaved edits");
            ImGui::SameLine();
        }
        if (!selection_.empty()) {
            ImGui::TextDisabled("  Selected: %s", selectionLabel(*w, selection_).c_str());
            ImGui::SameLine();
        }
    }
    ImGui::TextDisabled("  %s", status_.c_str());
}

void Editor::handleShortcuts(World* w) {
    const auto& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        setMode(mode_ == Mode::Edit ? Mode::Play : Mode::Edit);
        return;
    }
    if (mode_ != Mode::Edit || io.WantTextInput) {
        return;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && w) {
        saveMap(*w);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal) && w) {
        undo(*w, false);
    }
    if ((ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
         ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) &&
        w) {
        undo(*w, true);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal) && w) {
        duplicateSelection(*w);
    }
    // Plain keys only act while the Scene has the mouse, so typing elsewhere is safe.
    if (!viewportHovered_) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        tool_ = Tool::Select;
    } else if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        tool_ = Tool::Translate;
    } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        tool_ = Tool::Rotate;
    } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        tool_ = Tool::Scale;
    } else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        editorCamera_.setOrthographic(!editorCamera_.camera.orthographic);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        selection_ = {};
    }
    if (!w) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        frameSelection(*w, alpha_);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        const auto& g = w->authored().grid;
        editorCamera_.frame({0, 0, 0}, {static_cast<float>(g.width()), 2, static_cast<float>(g.depth())});
    } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        deleteSelection(*w);
    }
}
} // namespace fe
