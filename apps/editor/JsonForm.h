#pragma once
#include "engine/core/Json.h"
#include <QStringList>
#include <QWidget>
#include <functional>
#include <map>
#include <string>
class QFormLayout;
class QVBoxLayout;

namespace fe::editor {
// What a form needs to know about the project to offer choices instead of
// free text: asset paths of a kind, Lua modules of a folder, scene ids, the
// record ids of a data table.
struct FormChoices {
    std::function<QStringList(const std::string& assetKind)> assets;
    std::function<QStringList(const std::string& folder)> modules;
    std::function<QStringList()> scenes;
    std::function<QStringList(const std::string& table)> records;
};

// A form over any JSON value: the Inspector's view of a scene object, a
// component, the environment, a data record, the project settings.
//
// Widgets follow the value's own types (number, text, checkbox, vector row,
// nested group, list). A game's field specs (the `fields` of a type in its
// editor.describe hook: string, number, integer, boolean, enum, asset,
// module, map_id, ref, list, record, map) refine an object's fields into
// pickers and add the fields the value does not hold yet. Every list and
// group can also be edited as JSON text, the fallback for whatever the
// widgets do not cover.
//
// The form does not own the data: an edit calls onEdited with the whole new
// value, and the owner answers by calling setValue again (with what it
// accepted). A setValue that changes only values updates the widgets in
// place, so focus and scroll survive edits and live updates.
class JsonForm final : public QWidget {
public:
    explicit JsonForm(QWidget* parent = nullptr);

    // `fields`: the spec object for the value's top-level fields, or null.
    // `fieldsAt`: specs for nested objects by JSON pointer ("/props").
    void setValue(const Json& value, const Json* fields = nullptr, std::map<std::string, Json> fieldsAt = {});
    [[nodiscard]] const Json& value() const { return value_; }

    // Keys shown but not editable, by name at any depth ("serial").
    void setReadOnly(QStringList keys) { readOnly_ = std::move(keys); }
    // Choices for plain string fields by key name ("type" -> point, spot):
    // what the engine's own objects accept.
    void setOptions(std::map<std::string, QStringList> options) { options_ = std::move(options); }
    void setChoices(FormChoices choices) { choices_ = std::move(choices); }

    std::function<void(const Json& value)> onEdited;

private:
    void rebuild();
    void addValue(QFormLayout* form, const QString& label, const std::string& pointer, const Json& value,
                  const Json* spec, const QString& tip);
    void addObject(QVBoxLayout* into, const std::string& pointer, const Json& object, const Json* fields);
    QWidget* scalarEditor(const std::string& pointer, const std::string& key, const Json& value,
                          const Json* spec);
    QWidget* vectorEditor(const std::string& pointer, const std::string& key, const Json& value);
    QWidget* listEditor(const std::string& pointer, const Json& value, const Json* spec);
    void commit(const std::string& pointer, const Json& value);
    void editAsText(const std::string& pointer);
    [[nodiscard]] const Json* specFieldsAt(const std::string& pointer) const;
    [[nodiscard]] std::string shapeOf(const Json& value) const;
    [[nodiscard]] bool readOnly(const std::string& key) const;

    Json value_;
    Json fields_;
    bool hasFields_ = false;
    std::map<std::string, Json> fieldsAt_;
    std::string shape_;
    std::map<std::string, std::function<void(const Json&)>> updaters_;
    QStringList readOnly_;
    std::map<std::string, QStringList> options_;
    FormChoices choices_;
    QVBoxLayout* layout_ = nullptr;
    QWidget* body_ = nullptr;
};

// Json <-> text for the "edit as JSON" dialogs and data files.
[[nodiscard]] QString jsonText(const Json& value);
} // namespace fe::editor
