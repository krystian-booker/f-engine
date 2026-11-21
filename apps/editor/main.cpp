// fengine_editor: the standalone editor application.
//
//   fengine_editor <game folder> [--map id] [--shaders dir] [--user-data dir]
//                  [--mute] [--frames n] [--shot file.png] [--redock] [--selftest]
//
// The last four are for self-check runs: quit after n frames (exit code 1
// when the Scene view never drew), write the Scene view to a PNG, float and
// re-dock the Scene view on the way, and drive an editing and a Play session
// (nothing is saved).
#include "apps/editor/EditorWindow.h"
#include "engine/core/Log.h"
#include "engine/runtime/Headless.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <cstdio>
#include <stdexcept>

namespace {
std::filesystem::path path(const QString& text) {
    return std::filesystem::path(text.toStdString());
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("fengine-editor"));
    QCoreApplication::setOrganizationName(QStringLiteral("fengine"));

    fe::editor::EditorOptions options;
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        const auto value = [&]() -> QString {
            return i + 1 < args.size() ? args[++i] : QString();
        };
        if (args[i] == QLatin1String("--map")) {
            options.map = value().toStdString();
        } else if (args[i] == QLatin1String("--shaders")) {
            options.shaders = path(value());
        } else if (args[i] == QLatin1String("--user-data")) {
            options.userData = path(value());
        } else if (args[i] == QLatin1String("--frames")) {
            options.frames = value().toInt();
        } else if (args[i] == QLatin1String("--shot")) {
            options.shot = path(value());
        } else if (args[i] == QLatin1String("--redock")) {
            options.redock = true;
        } else if (args[i] == QLatin1String("--selftest")) {
            options.selfTest = true;
        } else if (args[i] == QLatin1String("--mute")) {
            options.mute = true;
        } else if (!args[i].startsWith(QLatin1Char('-'))) {
            options.game = path(args[i]);
        } else {
            std::fprintf(stderr, "Unknown option %s\n", qPrintable(args[i]));
            return 2;
        }
    }
    if (options.game.empty()) {
        // No project named: the last one, another folder, or a new project.
        QSettings settings;
        const QString last = settings.value(QStringLiteral("lastProject")).toString();
        QMessageBox ask(
            QMessageBox::Question, QStringLiteral("fengine editor"),
            QObject::tr("Which project?\n\nA project is a game folder: the one holding game.json."));
        QAbstractButton* reopen = nullptr;
        if (!last.isEmpty() && QFile::exists(last + QStringLiteral("/game.json"))) {
            reopen =
                ask.addButton(QObject::tr("Reopen %1").arg(QDir(last).dirName()), QMessageBox::AcceptRole);
        }
        auto* open = ask.addButton(QObject::tr("Open Project..."), QMessageBox::ActionRole);
        auto* create = ask.addButton(QObject::tr("New Project..."), QMessageBox::ActionRole);
        ask.addButton(QMessageBox::Cancel);
        ask.exec();
        if (reopen && ask.clickedButton() == reopen) {
            options.game = path(last);
        } else if (ask.clickedButton() == open) {
            options.game =
                path(QFileDialog::getExistingDirectory(nullptr, QObject::tr("Open a game folder")));
        } else if (ask.clickedButton() == create) {
            options.game = fe::editor::createProject(nullptr);
        }
        if (options.game.empty()) {
            return 0;
        }
    }

    try {
        const auto base = path(QCoreApplication::applicationDirPath());
        if (options.shaders.empty()) {
            options.shaders = std::filesystem::is_directory(base / "shaders")
                                  ? base / "shaders"
                                  : std::filesystem::path(FENGINE_SHADER_DIR);
        }
        options.builtin = fe::findBuiltinRoot(base / "fengine_editor");
        options.game = std::filesystem::absolute(options.game);
        if (!std::filesystem::is_regular_file(options.game / "game.json")) {
            throw std::runtime_error(options.game.string() + " is not a game folder (it has no game.json)");
        }
        if (options.frames == 0) {
            QSettings().setValue(QStringLiteral("lastProject"),
                                 QString::fromStdString(options.game.string()));
        }
        if (options.userData.empty()) {
            // The edit engine's own folder, apart from the game's saves.
            options.userData = path(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)) /
                               "projects" / options.game.filename();
        }
        std::filesystem::create_directories(options.userData);
        if (options.selfTest) {
            // The self-test saves, bakes and records: on a copy of the game,
            // never on the game itself.
            const auto copy = options.userData / "selftest-project" / options.game.filename();
            std::filesystem::remove_all(copy);
            std::filesystem::create_directories(copy.parent_path());
            std::filesystem::copy(options.game, copy, std::filesystem::copy_options::recursive);
            options.game = copy;
        }
        const bool selfCheck = options.frames > 0;
        fe::editor::EditorWindow window(std::move(options));
        window.show();
        const int code = QApplication::exec();
        if (selfCheck) {
            const bool passed = window.selfCheckPassed();
            std::printf("%s\n", passed ? "EDITOR SELF-CHECK PASS" : "EDITOR SELF-CHECK FAIL");
            return passed ? code : 1;
        }
        return code;
    } catch (const std::exception& error) {
        fe::log(fe::LogLevel::Error, error.what());
        if (options.frames == 0) {
            QMessageBox::critical(nullptr, QStringLiteral("fengine editor"), QString::fromUtf8(error.what()));
        }
        return 1;
    }
}
