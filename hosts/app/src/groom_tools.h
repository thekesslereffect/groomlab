// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Groom Tools dock, as Maya's Groom Tools window (hosts/maya/scripts/groomlab/ui/palette.py
// ToolPalette, tool_tabs.py) and Blender's Groom Tools panel lay it out, built from the shared UI
// definitions (engine/ui/palette.json through definitions.h): one column that scrolls,
//
//   header        the groom's name, the eye (show or hide the groom), the Graph Editor, ⋯ More
//                 (palette.header.more: New Groom on Mesh, Theme)
//   Group         the guide group brushes edit and new guides go into (All groups, then each
//                 group, children under their parents), Isolate (its level at full color, the
//                 others dimmed); Symmetry and across what (World X, Y, Z, Topology)
//   status        a line that takes no room while it has nothing to say
//   no groom      palette.empty: Start a groom (New Groom on Mesh, Import ▾)
//   first groom   palette.starter: Place some guides, Give it a look (ticked as the groom gets there)
//   tabs          palette.tabs: Tools (the tiles by category: Create, Shape, Trim, Guides; Unfreeze
//                 All beside Freeze), Paint (the Paint tile over the maps panel), Correctives (the
//                 correctives panel), Photos (reference photos as cameras: photos_panel.h)
//   the tool      its name, what it does and its card (brush_panel.h: the settings it uses)
//   sections      palette.sections: Tubes (tube_panel.h: the targets' tube shape and sections, the shape
//                 library and its outline editor, Fill Tubes; opened by Place, Draw and Shape), While sculpting (how the strands draw
//                 while a stroke is live, Full density), Guide groups & edits (the guides panel,
//                 Tools tab), Output (palette.output: Simulate Range, Export ▾, Render ▾, Studio ▾,
//                 Transfer..., Import ▾)
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
class TubePanel;
class ValueSlider;

// ---- ToolGrid --------------------------------------------------------------------------------------
// Tiles that reflow to the width: as many a row as fit (at most `maxColumns`, at least two), each
// as wide as the row allows (Maya's widgets.ToolGrid).
class ToolGrid : public QWidget {
    Q_OBJECT
public:
    explicit ToolGrid(QWidget* parent = nullptr);
    void setButtons(std::vector<QAbstractButton*> buttons, int maxColumns);
    void setMaxColumns(int maxColumns);
    int columnsFor(int width) const;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    QSize tile() const;  // the widest and tallest button's minimum
    void place();
    std::vector<QAbstractButton*> buttons_;
    int maxColumns_ = 4;
    int spacing_ = 6;
};

// ---- Section ---------------------------------------------------------------------------------------
// A titled section that folds away: its header shows or hides the content (Maya's widgets.Section).
class Section : public QWidget {
    Q_OBJECT
public:
    Section(const QString& title, QWidget* content, bool expanded = false, QWidget* parent = nullptr);
    void setExpanded(bool expanded);
    bool expanded() const { return expanded_; }
    QPushButton* header() const { return header_; }
    QWidget* content() const { return content_; }

signals:
    void toggled(bool expanded);

private:
    QString title_;
    QPushButton* header_ = nullptr;
    QWidget* content_ = nullptr;
    bool expanded_ = false;
};

// ---- GroomTools ------------------------------------------------------------------------------------
class GroomTools : public QWidget {
    Q_OBJECT
public:
    explicit GroomTools(QWidget* parent = nullptr);

    // ---- What it works on ----------------------------------------------------------------------
    void setSession(BrushSession* session);
    // The window's bindings (app_actions.h): the More menu, the empty state's buttons, the Output
    // strip and the Parts box's buttons are filled from the definitions with them (ids without a
    // binding hidden, as in the menus).
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
    // The flow arrows and sinks the groom keeps (the Flow card's count, beside Clear All).
    void setFlowCount(std::size_t arrows, std::size_t sinks);
    // Whether a section (palette.sections) shows for a tool on a tab: its "tools" (the tool one of
    // them) and "tabs" (the tab one of them); one with neither always shows. Maya's and Blender's rule.
    static bool sectionShown(const ui::json& section, const std::string& tool, const std::string& tab);
    // The first-groom steps' progress (palette.starter): shown while a groom is open, a step is
    // left and they were not dismissed.
    void setStarterProgress(bool guidesPlaced, bool lookGiven);
    void setStarterDismissed(bool dismissed);
    bool starterShown() const;

    // ---- The tool ------------------------------------------------------------------------------
    // The active tool: a catalog id ("" none, the camera), its tiles lit and its card shown.
    void setTool(const std::string& id);
    const std::string& tool() const { return tool_; }
    // The group Isolate shows at full color with its child groups ("": Isolate off, or All groups).
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

    // ---- Tabs and sections --------------------------------------------------------------------
    // The tab by palette.json's id or an alias ("tools", "paint", "correctives" or "sculpt",
    // "photos" or "reference"; "guides": the Tools tab with its guide groups open). False for
    // another word.
    bool showTab(const QString& id);
    QString currentTab() const;
    // The pages the window fills: the Paint tab's maps panel, the Correctives tab's panel, the
    // Guide groups & edits section's guides panel (their own scroll areas fit their content
    // here: the whole dock scrolls as one column).
    void setPaintContent(QWidget* content);
    void setCorrectivesContent(QWidget* content);
    void setGroupsContent(QWidget* content);
    void setPhotosContent(QWidget* content);  // the Photos tab's panel (photos_panel.h)
    Section* section(const std::string& id) const;  // palette.sections' ids: tubes, sculptDisplay, groups, output
    // The strands while a stroke is live (DisplaySettings::sculptDisplay: 0 as they are, 1 dimmed,
    // 2 guides only) and Full density.
    int sculptDisplay() const;
    void setSculptDisplay(int display);  // no signal
    // The Tubes section reads the groom again (the selection or the solo changed, an undo, a
    // graph edit): only while it is open (opening it reads it).
    void refreshTubes();

    // ---- The pieces (tests) ---------------------------------------------------------------------
    QAbstractButton* tile(const std::string& id) const;  // the tool's first tile (the Paint tab's for Paint)
    std::vector<QAbstractButton*> tiles() const;         // every tool tile, Unfreeze All among them
    BrushPanel* card() const { return card_; }
    TubePanel* tubePanel() const { return tubes_; }
    QLabel* toolHeading() const { return heading_; }
    QLabel* toolHint() const { return hint_; }
    QComboBox* groupBox() const { return group_; }
    QCheckBox* isolateBox() const { return isolate_; }
    QCheckBox* symmetryBox() const { return symmetry_; }
    QComboBox* symmetryMode() const { return symmetryMode_; }
    QToolButton* eyeButton() const { return eye_; }
    QToolButton* guidesEyeButton() const { return guidesEye_; }
    QToolButton* graphButton() const { return graph_; }
    QMenu* moreMenu() const { return moreMenu_; }
    QWidget* emptyState() const { return empty_; }
    QWidget* work() const { return work_; }
    QTabBar* tabBar() const { return tabs_; }
    QCheckBox* fullDensityBox() const { return fullDensity_; }
    QComboBox* sculptDisplayBox() const { return sculpt_; }
    const std::vector<QToolButton*>& outputChips() const { return outputChips_; }
    QScrollArea* scrollArea() const { return scroll_; }

signals:
    // A tile: the catalog id ("select", "freeze", "paint", "comb"...).
    void toolChosen(const QString& id);
    void unfreezeAllRequested();  // the Unfreeze All tile
    void settingsChanged();       // the card, the group or the symmetry edited the brush settings
    void guidesEdited();          // the card or the Tubes section changed the guides (Unfreeze All, a tube edit)
    void tubesGraphEdited();      // the Tubes section's Fill Tubes or Tube Blend changed the graph
    void tubesPreviewed();        // the tube outline editor's live preview (no undo step): evaluate and redraw
    void lengthsPreviewed();      // the Keys box changed the Length Keys preview: evaluate and redraw
    void clearFlowRequested();
    void isolateChanged();        // Isolate or the group: the display's isolated level
    void sculptDisplayChanged();  // While sculpting: the strands' display or Full density
    void hairVisibilityToggled(bool visible);    // the hair's eye: the strands alone
    void guidesVisibilityToggled(bool visible);  // the guides' eye: every guide group (Properties > Guides)
    void graphRequested();
    void tabChanged(const QString& id);
    void starterStepRequested(const QString& id);  // "guides" or "look"
    void starterDismissRequested();

private:
    QToolButton* makeTile(const std::string& id, const QString& label, const QString& tip);
    void refreshTiles();          // checked state and the icons' tint
    void refreshGroupChoice();    // the Group combo from the settings' group and level
    void refreshSymmetry();
    void groupPicked();
    void symmetryEdited();
    void refreshCard();           // the heading, hint and card for the tool
    void refreshStarter();
    void refreshVisibility();     // empty state vs the work, the sections (refreshSections)
    void refreshSections();       // the sections the tool and tab use (palette.sections' tools, tabs)
    QWidget* outputStrip(const ui::Bindings& bindings, const ui::FillOptions& options);
    QToolButton* menuChip(const ui::Item& item, const ui::Bindings& bindings, const ui::FillOptions& options, QWidget* parent);
    QToolButton* actionChip(const ui::Item& item, const ui::Bindings& bindings, QWidget* parent);
    void fitScroll(QWidget* content);  // a hosted panel's own scroll area as tall as its content

    BrushSession* session_ = nullptr;
    bool updating_ = false;
    QString name_;
    std::string tool_;
    bool tileLabels_ = true;
    bool starterDismissed_ = false;
    bool guidesPlaced_ = false, lookGiven_ = false;
    QScrollArea* scroll_ = nullptr;
    QVBoxLayout* outer_ = nullptr;
    // Header.
    QLabel* groomName_ = nullptr;
    QToolButton* eye_ = nullptr;
    QToolButton* guidesEye_ = nullptr;
    QToolButton* graph_ = nullptr;
    QToolButton* more_ = nullptr;
    QMenu* moreMenu_ = nullptr;
    // Group and symmetry.
    QWidget* targetRow_ = nullptr;
    QComboBox* group_ = nullptr;
    QCheckBox* isolate_ = nullptr;
    QCheckBox* symmetry_ = nullptr;
    QComboBox* symmetryMode_ = nullptr;
    QWidget* centerRow_ = nullptr;
    ValueSlider* symmetryCenter_ = nullptr;
    QLabel* status_ = nullptr;
    // No groom; a first groom.
    QFrame* empty_ = nullptr;
    QVBoxLayout* emptyItems_ = nullptr;
    QFrame* starter_ = nullptr;
    std::map<std::string, QPushButton*> starterRows_;
    QLabel* starterTip_ = nullptr;
    // The work: tabs, the tool, the sections.
    QWidget* work_ = nullptr;
    QTabBar* tabs_ = nullptr;
    std::vector<std::string> tabIds_;
    std::vector<QWidget*> pages_;
    QWidget* paintHolder_ = nullptr;
    QWidget* correctivesHolder_ = nullptr;
    QWidget* photosHolder_ = nullptr;
    std::map<std::string, std::vector<QToolButton*>> tiles_;
    std::vector<QToolButton*> allTiles_;
    std::vector<ToolGrid*> grids_;
    QLabel* heading_ = nullptr;
    QLabel* hint_ = nullptr;
    BrushPanel* card_ = nullptr;
    std::map<std::string, Section*> sections_;
    std::map<std::string, ui::json> sectionRules_;  // palette.sections by id: the tools and tabs each shows with
    TubePanel* tubes_ = nullptr;
    std::string tubesOpenedFor_;  // the tool that last opened the Tubes section
    QWidget* groupsHolder_ = nullptr;
    QComboBox* sculpt_ = nullptr;
    QCheckBox* fullDensity_ = nullptr;
    QWidget* outputHolder_ = nullptr;
    std::vector<QToolButton*> outputChips_;
    // The buttons that run a binding, with the binding's enabled state (refresh).
    std::vector<std::pair<QPointer<QAbstractButton>, std::function<bool()>>> enables_;
};

}  // namespace groomlab::app
