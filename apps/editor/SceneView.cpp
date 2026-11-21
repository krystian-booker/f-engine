#include "apps/editor/SceneView.h"
#include "apps/editor/ViewportWindow.h"
#include "engine/editor/EditorSession.h"
#include "engine/editor/SceneEdits.h"
#include "engine/editor/WalkBake.h"
#include "engine/world/World.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
// ImGuizmo.h needs imgui.h before it.
#include <ImGuizmo.h>

namespace fe::editor {
namespace {
ImU32 toColor(glm::vec4 c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
}
} // namespace

SceneView::SceneView(EditorSession& session, ViewportWindow& window, int pixelsPerUnit)
    : session_(session), window_(window), pixelsPerUnit_(pixelsPerUnit) {
    // Unity's Scene view: right or Alt+left drag orbits, middle drag pans,
    // the wheel zooms. The left button alone belongs to the selection.
    window_.onPress = [this](const ViewportWindow::Pointer& pointer) {
        mouse_ = pointer.position;
        const bool alt = pointer.modifiers.testFlag(Qt::AltModifier);
        if (pointer.buttons.testFlag(Qt::RightButton) || pointer.buttons.testFlag(Qt::MiddleButton) ||
            (alt && pointer.buttons.testFlag(Qt::LeftButton))) {
            cameraDrag_ = pointer.position;
        }
        leftDown_ = pointer.buttons.testFlag(Qt::LeftButton) && !alt;
        window_.requestActivate();
    };
    window_.onRelease = [this](const ViewportWindow::Pointer& pointer) {
        mouse_ = pointer.position;
        leftDown_ = pointer.buttons.testFlag(Qt::LeftButton) && leftDown_;
        if (pointer.buttons == Qt::NoButton) {
            cameraDrag_.reset();
        }
    };
    window_.onMove = [this](const ViewportWindow::Pointer& pointer) {
        mouse_ = pointer.position;
        if (!cameraDrag_) {
            return;
        }
        const glm::vec2 delta = pointer.position - *cameraDrag_;
        cameraDrag_ = pointer.position;
        const bool alt = pointer.modifiers.testFlag(Qt::AltModifier);
        if (pointer.buttons.testFlag(Qt::RightButton) || (alt && pointer.buttons.testFlag(Qt::LeftButton))) {
            camera_.orbit(delta);
        } else if (pointer.buttons.testFlag(Qt::MiddleButton)) {
            camera_.pan(delta, static_cast<float>(window_.pixels().y));
        }
    };
    window_.onWheel = [this](float notches) {
        camera_.zoom(notches);
    };
    window_.onLeave = [this] {
        if (!leftDown_ && !cameraDrag_) {
            mouse_ = glm::vec2(-1e6f);
        }
    };
    window_.onKey = [this](QKeyEvent* event) {
        if (onKey) {
            onKey(event);
        }
    };
    window_.onDrop = [this](const QStringList& paths, glm::vec2 position) {
        const auto& world = session_.world();
        if (!world || !onAssetsDropped) {
            return;
        }
        const auto hit = pickGround(*world, camera_.camera, viewRect(), position, 1.0);
        if (hit) {
            onAssetsDropped(paths, hit->point, hit->cell);
        }
    };
}

ViewRect SceneView::viewRect() const {
    const auto size = window_.pixels();
    return ViewRect{glm::ivec4(0, 0, std::max(size.x, 1), std::max(size.y, 1))};
}

void SceneView::frameScene() {
    if (const auto* map = session_.map()) {
        camera_.frame({0, 0, 0},
                      {static_cast<float>(map->grid.width()), 2, static_cast<float>(map->grid.depth())});
    }
}

void SceneView::frameSelection() {
    const auto& world = session_.world();
    if (!world) {
        return;
    }
    if (const auto bounds = selectionBounds(*world, session_.selection(), 1.0)) {
        camera_.frame(bounds->first, bounds->second);
    } else {
        frameScene();
    }
}

void SceneView::platformFrame(float deltaSeconds) {
    auto& io = ImGui::GetIO();
    const auto size = window_.pixels();
    io.DisplaySize = ImVec2(static_cast<float>(std::max(size.x, 1)), static_cast<float>(std::max(size.y, 1)));
    io.DeltaTime = deltaSeconds;
    io.FontGlobalScale = static_cast<float>(window_.devicePixelRatio());
    io.AddMousePosEvent(mouse_.x, mouse_.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, leftDown_);
    const auto modifiers = QGuiApplication::keyboardModifiers();
    io.AddKeyEvent(ImGuiMod_Ctrl, modifiers.testFlag(Qt::ControlModifier));
    io.AddKeyEvent(ImGuiMod_Shift, modifiers.testFlag(Qt::ShiftModifier));
    io.AddKeyEvent(ImGuiMod_Alt, modifiers.testFlag(Qt::AltModifier));
}

WorldDraw SceneView::draw() const {
    WorldDraw draw;
    draw.world = session_.world();
    draw.camera = camera_.camera;
    draw.snapCamera = true;
    return draw;
}

void SceneView::overlay() {
    ImGuizmo::BeginFrame();
    const auto& world = session_.world();
    if (!world) {
        return;
    }
    const auto rect = viewRect();
    const bool hovered = rect.contains(mouse_) && !cameraDrag_;
    handles.layer = session_.layer;
    handles.schemas = &session_.schemas();
    if (showWalkability || tool == Tool::Paint) {
        drawWalkability();
    }
    drawHandles();
    if (tool == Tool::Paint) {
        paintCells(hovered);
        return;
    }
    const bool overGizmo = drawGizmo();
    gizmoOver_ = overGizmo;
    if (gizmoWasUsing_ && !gizmoUsing_) {
        session_.endGesture();
    }
    gizmoWasUsing_ = gizmoUsing_;
    const bool wasDragging = groundDragging_;
    if (!overGizmo) {
        groundDrag(hovered);
    }
    if (wasDragging && !groundDragging_) {
        session_.endGesture();
    }
    pick(hovered && !overGizmo, ImGui::IsMouseClicked(ImGuiMouseButton_Left));
}

void SceneView::drawHandles() {
    auto& world = *session_.world();
    batch_.clear();
    buildHandles(world, handles, session_.selection(), hovered_, 1.0, batch_, session_.extras());
    const auto rect = viewRect();
    auto* draw = ImGui::GetBackgroundDrawList();
    const float scale = static_cast<float>(window_.devicePixelRatio());
    const auto project = [&](glm::vec3 point) -> std::optional<ImVec2> {
        const auto px = projectToView(camera_.camera, rect, point, 1.0);
        if (!px) {
            return std::nullopt;
        }
        return ImVec2(px->x, px->y);
    };
    for (const auto& line : batch_.lines) {
        const auto a = project(line.a), b = project(line.b);
        if (a && b) {
            draw->AddLine(*a, *b, toColor(line.color), line.thickness * scale);
        }
    }
    for (const auto& icon : batch_.icons) {
        const auto p = project(icon.position);
        if (!p) {
            continue;
        }
        const float r = icon.radiusPixels * scale;
        const auto color = toColor(icon.color);
        const auto dark = toColor({0, 0, 0, icon.color.a * 0.6f});
        const float width = 1.5f * scale;
        switch (icon.shape) {
        case HandleBatch::Icon::Shape::Diamond:
            draw->AddQuadFilled({p->x, p->y - r}, {p->x + r, p->y}, {p->x, p->y + r}, {p->x - r, p->y}, dark);
            draw->AddQuad({p->x, p->y - r}, {p->x + r, p->y}, {p->x, p->y + r}, {p->x - r, p->y}, color,
                          width);
            break;
        case HandleBatch::Icon::Shape::Circle:
            draw->AddCircleFilled(*p, r, dark);
            draw->AddCircle(*p, r, color, 0, width);
            break;
        case HandleBatch::Icon::Shape::Square:
            draw->AddRectFilled({p->x - r, p->y - r}, {p->x + r, p->y + r}, dark);
            draw->AddRect({p->x - r, p->y - r}, {p->x + r, p->y + r}, color, 0, 0, width);
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
                draw->AddLine(from, to, color, width);
            }
            break;
        }
    }
    if (handles.labels) {
        for (const auto& label : batch_.labels) {
            const auto p = project(label.position);
            if (!p) {
                continue;
            }
            const ImVec2 at(p->x + 9 * scale, p->y - 7 * scale);
            draw->AddText({at.x + 1, at.y + 1}, toColor({0, 0, 0, 0.8f}), label.text.c_str());
            draw->AddText(at, toColor(label.color), label.text.c_str());
        }
    }
}

// Crosses over the cells that cannot be walked on, outlines around the
// hand-painted ones, and the brush under the cursor.
void SceneView::drawWalkability() {
    const auto& map = session_.world()->authored();
    const auto& grid = map.grid;
    const int layer = std::clamp(session_.layer, 0, grid.layerCount() - 1);
    const auto rect = viewRect();
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto project = [&](float x, float z) -> std::optional<ImVec2> {
        const auto px =
            projectToView(camera_.camera, rect, {x, grid.heightAt({x, z}, layer) + 0.03f, z}, 1.0);
        return px ? std::optional<ImVec2>(ImVec2(px->x, px->y)) : std::nullopt;
    };
    const ImU32 blocked = toColor({0.95f, 0.25f, 0.2f, 0.75f});
    for (int z = 0; z < grid.depth(); ++z) {
        for (int x = 0; x < grid.width(); ++x) {
            const float fx = static_cast<float>(x), fz = static_cast<float>(z);
            if (grid.walkableAt({fx + 0.5f, fz + 0.5f}, layer)) {
                continue;
            }
            const auto a = project(fx + 0.2f, fz + 0.2f), b = project(fx + 0.8f, fz + 0.8f);
            const auto c = project(fx + 0.8f, fz + 0.2f), d = project(fx + 0.2f, fz + 0.8f);
            if (a && b && c && d) {
                draw->AddLine(*a, *b, blocked, 1.5f);
                draw->AddLine(*c, *d, blocked, 1.5f);
            }
        }
    }
    const auto outline = [&](GridPos cell, ImU32 color, float inset, float width) {
        const float x0 = static_cast<float>(cell.x) + inset, z0 = static_cast<float>(cell.z) + inset;
        const float x1 = static_cast<float>(cell.x + 1) - inset, z1 = static_cast<float>(cell.z + 1) - inset;
        const auto a = project(x0, z0), b = project(x1, z0), c = project(x1, z1), d = project(x0, z1);
        if (a && b && c && d) {
            draw->AddQuad(*a, *b, *c, *d, color, width);
        }
    };
    for (const auto& cell : overriddenCells(map, layer)) {
        outline(cell, toColor({0.3f, 0.75f, 1.0f, 0.9f}), 0.08f, 1.5f);
    }
    if (tool == Tool::Paint && brushShape == BrushShape::Brush && rect.contains(mouse_)) {
        if (const auto hit = pickGround(*session_.world(), camera_.camera, rect, mouse_, 1.0, 1u << layer)) {
            for (int dz = -brushRadius; dz <= brushRadius; ++dz) {
                for (int dx = -brushRadius; dx <= brushRadius; ++dx) {
                    outline({hit->cell.x + dx, hit->cell.z + dz, layer}, toColor({1, 1, 1, 0.95f}), 0.0f,
                            2.0f);
                }
            }
        }
    }
}

// The Paint tool. Brush: a stroke while the left button is down, one undo
// step. Rectangle: from the cell pressed to the cell released. Flood: a click.
void SceneView::paintCells(bool hovered) {
    hovered_ = {};
    const auto& grid = session_.world()->authored().grid;
    const int layer = std::clamp(session_.layer, 0, grid.layerCount() - 1);
    std::optional<GridPos> under;
    if (hovered) {
        if (const auto hit =
                pickGround(*session_.world(), camera_.camera, viewRect(), mouse_, 1.0, 1u << layer)) {
            under = GridPos{hit->cell.x, hit->cell.z, layer};
        }
    }
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (brushShape == BrushShape::Flood) {
        if (under && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            session_.floodWalkability(*under, brush);
        }
        return;
    }
    if (brushShape == BrushShape::Rectangle) {
        if (under && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            rectStart_ = under;
        }
        if (rectStart_ && under) {
            rectEnd_ = under;
        }
        if (rectStart_ && rectEnd_) {
            // The rectangle so far.
            auto* draw = ImGui::GetBackgroundDrawList();
            const auto corner = [&](int x, int z) -> std::optional<ImVec2> {
                const float fx = static_cast<float>(x), fz = static_cast<float>(z);
                const auto px = projectToView(camera_.camera, viewRect(),
                                              {fx, grid.heightAt({fx, fz}, layer) + 0.05f, fz}, 1.0);
                return px ? std::optional<ImVec2>(ImVec2(px->x, px->y)) : std::nullopt;
            };
            const int x0 = std::min(rectStart_->x, rectEnd_->x),
                      x1 = std::max(rectStart_->x, rectEnd_->x) + 1;
            const int z0 = std::min(rectStart_->z, rectEnd_->z),
                      z1 = std::max(rectStart_->z, rectEnd_->z) + 1;
            const auto a = corner(x0, z0), b = corner(x1, z0), c = corner(x1, z1), d = corner(x0, z1);
            if (a && b && c && d) {
                draw->AddQuad(*a, *b, *c, *d, toColor({1, 1, 1, 0.95f}), 2.0f);
            }
        }
        if (rectStart_ && !down) {
            if (rectEnd_) {
                session_.paintWalkabilityRect(*rectStart_, *rectEnd_, brush, brushClears);
            }
            rectStart_.reset();
            rectEnd_.reset();
        }
        return;
    }
    if (down && under) {
        session_.paintWalkability(*under, brushRadius, brush, brushClears, true);
        painting_ = true;
    } else if (painting_ && !down) {
        painting_ = false;
        session_.endGesture();
    }
}

// Returns whether the gizmo has the mouse (in use, or hovered).
bool SceneView::drawGizmo() {
    gizmoUsing_ = false;
    auto& world = *session_.world();
    const auto selection = session_.selection();
    if (tool == Tool::Select || tool == Tool::Paint || !selectionValid(world, selection)) {
        return false;
    }
    const auto transform = selectionTransform(world, selection);
    if (!transform) {
        return false;
    }
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (tool == Tool::Rotate) {
        if (!transform->canRotate) {
            return false;
        }
        operation = ImGuizmo::ROTATE_Y;
    } else if (tool == Tool::Scale) {
        if (!transform->canScale) {
            return false;
        }
        operation = transform->uniformScale ? ImGuizmo::SCALEU : ImGuizmo::SCALE;
    }
    const auto& camera = camera_.camera;
    const auto rect = viewRect();
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0, 0, static_cast<float>(rect.pixels.z), static_cast<float>(rect.pixels.w));
    ImGuizmo::SetOrthographic(camera.orthographic);
    ImGuizmo::AllowAxisFlip(false);
    const auto view = camera.view(1.0);
    const auto projection = camera.projection(rect.aspect(), true, 1.0);
    const float yaw = transform->facingOnly ? std::atan2(transform->facing.x, transform->facing.y)
                                            : glm::radians(transform->yawDegrees);
    auto model = glm::translate(glm::mat4(1), transform->position) *
                 glm::rotate(glm::mat4(1), yaw, glm::vec3(0, 1, 0)) *
                 glm::scale(glm::mat4(1), transform->scale);
    const float translateSnap[3] = {0.5f, 0.25f, 0.5f};
    const float rotateSnap[3] = {15, 15, 15};
    const float scaleSnap[3] = {0.25f, 0.25f, 0.25f};
    const float* snapping = nullptr;
    if (snap && !ImGui::GetIO().KeyCtrl) {
        snapping = operation == ImGuizmo::TRANSLATE  ? translateSnap
                   : operation == ImGuizmo::ROTATE_Y ? rotateSnap
                                                     : scaleSnap;
    }
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), operation,
                                              ImGuizmo::LOCAL, glm::value_ptr(model), nullptr, snapping);
    gizmoUsing_ = ImGuizmo::IsUsing();
    if (!changed || !gizmoUsing_) {
        return gizmoUsing_ || ImGuizmo::IsOver();
    }
    // Read the matrix back: T * Ry(yaw) * S.
    SelectionTransform next = *transform;
    next.position = glm::vec3(model[3]);
    next.scale = {glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                  glm::length(glm::vec3(model[2]))};
    if (transform->uniformScale) {
        next.scale = glm::vec3(next.scale.x);
    }
    const float newYaw = std::atan2(-model[0][2], model[0][0]);
    if (transform->facingOnly) {
        next.facing = {std::sin(newYaw), std::cos(newYaw)};
    } else {
        next.yawDegrees = glm::degrees(newYaw);
    }
    if (operation != ImGuizmo::TRANSLATE) {
        next.position = transform->position;
    }
    if (operation != ImGuizmo::SCALE && operation != ImGuizmo::SCALEU) {
        next.scale = transform->scale;
    }
    session_.setTransform(selection, next, true);
    return true;
}

// Dragging the selected placement or point across the ground.
void SceneView::groundDrag(bool hovered) {
    auto& world = *session_.world();
    const auto selection = session_.selection();
    const bool draggable =
        selection.kind == Selection::Kind::Placement || selection.kind == Selection::Kind::Point;
    if (!groundDragging_) {
        if (gizmoUsing_ || !hovered || !draggable || !selectionValid(world, selection) ||
            !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || hovered_ != selection ||
            ImGui::GetIO().KeyCtrl) {
            return;
        }
        groundDragging_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !selectionValid(world, selection)) {
        groundDragging_ = false;
        return;
    }
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
        return; // a click selects; only a real drag moves
    }
    const auto hit = pickGround(world, camera_.camera, viewRect(), mouse_, 1.0);
    if (!hit) {
        return;
    }
    const auto& grid = world.authored().grid;
    const bool snapped = snap && !ImGui::GetIO().KeyShift;
    glm::vec3 position = snapped ? cellCentre(grid, hit->cell) : hit->point;
    if (snapped && selection.kind == Selection::Kind::Placement) {
        // A model snaps by its footprint: even sizes to cell corners, odd ones to centres.
        if (const auto* placement = findPlacement(world.authored(), selection.id)) {
            if (const auto path = placement->props.find("model");
                path != placement->props.end() && path->is_string()) {
                if (const auto model = session_.engine().assets().tryModel(path->get<std::string>())) {
                    position = snapForFootprint(grid, hit->point, footprintCells(*model, *placement),
                                                hit->cell.layer);
                }
            }
        }
    }
    auto transform = selectionTransform(world, selection);
    if (!transform || (transform->position == position && transform->layer == hit->cell.layer)) {
        return;
    }
    transform->position = position;
    transform->layer = hit->cell.layer;
    session_.setTransform(selection, *transform, true);
}

void SceneView::pick(bool hovered, bool clicked) {
    hovered_ = {};
    if (!hovered || gizmoUsing_ || groundDragging_ || ImGui::GetIO().KeyAlt) {
        return;
    }
    auto& world = *session_.world();
    PickOptions options;
    options.camera = &camera_.camera;
    options.pixelsPerUnit = pixelsPerUnit_;
    options.zones = handles.zones;
    options.points = handles.points;
    options.lights = handles.lights;
    options.emitters = handles.emitters;
    options.colliders = handles.colliders;
    options.handleRadiusPixels = 10.0f * static_cast<float>(window_.devicePixelRatio());
    if (auto found = pickScene(world, viewRect(), mouse_, options)) {
        // The stand-in entity of a placement is the placement.
        if (found->kind == Selection::Kind::Entity) {
            const auto id = entityPlacement(world, found->entity);
            found = id.empty() ? std::optional<Selection>() : Selection::placement(id);
        }
        if (found) {
            hovered_ = *found;
        }
    }
    if (!clicked) {
        return;
    }
    if (ImGui::GetIO().KeyCtrl) {
        session_.toggle(hovered_);
    } else if (hovered_ != session_.selection() || !session_.extras().empty()) {
        session_.select(hovered_);
    }
}
} // namespace fe::editor
