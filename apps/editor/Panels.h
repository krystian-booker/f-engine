#pragma once
#include "apps/editor/JsonForm.h"
#include "engine/core/Json.h"
#include "engine/editor/Selection.h"
#include <QProcess>
#include <QWidget>
#include <filesystem>
#include <functional>
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace fe {
class EditorSession;
class Engine;
} // namespace fe

namespace fe::editor {
// The Scripts panel: what Lua is doing. The modules the game has loaded, the
// script errors so far, the tasks (coroutines) the game says are running,
// what the scene's objects are wired to, and a console that evaluates Lua in
// the game. It watches the running game while one plays, else the edit
// engine.
class ScriptsPanel final : public QWidget {
public:
    explicit ScriptsPanel(EditorSession& session, QWidget* parent = nullptr);
    // The engine to watch (the play engine while playing) and the world its
    // events are asked for.
    void setEngine(Engine* engine, bool playing, const std::string& world);
    // A few times a second.
    void refresh();
    // The scene's structure or schemas changed.
    void refreshWiring();

    std::function<void(const std::filesystem::path& file, int line)> onOpenFile;
    std::function<void(const Selection& selection)> onSelect;

private:
    void runConsole();

    EditorSession& session_;
    Engine* engine_ = nullptr;
    bool playing_ = false;
    std::string world_;
    QTabWidget* tabs_ = nullptr;
    QTreeWidget* modules_ = nullptr;
    QTreeWidget* errors_ = nullptr;
    QTreeWidget* tasks_ = nullptr;
    QTreeWidget* events_ = nullptr;
    QTreeWidget* wiring_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QLineEdit* input_ = nullptr;
    QLabel* target_ = nullptr;
    QStringList history_;
    int historyAt_ = 0;
    std::size_t errorsShown_ = 0, modulesShown_ = 0;
    Engine* errorsOf_ = nullptr;
};

// The Data panel: the game's data tables (the `tables` of its
// editor.describe hook), a record at a time, checked by the game's
// editor.validate_table hook before they are written.
class DataPanel final : public QWidget {
public:
    explicit DataPanel(EditorSession& session, QWidget* parent = nullptr);
    void setChoices(const FormChoices& choices);
    // The schemas changed: the list of tables.
    void refreshTables();
    [[nodiscard]] bool unsaved() const { return dirty_; }
    bool save();

    std::function<void(const std::filesystem::path& file)> onSaved;

private:
    void loadTable(int index);
    void showRecord(int row);
    void fillRecords();
    void setDirty(bool dirty);

    EditorSession& session_;
    QComboBox* tables_ = nullptr;
    QListWidget* records_ = nullptr;
    JsonForm* form_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* save_ = nullptr;
    Json table_;    // the table's description
    Json document_; // its records
    bool dirty_ = false;
};

// The Build panel: the project-wide jobs, each a run of the engine's command
// line tools with its output shown here. Validate (--check), the game's Lua
// tests (--test), the walkability bake (--bake) and packaging a build.
class BuildPanel final : public QWidget {
public:
    BuildPanel(std::filesystem::path gameRoot, QWidget* parent = nullptr);
    [[nodiscard]] bool running() const { return process_.state() != QProcess::NotRunning; }
    void run(const QString& title, const QString& program, const QStringList& arguments);
    void validate();
    void test();
    void bake();
    void package();
    // Plays the game in its own window while recording the input as a
    // replay, and runs a recorded replay without a window.
    void recordReplay();
    void runReplay();
    // The same jobs with the files given instead of asked for.
    void recordReplayTo(const QString& file, const QStringList& extra = {});
    void runReplayFile(const QString& file);
    void packageFrom(const QString& releaseBuild, const QString& outputFolder);

    std::function<void(const std::filesystem::path& file, int line)> onOpenFile;
    std::function<void(const QString& title, int exitCode)> onFinished;

private:
    [[nodiscard]] QString headless() const;

    std::filesystem::path root_;
    QProcess process_;
    QPlainTextEdit* output_ = nullptr;
    QLabel* state_ = nullptr;
    QString title_;
    std::vector<QPushButton*> buttons_;
};
} // namespace fe::editor
