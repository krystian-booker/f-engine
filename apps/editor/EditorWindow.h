#pragma once
#include "apps/editor/CodeEditor.h"
#include "apps/editor/JsonForm.h"
#include "apps/editor/NativeSurface.h"
#include "engine/core/GameConfig.h"
#include "engine/editor/Selection.h"
#include "engine/nav/Grid.h"
#include "engine/platform/FileWatcher.h"
#include <QElapsedTimer>
#include <QEvent>
#include <QMainWindow>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
class QAction;
class QComboBox;
class QDockWidget;
class QKeyEvent;
class QLabel;
class QMenu;
class QPlainTextEdit;

namespace fe {
class AssetCatalog;
class EditorSession;
class Engine;
class FrameRenderer;
class Renderer;
} // namespace fe

namespace fe::editor {
class BuildPanel;
class DataPanel;
class HierarchyPanel;
class InspectorPanel;
class PlaySession;
class ProjectPanel;
class SceneView;
class Thumbnails;
class ScriptsPanel;
class ViewportWindow;

struct EditorOptions {
    std::filesystem::path game;     // the project: a game folder
    std::filesystem::path shaders;  // compiled shaders
    std::filesystem::path builtin;  // the engine's builtin assets
    std::filesystem::path userData; // the editor's own folder for this project
    std::string map;                // scene to open first; empty = the last one, else the first
    bool mute = false;              // Play mode without sound
    // Self-check runs (tests): stop after `frames` frames, writing the Scene
    // view as a PNG to `shot` near the end and, with `redock`, floating and
    // re-docking the Scene view on the way (it must draw again afterwards).
    // `selfTest` also drives the editor through an editing and a Play
    // session and checks what comes of it; it never saves anything.
    int frames = 0;
    std::filesystem::path shot;
    bool redock = false;
    bool selfTest = false;
};

// Asks where and what, and writes a new, minimal game folder.
std::filesystem::path createProject(QWidget* parent);

// The editor's main window for one open project: Unity's layout over the
// engine. It owns the renderer, the edit engine (which never runs the game)
// with its editing session, and, between Play and Stop, a play session.
class EditorWindow final : public QMainWindow {
public:
    explicit EditorWindow(EditorOptions options);
    ~EditorWindow() override;

    // Whether a self-check run met its expectations (see EditorOptions).
    [[nodiscard]] bool selfCheckPassed() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildPanels();
    void buildMenus();
    void wireSession();
    void refreshScenesMenu();
    void refreshCreateMenu();
    void refreshTitle();
    void refreshActions();
    void refreshChoices();
    void renderFrame();
    void slowTick();
    void appendLog(int level, const QString& line);
    void dispatchShortcut(QKeyEvent* event);

    // Scenes.
    bool confirmSceneChange(); // false = the user cancelled
    void openScene(const std::string& id);
    void newScene();
    void saveSceneAs();
    void renameScene();
    void deleteScene();
    void projectSettings();

    // Objects.
    void createObject(Selection::Kind kind, const std::string& type = {});
    void placeAsset(const std::filesystem::path& file, const glm::vec3* point, const GridPos* cell);
    void openFile(const std::filesystem::path& file, int line = 0);

    // Play mode.
    void play();
    void stop();
    void togglePause();
    [[nodiscard]] bool playing() const { return play_ != nullptr; }
    void showPlayHierarchy(bool force);

    void selfTestStep();
    void selfTestInput();
    void selfTestJobs();
    void expect(bool condition, const std::string& what);
    void sendMouse(QEvent::Type type, glm::vec2 pixel, Qt::MouseButton button, Qt::MouseButtons buttons);
    [[nodiscard]] glm::vec2 scenePixel(glm::vec3 point) const;

    EditorOptions options_;
    GameConfig config_;
    HeadlessSurface surface_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<FrameRenderer> frame_;
    std::unique_ptr<EditorSession> session_;
    std::unique_ptr<AssetCatalog> catalog_;
    std::unique_ptr<SceneView> sceneView_;
    std::unique_ptr<PlaySession> play_;
    std::unique_ptr<FileWatcher> watcher_;
    std::unique_ptr<Thumbnails> thumbnails_;
    FormChoices choices_;
    Selection playSelection_;
    std::string playSignature_;

    ViewportWindow* scene_ = nullptr;
    ViewportWindow* game_ = nullptr;
    QDockWidget* sceneDock_ = nullptr;
    QDockWidget* gameDock_ = nullptr;
    QDockWidget* codeDock_ = nullptr;
    QDockWidget* consoleDock_ = nullptr;
    QDockWidget* scriptsDock_ = nullptr;
    QDockWidget* buildDock_ = nullptr;
    HierarchyPanel* hierarchy_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    ProjectPanel* project_ = nullptr;
    CodePanel* code_ = nullptr;
    ScriptsPanel* scripts_ = nullptr;
    DataPanel* data_ = nullptr;
    BuildPanel* build_ = nullptr;
    QPlainTextEdit* console_ = nullptr;
    QComboBox* consoleLevel_ = nullptr;
    QLabel* stats_ = nullptr;
    QMenu* scenesMenu_ = nullptr;
    QMenu* createMenu_ = nullptr;
    QAction *undo_ = nullptr, *redo_ = nullptr, *save_ = nullptr, *play_action_ = nullptr, *pause_ = nullptr,
            *step_ = nullptr, *duplicate_ = nullptr, *delete_ = nullptr;
    std::vector<QAction*> tools_;
    QTimer timer_, slow_;
    QElapsedTimer clock_;
    qint64 lastFrameNs_ = 0;
    std::uint64_t frames_ = 0, rendered_ = 0;
    bool redocked_ = false, closing_ = false, sceneFog_ = false, sceneFollowsGame_ = true;
    int failures_ = 0, checks_ = 0;
    Json testBefore_;
    // The self-test's scratch state.
    std::string testPoint_;
    glm::vec3 testStart_{0}, testAxisWorld_{1, 0, 0};
    glm::vec2 testPixel_{0}, testAxis_{1, 0};
    GridPos testCell_;
    std::filesystem::path testModel_;
    std::size_t testCount_ = 0;
    std::vector<std::pair<std::string, std::function<void()>>> jobs_;
    std::size_t job_ = 0;
    bool jobsDone_ = false, jobRunning_ = false;
};
} // namespace fe::editor
