// SPDX-License-Identifier: Apache-2.0
#include "app_shell.h"

#include <algorithm>

#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QSplitter>
#include <QStackedWidget>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "cards_panel.h"
#include "export_panel.h"
#include "groom_tools.h"
#include "node_graph.h"
#include "theme.h"
#include "timeline.h"

namespace groomlab::app {

namespace {

// A section: a widget with a column layout the panel's content goes into.
QWidget* section(QWidget* parent, int margin) {
    auto* w = new QWidget(parent);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(8);
    return w;
}

QLabel* dot(const QColor& color, QWidget* parent) {
    auto* d = new QLabel(parent);
    d->setFixedSize(8, 8);
    d->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(color.name()));
    return d;
}

}  // namespace

AppShell::AppShell(QMainWindow* window, OverlayHost* overlay) : QObject(window), window_(window), overlay_(overlay) {
    window_->setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
    window_->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    // The side docks run the window's whole height; the Graph Editor and the timeline sit between them.
    window_->setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    window_->setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    window_->setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    window_->setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);

    // No tool bar: the file's name is the window's title (MainWindow::refreshTitle), the modes are
    // in View > Mode (Ctrl+1..4), Render and the Graph Editor in the GroomLab menu (Tab), Undo and
    // Redo at the viewport's top-right (OverlayHost) and in Edit, the presets in the Graph Editor.

    // ---- Groom Tools (left): Maya's Groom Tools from palette.json (groom_tools.h).
    groomToolsWidget_ = new GroomTools();
    for (Panel p : {Panel::Guides, Panel::Maps, Panel::Correctives, Panel::Brush}) {
        sections_[static_cast<std::size_t>(p)] = groomToolsWidget_;
    }
    groomTools_ = makeDock(tr("Groom Tools"), "groomToolsDock", groomToolsWidget_);
    window_->addDockWidget(Qt::LeftDockWidgetArea, groomTools_);

    // ---- Properties (right): the mode's panel.
    propertiesStack_ = new QStackedWidget();
    propertiesStack_->setObjectName(QStringLiteral("propertiesStack"));
    displayPage_ = section(propertiesStack_, 8);
    sections_[static_cast<std::size_t>(Panel::Display)] = displayPage_;
    lookCard_ = new LookCard(propertiesStack_);
    lookCard_->setDocked(true);
    lookCard_->setTabsShown(false);  // the dock's tabs pick its page
    cardsPanel_ = new CardsPanel(propertiesStack_);
    cardsPanel_->setDocked(true);
    exportPanel_ = new ExportPanel(propertiesStack_);
    exportPanel_->setDocked(true);
    auto* empty = new QLabel(tr("Open a groom (File > Open Groom, or New Groom on Mesh) to use this mode."), propertiesStack_);
    empty->setObjectName(QStringLiteral("propertiesEmpty"));
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignCenter);
    empty->setMargin(24);
    empty->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    emptyPage_ = empty;
    for (QWidget* page : {displayPage_, static_cast<QWidget*>(lookCard_), static_cast<QWidget*>(cardsPanel_),
                          static_cast<QWidget*>(exportPanel_), emptyPage_}) {
        propertiesStack_->addWidget(page);
    }
    // Groom and Look: tabs over the page, Attributes (the groom's settings) and the look card's
    // Material, Lighting and Render; Cards and Export show their own panel alone.
    propertiesTabs_ = new QTabBar();
    propertiesTabs_->setObjectName(QStringLiteral("propertiesTabs"));
    propertiesTabs_->setDocumentMode(true);
    propertiesTabs_->setExpanding(false);
    propertiesTabs_->setDrawBase(true);
    propertiesTabs_->setUsesScrollButtons(false);
    for (const QString& title : {tr("Attributes"), tr("Material"), tr("Lighting"), tr("Render")}) propertiesTabs_->addTab(title);
    propertiesTabs_->setTabToolTip(0, tr("The groom's settings, in the order of Maya's Attribute Editor"));
    propertiesTabs_->setTabToolTip(1, tr("The hair's color and shading"));
    propertiesTabs_->setTabToolTip(2, tr("The viewport's lookdev lighting: studio presets and lights"));
    propertiesTabs_->setTabToolTip(3, tr("The lookdev render's quality and tone"));
    connect(propertiesTabs_, &QTabBar::currentChanged, this, [this](int) { refreshPages(); });
    auto* propertiesBody = new QWidget();
    auto* propertiesColumn = new QVBoxLayout(propertiesBody);
    propertiesColumn->setContentsMargins(0, 0, 0, 0);
    propertiesColumn->setSpacing(0);
    propertiesColumn->addWidget(propertiesTabs_);
    propertiesColumn->addWidget(propertiesStack_, 1);
    properties_ = makeDock(tr("Properties"), "propertiesDock", propertiesBody);
    window_->addDockWidget(Qt::RightDockWidgetArea, properties_);
    // Node List, Look Settings and Help: tabs beside Properties, shown on demand.
    nodesDock_ = makeDock(tr("Node List"), "nodesDock", nullptr);
    lookDock_ = makeDock(tr("Look Settings"), "lookDock", nullptr);
    helpDock_ = makeDock(tr("Help"), "helpDock", nullptr);
    sections_[static_cast<std::size_t>(Panel::Nodes)] = nodesDock_;
    sections_[static_cast<std::size_t>(Panel::Look)] = lookDock_;
    sections_[static_cast<std::size_t>(Panel::Help)] = helpDock_;
    for (QDockWidget* dock : {nodesDock_, lookDock_, helpDock_}) {
        window_->addDockWidget(Qt::RightDockWidgetArea, dock);
        window_->tabifyDockWidget(properties_, dock);
        dock->hide();
    }
    properties_->raise();

    // ---- The Graph Editor (bottom): the canvas, the selected node's settings at its right.
    graph_ = new NodeGraphView();
    graph_->setFitMargins(QMargins(24, 24, 24, 24));
    nodeSide_ = section(nullptr, 8);
    nodeSide_->setObjectName(QStringLiteral("nodeSettings"));
    {
        auto* header = new QHBoxLayout();
        header->setSpacing(8);
        nodeDot_ = dot(theme::blue(), nodeSide_);
        nodeTitle_ = new QLabel(tr("Node Settings"), nodeSide_);
        nodeTitle_->setFont(theme::uiFont(13, 800));
        nodeTitle_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
        header->addWidget(nodeDot_);
        header->addWidget(nodeTitle_, 1);
        static_cast<QVBoxLayout*>(nodeSide_->layout())->addLayout(header);
    }
    sections_[static_cast<std::size_t>(Panel::Node)] = nodeSide_;
    graphSplit_ = new QSplitter(Qt::Horizontal);
    graphSplit_->setObjectName(QStringLiteral("graphSplit"));
    graphSplit_->addWidget(graph_);
    graphSplit_->addWidget(nodeSide_);
    graphSplit_->setStretchFactor(0, 1);
    graphSplit_->setStretchFactor(1, 0);
    // The graph keeps room to work in a narrow window; the settings scroll inside their own width.
    graph_->setMinimumWidth(240);
    nodeSide_->setMinimumWidth(200);
    graphSplit_->setCollapsible(0, false);
    graphSplit_->setSizes({640, 360});
    // Its bar, as Maya's Graph Editor's tool bar: Add Node, Presets, the status line, then the
    // Edit menu (the three dots). The window fills the menus (the shared definitions: graph_ui.json).
    graphBar_ = new QWidget();
    graphBar_->setObjectName(QStringLiteral("graphBar"));
    graphBar_->setAttribute(Qt::WA_StyledBackground, true);
    {
        auto* row = new QHBoxLayout(graphBar_);
        row->setContentsMargins(8, 4, 6, 4);
        row->setSpacing(4);
        auto menuButton = [this, row](const QString& text, const char* name, const QString& tip) {
            auto* b = new QToolButton(graphBar_);
            b->setObjectName(QString::fromLatin1(name));
            b->setText(text);
            b->setToolTip(tip);
            b->setPopupMode(QToolButton::InstantPopup);
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            b->setAutoRaise(true);
            row->addWidget(b);
            return b;
        };
        graphAdd_ = menuButton(tr("Add Node  \u25BE"), "graphAddButton", tr("Add a node (after the selected one). Also: Tab, or right-click the canvas"));
        graphPresets_ = menuButton(tr("Presets  \u25BE"), "graphPresetsButton",
                                   tr("Start from a look (hair, fur, feathers), a technique to learn from (blending, layers, maps, motion...), "
                                      "save this graph as a preset, or copy and paste the whole graph as text to share it"));
        graphStatus_ = new QLabel(graphBar_);
        graphStatus_->setObjectName(QStringLiteral("graphStatus"));
        graphStatus_->setTextFormat(Qt::PlainText);
        graphStatus_->setMinimumWidth(40);
        graphStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addSpacing(8);
        row->addWidget(graphStatus_, 1);
        // Snap: nodes and pins dragged land on the grid (Maya's Graph Editor's Snap).
        graphSnap_ = new QToolButton(graphBar_);
        graphSnap_->setObjectName(QStringLiteral("graphSnapButton"));
        graphSnap_->setText(tr("Snap"));
        graphSnap_->setToolTip(tr("Nodes and pins you drag land on the grid"));
        graphSnap_->setCheckable(true);
        graphSnap_->setAutoRaise(true);
        row->addWidget(graphSnap_);
        graphEdit_ = menuButton(QStringLiteral("\u00B7\u00B7\u00B7"), "graphEditButton",
                                tr("Edit: undo, framing, layout, groups, Copy and Paste Graph, units, theme"));
        const QString chip = theme::uiColor("hover").name();
        graphBar_->setStyleSheet(QStringLiteral("#graphBar { background: %1; border-bottom: 1px solid %2; }"
                                                "#graphBar QToolButton { color: %3; font-weight: 700; padding: 3px 9px; border: none; border-radius: 5px; }"
                                                "#graphBar QToolButton:hover, #graphBar QToolButton:pressed { background: %4; color: %5; }"
                                                "#graphBar QToolButton:disabled { color: %6; }"
                                                "#graphBar QToolButton:checked { background: %4; color: %7; }"
                                                "#graphBar QToolButton::menu-indicator { image: none; width: 0px; }"
                                                "#graphBar QLabel { color: %6; }")
                                     .arg(theme::uiColor("panel").name(), theme::divider().name(), theme::textSoft().name(), chip,
                                          theme::text().name(), theme::textMuted().name(), theme::nodeSelected().name()));
    }
    auto* graphBody = new QWidget();
    auto* graphColumn = new QVBoxLayout(graphBody);
    graphColumn->setContentsMargins(0, 0, 0, 0);
    graphColumn->setSpacing(0);
    graphColumn->addWidget(graphBar_);
    graphColumn->addWidget(graphSplit_, 1);
    graphDock_ = makeDock(tr("Graph Editor"), "graphDock", graphBody);
    window_->addDockWidget(Qt::BottomDockWidgetArea, graphDock_);

    // ---- The timeline: a strip under the graph (no title: Maya's time slider).
    timeline_ = new Timeline();
    timeline_->setDocked(true);
    timelineDock_ = makeDock(tr("Timeline"), "timelineDock", timeline_);
    timelineDock_->setTitleBarWidget(new QWidget(timelineDock_));
    timelineDock_->setFeatures(QDockWidget::DockWidgetClosable);
    window_->addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
    window_->splitDockWidget(graphDock_, timelineDock_, Qt::Vertical);
    graphDock_->hide();
    timelineDock_->hide();

    setFileName({}, false);
    setNodeCount(0);
    setMode(Mode::Groom);
}

QDockWidget* AppShell::makeDock(const QString& title, const char* name, QWidget* body) {
    auto* dock = new QDockWidget(title, window_);
    dock->setObjectName(QString::fromLatin1(name));
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    // The dock's solid panel (the style sheet's #dockBody) with the content inside.
    auto* frame = new QWidget(dock);
    frame->setObjectName(QStringLiteral("dockBody"));
    frame->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (body) layout->addWidget(body);
    dock->setWidget(frame);
    return dock;
}

std::array<QDockWidget*, 7> AppShell::docks() const {
    return {groomTools_, properties_, graphDock_, timelineDock_, nodesDock_, lookDock_, helpDock_};
}

void AppShell::resetLayout() {
    // The side docks a share of the window's width, the Graph Editor a third of its height.
    const int w = std::max(window_->width(), 800), h = std::max(window_->height(), 500);
    window_->resizeDocks({groomTools_, properties_}, {std::clamp(w * 18 / 100, 210, 320), std::clamp(w * 21 / 100, 250, 360)},
                         Qt::Horizontal);
    window_->resizeDocks({graphDock_}, {std::clamp(h * 34 / 100, 200, 420)}, Qt::Vertical);
    // The graph about two thirds of the Graph Editor, its node's settings the rest (proportions:
    // the splitter scales them to its width).
    graphSplit_->setSizes({640, 360});
}

void AppShell::layoutRestored() {
    groomToolsWanted_ = !groomTools_->isHidden();
    graphDock_->setProperty("sized", true);
    timelineDock_->setVisible(timelineShown_);
    setMode(mode_);
}

void AppShell::setMode(Mode mode) {
    const bool wasGroom = mode_ == Mode::Groom;
    mode_ = mode;
    overlay_->setMode(mode);
    // The modes are workspaces: Groom brings the Groom Tools back as they were; the others hide
    // them and give the Properties dock their panel.
    if (mode == Mode::Groom) {
        if (groomToolsWanted_) groomTools_->show();
    } else {
        // Look opens on the look (its Material tab) unless a look tab is already up.
        if (mode == Mode::Look && propertiesTab() == PropertiesTab::Attributes) {
            const QSignalBlocker block(propertiesTabs_);
            propertiesTabs_->setCurrentIndex(static_cast<int>(PropertiesTab::Material));
        }
        if (wasGroom) groomToolsWanted_ = !groomTools_->isHidden();
        groomTools_->hide();
        properties_->show();
        properties_->raise();
    }
    refreshPages();
}

void AppShell::refreshPages() {
    QWidget* page = displayPage_;
    const bool tabbed = mode_ == Mode::Groom || mode_ == Mode::Look;
    propertiesTabs_->setVisible(tabbed);
    if (tabbed) {
        // Attributes, else the look card on its tab (no zeros before a groom: it waits for one).
        const PropertiesTab tab = propertiesTab();
        if (tab != PropertiesTab::Attributes) {
            lookCard_->setTab(static_cast<LookCard::Tab>(static_cast<int>(tab) - 1));
            page = groomOpen() ? static_cast<QWidget*>(lookCard_) : emptyPage_;
        }
    } else {
        page = !groomOpen() ? emptyPage_
               : mode_ == Mode::Cards ? static_cast<QWidget*>(cardsPanel_)
                                      : static_cast<QWidget*>(exportPanel_);
    }
    propertiesStack_->setCurrentWidget(page);
}

void AppShell::setPropertiesTab(PropertiesTab tab) {
    properties_->show();
    properties_->raise();
    if (mode_ != Mode::Groom && mode_ != Mode::Look) return;  // Cards and Export show their own panel
    propertiesTabs_->setCurrentIndex(static_cast<int>(tab));
    refreshPages();
}

AppShell::PropertiesTab AppShell::propertiesTab() const {
    return static_cast<PropertiesTab>(std::max(0, propertiesTabs_->currentIndex()));
}

void AppShell::setFileName(const QString& name, bool dirty) {
    name_ = name;
    overlay_->setFileName(name, dirty);
    refreshEnabled();
    refreshPages();
}

void AppShell::refreshEnabled() {
    const bool open = groomOpen();
    // Nothing to add or edit in the graph yet: its bar waits for a groom (Presets too: Apply on
    // a Mesh starts one).
    graphAdd_->setEnabled(open);
    graphEdit_->setEnabled(open);
    // The Groom Tools show how to start a groom until one is open (palette.empty).
    groomToolsWidget_->setGroomName(name_);
}

void AppShell::setTool(const std::string& brushMode, bool select) {
    groomToolsWidget_->setTool(select ? std::string("select") : brushMode);
    overlay_->setTool(brushMode, select);
}

void AppShell::setGraphShown(bool shown, int nodeCount) {
    setNodeCount(nodeCount);
    if (!shown) {
        graphDock_->hide();
        return;
    }
    const bool first = graphDock_->isHidden() && !graphDock_->property("sized").toBool();
    graphDock_->show();
    graphDock_->raise();
    if (first) {
        // Its first showing: a third of the window's height (a restored layout keeps its own).
        graphDock_->setProperty("sized", true);
        window_->resizeDocks({graphDock_}, {std::clamp(window_->height() * 34 / 100, 200, 420)}, Qt::Vertical);
    }
    graph_->setFocus();
}

bool AppShell::graphShown() const { return !graphDock_->isHidden(); }

void AppShell::setNodeCount(int nodeCount) {
    nodeCount_ = nodeCount;
    graphDock_->setWindowTitle(nodeCount > 0 ? tr("Graph Editor  \u00B7  %n nodes", "", nodeCount) : tr("Graph Editor"));
}

void AppShell::setGraphStatus(const QString& text) {
    graphStatus_->setText(text);
    graphStatus_->setToolTip(text);
}

void AppShell::setPanelContent(Panel which, const QString& title, const QColor& color, QWidget* content) {
    QWidget* holder = panel(which);
    if (!holder || !content) return;
    switch (which) {
        case Panel::Look:
        case Panel::Nodes:
        case Panel::Help: {
            auto* dock = static_cast<QDockWidget*>(holder);
            auto* body = section(nullptr, 8);
            body->layout()->addWidget(content);
            static_cast<QVBoxLayout*>(dock->widget()->layout())->addWidget(body);
            dock->setWindowTitle(title);
            break;
        }
        case Panel::Node:
            static_cast<QVBoxLayout*>(holder->layout())->addWidget(content, 1);
            setPanelTitle(which, title, color);
            break;
        // The Groom Tools' pages: their titles are palette.json's.
        case Panel::Guides: groomToolsWidget_->setGroupsContent(content); break;
        case Panel::Maps: groomToolsWidget_->setPaintContent(content); break;
        case Panel::Correctives: groomToolsWidget_->setCorrectivesContent(content); break;
        case Panel::Brush: break;  // (the card is the Groom Tools' own)
        default:
            static_cast<QVBoxLayout*>(holder->layout())->addWidget(content, 1);
            setPanelTitle(which, title, color);
            break;
    }
}

void AppShell::setPanelTitle(Panel which, const QString& title, const QColor& color) {
    switch (which) {
        case Panel::Look:
        case Panel::Nodes:
        case Panel::Help: static_cast<QDockWidget*>(panel(which))->setWindowTitle(title); break;
        case Panel::Node:
            nodeTitle_->setText(title);
            nodeDot_->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(color.name()));
            break;
        default: break;
    }
}

void AppShell::showPanel(Panel which, bool show) {
    QWidget* holder = panel(which);
    if (!holder) return;
    switch (which) {
        case Panel::Guides:
        case Panel::Maps:
        case Panel::Correctives:
        case Panel::Brush: {
            // A tab of the Groom Tools (Guides: the Tools tab with its guide groups open; Brush:
            // the Tools tab and the tool's card).
            const QString tab = which == Panel::Guides ? QStringLiteral("guides")
                                : which == Panel::Maps ? QStringLiteral("paint")
                                : which == Panel::Correctives ? QStringLiteral("correctives")
                                                              : QStringLiteral("tools");
            if (show) {
                groomToolsWanted_ = true;
                groomTools_->show();
                groomTools_->raise();
                groomToolsWidget_->showTab(tab);
            } else if (which == Panel::Guides) {
                groomToolsWidget_->section("groups")->setExpanded(false);
            } else if (which != Panel::Brush && groomToolsWidget_->currentTab() == tab) {
                groomToolsWidget_->showTab(QStringLiteral("tools"));
            }
            break;
        }
        case Panel::Display:
            if (show) setPropertiesTab(PropertiesTab::Attributes);
            else properties_->hide();
            break;
        case Panel::Look:
        case Panel::Nodes:
        case Panel::Help:
            holder->setVisible(show);
            if (show) holder->raise();
            break;
        case Panel::Node:
            // Part of the Graph Editor: shown with it.
            if (show) {
                graphDock_->show();
                graphDock_->raise();
            }
            break;
        case Panel::Count: break;
    }
}

void AppShell::togglePanel(Panel which) { showPanel(which, !panelShown(which)); }

bool AppShell::panelShown(Panel which) const {
    QWidget* holder = panel(which);
    if (!holder) return false;
    switch (which) {
        case Panel::Guides:
            return !groomTools_->isHidden() && groomToolsWidget_->currentTab() == QLatin1String("tools") &&
                   groomToolsWidget_->section("groups")->expanded();
        case Panel::Maps: return !groomTools_->isHidden() && groomToolsWidget_->currentTab() == QLatin1String("paint");
        case Panel::Correctives: return !groomTools_->isHidden() && groomToolsWidget_->currentTab() == QLatin1String("correctives");
        case Panel::Brush: return !groomTools_->isHidden() && groomToolsWidget_->currentTab() == QLatin1String("tools");
        case Panel::Display: return !properties_->isHidden() && propertiesStack_->currentWidget() == displayPage_;
        case Panel::Node: return !graphDock_->isHidden();
        default: return !holder->isHidden();
    }
}

void AppShell::setTimelineShown(bool shown) {
    timelineShown_ = shown;
    timelineDock_->setVisible(shown);
}

void AppShell::setRailLabels(bool shown) { groomToolsWidget_->setTileLabels(shown); }

bool AppShell::railLabels() const { return groomToolsWidget_->tileLabels(); }

}  // namespace groomlab::app
