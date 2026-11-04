// SPDX-License-Identifier: Apache-2.0
#include "app_actions.h"

#include <algorithm>
#include <optional>

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QUrl>

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
#include "viewport.h"

#include "groomlab/brush_catalog.h"
#include "groomlab/presets.h"

namespace groomlab::app {

namespace {

using ui::json;

QString str(const json& value) { return value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString(); }

// The Look card's lighting presets by name, as the Studio menu shows them.
QString lightingLabel(const std::string& name) {
    if (name == "env") return QObject::tr("Environment");
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
        b["view.mode." + name.toStdString()] = {[m, name](const json&, bool) { m->setUiMode(name); },
                                                [m, mode](const json&) { return m->shell_->mode() == mode; },
                                                {},
                                                {},
                                                {QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i))},
                                                QStringLiteral("mode_") + name};
    }
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
    b["display.toggleGuides"] = {run([m] { m->setGuidesVisible(!m->guidesVisible()); }), {}, groomOpen, {}, {}, QStringLiteral("toggleGuides")};

    // ---- The GroomLab menu ----
    // Groom Tools: the dock shown and raised (Groom mode, where it lives).
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
    b["sim.range"] = {run([m] { m->simulateTo(); }), {}, groomOpen};
    b["sim.bake"] = {run([m] { m->bakeSimulation(); }), {}, groomOpen};
    b["sim.clear"] = {run([m] { m->invalidateSimulation(); }), {}, groomOpen};
    for (const char* lod : {"hero", "mid", "low"}) {
        // A Cards node before the output at that level of detail (the Cards card's Add Cards + LOD).
        b[std::string("add.cards.") + lod] = {run([m, lod = std::string(lod)] {
                                                  if (!m->document_) return;
                                                  std::string made;
                                                  std::string error = m->document_->addCardsNode(&made);
                                                  if (error.empty()) error = m->document_->applyCardPreset(made, lod);
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
    // and lights it (the images in the temp folder: the render window saves them elsewhere).
    auto renderImage = [m](const QString& renderer, const QString& kind) {
        const QString name = m->document_ && !m->document_->path().empty()
                                 ? QFileInfo(QString::fromStdString(m->document_->path())).completeBaseName()
                                 : QStringLiteral("groom");
        return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("GroomLab-%3/%1_%2/%1.png").arg(name, kind, renderer));
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

// ---- The marking menu (marking_menu.json, as Maya's hotkeys.py) -------------------------------------

std::vector<MarkingMenu::Item> AppActions::markingMenuItems(MainWindow& w) {
    using Item = MarkingMenu::Item;
    const ui::Definitions& d = ui::Definitions::shipped();
    auto icon = [](const std::string& stem) {
        const QString path = QStringLiteral(":/groomlab/icons/groomLab_%1.svg").arg(QString::fromStdString(stem));
        return stem.empty() || !QFile::exists(path) ? QString() : path;
    };
    auto tool = [&](const std::string& id, Item::Kind kind) -> std::optional<Item> {
        const BrushEntry* e = brushEntry(id);
        if (!e) return std::nullopt;
        Item item;
        item.kind = kind;
        item.id = id;
        item.label = QString::fromStdString(e->label);
        item.tip = QString::fromStdString(e->hint);
        item.icon = icon(e->icon);
        item.color = theme::categoryColor(e->category);
        return item;
    };
    std::vector<Item> out;
    // The brushes reached for most around the cursor.
    std::set<std::string> radial;
    for (const json& entry : d.get("markingMenu/radial")) {
        const std::string id = entry.value("tool", std::string());
        radial.insert(id);
        if (std::optional<Item> item = tool(id, Item::Kind::Radial)) {
            item->position = MarkingMenu::positionIndex(entry.value("position", std::string()));
            out.push_back(*item);
        }
    }
    // The rest of the catalog in its order (but what the overflow excludes: Select and Freeze,
    // which the tail lists).
    const json& overflow = d.get("markingMenu/overflow");
    if (overflow.value("generator", std::string()) == "brushCatalog") {
        std::set<std::string> exclude;
        if (const auto it = overflow.find("exclude"); it != overflow.end()) exclude = it->get<std::set<std::string>>();
        for (const BrushEntry& e : brushCatalog()) {
            if (radial.count(e.id) || exclude.count(e.id)) continue;
            if (std::optional<Item> item = tool(e.id, Item::Kind::Overflow)) out.push_back(*item);
        }
    }
    // The tail: tools, and the definitions' actions with a binding here (the unbound hidden).
    const ui::Bindings bound = bindings(w);
    ui::ResolveOptions options;
    options.state = state(w);
    options.keep = [&bound](const std::string& id) { return bound.count(id) > 0; };
    for (const json& entry : d.get("markingMenu/tail")) {
        if (entry.is_object() && entry.contains("tool")) {
            if (std::optional<Item> item = tool(entry.at("tool").get<std::string>(), Item::Kind::Tail)) out.push_back(*item);
            continue;
        }
        for (const ui::Item& resolved : d.resolve(json::array({entry}), options)) {
            if (resolved.kind != ui::Item::Kind::Action) continue;
            Item item;
            item.kind = Item::Kind::Tail;
            item.id = resolved.id;
            item.tool = false;
            item.label = QString::fromStdString(resolved.label);
            item.tip = QString::fromStdString(resolved.tip);
            item.icon = icon(resolved.icon);
            out.push_back(item);
        }
    }
    return out;
}

void AppActions::runMarkingMenuItem(MainWindow& w, const MarkingMenu::Item& item) {
    if (!item.tool) {
        const ui::Bindings bound = bindings(w);
        if (const auto it = bound.find(item.id); it != bound.end() && it->second.trigger) it->second.trigger(nullptr, false);
        return;
    }
    // A tool: Groom mode, where the tools work; Select is the Select tool (never its toggle back to
    // the camera, as its tile does when picked again).
    w.setUiMode(QStringLiteral("groom"));
    if (item.id == "select") w.viewport_->setToolMode(Viewport::ToolMode::Select);
    else w.chooseTool(QString::fromStdString(item.id));
}

std::map<std::string, QAction*> buildDefinitionsMenuBar(QMenuBar* bar, MainWindow& window) {
    const ui::Definitions& d = ui::Definitions::shipped();
    return ui::fillMenuBar(bar, d.get("menus/app.menuBar"), AppActions::bindings(window), AppActions::fillOptions(window), d);
}

}  // namespace groomlab::app
