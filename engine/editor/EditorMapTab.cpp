#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

std::optional<GridPos> Editor::hoveredCell(World& w) {
    const auto ray = mouseRay(w, alpha_);
    if (!ray) {
        return std::nullopt;
    }
    const auto hit = raycastGrid(w.grid(), ray->origin, ray->direction, sceneCamera(w).farPlane);
    if (!hit) {
        return std::nullopt;
    }
    return hit->cell;
}

void Editor::updateOverlays(World& w) {
    auto& overlays = w.overlays();
    const auto& boxed = w.boxBlockedCells();
    const auto key = w.name() + "/" + std::to_string(revision_) + "/" + std::to_string(layer_) + "/" + brush_.tag +
                     (showGrid_ ? "/on" : "/off") + (showBoxBlocked_ ? "/boxes" : "") + "/" +
                     std::to_string(boxed.size()) + "/" + std::to_string(w.map().grid.revision()) +
                     (bakePreview_ ? "/bake" : "");
    if (key == gridOverlayKey_ && overlays.contains("editor:blocked") == showGrid_) {
        return;
    }
    gridOverlayKey_ = key;
    for (const char* name : {"editor:blocked", "editor:tag", "editor:boxblocked", "editor:bake_walk",
                             "editor:bake_block", "editor:bake_partial", "editor:bake_island", "editor:bake_water",
                             "editor:bake_unsure"}) {
        overlays.erase(name);
    }
    if (!showGrid_) {
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
    if (showBoxBlocked_) {
        // Cells a collider box stands on: walkable, but routes go around them.
        std::vector<GridPos> cells;
        for (const auto key : boxed) {
            const GridPos p{static_cast<int>(key & 0xFFFFF), static_cast<int>((key >> 20) & 0xFFFFF),
                            static_cast<int>(key >> 40)};
            if (p.layer == layer_) {
                cells.push_back(p);
            }
        }
        overlays["editor:boxblocked"] = OverlayLayer{cells, {1, 0.6f, 0.1f, 0.3f}, 0, 93, true, true};
    }
    if (bakePreview_ && bakeOptions_.layer == layer_) {
        std::vector<GridPos> walk, block, partial, island, water, unsure;
        for (int z = 0; z < bakePreview_->depth; ++z) {
            for (int x = 0; x < bakePreview_->width; ++x) {
                const auto& c = bakePreview_->at(x, z);
                if (!c.hit || !bakeOptions_.inRegion(x, z)) {
                    continue;
                }
                const GridPos p{x, z, layer_};
                if (c.water) {
                    water.push_back(p);
                } else if (c.island) {
                    island.push_back(p);
                } else if (c.partial) {
                    partial.push_back(p);
                } else if (c.confidence < 0.34f) {
                    unsure.push_back(p);
                } else if (c.walkable) {
                    walk.push_back(p);
                } else {
                    block.push_back(p);
                }
            }
        }
        overlays["editor:bake_walk"] = OverlayLayer{walk, {0.2f, 0.9f, 0.3f, 0.3f}, 0, 95, true, false};
        overlays["editor:bake_block"] = OverlayLayer{block, {0.9f, 0.15f, 0.15f, 0.45f}, 0, 96, true, true};
        overlays["editor:bake_partial"] = OverlayLayer{partial, {1, 0.9f, 0.2f, 0.5f}, 0.5f, 97, true, true};
        overlays["editor:bake_unsure"] = OverlayLayer{unsure, {1, 0.75f, 0.2f, 0.4f}, 0.5f, 97, true, true};
        overlays["editor:bake_island"] = OverlayLayer{island, {1, 0.45f, 0.1f, 0.45f}, 0, 98, true, true};
        overlays["editor:bake_water"] = OverlayLayer{water, {0.2f, 0.5f, 1, 0.45f}, 0, 96, true, true};
    }
}

void Editor::drawPaint(World& w) {
    ImGui::PushID("paint");
    const auto& grid = w.authored().grid;
    ImGui::Checkbox("Show grid overlay", &showGrid_);
    ImGui::SameLine();
    ImGui::Checkbox("Cells routes avoid", &showBoxBlocked_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Orange: walkable cells a collider box stands on. Bodies slide along the box;\n"
                          "planned routes go around these cells.");
    }
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
    const char* modes[] = {"Walkable", "Height", "Ramp", "Add tag", "Remove tag", "Block (quarter cells)"};
    int mode = blockBrush_ ? 5 : static_cast<int>(brush_.mode);
    if (ImGui::Combo("Brush", &mode, modes, 6)) {
        blockBrush_ = mode == 5;
        if (!blockBrush_) {
            brush_.mode = static_cast<GridBrush::Mode>(mode);
        }
    }
    if (blockBrush_) {
        ImGui::TextDisabled("Paints collider boxes a quarter of a cell at a time (sub-cell blocking)");
        ImGui::DragFloat("Box height", &blockHeight_, 0.05f, 0.1f, 20);
        ImGui::Checkbox("Erase", &blockErase_);
        ImGui::SameLine();
        ImGui::TextDisabled("(or hold Shift)");
    } else {
        const char* shapes[] = {"Brush", "Rectangle", "Flood fill"};
        int shape = static_cast<int>(brushShape_);
        if (ImGui::Combo("Shape", &shape, shapes, 3)) {
            brushShape_ = static_cast<BrushShape>(shape);
            fillStart_.reset();
        }
        if (brushShape_ == BrushShape::Single) {
            ImGui::SliderInt("Radius", &brushRadius_, 0, 4);
        } else if (brushShape_ == BrushShape::Rectangle) {
            ImGui::TextDisabled("Drag from one corner to the other");
        } else {
            ImGui::TextDisabled("Click a cell: every connected cell with its walkability is painted");
        }
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
    }
    bool painting = tool_ == Tool::Paint;
    if (ImGui::Checkbox(ICON_FA_PAINTBRUSH " Paint with the left mouse button", &painting)) {
        tool_ = painting ? Tool::Paint : Tool::Translate;
    }
    ImGui::PopID();
    const auto ray = mouseRay(w, alpha_);
    const auto hit = ray ? raycastGrid(w.grid(), ray->origin, ray->direction, sceneCamera(w).farPlane) : std::nullopt;
    if (!hit) {
        if (fillStart_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            fillStart_.reset();
            w.overlays().erase("editor:fillrect");
        }
        return;
    }
    const GridPos target{hit->cell.x, hit->cell.z, layer_};
    if (!grid.contains(target)) {
        return;
    }
    const auto& c = grid.at(target);
    ImGui::Text("Cell %d,%d  height %.2f  %s", target.x, target.z, c.height, c.walkable ? "walkable" : "blocked");
    w.cursor() = OverlayCursor{true, target, {1, 1, 1, 0.8f}};
    cursorShown_ = true;
    if (tool_ != Tool::Paint || !viewportHovered_) {
        return;
    }
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (blockBrush_) {
        if (down) {
            const bool erase = blockErase_ || ImGui::GetIO().KeyShift;
            edit(w, [&](MapData& m) { paintBlock(m, {hit->point.x, hit->point.z}, layer_, blockHeight_, !erase); });
        }
        return;
    }
    if (const auto problem = brushProblem(grid, brush_, layer_); !problem.empty()) {
        if (down) {
            status_ = problem;
        }
        return;
    }
    switch (brushShape_) {
    case BrushShape::Single:
        if (down) {
            edit(w, [&](MapData& m) { paint(m.grid, brush_, target, brushRadius_); });
        }
        break;
    case BrushShape::Flood:
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            gesture_ = false;
            edit(w, [&](MapData& m) { floodFill(m.grid, brush_, target); });
            gesture_ = false;
        }
        break;
    case BrushShape::Rectangle:
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            fillStart_ = target;
        }
        if (fillStart_) {
            std::vector<GridPos> cells;
            for (int z = std::min(fillStart_->z, target.z); z <= std::max(fillStart_->z, target.z); ++z) {
                for (int x = std::min(fillStart_->x, target.x); x <= std::max(fillStart_->x, target.x); ++x) {
                    cells.push_back({x, z, layer_});
                }
            }
            w.overlays()["editor:fillrect"] = OverlayLayer{cells, {1, 1, 1, 0.25f}, 0, 99, true, true};
            if (!down) {
                const auto start = *fillStart_;
                fillStart_.reset();
                w.overlays().erase("editor:fillrect");
                gesture_ = false;
                edit(w, [&](MapData& m) { fillRect(m.grid, brush_, start, target); });
                gesture_ = false;
            }
        }
        break;
    }
}

void Editor::drawWalkability(World& w) {
    ImGui::TextWrapped("Bake a first pass of walkability, heights and ramps from the map's geometry, then clean up "
                       "with the brush. Objects smaller than a cell get collider boxes instead of whole blocked cells.");
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Bake walkability...")) {
        bakeDialog_ = true;
        bakeOptions_.layer = layer_;
        ImGui::OpenPopup("Bake walkability");
    }
    if (bakePreview_) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CHECK " Apply preview")) {
            const auto result = *bakePreview_;
            const auto options = bakeOptions_;
            gesture_ = false;
            int changed = 0;
            edit(w, [&](MapData& m) { changed = applyBake(m, result, options); });
            gesture_ = false;
            bakePreview_.reset();
            status_ = "Baked walkability: " + std::to_string(changed) + " cell(s) changed, " +
                      std::to_string(result.colliders.size()) + " box(es) added";
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK " Clear preview")) {
            bakePreview_.reset();
        }
        ImGui::TextDisabled("%s", bakeSummary_.c_str());
        ImGui::TextDisabled("green walkable, red blocked, yellow partial or unsure (check these), orange unreachable, "
                            "blue water");
    }
    drawBakeDialog(w);
}

void Editor::drawBakeDialog(World& w) {
    if (!bakeDialog_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Bake walkability", &bakeDialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    auto& o = bakeOptions_;
    const auto& grid = w.authored().grid;
    ImGui::SliderInt("Layer to write", &o.layer, 0, grid.layerCount() - 1);
    ImGui::SeparatorText("What counts as ground");
    ImGui::SliderInt("Samples per cell side", &o.samples, 2, 8);
    ImGui::SliderFloat("Max slope (degrees)", &o.maxSlopeDegrees, 5, 80, "%.0f");
    ImGui::DragFloat("Head room", &o.clearance, 0.05f, 0.5f, 5, "%.2f units");
    ImGui::DragFloat("Step", &o.step, 0.05f, 0.1f, 2, "%.2f units");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ground within this of the cell's floor is the same floor; also the largest step\n"
                          "between cells that still connects them (for finding unreachable islands).");
    }
    ImGui::SliderFloat("Walkable above", &o.coverage, 0.5f, 1, "%.2f of the cell");
    ImGui::SliderFloat("Blocked below", &o.partialCoverage, 0, 0.5f, "%.2f of the cell");
    ImGui::TextDisabled("Between the two, the cell is walkable and the covered quarters get boxes.");
    bool band = o.minHeight > -1e8f || o.maxHeight < 1e8f;
    if (ImGui::Checkbox("Only surfaces in a height band", &band)) {
        if (band) {
            o.minHeight = 0;
            o.maxHeight = 4;
        } else {
            o.minHeight = -1e9f;
            o.maxHeight = 1e9f;
        }
    }
    if (band) {
        ImGui::DragFloatRange2("Band", &o.minHeight, &o.maxHeight, 0.25f, -100, 100, "%.2f", "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("For a bridge deck or an upper floor: bake only what lies between these heights into the layer");
        }
    }
    ImGui::SeparatorText("What to write");
    ImGui::Checkbox("Walkability", &o.writeWalkable);
    ImGui::SameLine();
    ImGui::Checkbox("Heights", &o.writeHeights);
    ImGui::SameLine();
    ImGui::Checkbox("Ramps", &o.writeRamps);
    ImGui::Checkbox("Water tag", &o.writeWater);
    ImGui::SameLine();
    ImGui::Checkbox("Boxes for partial cells", &o.writeColliders);
    ImGui::Checkbox("Block the outer ring", &o.blockEdges);
    ImGui::SameLine();
    ImGui::Checkbox("Block unreachable islands", &o.pruneIslands);
    ImGui::Checkbox("Never unblock cells painted blocked", &o.keepBlocked);
    bool regional = o.region.has_value();
    if (ImGui::Checkbox("Only a rectangle of cells", &regional)) {
        if (regional) {
            o.region = glm::ivec4(0, 0, grid.width(), grid.depth());
        } else {
            o.region.reset();
        }
    }
    if (o.region) {
        int values[4] = {o.region->x, o.region->y, o.region->z, o.region->w};
        if (ImGui::DragInt4("x, z, width, depth", values, 0.2f, 0, std::max(grid.width(), grid.depth()))) {
            o.region = glm::ivec4(values[0], values[1], values[2], values[3]);
        }
    }
    ImGui::Separator();
    const auto model = engine_.assets().tryModel(w.geometryPath());
    if (!model) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The map has no geometry to bake from (%s)", w.geometryPath().c_str());
    }
    ImGui::BeginDisabled(!model);
    if (ImGui::Button(ICON_FA_EYE " Preview")) {
        bakePreview_ = bakeWalkability(*model, grid, o);
        bakeSummary_ = std::to_string(bakePreview_->walkable) + " walkable, " + std::to_string(bakePreview_->blocked) +
                       " blocked (" + std::to_string(bakePreview_->islands) + " unreachable), " +
                       std::to_string(bakePreview_->partial) + " partial with " +
                       std::to_string(bakePreview_->colliders.size()) + " box(es), " +
                       std::to_string(bakePreview_->unsampled) + " without geometry";
        layer_ = o.layer;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CHECK " Bake and apply")) {
        const auto result = bakeWalkability(*model, grid, o);
        const auto options = o;
        gesture_ = false;
        int changed = 0;
        edit(w, [&](MapData& m) { changed = applyBake(m, result, options); });
        gesture_ = false;
        bakePreview_.reset();
        status_ = "Baked walkability: " + std::to_string(changed) + " cell(s) changed, " +
                  std::to_string(result.colliders.size()) + " box(es) added";
        layer_ = o.layer;
        bakeDialog_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        bakeDialog_ = false;
        ImGui::CloseCurrentPopup();
    }
    if (bakePreview_) {
        ImGui::TextDisabled("%s", bakeSummary_.c_str());
    }
    ImGui::EndPopup();
}

void Editor::drawLinks(World& w) {
    const auto& grid = w.authored().grid;
    auto links = grid.links();
    static int selectedLink = -1;
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
        const auto t = sceneCamera(w).focus();
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
            selectedLink = static_cast<int>(links.size()) - 1;
        }
    }
    for (std::size_t i = 0; i < links.size(); ++i) {
        const auto& l = links[i];
        ImGui::PushID(static_cast<int>(i));
        const auto label = std::string(l.kind == LinkKind::Ladder ? "Ladder " : "Link ") + std::to_string(l.x) +
                           "," + std::to_string(l.z) + ": layer " + std::to_string(l.from) + " -> " +
                           std::to_string(l.to);
        if (ImGui::Selectable(label.c_str(), selectedLink == static_cast<int>(i))) {
            selectedLink = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (!validIndex(selectedLink, links)) {
        return;
    }
    auto& link = links[static_cast<std::size_t>(selectedLink)];
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
        links.erase(links.begin() + selectedLink);
        selectedLink = -1;
    }
    if (changed || remove) {
        apply(links);
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
    gesture_ = false;
    edit(w, [&](MapData& m) {
        std::vector<std::string> found;
        const int count = fe::importMarkers(m, *model, found);
        if (imported < 0) {
            imported = count;
            problems = std::move(found);
        }
    });
    gesture_ = false;
    status_ = "Imported " + std::to_string(imported) + " markers";
    for (const auto& problem : problems) {
        status_ += "; skipped " + problem;
    }
}

void Editor::drawMapTab() {
    const auto w = world(); // held for the frame (see Editor::world)
    if (!w) {
        ImGui::TextDisabled("No world loaded");
        return;
    }
    if (w.get() != historyWorld_) {
        history_.sync(*w);
        historyWorld_ = w.get();
    }
    const auto& map = w->authored();
    layer_ = std::clamp(layer_, 0, map.grid.layerCount() - 1);
    ImGui::Text("%s (%s)  %dx%d, %d layer(s)", map.name.c_str(), map.id.c_str(), map.grid.width(), map.grid.depth(),
                map.grid.layerCount());
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save map")) {
        saveMap(*w);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(history_.undoCount(*w) == 0);
    if (ImGui::Button(ICON_FA_ROTATE_LEFT " Undo")) {
        undo(*w, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(history_.redoCount(*w) == 0);
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT " Redo")) {
        undo(*w, true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Import Blender markers")) {
        importMarkers(*w);
    }
    if (ImGui::CollapsingHeader("Grid painting", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawPaint(*w);
    } else if (tool_ == Tool::Paint) {
        tool_ = Tool::Translate;
    }
    if (ImGui::CollapsingHeader("Walkability bake")) {
        ImGui::PushID("bake");
        drawWalkability(*w);
        ImGui::PopID();
    } else {
        drawBakeDialog(*w); // the modal stays open while its header is closed
    }
    if (ImGui::CollapsingHeader("Layer links")) {
        ImGui::PushID("links");
        drawLinks(*w);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Environment")) {
        ImGui::PushID("environment");
        drawEnvironment(*w);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Map props")) {
        ImGui::PushID("mapprops");
        auto props = map.props;
        const bool changed = props.is_object() ? editProps(fieldsFor("map_props", {"schemas"}), props)
                                               : editJson("##mapprops", props, 200);
        if (changed) {
            edit(*w, [&](MapData& m) { m.props = props; });
        }
        ImGui::PopID();
    }
}
} // namespace fe
