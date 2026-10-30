#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <set>

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
                     (showGrid_ ? "/on" : "/off") + (showBoxBlocked_ ? "/boxes" : "") + (showOverrides_ ? "/over" : "") +
                     "/" + std::to_string(boxed.size()) + "/" + std::to_string(w.map().grid.revision());
    if (key == gridOverlayKey_ && overlays.contains("editor:blocked") == showGrid_) {
        return;
    }
    gridOverlayKey_ = key;
    for (const char* name : {"editor:blocked", "editor:tag", "editor:boxblocked", "editor:override"}) {
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
    if (showOverrides_ && !w.authored().bake.overrides.empty()) {
        // Cells an author corrected by hand: they keep their value through re-bakes.
        overlays["editor:override"] = OverlayLayer{overriddenCells(w.authored(), layer_), {0.75f, 0.4f, 1, 0.3f}, 0, 94, true, true};
    }
}

void Editor::drawPaint(World& w) {
    ImGui::PushID("paint");
    const auto& map = w.authored();
    const auto& grid = map.grid;
    const bool derived = !map.bake.empty();
    if (derived) {
        ImGui::TextWrapped("The brush records overrides: cells that keep what you paint when the walkability is "
                           "baked again. Clear override hands a cell back to the bake.");
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
    const char* modes[] = {"Walkable", "Height", "Ramp", "Add tag", "Remove tag", "Clear override"};
    int mode = clearOverrides_ ? 5 : static_cast<int>(brush_.mode);
    if (ImGui::Combo("Brush", &mode, modes, derived ? 6 : 5)) {
        clearOverrides_ = mode == 5;
        if (!clearOverrides_) {
            brush_.mode = static_cast<GridBrush::Mode>(mode);
        }
    }
    const char* shapes[] = {"Brush", "Rectangle", "Flood fill"};
    int shape = static_cast<int>(brushShape_);
    if (ImGui::Combo("Shape", &shape, shapes, clearOverrides_ ? 1 : 3)) {
        brushShape_ = static_cast<BrushShape>(shape);
        fillStart_.reset();
    }
    if (clearOverrides_) {
        brushShape_ = BrushShape::Single;
    }
    if (brushShape_ == BrushShape::Single) {
        ImGui::SliderInt("Radius", &brushRadius_, 0, 4);
    } else if (brushShape_ == BrushShape::Rectangle) {
        ImGui::TextDisabled("Drag from one corner to the other");
    } else {
        ImGui::TextDisabled("Click a cell: every connected cell with its walkability is painted");
    }
    if (!clearOverrides_) {
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
        } else {
            ImGui::InputText("Tag", &brush_.tag);
        }
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
    // The cells of the brush square around a cell, inside the grid.
    const auto square = [&](GridPos centre) {
        std::vector<GridPos> cells;
        for (int z = centre.z - brushRadius_; z <= centre.z + brushRadius_; ++z) {
            for (int x = centre.x - brushRadius_; x <= centre.x + brushRadius_; ++x) {
                if (grid.inRange(x, z, layer_)) {
                    cells.push_back({x, z, layer_});
                }
            }
        }
        return cells;
    };
    if (clearOverrides_) {
        if (down) {
            std::optional<glm::ivec4> region;
            bool cleared = false;
            edit(w, [&](MapData& m) {
                for (const auto& cell : square(target)) {
                    if (clearOverride(m, cell)) {
                        cleared = true;
                        const glm::ivec4 one(cell.x, cell.z, 1, 1);
                        region = region ? unionRegion(*region, one) : one;
                    }
                }
            });
            if (cleared && region) {
                rebake(w, region, true); // the bake's own value comes back
                gesture_ = true;         // the stroke goes on
            }
        }
        return;
    }
    if (const auto problem = brushProblem(grid, brush_, layer_); !problem.empty()) {
        if (down) {
            status_ = problem;
        }
        return;
    }
    // On a derived map the brush writes overrides; on a hand-painted one, the grid.
    const auto strokeCells = [&](const std::vector<GridPos>& cells) {
        edit(w, [&](MapData& m) {
            if (derived) {
                for (const auto& cell : cells) {
                    setOverride(m, cell, brush_);
                }
            } else {
                for (const auto& cell : cells) {
                    paint(m.grid, brush_, cell, 0);
                }
            }
        });
    };
    switch (brushShape_) {
    case BrushShape::Single:
        if (down) {
            strokeCells(square(target));
        }
        break;
    case BrushShape::Flood:
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            gesture_ = false;
            if (derived) {
                // The connected cells that share the start's walkability, on this layer.
                std::vector<GridPos> cells, stack{target};
                std::set<std::pair<int, int>> seen{{target.x, target.z}};
                const bool walkable = c.walkable;
                while (!stack.empty()) {
                    const auto cell = stack.back();
                    stack.pop_back();
                    cells.push_back(cell);
                    for (const auto [dx, dz] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                        const GridPos next{cell.x + dx, cell.z + dz, layer_};
                        if (grid.inRange(next) && grid.at(next).walkable == walkable && seen.insert({next.x, next.z}).second) {
                            stack.push_back(next);
                        }
                    }
                }
                strokeCells(cells);
            } else {
                edit(w, [&](MapData& m) { floodFill(m.grid, brush_, target); });
            }
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
                edit(w, [&](MapData& m) {
                    if (derived) {
                        setOverrideRect(m, start, target, brush_);
                    } else {
                        fillRect(m.grid, brush_, start, target);
                    }
                });
                gesture_ = false;
            }
        }
        break;
    }
}

// The placements the bake reads, in the form that tells a change: id, model,
// position, yaw, scale and layer.
Json bakedPlacementsJson(const MapData& map) {
    Json out = Json::array();
    for (const auto& p : map.placements) {
        if (const auto model = bakedModelOf(p)) {
            out.push_back({{"id", p.id},   {"model", *model},         {"position", toJson(p.position)},
                           {"yaw", p.yaw}, {"scale", toJson(p.scale)}, {"layer", p.layer}});
        }
    }
    return out;
}

void Editor::rebake(World& w, std::optional<glm::ivec4> region, bool joinGesture) {
    auto options = BakeOptions::fromJson(w.authored().bake.options);
    options.region = region;
    rebake(w, options, joinGesture, false);
}

void Editor::rebake(World& w, const BakeOptions& given, bool joinGesture, bool storeOptions) {
    const auto& authored = w.authored();
    auto options = given;
    options.layer = std::clamp(options.layer, 0, authored.grid.layerCount() - 1);
    if (options.region && options.region->x <= 0 && options.region->y <= 0 &&
        options.region->x + options.region->z >= authored.grid.width() &&
        options.region->y + options.region->w >= authored.grid.depth()) {
        options.region.reset(); // the whole map: a full bake
    }
    const auto scene = sceneForMap(authored, engine_.assets(), w.geometryPath());
    const auto result = bakeWalkability(scene.pieces, authored.grid, options);
    const Json optionsJson = storeOptions || authored.bake.options.empty() ? options.toJson() : authored.bake.options;
    MapData fingerprinted = authored;
    fingerprinted.bake.options = optionsJson;
    const auto fingerprint = bakeFingerprint(fingerprinted, w.geometryPath(), gameFileReader(engine_.lua().gameRoot()));
    int changed = 0;
    const auto apply = [&](MapData& m) {
        changed = applyBake(m, result, options);
        m.bake.fingerprint = fingerprint;
        m.bake.options = optionsJson;
    };
    if (joinGesture) {
        history_.apply(w, apply, false);
    } else {
        gesture_ = false;
        edit(w, apply);
        gesture_ = false;
    }
    history_.settle(w);
    gestureBakeSnapshot_.reset();
    ++revision_;
    status_ = (options.region ? "Re-baked walkability around the change: " : "Baked walkability: ") +
              std::to_string(changed) + " cell(s) changed, " + std::to_string(w.authored().bake.colliders.size()) +
              " box(es)";
    for (const auto& problem : scene.problems) {
        status_ += "; " + problem;
    }
    bakeStatusRevision_ = ~std::uint64_t{0};
}

void Editor::autoRebake(World& w) {
    const Json before = std::move(*gestureBakeSnapshot_);
    gestureBakeSnapshot_.reset();
    const auto& authored = w.authored();
    if (!authored.bake.autoBake || authored.bake.empty()) {
        return; // a hand-painted map: Bake now opts in
    }
    const auto after = bakedPlacementsJson(authored);
    if (before == after) {
        bakeStatusRevision_ = ~std::uint64_t{0};
        return;
    }
    const auto& grid = authored.grid;
    std::optional<glm::ivec4> region;
    const auto grow = [&](const Json& entry) {
        Placement p;
        p.position = jsonVec3(entry.at("position"));
        p.yaw = entry.at("yaw");
        p.scale = jsonVec3(entry.at("scale"));
        const auto model = engine_.assets().tryModel(entry.at("model").get<std::string>());
        const auto cells = model ? bakeRegionFor(*model, p, grid) : glm::ivec4(0, 0, grid.width(), grid.depth());
        region = region ? unionRegion(*region, cells) : cells;
    };
    for (const auto& entry : before) {
        if (std::find(after.begin(), after.end(), entry) == after.end()) {
            grow(entry);
        }
    }
    for (const auto& entry : after) {
        if (std::find(before.begin(), before.end(), entry) == before.end()) {
            grow(entry);
        }
    }
    if (region) {
        rebake(w, region, true);
    }
}

void Editor::refreshBakeStatus(World& w) {
    if (gesture_ || (bakeStatusRevision_ == revision_ && bakeStatusWorld_ == w.name())) {
        return;
    }
    bakeStatusRevision_ = revision_;
    bakeStatusWorld_ = w.name();
    const auto& map = w.authored();
    if (map.bake.empty()) {
        bakeStale_ = false;
        bakeStatus_.clear();
        return;
    }
    const auto fingerprint = bakeFingerprint(map, w.geometryPath(), gameFileReader(engine_.lua().gameRoot()));
    bakeStale_ = fingerprint != map.bake.fingerprint;
    int walkable = 0, blocked = 0;
    const int layer = std::clamp(layer_, 0, map.grid.layerCount() - 1);
    for (int z = 0; z < map.grid.depth(); ++z) {
        for (int x = 0; x < map.grid.width(); ++x) {
            (map.grid.at(x, z, layer).walkable ? walkable : blocked)++;
        }
    }
    bakeStatus_ = std::to_string(walkable) + " walkable, " + std::to_string(blocked) + " blocked cell(s), " +
                  std::to_string(map.bake.colliders.size()) + " baked box(es), " +
                  std::to_string(map.bake.overrides.size()) + " override(s)";
}

void Editor::drawWalkability(World& w) {
    const auto& map = w.authored();
    refreshBakeStatus(w);
    bool autoBake = map.bake.autoBake && !map.bake.empty();
    if (ImGui::Checkbox("Auto bake", &autoBake)) {
        gesture_ = false;
        edit(w, [&](MapData& m) {
            m.bake.autoBake = autoBake;
            if (m.bake.options.empty()) {
                m.bake.options = BakeOptions{}.toJson();
            }
        });
        gesture_ = false;
        if (autoBake) {
            rebake(w, std::nullopt, false);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Derive walkability from the terrain and the placed models, again whenever a model is\n"
                          "placed, moved, turned, scaled or removed. Fix exceptions with the brush.");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Bake now")) {
        rebake(w, std::nullopt, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Options...")) {
        bakeOptions_ = BakeOptions::fromJson(map.bake.options);
        bakeOptions_.layer = layer_;
        ImGui::OpenPopup("Bake options");
    }
    if (map.bake.empty()) {
        ImGui::TextDisabled("Hand-painted walkability. Bake now derives it from the terrain and the placed\n"
                            "models; Auto bake keeps it up to date.");
    } else if (bakeStale_) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), ICON_FA_TRIANGLE_EXCLAMATION
                           " Bake out of date: the geometry, a model or a placement changed. Press Bake now.");
    } else {
        ImGui::TextDisabled("Fresh: %s", bakeStatus_.c_str());
    }
    ImGui::Checkbox("Show grid overlay", &showGrid_);
    ImGui::SameLine();
    ImGui::Checkbox("Cells routes avoid", &showBoxBlocked_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Orange cells: walkable, but a box stands on them. Bodies slide along the box;\n"
                          "planned routes go around these cells.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Baked boxes", &handles_.bakedBoxes);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Orange outlines: the collider boxes the bake made from walls, posts and drops.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Overrides", &showOverrides_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Violet: cells fixed by hand with the brush; they keep their value through re-bakes.");
    }
    drawBakeOptions(w);
}

void Editor::drawBakeOptions(World& w) {
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Bake options", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    auto& o = bakeOptions_;
    const auto& grid = w.authored().grid;
    ImGui::SliderInt("Layer to write", &o.layer, 0, grid.layerCount() - 1);
    ImGui::SeparatorText("What counts as ground");
    ImGui::SliderInt("Samples per cell side", &o.samples, 2, 8);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("4 makes quarter-cell boxes, 8 eighth-cell boxes (finer, slower).");
    }
    ImGui::SliderFloat("Max slope (degrees)", &o.maxSlopeDegrees, 5, 80, "%.0f");
    ImGui::DragFloat("Head room", &o.clearance, 0.05f, 0.5f, 5, "%.2f units");
    ImGui::DragFloat("Step", &o.step, 0.05f, 0.1f, 2, "%.2f units");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ground within this of the cell's floor is the same floor; also the largest step\n"
                          "between cells that still connects them (for finding unreachable islands).");
    }
    ImGui::SliderFloat("Walkable above", &o.minCoverage, 0, 1, "%.2f of the cell");
    ImGui::TextDisabled("Below it the cell is blocked; above, the parts that are not ground get boxes.");
    ImGui::Checkbox("Block the outer ring", &o.blockEdges);
    ImGui::SameLine();
    ImGui::Checkbox("Block unreachable islands", &o.pruneIslands);
    ImGui::Checkbox("Boxes at drops inside a cell", &o.boxDrops);
    ImGui::SeparatorText("This bake only");
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
    if (ImGui::Button(ICON_FA_CHECK " Bake with these")) {
        rebake(w, o, false, true);
        layer_ = o.layer;
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The settings above the line are kept with the map and used by Auto bake.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
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
    // A new map derives its walkability as soon as it opens.
    if (map.bake.autoBake && !map.bake.empty() && map.bake.fingerprint.empty() && !gesture_) {
        rebake(*w, std::nullopt, false);
    }
    if (ImGui::CollapsingHeader("Walkability", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("bake");
        drawWalkability(*w);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader(map.bake.empty() ? "Paint walkability" : "Fix walkability by hand")) {
        drawPaint(*w);
    } else if (tool_ == Tool::Paint) {
        tool_ = Tool::Translate;
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
