// SPDX-License-Identifier: Apache-2.0
// MainWindow: its panels, built and wired to the document, the viewport and the docks.

#include "main_window_internal.h"

#include <QFileInfo>

#include <cmath>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

#include <QDockWidget>

#include "groomlab/brush_catalog.h"

#include "app_actions.h"
#include "app_shell.h"
#include "groom_tools.h"
#include "marking_menu.h"
#include "cards_panel.h"
#include "correctives_panel.h"
#include "export_panel.h"
#include "glass.h"
#include "guides_panel.h"
#include "look_panel.h"
#include "maps_panel.h"
#include "node_graph.h"
#include "overlay.h"
#include "param_panel.h"
#include "theme.h"
#include "viewport.h"

namespace groomlab::app {

void MainWindow::buildPanels() {
    // The ordinary panels, each placed in its dock section (app_shell.h).
    groomTools_ = shell_->groomTools();
    auto* nodesHost = new QWidget();
    auto* nodesLayout = new QVBoxLayout(nodesHost);
    nodesLayout->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter(Qt::Vertical, nodesHost);
    nodes_ = new QListWidget(split);
    params_ = new ParamPanel(split);
    split->addWidget(nodes_);
    split->addWidget(params_);
    split->setStretchFactor(1, 1);
    nodesLayout->addWidget(split);
    shell_->setPanelContent(AppShell::Panel::Nodes, tr("Node List"), theme::blue(), nodesHost);
    connect(nodes_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        params_->showNode(item ? item->data(Qt::UserRole).toString().toStdString() : std::string());
    });
    connect(params_, &ParamPanel::edited, this, [this] { graphEdited(); });
    connect(params_, &ParamPanel::dragging, this, [this](bool on) { paramDragged(on); });
    connect(params_, &ParamPanel::message, this, [this](const QString& text) { report(text); });
    connect(params_, &ParamPanel::paintParamRequested, this,
            [this](const std::string& node, const std::string& name) { paintParam(QString::fromStdString(node), QString::fromStdString(name)); });

    graphParams_ = new ParamPanel();
    shell_->setPanelContent(AppShell::Panel::Node, tr("Node Settings"), theme::blue(), graphParams_);
    connect(graphParams_, &ParamPanel::edited, this, [this] { graphEdited(); });
    connect(graphParams_, &ParamPanel::dragging, this, [this](bool on) { paramDragged(on); });
    connect(graphParams_, &ParamPanel::message, this, [this](const QString& text) { report(text); });
    connect(graphParams_, &ParamPanel::paintParamRequested, this,
            [this](const std::string& node, const std::string& name) { paintParam(QString::fromStdString(node), QString::fromStdString(name)); });

    // The Properties dock's Groom workspace (main_window_properties.cpp: properties.json).
    buildProperties();

    // The maps: made, painted and shown through the document and the brush session.
    maps_ = new MapsPanel();
    maps_->setSession(&brush_);
    shell_->setPanelContent(AppShell::Panel::Maps, tr("Maps"), theme::yellow(), maps_);
    connect(maps_, &MapsPanel::edited, this, [this] {
        guidesEdited();
        refreshDisplayChoices();
        params_->refresh();
        graphParams_->refresh();
    });
    connect(maps_, &MapsPanel::displayChanged, this, [this] {
        refreshDisplayChoices();
        redisplay();
    });
    connect(maps_, &MapsPanel::brushChanged, this, [this] {
        groomTools_->refresh();
        refreshTool();
    });
    connect(maps_, &MapsPanel::paintRequested, this, [this] {
        setUiMode(QStringLiteral("groom"));
        viewport_->setToolMode(Viewport::ToolMode::Brush);
        groomTools_->refresh();
        viewport_->refreshOverlay();
        refreshTool();
    });
    connect(maps_, &MapsPanel::message, this, [this](const QString& text) { report(text); });
    connect(maps_, &MapsPanel::paintParamRequested, this, [this](const std::string& node, const std::string& param, const std::string& kind) {
        paintParam(QString::fromStdString(node), QString::fromStdString(param), QString::fromStdString(kind));
    });

    look_ = new LookPanel();
    shell_->setPanelContent(AppShell::Panel::Look, tr("Look Settings"), theme::yellow(), look_);
    help_ = new QLabel();
    help_->setWordWrap(true);
    help_->setTextFormat(Qt::RichText);
    help_->setOpenExternalLinks(true);
    help_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    shell_->setPanelContent(AppShell::Panel::Help, tr("Help"), theme::blue(), new FitScrollArea(help_));  // scrolls when short
    // The Groom Tools (groom_tools.h): the tiles pick the tool, the card and the rows edit the
    // brush settings, the sections the strands while sculpting; the guides panel is its Guide
    // groups & edits (the parts are the Part tool's card's).
    groomTools_->setSession(&brush_);
    connect(groomTools_, &GroomTools::settingsChanged, this, [this] {
        viewport_->syncView();
        viewport_->refreshOverlay();
        refreshTool();
    });
    connect(groomTools_, &GroomTools::toolChosen, this, [this](const QString& id) { chooseTool(id); });
    connect(groomTools_, &GroomTools::guidesEdited, this, [this] { guidesEdited(); });
    connect(groomTools_, &GroomTools::tubesPreviewed, this, [this] { reevaluate(); });
    connect(groomTools_, &GroomTools::lengthsPreviewed, this, [this] {
        viewport_->refreshOverlay();
        reevaluate();
    });
    connect(groomTools_, &GroomTools::unfreezeAllRequested, this, [this] {
        if (!document_) return;
        document_->editGuides("unfreeze all", [](GuideHierarchy& h) { unfreezeAll(h); });
        guidesEdited();
    });
    connect(groomTools_, &GroomTools::clearFlowRequested, this, [this] { guidesPanel_->clearFlow(); });
    connect(groomTools_, &GroomTools::isolateChanged, this, [this] {
        settings_.isolateGroup = groomTools_->isolatedGroup();
        refreshProperties();  // (Isolate Group follows)
        redisplay();
    });
    connect(groomTools_, &GroomTools::hairVisibilityToggled, this, [this](bool on) { setHairVisible(on); });
    connect(groomTools_, &GroomTools::guidesVisibilityToggled, this, [this](bool on) { setGuidesVisible(on); });
    connect(groomTools_, &GroomTools::graphRequested, this, [this] { setGraphShown(true); });
    connect(groomTools_, &GroomTools::toolStopRequested, this, [this] {
        if (viewport_->toolMode() == Viewport::ToolMode::Brush) viewport_->setToolMode(Viewport::ToolMode::Camera);
    });
    connect(groomTools_, &GroomTools::tabChanged, this, [this](const QString& tab) {
        // Leaving the Paint tab is done painting (Maya's): the camera again.
        if (tab != QLatin1String("paint") && viewport_->toolMode() == Viewport::ToolMode::Brush && brush_.toolId() == "paint") {
            viewport_->setToolMode(Viewport::ToolMode::Camera);
        }
    });
    // The first-groom card over the viewport (overlay.h): its steps pick their tools.
    connect(host_, &OverlayHost::starterStepRequested, this, [this](const QString& step) {
        if (step == QLatin1String("guides")) {
            groomTools_->showTab(QStringLiteral("tools"));
            chooseTool(QStringLiteral("place"));
        } else {
            setGraphShown(true);
        }
    });
    connect(host_, &OverlayHost::starterDismissRequested, this, [this] {
        starterDismissed_ = true;
        if (interactive_) QSettings().setValue(QStringLiteral("ui/hideStarter"), true);
        host_->setStarterDismissed(true);
    });
    starterDismissed_ = interactive_ && QSettings().value(QStringLiteral("ui/hideStarter"), false).toBool();
    host_->setStarterDismissed(starterDismissed_);
    guidesPanel_ = new GuidesPanel();
    guidesPanel_->setPartsShown(false);  // (the Part tool's card lists them, as Maya's)
    shell_->setPanelContent(AppShell::Panel::Guides, tr("Guide groups"), theme::emerald(), guidesPanel_);
    connect(guidesPanel_, &GuidesPanel::edited, this, [this] { guidesEdited(); });
    // The correctives: the layers, and the one the brushes sculpt into at the timeline's frame.
    correctives_ = new CorrectivesPanel();
    shell_->setPanelContent(AppShell::Panel::Correctives, tr("Correctives"), theme::red(), correctives_);
    correctives_->setSession(&brush_);
    connect(correctives_, &CorrectivesPanel::edited, this, [this] { guidesEdited(); });
    connect(correctives_, &CorrectivesPanel::targetChanged, this, [this] {
        // A shot sculpt (or a Sculpt node) wants the brush: the layer (the node) takes the strokes
        // from here on.
        if ((!correctives_->targetLayer().empty() || !correctives_->targetNode().empty()) &&
            viewport_->toolMode() != Viewport::ToolMode::Brush) {
            setUiMode(QStringLiteral("groom"));
            viewport_->setToolMode(Viewport::ToolMode::Brush);
        }
        refreshTool();
    });
    connect(correctives_, &CorrectivesPanel::message, this, [this](const QString& text) { report(text); });
    connect(guidesPanel_, &GuidesPanel::displayChanged, this, [this] {
        if (document_) settings_.hiddenGuideGroups = document_->hiddenGroupsText();
        redisplay();
        guidesPanel_->invalidate();  // (not now: a row's check box may be what changed)
        refreshUndoActions();        // (a group's color is a step)
    });
    // A row clicked: the group the brushes edit and new guides go into (Maya's pick_group): the
    // Groom Tools set it (settingsChanged, isolateChanged).
    connect(guidesPanel_, &GuidesPanel::message, this, [this](const QString& text) { report(text); });
    connect(look_, &LookPanel::materialChanged, this, [this] {
        if (updatingLook_) return;
        shell_->lookCard()->setMaterial(look_->material());
        applyLook();
    });
    connect(look_, &LookPanel::lightingChanged, this, [this] {
        if (updatingLook_) return;
        shell_->lookCard()->setLighting(look_->lighting());
        applyLook();
    });
    connect(look_, &LookPanel::surfaceChanged, this, [this](int index, const SurfaceMaterial& material) {
        if (updatingLook_) return;
        GroomRenderer& r = viewport_->renderer();
        if (index == 0) {
            r.setScalpMaterial(material);
        } else if (index > 0 && static_cast<std::size_t>(index) <= props_.size()) {
            props_[index - 1]->material = material;
            r.propsChanged();
        }
        refreshSurfaces();
        viewport_->update();
    });
    refreshSurfaces();
    shell_->lookCard()->setMaterial(look_->material());
    shell_->lookCard()->setLighting(look_->lighting());
    applyLook();
}

void MainWindow::wireOverlay() {
    // The marking menu (Y + click over the viewport, marking_menu.json): the press opens it, the
    // drag and the release that follow go to it (the viewport keeps the mouse meanwhile).
    markingMenu_ = new MarkingMenu(host_);
    connect(viewport_, &Viewport::markingMenuPressed, this, [this](QPointF at) { showMarkingMenu(viewport_->mapTo(host_, at), true); });
    connect(viewport_, &Viewport::markingMenuDragged, this, [this](QPointF at) { markingMenu_->track(viewport_->mapTo(host_, at)); });
    connect(viewport_, &Viewport::markingMenuReleased, this, [this](QPointF at) { markingMenu_->release(viewport_->mapTo(host_, at)); });
    connect(markingMenu_, &MarkingMenu::picked, this, [this](const MarkingMenu::Item& item) { AppActions::runMarkingMenuItem(*this, item); });
    connect(markingMenu_, &MarkingMenu::dismissed, this, [this] { viewport_->setFocus(); });
    connect(host_, &OverlayHost::undoRequested, this, [this] { undo(); });
    connect(host_, &OverlayHost::redoRequested, this, [this] { redo(); });
    connect(host_, &OverlayHost::presetsRequested, this, [this] { showPresetsMenu(); });
    // The Graph Editor closed or shown by its own button or View > Panels: the graph follows (it
    // reads the document only while it shows).
    connect(shell_->graphDock(), &QDockWidget::visibilityChanged, this, [this](bool) {
        const bool shown = graphShown();
        if (shown != (graph_->document() != nullptr)) {
            if (shown && !document_) return;  // an empty Graph Editor until a groom opens
            setGraphShown(shown);
        }
    });
    // The empty-state card and a file dropped on the window.
    connect(host_, &OverlayHost::openRequested, this, [this] { chooseOpen(); });
    connect(host_, &OverlayHost::newRequested, this, [this] { chooseMesh(); });
    // A dropped or recent file asks first when the open groom has unsaved edits.
    connect(host_, &OverlayHost::fileDropped, this, [this](const QString& path) {
        confirmDiscard(tr("opening %1").arg(QFileInfo(path).fileName()), [this, path] { openPath(path); });
    });
    host_->setRecentFiles(recentFiles());
    // The look card edits the material and lighting; the full look panel follows.
    connect(shell_->lookCard(), &LookCard::materialEdited, this, [this] {
        updatingLook_ = true;
        look_->setMaterial(shell_->lookCard()->material());
        updatingLook_ = false;
        applyLook();
    });
    connect(shell_->lookCard(), &LookCard::lightingEdited, this, [this] {
        updatingLook_ = true;
        look_->setLighting(shell_->lookCard()->lighting());
        updatingLook_ = false;
        applyLook();
    });
    connect(shell_->lookCard(), &LookCard::surfaceEdited, this, [this](int index, const SurfaceMaterial& material) {
        // The card's edit onto the surface, the full panel following.
        GroomRenderer& r = viewport_->renderer();
        if (index == 0) {
            r.setScalpMaterial(material);
        } else if (index > 0 && static_cast<std::size_t>(index) <= props_.size()) {
            props_[index - 1]->material = material;
            r.propsChanged();
        }
        updatingLook_ = true;
        std::vector<std::pair<QString, SurfaceMaterial>> surfaces{{tr("scalp"), r.scalpMaterial()}};
        for (const SurfacePtr& p : props_) surfaces.emplace_back(QString::fromStdString(p->name), p->material);
        look_->setSurfaces(surfaces);
        updatingLook_ = false;
        viewport_->update();
    });
    connect(shell_->lookCard(), &LookCard::moreRequested, this, [this] { shell_->togglePanel(AppShell::Panel::Look); });
    // The card's swatches and main sliders edit the groom's last HairColor node (saved with the
    // groom): a slider's drag previews each step and records one edit at its end, a swatch sets the
    // pigment as one edit.
    connect(shell_->lookCard(), &LookCard::groomColorEdited, this,
            [this](const std::string& param, double value, bool live) { editGroomColor(param, value, live); });
    connect(shell_->lookCard(), &LookCard::groomSwatchChosen, this, [this](const std::string& preset) {
        const std::string node = hairColorNode();
        if (!document_ || node.empty()) return;
        const HairMaterial p = hairPreset(preset);
        const std::string error = document_->editGraph("hair color " + preset, [&](nlohmann::json& g) {
            for (nlohmann::json& n : g["nodes"]) {
                if (n.value("name", std::string()) != node) continue;
                n["params"]["mode"] = "melanin";  // a swatch is a pigment
                n["params"]["melanin"] = p.melanin;
                n["params"]["redness"] = p.melaninRedness;
                // Its dye too (white: none), so Dyed shows and the other swatches undo it.
                n["params"]["dye"] = {p.baseColor[0], p.baseColor[1], p.baseColor[2]};
            }
        });
        if (!error.empty()) {
            report(QString::fromStdString(error));
            refreshGroomColor();
            return;
        }
        graphEdited();
    });
    // HairColor's mode, and its picked colors (the hair's, the tips'): one edit each, a color
    // dragged in its picker previewed live and recorded once.
    connect(shell_->lookCard(), &LookCard::groomModeChosen, this, [this](const std::string& mode) { chooseGroomMode(mode); });
    connect(shell_->lookCard(), &LookCard::groomColorPicked, this,
            [this](const std::string& param, const RGB& c, bool live) { pickGroomColor(param, c, live); });
    // The graph: a selected node's params beside it; its edits shown and undoable.
    connect(graph_, &NodeGraphView::selectionChanged, this, [this](const std::string& node) {
        graphParams_->showNode(node);
        if (node.empty()) {
            shell_->showPanel(AppShell::Panel::Node, false);
            return;
        }
        const nlohmann::json* spec = document_ ? document_->nodeSpec(node) : nullptr;
        const std::string category = spec ? spec->value("category", std::string()) : std::string();
        // "Node Settings" in the node's category color (the node's name and type head the panel).
        shell_->setPanelTitle(AppShell::Panel::Node, tr("Node Settings"), theme::nodeCategoryColor(category));
        shell_->showPanel(AppShell::Panel::Node, true);
    });
    // View Up to Here (V): the viewport shows the graph's result at that node.
    connect(graph_, &NodeGraphView::viewNodeChanged, this, [this](const std::string& node) {
        if (node == viewNode_) return;
        viewNode_ = node;
        if (document_) reevaluate();
    });
    connect(graph_, &NodeGraphView::edited, this, [this] { graphEdited(); });
    connect(graph_, &NodeGraphView::message, this, [this](const QString& text) { report(text); });
    // Escape in the graph (no search open): the viewport takes the focus back, the dock stays.
    connect(graph_, &NodeGraphView::hideRequested, this, [this] { viewport_->setFocus(); });
}

void MainWindow::wireTimelineCardsAndExport() {
    // The timeline names frames; the window evaluates and draws them (frames the clock passes
    // while one is being made are never asked for: the clock is read again after each).
    Timeline* timeline = shell_->timeline();
    connect(timeline, &Timeline::frameChanged, this, [this](double frame) { showFrame(frame); });
    connect(timeline, &Timeline::rangeChanged, this, [this](double, double) { refreshExportState(); });
    connect(timeline, &Timeline::playingChanged, this, [this](bool playing) {
        if (!playing && document_) {
            report(tr("Played %1 frames, %2 dropped").arg(shownFrames_).arg(shell_->timeline()->droppedFrames()), false);
        }
        shownFrames_ = 0;
        refreshStats();
    });
    connect(timeline, &Timeline::followScalpChanged, this, [this](bool on) {
        if (!document_) return;
        document_->setFollowScalp(on);
        reevaluate();
        refreshTitle();
    });
    connect(timeline, &Timeline::simulateRequested, this, [this] { simulateTo(); });
    connect(timeline, &Timeline::bakeRequested, this, [this] { bakeSimulation(); });
    connect(timeline, &Timeline::invalidateRequested, this, [this] { invalidateSimulation(); });
    connect(timeline, &Timeline::simulationCancelRequested, this, [this] { cancelJob(); });

    CardsPanel* cards = shell_->cardsPanel();
    connect(cards, &CardsPanel::edited, this, [this](bool live) {
        if (live) {
            reevaluate(true);
            refreshCards();
        } else {
            graphEdited();
        }
    });
    connect(cards, &CardsPanel::viewChanged, this, [this] { reevaluate(); });
    connect(cards, &CardsPanel::bakeRequested, this, [this] { bakeCards(); });
    connect(cards, &CardsPanel::addNodeRequested, this, [this] {
        if (!document_) return;
        std::string made;
        const std::string error = document_->addCardsNode(&made);
        if (!error.empty()) {
            report(tr("Cannot add a Cards node: %1").arg(QString::fromStdString(error)));
            return;
        }
        shell_->cardsPanel()->setShowCards(true);
        graphEdited();
    });

    ExportPanel* exports = shell_->exportPanel();
    connect(exports, &ExportPanel::exportRequested, this, [this] { runExport(); });
    connect(exports, &ExportPanel::cancelRequested, this, [this] { cancelJob(); });
    connect(exports, &ExportPanel::browseRequested, this, [this] { chooseExportPath(); });
    connect(exports, &ExportPanel::saveRequested, this, [this] { save(); });
    connect(exports, &ExportPanel::saveAsRequested, this, [this] { saveAs(); });
}

}  // namespace groomlab::app

namespace groomlab::app {

bool MainWindow::showMarkingMenu(QPointF at, bool held) {
    if (!document_) {
        report(tr("Open a groom first: the marking menu picks its tools"));
        return false;
    }
    markingMenu_->setItems(AppActions::markingMenuItems(*this));
    markingMenu_->popup(at, held);
    return true;
}

void MainWindow::chooseTool(const QString& id) {
    const std::string tool = id.toStdString();
    groomTools_->setStatus({});
    if (tool != "select" && locked_ && locked_->isChecked()) {
        // A locked groom ignores brushes and part strokes (Maya's locked attribute).
        groomTools_->setStatus(tr("The groom is locked: unlock it in Properties > Groom to brush it."));
        return;
    }
    if (tool == "select") {
        // The Select tool, or (picked again) the camera.
        viewport_->setToolMode(viewport_->toolMode() == Viewport::ToolMode::Select ? Viewport::ToolMode::Camera : Viewport::ToolMode::Select);
        return;
    }
    const bool brushing = viewport_->toolMode() == Viewport::ToolMode::Brush;
    const std::string before = brush_.toolId();
    if (tool == "paint") {
        if (brushing && before == "paint") {
            viewport_->setToolMode(Viewport::ToolMode::Camera);  // the Paint tile again: done painting
            return;
        }
        groomTools_->showTab(QStringLiteral("paint"));
        if (maps_->selectedMap().empty()) {
            groomTools_->setStatus(tr("Make a map to paint (+ New map below), or pick one."));
            return;
        }
    }
    // Each brush keeps its own strength (the catalog's default the first time).
    if (brushing && !before.empty()) toolStrengths_[before] = brush_.settings().brush.strength;
    try {
        brush_.setTool(tool);
    } catch (const std::invalid_argument& e) {
        groomTools_->setStatus(QString::fromUtf8(e.what()));
        report(QString::fromUtf8(e.what()));
        return;
    }
    if (tool != before) {
        const auto kept = toolStrengths_.find(tool);
        if (kept != toolStrengths_.end()) brush_.settings().brush.strength = kept->second;
        else if (const groomlab::BrushEntry* e = brushEntry(tool)) brush_.settings().brush.strength = e->defaultStrength;
    }
    if (tool == "paint") {
        maps_->paintSelected();  // the map's kind, value and channels on the brush; brush mode (paintRequested)
    } else {
        viewport_->setToolMode(Viewport::ToolMode::Brush);
    }
    groomTools_->refresh();
    viewport_->refreshOverlay();
    refreshTool();
}

}  // namespace groomlab::app
