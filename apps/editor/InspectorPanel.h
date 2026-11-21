#pragma once
#include "apps/editor/JsonForm.h"
#include "engine/editor/Selection.h"
#include <QScrollArea>
#include <memory>
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace fe {
class EditorSession;
class World;
} // namespace fe

namespace fe::editor {
// The Inspector: every value of what is selected, editable.
//
//  - a scene object: its fields as the map file holds them, with the game's
//    own form for its props (the type's fields from editor.describe);
//  - nothing selected: the scene itself (name, environment, props);
//  - while the game plays, a live entity: every component it has.
class InspectorPanel final : public QScrollArea {
public:
    explicit InspectorPanel(QWidget* parent = nullptr);
    void setChoices(const FormChoices& choices);

    // Edit mode. Call again whenever the selection or its values change.
    void inspect(EditorSession& session);
    // Play mode: the components of a live entity; again each frame that
    // should show fresh values. A dead entity shows as nothing.
    void inspectEntity(const std::shared_ptr<World>& world, const Selection& selection);
    void clear(const QString& message);

private:
    struct ComponentForm;
    void showProblem(const std::string& problem);
    void resetForms(int count);

    QWidget* body_ = nullptr;
    QVBoxLayout* column_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* problem_ = nullptr;
    QLineEdit* name_ = nullptr; // a point's name (its key in the map)
    std::vector<QLabel*> headings_;
    std::vector<JsonForm*> forms_;
    FormChoices choices_;
    EditorSession* session_ = nullptr;
    std::string shown_; // what the forms were built for
};
} // namespace fe::editor
