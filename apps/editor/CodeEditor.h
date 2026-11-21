#pragma once
#include <QPlainTextEdit>
#include <QStringList>
#include <QTabWidget>
#include <filesystem>
#include <functional>
#include <map>
class QCompleter;
class QLineEdit;
class QStringListModel;
class QSyntaxHighlighter;

namespace fe::editor {
// What the code editor knows of the engine's Lua API: names to complete and
// a line of documentation for each ("fe.world.load" -> signature and doc).
struct ApiDocs {
    QStringList names;
    std::map<QString, QString> docs;
};

// One text file: line numbers, syntax colours by file type (Lua, JSON,
// shader), completion of the engine's API and of the words in the file,
// documentation on hover, and a marked error line.
class CodeEditor final : public QPlainTextEdit {
public:
    CodeEditor(std::filesystem::path file, const ApiDocs* api, QWidget* parent = nullptr);

    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    // Reads the file from disk, dropping edits. False when it cannot be read.
    bool load();
    bool save();
    void goToLine(int line);
    // Marks a line as the place of an error (0 clears the mark).
    void setErrorLine(int line, const QString& message);

    void paintLineArea(QPaintEvent* event);
    [[nodiscard]] int lineAreaWidth() const;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    [[nodiscard]] QString wordAt(QTextCursor cursor, bool dotted) const;
    void refreshMarks();
    void complete();

    std::filesystem::path file_;
    const ApiDocs* api_;
    QWidget* lineArea_ = nullptr;
    QCompleter* completer_ = nullptr;
    QStringListModel* words_ = nullptr;
    int errorLine_ = 0;
    QString errorText_;
};

// The editor's tabs of open files, with find and replace.
class CodePanel final : public QWidget {
public:
    explicit CodePanel(QWidget* parent = nullptr);
    void setApi(ApiDocs api) { api_ = std::move(api); }

    // Opens (or shows) a file, at a line when given (1-based).
    CodeEditor* open(const std::filesystem::path& file, int line = 0);
    [[nodiscard]] CodeEditor* current() const;
    [[nodiscard]] CodeEditor* find(const std::filesystem::path& file) const;
    bool saveCurrent();
    bool saveAll();
    [[nodiscard]] bool hasUnsaved() const;
    // A file changed on disk: an open copy without edits follows it.
    void fileChanged(const std::filesystem::path& file);
    void showFind();

    std::function<void(const std::filesystem::path& file)> onSaved;

private:
    void closeTab(int index);
    void retitle(CodeEditor* editor);
    void findNext(bool backwards);

    ApiDocs api_;
    QTabWidget* tabs_ = nullptr;
    QWidget* findBar_ = nullptr;
    QLineEdit* findText_ = nullptr;
    QLineEdit* replaceText_ = nullptr;
};
} // namespace fe::editor
