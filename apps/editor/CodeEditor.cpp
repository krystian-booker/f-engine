#include "apps/editor/CodeEditor.h"
#include <QAbstractItemView>
#include <QCompleter>
#include <QFile>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTabBar>
#include <QTextBlock>
#include <QToolTip>
#include <QVBoxLayout>
#include <set>

namespace fe::editor {
namespace {
QString qpath(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

// Colours that read on light and dark palettes alike.
struct Colors {
    QColor keyword, string, number, comment, api;
    explicit Colors(bool dark)
        : keyword(dark ? "#c792ea" : "#7b30a0"), string(dark ? "#c3e88d" : "#3a7d22"),
          number(dark ? "#f78c6c" : "#b3521f"), comment(dark ? "#7f8c98" : "#7a8590"),
          api(dark ? "#82aaff" : "#1f5fbf") {}
};

class Highlighter final : public QSyntaxHighlighter {
public:
    enum class Language { Lua, Json, Shader, Plain };
    Highlighter(QTextDocument* document, Language language, bool dark)
        : QSyntaxHighlighter(document), language_(language), colors_(dark) {
        static const char* const lua[] = {"and",   "break", "do",       "else", "elseif", "end",
                                          "false", "for",   "function", "goto", "if",     "in",
                                          "local", "nil",   "not",      "or",   "repeat", "return",
                                          "then",  "true",  "until",    "while"};
        static const char* const shader[] = {"void",   "float",   "int",   "bool",      "vec2",    "vec3",
                                             "vec4",   "mat3",    "mat4",  "if",        "else",    "for",
                                             "return", "uniform", "const", "sampler2D", "discard", "true",
                                             "false",  "in",      "out",   "inout"};
        static const char* const json[] = {"true", "false", "null"};
        QStringList words;
        if (language == Language::Lua) {
            for (const auto* word : lua) {
                words << QLatin1String(word);
            }
        } else if (language == Language::Shader) {
            for (const auto* word : shader) {
                words << QLatin1String(word);
            }
        } else if (language == Language::Json) {
            for (const auto* word : json) {
                words << QLatin1String(word);
            }
        }
        if (!words.isEmpty()) {
            keywords_ = QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(words.join(QLatin1Char('|'))));
        }
    }

protected:
    void highlightBlock(const QString& text) override {
        if (language_ == Language::Plain) {
            return;
        }
        const auto paint = [&](const QRegularExpression& pattern, const QColor& color) {
            auto matches = pattern.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                setFormat(static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()),
                          color);
            }
        };
        static const QRegularExpression number(
            QStringLiteral("\\b(0x[0-9a-fA-F]+|\\d+\\.?\\d*([eE][-+]?\\d+)?)\\b"));
        static const QRegularExpression api(QStringLiteral("\\bfe(\\.[A-Za-z_][A-Za-z0-9_]*)+"));
        if (keywords_.isValid() && !keywords_.pattern().isEmpty()) {
            paint(keywords_, colors_.keyword);
        }
        paint(number, colors_.number);
        if (language_ == Language::Lua) {
            paint(api, colors_.api);
        }
        // Strings and comments by hand: either can hold the other's opener.
        // Block state 1 is inside a Lua long comment or long string, 2 inside
        // a C block comment.
        int i = 0;
        const int n = static_cast<int>(text.size());
        int state = previousBlockState();
        const auto closeLong = [&](int from, const QColor& color, const QString& closer, int inside) {
            const int end = static_cast<int>(text.indexOf(closer, from));
            if (end < 0) {
                setFormat(from, n - from, color);
                setCurrentBlockState(inside);
                return n;
            }
            setFormat(from, end + static_cast<int>(closer.size()) - from, color);
            setCurrentBlockState(0);
            return end + static_cast<int>(closer.size());
        };
        setCurrentBlockState(0);
        if (state == 1) {
            i = closeLong(0, colors_.comment, QStringLiteral("]]"), 1);
        } else if (state == 2) {
            i = closeLong(0, colors_.comment, QStringLiteral("*/"), 2);
        }
        while (i < n) {
            const QChar c = text[i];
            if (language_ == Language::Lua && text.mid(i, 4) == QLatin1String("--[[")) {
                i = closeLong(i, colors_.comment, QStringLiteral("]]"), 1);
            } else if (language_ == Language::Lua && text.mid(i, 2) == QLatin1String("--")) {
                setFormat(i, n - i, colors_.comment);
                break;
            } else if (language_ == Language::Lua && text.mid(i, 2) == QLatin1String("[[")) {
                i = closeLong(i, colors_.string, QStringLiteral("]]"), 1);
            } else if (language_ == Language::Shader && text.mid(i, 2) == QLatin1String("//")) {
                setFormat(i, n - i, colors_.comment);
                break;
            } else if (language_ == Language::Shader && text.mid(i, 2) == QLatin1String("/*")) {
                i = closeLong(i, colors_.comment, QStringLiteral("*/"), 2);
            } else if (c == QLatin1Char('"') || (c == QLatin1Char('\'') && language_ != Language::Json)) {
                int end = i + 1;
                while (end < n && text[end] != c) {
                    end += text[end] == QLatin1Char('\\') ? 2 : 1;
                }
                end = std::min(end + 1, n);
                setFormat(i, end - i, colors_.string);
                i = end;
            } else {
                ++i;
            }
        }
    }

private:
    Language language_;
    Colors colors_;
    QRegularExpression keywords_;
};

Highlighter::Language languageOf(const std::filesystem::path& file) {
    const auto extension = file.extension().string();
    if (extension == ".lua") {
        return Highlighter::Language::Lua;
    }
    if (extension == ".json") {
        return Highlighter::Language::Json;
    }
    if (extension == ".sc" || extension == ".sh" || extension == ".glsl") {
        return Highlighter::Language::Shader;
    }
    return Highlighter::Language::Plain;
}

class LineArea final : public QWidget {
public:
    explicit LineArea(CodeEditor* editor) : QWidget(editor), editor_(editor) {}
    [[nodiscard]] QSize sizeHint() const override { return {editor_->lineAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { editor_->paintLineArea(event); }

private:
    CodeEditor* editor_;
};
} // namespace

CodeEditor::CodeEditor(std::filesystem::path file, const ApiDocs* api, QWidget* parent)
    : QPlainTextEdit(parent), file_(std::move(file)), api_(api) {
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(font().pointSizeF());
    setFont(mono);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    new Highlighter(document(), languageOf(file_), dark);

    lineArea_ = new LineArea(this);
    const auto margins = [this] {
        setViewportMargins(lineAreaWidth(), 0, 0, 0);
    };
    QObject::connect(this, &QPlainTextEdit::blockCountChanged, this, margins);
    QObject::connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy != 0) {
            lineArea_->scroll(0, dy);
        } else {
            lineArea_->update(0, rect.y(), lineArea_->width(), rect.height());
        }
    });
    QObject::connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { refreshMarks(); });
    margins();

    words_ = new QStringListModel(this);
    completer_ = new QCompleter(words_, this);
    completer_->setWidget(this);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setCaseSensitivity(Qt::CaseSensitive);
    QObject::connect(completer_, qOverload<const QString&>(&QCompleter::activated), this,
                     [this](const QString& completion) {
                         QTextCursor cursor = textCursor();
                         const int typed = static_cast<int>(completer_->completionPrefix().size());
                         cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, typed);
                         cursor.insertText(completion);
                         setTextCursor(cursor);
                     });
    refreshMarks();
}

bool CodeEditor::load() {
    QFile source(qpath(file_));
    if (!source.open(QIODevice::ReadOnly)) {
        return false;
    }
    setPlainText(QString::fromUtf8(source.readAll()));
    document()->setModified(false);
    return true;
}

bool CodeEditor::save() {
    QSaveFile target(qpath(file_));
    if (!target.open(QIODevice::WriteOnly)) {
        return false;
    }
    target.write(toPlainText().toUtf8());
    if (!target.commit()) {
        return false;
    }
    document()->setModified(false);
    return true;
}

void CodeEditor::goToLine(int line) {
    const auto block = document()->findBlockByNumber(std::max(line, 1) - 1);
    if (block.isValid()) {
        QTextCursor cursor(block);
        setTextCursor(cursor);
        centerCursor();
    }
    setFocus();
}

void CodeEditor::setErrorLine(int line, const QString& message) {
    errorLine_ = line;
    errorText_ = message;
    refreshMarks();
}

void CodeEditor::refreshMarks() {
    QList<QTextEdit::ExtraSelection> marks;
    QTextEdit::ExtraSelection currentLine;
    QColor shade = palette().color(QPalette::Highlight);
    shade.setAlpha(28);
    currentLine.format.setBackground(shade);
    currentLine.format.setProperty(QTextFormat::FullWidthSelection, true);
    currentLine.cursor = textCursor();
    currentLine.cursor.clearSelection();
    marks.append(currentLine);
    if (errorLine_ > 0) {
        const auto block = document()->findBlockByNumber(errorLine_ - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection error;
            error.format.setBackground(QColor(200, 60, 40, 70));
            error.format.setProperty(QTextFormat::FullWidthSelection, true);
            error.format.setToolTip(errorText_);
            error.cursor = QTextCursor(block);
            marks.append(error);
        }
    }
    setExtraSelections(marks);
}

int CodeEditor::lineAreaWidth() const {
    int digits = 1;
    for (int count = std::max(1, blockCount()); count >= 10; count /= 10) {
        ++digits;
    }
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(digits, 3);
}

void CodeEditor::paintLineArea(QPaintEvent* event) {
    QPainter painter(lineArea_);
    painter.fillRect(event->rect(), palette().color(QPalette::Window));
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const bool error = errorLine_ == number + 1;
            painter.setPen(error ? QColor(220, 80, 60)
                                 : palette().color(number == current ? QPalette::Text : QPalette::Mid));
            painter.drawText(0, top, lineArea_->width() - 6, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    lineArea_->setGeometry(QRect(area.left(), area.top(), lineAreaWidth(), area.height()));
}

// The identifier around (or ending at) the cursor; `dotted` takes the whole
// "fe.world.load" path.
QString CodeEditor::wordAt(QTextCursor cursor, bool dotted) const {
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    const auto part = [&](QChar c) {
        return c.isLetterOrNumber() || c == QLatin1Char('_') ||
               (dotted && (c == QLatin1Char('.') || c == QLatin1Char(':')));
    };
    int from = at;
    while (from > 0 && part(line[from - 1])) {
        --from;
    }
    int to = at;
    if (!dotted || !cursor.atBlockEnd()) {
        while (to < line.size() && (line[to].isLetterOrNumber() || line[to] == QLatin1Char('_'))) {
            ++to;
        }
    }
    return line.mid(from, to - from);
}

void CodeEditor::complete() {
    QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    int from = cursor.positionInBlock();
    while (from > 0 && (line[from - 1].isLetterOrNumber() || line[from - 1] == QLatin1Char('_') ||
                        line[from - 1] == QLatin1Char('.'))) {
        --from;
    }
    const QString prefix = line.mid(from, cursor.positionInBlock() - from);
    if (prefix.size() < 2) {
        completer_->popup()->hide();
        return;
    }
    // The API's names and the identifiers of this file.
    std::set<QString> all;
    if (api_) {
        all.insert(api_->names.begin(), api_->names.end());
    }
    static const QRegularExpression identifier(QStringLiteral("[A-Za-z_][A-Za-z0-9_]{2,}"));
    auto found = identifier.globalMatch(toPlainText());
    while (found.hasNext()) {
        const auto word = found.next().captured();
        if (word != prefix) {
            all.insert(word);
        }
    }
    words_->setStringList(QStringList(all.begin(), all.end()));
    completer_->setCompletionPrefix(prefix);
    if (completer_->completionCount() == 0) {
        completer_->popup()->hide();
        return;
    }
    QRect where = cursorRect();
    where.setWidth(completer_->popup()->sizeHintForColumn(0) +
                   completer_->popup()->verticalScrollBar()->sizeHint().width() + 16);
    completer_->popup()->setCurrentIndex(completer_->completionModel()->index(0, 0));
    completer_->complete(where);
}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
    if (completer_->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore(); // the popup takes these
            return;
        default:
            break;
        }
    }
    if (event->key() == Qt::Key_Tab && !textCursor().hasSelection()) {
        insertPlainText(file_.extension() == ".lua" || file_.extension() == ".json" ? QStringLiteral("  ")
                                                                                    : QStringLiteral("    "));
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        event->modifiers() == Qt::NoModifier) {
        // Keep the line's indentation.
        const QString line = textCursor().block().text();
        int indent = 0;
        while (indent < line.size() && line[indent] == QLatin1Char(' ')) {
            ++indent;
        }
        QPlainTextEdit::keyPressEvent(event);
        insertPlainText(QString(indent, QLatin1Char(' ')));
        return;
    }
    const bool forced = event->key() == Qt::Key_Space && event->modifiers().testFlag(Qt::ControlModifier);
    if (!forced) {
        QPlainTextEdit::keyPressEvent(event);
    }
    const QString typed = event->text();
    if (forced || (!typed.isEmpty() && (typed.at(0).isLetterOrNumber() || typed.at(0) == QLatin1Char('_') ||
                                        typed.at(0) == QLatin1Char('.')))) {
        complete();
    } else {
        completer_->popup()->hide();
    }
}

bool CodeEditor::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip && api_) {
        const auto* help = static_cast<QHelpEvent*>(event);
        QPoint at = help->pos();
        at.setX(at.x() - lineAreaWidth());
        const QString word = wordAt(cursorForPosition(at), true).replace(QLatin1Char(':'), QLatin1Char('.'));
        // "world:spawn" is documented under its type; try the path, then the
        // last name alone.
        auto found = api_->docs.find(word);
        if (found == api_->docs.end()) {
            found = api_->docs.find(word.section(QLatin1Char('.'), -1));
        }
        if (found != api_->docs.end() && !word.isEmpty()) {
            QToolTip::showText(help->globalPos(), found->second, this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QPlainTextEdit::event(event);
}

CodePanel::CodePanel(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("codeTabs"));
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    column->addWidget(tabs_, 1);

    findBar_ = new QWidget(this);
    auto* row = new QHBoxLayout(findBar_);
    row->setContentsMargins(4, 2, 4, 2);
    findText_ = new QLineEdit(findBar_);
    findText_->setPlaceholderText(tr("Find"));
    replaceText_ = new QLineEdit(findBar_);
    replaceText_->setPlaceholderText(tr("Replace with"));
    auto* previous = new QPushButton(tr("Previous"), findBar_);
    auto* next = new QPushButton(tr("Next"), findBar_);
    auto* replace = new QPushButton(tr("Replace"), findBar_);
    auto* replaceAll = new QPushButton(tr("All"), findBar_);
    auto* close = new QPushButton(QStringLiteral("×"), findBar_);
    close->setFixedWidth(28);
    for (QWidget* widget : std::initializer_list<QWidget*>{findText_, previous, next, replaceText_, replace,
                                                           replaceAll, close}) {
        row->addWidget(widget);
    }
    findBar_->setVisible(false);
    column->addWidget(findBar_);

    QObject::connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTab(index); });
    QObject::connect(findText_, &QLineEdit::returnPressed, this, [this] { findNext(false); });
    QObject::connect(next, &QPushButton::clicked, this, [this] { findNext(false); });
    QObject::connect(previous, &QPushButton::clicked, this, [this] { findNext(true); });
    QObject::connect(close, &QPushButton::clicked, this, [this] { findBar_->setVisible(false); });
    QObject::connect(replace, &QPushButton::clicked, this, [this] {
        auto* editor = current();
        if (editor && editor->textCursor().hasSelection() &&
            editor->textCursor().selectedText() == findText_->text()) {
            editor->textCursor().insertText(replaceText_->text());
        }
        findNext(false);
    });
    QObject::connect(replaceAll, &QPushButton::clicked, this, [this] {
        auto* editor = current();
        if (!editor || findText_->text().isEmpty()) {
            return;
        }
        // One undo step for the lot.
        QTextCursor group(editor->document());
        group.beginEditBlock();
        QTextCursor cursor(editor->document());
        while (true) {
            cursor = editor->document()->find(findText_->text(), cursor, QTextDocument::FindCaseSensitively);
            if (cursor.isNull()) {
                break;
            }
            cursor.insertText(replaceText_->text());
        }
        group.endEditBlock();
    });

    const auto shortcut = [this](const QKeySequence& keys, std::function<void()> action) {
        auto* made = new QShortcut(keys, this);
        made->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(made, &QShortcut::activated, this, std::move(action));
    };
    shortcut(QKeySequence::Find, [this] { showFind(); });
    shortcut(QKeySequence::Save, [this] { saveCurrent(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_G), [this] {
        if (auto* editor = current()) {
            bool ok = false;
            const int line =
                QInputDialog::getInt(this, tr("Go to line"), tr("Line"),
                                     editor->textCursor().blockNumber() + 1, 1, editor->blockCount(), 1, &ok);
            if (ok) {
                editor->goToLine(line);
            }
        }
    });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_W), [this] {
        if (tabs_->currentIndex() >= 0) {
            closeTab(tabs_->currentIndex());
        }
    });
}

CodeEditor* CodePanel::current() const {
    return static_cast<CodeEditor*>(tabs_->currentWidget());
}

CodeEditor* CodePanel::find(const std::filesystem::path& file) const {
    std::error_code error;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = static_cast<CodeEditor*>(tabs_->widget(i));
        if (editor->file() == file || std::filesystem::equivalent(editor->file(), file, error)) {
            return editor;
        }
    }
    return nullptr;
}

void CodePanel::retitle(CodeEditor* editor) {
    const int index = tabs_->indexOf(editor);
    if (index >= 0) {
        tabs_->setTabText(index, QString::fromStdString(editor->file().filename().string()) +
                                     (editor->document()->isModified() ? QStringLiteral(" *") : QString()));
        tabs_->setTabToolTip(index, qpath(editor->file()));
    }
}

CodeEditor* CodePanel::open(const std::filesystem::path& file, int line) {
    auto* editor = find(file);
    if (!editor) {
        editor = new CodeEditor(file, &api_, tabs_);
        if (!editor->load()) {
            delete editor;
            QMessageBox::warning(this, tr("Open"), tr("Cannot read %1").arg(qpath(file)));
            return nullptr;
        }
        tabs_->addTab(editor, QString());
        retitle(editor);
        QObject::connect(editor->document(), &QTextDocument::modificationChanged, this,
                         [this, editor] { retitle(editor); });
    }
    tabs_->setCurrentWidget(editor);
    if (line > 0) {
        editor->goToLine(line);
    } else {
        editor->setFocus();
    }
    return editor;
}

bool CodePanel::saveCurrent() {
    auto* editor = current();
    if (!editor) {
        return true;
    }
    if (!editor->save()) {
        QMessageBox::warning(this, tr("Save"), tr("Cannot write %1").arg(qpath(editor->file())));
        return false;
    }
    editor->setErrorLine(0, {});
    if (onSaved) {
        onSaved(editor->file());
    }
    return true;
}

bool CodePanel::saveAll() {
    bool all = true;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = static_cast<CodeEditor*>(tabs_->widget(i));
        if (!editor->document()->isModified()) {
            continue;
        }
        if (editor->save()) {
            editor->setErrorLine(0, {});
            if (onSaved) {
                onSaved(editor->file());
            }
        } else {
            all = false;
        }
    }
    return all;
}

bool CodePanel::hasUnsaved() const {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (static_cast<CodeEditor*>(tabs_->widget(i))->document()->isModified()) {
            return true;
        }
    }
    return false;
}

void CodePanel::fileChanged(const std::filesystem::path& file) {
    if (auto* editor = find(file); editor && !editor->document()->isModified()) {
        const int line = editor->textCursor().blockNumber() + 1;
        const QString before = editor->toPlainText();
        QFile source(qpath(file));
        if (source.open(QIODevice::ReadOnly) && QString::fromUtf8(source.readAll()) != before) {
            editor->load();
            editor->goToLine(line);
        }
    }
}

void CodePanel::showFind() {
    findBar_->setVisible(true);
    if (auto* editor = current(); editor && editor->textCursor().hasSelection()) {
        findText_->setText(editor->textCursor().selectedText());
    }
    findText_->setFocus();
    findText_->selectAll();
}

void CodePanel::findNext(bool backwards) {
    auto* editor = current();
    if (!editor || findText_->text().isEmpty()) {
        return;
    }
    QTextDocument::FindFlags flags;
    if (backwards) {
        flags |= QTextDocument::FindBackward;
    }
    if (!editor->find(findText_->text(), flags)) {
        // Wrap around.
        QTextCursor cursor = editor->textCursor();
        cursor.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        editor->setTextCursor(cursor);
        editor->find(findText_->text(), flags);
    }
}

void CodePanel::closeTab(int index) {
    auto* editor = static_cast<CodeEditor*>(tabs_->widget(index));
    if (editor->document()->isModified()) {
        const auto answer = QMessageBox::question(
            this, tr("Close"),
            tr("Save the changes to %1?").arg(QString::fromStdString(editor->file().filename().string())),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel) {
            return;
        }
        if (answer == QMessageBox::Save) {
            tabs_->setCurrentIndex(index);
            if (!saveCurrent()) {
                return;
            }
        }
    }
    tabs_->removeTab(index);
    editor->deleteLater();
}
} // namespace fe::editor
