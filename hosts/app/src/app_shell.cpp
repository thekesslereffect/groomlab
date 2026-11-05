// SPDX-License-Identifier: Apache-2.0
#include "app_shell.h"

#include <algorithm>
#include <vector>

#include <QShortcut>
#include <QAction>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QSplitter>
#include <QStyle>
#include <QStackedWidget>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "cards_panel.h"
#include "export_panel.h"
#include "groom_tools.h"
#include "definitions.h"
#include "icon_rail.h"
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

void hideDockedTitle(QDockWidget* dock) {
    auto apply = [dock](bool floating) {
        const bool hidden = dock->titleBarWidget() != nullptr;
        if (floating == hidden) dock->setTitleBarWidget(floating ? nullptr : new QWidget(dock));
    };
    QObject::connect(dock, &QDockWidget::topLevelChanged, dock, apply);
    apply(dock->isFloating());
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
    // The rail at the dock's outer edge holds the Groom Tools' pages (Tools, Paint, Correctives,
    // Photos: their tab bar, hidden, keeps which is open); the open page's icon folds the dock.
    auto* leftBody = new QWidget();
    {
        auto* row = new QHBoxLayout(leftBody);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        leftRail_ = new IconRail(IconRail::Side::Left, leftBody);
        QTabBar* pages = groomToolsWidget_->tabBar();
        // The pages' icons from the shared definitions (palette/tabs: the hosts' order and glyphs).
        const ui::json& tabs = ui::Definitions::shipped().get("palette/tabs");
        for (int i = 0; i < pages->count(); ++i) {
            const std::string icon = i < static_cast<int>(tabs.size()) ? tabs[static_cast<std::size_t>(i)].value("icon", std::string("ui_tools")) : "ui_tools";
            leftRail_->addPage(QStringLiteral(":/groomlab/icons/%1.svg").arg(QString::fromStdString(icon)), pages->tabText(i), QStringLiteral("leftRail_%1").arg(i));
        }
        groomToolsWidget_->setTabsShown(false);
        pages->hide();
        leftRail_->setCurrent(pages->currentIndex());
        connect(leftRail_, &IconRail::picked, pages, &QTabBar::setCurrentIndex);
        connect(pages, &QTabBar::currentChanged, leftRail_, &IconRail::setCurrent);
        connect(leftRail_, &IconRail::collapseRequested, this, [this](bool fold) { foldDock(true, fold); });
        leftContent_ = groomToolsWidget_;
        row->addWidget(leftRail_);
        row->addWidget(groomToolsWidget_, 1);
    }
    groomTools_ = makeDock(tr("Groom Tools"), "groomToolsDock", leftBody);
    window_->addDockWidget(Qt::LeftDockWidgetArea, groomTools_);

    // ---- Properties (right): the mode's panel.
    propertiesStack_ = new QStackedWidget();
    propertiesStack_->setObjectName(QStringLiteral("propertiesStack"));
    displayPage_ = section(propertiesStack_, 0);  // (its content carries the docks' gutter, as the other pages)
    sections_[static_cast<std::size_t>(Panel::Display)] = displayPage_;
    lookCard_ = new LookCard(propertiesStack_);
    lookCard_->setDocked(true);
    lookCard_->setTabsShown(false);  // the dock's tabs pick its page
    cardsPanel_ = new CardsPanel(propertiesStack_);
    cardsPanel_->setDocked(true);
    exportPanel_ = new ExportPanel(propertiesStack_);
    exportPanel_->setDocked(true);
    auto* empty = new QLabel(tr("<p style='margin:0 0 6px 0; font-weight:600; color:%1'>Nothing to edit yet</p>"
                                "<p style='margin:0'>A groom's settings show here once one is open.</p>")
                                 .arg(theme::text().name()),
                             propertiesStack_);
    empty->setObjectName(QStringLiteral("propertiesEmpty"));
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    empty->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    // The shared empty state: a card at the top of the page (as the Cards panel's): its icon, title and line centered.
    auto* emptyHost = new QWidget(propertiesStack_);
    auto* emptyColumn = new QVBoxLayout(emptyHost);
    emptyColumn->setContentsMargins(12, 9, 18, 12);  // (where the Attributes cards end)
    auto* emptyCard = new QWidget(emptyHost);
    emptyCard->setProperty("role", "tray");
    emptyCard->setAttribute(Qt::WA_StyledBackground, true);
    auto* emptyCardColumn = new QVBoxLayout(emptyCard);
    emptyCardColumn->setContentsMargins(16, 24, 16, 24);
    emptyCardColumn->setSpacing(10);
    emptyCardColumn->addWidget(emptyIcon(QStringLiteral("groups"), emptyCard), 0, Qt::AlignHCenter);
    empty->setParent(emptyCard);
    emptyCardColumn->addWidget(empty);
    emptyColumn->addWidget(emptyCard);
    emptyColumn->addStretch(1);
    emptyPage_ = emptyHost;
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
    propertiesTabs_->setDrawBase(false);
    propertiesTabs_->setProperty("role", "pills");
    propertiesTabs_->setUsesScrollButtons(false);
    // The pages from the shared definitions (properties/pages: Attributes, Material, Lighting,
    // Render, Cards, Export, in PropertiesTab's order; their icons and the actions whose shortcuts open them).
    std::vector<QString> pageIcons;
    for (const ui::json& page : ui::Definitions::shipped().get("properties/pages")) {
        const int i = propertiesTabs_->addTab(QString::fromStdString(page.value("label", std::string())));
        propertiesTabs_->setTabToolTip(i, QString::fromStdString(page.value("tip", std::string())));
        pageIcons.push_back(QStringLiteral(":/groomlab/icons/%1.svg").arg(QString::fromStdString(page.value("icon", std::string("ui_sliders")))));
        pageActions_.push_back(page.value("action", std::string()));
    }
    connect(propertiesTabs_, &QTabBar::currentChanged, this, [this](int index) {
        refreshPages();
        emit propertiesTabPicked(index);
    });
    // The page's title over it, the rail at the dock's outer edge (the tab bar, hidden, keeps which
    // page is open: the modes follow it).
    auto* propertiesBody = new QWidget();
    auto* propertiesRow = new QHBoxLayout(propertiesBody);
    propertiesRow->setContentsMargins(0, 0, 0, 0);
    propertiesRow->setSpacing(0);
    rightContent_ = new QWidget(propertiesBody);
    auto* propertiesColumn = new QVBoxLayout(rightContent_);
    // (6 px between the pages' scroll bar and the rail's rule: one seam, not two)
    propertiesColumn->setContentsMargins(0, 0, 6, 0);
    propertiesColumn->setSpacing(0);
    propertiesTitle_ = new QLabel(tr("Attributes"), rightContent_);
    propertiesTitle_->setObjectName(QStringLiteral("propertiesTitle"));
    {
        // components.pageTitle: its text at the cards' left edge (the pages' 12 px gutter).
        const int inset = theme::component("pageTitle", "inset", 12);
        propertiesTitle_->setContentsMargins(inset - 1, 14, inset, 0);  // (its baseline on the Groom Tools name's)
        propertiesTitle_->setStyleSheet(QStringLiteral("font-size: %1px; font-weight: %2;")
                                            .arg(theme::component("pageTitle", "size", 15))
                                            .arg(theme::component("pageTitle", "weight", 700)));
    }
    propertiesColumn->addWidget(propertiesTitle_);
    propertiesTabs_->setParent(rightContent_);
    propertiesTabs_->hide();
    propertiesColumn->addWidget(propertiesStack_, 1);
    propertiesRow->addWidget(rightContent_, 1);
    rightRail_ = new IconRail(IconRail::Side::Right, propertiesBody);
    {
        for (int i = 0; i < propertiesTabs_->count(); ++i) {
            rightRail_->addPage(pageIcons[static_cast<std::size_t>(i)], propertiesTabs_->tabText(i), QStringLiteral("rightRail_%1").arg(i));
        }
        connect(rightRail_, &IconRail::picked, propertiesTabs_, &QTabBar::setCurrentIndex);
        connect(propertiesTabs_, &QTabBar::currentChanged, this, [this](int index) {
            rightRail_->setCurrent(index);
            propertiesTitle_->setText(propertiesTabs_->tabText(index));
        });
        connect(rightRail_, &IconRail::collapseRequested, this, [this](bool fold) { foldDock(false, fold); });
    }
    propertiesRow->addWidget(rightRail_);
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
    // Docked, a dock's own title bar only restates its content's header (the groom's name, the
    // tabs, the graph's bar): hidden; floating, it is back (to drag, dock and close the window).
    // View > Panels shows and hides the docks.
    for (QDockWidget* d : {groomTools_, properties_, nodesDock_, lookDock_, helpDock_}) hideDockedTitle(d);
    // A folded dock floated, moved or covered by a tab beside it opens again (a 44 px window or tab
    // group helps no one); moved to the other side, its rail goes to the new outer edge.
    for (const bool left : {true, false}) {
        QDockWidget* dock = left ? groomTools_ : properties_;
        connect(dock, &QDockWidget::topLevelChanged, this, [this, left](bool floating) {
            if (floating) foldDock(left, false);
        });
        connect(dock, &QDockWidget::dockLocationChanged, this, [this, left](Qt::DockWidgetArea area) { placeRail(left, area); });
    }
    for (QDockWidget* dock : {nodesDock_, lookDock_, helpDock_}) {
        // (only a tab of Properties' group: one floating, elsewhere, or back from a minimize is not)
        connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible) {
            if (visible && !dock->isFloating() && window_->tabifiedDockWidgets(properties_).contains(dock)) foldDock(false, false);
        });
    }
    // The rails are the side docks' outer edges: those docks stay at the sides (or float).
    for (QDockWidget* dock : {groomTools_, properties_}) dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    // The Groom Tools' pages from the keyboard (Shift+1..4, as Ctrl+1..4 the Properties').
    for (int i = 0; i < leftRail_->count(); ++i) {
        auto* key = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key(Qt::Key_1 + i)), window_);
        key->setContext(Qt::WindowShortcut);
        connect(key, &QShortcut::activated, this, [this, i] { showToolsPage(i); });
        leftRail_->setPageKey(i, key->key().toString(QKeySequence::NativeText));
    }

    // ---- The Graph Editor (bottom): the canvas, the selected node's settings at its right.
    graph_ = new NodeGraphView();
    graph_->setFitMargins(QMargins(24, 24, 24, 24));
    nodeSide_ = section(nullptr, 12);
    nodeSide_->setObjectName(QStringLiteral("nodeSettings"));
    nodeSide_->setProperty("role", "tray");  // a card, as Properties' sections
    nodeSide_->setAttribute(Qt::WA_StyledBackground, true);
    {
        auto* header = new QHBoxLayout();
        header->setSpacing(8);
        nodeDot_ = dot(theme::blue(), nodeSide_);
        nodeTitle_ = new QLabel(nodeSide_);
        header->addWidget(nodeDot_);
        header->addWidget(nodeTitle_, 1);
        static_cast<QVBoxLayout*>(nodeSide_->layout())->addLayout(header);
        setPanelTitle(Panel::Node, tr("Node Settings"), theme::blue());
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
        graphCount_ = new QLabel(graphBar_);
        graphCount_->setObjectName(QStringLiteral("graphCount"));
        graphCount_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(theme::textMuted().name()));
        row->addWidget(graphCount_);
        row->addSpacing(6);
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
                                                "#graphBar QToolButton { color: %3; font-weight: 500; padding: 3px 9px; border: none; border-radius: 5px; }"
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
    hideDockedTitle(graphDock_);
    window_->addDockWidget(Qt::BottomDockWidgetArea, graphDock_);

    // ---- The timeline: a strip under the graph (no title: Maya's time slider).
    timeline_ = new Timeline();
    timeline_->setDocked(true);
    timeline_->setObjectName(QStringLiteral("viewportTimeline"));
    // Under the viewport, its width (between the side docks): a bar, never a dock beside the graph
    // (MainWindow puts it in the viewport's column).
    timeline_->hide();
    graphDock_->hide();

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

std::array<QDockWidget*, 6> AppShell::docks() const {
    return {groomTools_, properties_, graphDock_, nodesDock_, lookDock_, helpDock_};
}

void AppShell::resetLayout() {
    foldDock(true, false);
    foldDock(false, false);
    // The side docks a share of the window's width, the Graph Editor a third of its height.
    const int w = std::max(window_->width(), 800), h = std::max(window_->height(), 500);
    // (each with its icon rail on top of its content's width)
    window_->resizeDocks({groomTools_, properties_},
                         {std::clamp(w * 18 / 100, 210, 320) + IconRail::railWidth(), std::clamp(w * 21 / 100, 250, 360) + IconRail::railWidth()},
                         Qt::Horizontal);
    window_->resizeDocks({graphDock_}, {std::clamp(h * 34 / 100, 200, 420)}, Qt::Vertical);
    // The graph about two thirds of the Graph Editor, its node's settings the rest (proportions:
    // the splitter scales them to its width).
    graphSplit_->setSizes({640, 360});
}

void AppShell::layoutRestored() {
    groomToolsWanted_ = !groomTools_->isHidden();
    graphDock_->setProperty("sized", true);
    timeline_->setVisible(timelineShown_);
    setMode(mode_);
}

void AppShell::setMode(Mode mode) {
    const bool wasGroom = mode_ == Mode::Groom;
    mode_ = mode;
    overlay_->setMode(mode);
    // The modes are the Properties dock's tabs (no switch of their own): Cards and Export their tab,
    // Look the look's (Material, unless a look tab is up), Groom the groom's (Attributes, unless a
    // look tab is up). The Groom Tools stay as they are.
    (void)wasGroom;
    {
        const QSignalBlocker block(propertiesTabs_);
        const PropertiesTab tab = propertiesTab();
        const bool lookTab = tab == PropertiesTab::Material || tab == PropertiesTab::Lighting || tab == PropertiesTab::Render;
        if (mode == Mode::Cards) propertiesTabs_->setCurrentIndex(static_cast<int>(PropertiesTab::Cards));
        else if (mode == Mode::Export) propertiesTabs_->setCurrentIndex(static_cast<int>(PropertiesTab::Export));
        else if (mode == Mode::Look && !lookTab) propertiesTabs_->setCurrentIndex(static_cast<int>(PropertiesTab::Material));
        else if (mode == Mode::Groom && (tab == PropertiesTab::Cards || tab == PropertiesTab::Export))
            propertiesTabs_->setCurrentIndex(static_cast<int>(PropertiesTab::Attributes));
    }
    if (mode != Mode::Groom) {
        properties_->show();
        properties_->raise();
        foldDock(false, false);  // (a mode picked by its shortcut: its page in view)
    }
    refreshPages();
}

void AppShell::foldDock(bool left, bool fold) {
    // Folded: the dock down to its rail (the viewport takes the room); unfolded: back to its width.
    QDockWidget* dock = left ? groomTools_ : properties_;
    QWidget* content = left ? leftContent_ : rightContent_;
    IconRail* rail = left ? leftRail_ : rightRail_;
    int& width = left ? leftWidth_ : rightWidth_;
    if (fold == rail->collapsed()) return;
    if (fold) {
        if (content->isVisible()) width = dock->width();
        content->hide();
        dock->setMaximumWidth(IconRail::railWidth());
        window_->resizeDocks({dock}, {IconRail::railWidth()}, Qt::Horizontal);
    } else {
        dock->setMaximumWidth(QWIDGETSIZE_MAX);
        content->show();
        window_->resizeDocks({dock}, {std::max(width, theme::component("iconRail", "openMin", 220))}, Qt::Horizontal);
    }
    rail->setCollapsed(fold);
}

void AppShell::showToolsPage(int index) {
    groomTools_->show();
    groomTools_->raise();
    foldDock(true, false);
    groomToolsWidget_->tabBar()->setCurrentIndex(index);
}

void AppShell::setFoldKeys(const QString& left, const QString& right) {
    leftRail_->setFoldKey(left);
    rightRail_->setFoldKey(right);
}

void AppShell::toggleFold(bool left) {
    QDockWidget* dock = left ? groomTools_ : properties_;
    if (dock->isHidden()) {
        dock->show();
        foldDock(left, false);
        return;
    }
    if (dock->isFloating()) return;  // (a floating dock does not fold)
    foldDock(left, !folded(left));
}

bool AppShell::folded(bool left) const { return (left ? leftRail_ : rightRail_)->collapsed(); }

int AppShell::openWidth(bool left) const {
    const QWidget* content = left ? leftContent_ : rightContent_;
    return content->isVisible() ? (left ? groomTools_ : properties_)->width() : (left ? leftWidth_ : rightWidth_);
}

void AppShell::restoreFold(bool left, bool fold, int width) {
    if (fold) foldDock(left, true);
    (left ? leftWidth_ : rightWidth_) = width;  // (the width it opens at, not the rail's)
}

void AppShell::placeRail(bool left, Qt::DockWidgetArea area) {
    IconRail* rail = left ? leftRail_ : rightRail_;
    if (area != Qt::LeftDockWidgetArea && area != Qt::RightDockWidgetArea) return;
    const IconRail::Side side = area == Qt::LeftDockWidgetArea ? IconRail::Side::Left : IconRail::Side::Right;
    if (side == rail->side()) return;
    auto* row = qobject_cast<QHBoxLayout*>(rail->parentWidget()->layout());
    if (!row) return;
    row->removeWidget(rail);
    row->insertWidget(side == IconRail::Side::Left ? 0 : row->count(), rail);
    rail->setSide(side);
}

void AppShell::setPageKeys(const std::function<QString(const std::string& action)>& keyOf) {
    for (int i = 0; i < static_cast<int>(pageActions_.size()); ++i) {
        if (!pageActions_[static_cast<std::size_t>(i)].empty()) rightRail_->setPageKey(i, keyOf(pageActions_[static_cast<std::size_t>(i)]));
    }
}

void AppShell::refreshPages() {
    // The modes as workspaces: Groom edits the groom (Attributes first, the look's tabs beside
    // it), Look its material, lights and render (their tabs alone), Cards and Export their own
    // panel. No groom: the empty page (no editor full of zeros).
    // The page is the tab's: Attributes the groom's settings, Material, Lighting and Render the look
    // card's, Cards and Export their panels. No groom: the empty page (no editor full of zeros).
    QWidget* page = displayPage_;
    const bool open = groomOpen();
    // The rail and the title follow the tab (also when a mode set it with the tab bar's signals blocked).
    rightRail_->setCurrent(propertiesTabs_->currentIndex());
    propertiesTitle_->setText(propertiesTabs_->tabText(propertiesTabs_->currentIndex()));
    const PropertiesTab tab = propertiesTab();
    if (!open) {
        page = emptyPage_;
    } else if (tab == PropertiesTab::Material || tab == PropertiesTab::Lighting || tab == PropertiesTab::Render) {
        lookCard_->setTab(static_cast<LookCard::Tab>(static_cast<int>(tab) - 1));
        page = lookCard_;
    } else if (tab == PropertiesTab::Cards) {
        page = cardsPanel_;
    } else if (tab == PropertiesTab::Export) {
        page = exportPanel_;
    }
    propertiesStack_->setCurrentWidget(page);
}

void AppShell::setPropertiesTab(PropertiesTab tab) {
    properties_->show();
    properties_->raise();
    foldDock(false, false);
    propertiesTabs_->setCurrentIndex(static_cast<int>(tab));  // (a tab picks its mode: propertiesTabPicked)
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
    overlay_->setGraphOpen(shown);
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
    if (graphCount_) graphCount_->setText(nodeCount > 0 ? tr("%n nodes", "", nodeCount) : QString());
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
        case Panel::Node: {
            // No node: a section header, as Properties'; a node: its name after its category's dot.
            const bool none = title == tr("Node Settings");
            nodeTitle_->setText(title);
            nodeTitle_->setProperty("role", none ? "listTitle" : "title");
            nodeTitle_->style()->unpolish(nodeTitle_);
            nodeTitle_->style()->polish(nodeTitle_);
            nodeDot_->setVisible(!none);
            nodeDot_->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(color.name()));
            break;
        }
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
            } else if (which != Panel::Brush && which != Panel::Guides && groomToolsWidget_->currentTab() == tab) {
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
            return !groomTools_->isHidden() && groomToolsWidget_->currentTab() == QLatin1String("tools");
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
    timeline_->setVisible(shown);
}

void AppShell::setRailLabels(bool shown) { groomToolsWidget_->setTileLabels(shown); }

bool AppShell::railLabels() const { return groomToolsWidget_->tileLabels(); }

}  // namespace groomlab::app
