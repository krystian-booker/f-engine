#include "apps/editor/HierarchyPanel.h"
#include "engine/editor/EditorSession.h"
#include "engine/world/Components.h"
#include "engine/world/World.h"
#include <QLineEdit>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

namespace fe::editor {
namespace {
constexpr int RowRole = Qt::UserRole + 1;   // index into rows_, -1 for headings
constexpr int WorldRole = Qt::UserRole + 2; // a world's name on its heading
} // namespace

HierarchyPanel::HierarchyPanel(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("hierarchyFilter"));
    filter_->setPlaceholderText(tr("Search"));
    filter_->setClearButtonEnabled(true);
    column->addWidget(filter_);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("hierarchyTree"));
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    column->addWidget(tree_);

    QObject::connect(filter_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    QObject::connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (syncing_) {
            return;
        }
        std::vector<Selection> selected;
        for (const auto* chosen : tree_->selectedItems()) {
            if (const auto selection = selectionOf(chosen); !selection.empty()) {
                selected.push_back(selection);
            }
        }
        // The row clicked last is the primary: the session takes the last one.
        if (const auto current = selectionOf(tree_->currentItem()); !current.empty()) {
            std::erase(selected, current);
            selected.push_back(current);
        }
        if (onSelected) {
            onSelected(selected);
        }
    });
    QObject::connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* chosen) {
        if (const auto selection = selectionOf(chosen); !selection.empty() && onActivated) {
            onActivated(selection);
        }
    });
    QObject::connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* chosen) {
        const auto world = chosen->data(0, WorldRole).toString();
        if (!world.isEmpty() && onWorldChosen) {
            onWorldChosen(world.toStdString());
        }
    });
    QObject::connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        if (!onContextMenu) {
            return;
        }
        QMenu menu(this);
        onContextMenu(menu, selectionOf(tree_->itemAt(at)));
        if (!menu.isEmpty()) {
            menu.exec(tree_->viewport()->mapToGlobal(at));
        }
    });
}

Selection HierarchyPanel::selectionOf(const QTreeWidgetItem* chosen) const {
    if (!chosen) {
        return {};
    }
    const int index = chosen->data(0, RowRole).toInt() - 1;
    return index >= 0 && index < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(index)]
                                                                : Selection{};
}

QTreeWidgetItem* HierarchyPanel::row(QTreeWidgetItem* parent, const QString& label,
                                     const Selection& selection) {
    auto* made = new QTreeWidgetItem(parent, QStringList{label});
    rows_.push_back(selection);
    made->setData(0, RowRole, static_cast<int>(rows_.size()));
    return made;
}

QTreeWidgetItem* HierarchyPanel::section(const QString& name, std::size_t count) {
    auto* made = new QTreeWidgetItem(tree_, QStringList{QStringLiteral("%1 (%2)").arg(name).arg(count)});
    made->setFlags(made->flags() & ~Qt::ItemIsSelectable);
    QFont bold = made->font(0);
    bold.setBold(true);
    made->setFont(0, bold);
    made->setExpanded(true);
    return made;
}

void HierarchyPanel::showScene(EditorSession& session) {
    syncing_ = true;
    tree_->clear();
    rows_.clear();
    if (const auto* map = session.map()) {
        auto* points = section(tr("Points"), map->points.size());
        for (const auto& [name, point] : map->points) {
            row(points, QString::fromStdString(name), Selection::point(name));
        }
        // Placements by type, as the game names them.
        std::map<std::string, std::vector<const Placement*>> byType;
        for (const auto& placement : map->placements) {
            byType[placement.type].push_back(&placement);
        }
        auto* placements = section(tr("Placements"), map->placements.size());
        for (const auto& [type, members] : byType) {
            auto* group = new QTreeWidgetItem(
                placements,
                QStringList{QStringLiteral("%1 (%2)").arg(QString::fromStdString(type)).arg(members.size())});
            group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
            group->setExpanded(members.size() <= 32);
            for (const auto* placement : members) {
                row(group, QString::fromStdString(placement->id), Selection::placement(placement->id));
            }
        }
        auto* zones = section(tr("Zones"), map->zones.size());
        for (const auto& zone : map->zones) {
            row(zones,
                QStringLiteral("%1  [%2]")
                    .arg(QString::fromStdString(zone.id), QString::fromStdString(zone.type)),
                Selection::zone(zone.id));
        }
        auto* lights = section(tr("Lights"), map->lights.size());
        for (std::size_t i = 0; i < map->lights.size(); ++i) {
            row(lights, tr("Light %1").arg(i + 1), Selection::light(static_cast<int>(i)));
        }
        auto* emitters = section(tr("Emitters"), map->emitters.size());
        for (std::size_t i = 0; i < map->emitters.size(); ++i) {
            row(emitters, tr("Emitter %1").arg(i + 1), Selection::emitter(static_cast<int>(i)));
        }
        auto* colliders = section(tr("Colliders"), map->colliders.size());
        for (std::size_t i = 0; i < map->colliders.size(); ++i) {
            row(colliders, tr("Collider %1").arg(i + 1), Selection::collider(static_cast<int>(i)));
        }
    }
    syncing_ = false;
    applyFilter();
    setSelected(session.selected());
}

void HierarchyPanel::showEntities(const std::shared_ptr<World>& world,
                                  const std::vector<std::string>& worlds) {
    syncing_ = true;
    tree_->clear();
    rows_.clear();
    for (const auto& name : worlds) {
        const bool shown = world && world->name() == name;
        auto* heading = new QTreeWidgetItem(tree_, QStringList{QString::fromStdString(name)});
        heading->setFlags(heading->flags() & ~Qt::ItemIsSelectable);
        heading->setData(0, WorldRole, QString::fromStdString(name));
        QFont bold = heading->font(0);
        bold.setBold(shown);
        heading->setFont(0, bold);
        if (!shown) {
            continue;
        }
        heading->setExpanded(true);
        auto* authored = new QTreeWidgetItem(heading, QStringList{tr("From the scene")});
        auto* spawned = new QTreeWidgetItem(heading, QStringList{tr("Spawned at run time")});
        for (auto* group : {authored, spawned}) {
            group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
            group->setExpanded(true);
        }
        const auto& registry = world->registry();
        for (const auto entity : registry.view<const Name>()) {
            const auto& id = registry.get<const Name>(entity).id;
            const bool placed = !entityPlacement(*world, entity).empty();
            QString label = QString::fromStdString(id);
            if (label.isEmpty()) {
                label = tr("entity %1").arg(static_cast<quint32>(entt::to_integral(entity)));
            }
            row(placed ? authored : spawned, label, Selection::ofEntity(*world, entity));
        }
        authored->setText(0, tr("From the scene (%1)").arg(authored->childCount()));
        spawned->setText(0, tr("Spawned at run time (%1)").arg(spawned->childCount()));
    }
    syncing_ = false;
    applyFilter();
}

void HierarchyPanel::setSelected(const std::vector<Selection>& selected) {
    syncing_ = true;
    tree_->clearSelection();
    QTreeWidgetItemIterator each(tree_);
    QTreeWidgetItem* last = nullptr;
    for (; *each; ++each) {
        const auto selection = selectionOf(*each);
        if (!selection.empty() && std::find(selected.begin(), selected.end(), selection) != selected.end()) {
            (*each)->setSelected(true);
            if (!selected.empty() && selection == selected.back()) {
                last = *each;
            }
        }
    }
    if (last) {
        tree_->setCurrentItem(last, 0, QItemSelectionModel::NoUpdate);
        tree_->scrollToItem(last);
    }
    syncing_ = false;
}

void HierarchyPanel::applyFilter() {
    const QString needle = filter_->text().trimmed();
    QTreeWidgetItemIterator each(tree_);
    for (; *each; ++each) {
        auto* item = *each;
        if (item->data(0, RowRole).toInt() == 0) {
            continue; // headings stay
        }
        item->setHidden(!needle.isEmpty() && !item->text(0).contains(needle, Qt::CaseInsensitive));
    }
}
} // namespace fe::editor
