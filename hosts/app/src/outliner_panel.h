// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Groom Tools' Outliner tab (the app's own: a document holds several grooms, each on its own
// scalp, as a Maya scene or a .blend does): the scalps, each with the grooms on it. A click makes a
// groom the one every tool and panel works on; the box shows or hides another groom's hair; a
// double-click renames; the right-click menu removes one; Add Groom grows another on a mesh. The
// window fills it (MainWindow::refreshOutliner) and does what its signals ask.

#include <vector>

#include <QString>
#include <QWidget>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace groomlab::app {

class OutlinerPanel : public QWidget {
    Q_OBJECT
public:
    // One groom, in the document's order: its name, its scalp's (grooms on the same mesh share a
    // scalp row), whether it is the active one, shown, or has unsaved edits.
    struct Entry {
        QString name;
        QString scalp;
        bool active = false;
        bool visible = true;
        bool dirty = false;
    };
    explicit OutlinerPanel(QWidget* parent = nullptr);

    void setEntries(const std::vector<Entry>& entries);
    const std::vector<Entry>& entries() const { return entries_; }
    QTreeWidget* tree() const { return tree_; }
    QPushButton* addButton() const { return add_; }
    // The groom row of document index `index` (null: none).
    QTreeWidgetItem* groomItem(int index) const;

signals:
    void groomActivated(int index);
    void visibilityChanged(int index, bool visible);
    void renameRequested(int index, const QString& name);
    void removeRequested(int index);
    void addRequested();

private:
    void showMenu(const QPoint& pos);
    QTreeWidget* tree_ = nullptr;
    QPushButton* add_ = nullptr;
    std::vector<Entry> entries_;
    bool filling_ = false;  // (the rows' own changes while they are made: no signals)
};

}  // namespace groomlab::app
