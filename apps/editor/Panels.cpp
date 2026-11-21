#include "apps/editor/Panels.h"
#include "apps/editor/GameHooks.h"
#include "engine/core/Paths.h"
#include "engine/editor/EditorSession.h"
#include "engine/editor/PropLinks.h"
#include "engine/world/World.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace fe::editor {
namespace {
QTreeWidget* table(QWidget* parent, const QStringList& columns) {
    auto* tree = new QTreeWidget(parent);
    tree->setHeaderLabels(columns);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(true);
    return tree;
}

std::string str(const Json& object, const char* key) {
    const auto found = object.is_object() ? object.find(key) : object.end();
    if (found == object.end()) {
        return {};
    }
    return found->is_string() ? found->get<std::string>() : found->dump();
}

// "game/field.lua:123:" somewhere in an error message or a tool's output.
bool fileAndLine(const QString& text, QString& file, int& line) {
    static const QRegularExpression pattern(QStringLiteral("([\\w./\\-]+\\.(?:lua|json|sc)):(\\d+)"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    file = match.captured(1);
    line = match.captured(2).toInt();
    return true;
}
} // namespace

ScriptsPanel::ScriptsPanel(EditorSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    target_ = new QLabel(this);
    target_->setContentsMargins(6, 2, 6, 0);
    column->addWidget(target_);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("scriptsTabs"));
    tabs_->setDocumentMode(true);
    column->addWidget(tabs_, 1);

    modules_ = table(tabs_, {tr("Module"), tr("File")});
    modules_->setObjectName(QStringLiteral("scriptsModules"));
    tabs_->addTab(modules_, tr("Loaded Modules"));
    QObject::connect(modules_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
        if (onOpenFile) {
            onOpenFile(std::filesystem::path(row->data(0, Qt::UserRole).toString().toStdString()), 0);
        }
    });

    wiring_ = table(tabs_, {tr("Object"), tr("Type"), tr("Field"), tr("Linked to")});
    wiring_->setObjectName(QStringLiteral("scriptsWiring"));
    tabs_->addTab(wiring_, tr("Scene Wiring"));
    QObject::connect(wiring_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* row) {
        if (onSelect) {
            const auto id = row->data(0, Qt::UserRole).toString().toStdString();
            onSelect(row->data(1, Qt::UserRole).toBool() ? Selection::zone(id) : Selection::placement(id));
        }
    });
    QObject::connect(wiring_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
        const auto file = row->data(3, Qt::UserRole).toString();
        if (!file.isEmpty() && onOpenFile) {
            onOpenFile(std::filesystem::path(file.toStdString()), 0);
        }
    });

    tasks_ = table(tabs_, {tr("Task"), tr("Status"), tr("Age"), QString()});
    tasks_->setObjectName(QStringLiteral("scriptsTasks"));
    tabs_->addTab(tasks_, tr("Running Tasks"));

    events_ = table(tabs_, {tr("Time"), tr("Event"), tr("Who"), tr("What")});
    tabs_->addTab(events_, tr("Recent Events"));

    errors_ = table(tabs_, {tr("Time"), tr("Error")});
    errors_->setObjectName(QStringLiteral("scriptsErrors"));
    tabs_->addTab(errors_, tr("Errors"));
    QObject::connect(errors_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
        QString file;
        int line = 0;
        if (onOpenFile && fileAndLine(row->toolTip(1), file, line)) {
            onOpenFile(session_.gameRoot() / file.toStdString(), line);
        }
    });

    auto* console = new QWidget(tabs_);
    auto* consoleColumn = new QVBoxLayout(console);
    consoleColumn->setContentsMargins(0, 0, 0, 0);
    output_ = new QPlainTextEdit(console);
    output_->setObjectName(QStringLiteral("luaOutput"));
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(2000);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    output_->setFont(mono);
    input_ = new QLineEdit(console);
    input_->setObjectName(QStringLiteral("luaInput"));
    input_->setFont(mono);
    input_->setPlaceholderText(
        tr("Lua to evaluate in the game, e.g. fe.input.device_type()   (Enter runs it)"));
    consoleColumn->addWidget(output_, 1);
    consoleColumn->addWidget(input_);
    tabs_->addTab(console, tr("Lua Console"));
    QObject::connect(input_, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    refreshWiring();
}

void ScriptsPanel::setEngine(Engine* engine, bool playing, const std::string& world) {
    if (engine != engine_) {
        errorsShown_ = 0;
        modulesShown_ = static_cast<std::size_t>(-1);
        errors_->clear();
    }
    engine_ = engine;
    playing_ = playing;
    world_ = world;
    target_->setText(
        playing ? tr("Watching the running game")
                : tr("Watching the editor's copy of the game's scripts (press Play to watch them run)"));
}

void ScriptsPanel::runConsole() {
    const QString code = input_->text().trimmed();
    if (code.isEmpty() || !engine_) {
        return;
    }
    history_ << code;
    input_->clear();
    output_->appendPlainText(QStringLiteral("> ") + code);
    const auto result = engine_->lua().evaluate(code.toStdString());
    output_->appendPlainText(
        QString::fromStdString(result.output.empty() ? std::string("(no value)") : result.output));
}

void ScriptsPanel::refresh() {
    if (!engine_ || !isVisible()) {
        return;
    }
    auto& lua = engine_->lua();
    // Modules: rebuilt only when their number changes.
    const auto loaded = lua.loadedModules();
    if (loaded.size() != modulesShown_) {
        modulesShown_ = loaded.size();
        modules_->clear();
        for (const auto& module : loaded) {
            std::error_code error;
            const auto relative = std::filesystem::relative(module.file, session_.gameRoot(), error);
            auto* row = new QTreeWidgetItem(
                modules_, {QString::fromStdString(module.name),
                           QString::fromStdString(error ? module.file.string() : relative.generic_string())});
            row->setData(0, Qt::UserRole, QString::fromStdString(module.file.string()));
        }
        modules_->resizeColumnToContents(0);
        tabs_->setTabText(0, tr("Loaded Modules (%1)").arg(loaded.size()));
    }
    const auto& errors = lua.errors();
    if (errors.size() != errorsShown_) {
        errorsShown_ = errors.size();
        errors_->clear();
        for (const auto& error : errors) {
            const QString message = QString::fromStdString(error.message);
            auto* row = new QTreeWidgetItem(
                errors_, {QString::number(error.time, 'f', 2), message.section(QLatin1Char('\n'), 0, 0)});
            row->setToolTip(1, message);
        }
        tabs_->setTabText(tabs_->indexOf(errors_), tr("Errors (%1)").arg(errors.size()));
    }
    if (!playing_) {
        if (tasks_->topLevelItemCount() > 0) {
            tasks_->clear();
            events_->clear();
            tabs_->setTabText(tabs_->indexOf(tasks_), tr("Running Tasks"));
        }
        return;
    }
    if (tabs_->currentWidget() == tasks_ || tasks_->topLevelItemCount() == 0) {
        const auto tasks = hookJson(*engine_, "tasks").value_or(Json::array());
        tasks_->clear();
        for (const auto& task : tasks) {
            auto* row =
                new QTreeWidgetItem(tasks_, {QString::fromStdString(str(task, "name")),
                                             QString::fromStdString(str(task, "status")),
                                             QStringLiteral("%1 s").arg(task.value("age", 0.0), 0, 'f', 1)});
            auto* cancel = new QPushButton(tr("Cancel"), tasks_);
            const int index = task.value("index", 0);
            QObject::connect(cancel, &QPushButton::clicked, this, [this, index] {
                if (engine_) {
                    hookCall(*engine_, "cancel_task", index);
                }
            });
            tasks_->setItemWidget(row, 3, cancel);
        }
        tabs_->setTabText(tabs_->indexOf(tasks_), tr("Running Tasks (%1)").arg(tasks.size()));
    }
    if (tabs_->currentWidget() == events_) {
        const auto events = hookJson(*engine_, "recent_events", world_).value_or(Json::array());
        events_->clear();
        for (auto event = events.rbegin(); event != events.rend(); ++event) {
            const auto a = str(*event, "a"), b = str(*event, "b");
            new QTreeWidgetItem(events_, {QString::number(event->value("time", 0.0), 'f', 2),
                                          QString::fromStdString(str(*event, "type")),
                                          QString::fromStdString(a + (b.empty() ? "" : " / " + b)),
                                          QString::fromStdString(str(*event, "name"))});
        }
    }
}

void ScriptsPanel::refreshWiring() {
    wiring_->clear();
    const auto* map = session_.map();
    if (!map) {
        return;
    }
    int count = 0;
    const auto add = [&](const std::string& id, const std::string& type, const Json& props, const char* group,
                         bool zone) {
        const auto* spec = typeFields(session_.schemas(), group, type);
        for (const auto& link : propLinks(spec, props)) {
            const bool script = link.kind == PropLink::Kind::Script;
            auto* row = new QTreeWidgetItem(
                wiring_,
                {QString::fromStdString(id), QString::fromStdString(type), QString::fromStdString(link.label),
                 (script ? tr("script  ") : tr("scene  ")) + QString::fromStdString(link.value)});
            row->setData(0, Qt::UserRole, QString::fromStdString(id));
            row->setData(1, Qt::UserRole, zone);
            if (script) {
                // A module name is a path under the game folder.
                std::string path = link.value;
                std::replace(path.begin(), path.end(), '.', '/');
                for (const auto& candidate :
                     {session_.gameRoot() / (path + ".lua"), session_.gameRoot() / path / "init.lua"}) {
                    if (std::filesystem::is_regular_file(candidate)) {
                        row->setData(3, Qt::UserRole, QString::fromStdString(candidate.string()));
                        break;
                    }
                }
                if (row->data(3, Qt::UserRole).toString().isEmpty()) {
                    row->setForeground(3, QColor(220, 90, 60));
                    row->setToolTip(3, tr("No such module in the project"));
                }
            }
            ++count;
        }
    };
    for (const auto& placement : map->placements) {
        add(placement.id, placement.type, placement.props, "entities", false);
    }
    for (const auto& zone : map->zones) {
        add(zone.id, zone.type, zone.props, "zones", true);
    }
    tabs_->setTabText(tabs_->indexOf(wiring_), tr("Scene Wiring (%1)").arg(count));
    for (int i = 0; i < 3; ++i) {
        wiring_->resizeColumnToContents(i);
    }
}

DataPanel::DataPanel(EditorSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    tables_ = new QComboBox(this);
    tables_->setObjectName(QStringLiteral("dataTables"));
    top->addWidget(tables_, 1);
    auto* add = new QPushButton(tr("Add"), this);
    auto* duplicate = new QPushButton(tr("Duplicate"), this);
    auto* remove = new QPushButton(tr("Delete"), this);
    save_ = new QPushButton(tr("Save"), this);
    auto* revert = new QPushButton(tr("Revert"), this);
    for (auto* button : {add, duplicate, remove, save_, revert}) {
        top->addWidget(button);
    }
    column->addLayout(top);
    auto* split = new QSplitter(Qt::Horizontal, this);
    records_ = new QListWidget(split);
    records_->setObjectName(QStringLiteral("dataRecords"));
    auto* scroll = new QScrollArea(split);
    scroll->setWidgetResizable(true);
    form_ = new JsonForm(scroll);
    scroll->setWidget(form_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    column->addWidget(split, 1);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    column->addWidget(status_);

    QObject::connect(tables_, &QComboBox::activated, this, [this](int index) {
        if (dirty_ &&
            QMessageBox::question(this, tr("Data"), tr("Discard the unsaved changes to this table?")) !=
                QMessageBox::Yes) {
            return;
        }
        loadTable(index);
    });
    QObject::connect(records_, &QListWidget::currentRowChanged, this, [this](int row) { showRecord(row); });
    form_->onEdited = [this](const Json& edited) {
        const int row = records_->currentRow();
        if (row >= 0 && row < static_cast<int>(document_.size())) {
            document_[static_cast<std::size_t>(row)] = edited;
            showRecord(row);
            records_->item(row)->setText(QString::fromStdString(str(edited, "id")));
            setDirty(true);
        }
    };
    QObject::connect(add, &QPushButton::clicked, this, [this] {
        if (!document_.is_array()) {
            return;
        }
        // A new record has the fields of the first one, emptied.
        Json record = Json::object();
        if (!document_.empty() && document_.front().is_object()) {
            for (auto field = document_.front().begin(); field != document_.front().end(); ++field) {
                record[field.key()] = field->is_string()    ? Json("")
                                      : field->is_number()  ? Json(0)
                                      : field->is_boolean() ? Json(false)
                                      : field->is_array()   ? Json::array()
                                                            : Json::object();
            }
        }
        record["id"] = "new_record";
        document_.push_back(record);
        fillRecords();
        records_->setCurrentRow(static_cast<int>(document_.size()) - 1);
        setDirty(true);
    });
    QObject::connect(duplicate, &QPushButton::clicked, this, [this] {
        const int row = records_->currentRow();
        if (row < 0) {
            return;
        }
        Json copy = document_[static_cast<std::size_t>(row)];
        copy["id"] = str(copy, "id") + "_copy";
        document_.insert(document_.begin() + row + 1, copy);
        fillRecords();
        records_->setCurrentRow(row + 1);
        setDirty(true);
    });
    QObject::connect(remove, &QPushButton::clicked, this, [this] {
        const int row = records_->currentRow();
        if (row < 0) {
            return;
        }
        document_.erase(document_.begin() + row);
        fillRecords();
        records_->setCurrentRow(std::min(row, static_cast<int>(document_.size()) - 1));
        setDirty(true);
    });
    QObject::connect(save_, &QPushButton::clicked, this, [this] { save(); });
    QObject::connect(revert, &QPushButton::clicked, this, [this] { loadTable(tables_->currentIndex()); });
    refreshTables();
}

void DataPanel::setChoices(const FormChoices& choices) {
    form_->setChoices(choices);
}

void DataPanel::setDirty(bool dirty) {
    dirty_ = dirty;
    save_->setEnabled(dirty);
}

void DataPanel::refreshTables() {
    const QString current = tables_->currentText();
    tables_->clear();
    const auto tables = editorui::jsonArray(session_.schemas(), "tables");
    for (const auto& described : tables) {
        tables_->addItem(QString::fromStdString(str(described, "name")),
                         QString::fromStdString(described.dump()));
    }
    if (tables.empty()) {
        status_->setText(
            tr("This game describes no data tables (the `tables` of its editor.describe hook)."));
        document_ = Json();
        fillRecords();
        setDirty(false);
        return;
    }
    const int index = std::max(0, tables_->findText(current));
    tables_->setCurrentIndex(index);
    if (!dirty_) {
        loadTable(index);
    }
}

void DataPanel::loadTable(int index) {
    table_ = Json::parse(tables_->itemData(index).toString().toStdString(), nullptr, false);
    document_ = Json::array();
    status_->clear();
    try {
        document_ = readJson(resolveInside(session_.gameRoot(), str(table_, "file")));
        if (!document_.is_array()) {
            status_->setText(tr("%1 is not a list of records; edit it in the code editor.")
                                 .arg(QString::fromStdString(str(table_, "file"))));
            document_ = Json::array();
        }
    } catch (const std::exception& error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
    fillRecords();
    records_->setCurrentRow(document_.empty() ? -1 : 0);
    setDirty(false);
}

void DataPanel::fillRecords() {
    const QSignalBlocker blocker(records_);
    records_->clear();
    if (document_.is_array()) {
        for (const auto& record : document_) {
            records_->addItem(QString::fromStdString(str(record, "id")));
        }
    }
    if (records_->count() == 0) {
        form_->setValue(Json());
    }
}

void DataPanel::showRecord(int row) {
    if (row < 0 || !document_.is_array() || row >= static_cast<int>(document_.size())) {
        form_->setValue(Json());
        return;
    }
    const Json* fields = nullptr;
    const auto schema = str(table_, "schema");
    if (!schema.empty()) {
        fields = session_.fieldsFor(schema, {"schemas"});
    }
    form_->setValue(document_[static_cast<std::size_t>(row)], fields);
}

bool DataPanel::save() {
    if (!dirty_) {
        return true;
    }
    const auto name = str(table_, "name"), file = str(table_, "file");
    auto& engine = session_.engine();
    std::string problem;
    if (const auto validate = gameHook(engine, "validate_table"); validate != sol::lua_nil) {
        const auto result = engine.lua().invoke(validate, name, jsonToLua(engine.lua().lua(), document_));
        if (!result.valid()) {
            const sol::error error = result;
            problem = error.what();
        } else if (result.return_count() > 0 && !result.get<bool>(0)) {
            problem = result.return_count() > 1 ? result.get<std::string>(1) : "Validation failed";
        }
    } else {
        for (std::size_t i = 0; i < document_.size(); ++i) {
            if (!document_[i].is_object() || !document_[i].contains("id") ||
                !document_[i]["id"].is_string() || document_[i]["id"].get<std::string>().empty()) {
                problem = "record " + std::to_string(i + 1) + ": every record needs a string id";
                break;
            }
        }
    }
    if (!problem.empty()) {
        status_->setText(tr("Not saved: %1").arg(QString::fromStdString(problem)));
        return false;
    }
    try {
        const auto path = resolveInside(session_.gameRoot(), file);
        writeJson(path, document_);
        setDirty(false);
        status_->setText(tr("Saved %1").arg(QString::fromStdString(file)));
        if (onSaved) {
            onSaved(path);
        }
        return true;
    } catch (const std::exception& error) {
        status_->setText(tr("Not saved: %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
}

BuildPanel::BuildPanel(std::filesystem::path gameRoot, QWidget* parent)
    : QWidget(parent), root_(std::move(gameRoot)) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(4, 4, 4, 4);
    auto* row = new QHBoxLayout;
    const auto button = [&](const QString& label, const QString& tip, void (BuildPanel::*action)()) {
        auto* made = new QPushButton(label, this);
        made->setToolTip(tip);
        QObject::connect(made, &QPushButton::clicked, this, action);
        row->addWidget(made);
        buttons_.push_back(made);
    };
    button(tr("Validate"), tr("Check every scene, sprite and referenced asset, and the game's own validate"),
           &BuildPanel::validate);
    button(tr("Run Tests"), tr("Run the game's Lua tests (tests/*.lua)"), &BuildPanel::test);
    button(tr("Bake Walkability"), tr("Derive each scene's walkable cells from its geometry"),
           &BuildPanel::bake);
    button(tr("Record Replay..."), tr("Play the game in its own window, recording the input as a replay"),
           &BuildPanel::recordReplay);
    button(tr("Run Replay..."), tr("Run a recorded replay without a window; its asserts decide the outcome"),
           &BuildPanel::runReplay);
    button(tr("Package..."), tr("Build a shippable folder of the game"), &BuildPanel::package);
    row->addStretch(1);
    state_ = new QLabel(this);
    row->addWidget(state_);
    column->addLayout(row);
    output_ = new QPlainTextEdit(this);
    output_->setObjectName(QStringLiteral("buildOutput"));
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(20000);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    output_->setFont(mono);
    output_->viewport()->installEventFilter(this);
    column->addWidget(output_, 1);

    process_.setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(&process_, &QProcess::readyRead, this, [this] {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(QString::fromUtf8(process_.readAll()));
        output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
    });
    const auto done = [this](int code) {
        for (auto* each : buttons_) {
            each->setEnabled(true);
        }
        state_->setText(code == 0 ? tr("%1: passed").arg(title_)
                                  : tr("%1: FAILED (exit code %2)").arg(title_).arg(code));
        output_->appendPlainText(code == 0 ? tr("\n-- %1 finished --").arg(title_)
                                           : tr("\n-- %1 failed with exit code %2 --").arg(title_).arg(code));
        if (onFinished) {
            onFinished(title_, code);
        }
    };
    QObject::connect(&process_, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit ? code : -1);
    });
    QObject::connect(&process_, &QProcess::errorOccurred, this, [this, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            output_->appendPlainText(tr("Could not start %1").arg(process_.program()));
            done(-1);
        }
    });
    // Double-clicking a line that names a file opens it there.
    QObject::connect(output_, &QPlainTextEdit::selectionChanged, this, [this] {
        const auto cursor = output_->textCursor();
        if (!cursor.hasSelection() || !onOpenFile) {
            return;
        }
        QString file;
        int line = 0;
        if (fileAndLine(cursor.block().text(), file, line) && cursor.selectedText().size() > 0 &&
            cursor.block().text().contains(cursor.selectedText()) && file.contains(cursor.selectedText())) {
            const auto path = root_ / file.toStdString();
            if (std::filesystem::is_regular_file(path)) {
                onOpenFile(path, line);
            }
        }
    });
}

QString BuildPanel::headless() const {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fengine_headless"));
}

void BuildPanel::run(const QString& title, const QString& program, const QStringList& arguments) {
    if (running()) {
        return;
    }
    title_ = title;
    for (auto* each : buttons_) {
        each->setEnabled(false);
    }
    state_->setText(tr("%1: running...").arg(title));
    output_->clear();
    output_->appendPlainText(QStringLiteral("$ %1 %2\n").arg(program, arguments.join(QLatin1Char(' '))));
    process_.start(program, arguments);
}

void BuildPanel::validate() {
    run(tr("Validate"), headless(), {QStringLiteral("--check"), QString::fromStdString(root_.string())});
}

void BuildPanel::test() {
    run(tr("Tests"), headless(), {QStringLiteral("--test"), QString::fromStdString(root_.string())});
}

void BuildPanel::bake() {
    run(tr("Bake"), headless(), {QStringLiteral("--bake"), QString::fromStdString(root_.string())});
}

void BuildPanel::recordReplay() {
    std::filesystem::create_directories(root_ / "replays");
    const QString file = QFileDialog::getSaveFileName(
        this, tr("Record Replay"), QString::fromStdString((root_ / "replays" / "session.json").string()),
        tr("Replays (*.json)"));
    if (!file.isEmpty()) {
        recordReplayTo(file);
    }
}

void BuildPanel::recordReplayTo(const QString& file, const QStringList& extra) {
    run(tr("Record"), QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fengine")),
        QStringList{QString::fromStdString(root_.string()), QStringLiteral("--record"), file} + extra);
}

void BuildPanel::runReplay() {
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Run Replay"), QString::fromStdString((root_ / "replays").string()), tr("Replays (*.json)"));
    if (!file.isEmpty()) {
        runReplayFile(file);
    }
}

void BuildPanel::runReplayFile(const QString& file) {
    run(tr("Replay"), headless(), {QString::fromStdString(root_.string()), QStringLiteral("--replay"), file});
}

// Packaging needs a Release build of the engine (no editor, no hot reload):
// the folder is asked for once and remembered.
void BuildPanel::package() {
    QSettings settings;
    QString release = settings.value(QStringLiteral("package/releaseBuild")).toString();
    const auto usable = [](const QString& folder) {
        return !folder.isEmpty() && (QFile::exists(QDir(folder).filePath(QStringLiteral("fengine"))) ||
                                     QFile::exists(QDir(folder).filePath(QStringLiteral("fengine.exe"))));
    };
    if (!usable(release)) {
        const QDir build(QCoreApplication::applicationDirPath());
        for (const char* name : {"../build-release", "../build"}) {
            if (usable(build.filePath(QLatin1String(name)))) {
                release = QDir::cleanPath(build.filePath(QLatin1String(name)));
            }
        }
    }
    release = QFileDialog::getExistingDirectory(
        this,
        tr("Release build of the engine (cmake -DCMAKE_BUILD_TYPE=Release) holding the fengine executable"),
        release);
    if (release.isEmpty()) {
        return;
    }
    if (!usable(release)) {
        QMessageBox::warning(this, tr("Package"), tr("%1 holds no fengine executable.").arg(release));
        return;
    }
    settings.setValue(QStringLiteral("package/releaseBuild"), release);
    const QString output =
        QFileDialog::getExistingDirectory(this, tr("Folder to write the package into"),
                                          settings.value(QStringLiteral("package/output")).toString());
    if (output.isEmpty()) {
        return;
    }
    settings.setValue(QStringLiteral("package/output"), output);
    packageFrom(release, output);
}

void BuildPanel::packageFrom(const QString& releaseBuild, const QString& outputFolder) {
    const QDir from(releaseBuild);
    const QString target = QDir(outputFolder).filePath(QString::fromStdString(root_.filename().string()));
    QStringList arguments{QStringLiteral(FENGINE_SOURCE_DIR "/tools/package_game.py"),
                          QStringLiteral("--runtime"),
                          from.filePath(QStringLiteral("fengine")),
                          QStringLiteral("--assetc"),
                          from.filePath(QStringLiteral("tools/assetc/fengine_assetc")),
                          QStringLiteral("--shaders"),
                          from.filePath(QStringLiteral("shaders")),
                          QStringLiteral("--game"),
                          QString::fromStdString(root_.string()),
                          QStringLiteral("--validator"),
                          from.filePath(QStringLiteral("fengine_headless")),
                          QStringLiteral("--licenses"),
                          from.filePath(QStringLiteral("licenses")),
                          QStringLiteral("--output"),
                          target,
                          QStringLiteral("--zip"),
                          QStringLiteral("--self-check")};
    // A game with shaders of its own needs the shader compiler the engine
    // build made, and the engine's shader headers.
    if (std::filesystem::is_directory(root_ / "shaders")) {
        QDirIterator files(releaseBuild, {QStringLiteral("shaderc"), QStringLiteral("shaderc.exe")},
                           QDir::Files, QDirIterator::Subdirectories);
        if (files.hasNext()) {
            arguments << QStringLiteral("--shaderc") << files.next() << QStringLiteral("--shader-include")
                      << from.filePath(QStringLiteral("shader-include"));
        }
    }
    run(tr("Package"), QStringLiteral("python3"), arguments);
}
} // namespace fe::editor
