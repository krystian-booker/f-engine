#include "apps/editor/ProjectPanel.h"
#include "apps/editor/Thumbnails.h"
#include "engine/assets/SpriteSheetBuilder.h"
#include "engine/core/Json.h"
#include "engine/editor/AssetCatalog.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace fe::editor {
namespace {
QString qpath(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

// Shows an asset's picture in place of its file type's icon.
class ThumbnailDelegate final : public QStyledItemDelegate {
public:
    ThumbnailDelegate(QFileSystemModel* model, Thumbnails* thumbnails, QObject* parent)
        : QStyledItemDelegate(parent), model_(model), thumbnails_(thumbnails) {}

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (thumbnails_ && !model_->isDir(index)) {
            const QIcon picture =
                thumbnails_->icon(std::filesystem::path(model_->filePath(index).toStdString()));
            if (!picture.isNull()) {
                option->icon = picture;
            }
        }
    }

private:
    QFileSystemModel* model_;
    Thumbnails* thumbnails_;
};
} // namespace

ProjectPanel::ProjectPanel(std::filesystem::path gameRoot, AssetCatalog& catalog, QWidget* parent)
    : QWidget(parent), root_(std::move(gameRoot)), catalog_(catalog) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    auto* split = new QSplitter(Qt::Horizontal, this);
    column->addWidget(split, 1);

    folders_ = new QFileSystemModel(this);
    folders_->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    folders_->setReadOnly(false);
    folders_->setRootPath(qpath(root_));
    tree_ = new QTreeView(split);
    tree_->setObjectName(QStringLiteral("projectFolders"));
    tree_->setModel(folders_);
    tree_->setRootIndex(folders_->index(qpath(root_)));
    tree_->setHeaderHidden(true);
    for (int i = 1; i < folders_->columnCount(); ++i) {
        tree_->hideColumn(i);
    }
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setDragDropMode(QAbstractItemView::DragDrop);
    tree_->setDefaultDropAction(Qt::MoveAction);

    files_ = new QFileSystemModel(this);
    files_->setFilter(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    files_->setReadOnly(false);
    files_->setRootPath(qpath(root_));
    list_ = new QListView(split);
    list_->setObjectName(QStringLiteral("projectFiles"));
    list_->setModel(files_);
    list_->setRootIndex(files_->index(qpath(root_)));
    list_->setViewMode(QListView::IconMode);
    list_->setResizeMode(QListView::Adjust);
    list_->setIconSize(QSize(64, 64));
    list_->setGridSize(QSize(112, 104));
    list_->setWordWrap(true);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setDragDropMode(QAbstractItemView::DragDrop);
    list_->setDefaultDropAction(Qt::MoveAction);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

    details_ = new QLabel(this);
    details_->setObjectName(QStringLiteral("projectDetails"));
    details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details_->setContentsMargins(6, 3, 6, 3);
    column->addWidget(details_);

    QObject::connect(
        tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& index) { list_->setRootIndex(files_->index(folders_->filePath(index))); });
    QObject::connect(list_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        if (files_->isDir(index)) {
            const auto folder = files_->filePath(index);
            list_->setRootIndex(files_->index(folder));
            tree_->setCurrentIndex(folders_->index(folder));
        } else if (onOpen) {
            onOpen(std::filesystem::path(files_->filePath(index).toStdString()));
        }
    });
    QObject::connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     [this](const QModelIndex& index) { showDetails(index); });
    QObject::connect(tree_, &QTreeView::customContextMenuRequested, this,
                     [this](const QPoint& at) { contextMenu(at, false); });
    QObject::connect(list_, &QListView::customContextMenuRequested, this,
                     [this](const QPoint& at) { contextMenu(at, true); });
    const auto changed = [this] {
        if (onFilesChanged) {
            onFilesChanged();
        }
    };
    QObject::connect(files_, &QFileSystemModel::fileRenamed, this, changed);
    QObject::connect(files_, &QFileSystemModel::rowsRemoved, this, changed);
    QObject::connect(files_, &QFileSystemModel::rowsInserted, this, changed);
}

void ProjectPanel::setThumbnails(Thumbnails* thumbnails) {
    thumbnails_ = thumbnails;
    list_->setItemDelegate(new ThumbnailDelegate(files_, thumbnails, list_));
    if (thumbnails) {
        thumbnails->onReady = [this] {
            list_->viewport()->update();
        };
    }
}

void ProjectPanel::spriteSheetWizard(const std::filesystem::path& image) {
    const QSize size = QImageReader(qpath(image)).size();
    if (!size.isValid()) {
        QMessageBox::warning(this, tr("Sprite Sheet"),
                             tr("%1 is not an image the editor can read").arg(qpath(image)));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(
        tr("Sprite Sheet: %1 (%2 x %3)").arg(qpath(image.filename())).arg(size.width()).arg(size.height()));
    dialog.resize(560, 460);
    auto* form = new QFormLayout(&dialog);
    const auto number = [&](int value, int low, int high) {
        auto* box = new QSpinBox(&dialog);
        box->setRange(low, high);
        box->setValue(value);
        return box;
    };
    auto* width = number(std::min(32, size.width()), 1, size.width());
    auto* height = number(std::min(48, size.height()), 1, size.height());
    auto* directions = new QComboBox(&dialog);
    directions->addItems({QStringLiteral("1"), QStringLiteral("4"), QStringLiteral("8")});
    directions->setCurrentIndex(1);
    auto* unit = number(32, 1, 1024);
    form->addRow(tr("Frame width (pixels)"), width);
    form->addRow(tr("Frame height (pixels)"), height);
    form->addRow(tr("Rows per clip (facing directions)"), directions);
    form->addRow(tr("Pixels per world unit"), unit);
    auto* clips = new QTableWidget(0, 4, &dialog);
    clips->setHorizontalHeaderLabels(
        {tr("Clip"), tr("Frames (columns, e.g. 0-3 or 0,1,2,1)"), tr("Seconds per frame"), tr("Loops")});
    clips->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    const auto addClip = [clips](const QString& name, const QString& frames) {
        const int row = clips->rowCount();
        clips->insertRow(row);
        clips->setItem(row, 0, new QTableWidgetItem(name));
        clips->setItem(row, 1, new QTableWidgetItem(frames));
        clips->setItem(row, 2, new QTableWidgetItem(QStringLiteral("0.15")));
        auto* loops = new QTableWidgetItem;
        loops->setCheckState(Qt::Checked);
        clips->setItem(row, 3, loops);
    };
    addClip(QStringLiteral("idle"), QStringLiteral("0"));
    addClip(QStringLiteral("walk"), QStringLiteral("0-3"));
    form->addRow(clips);
    auto* more = new QPushButton(tr("Add Clip"), &dialog);
    QObject::connect(more, &QPushButton::clicked, &dialog,
                     [addClip] { addClip(QStringLiteral("clip"), QStringLiteral("0")); });
    form->addRow(more);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    SheetSpec spec;
    spec.atlas = image.filename().string();
    spec.frameWidth = width->value();
    spec.frameHeight = height->value();
    spec.directions = directions->currentText().toInt();
    spec.pixelsPerUnit = unit->value();
    for (int row = 0; row < clips->rowCount(); ++row) {
        SheetClip clip;
        clip.name = clips->item(row, 0)->text().trimmed().toStdString();
        if (clip.name.empty()) {
            continue;
        }
        // "0-3" is a range, "0,1,2,1" a list; both may be mixed.
        for (const auto& part : clips->item(row, 1)->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const auto ends = part.trimmed().split(QLatin1Char('-'));
            const int from = ends.first().toInt(), to = ends.last().toInt();
            for (int frame = from; frame <= std::max(from, to) && clip.frames.size() < 512; ++frame) {
                clip.frames.push_back(frame);
            }
        }
        clip.duration = std::max(0.01, clips->item(row, 2)->text().toDouble());
        clip.loop = clips->item(row, 3)->checkState() == Qt::Checked;
        clip.rowOffset = row * spec.directions;
        spec.clips.push_back(std::move(clip));
    }
    try {
        auto target = image;
        target.replace_extension(".anim.json");
        writeJson(target, buildSheetAnimation(spec, size.width(), size.height()));
        if (onOpen) {
            onOpen(target);
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Sprite Sheet"), QString::fromUtf8(error.what()));
    }
}

std::filesystem::path ProjectPanel::currentFolder() const {
    return std::filesystem::path(files_->filePath(list_->rootIndex()).toStdString());
}

std::string ProjectPanel::relative(const std::filesystem::path& file) const {
    std::error_code error;
    const auto path = std::filesystem::relative(file, root_, error);
    return error ? file.generic_string() : path.generic_string();
}

void ProjectPanel::reveal(const std::filesystem::path& file) {
    const auto folder = qpath(file.parent_path());
    tree_->setCurrentIndex(folders_->index(folder));
    list_->setRootIndex(files_->index(folder));
    const auto index = files_->index(qpath(file));
    list_->setCurrentIndex(index);
    list_->scrollTo(index);
}

void ProjectPanel::showDetails(const QModelIndex& index) {
    if (!index.isValid() || files_->isDir(index)) {
        details_->clear();
        return;
    }
    const std::filesystem::path file(files_->filePath(index).toStdString());
    const auto path = relative(file);
    QString text =
        QStringLiteral("%1  •  %2  •  %3 KB")
            .arg(QString::fromStdString(path), QString::fromLatin1(assetKindName(assetKindOf(path))))
            .arg((files_->size(index) + 1023) / 1024);
    const auto& references = catalog_.references(path);
    if (!references.empty()) {
        QStringList users;
        for (std::size_t i = 0; i < references.size() && i < 6; ++i) {
            users << QString::fromStdString(references[i].file + " (" + references[i].where + ")");
        }
        if (references.size() > 6) {
            users << tr("and %1 more").arg(references.size() - 6);
        }
        text += tr("\nUsed by: %1").arg(users.join(QStringLiteral(", ")));
    } else {
        text += tr("\nNot referenced by any scene or data file");
    }
    details_->setText(text);
}

void ProjectPanel::create(bool folder) {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, folder ? tr("New Folder") : tr("New Lua Script"),
                              folder ? tr("Folder name") : tr("Script name"), QLineEdit::Normal,
                              folder ? tr("folder") : tr("script.lua"), &ok)
            .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    std::filesystem::path target = currentFolder() / name.toStdString();
    if (!folder && target.extension().empty()) {
        target += ".lua";
    }
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
        QMessageBox::warning(this, tr("Project"), tr("%1 exists already").arg(qpath(target.filename())));
        return;
    }
    if (folder) {
        std::filesystem::create_directories(target, error);
    } else {
        QFile made(qpath(target));
        if (made.open(QIODevice::WriteOnly)) {
            const auto module = target.stem().string();
            made.write(("-- " + module + "\nlocal " + module + " = {}\n\nreturn " + module + "\n").c_str());
        }
    }
    if (!folder && onOpen) {
        onOpen(target);
    }
}

void ProjectPanel::contextMenu(const QPoint& at, bool inList) {
    QMenu menu(this);
    const QModelIndex index = inList ? list_->indexAt(at) : QModelIndex();
    std::filesystem::path file;
    if (inList && index.isValid()) {
        file = std::filesystem::path(files_->filePath(index).toStdString());
    } else if (!inList && tree_->indexAt(at).isValid()) {
        const auto folder = folders_->filePath(tree_->indexAt(at));
        tree_->setCurrentIndex(tree_->indexAt(at));
        list_->setRootIndex(files_->index(folder));
    }
    auto* create = menu.addMenu(tr("Create"));
    create->addAction(tr("Folder"), this, [this] { this->create(true); });
    create->addAction(tr("Lua Script"), this, [this] { this->create(false); });
    create->addAction(tr("Scene..."), this, [this] {
        if (onNewScene) {
            onNewScene();
        }
    });
    if (!file.empty()) {
        const bool isFile = !files_->isDir(index);
        menu.addSeparator();
        if (isFile) {
            menu.addAction(tr("Open"), this, [this, file] {
                if (onOpen) {
                    onOpen(file);
                }
            });
            const auto kind = assetKindOf(relative(file));
            if (kind == AssetKind::Model || kind == AssetKind::Sprite) {
                menu.addAction(tr("Place in Scene"), this, [this, file] {
                    if (onPlaceInScene) {
                        onPlaceInScene(file);
                    }
                });
            }
            if (file.extension() == ".png") {
                menu.addAction(tr("Make Sprite Sheet..."), this, [this, file] { spriteSheetWizard(file); });
            }
            menu.addAction(tr("Open in External Program"), this,
                           [file] { QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(file))); });
        }
        menu.addAction(tr("Rename"), this, [this, index] { list_->edit(index); });
        menu.addAction(tr("Delete"), this, [this, file] {
            const auto path = relative(file);
            const auto& references = catalog_.references(path);
            QString question = tr("Move %1 to the trash?").arg(QString::fromStdString(path));
            if (!references.empty()) {
                question += tr("\n\nIt is used in %1 place(s), for example %2.")
                                .arg(references.size())
                                .arg(QString::fromStdString(references.front().file));
            }
            if (QMessageBox::question(this, tr("Delete"), question) != QMessageBox::Yes) {
                return;
            }
            if (!QFile::moveToTrash(qpath(file))) {
                QMessageBox::warning(this, tr("Delete"),
                                     tr("Could not move %1 to the trash").arg(qpath(file)));
            }
        });
        menu.addAction(tr("Copy Path"), this, [this, file] {
            QApplication::clipboard()->setText(QString::fromStdString(relative(file)));
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Show in File Manager"), this, [this, file] {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(qpath(file.empty() ? currentFolder() : file.parent_path())));
    });
    menu.exec((inList ? list_->viewport() : tree_->viewport())->mapToGlobal(at));
}
} // namespace fe::editor
