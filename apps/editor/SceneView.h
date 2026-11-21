#pragma once
#include "engine/editor/EditorCamera.h"
#include "engine/editor/MapEdits.h"
#include "engine/editor/SceneHandles.h"
#include "engine/editor/ScenePicking.h"
#include "engine/runtime/Engine.h"
#include <QStringList>
#include <functional>
#include <optional>
class QKeyEvent;

namespace fe {
class EditorSession;
}

namespace fe::editor {
class ViewportWindow;

// The Scene view: the open scene drawn with the editor's own camera, with
// the marks of what a scene does not show (zones, points, lights...), the
// selection's gizmo, and the mouse: click to select, drag to move, the
// camera on the right and middle buttons.
//
// The marks and the gizmo are an ImGui overlay drawn over the scene into the
// view's swap chain; ImGui gets this view's size and mouse and nothing else.
class SceneView {
public:
    enum class Tool { Select, Move, Rotate, Scale, Paint };

    SceneView(EditorSession& session, ViewportWindow& window, int pixelsPerUnit);

    [[nodiscard]] ViewportWindow& window() { return window_; }
    [[nodiscard]] EditorCamera& camera() { return camera_; }
    // Where new objects go: the ground under the camera's focus.
    [[nodiscard]] glm::vec3 focus() const { return camera_.camera.focus(); }

    Tool tool = Tool::Move;
    bool snap = true;
    HandleOptions handles;
    // The Paint tool: what it paints on the cells under the cursor, or
    // (clearing) that it removes hand-painted overrides.
    GridBrush brush;
    int brushRadius = 0;
    bool brushClears = false;
    // A square around the cursor, the rectangle dragged out, or every
    // connected cell like the one clicked.
    enum class BrushShape { Brush, Rectangle, Flood };
    BrushShape brushShape = BrushShape::Brush;
    // Marks the cells nobody can walk on and the hand-painted ones (always
    // on while painting).
    bool showWalkability = false;

    // Whether the gizmo has the mouse: hovered, or being dragged.
    [[nodiscard]] bool gizmoHovered() const { return gizmoOver_; }
    [[nodiscard]] bool gizmoDragging() const { return gizmoUsing_; }

    void frameScene();
    void frameSelection();

    // ImGui's display size, time step and mouse for this frame (the
    // renderer's platform hook).
    void platformFrame(float deltaSeconds);
    // The scene's draw, for the frame renderer.
    [[nodiscard]] WorldDraw draw() const;
    // After the frame's ImGui::NewFrame: handles, gizmo and this frame's
    // mouse work.
    void overlay();

    // Assets dropped on the view: project-relative paths and the ground
    // point under the cursor.
    std::function<void(const QStringList& paths, glm::vec3 point, GridPos cell)> onAssetsDropped;
    std::function<void(QKeyEvent* event)> onKey;

private:
    [[nodiscard]] ViewRect viewRect() const;
    void drawHandles();
    bool drawGizmo();
    void groundDrag(bool hovered);
    void pick(bool hovered, bool clicked);
    void drawWalkability();
    void paintCells(bool hovered);

    EditorSession& session_;
    ViewportWindow& window_;
    int pixelsPerUnit_;
    EditorCamera camera_;
    HandleBatch batch_;
    Selection hovered_;
    std::optional<glm::vec2> cameraDrag_;
    glm::vec2 mouse_{-1e6f};
    bool gizmoUsing_ = false, gizmoWasUsing_ = false, groundDragging_ = false, gizmoOver_ = false;
    bool leftDown_ = false, painting_ = false;
    std::optional<GridPos> rectStart_, rectEnd_;
};
} // namespace fe::editor
