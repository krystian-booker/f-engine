#pragma once
#include "engine/editor/Selection.h"
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>
class QLineEdit;
class QMenu;
class QTreeWidget;
class QTreeWidgetItem;

namespace fe {
class EditorSession;
class World;
} // namespace fe

namespace fe::editor {
// The Hierarchy: the open scene's objects grouped by kind, or, while the
// game plays, the live entities of the world on screen. Selection goes both
// ways with whoever owns it (the session in Edit mode, the play session in
// Play mode).
class HierarchyPanel final : public QWidget {
public:
    explicit HierarchyPanel(QWidget* parent = nullptr);

    // Edit mode: the session's scene. Rebuilds the tree; call again when the
    // structure changes.
    void showScene(EditorSession& session);
    // Play mode: the entities of `world` (authored ones under their
    // placement's name, the rest as spawned at run time).
    void showEntities(const std::shared_ptr<World>& world, const std::vector<std::string>& worlds);
    // Marks the rows of what is selected, without telling anyone.
    void setSelected(const std::vector<Selection>& selected);

    std::function<void(const std::vector<Selection>& selected)> onSelected;
    std::function<void(const Selection& selection)> onActivated; // double click: frame it
    std::function<void(QMenu& menu, const Selection& under)> onContextMenu;
    std::function<void(const std::string& world)> onWorldChosen; // Play mode: a world's heading clicked

private:
    QTreeWidgetItem* row(QTreeWidgetItem* parent, const QString& label, const Selection& selection);
    QTreeWidgetItem* section(const QString& name, std::size_t count);
    void applyFilter();
    [[nodiscard]] Selection selectionOf(const QTreeWidgetItem* row) const;

    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    std::vector<Selection> rows_; // by the index stored in each row
    bool syncing_ = false;
};
} // namespace fe::editor
