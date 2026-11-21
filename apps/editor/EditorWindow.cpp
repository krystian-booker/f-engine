#include "apps/editor/EditorWindow.h"
#include "apps/editor/HierarchyPanel.h"
#include "apps/editor/InspectorPanel.h"
#include "apps/editor/Panels.h"
#include "apps/editor/PlaySession.h"
#include "apps/editor/ProjectPanel.h"
#include "apps/editor/SceneView.h"
#include "apps/editor/Thumbnails.h"
#include "apps/editor/ViewportWindow.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/editor/AssetCatalog.h"
#include "engine/editor/EditorSession.h"
#include "engine/editor/MapFiles.h"
#include "engine/editor/SceneEdits.h"
#include "engine/render/FrameRenderer.h"
#include "engine/render/Renderer.h"
#include "engine/runtime/Engine.h"
#include "engine/script/Binder.h"
#include "engine/world/ComponentInfo.h"
#include "engine/world/World.h"
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <imgui.h>

namespace fe::editor {
namespace {
constexpr int FrameIntervalMs = 16;
constexpr int LayoutVersion = 3;

QString qstr(const std::string& text) {
    return QString::fromStdString(text);
}

AssetKind kindNamed(const std::string& name) {
    static const std::pair<const char*, AssetKind> kinds[] = {
        {"sprite", AssetKind::Sprite}, {"image", AssetKind::Image}, {"model", AssetKind::Model},
        {"audio", AssetKind::Audio},   {"lut", AssetKind::Lut},     {"shader", AssetKind::Shader},
    };
    for (const auto& [text, kind] : kinds) {
        if (name == text) {
            return kind;
        }
    }
    return AssetKind::Other;
}

// The engine's Lua API as the code editor offers it: "fe.world.load" for
// module functions, the bare name for the methods and fields of types.
ApiDocs apiDocs(const BindingDocs& docs) {
    ApiDocs api;
    for (const auto& entry : docs.entries()) {
        const bool module = entry.owner.rfind("fe", 0) == 0;
        const QString name = module ? qstr(entry.owner + "." + entry.name) : qstr(entry.name);
        const QString text =
            qstr(entry.signature.empty() ? entry.owner + "." + entry.name : entry.signature) +
            (entry.doc.empty() ? QString() : QStringLiteral("\n\n") + qstr(entry.doc));
        api.names << name;
        api.docs[name] = text;
        api.docs[qstr(entry.owner + "." + entry.name)] = text;
        if (!module) {
            api.docs.emplace(qstr(entry.name), text);
        }
    }
    api.names.removeDuplicates();
    return api;
}
} // namespace

// A new game folder with the least a game needs: game.json, a main.lua that
// shows the first scene, and that scene. Returns the folder, empty when the
// user cancelled or it could not be made.
std::filesystem::path createProject(QWidget* parent) {
    const QString where =
        QFileDialog::getExistingDirectory(parent, QObject::tr("Folder to create the project in"));
    if (where.isEmpty()) {
        return {};
    }
    bool ok = false;
    const QString title = QInputDialog::getText(parent, QObject::tr("New Project"), QObject::tr("Game title"),
                                                QLineEdit::Normal, QObject::tr("My Game"), &ok)
                              .trimmed();
    if (!ok || title.isEmpty()) {
        return {};
    }
    std::string id;
    for (const QChar c : title.toLower()) {
        id += c.isLetterOrNumber() && c.unicode() < 128 ? static_cast<char>(c.unicode()) : '-';
    }
    const auto folder = std::filesystem::path(where.toStdString()) / id;
    try {
        if (std::filesystem::exists(folder)) {
            throw std::runtime_error(folder.string() + " exists already");
        }
        std::filesystem::create_directories(folder);
        writeJson(folder / "game.json", Json{{"title", title.toStdString()}, {"id", id}, {"entry", "main"}});
        writeMap(folder, makeBlankMap("start", "Start", 24, 24));
        writeTextAtomic(
            folder / "main.lua",
            "-- " + title.toStdString() +
                "\nlocal world\n\nreturn {\n  init = function()\n    world = fe.world.load(\"field\", "
                "\"start\")\n  end,\n  update = function() end,\n  draw = function()\n    "
                "world:draw()\n  end,\n}\n");
    } catch (const std::exception& error) {
        QMessageBox::warning(parent, QObject::tr("New Project"), QString::fromUtf8(error.what()));
        return {};
    }
    return folder;
}

EditorWindow::EditorWindow(EditorOptions options)
    : options_(std::move(options)), config_(GameConfig::load(options_.game)) {
    // Log records can come from worker threads; the console lives on this one.
    setLogSink([window = QPointer<EditorWindow>(this)](LogLevel level, std::string_view message) {
        QMetaObject::invokeMethod(
            qApp,
            [window, level,
             line = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()))] {
                if (window && !window->closing_) {
                    window->appendLog(static_cast<int>(level), line);
                }
            },
            Qt::QueuedConnection);
    });

    scene_ = new ViewportWindow;
    game_ = new ViewportWindow;
    renderer_ = std::make_unique<Renderer>(surface_, options_.shaders);
    // The timer paces frames. Waiting for vertical blank would stall the whole
    // window whenever one view's surface is hidden and never presented.
    renderer_->setVsync(false);

    EngineOptions engine;
    engine.gameRoot = options_.game;
    engine.builtinRoot = options_.builtin;
    engine.userData = options_.userData / "edit";
    engine.development = true;
    engine.backgroundSaves = false;
    std::filesystem::create_directories(engine.userData);
    engine_ = std::make_unique<Engine>(std::move(engine), config_);
    frame_ = std::make_unique<FrameRenderer>(*renderer_, engine_->assets(), config_.pixelsPerUnit);
    renderer_->attach(frame_.get());
    session_ = std::make_unique<EditorSession>(*engine_);
    catalog_ = std::make_unique<AssetCatalog>(options_.game, options_.builtin);
    sceneView_ = std::make_unique<SceneView>(*session_, *scene_, config_.pixelsPerUnit);
    renderer_->setImGuiPlatformFrame([this] { sceneView_->platformFrame(FrameIntervalMs / 1000.0f); });
    watcher_ = std::make_unique<FileWatcher>(options_.game, 512);
    watcher_->poll(); // the first pass only takes stock

    thumbnails_ = std::make_unique<Thumbnails>(*renderer_, *engine_, options_.userData / "thumbnails",
                                               config_.pixelsPerUnit);
    buildPanels();
    project_->setThumbnails(thumbnails_.get());
    buildMenus();
    wireSession();

    session_->refreshSchemas();
    catalog_->setSchemas(session_->schemas());
    catalog_->scan();
    refreshChoices();
    code_->setApi(apiDocs(engine_->docs()));

    QSettings settings;
    const auto scenes = session_->scenes();
    std::string first = options_.map;
    if (first.empty()) {
        first =
            settings.value(QStringLiteral("project/%1/scene").arg(qstr(config_.id))).toString().toStdString();
    }
    if (first.empty() || std::find(scenes.begin(), scenes.end(), first) == scenes.end()) {
        first = scenes.empty() ? std::string() : scenes.front();
    }
    if (!first.empty()) {
        openScene(first);
    } else {
        statusBar()->showMessage(tr("This project has no scenes yet: File > New Scene"));
        refreshTitle();
    }
    if (options_.frames == 0 && settings.value(QStringLiteral("layoutVersion")).toInt() == LayoutVersion) {
        restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
        restoreState(settings.value(QStringLiteral("layout")).toByteArray());
    }

    clock_.start();
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { renderFrame(); });
    timer_.start(FrameIntervalMs);
    QObject::connect(&slow_, &QTimer::timeout, this, [this] { slowTick(); });
    slow_.start(250);
}

EditorWindow::~EditorWindow() {
    closing_ = true;
    timer_.stop();
    slow_.stop();
    setLogSink({});
    // The widgets outlive the members below; nothing of theirs may reach the
    // renderer or an engine after it is gone.
    play_.reset();
    project_->setThumbnails(nullptr);
    thumbnails_.reset();
    scene_->release();
    game_->release();
    sceneView_.reset();
    session_.reset();
    frame_.reset();
    engine_.reset();
    renderer_.reset();
}

void EditorWindow::buildPanels() {
    resize(1600, 940);
    setDockNestingEnabled(true);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    // Docks only: the window has no central widget, so the Scene and Game
    // views dock, tab and float like every other panel.
    auto* centre = new QWidget(this);
    centre->setMaximumSize(0, 0);
    setCentralWidget(centre);

    const auto dock = [this](const QString& title, const char* name, QWidget* widget,
                             Qt::DockWidgetArea area) {
        auto* made = new QDockWidget(title, this);
        made->setObjectName(QLatin1String(name));
        made->setWidget(widget);
        addDockWidget(area, made);
        return made;
    };

    sceneDock_ =
        dock(tr("Scene"), "scene", QWidget::createWindowContainer(scene_, this), Qt::TopDockWidgetArea);
    gameDock_ = dock(tr("Game"), "game", QWidget::createWindowContainer(game_, this), Qt::TopDockWidgetArea);
    code_ = new CodePanel(this);
    codeDock_ = dock(tr("Code"), "code", code_, Qt::TopDockWidgetArea);

    hierarchy_ = new HierarchyPanel(this);
    auto* hierarchyDock = dock(tr("Hierarchy"), "hierarchy", hierarchy_, Qt::TopDockWidgetArea);
    inspector_ = new InspectorPanel(this);
    auto* inspectorDock = dock(tr("Inspector"), "inspector", inspector_, Qt::TopDockWidgetArea);

    project_ = new ProjectPanel(options_.game, *catalog_, this);
    auto* projectDock = dock(tr("Project"), "project", project_, Qt::BottomDockWidgetArea);

    auto* consolePanel = new QWidget(this);
    auto* consoleColumn = new QVBoxLayout(consolePanel);
    consoleColumn->setContentsMargins(0, 0, 0, 0);
    consoleColumn->setSpacing(0);
    auto* consoleBar = new QHBoxLayout;
    consoleBar->setContentsMargins(4, 2, 4, 2);
    consoleLevel_ = new QComboBox(consolePanel);
    consoleLevel_->addItems(
        {tr("Everything"), tr("Info and above"), tr("Warnings and errors"), tr("Errors only")});
    consoleLevel_->setCurrentIndex(1);
    auto* clear = new QPushButton(tr("Clear"), consolePanel);
    consoleBar->addWidget(consoleLevel_);
    consoleBar->addWidget(clear);
    consoleBar->addStretch(1);
    consoleColumn->addLayout(consoleBar);
    console_ = new QPlainTextEdit(consolePanel);
    console_->setObjectName(QStringLiteral("consoleLog"));
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(5000);
    consoleColumn->addWidget(console_, 1);
    QObject::connect(clear, &QPushButton::clicked, console_, &QPlainTextEdit::clear);
    consoleDock_ = dock(tr("Console"), "console", consolePanel, Qt::BottomDockWidgetArea);

    scripts_ = new ScriptsPanel(*session_, this);
    scriptsDock_ = dock(tr("Scripts"), "scripts", scripts_, Qt::BottomDockWidgetArea);
    data_ = new DataPanel(*session_, this);
    auto* dataDock = dock(tr("Data"), "data", data_, Qt::BottomDockWidgetArea);
    build_ = new BuildPanel(options_.game, this);
    buildDock_ = dock(tr("Build"), "build", build_, Qt::BottomDockWidgetArea);

    // Unity's default: Hierarchy | Scene, Game, Code | Inspector over the
    // Project, Console and the rest in tabs.
    splitDockWidget(hierarchyDock, sceneDock_, Qt::Horizontal);
    splitDockWidget(sceneDock_, inspectorDock, Qt::Horizontal);
    tabifyDockWidget(sceneDock_, gameDock_);
    tabifyDockWidget(gameDock_, codeDock_);
    tabifyDockWidget(projectDock, consoleDock_);
    tabifyDockWidget(consoleDock_, scriptsDock_);
    tabifyDockWidget(scriptsDock_, dataDock);
    tabifyDockWidget(dataDock, buildDock_);
    sceneDock_->raise();
    projectDock->raise();
    resizeDocks({hierarchyDock, sceneDock_, inspectorDock}, {280, 940, 380}, Qt::Horizontal);
    resizeDocks({sceneDock_, projectDock}, {640, 260}, Qt::Vertical);

    stats_ = new QLabel(this);
    statusBar()->addPermanentWidget(stats_);

    // Panels to the session and to each other.
    hierarchy_->onSelected = [this](const std::vector<Selection>& selected) {
        if (playing()) {
            playSelection_ = selected.empty() ? Selection{} : selected.back();
            inspector_->inspectEntity(play_->shownWorld(), playSelection_);
        } else {
            session_->selectAll(selected);
        }
    };
    hierarchy_->onActivated = [this](const Selection&) {
        if (!playing()) {
            sceneView_->frameSelection();
            sceneDock_->raise();
        }
    };
    hierarchy_->onWorldChosen = [this](const std::string& world) {
        if (playing()) {
            play_->showWorld(world);
            showPlayHierarchy(true);
        }
    };
    hierarchy_->onContextMenu = [this](QMenu& menu, const Selection& under) {
        if (playing()) {
            return;
        }
        if (!under.empty() && !session_->isSelected(under)) {
            session_->select(under);
        }
        menu.addMenu(createMenu_);
        if (!session_->selection().empty()) {
            menu.addSeparator();
            menu.addAction(duplicate_);
            menu.addAction(delete_);
            menu.addAction(tr("Frame in Scene"), this, [this] { sceneView_->frameSelection(); });
        }
    };
    sceneView_->onKey = [this](QKeyEvent* event) {
        dispatchShortcut(event);
    };
    sceneView_->onAssetsDropped = [this](const QStringList& paths, glm::vec3 point, GridPos cell) {
        for (const auto& path : paths) {
            placeAsset(std::filesystem::path(path.toStdString()), &point, &cell);
        }
    };
    project_->onOpen = [this](const std::filesystem::path& file) {
        openFile(file);
    };
    project_->onPlaceInScene = [this](const std::filesystem::path& file) {
        placeAsset(file, nullptr, nullptr);
    };
    project_->onNewScene = [this] {
        newScene();
    };
    code_->onSaved = [this](const std::filesystem::path& file) {
        statusBar()->showMessage(tr("Saved %1").arg(qstr(file.filename().string())), 4000);
    };
    scripts_->onOpenFile = [this](const std::filesystem::path& file, int line) {
        openFile(file, line);
    };
    scripts_->onSelect = [this](const Selection& selection) {
        if (!playing() && session_->world() && selectionValid(*session_->world(), selection)) {
            session_->select(selection);
        }
    };
    build_->onOpenFile = [this](const std::filesystem::path& file, int line) {
        openFile(file, line);
    };
    scripts_->setEngine(engine_.get(), false, {});
}

void EditorWindow::buildMenus() {
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New &Project..."), this, [this] {
        if (const auto made = createProject(this); !made.empty()) {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), {qstr(made.string())});
        }
    });
    file->addAction(tr("Open Pro&ject..."), this, [this] {
        const QString chosen =
            QFileDialog::getExistingDirectory(this, tr("Open a game folder (it holds game.json)"));
        if (!chosen.isEmpty()) {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), {chosen});
        }
    });
    file->addSeparator();
    file->addAction(tr("&New Scene..."), QKeySequence::New, this, [this] { newScene(); });
    scenesMenu_ = file->addMenu(tr("&Open Scene"));
    save_ = file->addAction(tr("&Save"), QKeySequence::Save, this, [this] {
        // Ctrl+S saves what has the focus: the code editor's file, else the
        // data table, else the scene.
        if (code_->isAncestorOf(QApplication::focusWidget())) {
            code_->saveCurrent();
        } else if (data_->isAncestorOf(QApplication::focusWidget())) {
            data_->save();
        } else {
            session_->save();
        }
    });
    file->addAction(tr("Save Scene &As..."), QKeySequence::SaveAs, this, [this] { saveSceneAs(); });
    file->addAction(tr("Save A&ll"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S), this, [this] {
        code_->saveAll();
        data_->save();
        if (session_->unsaved()) {
            session_->save();
        }
    });
    file->addSeparator();
    file->addAction(tr("&Import Scene Geometry..."), this, [this] {
        if (!session_->world()) {
            return;
        }
        const QString chosen = QFileDialog::getOpenFileName(
            this, tr("Geometry for scene '%1' (replaces the current one)").arg(qstr(session_->sceneId())), {},
            tr("glTF (*.glb *.gltf)"));
        if (!chosen.isEmpty()) {
            session_->importGeometry(std::filesystem::path(chosen.toStdString()));
        }
    });
    file->addAction(tr("Import &Markers from Geometry"), this, [this] { session_->importGeometryMarkers(); });
    file->addSeparator();
    file->addAction(tr("&Rename Scene..."), this, [this] { renameScene(); });
    file->addAction(tr("&Delete Scene..."), this, [this] { deleteScene(); });
    file->addSeparator();
    file->addAction(tr("&Project Settings..."), this, [this] { projectSettings(); });
    file->addAction(tr("Show Project in File &Manager"), this,
                    [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(qstr(options_.game.string()))); });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    undo_ = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, [this] { session_->undo(); });
    redo_ = edit->addAction(tr("&Redo"), QKeySequence::Redo, this, [this] { session_->redo(); });
    edit->addSeparator();
    duplicate_ = edit->addAction(tr("&Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), this,
                                 [this] { session_->duplicateSelection(); });
    delete_ =
        edit->addAction(tr("De&lete"), QKeySequence::Delete, this, [this] { session_->deleteSelection(); });
    edit->addAction(tr("Dese&lect"), QKeySequence(Qt::Key_Escape), this, [this] { session_->select({}); });
    edit->addSeparator();
    edit->addAction(tr("&Find in File"), this, [this] {
        codeDock_->raise();
        code_->showFind();
    });
    // These act on the scene; while typing in a field they are that field's.
    for (auto* action : {undo_, redo_, duplicate_, delete_}) {
        action->setShortcutContext(Qt::WindowShortcut);
    }

    auto* object = menuBar()->addMenu(tr("&GameObject"));
    createMenu_ = object->addMenu(tr("&Create"));
    object->addAction(tr("&Bake Scene Walkability"), this, [this] { session_->bake(); });
    object->addAction(tr("&Frame Selected"), QKeySequence(Qt::Key_F), this,
                      [this] { sceneView_->frameSelection(); });
    object->addAction(tr("Frame &Scene"), QKeySequence(Qt::Key_Home), this,
                      [this] { sceneView_->frameScene(); });

    auto* view = menuBar()->addMenu(tr("&View"));
    auto* toolbar = addToolBar(tr("Tools"));
    toolbar->setObjectName(QStringLiteral("tools"));
    toolbar->setMovable(false);
    auto* toolGroup = new QActionGroup(this);
    const auto tool = [&](const QString& label, const QKeySequence& keys, SceneView::Tool which) {
        auto* action = view->addAction(label, keys, this, [this, which] { sceneView_->tool = which; });
        action->setCheckable(true);
        action->setChecked(sceneView_->tool == which);
        toolGroup->addAction(action);
        toolbar->addAction(action);
        tools_.push_back(action);
    };
    tool(tr("Select"), QKeySequence(Qt::Key_Q), SceneView::Tool::Select);
    tool(tr("Move"), QKeySequence(Qt::Key_W), SceneView::Tool::Move);
    tool(tr("Rotate"), QKeySequence(Qt::Key_E), SceneView::Tool::Rotate);
    tool(tr("Scale"), QKeySequence(Qt::Key_R), SceneView::Tool::Scale);
    tool(tr("Paint Walkability"), QKeySequence(Qt::Key_B), SceneView::Tool::Paint);
    // What the Paint tool paints, and in what shape.
    auto* brush = new QComboBox(this);
    brush->setObjectName(QStringLiteral("brushMode"));
    brush->addItems({tr("Walkable"), tr("Blocked"), tr("Set height"), tr("Ramp north"), tr("Ramp east"),
                     tr("Ramp south"), tr("Ramp west"), tr("No ramp"), tr("Add tag"), tr("Remove tag"),
                     tr("Clear hand-painted")});
    brush->setToolTip(tr("What Paint Walkability paints"));
    auto* shape = new QComboBox(this);
    shape->setObjectName(QStringLiteral("brushShape"));
    shape->addItems({tr("Brush"), tr("Rectangle"), tr("Flood fill")});
    shape->setToolTip(tr("Brush: the cells under the cursor. Rectangle: drag from corner to corner. "
                         "Flood fill: every connected cell like the one clicked."));
    auto* radius = new QSpinBox(this);
    radius->setRange(0, 6);
    radius->setPrefix(tr("radius "));
    auto* height = new QDoubleSpinBox(this);
    height->setRange(-50, 100);
    height->setSingleStep(0.25);
    height->setPrefix(tr("height "));
    auto* tag = new QLineEdit(this);
    tag->setPlaceholderText(tr("tag"));
    tag->setMaximumWidth(90);
    tag->setToolTip(tr("The cell tag the Add tag and Remove tag brushes paint (water, mud...)"));
    auto* layer = new QSpinBox(this);
    layer->setRange(0, 15);
    layer->setPrefix(tr("layer "));
    layer->setToolTip(tr("The grid layer new objects go on and painting affects"));
    QObject::connect(brush, &QComboBox::currentIndexChanged, this, [this](int index) {
        auto& painted = sceneView_->brush;
        sceneView_->brushClears = index == 10;
        painted.walkable = index == 0;
        painted.mode = index == 2                 ? GridBrush::Mode::Height
                       : index >= 3 && index <= 7 ? GridBrush::Mode::Ramp
                       : index == 8               ? GridBrush::Mode::AddTag
                       : index == 9               ? GridBrush::Mode::RemoveTag
                                                  : GridBrush::Mode::Walkable;
        static const Ramp ramps[] = {Ramp::North, Ramp::East, Ramp::South, Ramp::West, Ramp::None};
        painted.ramp = index >= 3 && index <= 7 ? ramps[index - 3] : Ramp::None;
    });
    QObject::connect(shape, &QComboBox::currentIndexChanged, this, [this](int index) {
        sceneView_->brushShape = static_cast<SceneView::BrushShape>(index);
    });
    QObject::connect(tag, &QLineEdit::textChanged, this,
                     [this](const QString& text) { sceneView_->brush.tag = text.trimmed().toStdString(); });
    QObject::connect(radius, &QSpinBox::valueChanged, this,
                     [this](int value) { sceneView_->brushRadius = value; });
    QObject::connect(height, &QDoubleSpinBox::valueChanged, this,
                     [this](double value) { sceneView_->brush.height = static_cast<float>(value); });
    QObject::connect(layer, &QSpinBox::valueChanged, this, [this](int value) {
        const auto* map = session_->map();
        session_->layer = map ? std::clamp(value, 0, map->grid.layerCount() - 1) : 0;
    });
    for (QWidget* widget : std::initializer_list<QWidget*>{brush, shape, radius, height, tag, layer}) {
        toolbar->addWidget(widget);
    }
    view->addSeparator();
    toolbar->addSeparator();
    auto* snap = view->addAction(tr("Snap to Grid"), this, [this](bool on) { sceneView_->snap = on; });
    snap->setCheckable(true);
    snap->setChecked(true);
    toolbar->addAction(snap);
    view->addSeparator();
    const auto mark = [&](const QString& label, bool HandleOptions::* flag) {
        auto* action =
            view->addAction(label, this, [this, flag](bool on) { sceneView_->handles.*flag = on; });
        action->setCheckable(true);
        action->setChecked(sceneView_->handles.*flag);
    };
    auto* walkability =
        view->addAction(tr("Show Walkability"), this, [this](bool on) { sceneView_->showWalkability = on; });
    walkability->setCheckable(true);
    auto* follow =
        view->addAction(tr("Scene Follows Game"), this, [this](bool on) { sceneFollowsGame_ = on; });
    follow->setCheckable(true);
    follow->setChecked(true);
    follow->setToolTip(
        tr("While the game plays, the Scene view shows the game's world through the editor's camera"));
    auto* fog = view->addAction(tr("Scene Fog"), this, [this](bool on) { sceneFog_ = on; });
    fog->setCheckable(true);
    fog->setToolTip(tr("Draw the scene's fog in the Scene view (the Game view always does)"));
    mark(tr("Show Zones"), &HandleOptions::zones);
    mark(tr("Show Points"), &HandleOptions::points);
    mark(tr("Show Lights"), &HandleOptions::lights);
    mark(tr("Show Emitters"), &HandleOptions::emitters);
    mark(tr("Show Colliders"), &HandleOptions::colliders);
    mark(tr("Show Baked Collision"), &HandleOptions::bakedBoxes);
    mark(tr("Show Labels"), &HandleOptions::labels);

    // Play controls, centred like Unity's.
    auto* left = new QWidget(this);
    left->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(left);
    auto* run = menuBar()->addMenu(tr("&Play"));
    play_action_ = run->addAction(tr("&Play"), QKeySequence(Qt::CTRL | Qt::Key_P), this, [this] {
        if (playing()) {
            stop();
        } else {
            play();
        }
    });
    pause_ = run->addAction(tr("P&ause"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), this,
                            [this] { togglePause(); });
    pause_->setCheckable(true);
    step_ = run->addAction(tr("&Step One Tick"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_P), this, [this] {
        if (playing()) {
            if (!play_->paused) {
                togglePause();
            }
            play_->step();
        }
    });
    for (auto* action : {play_action_, pause_, step_}) {
        toolbar->addAction(action);
    }
    auto* right = new QWidget(this);
    right->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(right);
    run->addSeparator();
    run->addAction(tr("&Validate Project"), this, [this] {
        buildDock_->raise();
        build_->validate();
    });
    run->addAction(tr("Run &Tests"), this, [this] {
        buildDock_->raise();
        build_->test();
    });
    run->addAction(tr("&Bake Walkability"), this, [this] {
        if (session_->unsaved() && !session_->save()) {
            return;
        }
        buildDock_->raise();
        build_->bake();
    });
    run->addAction(tr("Pac&kage Game..."), this, [this] {
        buildDock_->raise();
        build_->package();
    });

    auto* window = menuBar()->addMenu(tr("&Window"));
    for (auto* dock : findChildren<QDockWidget*>()) {
        window->addAction(dock->toggleViewAction());
    }
    window->addSeparator();
    window->addAction(tr("Reset Layout"), this, [this] {
        QSettings().remove(QStringLiteral("layout"));
        QMessageBox::information(this, tr("Layout"),
                                 tr("The default layout returns the next time the editor starts."));
    });
    refreshScenesMenu();
    refreshCreateMenu();
    refreshActions();
}

void EditorWindow::wireSession() {
    session_->onStatus = [this](const std::string& text) {
        statusBar()->showMessage(qstr(text), 8000);
    };
    session_->onChanged = [this](unsigned changes) {
        using Change = EditorSession::Change;
        if (changes & Change::SceneChanged) {
            refreshScenesMenu();
            sceneView_->frameScene();
            catalog_->scan();
            refreshChoices();
        }
        if (changes & Change::SchemasChanged) {
            catalog_->setSchemas(session_->schemas());
            refreshCreateMenu();
            data_->refreshTables();
        }
        if (!playing()) {
            if (changes & (Change::SceneChanged | Change::StructureChanged)) {
                hierarchy_->showScene(*session_);
            } else if (changes & Change::SelectionChanged) {
                hierarchy_->setSelected(session_->selected());
            }
            inspector_->inspect(*session_);
        }
        if (changes & (Change::SceneChanged | Change::StructureChanged | Change::ValuesChanged |
                       Change::SchemasChanged)) {
            scripts_->refreshWiring();
        }
        refreshTitle();
        refreshActions();
    };
}

void EditorWindow::refreshScenesMenu() {
    scenesMenu_->clear();
    for (const auto& id : session_->scenes()) {
        auto* action = scenesMenu_->addAction(qstr(id), this, [this, id] {
            if (confirmSceneChange()) {
                openScene(id);
            }
        });
        action->setCheckable(true);
        action->setChecked(id == session_->sceneId());
    }
    scenesMenu_->setEnabled(!scenesMenu_->isEmpty());
}

void EditorWindow::refreshCreateMenu() {
    createMenu_->clear();
    auto* placements = createMenu_->addMenu(tr("Placement"));
    for (const auto& type : session_->typeNames("entities")) {
        placements->addAction(qstr(type), this,
                              [this, type] { createObject(Selection::Kind::Placement, type); });
    }
    createMenu_->addAction(tr("Zone"), this, [this] { createObject(Selection::Kind::Zone); });
    createMenu_->addAction(tr("Point..."), this, [this] { createObject(Selection::Kind::Point); });
    createMenu_->addAction(tr("Light"), this, [this] { createObject(Selection::Kind::Light); });
    createMenu_->addAction(tr("Particle Emitter"), this, [this] { createObject(Selection::Kind::Emitter); });
    createMenu_->addAction(tr("Collider"), this, [this] { createObject(Selection::Kind::Collider); });
}

void EditorWindow::refreshTitle() {
    const auto scene = session_->sceneId();
    setWindowTitle(QStringLiteral("%1%2 - %3 - fengine editor")
                       .arg(scene.empty() ? tr("(no scene)") : qstr(scene),
                            session_->unsaved() ? QStringLiteral(" *") : QString(), qstr(config_.title)));
}

void EditorWindow::refreshActions() {
    const bool editing = !playing() && session_->world() != nullptr;
    undo_->setEnabled(editing && session_->canUndo());
    redo_->setEnabled(editing && session_->canRedo());
    duplicate_->setEnabled(editing && !session_->selection().empty());
    delete_->setEnabled(editing && !session_->selection().empty());
    createMenu_->setEnabled(editing);
    play_action_->setText(playing() ? tr("Stop") : tr("Play"));
    pause_->setEnabled(playing());
    step_->setEnabled(playing());
    pause_->setChecked(playing() && play_->paused);
}

void EditorWindow::refreshChoices() {
    choices_.assets = [this](const std::string& kind) {
        QStringList out;
        for (const auto* entry : catalog_->ofKind(kindNamed(kind))) {
            out << qstr(entry->path);
        }
        return out;
    };
    choices_.modules = [this](const std::string& folder) {
        QStringList out;
        for (const auto& module : catalog_->modules(folder)) {
            out << qstr(module);
        }
        return out;
    };
    choices_.scenes = [this] {
        QStringList out;
        for (const auto& id : session_->scenes()) {
            out << qstr(id);
        }
        return out;
    };
    choices_.records = [this](const std::string& table) {
        QStringList out;
        for (const auto& described : editorui::jsonArray(session_->schemas(), "tables")) {
            if (editorui::jsonString(described, "name", {}) != table) {
                continue;
            }
            try {
                const auto records =
                    readJson(resolveInside(options_.game, editorui::jsonString(described, "file", {})));
                for (const auto& record : records) {
                    if (const auto id = editorui::jsonString(record, "id", {}); !id.empty()) {
                        out << qstr(id);
                    }
                }
            } catch (const std::exception&) {
                // An unreadable table offers no choices; the field stays free text.
            }
        }
        return out;
    };
    inspector_->setChoices(choices_);
    data_->setChoices(choices_);
}

void EditorWindow::appendLog(int level, const QString& line) {
    if (level < consoleLevel_->currentIndex()) {
        return;
    }
    static const char* const names[] = {"debug", "info", "warning", "error"};
    static const char* const colors[] = {"#8a8f98", "", "#d9a441", "#e0603a"};
    const int index = std::clamp(level, 0, 3);
    const QString text =
        QStringLiteral("%1  %2").arg(QLatin1String(names[index]), -8).arg(line).toHtmlEscaped();
    console_->appendHtml(QLatin1String(colors[index])[0] == QChar()
                             ? QStringLiteral("<pre style=\"margin:0\">%1</pre>").arg(text)
                             : QStringLiteral("<pre style=\"margin:0;color:%2\">%1</pre>")
                                   .arg(text, QLatin1String(colors[index])));
    if (level >= 3 && !options_.frames) {
        consoleDock_->toggleViewAction()->setText(tr("Console"));
    }
}

// A key pressed in a native view: the window's shortcuts by hand.
void EditorWindow::dispatchShortcut(QKeyEvent* event) {
    const QKeySequence pressed(event->keyCombination());
    for (auto* action : findChildren<QAction*>()) {
        if (action->isEnabled() && !action->shortcut().isEmpty() && action->shortcut() == pressed) {
            action->trigger();
            return;
        }
    }
}

bool EditorWindow::confirmSceneChange() {
    if (!session_->unsaved() || options_.frames > 0) {
        return true;
    }
    const auto answer = QMessageBox::question(
        this, tr("Unsaved Scene"), tr("Save the changes to scene '%1'?").arg(qstr(session_->sceneId())),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) {
        return false;
    }
    return answer == QMessageBox::Discard || session_->save();
}

void EditorWindow::openScene(const std::string& id) {
    if (session_->openScene(id)) {
        QSettings().setValue(QStringLiteral("project/%1/scene").arg(qstr(config_.id)), qstr(id));
    }
}

void EditorWindow::newScene() {
    if (!confirmSceneChange()) {
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Scene"));
    auto* form = new QFormLayout(&dialog);
    auto* id = new QLineEdit(&dialog);
    id->setPlaceholderText(tr("lowercase letters, digits, _ and -"));
    auto* name = new QLineEdit(&dialog);
    auto* width = new QSpinBox(&dialog);
    auto* depth = new QSpinBox(&dialog);
    for (auto* size : {width, depth}) {
        size->setRange(1, 512);
        size->setValue(24);
    }
    form->addRow(tr("Id (folder name)"), id);
    form->addRow(tr("Name"), name);
    form->addRow(tr("Width (cells)"), width);
    form->addRow(tr("Depth (cells)"), depth);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!validMapId(id->text().toStdString())) {
            QMessageBox::warning(&dialog, tr("New Scene"), tr("A scene id is 1 to 64 of a-z, 0-9, _ and -."));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted) {
        session_->createScene(id->text().toStdString(), name->text().toStdString(), width->value(),
                              depth->value());
    }
}

void EditorWindow::saveSceneAs() {
    if (!session_->world()) {
        return;
    }
    bool ok = false;
    const QString id = QInputDialog::getText(this, tr("Save Scene As"), tr("New scene id"), QLineEdit::Normal,
                                             qstr(session_->sceneId() + "_copy"), &ok);
    if (ok && !id.isEmpty()) {
        session_->saveSceneAs(id.toStdString());
    }
}

void EditorWindow::renameScene() {
    if (!session_->world()) {
        return;
    }
    bool ok = false;
    const QString id = QInputDialog::getText(this, tr("Rename Scene"),
                                             tr("New id for '%1'.\nScripts and scenes that name it must be "
                                                "updated by hand; the Project panel shows who uses it.")
                                                 .arg(qstr(session_->sceneId())),
                                             QLineEdit::Normal, qstr(session_->sceneId()), &ok);
    if (ok && !id.isEmpty() && id.toStdString() != session_->sceneId()) {
        session_->renameScene(id.toStdString());
    }
}

void EditorWindow::deleteScene() {
    const auto id = session_->sceneId();
    if (id.empty()) {
        return;
    }
    if (QMessageBox::warning(
            this, tr("Delete Scene"),
            tr("Delete scene '%1' and everything in maps/%1? This cannot be undone.").arg(qstr(id)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
        session_->deleteScene(id);
        if (const auto scenes = session_->scenes(); !scenes.empty()) {
            openScene(scenes.front());
        }
    }
}

void EditorWindow::projectSettings() {
    const auto file = options_.game / "game.json";
    Json settings;
    try {
        settings = readJson(file);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Project Settings"), QString::fromUtf8(error.what()));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Project Settings (game.json)"));
    dialog.resize(520, 560);
    auto* column = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto* form = new JsonForm(scroll);
    form->setOptions(
        {{"canvas_fit", {QStringLiteral("letterbox"), QStringLiteral("stretch"), QStringLiteral("integer")}},
         {"canvas_scale", {QStringLiteral("pixel"), QStringLiteral("smooth")}}});
    form->setValue(settings);
    form->onEdited = [&](const Json& edited) {
        settings = edited;
        form->setValue(settings);
    };
    scroll->setWidget(form);
    column->addWidget(scroll, 1);
    column->addWidget(new QLabel(tr("Changes apply the next time the game starts (Play)."), &dialog));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    column->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    try {
        writeJson(file, settings);
        config_ = GameConfig::load(options_.game);
        refreshTitle();
        statusBar()->showMessage(tr("Saved game.json"), 5000);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Project Settings"), QString::fromUtf8(error.what()));
    }
}

void EditorWindow::createObject(Selection::Kind kind, const std::string& type) {
    if (playing() || !session_->world()) {
        return;
    }
    const auto at = sceneView_->focus();
    if (kind == Selection::Kind::Placement) {
        session_->addPlacement(type, at);
    } else if (kind == Selection::Kind::Point) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Point"), tr("Point name"), QLineEdit::Normal,
                                                   QStringLiteral("point"), &ok);
        if (ok && !name.isEmpty()) {
            session_->add(kind, at, name.toStdString());
        }
    } else {
        session_->add(kind, at);
    }
    sceneDock_->raise();
}

// A model or a sprite becomes a placement of the game's default type for it,
// at the drop point or, from the menu, where the camera looks.
void EditorWindow::placeAsset(const std::filesystem::path& file, const glm::vec3* point,
                              const GridPos* cell) {
    const auto& world = session_->world();
    if (playing() || !world) {
        return;
    }
    std::error_code error;
    const auto relative = std::filesystem::relative(file, options_.game, error);
    if (error || relative.empty() || relative.native().front() == '.') {
        statusBar()->showMessage(tr("Only files of this project can be placed"), 5000);
        return;
    }
    const auto path = relative.generic_string();
    const auto kind = assetKindOf(path);
    const auto& grid = world->authored().grid;
    const glm::vec3 at = point ? *point : sceneView_->focus();
    const GridPos where = cell ? *cell : snapToCell(grid, at, session_->layer);
    const auto model = kind == AssetKind::Model ? engine_->assets().tryModel(path) : nullptr;
    const auto placement = placementForAsset(
        kind, path, where, grid, session_->defaultType(kind == AssetKind::Model ? "models" : "entities"),
        world->authored().placements, model.get(), point ? std::optional<glm::vec3>(*point) : std::nullopt);
    if (!placement) {
        statusBar()->showMessage(tr("%1 cannot stand in a scene (models and sprites can)").arg(qstr(path)),
                                 5000);
        return;
    }
    session_->place(*placement);
    sceneDock_->raise();
}

void EditorWindow::openFile(const std::filesystem::path& file, int line) {
    const auto extension = file.extension().string();
    if (file.filename() == "map.json" && file.parent_path().parent_path().filename() == "maps") {
        if (confirmSceneChange()) {
            openScene(file.parent_path().filename().string());
            sceneDock_->raise();
        }
        return;
    }
    static const char* const text[] = {".lua", ".json", ".sc",   ".sh",  ".glsl",
                                       ".txt", ".md",   ".cube", ".def", ".csv"};
    if (std::find_if(std::begin(text), std::end(text), [&](const char* each) { return extension == each; }) !=
        std::end(text)) {
        if (code_->open(file, line)) {
            codeDock_->show();
            codeDock_->raise();
        }
        return;
    }
    const auto kind = assetKindOf(file.generic_string());
    if (kind == AssetKind::Model || kind == AssetKind::Sprite) {
        placeAsset(file, nullptr, nullptr);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(qstr(file.string())));
}

void EditorWindow::play() {
    if (playing()) {
        return;
    }
    code_->saveAll();
    data_->save();
    if (session_->unsaved() && options_.frames == 0) {
        QMessageBox ask(QMessageBox::Question, tr("Play"),
                        tr("The game loads scenes from disk. Scene '%1' has unsaved changes.")
                            .arg(qstr(session_->sceneId())),
                        QMessageBox::NoButton, this);
        auto* saveFirst = ask.addButton(tr("Save and Play"), QMessageBox::AcceptRole);
        auto* asSaved = ask.addButton(tr("Play the Saved Version"), QMessageBox::DestructiveRole);
        ask.addButton(QMessageBox::Cancel);
        ask.exec();
        if (ask.clickedButton() == saveFirst) {
            if (!session_->save()) {
                return;
            }
        } else if (ask.clickedButton() != asSaved) {
            return;
        }
    }
    PlaySession::Options options;
    options.game = options_.game;
    options.builtin = options_.builtin;
    options.userData = options_.userData / "play";
    options.mute = options_.mute;
    std::filesystem::create_directories(options.userData);
    try {
        options.config = GameConfig::load(options_.game);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Play"), QString::fromUtf8(error.what()));
        return;
    }
    if (const auto* map = session_->map()) {
        options.scene = map->id;
        options.point = map->points.contains("start") ? std::string("start")
                        : map->points.empty()         ? std::string()
                                                      : map->points.begin()->first;
        if (options.point.empty()) {
            options.scene.clear(); // nowhere to stand: the game starts from its beginning
        }
    }
    console_->appendHtml(QStringLiteral("<pre style=\"margin:0;color:#5aa469\">-- Play --</pre>"));
    gameDock_->show();
    gameDock_->raise();
    game_->sync();
    try {
        play_ = std::make_unique<PlaySession>(*renderer_, *game_, std::move(options));
    } catch (const std::exception& error) {
        play_.reset();
        log(LogLevel::Error, error.what());
        statusBar()->showMessage(QString::fromUtf8(error.what()), 10000);
        consoleDock_->raise();
        return;
    }
    playSelection_ = {};
    playSignature_.clear();
    game_->requestActivate();
    scripts_->setEngine(&play_->engine(), true, {});
    inspector_->clear(tr("Select an entity in the Hierarchy"));
    showPlayHierarchy(true);
    refreshActions();
    statusBar()->showMessage(tr("Playing. Changes to the running game are discarded on Stop."));
}

void EditorWindow::stop() {
    if (!playing()) {
        return;
    }
    scripts_->setEngine(engine_.get(), false, {});
    play_.reset();
    playSelection_ = {};
    console_->appendHtml(QStringLiteral("<pre style=\"margin:0;color:#5aa469\">-- Stop --</pre>"));
    hierarchy_->showScene(*session_);
    inspector_->inspect(*session_);
    stats_->clear();
    sceneDock_->raise();
    refreshActions();
    statusBar()->showMessage(tr("Stopped"), 4000);
}

void EditorWindow::togglePause() {
    if (playing()) {
        play_->paused = !play_->paused;
        refreshActions();
    }
}

// The Hierarchy in Play mode: rebuilt when the shown world or its set of
// entities changes.
void EditorWindow::showPlayHierarchy(bool force) {
    const auto world = play_->shownWorld();
    std::string signature = world ? world->name() : std::string();
    if (world) {
        const auto names = world->registry().view<const Name>();
        std::uint64_t mix = 0;
        std::size_t count = 0;
        for (const auto entity : names) {
            mix = mix * 1099511628211ull + entt::to_integral(entity);
            ++count;
        }
        signature += ":" + std::to_string(count) + ":" + std::to_string(mix);
    }
    for (const auto& name : play_->worldNames()) {
        signature += "|" + name;
    }
    if (!force && signature == playSignature_) {
        return;
    }
    playSignature_ = signature;
    hierarchy_->showEntities(world, play_->worldNames());
    if (!playSelection_.empty()) {
        hierarchy_->setSelected({playSelection_});
    }
    scripts_->setEngine(&play_->engine(), true, world ? world->name() : std::string());
}

void EditorWindow::renderFrame() {
    ++frames_;
    const qint64 now = clock_.nsecsElapsed();
    const double elapsed =
        lastFrameNs_ == 0 ? FrameIntervalMs / 1000.0 : static_cast<double>(now - lastFrameNs_) * 1e-9;
    lastFrameNs_ = now;
    if (options_.frames > 0) {
        // A self-test floats and re-docks the Scene view before its script starts.
        const auto floatAt = static_cast<std::uint64_t>(options_.selfTest ? 8 : options_.frames / 3);
        const auto dockAt = static_cast<std::uint64_t>(options_.selfTest ? 14 : 2 * options_.frames / 3);
        if (options_.redock && frames_ == floatAt) {
            sceneDock_->setFloating(true);
        }
        if (options_.redock && frames_ == dockAt) {
            sceneDock_->setFloating(false);
            sceneDock_->raise();
            redocked_ = true;
            rendered_ = 0; // the check counts frames drawn after the view came back
        }
        if (options_.selfTest) {
            selfTestStep();
        }
        // The panels as drawn by Qt (the native views stay blank in it).
        if (!options_.shot.empty() &&
            frames_ == static_cast<std::uint64_t>(std::max(options_.frames - 8, 1))) {
            auto file = options_.shot;
            file.replace_filename(file.stem().string() + "-window.png");
            grab().save(qstr(file.string()));
        }
    }
    scene_->sync();
    game_->sync();
    if (play_) {
        play_->advance(elapsed);
        if (play_->wantsQuit()) {
            stop();
        }
    }
    if (!renderer_->beginFrame()) {
        return;
    }
    std::uint16_t nextView = 1;
    // While the game plays, the Scene view shows the game's world through
    // the editor's camera (View > Scene Follows Game); else the edited scene.
    const bool following = play_ && sceneFollowsGame_ && scene_->ready();
    const bool sceneDrawn = !following && scene_->ready() && session_->world();
    renderer_->setImGuiTarget(scene_->ready() ? scene_->target()
                                              : bgfx::FrameBufferHandle BGFX_INVALID_HANDLE);
    if (sceneDrawn) {
        engine_->canvas().begin();
        FrameOutput out;
        out.target = scene_->target();
        out.firstView = nextView;
        // Fog is tuned for the game's camera; from the editor's distance it
        // hides the scene, so the Scene view draws without it unless asked.
        auto& environment = session_->world()->environment();
        const float fog = environment.fogDensity;
        if (!sceneFog_) {
            environment.fogDensity = 0;
        }
        nextView =
            frame_->render({sceneView_->draw()}, engine_->canvas(), engine_->screen(), scene_->pixels(), 1.0,
                           static_cast<double>(frames_) * FrameIntervalMs / 1000.0, frames_, out);
        environment.fogDensity = fog;
        sceneView_->overlay();
    }
    if (scene_->ready()) {
        ++rendered_;
        if (!options_.shot.empty() && options_.frames > 0 &&
            frames_ == static_cast<std::uint64_t>(std::max(options_.frames - 5, 1))) {
            bgfx::requestScreenShot(scene_->target(), options_.shot.string().c_str());
        }
    }
    if (play_) {
        const auto size = scene_->pixels();
        const float aspect =
            static_cast<float>(std::max(size.x, 1)) / static_cast<float>(std::max(size.y, 1));
        nextView = play_->render(nextView, following ? &sceneView_->camera().camera : nullptr, aspect);
        if (following) {
            const auto texture = play_->sceneTexture();
            auto* draw = ImGui::GetBackgroundDrawList();
            const ImVec2 full(static_cast<float>(size.x), static_cast<float>(size.y));
            draw->AddRectFilled({0, 0}, full, IM_COL32(18, 18, 22, 255));
            if (bgfx::isValid(texture)) {
                const bool flip = bgfx::getCaps()->originBottomLeft;
                draw->AddImage(static_cast<ImTextureID>(texture.idx + 1), {0, 0}, full,
                               {0, flip ? 1.0f : 0.0f}, {1, flip ? 0.0f : 1.0f});
            }
            const char* note = "Game world (live) - View > Scene Follows Game";
            draw->AddText({9, 9}, IM_COL32(0, 0, 0, 200), note);
            draw->AddText({8, 8}, IM_COL32(120, 220, 140, 255), note);
        }
    }
    nextView = thumbnails_->render(nextView);
    const auto frame = renderer_->endFrame();
    frame_->finishFrame(frame);
    thumbnails_->finishFrame(frame);
    if (play_) {
        play_->finishFrame(frame);
    }
    if (options_.frames > 0 && frames_ >= static_cast<std::uint64_t>(options_.frames) &&
        (!options_.selfTest || jobsDone_)) {
        close();
    }
}

// Four times a second: hot reload, and the panels that follow a running game.
void EditorWindow::slowTick() {
    bool luaChanged = false, filesChanged = false;
    for (const auto& change : watcher_->poll()) {
        filesChanged = true;
        code_->fileChanged(change.path);
        thumbnails_->forget(change.path);
        if (change.kind == FileChangeKind::Removed) {
            continue;
        }
        if (play_) {
            play_->reload(change.path);
        }
        if (change.path.extension() == ".lua") {
            engine_->reload(change.path);
            luaChanged = true;
        } else if (change.path.filename() != "map.json") {
            engine_->assets().reload(change.path); // the Scene view shows the new art
        }
    }
    if (luaChanged) {
        session_->refreshSchemas(); // the game's describe hook may say something new
    }
    if (filesChanged) {
        catalog_->scan();
        refreshScenesMenu();
    }
    if (play_) {
        showPlayHierarchy(false);
        inspector_->inspectEntity(play_->shownWorld(), playSelection_);
        const auto world = play_->shownWorld();
        stats_->setText(tr("%1   tick %2   %3 ms   %4 draw calls   %5 entities")
                            .arg(play_->paused ? tr("PAUSED") : tr("PLAYING"))
                            .arg(play_->ticks())
                            .arg(play_->frameMs(), 0, 'f', 1)
                            .arg(play_->drawCalls())
                            .arg(world ? world->registry().view<const Name>().size() : 0));
    }
    scripts_->refresh();
}

void EditorWindow::closeEvent(QCloseEvent* event) {
    if (options_.frames == 0) {
        if (!confirmSceneChange()) {
            event->ignore();
            return;
        }
        if (code_->hasUnsaved() || data_->unsaved()) {
            const auto answer =
                QMessageBox::question(this, tr("Quit"), tr("Save the open scripts and data tables?"),
                                      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            if (answer == QMessageBox::Cancel ||
                (answer == QMessageBox::Save && !(code_->saveAll() && data_->save()))) {
                event->ignore();
                return;
            }
        }
        QSettings settings;
        settings.setValue(QStringLiteral("layoutVersion"), LayoutVersion);
        settings.setValue(QStringLiteral("geometry"), saveGeometry());
        settings.setValue(QStringLiteral("layout"), saveState());
    }
    stop();
    timer_.stop();
    slow_.stop();
    event->accept();
}

void EditorWindow::expect(bool condition, const std::string& what) {
    ++checks_;
    if (!condition) {
        ++failures_;
        std::printf("SELF-TEST FAILED: %s\n", what.c_str());
        std::fflush(stdout);
    }
}

// A mouse event for the Scene view, as the window system would deliver it.
void EditorWindow::sendMouse(QEvent::Type type, glm::vec2 pixel, Qt::MouseButton button,
                             Qt::MouseButtons buttons) {
    const QPointF local(pixel.x / scene_->devicePixelRatio(), pixel.y / scene_->devicePixelRatio());
    QMouseEvent event(type, local, scene_->mapToGlobal(local), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(scene_, &event);
}

// Where a world point is in the Scene view, in pixels.
glm::vec2 EditorWindow::scenePixel(glm::vec3 point) const {
    const auto size = scene_->pixels();
    const ViewRect rect{glm::ivec4(0, 0, std::max(size.x, 1), std::max(size.y, 1))};
    return projectToView(sceneView_->camera().camera, rect, point, 1.0).value_or(glm::vec2(-1));
}

// The self-test's second act: the Scene view driven through real mouse
// events (select, drag, gizmo, paint, drop), then saving.
void EditorWindow::selfTestInput() {
    const auto& world = session_->world();
    const auto* map = session_->map();
    const auto position = [&] {
        return map->points.at(testPoint_).position;
    };
    const auto left = Qt::LeftButton;
    switch (frames_) {
    case 155: {
        sceneView_->frameScene();
        sceneView_->tool = SceneView::Tool::Select;
        sceneView_->snap = true;
        session_->select({});
        expect(!map->points.empty(), "the test scene has a point");
        testPoint_ = map->points.contains("start") ? "start" : map->points.begin()->first;
        for (const auto* entry : catalog_->ofKind(AssetKind::Model)) {
            if (!entry->path.empty() && entry->path.front() != '@' && entry->path.rfind("maps/", 0) != 0) {
                testModel_ = options_.game / entry->path;
                break;
            }
        }
        if (!testModel_.empty()) {
            (void)thumbnails_->icon(testModel_); // queues its picture
        }
        break;
    }
    case 160:
        testStart_ = position();
        testPixel_ = scenePixel(testStart_);
        expect(scene_->ready() && testPixel_.x >= 0, "the Scene view is showing the point");
        sendMouse(QEvent::MouseMove, testPixel_, Qt::NoButton, Qt::NoButton);
        break;
    case 162:
        sendMouse(QEvent::MouseButtonPress, testPixel_, left, left);
        break;
    case 164:
        sendMouse(QEvent::MouseButtonRelease, testPixel_, left, Qt::NoButton);
        break;
    case 166:
        expect(session_->selection() == Selection::point(testPoint_), "clicking a point's handle selects it");
        break;
    case 170:
        sendMouse(QEvent::MouseButtonPress, testPixel_, left, left);
        break;
    case 172:
    case 174:
        sendMouse(QEvent::MouseMove, scenePixel(testStart_ + glm::vec3(2, 0, 0)), Qt::NoButton, left);
        break;
    case 176:
        sendMouse(QEvent::MouseButtonRelease, scenePixel(testStart_ + glm::vec3(2, 0, 0)), left,
                  Qt::NoButton);
        break;
    case 178:
        expect(std::abs(position().x - (testStart_.x + 2)) < 0.75f, "dragging it across the ground moves it");
        session_->undo();
        expect(position() == testStart_, "and the drag is one undo step");
        break;
    case 182: {
        // The move gizmo's X axis: a little way from the origin along it.
        sceneView_->tool = SceneView::Tool::Move;
        sceneView_->snap = false;
        session_->select(Selection::point(testPoint_));
        testPixel_ = scenePixel(testStart_);
        // The gizmo works in the object's own space: its X axis follows the
        // point's facing.
        const auto facing = map->points.at(testPoint_).facing;
        const float yaw = std::atan2(facing.x, facing.y);
        testAxisWorld_ = glm::vec3(std::cos(yaw), 0, -std::sin(yaw));
        const glm::vec2 along = scenePixel(testStart_ + testAxisWorld_) - testPixel_;
        testAxis_ = glm::length(along) > 0.001f ? glm::normalize(along) : glm::vec2(1, 0);
        const float reach = 0.05f * static_cast<float>(scene_->pixels().x) * 0.6f;
        testPixel_ += testAxis_ * reach;
        sendMouse(QEvent::MouseMove, testPixel_, Qt::NoButton, Qt::NoButton);
        break;
    }
    case 184:
        sendMouse(QEvent::MouseButtonPress, testPixel_, left, left);
        break;
    case 186:
        sendMouse(QEvent::MouseMove, testPixel_ + testAxis_ * 30.0f, Qt::NoButton, left);
        break;
    case 188:
        sendMouse(QEvent::MouseMove, testPixel_ + testAxis_ * 70.0f, Qt::NoButton, left);
        break;
    case 190:
        sendMouse(QEvent::MouseButtonRelease, testPixel_ + testAxis_ * 70.0f, left, Qt::NoButton);
        break;
    case 192: {
        const glm::vec3 moved = position() - testStart_;
        const float along = glm::dot(moved, testAxisWorld_);
        expect(along > 0.05f && glm::length(moved - testAxisWorld_ * along) < 0.01f,
               "dragging the move gizmo's X axis moves the point along that axis only");
    }
        while (session_->canUndo()) {
            session_->undo();
        }
        break;
    case 196: {
        // Paint a cell blocked with the brush.
        sceneView_->tool = SceneView::Tool::Paint;
        sceneView_->brushShape = SceneView::BrushShape::Brush;
        sceneView_->brushClears = false;
        sceneView_->brushRadius = 0;
        sceneView_->brush = GridBrush{};
        sceneView_->brush.walkable = false;
        session_->select({});
        const auto& grid = map->grid;
        testCell_ = snapToCell(grid, testStart_, 0);
        // The nearest walkable cell that is not the point's own.
        for (int ring = 1; ring < 6; ++ring) {
            bool found = false;
            for (int dz = -ring; dz <= ring && !found; ++dz) {
                for (int dx = -ring; dx <= ring && !found; ++dx) {
                    const GridPos cell{testCell_.x + dx, testCell_.z + dz, 0};
                    if (grid.inRange(cell.x, cell.z, 0) &&
                        grid.walkableAt(
                            {static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f}, 0)) {
                        testCell_ = cell;
                        found = true;
                    }
                }
            }
            if (found) {
                break;
            }
        }
        testPixel_ = scenePixel(cellCentre(grid, testCell_));
        sendMouse(QEvent::MouseMove, testPixel_, Qt::NoButton, Qt::NoButton);
        break;
    }
    case 198:
        sendMouse(QEvent::MouseButtonPress, testPixel_, left, left);
        break;
    case 200:
        sendMouse(QEvent::MouseButtonRelease, testPixel_, left, Qt::NoButton);
        break;
    case 202:
        expect(!map->grid.walkableAt(
                   {static_cast<float>(testCell_.x) + 0.5f, static_cast<float>(testCell_.z) + 0.5f}, 0),
               "painting with the mouse blocks the cell under it");
        sceneView_->tool = SceneView::Tool::Move;
        testCount_ = map->placements.size();
        if (!testModel_.empty() && scene_->onDrop) {
            scene_->onDrop({qstr(testModel_.string())}, testPixel_);
        }
        break;
    case 204:
        if (!testModel_.empty()) {
            expect(map->placements.size() == testCount_ + 1, "dropping a model on the Scene view places it");
            expect(session_->selection().kind == Selection::Kind::Placement, "and selects the new placement");
        }
        openFile(options_.game / "main.lua");
        expect(code_->find(options_.game / "main.lua") != nullptr,
               "opening a script shows it in the Code panel");
        sceneDock_->raise();
        break;
    case 215:
        if (!testModel_.empty()) {
            expect(!thumbnails_->icon(testModel_).isNull(), "a model's thumbnail has been rendered");
        }
        expect(session_->save(), "saving the scene");
        expect(!session_->unsaved(), "a saved scene is not unsaved");
        expect(MapData::load(options_.game / "maps" / map->id / "map.json").toJson() == map->toJson(),
               "the file holds the scene as edited");
        (void)world;
        break;
    default:
        break;
    }
}

// The self-test's last act: the Build panel's jobs, one after another, each
// expected to succeed.
void EditorWindow::selfTestJobs() {
    if (jobsDone_ || frames_ < 220) {
        return;
    }
    if (jobs_.empty()) {
        build_->onFinished = [this](const QString& title, int code) {
            expect(code == 0,
                   "Build > " + title.toStdString() + " finished with exit code " + std::to_string(code));
            jobRunning_ = false;
        };
        jobs_.emplace_back("validate", [this] { build_->validate(); });
        jobs_.emplace_back("tests", [this] { build_->test(); });
        jobs_.emplace_back("bake", [this] { build_->bake(); });
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(options_.game / "replays", error)) {
            if (entry.path().extension() == ".json") {
                const auto replay = qstr(entry.path().string());
                jobs_.emplace_back("replay", [this, replay] { build_->runReplayFile(replay); });
                break;
            }
        }
        const auto recording = qstr((options_.userData / "selftest-recording.json").string());
        jobs_.emplace_back("record", [this, recording] {
            build_->recordReplayTo(
                recording, {QStringLiteral("--frames"), QStringLiteral("60"), QStringLiteral("--mute")});
        });
        // Packaging needs a Release build of the engine; FENGINE_RELEASE_BUILD names one.
        if (const auto release = qEnvironmentVariable("FENGINE_RELEASE_BUILD"); !release.isEmpty()) {
            const auto output = qstr((options_.userData / "selftest-package").string());
            jobs_.emplace_back("package", [this, release, output] {
                std::filesystem::remove_all(output.toStdString());
                std::filesystem::create_directories(output.toStdString());
                build_->packageFrom(release, output);
            });
        }
    }
    if (jobRunning_ || build_->running()) {
        return;
    }
    if (job_ >= jobs_.size()) {
        jobsDone_ = true;
        return;
    }
    jobRunning_ = true;
    std::printf("self-test job: %s\n", jobs_[job_].first.c_str());
    std::fflush(stdout);
    jobs_[job_++].second();
}

// The self-test script, by frame number. It edits the open scene in memory
// (never saving), then plays the game and inspects it.
void EditorWindow::selfTestStep() {
    const auto& world = session_->world();
    if (!world) {
        jobsDone_ = true; // nothing to test on
        return;
    }
    selfTestInput();
    selfTestJobs();
    const auto* map = session_->map();
    switch (frames_) {
    case 20: {
        testBefore_ = map->toJson();
        expect(!session_->unsaved(), "a freshly opened scene is saved");
        // The subject: the scene's first placement, else its first point.
        const auto first = !map->placements.empty() ? Selection::placement(map->placements.front().id)
                           : !map->points.empty()   ? Selection::point(map->points.begin()->first)
                                                    : Selection{};
        expect(!first.empty(), "the test scene has a placement or a point");
        session_->select(first);
        expect(session_->selection() == first, "selecting an object");
        break;
    }
    case 24: {
        const auto selection = session_->selection();
        auto transform = selectionTransform(*world, selection);
        expect(transform.has_value(), "the selection has a transform");
        if (transform) {
            const auto before = transform->position;
            const auto where = [&] {
                return selectionTransform(*world, selection)->position;
            };
            transform->position.x += 1;
            session_->setTransform(selection, *transform, true);
            session_->endGesture();
            expect(where().x == before.x + 1, "moving an object");
            expect(session_->unsaved() && session_->canUndo(), "a move is an unsaved, undoable edit");
            session_->undo();
            expect(where() == before, "undo restores the position");
            expect(!session_->unsaved(), "undoing back to the saved state is saved");
            session_->redo();
            expect(where().x == before.x + 1, "redo moves it again");
        }
        break;
    }
    case 28: {
        // A typed edit through the Inspector's path, valid and invalid.
        const auto selection = session_->selection();
        Json object = session_->objectJson(selection);
        expect(object.is_object(), "the selection has a form");
        if (object.is_object()) {
            object["facing"] = Json::array({1.0, 0.0});
            expect(session_->setObjectJson(selection, object).empty(), "a valid typed edit applies");
            expect(session_->objectJson(selection).at("facing").at(0).get<double>() == 1.0,
                   "the typed edit reached the map");
            object["position"] = "nowhere";
            expect(!session_->setObjectJson(selection, object).empty(), "an invalid typed edit is refused");
        }
        const auto lights = map->lights.size();
        const auto made = session_->add(Selection::Kind::Light, sceneView_->focus());
        expect(map->lights.size() == lights + 1 && made.kind == Selection::Kind::Light, "adding a light");
        session_->duplicateSelection();
        expect(map->lights.size() == lights + 2, "duplicating it");
        session_->deleteSelection();
        expect(map->lights.size() == lights + 1, "deleting the copy");
        const auto zone = session_->add(Selection::Kind::Zone, sceneView_->focus());
        Json renamed = session_->objectJson(zone);
        renamed["id"] = "self_test_zone";
        expect(session_->setObjectJson(zone, renamed).empty() && session_->selection().id == "self_test_zone",
               "renaming a zone keeps it selected");
        const auto cell = snapToCell(map->grid, sceneView_->focus(), 0);
        GridBrush blocked;
        blocked.walkable = false;
        session_->paintWalkability(cell, 1, blocked, false, false);
        expect(
            !map->grid.walkableAt({static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f}, 0),
            "painting cells as blocked");
        expect(!session_->setMapJson("/environment/fog_density", 0.01).empty() ||
                   session_->mapJson("/environment/fog_density").get<double>() > 0.0,
               "editing the environment");
        break;
    }
    case 34: {
        while (session_->canUndo()) {
            session_->undo();
        }
        expect(map->toJson() == testBefore_, "undoing everything restores the scene exactly");
        expect(!session_->unsaved(), "and it counts as saved");
        break;
    }
    case 40:
        play();
        expect(playing(), "Play starts the game");
        break;
    case 100:
        sceneDock_->raise(); // the Scene view follows the game; the Game view is now hidden
        break;
    case 130:
        if (playing()) {
            expect(bgfx::isValid(play_->sceneTexture()), "the Scene view shows the running game's world");
            int count = 0;
            if (SDL_JoystickID* pads = SDL_GetGamepads(&count)) {
                SDL_free(pads);
            }
            if (count > 0) {
                expect(!play_->engine().input().pads().empty(),
                       "a connected gamepad reaches the game in Play mode");
            }
            expect(play_->ticks() > 30, "the game ticks while playing");
            const auto shown = play_->shownWorld();
            expect(shown != nullptr, "the running game has a world");
            if (shown) {
                const auto names = shown->registry().view<const Name>();
                expect(names.begin() != names.end(), "with entities in it");
                if (names.begin() != names.end()) {
                    playSelection_ = Selection::ofEntity(*shown, *names.begin());
                    const auto components = componentsOf(*shown, playSelection_.entity);
                    expect(components.size() >= 2, "an entity shows its components");
                    inspector_->inspectEntity(shown, playSelection_);
                }
            }
            togglePause();
            const auto ticks = play_->ticks();
            play_->step();
            expect(play_->ticks() == ticks + 1, "Step runs exactly one tick while paused");
            togglePause();
        }
        break;
    case 150:
        stop();
        expect(!playing(), "Stop ends the game");
        expect(session_->map() && session_->map()->toJson() == testBefore_,
               "playing left the edited scene untouched");
        break;
    default:
        break;
    }
}

bool EditorWindow::selfCheckPassed() const {
    const bool shot = options_.shot.empty() || std::filesystem::is_regular_file(options_.shot);
    if (options_.selfTest) {
        std::printf("self-test: %d checks, %d failed\n", checks_, failures_);
    }
    return session_ && rendered_ > 0 && shot && (!options_.redock || redocked_) &&
           (!options_.selfTest || (checks_ > 40 && failures_ == 0));
}
} // namespace fe::editor
