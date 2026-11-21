#include "apps/editor/JsonForm.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace fe::editor {
namespace {
std::string text(const Json& object, const char* key, const std::string& fallback = {}) {
    const auto found = object.is_object() ? object.find(key) : object.end();
    return found != object.end() && found->is_string() ? found->get<std::string>() : fallback;
}

// "flicker_speed" -> "Flicker speed".
QString labelFor(const std::string& key) {
    QString label = QString::fromStdString(key);
    label.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!label.isEmpty()) {
        label[0] = label[0].toUpper();
    }
    return label;
}

bool numbers(const Json& value) {
    return value.is_array() && value.size() >= 2 && value.size() <= 4 &&
           std::all_of(value.begin(), value.end(), [](const Json& v) { return v.is_number(); });
}

bool colorKey(const std::string& key) {
    for (const char* word : {"color", "colour", "tint", "ambient"}) {
        if (key.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// What a field holds before anyone has set it.
Json defaultFor(const Json* spec) {
    if (!spec || !spec->is_object()) {
        return nullptr;
    }
    if (const auto given = spec->find("default"); given != spec->end()) {
        return *given;
    }
    const auto type = text(*spec, "type", "string");
    if (type == "number") {
        return 0.0;
    }
    if (type == "integer") {
        return 0;
    }
    if (type == "boolean") {
        return false;
    }
    if (type == "list") {
        return Json::array();
    }
    if (type == "record" || type == "map") {
        Json record = Json::object();
        if (const auto fields = spec->find("fields"); fields != spec->end() && fields->is_object()) {
            for (auto field = fields->begin(); field != fields->end(); ++field) {
                if (!field->value("optional", false)) {
                    record[field.key()] = defaultFor(&*field);
                }
            }
        }
        return record;
    }
    if (type == "enum") {
        const auto options = spec->find("options");
        return options != spec->end() && options->is_array() && !options->empty() ? options->front()
                                                                                  : Json("");
    }
    return "";
}

QDoubleSpinBox* numberBox(double value) {
    auto* box = new QDoubleSpinBox;
    box->setRange(-1e9, 1e9);
    box->setDecimals(3);
    box->setSingleStep(0.1);
    box->setKeyboardTracking(false);
    box->setValue(value);
    box->setMinimumWidth(60);
    return box;
}
} // namespace

QString jsonText(const Json& value) {
    return QString::fromStdString(value.dump(2));
}

JsonForm::JsonForm(QWidget* parent) : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
}

bool JsonForm::readOnly(const std::string& key) const {
    return readOnly_.contains(QString::fromStdString(key));
}

const Json* JsonForm::specFieldsAt(const std::string& pointer) const {
    if (pointer.empty()) {
        return hasFields_ ? &fields_ : nullptr;
    }
    const auto found = fieldsAt_.find(pointer);
    return found == fieldsAt_.end() ? nullptr : &found->second;
}

// The widget structure a value needs: every pointer with its type. Values
// with the same shape share widgets.
std::string JsonForm::shapeOf(const Json& value) const {
    std::string shape;
    const std::function<void(const Json&, const std::string&)> walk = [&](const Json& v,
                                                                          const std::string& at) {
        shape += at;
        shape += ':';
        shape += v.type_name();
        if (v.is_object()) {
            for (auto field = v.begin(); field != v.end(); ++field) {
                walk(*field, at + "/" + field.key());
            }
        } else if (v.is_array()) {
            shape += std::to_string(v.size());
            if (!numbers(v)) {
                for (std::size_t i = 0; i < v.size(); ++i) {
                    walk(v[i], at + "/" + std::to_string(i));
                }
            }
        }
        shape += ';';
    };
    walk(value, {});
    return shape;
}

void JsonForm::setValue(const Json& value, const Json* fields, std::map<std::string, Json> fieldsAt) {
    const Json newFields = fields ? *fields : Json();
    const bool sameSpecs = hasFields_ == (fields != nullptr) && fields_ == newFields && fieldsAt_ == fieldsAt;
    const auto shape = shapeOf(value);
    const bool sameShape = body_ && sameSpecs && shape == shape_;
    value_ = value;
    if (sameShape) {
        for (const auto& [pointer, update] : updaters_) {
            const Json::json_pointer at(pointer);
            if (value_.contains(at)) {
                update(value_.at(at));
            }
        }
        return;
    }
    fields_ = newFields;
    hasFields_ = fields != nullptr;
    fieldsAt_ = std::move(fieldsAt);
    shape_ = shape;
    rebuild();
}

void JsonForm::rebuild() {
    updaters_.clear();
    if (body_) {
        layout_->removeWidget(body_);
        body_->deleteLater();
    }
    body_ = new QWidget(this);
    auto* column = new QVBoxLayout(body_);
    column->setContentsMargins(0, 0, 0, 0);
    if (value_.is_object()) {
        addObject(column, {}, value_, specFieldsAt({}));
    } else if (!value_.is_null()) {
        auto* form = new QFormLayout;
        addValue(form, tr("Value"), {}, value_, nullptr, {});
        column->addLayout(form);
    }
    column->addStretch(1);
    layout_->addWidget(body_);
}

void JsonForm::addObject(QVBoxLayout* into, const std::string& pointer, const Json& object,
                         const Json* fields) {
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft);
    into->addLayout(form);
    // The value's own fields in file order, then the fields the game
    // describes that the value does not hold yet.
    for (auto field = object.begin(); field != object.end(); ++field) {
        const Json* spec = nullptr;
        if (fields && fields->is_object()) {
            if (const auto found = fields->find(field.key()); found != fields->end()) {
                spec = &*found;
            }
        }
        const QString tip = spec ? QString::fromStdString(text(*spec, "doc")) : QString();
        const QString label =
            spec ? QString::fromStdString(text(*spec, "label", field.key())) : labelFor(field.key());
        addValue(form, spec ? label : labelFor(field.key()), pointer + "/" + field.key(), *field, spec, tip);
    }
    if (fields && fields->is_object()) {
        for (auto spec = fields->begin(); spec != fields->end(); ++spec) {
            if (object.contains(spec.key()) || !spec->is_object()) {
                continue;
            }
            const QString label = QString::fromStdString(text(*spec, "label", spec.key()));
            const QString tip = QString::fromStdString(text(*spec, "doc"));
            // Shown with its default; the first edit writes the field.
            addValue(form, label, pointer + "/" + spec.key(), defaultFor(&*spec), &*spec, tip);
        }
    }
}

void JsonForm::addValue(QFormLayout* form, const QString& label, const std::string& pointer,
                        const Json& value, const Json* spec, const QString& tip) {
    const std::string key = pointer.substr(pointer.find_last_of('/') + 1);
    const std::string specType = spec ? text(*spec, "type") : std::string();
    auto* name = new QLabel(label);
    name->setToolTip(tip);
    if (value.is_object()) {
        // A nested group: its heading, then its fields indented.
        auto* heading = new QWidget;
        auto* row = new QHBoxLayout(heading);
        row->setContentsMargins(0, 6, 0, 0);
        QFont bold = name->font();
        bold.setBold(true);
        name->setFont(bold);
        row->addWidget(name, 1);
        auto* edit = new QToolButton;
        edit->setText(QStringLiteral("{ }"));
        edit->setToolTip(tr("Edit as JSON"));
        QObject::connect(edit, &QToolButton::clicked, this, [this, pointer] { editAsText(pointer); });
        row->addWidget(edit);
        form->addRow(heading);
        auto* group = new QWidget;
        auto* column = new QVBoxLayout(group);
        column->setContentsMargins(12, 0, 0, 0);
        const Json* fields = specFieldsAt(pointer);
        if (!fields && spec && specType == "record") {
            if (const auto own = spec->find("fields"); own != spec->end()) {
                fields = &*own;
            }
        }
        addObject(column, pointer, value, fields);
        form->addRow(group);
        return;
    }
    QWidget* editor = nullptr;
    if (numbers(value) && specType.empty()) {
        editor = vectorEditor(pointer, key, value);
    } else if (value.is_array()) {
        editor = listEditor(pointer, value, spec);
    } else {
        editor = scalarEditor(pointer, key, value, spec);
    }
    editor->setToolTip(tip);
    editor->setEnabled(!readOnly(key));
    form->addRow(name, editor);
}

QWidget* JsonForm::scalarEditor(const std::string& pointer, const std::string& key, const Json& value,
                                const Json* spec) {
    const std::string type = spec ? text(*spec, "type") : std::string();
    if (value.is_boolean() || type == "boolean") {
        auto* box = new QCheckBox;
        box->setChecked(value.is_boolean() && value.get<bool>());
        QObject::connect(box, &QCheckBox::toggled, this, [this, pointer](bool on) { commit(pointer, on); });
        updaters_[pointer] = [box](const Json& v) {
            const QSignalBlocker blocker(box);
            box->setChecked(v.is_boolean() && v.get<bool>());
        };
        return box;
    }
    const bool integer = type == "integer" || (type.empty() && value.is_number_integer());
    if (integer) {
        auto* box = new QSpinBox;
        box->setRange(-1000000000, 1000000000);
        box->setKeyboardTracking(false);
        box->setValue(value.is_number() ? static_cast<int>(value.get<double>()) : 0);
        QObject::connect(box, &QSpinBox::valueChanged, this, [this, pointer](int v) { commit(pointer, v); });
        updaters_[pointer] = [box](const Json& v) {
            const QSignalBlocker blocker(box);
            box->setValue(v.is_number() ? static_cast<int>(v.get<double>()) : 0);
        };
        return box;
    }
    if (value.is_number() || type == "number") {
        auto* box = numberBox(value.is_number() ? value.get<double>() : 0.0);
        QObject::connect(box, &QDoubleSpinBox::valueChanged, this,
                         [this, pointer](double v) { commit(pointer, v); });
        updaters_[pointer] = [box](const Json& v) {
            const QSignalBlocker blocker(box);
            box->setValue(v.is_number() ? v.get<double>() : 0.0);
        };
        return box;
    }
    if (value.is_null() && type.empty()) {
        auto* edit = new QPushButton(tr("null (edit as JSON)"));
        QObject::connect(edit, &QPushButton::clicked, this, [this, pointer] { editAsText(pointer); });
        return edit;
    }
    // Text: a list of choices when the game's spec or the engine names them.
    const QString current = value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString();
    QStringList choices;
    bool strict = false; // only the listed values are valid
    if (type == "enum") {
        if (const auto options = spec->find("options"); options != spec->end() && options->is_array()) {
            for (const auto& option : *options) {
                if (option.is_string()) {
                    choices << QString::fromStdString(option.get<std::string>());
                }
            }
        }
        strict = true;
    } else if (type == "asset" && choices_.assets) {
        choices = choices_.assets(text(*spec, "asset"));
    } else if (type == "module" && choices_.modules) {
        choices = choices_.modules(text(*spec, "folder"));
    } else if (type == "map_id" && choices_.scenes) {
        choices = choices_.scenes();
    } else if (type == "ref" && choices_.records) {
        choices = choices_.records(text(*spec, "table"));
    } else if (const auto found = options_.find(key); type.empty() && found != options_.end()) {
        choices = found->second;
        strict = true;
    } else if (type.empty() && choices_.assets && (key == "model" || key == "sprite")) {
        choices = choices_.assets(key);
    }
    if (!choices.isEmpty() || strict) {
        auto* box = new QComboBox;
        box->setEditable(!strict);
        if (!strict) {
            box->addItem(QString()); // optional fields can be cleared
        }
        box->addItems(choices);
        if (box->findText(current) < 0) {
            box->addItem(current);
        }
        box->setCurrentText(current);
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(8);
        const auto send = [this, pointer, box] {
            commit(pointer, box->currentText().toStdString());
        };
        QObject::connect(box, &QComboBox::activated, this, send);
        if (!strict) {
            QObject::connect(box->lineEdit(), &QLineEdit::editingFinished, this, send);
        }
        updaters_[pointer] = [box](const Json& v) {
            const QSignalBlocker blocker(box);
            const QString now = v.is_string() ? QString::fromStdString(v.get<std::string>()) : QString();
            if (box->findText(now) < 0) {
                box->addItem(now);
            }
            box->setCurrentText(now);
        };
        return box;
    }
    auto* line = new QLineEdit(current);
    QObject::connect(line, &QLineEdit::editingFinished, this,
                     [this, pointer, line] { commit(pointer, line->text().toStdString()); });
    updaters_[pointer] = [line](const Json& v) {
        if (!line->hasFocus()) {
            line->setText(v.is_string() ? QString::fromStdString(v.get<std::string>()) : QString());
        }
    };
    return line;
}

QWidget* JsonForm::vectorEditor(const std::string& pointer, const std::string& key, const Json& value) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    std::vector<QDoubleSpinBox*> boxes;
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto* box = numberBox(value[i].get<double>());
        boxes.push_back(box);
        layout->addWidget(box, 1);
    }
    const auto read = [boxes] {
        Json out = Json::array();
        for (const auto* box : boxes) {
            out.push_back(box->value());
        }
        return out;
    };
    for (auto* box : boxes) {
        QObject::connect(box, &QDoubleSpinBox::valueChanged, this,
                         [this, pointer, read] { commit(pointer, read()); });
    }
    QToolButton* swatch = nullptr;
    if (colorKey(key) && value.size() >= 3) {
        swatch = new QToolButton;
        swatch->setFixedWidth(26);
        const auto paint = [swatch](const Json& v) {
            const auto channel = [&](std::size_t i) {
                return std::clamp(static_cast<int>(std::lround(v[i].get<double>() * 255.0)), 0, 255);
            };
            swatch->setStyleSheet(QStringLiteral("background-color: rgb(%1,%2,%3); border: 1px solid #555;")
                                      .arg(channel(0))
                                      .arg(channel(1))
                                      .arg(channel(2)));
        };
        paint(value);
        QObject::connect(swatch, &QToolButton::clicked, this, [this, pointer, read] {
            Json now = read();
            const QColor start =
                QColor::fromRgbF(static_cast<float>(std::clamp(now[0].get<double>(), 0.0, 1.0)),
                                 static_cast<float>(std::clamp(now[1].get<double>(), 0.0, 1.0)),
                                 static_cast<float>(std::clamp(now[2].get<double>(), 0.0, 1.0)));
            const QColor picked = QColorDialog::getColor(start, this);
            if (picked.isValid()) {
                now[0] = picked.redF();
                now[1] = picked.greenF();
                now[2] = picked.blueF();
                commit(pointer, now);
            }
        });
        layout->addWidget(swatch);
        updaters_[pointer] = [boxes, paint](const Json& v) {
            for (std::size_t i = 0; i < boxes.size() && i < v.size(); ++i) {
                const QSignalBlocker blocker(boxes[i]);
                boxes[i]->setValue(v[i].get<double>());
            }
            paint(v);
        };
        return row;
    }
    updaters_[pointer] = [boxes](const Json& v) {
        for (std::size_t i = 0; i < boxes.size() && i < v.size(); ++i) {
            const QSignalBlocker blocker(boxes[i]);
            boxes[i]->setValue(v[i].get<double>());
        }
    };
    return row;
}

QWidget* JsonForm::listEditor(const std::string& pointer, const Json& value, const Json* spec) {
    auto* box = new QWidget;
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    const Json* of = nullptr;
    if (spec) {
        if (const auto found = spec->find("of"); found != spec->end() && found->is_object()) {
            of = &*found;
        }
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::string at = pointer + "/" + std::to_string(i);
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        addValue(form, QString::number(i + 1), at, value[i], of, {});
        layout->addLayout(form, 1);
        auto* remove = new QToolButton;
        remove->setText(QStringLiteral("−"));
        remove->setToolTip(tr("Remove"));
        QObject::connect(remove, &QToolButton::clicked, this, [this, pointer, i] {
            Json list = value_.at(Json::json_pointer(pointer));
            list.erase(list.begin() + static_cast<Json::difference_type>(i));
            commit(pointer, list);
        });
        layout->addWidget(remove, 0, Qt::AlignTop);
        column->addWidget(row);
    }
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("+ Add"));
    QObject::connect(add, &QPushButton::clicked, this, [this, pointer, of] {
        const Json::json_pointer at(pointer);
        Json list = value_.contains(at) ? value_.at(at) : Json::array();
        // A new entry: the spec's default, else a copy of the last one.
        list.push_back(of ? defaultFor(of) : list.empty() ? Json("") : list.back());
        commit(pointer, list);
    });
    buttons->addWidget(add);
    auto* edit = new QToolButton;
    edit->setText(QStringLiteral("[ ]"));
    edit->setToolTip(tr("Edit as JSON"));
    QObject::connect(edit, &QToolButton::clicked, this, [this, pointer] { editAsText(pointer); });
    buttons->addWidget(edit);
    buttons->addStretch(1);
    column->addLayout(buttons);
    return box;
}

void JsonForm::commit(const std::string& pointer, const Json& value) {
    Json next = value_;
    next[Json::json_pointer(pointer)] = value;
    if (next == value_) {
        return;
    }
    if (onEdited) {
        onEdited(next);
    }
}

void JsonForm::editAsText(const std::string& pointer) {
    const Json::json_pointer at(pointer);
    const Json current = value_.contains(at) ? value_.at(at) : Json();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit as JSON"));
    dialog.resize(520, 420);
    auto* column = new QVBoxLayout(&dialog);
    auto* editor = new QPlainTextEdit(jsonText(current));
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    editor->setFont(mono);
    column->addWidget(editor);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    column->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto parsed = Json::parse(editor->toPlainText().toStdString(), nullptr, false);
        if (parsed.is_discarded()) {
            QMessageBox::warning(&dialog, tr("Edit as JSON"), tr("This is not valid JSON."));
            return;
        }
        dialog.accept();
        commit(pointer, parsed);
    });
    dialog.exec();
}
} // namespace fe::editor
