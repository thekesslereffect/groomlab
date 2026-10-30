// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Groom Tools dock, as the redesign lays it out (research/ui-redesign/QT.md, "Groom Tools
// layout"; Maya's palette.py and Blender's Groom Tools panel follow the same shared definitions,
// engine/ui/palette.json through definitions.h). One model on every tab: pick a tool, its card
// right under the grid, then the list of things the tab works on with the picked one's own
// settings under it.
//
//   pinned        (outside the scroll area)
//     header      the groom's name, the eyes (its hair, its guides), the Graph Editor, ⋯ More
//                 (palette.header.more: New Groom on Mesh, Theme)
//     status      a line that takes no room while it has nothing to say
//     tabs        palette.tabs: Tools, Paint, Correctives, Photos
//     the grid    the tool tiles at a fixed size, as many to a row as the width takes (Tools and
//                 Correctives: Create, Shape, Trim, Guides, Unfreeze All beside Freeze; Paint: the
//                 Paint tile)
//   scrolls
//     no groom    palette.empty: Start a groom (New Groom on Mesh, Import ▾)
//     the card    the tool's card (brush_panel.h; the tubes' blocks, tube_panel.h, inside it); the
//                 Paint tab's is Paint's; none on Photos. With no tool: one muted line
//     the list    Tools: Guide groups (the guides panel, always open; Isolate is the eye in its
//                 header); Paint: Maps (the maps panel); Correctives: Layers (the correctives
//                 panel); Photos: the photos panel
//
// palette.output (Maya's and Blender's Output strip) is not the app's: Export ▾ and Render ▾ sit in
// the menu bar's corner (MainWindow), the rest in the GroomLab menu; the first-groom steps float
// over the viewport (overlay.h). How the strands draw while sculpting is Properties > Sculpting.
//
// The window (MainWindow) owns what the tools run: the dock asks (toolChosen, the signals below),
// the window does it and tells the dock back (setTool, setGroups, refresh).

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "card_parts.h"
#include "definitions.h"

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;
class QScrollArea;
class QTabBar;
class QToolButton;
class QVBoxLayout;

namespace groomlab::app {

class BrushPanel;
class BrushSession;
class GuidesPanel;
class TubePanel;

// ---- ToolGrid --------------------------------------------------------------------------------------
// Tiles at a fixed size (the largest button's minimum), left-aligned and never stretched: as many to
// a row as fit, (width + spacing) / (tile + spacing), at most `maxColumns`.
class ToolGrid : public QWidget {
    Q_OBJECT
public:
    explicit ToolGrid(QWidget* parent = nullptr);
    void setButtons(std::vector<QAbstractButton*> buttons, int maxColumns);
    void setMaxColumns(int maxColumns);
    int columnsFor(int width) const;
    int spacing() const { return spacing_; }
    QSize tile() const;  // the widest and tallest button's minimum
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    void place();
    std::vector<QAbstractButton*> buttons_;
    int maxColumns_ = 64;
    int spacing_ = 4;
};

// ---- GroomTools ------------------------------------------------------------------------------------
class GroomTools : public QWidget {
    Q_OBJECT
public:
    explicit GroomTools(QWidget* parent = nullptr);

    // ---- What it works on ----------------------------------------------------------------------
    void setSession(BrushSession* session);
    // The window's bindings (app_actions.h): the More menu, the empty state's buttons and the Parts
    // block's buttons are filled from the definitions with them (ids without a binding hidden, as in
    // the menus).
    void setBindings(const ui::Bindings& bindings, const ui::FillOptions& options);
    // The open groom's name ("" none: the empty state alone shows).
    void setGroomName(const QString& name);
    bool groomOpen() const { return !name_.isEmpty(); }
    // The guide groups, in tree order (children after their parents), each with its depth.
    struct GroupRow {
        std::string name;
        int depth = 0;
    };
    void setGroups(const std::vector<GroupRow>& groups);
    void setParts(const std::vector<std::string>& names);
    // The flow arrows and sinks the groom keeps (the Flow block's count, beside Clear all).
    void setFlowCount(std::size_t arrows, std::size_t sinks);
    // A button with a definitions menu (`ref`, e.g. "menus/output.export") that runs the window's
    // bindings: the empty state's Import ▾, the menu bar corner's Export ▾ and Render ▾. Its menu's
    // actions take `prefix` before their names, so the window's actions (found by name: tests,
    // scripts) stay the menu bar's own.
    static QToolButton* menuButton(const std::string& ref, const QString& text, const QString& tip, const ui::Bindings& bindings,
                                   const ui::FillOptions& options, QWidget* parent, const QString& prefix = QStringLiteral("groomTools_"));

    // ---- The tool ------------------------------------------------------------------------------
    // The active tool: a catalog id ("" none, the camera), its tiles lit and its card shown.
    void setTool(const std::string& id);
    const std::string& tool() const { return tool_; }
    // The group brushes edit (the session's: its group, else the first group at its level; "" every
    // level), and the one Isolate shows at full color with its child groups ("": Isolate off).
    std::string currentGroup() const;
    std::string isolatedGroup() const;
    // Everything takes the session's settings again (a radius drag, a picked level, a script).
    void refresh();
    // The tool tiles' names under their icons (View > Rail Labels): off, compact icon tiles.
    void setTileLabels(bool shown);
    bool tileLabels() const { return tileLabels_; }
    void setHairVisible(bool visible);  // the hair's eye (no signal)
    bool hairVisible() const;
    void setGuidesVisible(bool visible);  // the guides' eye (no signal)
    bool guidesVisible() const;
    // A line under the header (a tool that could not start, what to do first); "" hides it.
    void setStatus(const QString& text);
    QString status() const;

    // ---- Tabs and lists -----------------------------------------------------------------------
    // The tab by palette.json's id or an alias ("tools", "paint", "correctives" or "sculpt",
    // "photos" or "reference"; "guides": the Tools tab, its guide groups). False for another word.
    bool showTab(const QString& id);
    QString currentTab() const;
    // The lists the window fills: the Paint tab's maps panel (its Mode, Value + Fill and color go
    // into the Paint card), the Correctives tab's panel, the Tools tab's guides panel (Isolate in
    // its header), the Photos tab's panel. Their own scroll areas fit their content here: the
    // column scrolls as one.
    void setPaintContent(QWidget* content);
    void setCorrectivesContent(QWidget* content);
    void setGroupsContent(QWidget* content);
    void setPhotosContent(QWidget* content);
    // The tubes' blocks read the groom again (the selection or the solo changed, an undo, a graph
    // edit): now while a tool that shows them is on, else when one is picked.
    void refreshTubes();

    // ---- The pieces (tests) ---------------------------------------------------------------------
    QAbstractButton* tile(const std::string& id) const;  // the tool's first tile (the Paint tab's for Paint)
    std::vector<QAbstractButton*> tiles() const;         // every tool tile, Unfreeze All among them
    std::vector<ToolGrid*> grids() const { return grids_; }
    BrushPanel* card() const { return card_; }
    TubePanel* tubePanel() const { return tubes_; }
    QLabel* noToolLabel() const { return noTool_; }
    QToolButton* isolateButton() const { return isolate_; }
    QCheckBox* symmetryBox() const;
    QComboBox* symmetryMode() const;
    QToolButton* eyeButton() const { return eye_; }
    QToolButton* guidesEyeButton() const { return guidesEye_; }
    QToolButton* graphButton() const { return graph_; }
    QMenu* moreMenu() const { return moreMenu_; }
    QWidget* emptyState() const { return empty_; }
    QWidget* pinned() const { return pinned_; }
    QWidget* work() const { return work_; }
    QTabBar* tabBar() const { return tabs_; }
    QScrollArea* scrollArea() const { return scroll_; }

signals:
    // A tile: the catalog id ("select", "freeze", "paint", "comb"...).
    void toolChosen(const QString& id);
    void unfreezeAllRequested();  // the Unfreeze All tile
    void settingsChanged();       // the card or a picked group edited the brush settings
    void guidesEdited();          // the card or the tubes changed the guides (a tube edit, Apply)
    void tubesPreviewed();        // the tube outline editor's live preview (no undo step): evaluate and redraw
    void lengthsPreviewed();      // the Keys block changed the Length Keys preview: evaluate and redraw
    void clearFlowRequested();
    void isolateChanged();        // Isolate or the group: the display's isolated level
    void hairVisibilityToggled(bool visible);    // the hair's eye: the strands alone
    void guidesVisibilityToggled(bool visible);  // the guides' eye: every guide group (Properties > Guides)
    void graphRequested();
    void tabChanged(const QString& id);

private:
    QToolButton* makeTile(const std::string& id, const QString& label, const QString& tip);
    void refreshTiles();       // checked state and the icons' tint
    void refreshCard();        // the card for the tool and the tab
    void refreshVisibility();  // empty state vs the work, the tab's grid, card and list
    void groupPicked(const std::string& name);  // a guide groups row clicked
    QToolButton* menuChip(const ui::Item& item, const ui::Bindings& bindings, const ui::FillOptions& options, QWidget* parent);
    void fitScroll(QWidget* content);  // a hosted panel's own scroll area as tall as its content

    BrushSession* session_ = nullptr;
    bool updating_ = false;
    QString name_;
    std::string tool_;
    bool tileLabels_ = false;  // icons alone (View > Tool Labels: the names under them)
    std::vector<GroupRow> groups_;
    // Pinned.
    QWidget* pinned_ = nullptr;
    QLabel* groomName_ = nullptr;
    QToolButton* eye_ = nullptr;
    QToolButton* guidesEye_ = nullptr;
    QToolButton* graph_ = nullptr;
    QToolButton* more_ = nullptr;
    QMenu* moreMenu_ = nullptr;
    QLabel* status_ = nullptr;
    QTabBar* tabs_ = nullptr;
    std::vector<std::string> tabIds_;
    QWidget* toolsGrid_ = nullptr;  // the categories' grids (Tools, Correctives)
    QWidget* paintGrid_ = nullptr;  // the Paint tile (Paint)
    std::map<std::string, std::vector<QToolButton*>> tiles_;
    std::vector<QToolButton*> allTiles_;
    std::vector<ToolGrid*> grids_;
    // Scrolls.
    QScrollArea* scroll_ = nullptr;
    QFrame* empty_ = nullptr;
    QVBoxLayout* emptyItems_ = nullptr;
    QWidget* work_ = nullptr;
    BrushPanel* card_ = nullptr;
    QLabel* noTool_ = nullptr;
    TubePanel* tubes_ = nullptr;
    bool tubesStale_ = false;
    std::map<std::string, QWidget*> lists_;  // by tab id: the holder of its list
    QPointer<GuidesPanel> guides_;
    QToolButton* isolate_ = nullptr;
    // The buttons that run a binding, with the binding's enabled state (refresh).
    std::vector<std::pair<QPointer<QAbstractButton>, std::function<bool()>>> enables_;
};

}  // namespace groomlab::app
