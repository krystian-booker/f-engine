#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

std::optional<GridPos> Editor::hoveredCell(World& w) {
    const auto& io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        return std::nullopt;
    }
    const auto viewport = engine_.viewport();
    const glm::vec2 mouse(io.MousePos.x * io.DisplayFramebufferScale.x,
                          io.MousePos.y * io.DisplayFramebufferScale.y);
    const glm::vec2 ndc(mouse.x / static_cast<float>(viewport.x) * 2 - 1,
                        1 - mouse.y / static_cast<float>(viewport.y) * 2);
    glm::vec3 origin, direction;
    w.camera().ray(ndc, static_cast<float>(viewport.x) / static_cast<float>(viewport.y), origin, direction);
    const auto hit = raycastGrid(w.grid(), origin, direction, w.camera().farPlane);
    if (!hit) {
        return std::nullopt;
    }
    return hit->cell;
}

void Editor::updateOverlays(World& w) {
    auto& overlays = w.overlays();
    const auto key = w.name() + "/" + std::to_string(revision_) + "/" + std::to_string(layer_) + "/" +
                     brush_.tag + (showGrid_ ? "/on" : "/off");
    if (key == gridOverlayKey_ && overlays.contains("editor:blocked") == showGrid_) {
        return;
    }
    gridOverlayKey_ = key;
    if (!showGrid_) {
        overlays.erase("editor:blocked");
        overlays.erase("editor:tag");
        return;
    }
    const auto& grid = w.authored().grid;
    std::vector<GridPos> blocked, tagged;
    const auto tagMask = brush_.tag.empty() ? 0u : grid.tagMask({brush_.tag});
    for (int z = 0; z < grid.depth(); ++z) {
        for (int x = 0; x < grid.width(); ++x) {
            const auto& cell = grid.at(x, z, layer_);
            if (!cell.walkable) {
                blocked.push_back({x, z, layer_});
            }
            if (tagMask && (cell.tags & tagMask)) {
                tagged.push_back({x, z, layer_});
            }
        }
    }
    overlays["editor:blocked"] = OverlayLayer{blocked, {1, 0.25f, 0.3f, 0.35f}, 0, 90, true, true};
    overlays["editor:tag"] = OverlayLayer{tagged, {0.3f, 0.9f, 1, 0.35f}, 0, 91, true, true};
}

void Editor::drawPaint(World& w) {
    const auto& grid = w.authored().grid;
    ImGui::Checkbox("Show grid overlay", &showGrid_);
    ImGui::SliderInt("Layer", &layer_, 0, grid.layerCount() - 1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add layer")) {
        if (grid.layerCount() >= 16) {
            status_ = "A grid supports at most 16 layers";
        } else {
            const auto name = "layer" + std::to_string(grid.layerCount());
            edit(w, [&](MapData& m) {
                if (m.grid.layerCount() < 16) {
                    m.grid.addLayer(name);
                }
            });
            layer_ = grid.layerCount() - 1;
        }
    }
    const char* modes[] = {"Walkable", "Height", "Ramp", "Add tag", "Remove tag"};
    int mode = static_cast<int>(brush_.mode);
    if (ImGui::Combo("Brush", &mode, modes, 5)) {
        brush_.mode = static_cast<GridBrush::Mode>(mode);
    }
    ImGui::SliderInt("Radius", &brushRadius_, 0, 4);
    if (brush_.mode == GridBrush::Mode::Walkable) {
        ImGui::Checkbox("Walkable", &brush_.walkable);
    } else if (brush_.mode == GridBrush::Mode::Height) {
        ImGui::DragFloat("Height", &brush_.height, 0.25f, -20, 40);
    } else if (brush_.mode == GridBrush::Mode::Ramp) {
        const char* ramps[RampCount];
        for (int i = 0; i < RampCount; ++i) {
            ramps[i] = rampName(static_cast<Ramp>(i));
        }
        int ramp = static_cast<int>(brush_.ramp);
        if (ImGui::Combo("Ramp", &ramp, ramps, RampCount)) {
            brush_.ramp = static_cast<Ramp>(ramp);
        }
    }
    ImGui::InputText("Tag", &brush_.tag);
    ImGui::Checkbox("Paint with left mouse", &painting_);
    updateOverlays(w);
    const auto cell = hoveredCell(w);
    if (!cell) {
        return;
    }
    const GridPos target{cell->x, cell->z, layer_};
    const auto& c = grid.at(target);
    ImGui::Text("Cell %d,%d  height %.2f  %s", cell->x, cell->z, c.height,
                c.walkable ? "walkable" : "blocked");
    w.cursor() = OverlayCursor{true, target, {1, 1, 1, 0.8f}};
    cursorShown_ = true;
    if (!painting_ || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return;
    }
    if (const auto problem = brushProblem(grid, brush_, layer_); !problem.empty()) {
        status_ = problem;
        return;
    }
    edit(w, [&](MapData& m) { paint(m.grid, brush_, target, brushRadius_); });
}

void Editor::drawLinks(World& w) {
    const auto& grid = w.authored().grid;
    auto links = grid.links();
    // Applies the edited list when every link in it can be loaded.
    auto apply = [&](const std::vector<GridLink>& edited) {
        for (const auto& link : edited) {
            if (const auto problem = editProblem(grid, link); !problem.empty()) {
                status_ = problem;
                return false;
            }
        }
        edit(w, [&](MapData& m) { m.grid.setLinks(edited); });
        return true;
    };
    if (ImGui::Button("Add link")) {
        const auto t = w.camera().target;
        links.push_back({std::clamp(static_cast<int>(t.x), 0, grid.width() - 1),
                         std::clamp(static_cast<int>(t.z), 0, grid.depth() - 1),
                         0,
                         std::min(1, grid.layerCount() - 1),
                         {0, 0}});
        if (grid.layerCount() < 2) {
            links.back().kind = LinkKind::Ladder;
            links.back().direction = links.back().z > 0 ? glm::ivec2(0, -1) : glm::ivec2(0, 1);
        }
        if (apply(links)) {
            selectedLink_ = static_cast<int>(links.size()) - 1;
        }
    }
    for (std::size_t i = 0; i < links.size(); ++i) {
        const auto& l = links[i];
        ImGui::PushID(static_cast<int>(i));
        const auto label = std::string(l.kind == LinkKind::Ladder ? "Ladder " : "Link ") +
                           std::to_string(l.x) + "," + std::to_string(l.z) + ": layer " +
                           std::to_string(l.from) + " -> " + std::to_string(l.to);
        if (ImGui::Selectable(label.c_str(), selectedLink_ == static_cast<int>(i))) {
            selectedLink_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (!validIndex(selectedLink_, links)) {
        return;
    }
    auto& link = links[static_cast<std::size_t>(selectedLink_)];
    const char* kinds[] = {"step", "ladder"};
    int kind = static_cast<int>(link.kind);
    bool changed = false;
    if (ImGui::Combo("Kind", &kind, kinds, 2)) {
        link.kind = static_cast<LinkKind>(kind);
        if (link.kind == LinkKind::Ladder && link.direction == glm::ivec2(0)) {
            link.direction = rampDirection(Ramp::North);
        }
        changed = true;
    }
    const bool ladder = link.kind == LinkKind::Ladder;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("step: joins two layers at one cell (a bridge's foot)\n"
                          "ladder: climbs from the foot cell to the neighbour in its direction");
    }
    changed |= ImGui::InputInt(ladder ? "Foot x" : "Link x", &link.x);
    changed |= ImGui::InputInt(ladder ? "Foot z" : "Link z", &link.z);
    changed |= editLayer(ladder ? "Foot layer" : "From layer", link.from, grid);
    changed |= editLayer(ladder ? "Top layer" : "To layer", link.to, grid);
    const char* directions[RampCount];
    for (int i = 0; i < RampCount; ++i) {
        directions[i] = rampName(static_cast<Ramp>(i));
    }
    int direction = static_cast<int>(rampFromDirection(link.direction));
    if (ImGui::Combo(ladder ? "Way up" : "Direction", &direction, directions, RampCount)) {
        link.direction = rampDirection(static_cast<Ramp>(direction));
        changed = true;
    }
    if (ladder) {
        ImGui::TextDisabled("Top cell %d,%d", link.top().x, link.top().z);
    }
    link.x = std::clamp(link.x, 0, grid.width() - 1);
    link.z = std::clamp(link.z, 0, grid.depth() - 1);
    const bool remove = ImGui::Button("Delete link");
    if (remove) {
        links.erase(links.begin() + selectedLink_);
        selectedLink_ = -1;
    }
    if (changed || remove) {
        apply(links);
    }
}

void Editor::drawPoints(World& w) {
    const auto& points = w.authored().points;
    ImGui::InputText("Point name", &newPoint_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add at camera") && !newPoint_.empty()) {
        const MapPoint point{w.camera().target, {0, 1}, 0};
        edit(w, [&](MapData& m) { m.points[newPoint_] = point; });
        selectedPoint_ = newPoint_;
    }
    for (const auto& entry : points) {
        if (ImGui::Selectable(entry.first.c_str(), entry.first == selectedPoint_)) {
            selectedPoint_ = entry.first;
        }
    }
    const auto found = points.find(selectedPoint_);
    if (found == points.end()) {
        return;
    }
    auto point = found->second;
    bool changed = editVec3("Point position", point.position);
    changed |= ImGui::DragFloat2("Point facing", glm::value_ptr(point.facing), 0.05f, -1, 1);
    changed |= editLayer("Point layer", point.layer, w.authored().grid);
    const auto name = selectedPoint_;
    if (changed) {
        edit(w, [&](MapData& m) { m.points[name] = point; });
    }
    const auto playFrom = devtool("play_from");
    ImGui::BeginDisabled(playFrom == sol::lua_nil);
    if (ImGui::Button("Play from here")) {
        if (succeeded(engine_.lua().invoke(playFrom, w.authored().id, name))) {
            paused_ = false;
            status_ = "Playing from " + name;
        }
    }
    ImGui::EndDisabled();
    if (playFrom == sol::lua_nil && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Define fe.devtools.play_from(map, point) to enable");
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove point")) {
        edit(w, [&](MapData& m) { m.points.erase(name); });
        selectedPoint_.clear();
    }
}

void Editor::importMarkers(World& w) {
    const auto model = engine_.assets().tryModel(w.geometryPath());
    if (!model) {
        status_ = "Geometry not found";
        return;
    }
    std::vector<std::string> problems;
    int imported = -1;
    edit(w, [&](MapData& m) {
        std::vector<std::string> found;
        const int count = fe::importMarkers(m, *model, found);
        if (imported < 0) {
            imported = count;
            problems = std::move(found);
        }
    });
    status_ = "Imported " + std::to_string(imported) + " markers";
    for (const auto& problem : problems) {
        status_ += "; skipped " + problem;
    }
}

void Editor::drawMapTab() {
    auto* w = world();
    if (!w) {
        ImGui::TextDisabled("No world loaded");
        return;
    }
    if (w != historyWorld_) {
        // A reloaded world: its history stays only if the map is the one it left.
        history_.sync(*w);
        historyWorld_ = w;
    }
    const auto& map = w->authored();
    layer_ = std::clamp(layer_, 0, map.grid.layerCount() - 1);
    ImGui::Text("%s (%s)  %dx%d, %d layer(s)", map.name.c_str(), map.id.c_str(), map.grid.width(),
                map.grid.depth(), map.grid.layerCount());
    if (ImGui::Button("Save map")) {
        saveMap(*w);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(history_.undoCount(*w) == 0);
    if (ImGui::Button("Undo")) {
        undo(*w, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(history_.redoCount(*w) == 0);
    if (ImGui::Button("Redo")) {
        undo(*w, true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Import Blender markers")) {
        importMarkers(*w);
    }
    if (ImGui::CollapsingHeader("Grid painting", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawPaint(*w);
    } else {
        w->overlays().erase("editor:blocked");
        w->overlays().erase("editor:tag");
        gridOverlayKey_.clear();
    }
    if (ImGui::CollapsingHeader("Layer links")) {
        drawLinks(*w);
    }
    if (ImGui::CollapsingHeader("Placements")) {
        drawPlacements(*w);
    }
    if (ImGui::CollapsingHeader("Zones")) {
        drawZones(*w);
    } else {
        w->overlays().erase("editor:zone");
        zoneOverlayKey_.clear();
    }
    if (ImGui::CollapsingHeader("Points")) {
        drawPoints(*w);
    }
    if (ImGui::CollapsingHeader("Lights")) {
        drawLights(*w);
    }
    if (ImGui::CollapsingHeader("Emitters")) {
        drawEmitters(*w);
    }
    if (ImGui::CollapsingHeader("Colliders")) {
        drawColliders(*w);
    }
    if (ImGui::CollapsingHeader("Environment")) {
        drawEnvironment(*w);
    }
    if (ImGui::CollapsingHeader("Map props")) {
        auto props = map.props;
        if (editJson("##mapprops", props, 200)) {
            edit(*w, [&](MapData& m) { m.props = props; });
        }
    }
}
} // namespace fe
