// SPDX-License-Identifier: Apache-2.0
#pragma once

// The app's window shell, Maya's way: a QMainWindow with a menu bar (MainWindow::buildMenuBar) and
// no tool bar (the file's name is the window's title; the modes Groom / Look / Cards / Export are
// View > Mode and Ctrl+1..4; Render and the Graph Editor are in the GroomLab menu; Undo and Redo
// sit at the viewport's top-right), the viewport in the middle (OverlayHost: the viewport and what
// stays on the picture) and docks around it, movable and floatable, their layout saved between
// runs (MainWindow::saveLayout):
//
//   Groom Tools    left: Maya's Groom Tools (groom_tools.h, from palette.json): the group and
//                  symmetry, the tabs Tools / Paint / Correctives / Photos, the active tool's card,
//                  the sections (While sculpting, Guide groups & edits, Output)
//   Properties     right: what the mode is about (Groom: the Display options; Look: the look
//                  card; Cards: the cards panel; Export: the export panel)
//   Node List, Look Settings, Help   tabbed with Properties, shown on demand
//   Graph Editor   bottom: a bar (Add Node, Presets, the status line, the Edit menu), the node
//                  graph on an opaque canvas, the selected node's settings at its right (as Maya's)
//   Timeline       a strip under it while the groom animates
//
// The modes are workspaces: Groom shows the Groom Tools, the others hide them and give the
// Properties dock their own panel. The pieces keep the names the window's code and the tests
// use (lookCard, cardsPanel...); a panel (Panel) is a section of a dock now, shown by raising it
// (Guides, Maps, Correctives and Brush: the Groom Tools' tab or section).

#include <array>
#include <string>

#include <QObject>
#include <QPoint>
#include <QString>

#include "overlay.h"

class QAction;
class QDockWidget;
class QLabel;
class QMainWindow;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QToolButton;
class QWidget;

namespace groomlab::app {

class CardsPanel;
class ExportPanel;
class GroomTools;
class NodeGraphView;
class Timeline;

class AppShell : public QObject {
    Q_OBJECT
public:
    using Mode = OverlayHost::Mode;
    // The sections the window fills (setPanelContent) and shows by name (MainWindow::showUiPanel).
    enum class Panel { Guides, Nodes, Maps, Correctives, Look, Display, Brush, Node, Help, Count };

    AppShell(QMainWindow* window, OverlayHost* overlay);

    // ---- The pieces ---------------------------------------------------------------------------
    GroomTools* groomTools() const { return groomToolsWidget_; }
    LookCard* lookCard() const { return lookCard_; }
    CardsPanel* cardsPanel() const { return cardsPanel_; }
    ExportPanel* exportPanel() const { return exportPanel_; }
    Timeline* timeline() const { return timeline_; }
    NodeGraphView* graph() const { return graph_; }
    // The Graph Editor's bar: Add Node, Presets and Edit (the window fills their menus), the status.
    QWidget* graphBar() const { return graphBar_; }
    QToolButton* graphAddButton() const { return graphAdd_; }
    QToolButton* graphPresetsButton() const { return graphPresets_; }
    QToolButton* graphEditButton() const { return graphEdit_; }
    QToolButton* graphSnapButton() const { return graphSnap_; }
    QLabel* graphStatus() const { return graphStatus_; }
    void setGraphStatus(const QString& text);  // what the last graph edit did

    // ---- The docks ----------------------------------------------------------------------------
    QDockWidget* groomToolsDock() const { return groomTools_; }
    QDockWidget* propertiesDock() const { return properties_; }
    QDockWidget* graphDock() const { return graphDock_; }
    QDockWidget* timelineDock() const { return timelineDock_; }
    QDockWidget* nodesDock() const { return nodesDock_; }
    QDockWidget* lookDock() const { return lookDock_; }
    QDockWidget* helpDock() const { return helpDock_; }
    // Every dock (View > Panels lists their toggles in this order).
    std::array<QDockWidget*, 7> docks() const;
    // The docks' sizes for a window of this size (a first run, or View > Reset Layout).
    void resetLayout();
    // A saved layout was put back (QMainWindow::restoreState): its docks kept as they were, the
    // mode's and the timeline's own visibility applied again over it.
    void layoutRestored();

    // ---- The Properties tabs (Groom and Look): Attributes (the groom's settings in the
    // Attribute Editor's order), then the look card's Material, Lighting and Render.
    enum class PropertiesTab { Attributes, Material, Lighting, Render };
    void setPropertiesTab(PropertiesTab tab);  // shows the dock too
    PropertiesTab propertiesTab() const;
    QTabBar* propertiesTabs() const { return propertiesTabs_; }

    // ---- Modes (workspaces) -------------------------------------------------------------------
    void setMode(Mode mode);  // the docks and the overlay follow; no signal
    Mode mode() const { return mode_; }

    // ---- The groom ----------------------------------------------------------------------------
    // The groom's file name ("" no groom: what needs one is disabled and dimmed, the modes'
    // panels give way to "No groom open"); the window's title shows it.
    void setFileName(const QString& name, bool dirty);
    bool groomOpen() const { return !name_.isEmpty(); }
    // The active tool: its tile lit and its card shown, its name over the viewport.
    void setTool(const std::string& brushMode, bool select);

    // ---- The graph ----------------------------------------------------------------------------
    void setGraphShown(bool shown, int nodeCount);  // the Graph Editor dock shown (raised, focused) or closed
    bool graphShown() const;
    void setNodeCount(int nodeCount);  // the Graph Editor's title: "N nodes"
    int nodeCount() const { return nodeCount_; }

    // ---- Panels -------------------------------------------------------------------------------
    // A panel's content, placed in its dock section (the title names the tab or the dock).
    void setPanelContent(Panel panel, const QString& title, const QColor& dot, QWidget* content);
    // Shown: its dock shown and raised, its tab current; hidden: its dock closed (a tab: back to
    // the tools; Brush: the full settings folded; Node: kept, it is part of the Graph Editor).
    void showPanel(Panel panel, bool show);
    void togglePanel(Panel panel);
    bool panelShown(Panel panel) const;
    // The section holding the panel's content (its dock's for Look, Nodes and Help).
    QWidget* panel(Panel panel) const { return sections_[static_cast<std::size_t>(panel)]; }
    // A section's title changed (the Help page's subject, the selected node's category color).
    void setPanelTitle(Panel panel, const QString& title, const QColor& dot);

    // ---- The timeline -------------------------------------------------------------------------
    void setTimelineShown(bool shown);  // the strip shows while the groom animates
    bool timelineShown() const { return timelineShown_; }

    // ---- The tiles' names ---------------------------------------------------------------------
    // The tool tiles' names under their icons (View > Rail Labels; off: compact icon tiles).
    void setRailLabels(bool shown);
    bool railLabels() const;

private:
    QDockWidget* makeDock(const QString& title, const char* name, QWidget* body);
    void refreshPages();     // the Properties page for the mode (or "No groom open")
    void refreshEnabled();   // what needs a groom

    QMainWindow* window_;
    OverlayHost* overlay_;
    Mode mode_ = Mode::Groom;
    QString name_;
    bool timelineShown_ = false;
    bool groomToolsWanted_ = true;  // the Groom Tools' visibility to bring back in Groom mode
    int nodeCount_ = 0;
    // The Graph Editor's bar.
    QWidget* graphBar_ = nullptr;
    QToolButton* graphAdd_ = nullptr;
    QToolButton* graphPresets_ = nullptr;
    QToolButton* graphEdit_ = nullptr;
    QToolButton* graphSnap_ = nullptr;
    QLabel* graphStatus_ = nullptr;
    // The pieces.
    GroomTools* groomToolsWidget_ = nullptr;
    LookCard* lookCard_ = nullptr;
    CardsPanel* cardsPanel_ = nullptr;
    ExportPanel* exportPanel_ = nullptr;
    Timeline* timeline_ = nullptr;
    NodeGraphView* graph_ = nullptr;
    // The docks and their insides.
    QDockWidget* groomTools_ = nullptr;
    QDockWidget* properties_ = nullptr;
    QDockWidget* graphDock_ = nullptr;
    QDockWidget* timelineDock_ = nullptr;
    QDockWidget* nodesDock_ = nullptr;
    QDockWidget* lookDock_ = nullptr;
    QDockWidget* helpDock_ = nullptr;
    QStackedWidget* propertiesStack_ = nullptr;
    QTabBar* propertiesTabs_ = nullptr;
    QWidget* displayPage_ = nullptr;
    QWidget* emptyPage_ = nullptr;
    QSplitter* graphSplit_ = nullptr;
    QWidget* nodeSide_ = nullptr;
    QLabel* nodeDot_ = nullptr;
    QLabel* nodeTitle_ = nullptr;
    std::array<QWidget*, static_cast<std::size_t>(Panel::Count)> sections_{};
};

}  // namespace groomlab::app
