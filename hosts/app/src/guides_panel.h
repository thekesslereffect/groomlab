// SPDX-License-Identifier: Apache-2.0
#pragma once

// The guide groups list (the Groom Tools' Tools tab: Guide groups, always open), from the shared
// definitions (engine/ui/lists.json's groups), as the redesign lays it out: the header (Guide
// groups, how many, + New Group; the Groom Tools put Isolate's eye there), the groups as plain rows
// (the color dot and name, children under their parents, the guides it holds, the eye that shows or
// hides it; a click picks the group brushes edit, a double-click renames it in place, a right-click
// opens lists.groups.context: its color...), then the picked group's box: Rename, Select, Delete,
// Segments (resample), Edit Guides ▾ (Children, Sync +X → -X and -X → +X, Mirror Selection, Mirror
// Group, Reparent, Promote / Demote, Clear Selection) with the children count beside it.
//
// The edits go through the document, each one undo step; what the bar and menus run is the
// window's (app_actions.h: the prompts over the viewport), which calls the functions below. The
// tree is rebuilt only when what it shows changed, and while the panel is hidden not at all
// (invalidate: a stroke marks it stale; it catches up when it shows or is asked for its group).

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QString>
#include <QWidget>

#include "definitions.h"
#include "groomlab/guide_hierarchy.h"

class QCheckBox;
class QFrame;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

namespace groomlab::app {

class GroomDocument;
class ListBar;
class ListHeader;
class ValueSlider;

class GuidesPanel : public QWidget {
    Q_OBJECT
public:
    explicit GuidesPanel(QWidget* parent = nullptr);

    void setDocument(GroomDocument* document);  // null: nothing to edit
    // The window's bindings: the bar, Edit Guides and the right-click menu are built with them.
    void setActions(const ui::Bindings& bindings, const ui::FillOptions& options);
    // The tree takes the document's groups, selection, colors and hidden groups again (now).
    void refresh();
    // What the tree shows may have changed (a stroke, an undo): refreshed on the next turn of the
    // event loop while the panel shows, else when it shows or is asked for its group.
    void invalidate();
    bool stale() const { return stale_; }
    int rebuilds() const { return rebuilds_; }  // the tree's rebuilds so far (tests: no rebuild without a change)
    // The group the tree has current ("" none); setCurrentGroup picks one.
    std::string currentGroup() const;
    void setCurrentGroup(const std::string& name);
    // The conditions the right-click menu's entries need for the current group: "group" (one is
    // picked), "group.color" (it has a color of its own).
    std::set<std::string> menuState() const;
    // The right-click menu for the row at `pos` (the tree's viewport coordinates; that row made
    // current first), not shown: the caller pops it up (it deletes itself when it closes).
    QMenu* contextMenu(const QPoint& pos);
    void editCurrentName();  // the current group's name edited in place (Rename)
    int childrenCount() const;
    void setChildrenCount(int count);

    // The edits, as the bar and menus do them; false (and a message) when refused.
    bool addGroup(const std::string& parent, const std::string& name = {});  // parent "": a root group
    bool removeGroup(const std::string& name);
    bool renameGroup(const std::string& name, const std::string& to);
    // Children for the selected guides (those of the first one's group), else for every guide of
    // `parent`, into its child group (Maya's Edit Guides > Children).
    bool generateChildren(const std::string& parent, int count);
    bool deleteSelected();
    void selectGroup(const std::string& name);
    void clearSelection();
    // The mirror image of each selected guide selected (core mirrorSelection, across x = 0).
    bool mirrorSelection();
    // A root group mirrored across the plane x = 0 from one side (its guides there copied over the
    // other side's); the whole groom made symmetric the same way (every root group, children with
    // their parents); a group moved one level up, or down under a sibling group; the selected
    // children moved under another parent guide; the selected (else every) guide resampled along
    // its smooth curve to `segments` segments.
    bool mirrorGroup(const std::string& name, bool fromPositive = true);
    bool symmetrySync(bool fromPositive = true);
    bool promoteGroup(const std::string& name);
    bool demoteGroup(const std::string& name, const std::string& under);
    bool reparentSelected(GuideId parent);
    bool resampleGuides(int segments);
    // Every guide of a group back to its segments, those with their own too (frozen ones kept).
    bool resetGroupSegments(const std::string& name);
    // A group's own color ("#rrggbb"; "" its automatic color again).
    bool setGroupColor(const std::string& name, const std::string& color);
    // The root group above a group (itself for a root); the groups beside it (same parent).
    std::string rootOf(const std::string& name) const;
    std::vector<std::string> siblingsOf(const std::string& name) const;
    // The parts the Part brush drew, by name; one removed, every one cleared; the flow cleared.
    std::vector<std::string> partNames() const;
    // The flow arrows and sinks the groom keeps (an unreadable flow: none).
    std::pair<std::size_t, std::size_t> flowCount() const;
    bool removePart(const std::string& name);
    bool clearParts();
    bool clearFlow();
    // The parts and flow box shown (off in the Groom Tools: the Part and Flow tools' cards hold
    // them, as Maya's do).
    void setPartsShown(bool shown);

    // The pieces (tests, screenshots).
    QTreeWidget* tree() const { return tree_; }
    ListHeader* header() const { return header_; }
    ListBar* bar() const { return bar_; }          // the header's: + New Group
    ListBar* itemBar() const { return itemBar_; }  // the picked group's: Rename, Select, Delete
    QFrame* groupBox() const { return box_; }
    QPushButton* editsButton() const { return edits_; }
    QMenu* editsMenu() const { return editsMenu_; }

signals:
    // The document's guides changed through this panel: the host evaluates and redraws.
    void edited();
    // The selection, the hidden groups or the colors changed: the host redraws.
    void displayChanged();
    void message(const QString& text);
    void groupClicked(const std::string& name);  // a row clicked: the brushes' group (Maya's pick_group)

protected:
    void showEvent(QShowEvent* event) override;

private:
    void itemChanged(QTreeWidgetItem* item, int column);
    std::string groupOf(QTreeWidgetItem* item) const;
    void fresh() const;  // refreshed first when stale
    void refreshRows();  // the selection line, the enabled states (cheap)
    bool edit(const char* what, const std::string& label, const std::function<void(GuideHierarchy&)>& change);

    GroomDocument* document_ = nullptr;
    ui::Bindings bindings_;
    ui::FillOptions options_;
    bool updating_ = false;
    bool stale_ = false, pending_ = false;
    int rebuilds_ = 0;
    std::string shownKey_;  // what the tree was built from (rebuilt only when it changes)
    ListHeader* header_ = nullptr;
    ListBar* bar_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QFrame* box_ = nullptr;
    QLabel* empty_ = nullptr;  // no groups yet
    ListBar* itemBar_ = nullptr;
    QLabel* selection_ = nullptr;
    QPushButton* edits_ = nullptr;
    QMenu* editsMenu_ = nullptr;
    QSpinBox* count_ = nullptr;
    ValueSlider* segments_ = nullptr;
    QWidget* overlays_ = nullptr;  // the parts and flow box
    QListWidget* parts_ = nullptr;
    QLabel* flow_ = nullptr;
    QPushButton* removePart_ = nullptr;
    QPushButton* clearParts_ = nullptr;
    QPushButton* clearFlow_ = nullptr;
};

}  // namespace groomlab::app
