// SPDX-License-Identifier: Apache-2.0
#include "app_actions.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <stdexcept>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "app_shell.h"
#include "cards_panel.h"
#include "brush_panel.h"
#include "graph_presets.h"
#include "groom_tools.h"
#include "guides_panel.h"
#include "look_panel.h"
#include "main_window.h"
#include "marking_menu.h"
#include "node_graph.h"
#include "overlay.h"
#include "reference_window.h"
#include "theme.h"
#include "tube_panel.h"
#include "tube_shape_editor.h"
#include "viewport.h"

#include "groomlab/brush.h"
#include "groomlab/brush_catalog.h"
#include "groomlab/display.h"
#include "groomlab/presets.h"

namespace groomlab::app {

namespace {

using ui::json;

QString str(const json& value) { return value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString(); }

// The Look card's lighting presets by name, as the Studio menu shows them.
QString lightingLabel(const std::string& name) {
    if (name == "env") return QObject::tr("Environment");
    if (name == "rim") return QObject::tr("Backlit");  // (the preset; Rim is one of its lights)
    QString label = QString::fromStdString(name);
    if (!label.isEmpty()) label[0] = label[0].toUpper();
    return label;
}

}  // namespace

ui::Bindings AppActions::bindings(MainWindow& w) {
    MainWindow* m = &w;
    ui::Bindings b;
    auto run = [](std::function<void()> fn) { return [fn = std::move(fn)](const json&, bool) { fn(); }; };
    auto groomOpen = [m] { return m->document_ != nullptr; };
    using Mode = OverlayHost::Mode;
    using Tool = Viewport::ToolMode;

    // ---- File (the app's File menu and GroomLab > Groom Files) ----
    b["file.open"] = {run([m] { m->chooseOpen(); })};
    b["file.import"] = {run([m] { m->chooseOpen(); })};
    b["file.new"] = {run([m] { m->chooseMesh(); })};
    b["groom.create"] = {run([m] { m->chooseMesh(); })};
    b["file.save"] = {run([m] { m->save(); }), {}, groomOpen, {}, {QKeySequence::Save}};
    // (Spelled out: QKeySequence::SaveAs has no binding on Windows.)
    b["file.saveAs"] = {run([m] { m->saveAs(); }), {}, groomOpen, {}, {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)}, QStringLiteral("saveAs")};
    b["file.reload"] = {run([m] { m->askReload(); }), {}, [m] { return m->document_ && !m->document_->path().empty(); }, {},
                        {QKeySequence(Qt::CTRL | Qt::Key_R)}, QStringLiteral("reload")};
    b["file.export"] = {run([m] { m->exportAlembic(); }), {}, groomOpen};
    b["file.saveImage"] = {run([m] { m->chooseSaveImage(); }), {}, groomOpen, {}, {}, QStringLiteral("saveImage")};
    b["file.addProp"] = {run([m] { m->chooseProp(); }), {}, {}, {}, {}, QStringLiteral("addProp")};
    b["file.addEye"] = {run([m] { m->askEye(); }), {}, {}, {}, {}, QStringLiteral("addEye")};
    b["file.quit"] = {run([m] {
                          // Asks first when the groom has unsaved edits (an interactive window).
                          m->confirmDiscard(QObject::tr("quitting"), [m] {
                              m->closing_ = true;
                              qApp->quit();
                          });
                      }),
                      {}, {}, {}, {}, QStringLiteral("quit")};

    // ---- Edit ----
    auto undoText = [m] {
        return m->document_ && m->document_->canUndo() ? QString::fromStdString(m->document_->undoText()) : QObject::tr("Undo");
    };
    auto redoText = [m] {
        return m->document_ && m->document_->canRedo() ? QString::fromStdString(m->document_->redoText()) : QObject::tr("Redo");
    };
    auto canUndo = [m] { return m->document_ && m->document_->canUndo(); };
    auto canRedo = [m] { return m->document_ && m->document_->canRedo(); };
    b["edit.undo"] = {run([m] { m->undo(); }), {}, canUndo, undoText};
    b["edit.redo"] = {run([m] { m->redo(); }), {}, canRedo, redoText, {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)}};
    b["graph.undo"] = {run([m] { m->undo(); }), {}, canUndo, undoText};
    b["graph.redo"] = {run([m] { m->redo(); }), {}, canRedo, redoText};
    auto tool = [m](Tool t, const char* name) {
        return ui::Binding{[m, t](const json&, bool) { m->viewport_->setToolMode(t); },
                           [m, t](const json&) { return m->viewport_->toolMode() == t; }, {}, {}, {}, QString::fromLatin1(name)};
    };
    b["tool.camera"] = tool(Tool::Camera, "toolCamera");
    b["tool.brush"] = tool(Tool::Brush, "toolBrush");
    b["tool.select"] = tool(Tool::Select, "toolSelect");
    b["guides.deleteSelected"] = {run([m] { m->guidesPanel_->deleteSelected(); }), {}, groomOpen, {}, {}, QStringLiteral("deleteGuides")};
    // Alt+Shift+X: the brush's symmetry on or off (Maya's GroomLab hotkey set binds the same).
    b["brush.symmetry"] = {run([m] { m->toggleSymmetry(); }), {}, {}, {}, {}, QStringLiteral("symmetry")};

    // ---- View ----
    b["view.frameAll"] = {run([m] {
                              // F over the graph frames its selected node (none: the whole graph), as Maya's
                              // Graph Editor does; anywhere else the camera frames the scene.
                              if (m->graphShown() && m->graph_->hasFocus()) m->graph_->frameSelected();
                              else m->frameAll();
                          }),
                          {}, {}, {}, {QKeySequence(Qt::Key_F)}};
    b["graph.open"] = {run([m] {
                           // Tab shows the graph; shown, the graph's own Tab opens its search.
                           // Shown and focused; with the focus there, the graph's own search.
                           if (!m->graphShown()) {
                               m->setGraphShown(true);
                           } else if (m->graph_->hasFocus()) {
                               m->graph_->openSearch();
                           } else {
                               m->shell_->graphDock()->raise();
                               m->graph_->setFocus();
                           }
                       }),
                       {}, {}, {}, {QKeySequence(Qt::Key_Tab)}, QStringLiteral("graphEditor")};
    const std::pair<const char*, Mode> modes[] = {{"groom", Mode::Groom}, {"look", Mode::Look}, {"cards", Mode::Cards}, {"export", Mode::Export}};
    for (int i = 0; i < 4; ++i) {
        const QString name = QLatin1String(modes[i].first);
        const Mode mode = modes[i].second;
        // Ctrl+1..4, as the mode pills' tooltips say.
        // Groom's opens Attributes (also from a look page), the others their page; a folded dock opens.
        b["view.mode." + name.toStdString()] = {[m, name, mode](const json&, bool) {
                                                    m->setUiMode(name);
                                                    if (mode == Mode::Groom) m->shell_->setPropertiesTab(AppShell::PropertiesTab::Attributes);
                                                    else m->shell_->unfold(false);
                                                },
                                                [m, mode](const json&) { return m->shell_->mode() == mode; },
                                                {},
                                                {},
                                                {QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i))},
                                                QStringLiteral("mode_") + name};
    }
    // The folds (the open page's icon too); the keys in the rails' tooltips.
    // (a floating dock does not fold: the action off; Alt+Shift+Left/Right where brackets take AltGr)
    b["view.foldGroomTools"] = {[m](const json&, bool) { m->shell_->toggleFold(true); }, [m](const json&) { return m->shell_->folded(true); },
                                [m] { return !m->shell_->groomToolsDock()->isFloating(); }, {},
                                {QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Left)},
                                QStringLiteral("foldGroomTools")};
    b["view.foldProperties"] = {[m](const json&, bool) { m->shell_->toggleFold(false); }, [m](const json&) { return m->shell_->folded(false); },
                                [m] { return !m->shell_->propertiesDock()->isFloating(); }, {},
                                {QKeySequence(Qt::CTRL | Qt::Key_BracketRight), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Right)},
                                QStringLiteral("foldProperties")};
    b["view.railLabels"] = {[m](const json&, bool on) { m->setRailLabels(on); }, [m](const json&) { return m->railLabels(); }, {}, {}, {},
                            QStringLiteral("railLabels")};
    b["view.turntable"] = {[m](const json&, bool on) { m->setTurntable(on); }, [m](const json&) { return m->turntable(); }, {}, {}, {},
                           QStringLiteral("turntable")};

    // GroomLab > Display: the open groom (the app's one groom is every groom), and View > Guides
    // (Alt+Shift+G, Maya's GroomLab hotkey set's key for every groom's guides).
    b["display.showAllHair"] = {run([m] { m->setHairVisible(true); }), {}, groomOpen, {}, {}, QStringLiteral("showAllHair")};
    b["display.hideAllHair"] = {run([m] { m->setHairVisible(false); }), {}, groomOpen, {}, {}, QStringLiteral("hideAllHair")};
    b["display.showAllGuides"] = {run([m] { m->setGuidesVisible(true); }), {}, groomOpen, {}, {}, QStringLiteral("showAllGuides")};
    b["display.hideAllGuides"] = {run([m] { m->setGuidesVisible(false); }), {}, groomOpen, {}, {}, QStringLiteral("hideAllGuides")};
    b["display.toggleGuides"] = {run([m] { m->setGuidesVisible(!m->guidesVisible()); }), [m](const json&) { return m->guidesVisible(); },
                                 groomOpen, {}, {}, QStringLiteral("toggleGuides")};
    b["display.toggleHair"] = {run([m] { m->setHairVisible(!m->hairVisible()); }), [m](const json&) { return m->hairVisible(); },
                               groomOpen, {}, {}, QStringLiteral("toggleHair")};
    // The Properties' Display > Tubes: the tubes shaded in Groom mode whatever the brush.
    b["display.tubesAlways"] = {run([m] { m->tubes_->setChecked(!m->tubes_->isChecked()); }), [m](const json&) { return m->tubes_->isChecked(); },
                                groomOpen, {}, {}, QStringLiteral("tubesAlways")};

    // ---- The radial menu's Masking and Visibility (marking_menu.json) ----
    // Masking: the freeze mask (Full every guide unfrozen, Empty every guide frozen); its Select and
    // Mask Brush are the Select and Freeze tools.
    b["mask.full"] = {run([m] {
                          if (!m->document_) return;
                          m->document_->editGuides("unfreeze all", [](GuideHierarchy& h) { unfreezeAll(h); });
                          m->guidesEdited();
                      }),
                      {}, groomOpen, {}, {}, QStringLiteral("maskFull")};
    b["mask.empty"] = {run([m] {
                           if (!m->document_) return;
                           m->document_->editGuides("freeze all", [](GuideHierarchy& h) { soloGuides(h, {}); });
                           m->guidesEdited();
                       }),
                       {}, groomOpen, {}, {}, QStringLiteral("maskEmpty")};
    // Quick Select: the Select tool for one pick (the rest frozen), then back to the tool mode it
    // came from (the brush as it was: MainWindow's selectionChanged).
    b["mask.quickSelect"] = {run([m] {
                                 const Viewport::ToolMode from = m->viewport_->toolMode();
                                 m->setUiMode(QStringLiteral("groom"));
                                 m->brush_.selectHides = false;
                                 m->viewport_->setToolMode(Viewport::ToolMode::Select);
                                 m->quickSelectBack_ =
                                     static_cast<int>(from == Viewport::ToolMode::Select ? Viewport::ToolMode::Camera : from);
                                 const BrushEntry* e = brushEntry(m->brush_.toolId());
                                 m->notify(from == Viewport::ToolMode::Brush && e
                                               ? QObject::tr("Pick the guides to work on: then back to %1").arg(QString::fromStdString(e->label))
                                               : QObject::tr("Pick the guides to work on"));
                             }),
                             {}, groomOpen, {}, {}, QStringLiteral("maskQuickSelect")};
    // Visibility: guides hidden one by one (kHiddenLayer: hidden guides are locked too). Select is
    // the Select tool hiding what the lasso leaves out (BrushSession::selectHides).
    b["visibility.select"] = {run([m] {
                                  m->setUiMode(QStringLiteral("groom"));
                                  m->viewport_->setToolMode(Viewport::ToolMode::Select);
                                  m->brush_.selectHides = true;
                                  m->notify(QObject::tr("Lasso the guides to keep: every other guide hides (a pick of nothing shows them all)"));
                              }),
                              {}, groomOpen, {}, {}, QStringLiteral("visibilitySelect")};
    b["visibility.showAll"] = {run([m] {
                                   if (!m->document_) return;
                                   const auto h = m->document_->session().authoredHierarchy();
                                   if (!h || !anyGuideHidden(*h)) {
                                       m->notify(QObject::tr("No guide is hidden"));
                                       return;
                                   }
                                   m->document_->editGuides("show all guides", [](GuideHierarchy& g) { unfreezeAll(g, kHiddenLayer); });
                                   m->guidesEdited();
                               }),
                               {}, groomOpen, {}, {}, QStringLiteral("visibilityShowAll")};
    b["visibility.invert"] = {run([m] {
                                  if (!m->document_) return;
                                  m->document_->editGuides("invert hidden guides", [](GuideHierarchy& g) { invertHidden(g); });
                                  m->guidesEdited();
                              }),
                              {}, groomOpen, {}, {}, QStringLiteral("visibilityInvert")};
    // Tube's Draw Root and Edit Root: the Tube card's Draw root and Edit roots (the root mode, a
    // root tube's outline clicked on the scalp, or the roots' outlines edited); picked again, they
    // leave it (Edit: Finish edit), as the card's buttons do.
    auto rootMode = [m](RootTubeTool::Mode a, RootTubeTool::Mode b2) {
        return m->brush_.rootMode() && (m->brush_.roots().mode() == a || m->brush_.roots().mode() == b2);
    };
    auto tubeRoots = [m](bool draw) {
        m->setUiMode(QStringLiteral("groom"));
        if (m->brush_.toolId() != "shape" || m->viewport_->toolMode() != Viewport::ToolMode::Brush) m->chooseTool(QStringLiteral("shape"));
        m->shell_->groomToolsDock()->show();
        m->shell_->groomToolsDock()->raise();
        TubePanel* tubes = m->groomTools_->tubePanel();
        if (!tubes) return;
        if (draw) tubes->drawRoot();
        else tubes->editRoots();
    };
    b["tube.drawRoot"] = {run([tubeRoots] { tubeRoots(true); }),
                          [rootMode](const json&) { return rootMode(RootTubeTool::Mode::Place, RootTubeTool::Mode::Draw); }, groomOpen,
                          {}, {}, QStringLiteral("tubeDrawRoot")};
    b["tube.editRoot"] = {run([tubeRoots] { tubeRoots(false); }),
                          [rootMode](const json&) { return rootMode(RootTubeTool::Mode::Edit, RootTubeTool::Mode::Edit); }, groomOpen,
                          {}, {}, QStringLiteral("tubeEditRoot")};

    // ---- The GroomLab menu ----
    // Groom Tools: the dock shown and raised (Groom mode, where it lives).
    // GroomLab > Radial Menu Settings...: its shortcut and size (showRadialMenuSettings).
    b["radial.settings"] = {run([m] { AppActions::showRadialMenuSettings(*m); }), {}, {}, {}, {}, QStringLiteral("radialSettings")};
    b["tools.open"] = {run([m] {
                           m->setUiMode(QStringLiteral("groom"));
                           m->shell_->groomToolsDock()->show();
                           m->shell_->groomToolsDock()->raise();
                       }),
                       {}, {}, {}, {}, QStringLiteral("groomTools")};
    // The Part tool's card: the part picked in its list removed, every part cleared.
    b["parts.remove"] = {run([m] {
                             const std::string part = m->groomTools_->card()->selectedPart();
                             if (!part.empty()) m->guidesPanel_->removePart(part);
                         }),
                         {}, groomOpen};
    b["parts.clear"] = {run([m] { m->guidesPanel_->clearParts(); }), {}, groomOpen};
    // Utilities > Delete All Flow Arrows (the Flow card's Clear All): every arrow and sink, one undo step.
    b["flow.deleteAll"] = {run([m] {
                               const auto [arrows, sinks] = m->guidesPanel_->flowCount();
                               if (arrows + sinks == 0) {
                                   m->notify(QObject::tr("The groom has no flow arrows"));
                                   return;
                               }
                               if (m->guidesPanel_->clearFlow())
                                   m->notify(QObject::tr("Deleted %1 flow arrows and %2 sinks").arg(arrows).arg(sinks));
                           }),
                           {}, groomOpen};
    b["photos.guides"] = {run([m] {
                              if (ReferenceWindow* w = m->openReferenceWindow())
                                  // (Align while no photo is lined up: guides grow from lined-up photos)
                                  w->setStage(w->anyLinedUp() ? ReferenceWindow::Stage::Guides : ReferenceWindow::Stage::Align);
                          }),
                          {}, groomOpen, {}, {}, QStringLiteral("guidesFromReference")};
    b["groom.transfer"] = {run([m] { m->openTransferWindow(); }), {}, groomOpen, {}, {}, QStringLiteral("transfer")};
    b["cardAtlas.open"] = {run([m] { m->openCardAtlas(); })};
    b["sim.range"] = {run([m] { m->simulateTo(); }), {}, groomOpen};
    b["sim.bake"] = {run([m] { m->bakeSimulation(); }), {}, groomOpen};
    b["sim.clear"] = {run([m] { m->invalidateSimulation(); }), {}, groomOpen};
    for (const char* lod : {"hero", "mid", "low"}) {
        // Every Cards node at that level of detail, one made before the output when there is none.
        b[std::string("add.cards.") + lod] = {run([m, lod = std::string(lod)] {
                                                  if (!m->document_) return;
                                                  const std::string error = m->document_->addCards(lod);
                                                  if (!error.empty()) m->report(QObject::tr("Cannot add a Cards node: %1").arg(QString::fromStdString(error)));
                                                  m->shell_->cardsPanel()->setShowCards(true);
                                                  m->graphEdited();
                                              }),
                                              {}, groomOpen};
    }
    // Review > Density Heatmap: the Display panel's strand colors by density (off: solid).
    b["review.heatmap"] = {[m](const json&, bool on) { m->setDisplayText(on ? QStringLiteral("colorMode=density") : QStringLiteral("colorMode=solid")); },
                           [m](const json&) { return m->colorMode_ && m->colorMode_->currentIndex() == 5; }};
    b["review.turntable"] = {run([m] { m->setTurntable(!m->turntable()); })};
    b["studio.lighting"] = {[m](const json& value, bool) {
                                // In the current mode (switching to Look hid the Groom Tools): the
                                // Properties' Lighting tab, with lookdev on so the lights show.
                                m->applyLightingPreset(str(value));
                                if (m->lookdev_ && !m->lookdev_->isChecked()) m->lookdev_->setChecked(true);
                                m->shell_->setPropertiesTab(AppShell::PropertiesTab::Lighting);
                            },
                            [m](const json& value) { return value.is_string() && m->look_->lighting().presetName == value.get<std::string>(); }};

    // Render: RenderMan (prman) and V-Ray Standalone renders of the groom as the viewport frames
    // and lights it, in the groom's renders folder (Maya's; an untitled groom's: a temp folder of
    // its own); the render window saves them elsewhere too.
    auto renderImage = [m](const QString& renderer, const QString& kind) {
        const QString name = m->document_ && !m->document_->path().empty()
                                 ? QFileInfo(QString::fromStdString(m->document_->path())).completeBaseName()
                                 : QStringLiteral("groom");
        const QString folder = m->document_ ? QString::fromStdString(m->document_->assetFolder("renders"))
                                            : QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        return QDir(folder).filePath(QStringLiteral("%3/%1_%2/%1.png").arg(name, kind, renderer));
    };
    b["render.prmanFrame"] = {run([m, renderImage] { m->renderPrman(renderImage(QStringLiteral("prman"), QStringLiteral("frame")), 0, QSize(), false); }),
                              {}, groomOpen, {}, {}, QStringLiteral("prmanFrame")};
    b["render.prmanTurntable"] = {run([m, renderImage] {
                                      m->renderPrman(renderImage(QStringLiteral("prman"), QStringLiteral("turntable")), 36, QSize(), false);
                                  }),
                                  {}, groomOpen, {}, {}, QStringLiteral("prmanTurntable")};
    b["render.vrayFrame"] = {run([m, renderImage] { m->renderVray(renderImage(QStringLiteral("vray"), QStringLiteral("frame")), 0, QSize(), false); }),
                             {}, groomOpen, {}, {}, QStringLiteral("vrayFrame")};
    b["render.vrayTurntable"] = {run([m, renderImage] {
                                     m->renderVray(renderImage(QStringLiteral("vray"), QStringLiteral("turntable")), 36, QSize(), false);
                                 }),
                                 {}, groomOpen, {}, {}, QStringLiteral("vrayTurntable")};
    b["render.prmanLocation"] = {run([m] { m->choosePrman(); }), {}, {}, {}, {}, QStringLiteral("prmanLocation")};
    b["render.vrayLocation"] = {run([m] { m->chooseVrayStandalone(); }), {}, {}, {}, {}, QStringLiteral("vrayLocation")};

    // Export: Export mode on that target (its panel holds the frames, the options and the output).
    const std::pair<const char*, const char*> exports[] = {{"export.alembic", "alembic"},     {"export.usd", "usd"},
                                                           {"export.vrscene", "vrscene"},     {"export.unrealGroom", "unreal"},
                                                           {"export.unityStrands", "unity"},  {"export.gltfCards", "cards"},
                                                           {"export.cardMaps", "maps"}};
    for (const auto& [id, target] : exports) {
        b[id] = {run([m, target = QString::fromLatin1(target)] {
                     if (!m->document_) return;
                     m->setUiMode(QStringLiteral("export"));
                     m->setExportTarget(target);
                 }),
                 {}, groomOpen};
    }

    // Theme (the shipped and the user's themes): worn at once, saved for the next start.
    b["theme.set"] = {[m](const json& value, bool) { m->chooseTheme(str(value)); },
                      [](const json& value) { return str(value) == theme::currentTheme(); },
                      {},
                      {},
                      {},
                      QStringLiteral("theme_%1")};

    // Help.
    b["help.docs"] = {run([m] { m->openDocs(); })};
    b["help.shortcuts"] = {run([m] { m->showHelp(QStringLiteral("shortcuts")); })};
    b["help.updates"] = {run([m] { m->checkForUpdates(); })};
    b["help.discord"] = {run([] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kDiscordUrl))); }), {}, {}, {}, {}, QStringLiteral("discord")};
    b["help.donate"] = {run([m] { m->openDonate(); }), {}, {}, {}, {}, QStringLiteral("supportGroomLab")};
    b["help.about"] = {run([m] { m->showHelp(QStringLiteral("about")); })};

    // ---- The Graph Editor's menus (graph_ui.json): its Edit menu and the selected nodes' ----
    auto graphOn = [m] { return m->document_ != nullptr && m->graphShown(); };
    b["graph.frame"] = {run([m] { m->graph_->frameSelected(); }), {}, graphOn};
    b["graph.frameAll"] = {run([m] { m->graph_->fitAll(); }), {}, graphOn};
    b["graph.tidy"] = {run([m] { m->graph_->tidyLayout(); }), {}, graphOn};
    b["graph.group"] = {run([m] { m->graph_->groupSelected(); }), {}, graphOn};
    b["graph.ungroup"] = {run([m] { m->graph_->ungroupSelected(); }), {}, graphOn};
    b["graph.delete"] = {run([m] { m->graph_->deleteSelected(); }), {}, graphOn};
    b["graph.copy"] = {run([m] { m->copyGraph(); }), {}, groomOpen};
    b["graph.paste"] = {run([m] { m->pasteGraph(); }), {}, groomOpen};
    b["graph.gpuPreview"] = {[m](const json&, bool on) { m->setGpuPreview(on); }, [m](const json&) { return m->gpuPreview(); }};
    b["graph.reload"] = {run([m] { m->askReload(); }), {}, [m] { return m->document_ && !m->document_->path().empty(); }};
    auto selected = [m] { return m->graph_->selectedNode(); };
    auto nodeOn = [m] { return m->document_ != nullptr && !m->graph_->selectedNode().empty(); };
    b["node.setOutput"] = {run([m] { m->graph_->setSelectedAsOutput(); }), {}, nodeOn};
    b["node.view"] = {run([m] { m->graph_->toggleViewSelected(); }),
                      [m](const json&) { return !m->graph_->selectedNode().empty() && m->graph_->viewedNode() == m->graph_->selectedNode(); },
                      nodeOn};
    b["node.bypass"] = {[m, selected](const json&, bool on) {
                            if (!m->document_ || selected().empty()) return;
                            const std::string error = m->document_->setBypass(selected(), on);
                            if (!error.empty()) m->report(QString::fromStdString(error));
                            m->graphEdited();
                        },
                        [m, selected](const json&) {
                            if (!m->document_) return false;
                            for (const GroomDocument::NodeInfo& n : m->document_->nodes()) {
                                if (n.name == selected()) return n.bypass;
                            }
                            return false;
                        },
                        nodeOn};
    b["node.evaluate"] = {[m, selected](const json& value, bool) {
                              if (!m->document_ || selected().empty()) return;
                              const std::string error = m->document_->setEvaluation(selected(), value.get<std::string>());
                              if (!error.empty()) m->report(QString::fromStdString(error));
                              m->graphEdited();
                          },
                          [m, selected](const json& value) {
                              if (!m->document_ || !value.is_string()) return false;
                              for (const GroomDocument::NodeInfo& n : m->document_->nodes()) {
                                  if (n.name == selected()) return n.evaluation == value.get<std::string>();
                              }
                              return false;
                          },
                          nodeOn};
    b["node.rename"] = {run([m] { m->graph_->renameSelected(); }), {}, nodeOn};
    b["node.duplicate"] = {run([m] { m->graph_->duplicateSelected(); }), {}, nodeOn};
    b["node.delete"] = {run([m] { m->graph_->deleteSelected(); }), {}, nodeOn};
    b["compound.groupSelection"] = {run([m] { m->graph_->groupSelected(); }), {}, nodeOn};
    b["compound.ungroup"] = {run([m] { m->graph_->ungroupSelected(); }), {}, nodeOn};
    b["compound.saveAsNode"] = {run([m, selected] { m->graph_->openSaveNode(selected()); }), {}, nodeOn};
    b["compound.saveToLibrary"] = {run([m, selected] { m->graph_->saveToLibrary(selected()); }), {}, nodeOn};
    b["backdrop.add"] = {run([m] { m->graph_->addBackdrop(); }), {}, groomOpen};

    // Presets (graph_ui.json's presetMenu and presetItem; the generator gives each preset's path).
    b["preset.apply"] = {[m](const json& value, bool) {
        if (m->document_) m->applyPreset(str(value));
        else m->startFromPreset(str(value));  // no groom open: the preset on a new groom (a mesh asked for)
    }};
    b["preset.insert"] = {[m](const json& value, bool) { m->insertPreset(str(value)); }, {}, groomOpen};
    b["preset.save"] = {run([m] {
                            if (!m->graphShown()) m->setGraphShown(true);
                            m->graph_->promptName(QObject::tr("Save this graph as a preset"), QObject::tr("Preset name"),
                                                  [m](const QString& name) { m->savePreset(name); });
                        }),
                        {}, groomOpen};
    addListBindings(b, w);
    addPhotoBindings(b, w);
    addGraphBindings(b, w);
    addOutputBindings(b, w);
    return b;
}

ui::Generators AppActions::generators(MainWindow& w) {
    MainWindow* m = &w;
    ui::Generators g;
    // The themes (theme.set with the theme's name): the shipped ones and the user's, worn at once.
    g["themes"] = [](const json& entry) {
        json out = json::array();
        for (const QString& name : theme::themeNames()) {
            const QString label = theme::themeLabel(name);
            const QString tip = QObject::tr("Restyle GroomLab with the %1 theme (remembered across sessions)").arg(label);
            out.push_back({{"id", entry.value("action", std::string("theme.set"))},
                           {"value", name.toStdString()},
                           {"label", label.toStdString()},
                           {"tip", tip.toStdString()}});
        }
        return out;
    };
    // The Look card's lighting presets (GroomLab > Studio in the app).
    g["lightingPresets"] = [](const json& entry) {
        const std::string action = entry.value("action", std::string("studio.lighting"));
        const std::string tip = ui::Definitions::shipped().action(action).value("tip", std::string());
        json out = json::array();
        for (const std::string& name : lightingPresetNames()) {
            const std::string label = lightingLabel(name).toStdString();
            out.push_back({{"id", action}, {"value", name}, {"label", label}, {"tip", ui::fill(tip, {{"label", label}})}});
        }
        return out;
    };
    // The graph presets: a submenu per category (presetCategories first, in order), then the
    // presets with no category (the user's own); each a submenu of Apply and Insert, its
    // description the tip.
    g["presets"] = [m](const json&) {
        std::map<std::string, std::vector<PresetInfo>> byCategory;
        for (const PresetInfo& info : availablePresets()) byCategory[info.category].push_back(info);
        std::vector<std::string> order;
        for (const std::string& c : presetCategories()) {
            if (byCategory.count(c)) order.push_back(c);
        }
        for (const auto& [category, infos] : byCategory) {
            if (!category.empty() && std::find(order.begin(), order.end(), category) == order.end()) order.push_back(category);
        }
        const bool open = m->document_ != nullptr;
        auto preset = [open](const PresetInfo& info) {
            json apply = {{"id", "preset.apply"}, {"value", info.path}};
            if (!open) {
                apply["label"] = QObject::tr("Apply on a Mesh...").toStdString();
                apply["tip"] = QObject::tr("Pick a mesh (.obj or .abc): a new groom is made on it with this look").toStdString();
            }
            return json{{"submenu", info.title},
                        {"id", "preset/" + info.name},
                        {"tip", info.description.empty() ? info.name : info.description},
                        {"items", json::array({apply, {{"id", "preset.insert"}, {"value", info.path}}})}};
        };
        json out = json::array();
        for (const std::string& category : order) {
            json items = json::array();
            for (const PresetInfo& info : byCategory[category]) items.push_back(preset(info));
            out.push_back({{"submenu", category}, {"id", "presets/" + category}, {"items", items}});
        }
        if (byCategory.count(std::string())) {
            if (!order.empty()) out.push_back("-");
            for (const PresetInfo& info : byCategory[std::string()]) out.push_back(preset(info));
        }
        return out;
    };
    addGraphGenerators(g, w);
    addOutputGenerators(g, w);
    return g;
}

std::map<std::string, ui::MenuFiller> AppActions::fillers(MainWindow& w) {
    MainWindow* m = &w;
    std::map<std::string, ui::MenuFiller> f;
    // The Theme submenu (menus.json "theme"), where a container shows it as one generator.
    f["themeMenu"] = [m](QMenu* menu, const ui::Item&) {
        const ui::Definitions& d = ui::Definitions::shipped();
        const json& container = d.get("menus/theme");
        auto* sub = new QMenu(QString::fromStdString(ui::hostText("label", ui::kHost, {&container})), menu);
        sub->setObjectName(QStringLiteral("themeMenu"));
        sub->menuAction()->setToolTip(QString::fromStdString(ui::hostText("tip", ui::kHost, {&container})));
        menu->addMenu(sub);
        ui::FillOptions options = AppActions::fillOptions(*m);
        options.fillers.erase("themeMenu");
        ui::fillMenu(sub, container, AppActions::bindings(*m), options, d);
    };
    // View > Panels: the docks' toggles and their sections (app_shell.h).
    f["panels"] = [m](QMenu* menu, const ui::Item&) { m->fillPanelsMenu(menu); };
    addGraphFillers(f, w);
    addOutputFillers(f, w);
    return f;
}

std::set<std::string> AppActions::state(MainWindow& w) {
    std::set<std::string> s{"catalog"};
    if (w.document_) s.insert("groom");
    return s;
}

ui::FillOptions AppActions::fillOptions(MainWindow& w) {
    ui::FillOptions options;
    options.resolve.state = state(w);
    options.resolve.values = {{"donateUrl", kDonateUrl}};
    options.resolve.generators = generators(w);
    options.fillers = fillers(w);
    options.shortcutOwners = std::make_shared<std::map<std::string, QPointer<QAction>>>();
    return options;
}

// ---- The radial menu (marking_menu.json's radialMenu, as GroomBear's and RadialZ's) ---------------

namespace {

QString radialIcon(const std::string& stem) {
    if (stem.empty()) return {};
    const QString s = QString::fromStdString(stem);
    const QString path = s.startsWith(QLatin1String("ui_")) ? QStringLiteral(":/groomlab/icons/%1.svg").arg(s)
                                                            : QStringLiteral(":/groomlab/icons/groomLab_%1.svg").arg(s);
    return QFile::exists(path) ? path : QString();
}

// The brush options the radial menu sets (radialMenu.brushOptions): the switches, and the settings
// picked among values (choices.json).
bool* optionSwitch(BrushToolSettings& s, const std::string& setting) {
    if (setting == "stretch") return &s.brush.stretch;
    if (setting == "straighten") return &s.brush.straighten;
    if (setting == "collide") return &s.brush.collide;
    if (setting == "pressure") return &s.pressure;
    return nullptr;
}

std::string optionValue(const BrushToolSettings& s, const std::string& setting) {
    if (setting == "smoothMode") return smoothModeName(s.brush.smoothMode);
    if (setting == "tubeChannel") return tubeChannelName(s.brush.tubeChannel);
    return {};
}

bool setOptionValue(BrushToolSettings& s, const std::string& setting, const std::string& value) {
    try {
        if (setting == "smoothMode") s.brush.smoothMode = smoothModeFromString(value);
        else if (setting == "tubeChannel") s.brush.tubeChannel = tubeChannelFromString(value);
        else return false;
    } catch (const std::invalid_argument&) {
        return false;
    }
    return true;
}

}  // namespace

std::vector<MarkingMenu::Item> AppActions::radialMenuItems(MainWindow& w) {
    using Item = MarkingMenu::Item;
    const ui::Definitions& d = ui::Definitions::shipped();
    const ui::Bindings bound = bindings(w);
    ui::ResolveOptions options;
    options.state = state(w);
    options.keep = [&bound](const std::string& id) { return bound.count(id) > 0; };
    // An action of the definitions, bound here (else none), with its state when it has one.
    auto action = [&](const std::string& id, const json& entry) -> std::optional<Item> {
        for (const ui::Item& resolved : d.resolve(json::array({id}), options)) {
            if (resolved.kind != ui::Item::Kind::Action) continue;
            Item item;
            item.kind = Item::Kind::Action;
            item.id = resolved.id;
            item.label = QString::fromStdString(entry.value("label", resolved.label));
            item.tip = QString::fromStdString(resolved.tip);
            item.icon = radialIcon(entry.value("icon", resolved.icon));
            item.enabled = resolved.enabled;
            const auto b = bound.find(id);
            if (b != bound.end() && b->second.checked) {
                item.checked = b->second.checked(json());
                // With offLabel the label says the state (Hide Hair while the hair shows, Show Hair
                // while not): no check mark. Else drawn checked or not.
                if (entry.contains("offLabel")) {
                    if (!item.checked) item.label = QString::fromStdString(entry.at("offLabel").get<std::string>());
                    item.checked = false;
                } else {
                    item.checkable = true;
                }
            }
            if (b != bound.end() && b->second.enabled) item.enabled = item.enabled && b->second.enabled();
            return item;
        }
        return std::nullopt;
    };
    auto tool = [&](const std::string& id, const json& entry) -> std::optional<Item> {
        const BrushEntry* e = brushEntry(id);
        if (!e) return std::nullopt;
        Item item;
        item.kind = Item::Kind::Tool;
        item.id = id;
        item.label = QString::fromStdString(entry.value("label", e->label));
        item.tip = QString::fromStdString(e->hint);
        item.icon = radialIcon(e->icon);
        item.color = theme::categoryColor(e->category);
        return item;
    };
    // The picked brush's options, at the positions in order.
    const json& brushOptions = d.get("radialMenu/brushOptions");
    auto brushOptionsOf = [&](Item& submenu) {
        const std::string current = w.brush_.toolId();
        const BrushEntry* e = brushEntry(current);
        const QString name = e ? QString::fromStdString(e->label) : QObject::tr("the brush");
        const json& tools = brushOptions.at("tools");
        const auto rows = tools.find(current);
        const json& positions = brushOptions.at("positions");
        BrushToolSettings& s = w.brush_.settings();
        if (rows != tools.end()) {
            std::size_t k = 0;
            for (const json& row : *rows) {
                if (k >= positions.size()) break;
                std::optional<Item> item;
                if (row.contains("action")) {
                    item = action(row.at("action").get<std::string>(), row);
                } else {
                    const std::string setting = row.value("setting", std::string());
                    const std::string value = row.value("value", std::string());
                    Item option;
                    option.kind = Item::Kind::Option;
                    option.id = setting;
                    option.value = value;
                    option.checkable = true;
                    const BrushSettingSpec* spec = brushSettingSpec(setting);
                    if (value.empty()) {
                        const bool* on = optionSwitch(s, setting);
                        if (!on) continue;
                        option.checked = *on;
                        option.label = QString::fromStdString(row.value("label", spec ? spec->label : setting));
                        option.tip = spec ? QString::fromStdString(spec->hint) : QString();
                    } else {
                        option.checked = optionValue(s, setting) == value;
                        option.label = QString::fromStdString(row.value("label", value));
                        for (const json& c : d.choices(setting)) {
                            if (c.value("value", std::string()) != value) continue;
                            option.label = QString::fromStdString(row.value("label", c.value("label", value)));
                            option.tip = QString::fromStdString(c.value("tip", std::string()));
                        }
                    }
                    item = option;
                }
                if (!item) continue;
                item->position = MarkingMenu::positionIndex(positions[k++].get<std::string>());
                submenu.items.push_back(*item);
            }
        }
        if (submenu.items.empty()) {
            submenu.enabled = false;
            submenu.tip = QObject::tr("%1 has no options here (pick Comb, Smooth or Tube)").arg(name);
        } else {
            submenu.tip = QObject::tr("%1's options").arg(name);
        }
    };
    // Visibility > Groups: the groom's guide groups (parents before children), each picked to hide
    // or show it (the Guides panel's eye), drawn in its color; past eight, the eighth a More ring.
    auto guideGroupsOf = [&](Item& submenu) {
        const auto h = w.document_ ? w.document_->session().authoredHierarchy() : nullptr;
        if (!h || h->groups().empty()) {
            submenu.tip = QObject::tr("The groom has no guide groups yet");
            return;
        }
        const std::set<std::string>& hidden = w.document_->hiddenGroups();
        const std::map<std::string, std::string>& colors = w.document_->groupColors();
        const auto automatic = autoGuideColors(*h, guideHueSeed(w.document_->guideColorSeed()));
        auto shown = [](const std::array<float, 3>& linear) {
            auto channel = [](float c) {
                c = std::clamp(c, 0.0f, 1.0f);
                return static_cast<int>(std::lround((c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f) * 255.0f));
            };
            return QColor(channel(linear[0]), channel(linear[1]), channel(linear[2]));
        };
        std::vector<Item> rows;
        for (const GuideGroup& group : h->groups()) {
            Item row;
            row.kind = Item::Kind::Option;
            row.id = "guideGroup";
            row.value = group.name;
            row.label = QString::fromStdString(group.name);
            row.checkable = true;
            row.checked = !hidden.count(group.name);
            row.tip = row.checked ? QObject::tr("Hide %1 (its hair still grows)").arg(row.label) : QObject::tr("Show %1").arg(row.label);
            const auto own = colors.find(group.name);
            const auto a = automatic.find(group.id);
            row.color = own != colors.end() ? QColor(QString::fromStdString(own->second))
                        : group.hasColor    ? shown(group.color)
                        : a != automatic.end() ? shown(a->second)
                                               : QColor();
            rows.push_back(std::move(row));
        }
        // Eight to a ring, clockwise from N; with more, the last place opens the rest.
        std::function<void(Item&, std::size_t)> fill = [&](Item& into, std::size_t from) {
            const std::size_t left = rows.size() - from;
            const std::size_t here = left > 8 ? 7 : left;
            for (std::size_t k = 0; k < here; ++k) {
                Item row = rows[from + k];
                row.position = static_cast<int>(k);
                into.items.push_back(std::move(row));
            }
            if (left > 8) {
                Item more;
                more.kind = Item::Kind::Submenu;
                more.id = "guideGroups.more";
                more.label = QObject::tr("More");
                more.tip = QObject::tr("The other %1 groups").arg(left - 7);
                more.position = 7;
                fill(more, from + 7);
                into.items.push_back(std::move(more));
            }
        };
        fill(submenu, 0);
    };
    std::function<std::vector<Item>(const json&)> ring = [&](const json& entries) {
        std::vector<Item> out;
        for (const json& entry : entries) {
            std::optional<Item> item;
            if (entry.contains("tool")) {
                item = tool(entry.at("tool").get<std::string>(), entry);
            } else if (entry.contains("action")) {
                item = action(entry.at("action").get<std::string>(), entry);
            } else {
                Item submenu;
                submenu.kind = Item::Kind::Submenu;
                submenu.id = entry.value("generator", entry.value("label", std::string()));
                submenu.label = QString::fromStdString(entry.value("label", std::string()));
                submenu.tip = QString::fromStdString(entry.value("tip", std::string()));
                submenu.icon = radialIcon(entry.value("icon", std::string()));
                if (entry.value("generator", std::string()) == "brushOptions") brushOptionsOf(submenu);
                else if (entry.value("generator", std::string()) == "guideGroups") guideGroupsOf(submenu);
                else if (entry.contains("items")) submenu.items = ring(entry.at("items"));
                if (submenu.items.empty()) submenu.enabled = false;
                item = submenu;
            }
            if (!item) continue;
            item->position = MarkingMenu::positionIndex(entry.value("position", std::string()));
            out.push_back(*item);
        }
        return out;
    };
    return ring(d.get("radialMenu/items"));
}

void AppActions::runMarkingMenuItem(MainWindow& w, const MarkingMenu::Item& item) {
    using Kind = MarkingMenu::Item::Kind;
    if (item.kind == Kind::Action) {
        const ui::Bindings bound = bindings(w);
        if (const auto it = bound.find(item.id); it != bound.end() && it->second.trigger) it->second.trigger(nullptr, !item.checked);
        return;
    }
    if (item.kind == Kind::Option && item.id == "guideGroup") {
        // Visibility > Groups: the group hidden or shown, as the Guides panel's eye does.
        if (!w.document_) return;
        w.document_->setGroupHidden(item.value, item.checked);
        w.settings_.hiddenGuideGroups = w.document_->hiddenGroupsText();
        w.redisplay();
        w.guidesPanel_->invalidate();
        w.notify(item.checked ? QObject::tr("%1 hidden").arg(item.label) : QObject::tr("%1 shown").arg(item.label));
        return;
    }
    if (item.kind == Kind::Option) {
        BrushToolSettings& s = w.brush_.settings();
        if (bool* on = optionSwitch(s, item.id); on && item.value.empty()) *on = !*on;
        else if (!setOptionValue(s, item.id, item.value)) return;
        w.groomTools_->refresh();
        w.viewport_->refreshOverlay();
        return;
    }
    if (item.kind != Kind::Tool) return;
    // A tool: Groom mode, where the tools work; Select is the Select tool (never its toggle back to
    // the camera, as its tile does when picked again), selecting guides to work on (Masking).
    w.setUiMode(QStringLiteral("groom"));
    if (item.id == "select") {
        w.brush_.selectHides = false;
        w.viewport_->setToolMode(Viewport::ToolMode::Select);
    } else {
        w.chooseTool(QString::fromStdString(item.id));
    }
}

QDialog* AppActions::showRadialMenuSettings(MainWindow& w) {
    if (auto* open = w.findChild<QDialog*>(QStringLiteral("radialMenuSettings"))) {
        open->show();
        open->raise();
        open->activateWindow();
        return open;
    }
    auto* dialog = new QDialog(&w);
    dialog->setObjectName(QStringLiteral("radialMenuSettings"));
    dialog->setWindowTitle(QObject::tr("Radial Menu Settings"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* form = new QFormLayout;
    // The shortcut: one key with its modifiers (hold it, move, let go to pick).
    auto* keys = new QKeySequenceEdit(w.radialMenuShortcut(), dialog);
    keys->setObjectName(QStringLiteral("radialShortcut"));
    keys->setMaximumSequenceLength(1);
    keys->setClearButtonEnabled(true);
    keys->setToolTip(QObject::tr("Click and press the keys that open the radial menu (Alt+S to start with). Hold them, move "
                                 "onto an item and let go of the key to pick it"));
    auto* clash = new QLabel(dialog);
    clash->setObjectName(QStringLiteral("radialShortcutClash"));
    clash->setWordWrap(true);
    // The size, as a percentage of the designed one.
    auto* size = new QSpinBox(dialog);
    size->setObjectName(QStringLiteral("radialScale"));
    size->setRange(static_cast<int>(MarkingMenu::kMinScale * 100.0), static_cast<int>(MarkingMenu::kMaxScale * 100.0));
    size->setSingleStep(10);
    size->setSuffix(QStringLiteral(" %"));
    size->setValue(static_cast<int>(std::lround(w.markingMenu()->scale() * 100.0)));
    size->setToolTip(QObject::tr("How big the radial menu draws: its pills, text and reach (the mouse wheel over the open "
                                 "menu sizes it too)"));
    form->addRow(QObject::tr("Shortcut"), keys);
    form->addRow(QString(), clash);
    form->addRow(QObject::tr("Size"), size);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Close, dialog);
    auto* layout = new QVBoxLayout(dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    // A clash with a shortcut of the menus: neither would run (Qt drops an ambiguous shortcut).
    auto check = [&w, clash](const QKeySequence& seq) {
        QStringList users;
        for (QAction* a : w.findChildren<QAction*>()) {
            if (!seq.isEmpty() && a->shortcut() == seq) users << a->text().remove(QLatin1Char('&'));
        }
        clash->setText(users.isEmpty() ? QString()
                                       : QObject::tr("Also the shortcut of %1: pick another").arg(users.join(QStringLiteral(", "))));
        clash->setVisible(!users.isEmpty());
    };
    check(w.radialMenuShortcut());
    QObject::connect(keys, &QKeySequenceEdit::editingFinished, dialog, [&w, keys, check] {
        w.setRadialMenuShortcut(keys->keySequence());
        if (keys->keySequence() != w.radialMenuShortcut()) keys->setKeySequence(w.radialMenuShortcut());  // (empty: Alt+S)
        check(w.radialMenuShortcut());
    });
    QObject::connect(size, &QSpinBox::valueChanged, dialog, [&w](int percent) { w.setRadialMenuScale(percent / 100.0); });
    // The wheel over the menu: the box follows.
    QObject::connect(w.markingMenu(), &MarkingMenu::scaleChanged, size, [size](qreal scale) {
        const QSignalBlocker block(size);
        size->setValue(static_cast<int>(std::lround(scale * 100.0)));
    });
    QObject::connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, dialog, [&w, keys, size, check] {
        w.setRadialMenuShortcut(QKeySequence(Qt::ALT | Qt::Key_S));
        keys->setKeySequence(w.radialMenuShortcut());
        check(w.radialMenuShortcut());
        size->setValue(100);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    dialog->show();
    return dialog;
}

std::map<std::string, QAction*> buildDefinitionsMenuBar(QMenuBar* bar, MainWindow& window) {
    const ui::Definitions& d = ui::Definitions::shipped();
    return ui::fillMenuBar(bar, d.get("menus/app.menuBar"), AppActions::bindings(window), AppActions::fillOptions(window), d);
}

}  // namespace groomlab::app
