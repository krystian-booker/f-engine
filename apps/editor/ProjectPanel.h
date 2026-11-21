#pragma once
#include <QWidget>
#include <filesystem>
#include <functional>
class QFileSystemModel;
class QLabel;
class QListView;
class QMenu;
class QModelIndex;
class QTreeView;

namespace fe {
class AssetCatalog;
}

namespace fe::editor {
class Thumbnails;
// The Project browser: the game folder as folders (left) and the chosen
// folder's files (right), with the file operations of a file manager:
// create, rename (F2), move (drag), delete (to the trash). Files drag out
// onto the Scene view.
class ProjectPanel final : public QWidget {
public:
    ProjectPanel(std::filesystem::path gameRoot, AssetCatalog& catalog, QWidget* parent = nullptr);

    // Shows a file's folder and selects the file.
    void reveal(const std::filesystem::path& file);
    // Pictures for the files (see Thumbnails); redraws when one arrives.
    void setThumbnails(Thumbnails* thumbnails);
    // Asks how an image is cut into frames and clips, and writes the
    // .anim.json that makes it a sprite.
    void spriteSheetWizard(const std::filesystem::path& image);

    std::function<void(const std::filesystem::path& file)> onOpen;         // double click
    std::function<void(const std::filesystem::path& file)> onPlaceInScene; // from the menu
    std::function<void()> onNewScene;
    std::function<void()> onFilesChanged; // something was created, renamed, moved or removed

private:
    void contextMenu(const QPoint& at, bool inList);
    void showDetails(const QModelIndex& index);
    [[nodiscard]] std::filesystem::path currentFolder() const;
    [[nodiscard]] std::string relative(const std::filesystem::path& file) const;
    void create(bool folder);

    std::filesystem::path root_;
    AssetCatalog& catalog_;
    QFileSystemModel* folders_ = nullptr;
    QFileSystemModel* files_ = nullptr;
    QTreeView* tree_ = nullptr;
    QListView* list_ = nullptr;
    QLabel* details_ = nullptr;
    Thumbnails* thumbnails_ = nullptr;
};
} // namespace fe::editor
