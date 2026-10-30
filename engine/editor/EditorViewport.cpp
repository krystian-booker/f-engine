#include "engine/core/Paths.h"
#include "engine/editor/Editor.h"
#include "engine/editor/EditorDragDrop.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/editor/SceneEdits.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>

namespace fe {
using namespace editorui;

namespace {
ImU32 toColor(glm::vec4 c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
}
} // namespace

float Editor::pixelScale() const {
    return std::max(0.01f, ImGui::GetIO().DisplayFramebufferScale.x);
}

glm::vec2 Editor::mousePixels() const {
    const auto& io = ImGui::GetIO();
    return {io.MousePos.x * io.DisplayFramebufferScale.x, io.MousePos.y * io.DisplayFramebufferScale.y};
}

const Camera& Editor::sceneCamera(const World& w) const {
    return mode_ == Mode::Edit && !gameCamera_ ? editorCamera_.camera : w.camera();
}

std::optional<ViewRay> Editor::mouseRay(World& w, double alpha) const {
    const auto mouse = mousePixels();
    if (mode_ == Mode::Edit) {
        if (!viewportHovered_ || gizmoUsing_ || !viewRect_.contains(mouse)) {
            return std::nullopt;
        }
        return viewRay(sceneCamera(w), viewRect_, mouse, alpha);
    }
    if (ImGui::GetIO().WantCaptureMouse) {
        return std::nullopt;
    }
    ViewRect whole;
    whole.pixels = {0, 0, engine_.viewport().x, engine_.viewport().y};
    return viewRay(w.camera(), whole, mouse, alpha);
}

void Editor::adjustDraws(std::vector<WorldDraw>& draws) {
    if (!open_ || mode_ != Mode::Edit || viewRect_.pixels.z <= 0 || viewRect_.pixels.w <= 0) {
        return;
    }
    queueThumbnailDraws(draws);
    const auto w = world();
    if (!w) {
        return;
    }
    // The focus draw: the last full-screen draw of the world the editor shows.
    WorldDraw* focus = nullptr;
    for (auto& draw : draws) {
        if (draw.world == w && !draw.rect && draw.target.empty()) {
            focus = &draw;
        }
    }
    if (!focus) {
        // Nothing drew it (a title screen, a battle on top): show the map anyway.
        draws.push_back(WorldDraw{.world = w});
        focus = &draws.back();
    }
    focus->pixelRect = viewRect_.pixels;
    if (!gameCamera_) {
        focus->camera = editorCamera_.camera;
        focus->snapCamera = true;
    }
}

void Editor::drawViewportToolbar(World& w) {
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6, ImGui::GetCursorPosY() + 4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
    ImGui::TextDisabled(ICON_FA_LAYER_GROUP);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    const int layers = w.authored().grid.layerCount();
    ImGui::SliderInt("##layer", &layer_, 0, std::max(0, layers - 1), "Layer %d");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The layer the grid overlay, zone fills and new objects use");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &showGrid_);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &handles_.labels);
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_CROSSHAIRS " Frame")) {
        frameSelection(w, alpha_);
    }
    ImGui::SameLine();
    const auto& cam = editorCamera_.camera;
    ImGui::TextDisabled("%s  yaw %.0f  pitch %.0f  dist %.1f", cam.orthographic ? "ortho" : "persp", cam.yaw, cam.pitch,
                        cam.distance);
    if (!hovered_.empty() && selectionValid(w, hovered_)) {
        ImGui::SameLine();
        ImGui::TextDisabled("  " ICON_FA_HAND_POINTER " %s", selectionLabel(w, hovered_).c_str());
    }
    ImGui::PopStyleVar();
}

void Editor::viewportCamera(World& w, bool hovered, bool active) {
    const auto& io = ImGui::GetIO();
    if (gameCamera_) {
        return;
    }
    if (!cameraAdopted_) {
        editorCamera_.adopt(w.camera());
        cameraAdopted_ = true;
    }
    if (!hovered && !active) {
        return;
    }
    const glm::vec2 delta(io.MouseDelta.x, io.MouseDelta.y);
    const bool orbit = ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !io.KeyShift;
    const bool altOrbit = ImGui::IsMouseDragging(ImGuiMouseButton_Left) && io.KeyAlt;
    const bool pan = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                     (ImGui::IsMouseDragging(ImGuiMouseButton_Right) && io.KeyShift);
    if (pan) {
        editorCamera_.pan(delta, viewportSizePoints_.y);
    } else if (orbit || altOrbit) {
        editorCamera_.orbit(delta);
    }
    if (hovered && io.MouseWheel != 0) {
        editorCamera_.zoom(io.MouseWheel);
    }
}

void Editor::frameSelection(World& w, double alpha) {
    if (const auto bounds = selectionBounds(w, selection_, alpha)) {
        editorCamera_.frame(bounds->first, bounds->second);
        return;
    }
    const auto& g = w.authored().grid;
    editorCamera_.frame({0, 0, 0}, {static_cast<float>(g.width()), 2, static_cast<float>(g.depth())});
}

void Editor::drawHandles(World& w, ImDrawList* draw, double alpha) {
    handleBatch_.clear();
    handles_.layer = layer_;
    buildHandles(w, handles_, selection_, hovered_, alpha, handleBatch_, extra_);
    const auto& camera = sceneCamera(w);
    const float scale = pixelScale();
    const auto toPoints = [&](glm::vec3 world) -> std::optional<ImVec2> {
        const auto px = projectToView(camera, viewRect_, world, alpha);
        if (!px) {
            return std::nullopt;
        }
        return ImVec2(px->x / scale, px->y / scale);
    };
    const ImVec2 clipMin(viewportMinPoints_.x, viewportMinPoints_.y);
    const ImVec2 clipMax(viewportMinPoints_.x + viewportSizePoints_.x, viewportMinPoints_.y + viewportSizePoints_.y);
    draw->PushClipRect(clipMin, clipMax, true);
    for (const auto& line : handleBatch_.lines) {
        const auto a = toPoints(line.a), b = toPoints(line.b);
        if (a && b) {
            draw->AddLine(*a, *b, toColor(line.color), line.thickness);
        }
    }
    for (const auto& icon : handleBatch_.icons) {
        const auto p = toPoints(icon.position);
        if (!p) {
            continue;
        }
        const float r = icon.radiusPixels;
        const auto color = toColor(icon.color);
        const auto dark = toColor({0, 0, 0, icon.color.a * 0.6f});
        switch (icon.shape) {
        case HandleBatch::Icon::Shape::Diamond:
            draw->AddQuadFilled({p->x, p->y - r}, {p->x + r, p->y}, {p->x, p->y + r}, {p->x - r, p->y}, dark);
            draw->AddQuad({p->x, p->y - r}, {p->x + r, p->y}, {p->x, p->y + r}, {p->x - r, p->y}, color, 1.5f);
            break;
        case HandleBatch::Icon::Shape::Circle:
            draw->AddCircleFilled(*p, r, dark);
            draw->AddCircle(*p, r, color, 0, 1.5f);
            break;
        case HandleBatch::Icon::Shape::Square:
            draw->AddRectFilled({p->x - r, p->y - r}, {p->x + r, p->y + r}, dark);
            draw->AddRect({p->x - r, p->y - r}, {p->x + r, p->y + r}, color, 0, 0, 1.5f);
            break;
        case HandleBatch::Icon::Shape::Flag:
            draw->AddTriangleFilled(*p, {p->x + r * 1.6f, p->y + r * 0.5f}, {p->x, p->y + r}, color);
            break;
        case HandleBatch::Icon::Shape::Sun:
            draw->AddCircleFilled(*p, r * 0.6f, color);
            for (int i = 0; i < 8; ++i) {
                const float angle = glm::two_pi<float>() * static_cast<float>(i) / 8;
                const ImVec2 from(p->x + std::cos(angle) * r * 0.8f, p->y + std::sin(angle) * r * 0.8f);
                const ImVec2 to(p->x + std::cos(angle) * r * 1.3f, p->y + std::sin(angle) * r * 1.3f);
                draw->AddLine(from, to, color, 1.5f);
            }
            break;
        }
    }
    if (handles_.labels) {
        for (const auto& label : handleBatch_.labels) {
            const auto p = toPoints(label.position);
            if (!p) {
                continue;
            }
            const ImVec2 at(p->x + 9, p->y - 7);
            draw->AddText({at.x + 1, at.y + 1}, toColor({0, 0, 0, 0.8f}), label.text.c_str());
            draw->AddText(at, toColor(label.color), label.text.c_str());
        }
    }
    draw->PopClipRect();
}

bool Editor::drawGizmo(World& w, ImDrawList* draw, const ImVec2& min, const ImVec2& size, double alpha) {
    gizmoUsing_ = false;
    if (tool_ == Tool::Select || tool_ == Tool::Paint || !selectionValid(w, selection_) || gameCamera_) {
        return false;
    }
    auto transform = selectionTransform(w, selection_);
    if (!transform) {
        return false;
    }
    ImGuizmo::OPERATION operation;
    if (tool_ == Tool::Translate) {
        operation = ImGuizmo::TRANSLATE;
    } else if (tool_ == Tool::Rotate) {
        if (!transform->canRotate) {
            return false;
        }
        operation = ImGuizmo::ROTATE_Y;
    } else {
        if (!transform->canScale) {
            return false;
        }
        operation = transform->uniformScale ? ImGuizmo::SCALEU : ImGuizmo::SCALE;
    }
    const auto& camera = sceneCamera(w);
    ImGuizmo::SetDrawlist(draw);
    ImGuizmo::SetRect(min.x, min.y, size.x, size.y);
    ImGuizmo::SetOrthographic(camera.orthographic);
    ImGuizmo::AllowAxisFlip(false);
    const auto view = camera.view(alpha);
    const auto projection = camera.projection(viewRect_.aspect(), true, alpha);
    const float yaw = transform->facingOnly ? std::atan2(transform->facing.x, transform->facing.y)
                                            : glm::radians(transform->yawDegrees);
    auto model = glm::translate(glm::mat4(1), transform->position) * glm::rotate(glm::mat4(1), yaw, glm::vec3(0, 1, 0)) *
                 glm::scale(glm::mat4(1), transform->scale);
    const float translateSnap[3] = {0.5f, 0.25f, 0.5f};
    const float rotateSnap[3] = {15, 15, 15};
    const float scaleSnap[3] = {0.25f, 0.25f, 0.25f};
    const float* snap = nullptr;
    if (snap_ && !ImGui::GetIO().KeyCtrl) {
        snap = operation == ImGuizmo::TRANSLATE ? translateSnap : operation == ImGuizmo::ROTATE_Y ? rotateSnap : scaleSnap;
    }
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), operation,
                                              ImGuizmo::LOCAL, glm::value_ptr(model), nullptr, snap);
    gizmoUsing_ = ImGuizmo::IsUsing();
    if (!changed || !gizmoUsing_) {
        return gizmoUsing_ || ImGuizmo::IsOver();
    }
    // Read the matrix back: T * Ry(yaw) * S.
    SelectionTransform next = *transform;
    next.position = glm::vec3(model[3]);
    const glm::vec3 sx(model[0]), sy(model[1]), sz(model[2]);
    next.scale = {glm::length(sx), glm::length(sy), glm::length(sz)};
    if (transform->uniformScale) {
        next.scale = glm::vec3(next.scale.x);
    }
    const float newYaw = std::atan2(-model[0][2], model[0][0]);
    if (transform->facingOnly) {
        next.facing = {std::sin(newYaw), std::cos(newYaw)};
        next.yawDegrees = transform->yawDegrees;
    } else {
        next.yawDegrees = glm::degrees(newYaw);
    }
    if (operation != ImGuizmo::TRANSLATE) {
        next.position = transform->position;
    }
    if (operation != ImGuizmo::SCALE && operation != ImGuizmo::SCALEU) {
        next.scale = transform->scale;
    }
    const auto delta = next.position - transform->position;
    if (selection_.kind == Selection::Kind::Entity) {
        auto& t = w.registry().get<Transform>(selection_.entity);
        editTransform(w, selection_.entity, next.position, t.layer);
        t.facing = glm::length(next.facing) > 1e-4f ? glm::normalize(next.facing) : t.facing;
        if (!transform->facingOnly) {
            t.yaw = glm::radians(next.yawDegrees);
            t.previousYaw = t.yaw;
        }
        t.scale = next.scale;
        moveExtras(w, delta);
        return true;
    }
    const auto selection = selection_;
    edit(w, [&](MapData& m) { applySelectionTransform(m, selection, next); });
    moveExtras(w, delta);
    if (selection.kind == Selection::Kind::Placement) {
        if (const auto* p = findPlacement(w.authored(), selection.id)) {
            syncPlacementEntity(w, *p);
            if (transform->scale != next.scale && !w.registry().all_of<MeshInstance>(placementEntity(w, selection.id))) {
                placementChanged(w, selection.id); // sprite scale needs a respawn (deferred to the gesture's end)
            }
        }
    }
    return true;
}

void Editor::groundDrag(World& w, bool hovered, bool active, double alpha) {
    const bool draggable = selection_.kind == Selection::Kind::Placement || selection_.kind == Selection::Kind::Point ||
                           selection_.kind == Selection::Kind::Entity;
    if (!groundDragging_) {
        if (tool_ == Tool::Paint || gizmoUsing_ || !hovered || !draggable || !selectionValid(w, selection_)) {
            return;
        }
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::GetIO().KeyAlt || hovered_ != selection_ ||
            ImGuizmo::IsOver()) {
            return;
        }
        groundDragging_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !selectionValid(w, selection_)) {
        groundDragging_ = false;
        return;
    }
    if (!active && !hovered) {
        return;
    }
    const auto hit = pickGround(w, sceneCamera(w), viewRect_, mousePixels(), alpha);
    if (!hit) {
        return;
    }
    const auto& grid = w.authored().grid;
    const bool snapped = snap_ && !ImGui::GetIO().KeyCtrl;
    glm::vec3 position = snapped ? cellCentre(grid, hit->cell) : hit->point;
    const int layer = hit->cell.layer;
    if (snapped && selection_.kind == Selection::Kind::Placement) {
        // A model snaps by its footprint: even sizes to cell corners, odd ones to centres.
        if (const auto* p = findPlacement(w.authored(), selection_.id)) {
            if (const auto path = p->props.find("model"); path != p->props.end() && path->is_string()) {
                if (const auto model = engine_.assets().tryModel(path->get<std::string>())) {
                    position = snapForFootprint(grid, hit->point, footprintCells(*model, *p), layer);
                }
            }
        }
    }
    if (selection_.kind == Selection::Kind::Entity) {
        const auto before = w.registry().get<Transform>(selection_.entity).position;
        editTransform(w, selection_.entity, position, layer);
        moveExtras(w, position - before);
        return;
    }
    auto transform = selectionTransform(w, selection_);
    if (!transform || (transform->position == position && transform->layer == layer)) {
        return;
    }
    const auto delta = position - transform->position;
    transform->position = position;
    transform->layer = layer;
    const auto selection = selection_;
    edit(w, [&](MapData& m) { applySelectionTransform(m, selection, *transform); });
    if (selection.kind == Selection::Kind::Placement) {
        if (const auto* p = findPlacement(w.authored(), selection.id)) {
            syncPlacementEntity(w, *p);
        }
    }
    moveExtras(w, delta);
}

void Editor::pickAndSelect(World& w, bool hovered, bool clicked, double alpha) {
    hovered_ = {};
    if (!hovered || gizmoUsing_ || groundDragging_ || tool_ == Tool::Paint || ImGui::GetIO().KeyAlt) {
        return;
    }
    PickOptions options;
    options.camera = &sceneCamera(w);
    options.alpha = alpha;
    options.pixelsPerUnit = engine_.config().pixelsPerUnit;
    options.zones = handles_.zones;
    options.points = handles_.points;
    options.lights = handles_.lights;
    options.emitters = handles_.emitters;
    options.colliders = handles_.colliders;
    options.placements = handles_.placements || true; // spawned placements are always pickable
    if (const auto picked = pickScene(w, viewRect_, mousePixels(), options)) {
        hovered_ = *picked;
    }
    if (clicked && !ImGuizmo::IsOver()) {
        if (ImGui::GetIO().KeyCtrl && !selection_.empty() && !hovered_.empty()) {
            toggleExtra(hovered_);
        } else if (hovered_ != selection_ || !extra_.empty()) {
            select(hovered_);
        }
    }
}

void Editor::acceptViewportDrop(World& w, double alpha) {
    const auto place = [&](AssetKind kind, const std::string& path, glm::vec2 mouse) {
        const auto hit = pickGround(w, sceneCamera(w), viewRect_, mouse, alpha);
        if (!hit) {
            status_ = "Drop the asset onto the ground";
            return;
        }
        const auto model = kind == AssetKind::Model ? engine_.assets().tryModel(path) : nullptr;
        const auto placement = placementForAsset(kind, path, hit->cell, w.authored().grid, defaultType("entities"),
                                                 w.authored().placements, model.get(), hit->point);
        if (!placement) {
            status_ = "Only sprites (.anim.json) and models (.glb) can be placed in the scene";
            return;
        }
        gesture_ = false;
        edit(w, [&](MapData& m) { m.placements.push_back(*placement); });
        gesture_ = false;
        select(Selection::placement(placement->id));
        placementChanged(w, placement->id);
        tool_ = Tool::Translate;
        status_ = "Placed " + placement->id;
    };
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayloadType)) {
            const auto& asset = *static_cast<const AssetDragPayload*>(payload->Data);
            place(asset.kind, asset.path, mousePixels());
        }
        ImGui::EndDragDropTarget();
    }
    // Files dropped from the OS onto the Scene: ones already in the game folder are placed.
    for (const auto& [file, point] : droppedFiles_) {
        const glm::vec2 pixels = point * pixelScale();
        if (!viewRect_.contains(pixels)) {
            continue;
        }
        const auto relative = relativeTo(engine_.lua().gameRoot(), file);
        if (relative.empty()) {
            status_ = "Drop files onto the Assets panel to import them into the game first";
            continue;
        }
        place(assetKindOf(relative), relative, pixels);
    }
}

void Editor::drawViewport(const EditorFrame& /*frame*/) {
    auto* node = ImGui::DockBuilderGetCentralNode(dockspaceId_);
    if (!node) {
        return;
    }
    ImGui::SetNextWindowPos(node->Pos);
    ImGui::SetNextWindowSize(node->Size);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool begun = ImGui::Begin("##fe_scene", nullptr, flags);
    ImGui::PopStyleVar();
    viewportHovered_ = viewportActive_ = false;
    const auto w = world();
    if (begun) {
        if (w) {
            drawViewportToolbar(*w);
        } else {
            ImGui::TextDisabled("  No world loaded: start the game, or Play from a point");
        }
        const auto avail = ImGui::GetContentRegionAvail();
        if (avail.x > 8 && avail.y > 8) {
            const auto min = ImGui::GetCursorScreenPos();
            viewportMinPoints_ = {min.x, min.y};
            viewportSizePoints_ = {avail.x, avail.y};
            const float scale = pixelScale();
            viewRect_.pixels = {static_cast<int>(std::lround(min.x * scale)), static_cast<int>(std::lround(min.y * scale)),
                                static_cast<int>(std::lround(avail.x * scale)),
                                static_cast<int>(std::lround(avail.y * scale))};
            // The gizmo goes before the viewport's own button: ImGuizmo only
            // starts a drag while no ImGui item is hovered or active, and
            // IsAnyItemHovered() also counts last frame's item, so while the
            // pointer is over a gizmo handle (or a gizmo drag runs) the button
            // is left out, or the gizmo could never take the click.
            auto* draw = ImGui::GetWindowDrawList();
            const bool gizmo = w ? drawGizmo(*w, draw, min, avail, alpha_) : false;
            bool hovered = false, active = false;
            if (gizmo) {
                ImGui::Dummy(avail);
                hovered = true;
            } else {
                ImGui::InvisibleButton("##fe_viewport", avail,
                                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                           ImGuiButtonFlags_MouseButtonMiddle);
                hovered = ImGui::IsItemHovered();
                active = ImGui::IsItemActive();
            }
            viewportHovered_ = hovered;
            viewportActive_ = active;
            if (w) {
                layer_ = std::clamp(layer_, 0, w->authored().grid.layerCount() - 1);
                updateOverlays(*w);
                viewportCamera(*w, hovered && !gizmoUsing_, active && !gizmoUsing_);
                const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt;
                // Hover (what a click would pick), then dragging the selection
                // along the ground, then selecting.
                pickAndSelect(*w, hovered, false, alpha_);
                groundDrag(*w, hovered, active, alpha_);
                if (clicked && !gizmo && !groundDragging_) {
                    pickAndSelect(*w, hovered, true, alpha_);
                }
                drawHandles(*w, draw, alpha_);
                acceptViewportDrop(*w, alpha_);
                // Selection and hover lost to a respawn or undo.
                if (!selection_.empty() && !selectionValid(*w, selection_)) {
                    selection_ = {};
                }
            }
        }
    }
    ImGui::End();
}
} // namespace fe
