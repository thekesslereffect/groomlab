// SPDX-License-Identifier: Apache-2.0
#pragma once

// The correctives panel (the Groom Tools' Correctives tab), as Maya's CorrectivesBox lays it out
// from the shared definitions (engine/ui/lists.json's correctives), as the redesign lays it out
// under the tool card: the header (Layers, how many, + New and ◆ Fix Frame glyphs; the about line
// its tip), the groom as a stack, as Yeti's layers (the
// correctives top first, the graph's Sculpt nodes as "Node: sculpt1", the Base groom at the
// bottom; the check box turns a corrective on or off, a double-click renames it, a right-click
// opens lists.correctives.context, or nodeContext on a node's row), and the picked row's box (Up,
// Down, Solo, Delete; the corrective's weight with ◆, key it here). The picked row is what the brushes edit: the base groom (the
// guides), a corrective at the timeline's frame (a shot sculpt: the core's correctives through the
// brush session, so a correction on one frame rides along with the motion around it), or a Sculpt
// node's guides (sculpt.h; the document's sculpt target). A corrective's weight is its envelope's,
// or keyed (CorrectiveLayer::weightCurve: Key Weight Here, Clear Keys). Every edit goes to the
// document as one undo step (the correctives text, as a stroke's).
//
// The stack is read again only when the correctives or the graph changed; a new frame updates
// the weights alone (frameChanged), and a hidden panel catches up when it shows (invalidate).

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "definitions.h"

class QFrame;
class QLabel;
class QMenu;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace groomlab {
class Correctives;
}

namespace groomlab::app {

class BrushSession;
class GroomDocument;
class ListBar;
class ListHeader;
class ValueSlider;

class CorrectivesPanel : public QWidget {
    Q_OBJECT
public:
    explicit CorrectivesPanel(QWidget* parent = nullptr);

    void setDocument(GroomDocument* document);  // null: nothing to edit
    void setSession(BrushSession* session);     // whose strokes sculpt into the target layer
    // The window's bindings: the bar and the right-click menus are built with them.
    void setActions(const ui::Bindings& bindings, const ui::FillOptions& options);
    // The stack takes the document's layers and Sculpt nodes again (now); the brush session is
    // told the target and time.
    void refresh();
    void invalidate();     // stale: refreshed on the next turn while shown, else when shown
    void frameChanged();   // the timeline moved: the target's time, the weights shown
    bool stale() const { return stale_; }
    int rebuilds() const { return rebuilds_; }
    std::vector<std::string> layerNames() const;  // in the stack's order (bottom first)
    // The layer strokes sculpt into ("" the base groom: ordinary strokes).
    const std::string& targetLayer() const { return target_; }
    void setTargetLayer(const std::string& name);
    // The Sculpt node strokes shape ("" none: the base groom or a layer): the document's target.
    std::string targetNode() const;
    void setTargetNode(const std::string& node);
    bool clearNodeEdits(const std::string& node = {});  // a Sculpt node's edits dropped (the targeted one's by default; one undo step)
    // The picked row: a corrective's name ("" the base groom, a node or nothing); a node's name.
    std::string currentLayer() const;
    std::string currentNode() const;
    // The right-click menu for the row at `pos` (the tree's viewport coordinates; that row picked
    // first): a corrective's (lists.correctives.context), a Sculpt node's (nodeContext); null on
    // the base groom or nothing. Not shown: the caller pops it up (it deletes itself when closed).
    QMenu* contextMenu(const QPoint& pos);
    void editCurrentName();  // the picked corrective's name edited in place (Rename)

    // The edits, as the bar and menus do them; false (and a message) when refused. A new layer is
    // on every frame (`allFrames`), or full at the document's frame fading over `fadeFrames`
    // frames each side; its name is made unique ("layer", "shot<frame>").
    bool addLayer(bool allFrames, double fadeFrames = 6.0);
    bool removeLayer(const std::string& name);
    bool setLayerWeight(const std::string& name, float weight);  // a keyed layer: keyed at the document's time
    bool setLayerEnabled(const std::string& name, bool enabled);
    bool renameLayer(const std::string& name, const std::string& to);
    bool moveLayer(const std::string& name, int step);  // +1 up the stack (applied later), -1 down
    bool soloLayer(const std::string& name);  // only it on; when it is already the only one on, every one on again
    bool keyWeight(const std::string& name);  // its weight now keyed at the document's time
    bool clearKeys(const std::string& name);  // keys (or a fix's fades) dropped: on every frame at its weight now
    // The frame a corrective is strongest on (its strongest key, a fix's frame); none on every frame.
    std::optional<double> layerFrame(const std::string& name) const;

    QTreeWidget* tree() const { return tree_; }
    ListBar* bar() const { return bar_; }          // the header's: + New, Fix Frame
    ListBar* itemBar() const { return itemBar_; }  // the picked layer's: Up, Down, Solo, Delete

signals:
    void edited();          // the document's correctives changed: the host evaluates and redraws
    void targetChanged();   // the brushes sculpt into another layer
    void message(const QString& text);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void picked(QTreeWidgetItem* item);
    void applyTarget();  // the session's shot sculpt from the target and the document's time
    void refreshWeights();  // the weight column and slider at the document's time
    void selectTargetRow();
    bool editLayers(const std::string& label, const std::function<void(Correctives&)>& edit);

    GroomDocument* document_ = nullptr;
    BrushSession* session_ = nullptr;
    ui::Bindings bindings_;
    ui::FillOptions options_;
    ListHeader* header_ = nullptr;
    ListBar* bar_ = nullptr;
    ListBar* itemBar_ = nullptr;
    QFrame* box_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    ValueSlider* weight_ = nullptr;
    QToolButton* key_ = nullptr;
    QLabel* hint_ = nullptr;
    std::string target_;
    std::string shownKey_;
    std::shared_ptr<Correctives> stack_;  // the stack as last read (the weights read again per frame from it)
    bool updating_ = false;
    bool stale_ = false, pending_ = false;
    int rebuilds_ = 0;
};

}  // namespace groomlab::app
