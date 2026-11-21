#include "apps/editor/InspectorPanel.h"
#include "engine/editor/EditorSession.h"
#include "engine/editor/WalkBake.h"
#include "engine/world/ComponentInfo.h"
#include "engine/world/World.h"
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace fe::editor {
namespace {
QStringList list(const std::vector<std::string>& names) {
    QStringList out;
    for (const auto& name : names) {
        out << QString::fromStdString(name);
    }
    return out;
}
} // namespace

InspectorPanel::InspectorPanel(QWidget* parent) : QScrollArea(parent) {
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    body_ = new QWidget(this);
    column_ = new QVBoxLayout(body_);
    title_ = new QLabel(body_);
    title_->setObjectName(QStringLiteral("inspectorTitle"));
    QFont bold = title_->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.15);
    title_->setFont(bold);
    column_->addWidget(title_);
    name_ = new QLineEdit(body_);
    name_->setObjectName(QStringLiteral("inspectorName"));
    name_->setVisible(false);
    column_->addWidget(name_);
    problem_ = new QLabel(body_);
    problem_->setObjectName(QStringLiteral("inspectorProblem"));
    problem_->setWordWrap(true);
    problem_->setStyleSheet(QStringLiteral("color: #e0603a;"));
    problem_->setVisible(false);
    column_->addWidget(problem_);
    column_->addStretch(1);
    setWidget(body_);
    QObject::connect(name_, &QLineEdit::editingFinished, this, [this] {
        if (session_ && session_->selection().kind == Selection::Kind::Point) {
            session_->renamePoint(session_->selection().id, name_->text().toStdString());
        }
    });
}

void InspectorPanel::setChoices(const FormChoices& choices) {
    choices_ = choices;
    for (auto* form : forms_) {
        form->setChoices(choices_);
    }
}

void InspectorPanel::showProblem(const std::string& problem) {
    problem_->setText(QString::fromStdString(problem));
    problem_->setVisible(!problem.empty());
}

// Leaves exactly `count` forms (each under a heading), reusing the ones there.
void InspectorPanel::resetForms(int count) {
    while (static_cast<int>(forms_.size()) > count) {
        forms_.back()->deleteLater();
        headings_.back()->deleteLater();
        forms_.pop_back();
        headings_.pop_back();
    }
    while (static_cast<int>(forms_.size()) < count) {
        auto* heading = new QLabel(body_);
        QFont bold = heading->font();
        bold.setBold(true);
        heading->setFont(bold);
        heading->setStyleSheet(QStringLiteral("padding-top: 8px; border-top: 1px solid palette(mid);"));
        auto* form = new JsonForm(body_);
        form->setChoices(choices_);
        column_->insertWidget(column_->count() - 1, heading);
        column_->insertWidget(column_->count() - 1, form);
        headings_.push_back(heading);
        forms_.push_back(form);
    }
}

void InspectorPanel::clear(const QString& message) {
    session_ = nullptr;
    shown_.clear();
    resetForms(0);
    name_->setVisible(false);
    showProblem({});
    title_->setText(message);
}

void InspectorPanel::inspect(EditorSession& session) {
    session_ = &session;
    const auto& world = session.world();
    if (!world) {
        clear(tr("No scene is open"));
        session_ = &session;
        return;
    }
    const auto selection = session.selection();
    const std::string key = "edit:" + std::to_string(static_cast<int>(selection.kind)) + ":" + selection.id +
                            ":" + std::to_string(selection.index);
    if (key != shown_) {
        showProblem({});
        shown_ = key;
    }
    resetForms(1);
    headings_.front()->setVisible(false);
    auto* form = forms_.front();
    form->setReadOnly({});
    name_->setVisible(selection.kind == Selection::Kind::Point);

    if (selection.empty() || !selectionValid(*world, selection)) {
        // The scene itself.
        const auto* map = session.map();
        title_->setText(tr("Scene: %1").arg(QString::fromStdString(map->id)));
        // The bake's settings as the bake reads them: the file's, over the
        // defaults for whatever it does not say.
        Json bakeOptions = BakeOptions::fromJson(map->bake.options).toJson();
        bakeOptions.erase("region");
        Json links = session.mapJson("/grid/links");
        Json value = {{"name", session.mapJson("/name")},
                      {"props", session.mapJson("/props")},
                      {"walkability_bake", {{"auto", map->bake.autoBake}, {"options", bakeOptions}}},
                      {"layer_links", links.is_array() ? links : Json::array()},
                      {"environment", session.mapJson("/environment")}};
        std::map<std::string, Json> specs;
        if (const auto* fields = session.fieldsFor("map_props", {"schemas"})) {
            specs["/props"] = *fields;
        }
        form->setOptions({});
        form->onEdited = [this, form, &session](const Json& edited) {
            std::string problem;
            for (const char* part : {"name", "environment", "props"}) {
                if (edited.at(part) != form->value().at(part)) {
                    problem = session.setMapJson(std::string("/") + part, edited.at(part));
                }
            }
            if (edited.at("layer_links") != form->value().at("layer_links")) {
                problem =
                    session.setMapJson("/grid/links", edited.at("layer_links"), EditorSession::ValuesChanged);
            }
            const auto& bake = edited.at("walkability_bake");
            if (bake != form->value().at("walkability_bake")) {
                // Options that change what is walkable: bake again with them.
                const bool optionsChanged =
                    bake.at("options") != form->value().at("walkability_bake").at("options");
                problem = session.setMapJson("/bake/auto", bake.at("auto"));
                if (problem.empty() && optionsChanged) {
                    problem = session.setMapJson("/bake/options", bake.at("options"));
                    if (problem.empty()) {
                        session.bake(std::nullopt, true);
                        session.endGesture();
                    }
                }
            }
            showProblem(problem);
        };
        form->setValue(value, nullptr, std::move(specs));
        return;
    }

    title_->setText(QString::fromStdString(selectionLabel(*world, selection)));
    if (selection.kind == Selection::Kind::Point && !name_->hasFocus()) {
        name_->setText(QString::fromStdString(selection.id));
    }
    const Json value = session.objectJson(selection);
    std::map<std::string, Json> specs;
    std::map<std::string, QStringList> options;
    const auto type = value.is_object() && value.contains("type") && value.at("type").is_string()
                          ? value.at("type").get<std::string>()
                          : std::string();
    switch (selection.kind) {
    case Selection::Kind::Placement:
        if (const auto* fields = session.fieldsFor(type, {"entities"})) {
            specs["/props"] = *fields;
        }
        options["type"] = list(session.typeNames("entities"));
        break;
    case Selection::Kind::Zone:
        if (const auto* fields = session.fieldsFor(type, {"zones"})) {
            specs["/props"] = *fields;
        }
        options["type"] = list(session.typeNames("zones"));
        options["shape"] = {QStringLiteral("box"), QStringLiteral("ellipse"), QStringLiteral("polygon")};
        break;
    case Selection::Kind::Light:
        options["type"] = {QStringLiteral("point"), QStringLiteral("spot")};
        break;
    default:
        break;
    }
    form->setOptions(std::move(options));
    form->onEdited = [this, &session, selection](const Json& edited) {
        showProblem(session.setObjectJson(selection, edited));
    };
    form->setValue(value, nullptr, std::move(specs));
}

void InspectorPanel::inspectEntity(const std::shared_ptr<World>& world, const Selection& selection) {
    session_ = nullptr;
    name_->setVisible(false);
    if (!world || selection.kind != Selection::Kind::Entity ||
        !world->alive(selection.entity, selection.serial)) {
        if (!shown_.empty() || forms_.size() > 0) {
            clear(tr("Select an entity in the Hierarchy"));
        } else {
            title_->setText(tr("Select an entity in the Hierarchy"));
        }
        return;
    }
    const auto components = componentsOf(*world, selection.entity);
    const std::string key = "play:" + world->name() + ":" + std::to_string(selection.serial);
    if (key != shown_) {
        showProblem({});
        shown_ = key;
    }
    title_->setText(QString::fromStdString(selectionLabel(*world, selection)));
    resetForms(static_cast<int>(components.size()));
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto* info = components[i];
        headings_[i]->setVisible(true);
        headings_[i]->setText(QString::fromLatin1(info->name));
        auto* form = forms_[i];
        form->setOptions({});
        form->setReadOnly(list(info->readOnly));
        std::weak_ptr<World> weak = world;
        form->onEdited = [this, info, weak, selection](const Json& edited) {
            const auto live = weak.lock();
            if (live && live->alive(selection.entity, selection.serial)) {
                showProblem(info->write(*live, selection.entity, edited));
            }
        };
        form->setValue(info->read(*world, selection.entity));
    }
}
} // namespace fe::editor
