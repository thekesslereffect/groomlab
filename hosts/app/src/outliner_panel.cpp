// SPDX-License-Identifier: Apache-2.0
#include "outliner_panel.h"

#include <map>

#include <QFont>
#include <QHeaderView>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace groomlab::app {

namespace {

constexpr int kIndexRole = Qt::UserRole + 1;  // a groom row's document index (-1: a scalp row)

}  // namespace

OutlinerPanel::OutlinerPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("outlinerPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    add_ = new QPushButton(tr("Add Groom..."), this);
    add_->setObjectName(QStringLiteral("outlinerAdd"));
    add_->setToolTip(tr("Another groom in this document: on a mesh file (.obj or .abc), or the grooms of a groom file (.groom, "
                        "saved by Maya, Blender or the app), each with its own scalp, graph, guides and maps"));
    layout->addWidget(add_);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("outlinerTree"));
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(1);
    tree_->setRootIsDecorated(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->setMinimumHeight(120);
    layout->addWidget(tree_, 1);

    connect(add_, &QPushButton::clicked, this, &OutlinerPanel::addRequested);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        const int index = item ? item->data(0, kIndexRole).toInt() : -1;
        if (index >= 0 && !entries_.at(static_cast<std::size_t>(index)).active) emit groomActivated(index);
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (filling_ || !item) return;
        const int index = item->data(0, kIndexRole).toInt();
        if (index < 0) return;
        const Entry& e = entries_.at(static_cast<std::size_t>(index));
        const bool visible = item->checkState(0) == Qt::Checked;
        if (visible != e.visible) {
            emit visibilityChanged(index, visible);
            return;
        }
        const QString name = item->text(0).trimmed();
        if (!name.isEmpty() && name != e.name) emit renameRequested(index, name);
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &OutlinerPanel::showMenu);
}

void OutlinerPanel::setEntries(const std::vector<Entry>& entries) {
    filling_ = true;
    entries_ = entries;
    tree_->clear();
    std::map<QString, QTreeWidgetItem*> scalps;  // grooms on the same mesh under one scalp row
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        QTreeWidgetItem*& scalp = scalps[e.scalp];
        if (!scalp) {
            scalp = new QTreeWidgetItem(tree_, {e.scalp});
            scalp->setData(0, kIndexRole, -1);
            scalp->setFlags(Qt::ItemIsEnabled);
            scalp->setToolTip(0, tr("The scalp mesh %1: the grooms under it grow on it").arg(e.scalp));
        }
        auto* groom = new QTreeWidgetItem(scalp, {e.name});
        groom->setData(0, kIndexRole, static_cast<int>(i));
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
        // Another groom's box shows or hides its hair (the active one's: the Groom Tools' eye).
        if (!e.active) flags |= Qt::ItemIsUserCheckable;
        groom->setFlags(flags);
        if (!e.active) groom->setCheckState(0, e.visible ? Qt::Checked : Qt::Unchecked);
        QFont font = groom->font(0);
        font.setBold(e.active);
        font.setItalic(!e.active && !e.visible);
        groom->setFont(0, font);
        groom->setToolTip(0, e.active ? tr("%1: the groom every tool works on%2").arg(e.name, e.dirty ? tr(" (unsaved edits)") : QString())
                                      : tr("%1: click to work on it; the box shows or hides it").arg(e.name));
        if (e.active) tree_->setCurrentItem(groom);
    }
    tree_->expandAll();
    filling_ = false;
}

QTreeWidgetItem* OutlinerPanel::groomItem(int index) const {
    for (int s = 0; s < tree_->topLevelItemCount(); ++s) {
        QTreeWidgetItem* scalp = tree_->topLevelItem(s);
        for (int g = 0; g < scalp->childCount(); ++g) {
            if (scalp->child(g)->data(0, kIndexRole).toInt() == index) return scalp->child(g);
        }
    }
    return nullptr;
}

void OutlinerPanel::showMenu(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    const int index = item ? item->data(0, kIndexRole).toInt() : -1;
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (index >= 0) {
        menu->addAction(tr("Work on It"), this, [this, index] { emit groomActivated(index); })
            ->setEnabled(!entries_.at(static_cast<std::size_t>(index)).active);
        menu->addAction(tr("Rename"), this, [this, item] { tree_->editItem(item, 0); });
        QAction* remove = menu->addAction(tr("Remove Groom"), this, [this, index] { emit removeRequested(index); });
        remove->setEnabled(entries_.size() > 1);
        remove->setToolTip(tr("Take the groom out of the document (undo cannot bring it back; its files stay on disk)"));
        menu->addSeparator();
    }
    menu->addAction(tr("Add Groom..."), this, &OutlinerPanel::addRequested);
    menu->popup(tree_->viewport()->mapToGlobal(pos));
}

}  // namespace groomlab::app
