// SPDX-License-Identifier: Apache-2.0
// MainWindow: the document (time, cards, exports, files), its evaluations and its edits.

#include "reference_window.h"
#include "transfer_window.h"
#include "main_window_internal.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <QCheckBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>

#include "definitions.h"
#include "groom_tools.h"
#include "card_preview.h"
#include "cards_panel.h"
#include "correctives_panel.h"
#include "export_panel.h"
#include "glass.h"
#include "guides_panel.h"
#include "maps_panel.h"
#include "theme.h"
#include "mesh_io.h"
#include "node_graph.h"
#include "overlay.h"
#include "photos_panel.h"
#include "param_panel.h"
#include "viewport.h"

#include "groomlab/graph_io.h"

namespace groomlab::app {

// ---- Time ------------------------------------------------------------------------------------------

void MainWindow::refreshTimeline() {
    Timeline* timeline = shell_->timeline();
    const bool animated = document_ && document_->animated();
    if (animated) {
        // The caches' span; a graph that reads the time without a cache (Wind, Jiggle on a
        // still scalp) gets a hundred frames to play with.
        double first = 1.0, last = 100.0;
        document_->cacheFrameRange(&first, &last);
        timeline->setFps(document_->framesPerSecond());
        timeline->setRange(first, last);
        timeline->setFrame(document_->frame());
        const MeshPtr& scalp = document_->file().data.scalp;
        timeline->setFollowScalp(document_->followScalp(), scalp && scalp->hasRest());
    } else if (timeline->playing()) {
        timeline->setPlaying(false);
    }
    shell_->setTimelineShown(animated);
    refreshSimulation();
    refreshExportState();
}

void MainWindow::refreshSimulation() {
    Timeline* timeline = shell_->timeline();
    const bool shown = document_ && !document_->simulateNodes().empty();
    timeline->setSimulationShown(shown);
    if (!shown) return;
    FrameScrubber::Runs cached, baked;
    for (const SimStatus& s : document_->simulationStatus()) {
        for (const auto& [first, last] : s.ranges) cached.emplace_back(first, last);
        if (s.baked) baked.emplace_back(s.bakedFirst, s.bakedLast);
    }
    timeline->setSimulatedFrames(std::move(cached), std::move(baked));
}

bool MainWindow::simulateTo(double lastFrame) { return runSimulation(lastFrame, false); }

bool MainWindow::bakeSimulation(double lastFrame) { return runSimulation(lastFrame, true); }

bool MainWindow::runSimulation(double lastFrame, bool bake) {
    if (refuseWhileBusy(bake ? tr("baking the simulation") : tr("simulating"))) return false;
    settleCommit();
    if (!document_) {
        report(tr("No groom open to simulate"));
        return false;
    }
    if (document_->simulateNodes().empty()) {
        report(tr("The graph has no Simulate node: add one between the guides and Interpolate"));
        return false;
    }
    Timeline* timeline = shell_->timeline();
    if (timeline->playing()) timeline->setPlaying(false);
    const double last = lastFrame >= 0.0 ? lastFrame : timeline->last();
    timeline->setSimulationBusy(true);
    beginJob(bake ? tr("bake of the simulation") : tr("simulation"), timeline->simulateButton());
    // The strip grows as frames are made: the run is on this thread (the session is used from
    // one thread at a time), the events in between, as an export's: Cancel's click gets through.
    const SimProgress progress = [this](int done, int count) {
        if (done % 4 == 0 || done == count) refreshSimulation();
        pumpJobEvents();
        return !exportCancel_;
    };
    bool ok = true;
    QString said;
    try {
        if (bake) {
            const std::vector<std::string> files = document_->bakeSimulation(last, progress);
            said = tr("Baked the simulation to frame %1: %2").arg(last).arg(QString::fromStdString(files.front()));
        } else {
            const SimRun run = document_->simulateTo(last, progress);
            if (!run.errors.empty()) throw std::runtime_error(run.errors.front());
            said = (run.cancelled ? tr("Stopped after %1 frames in %2 s (%3 ms a frame): the frames made stay in the cache")
                                  : tr("Simulated %1 frames in %2 s (%3 ms a frame)"))
                       .arg(run.frames)
                       .arg(run.milliseconds / 1000.0, 0, 'f', 2)
                       .arg(run.frames > 0 ? run.milliseconds / run.frames : 0.0, 0, 'f', 1);
            if (run.cancelled) {
                ok = false;
                notify(said);
            }
        }
    } catch (const std::exception& e) {
        ok = false;
        report((bake ? tr("Cannot bake the simulation: %1") : tr("Cannot simulate: %1")).arg(QString::fromUtf8(e.what())));
    }
    timeline->setSimulationBusy(false);
    endJob();
    if (ok) notify(said);
    reevaluate();
    refreshSimulation();
    refreshTitle();
    return ok;
}

void MainWindow::invalidateSimulation() {
    if (!document_ || refuseWhileBusy(tr("clearing the simulation"))) return;
    document_->invalidateSimulation();
    notify(tr("Simulation cleared: the next frame shown simulates again"));
    reevaluate();
    refreshSimulation();
    refreshTitle();
}

void MainWindow::showFrame(double frame) {
    if (!document_) return;
    document_->setFrame(frame);
    reevaluate(true);
    ++shownFrames_;
    refreshExportState();
    correctives_->frameChanged();  // a shot sculpt records at the frame shown
}

bool MainWindow::setFrame(double frame) {
    if (!document_) {
        report(tr("No groom open to go to frame %1").arg(frame));
        return false;
    }
    document_->setFrame(frame);
    shell_->timeline()->setFrame(frame);
    reevaluate();
    refreshExportState();
    return true;
}

bool MainWindow::setFollowScalp(bool on) {
    if (!document_) {
        report(tr("No groom open to follow the scalp"));
        return false;
    }
    document_->setFollowScalp(on);
    refreshTimeline();
    reevaluate();
    refreshTitle();
    return true;
}

// ---- Cards -----------------------------------------------------------------------------------------

void MainWindow::setShowCards(bool on) {
    shell_->cardsPanel()->setShowCards(on);
    reevaluate();
}

void MainWindow::refreshCards() {
    CardsPanel* panel = shell_->cardsPanel();
    if (!document_) {
        panel->setStats(0, 0, 0, false);
        return;
    }
    const ViewResult& view = document_->lastView();
    std::size_t cards = 0, triangles = 0, textures = 0;
    for (const CardNodeSummary& n : view.cardNodes) {
        cards += n.cards;
        triangles += n.triangles;
        textures += n.textures;
    }
    panel->setStats(cards, triangles, textures, document_->bakeStale());
}

void MainWindow::bakeCards() {
    if (!document_) return;
    CardsPanel* panel = shell_->cardsPanel();
    bakeCardsTo(QString::fromStdString(document_->defaultBakeDirectory()), QString::fromStdString(document_->defaultBakeName()),
                panel->bakeResolution(), panel->bakeOcclusion());
}

bool MainWindow::bakeCardsTo(const QString& directory, const QString& name, int resolution, float occlusion) {
    if (refuseWhileBusy(tr("baking the cards"))) return false;
    settleCommit();
    if (!document_) {
        report(tr("No groom open to bake cards from"));
        return false;
    }
    const std::string dir = directory.isEmpty() ? document_->defaultBakeDirectory() : directory.toStdString();
    const std::string base = name.isEmpty() ? document_->defaultBakeName() : name.toStdString();
    QElapsedTimer timer;
    timer.start();
    CardBakeFiles files;
    try {
        working(tr("Baking maps..."));
        const BusyCursor wait;
        files = document_->bakeCards(dir, base, resolution, occlusion);
    } catch (const std::exception& e) {
        report(tr("Cannot bake the cards: %1").arg(QString::fromUtf8(e.what())));
        shell_->cardsPanel()->setBakeStatus(tr("Bake failed: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    notify(tr("Baked %1 cards (%2 textures) at %3 px in %4 ms to %5")
               .arg(files.cards)
               .arg(files.textures)
               .arg(files.size)
               .arg(timer.elapsed())
               .arg(QString::fromStdString(dir)));
    // The cards show with their new maps: the view takes the bake's atlas on its next build, and
    // the textured cards are made again (a bake under the same name rewrites the same preview
    // file; the renderer loads it afresh for a new surface).
    cardSurface_.reset();
    cardSurfaceMap_.clear();
    applyProps();
    shell_->cardsPanel()->setShowCards(true);
    reevaluate();
    shell_->cardsPanel()->refresh();
    refreshCards();
    return true;
}

// ---- Exports ----------------------------------------------------------------------------------------

void MainWindow::refreshExportState() {
    ExportPanel* panel = shell_->exportPanel();
    const Timeline* timeline = shell_->timeline();
    const bool animated = document_ && document_->animated();
    panel->setDocumentState(document_ != nullptr, document_ ? document_->frame() : 1.0, animated,
                            animated ? timeline->first() : 1.0, animated ? timeline->last() : 1.0,
                            document_ ? QString::fromStdString(document_->path()) : QString());
}

bool MainWindow::setExportTarget(const QString& target) {
    ExportPanel::Target t;
    if (!ExportPanel::targetFromName(target.toLower().toStdString(), &t)) {
        report(tr("No export target '%1' (alembic, usd, vrscene, unreal, unity, cards, maps)").arg(target));
        return false;
    }
    shell_->exportPanel()->setTarget(t);
    return true;
}

void MainWindow::runExport() {
    ExportPanel* panel = shell_->exportPanel();
    if (!document_ || panel->busy()) return;
    const QString path = panel->outputPath();
    if (path.isEmpty()) {
        report(tr("Name the output file first (or pick one with ...)"));
        return;
    }
    exportTo(QLatin1String(ExportPanel::targetName(panel->target())), path, panel->options());
}

bool MainWindow::exportTo(const QString& target, const QString& path, const ExportOptions& options) {
    if (refuseWhileBusy(tr("starting another export"))) return false;
    settleCommit();  // (what the export evaluates goes on from the commit's work)
    if (!document_) {
        report(tr("No groom open to export to %1").arg(path));
        return false;
    }
    ExportPanel* panel = shell_->exportPanel();
    if (panel->busy()) {
        report(tr("An export is running"));
        return false;
    }
    ExportPanel::Target t;
    if (!ExportPanel::targetFromName(target.toLower().toStdString(), &t)) {
        report(tr("No export target '%1' (alembic, usd, vrscene, unreal, unity, cards, maps)").arg(target));
        return false;
    }
    panel->setTarget(t);
    panel->setOutputPath(path);
    if (shell_->timeline()->playing()) shell_->timeline()->setPlaying(false);  // (its frames would evaluate meanwhile)
    panel->setBusy(true);
    beginJob(tr("export"), panel->exportButton());
    // The panel's bar and the toast keep up between frames: the export runs on this thread
    // (the session is used from one thread at a time), the events in between: Cancel's click
    // gets through, the rest of the window waits (eventFilter).
    const ExportProgress progress = [this, panel](std::size_t done, std::size_t count) {
        panel->setProgress(done, count);
        pumpJobEvents();
        return !exportCancel_;
    };
    QElapsedTimer timer;
    timer.start();
    ExportResult result;
    const std::string out = path.toStdString();
    try {
        switch (t) {
            case ExportPanel::Target::Alembic: result = document_->exportAlembic(out, options, progress); break;
            case ExportPanel::Target::Usd: result = document_->exportUsd(out, options, progress); break;
            case ExportPanel::Target::Unreal: result = document_->exportUnrealGroom(out, options, progress); break;
            case ExportPanel::Target::Unity: result = document_->exportUnityStrands(out, options, progress); break;
            case ExportPanel::Target::Cards: result = document_->exportCards(out, options, progress); break;
            case ExportPanel::Target::CardMaps: result = document_->exportCardMaps(out, options, progress); break;
            case ExportPanel::Target::Vrscene: result = document_->exportVrscene(out, options, progress); break;
            default: break;
        }
    } catch (const std::exception& e) {
        panel->setBusy(false);
        endJob();
        panel->setStatus(tr("Export failed: %1").arg(QString::fromUtf8(e.what())), true);
        report(tr("Cannot export: %1 (%2)").arg(QString::fromUtf8(e.what()), path));
        reevaluate(true);
        return false;
    }
    panel->setBusy(false);
    endJob();
    const double seconds = timer.nsecsElapsed() / 1e9;
    const QString what = t == ExportPanel::Target::CardMaps ? tr("%n maps", "", static_cast<int>(result.files.size()))
                         : t == ExportPanel::Target::Cards  ? tr("%n cards", "", static_cast<int>(result.strands))
                                                            : tr("%n frames", "", static_cast<int>(result.frames)) +
                                                                  tr(", %1 strands").arg(result.strands);
    const QString line = result.cancelled ? tr("Stopped after %1 (%2 s): %3").arg(what).arg(seconds, 0, 'f', 2).arg(path)
                                          : tr("Wrote %1 in %2 s: %3").arg(what).arg(seconds, 0, 'f', 2).arg(path);
    panel->setStatus(line);
    notify(line);
    // The view is back at the document's frame (the export evaluated other frames on it).
    reevaluate(true);
    return !result.cancelled;
}

void MainWindow::chooseExportPath() {
    ExportPanel* panel = shell_->exportPanel();
    const QString current = panel->outputPath();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export"), current, tr("All files (*)"));
    if (!path.isEmpty()) panel->setOutputPath(path);
}

// ---- Files ----------------------------------------------------------------------------------------

bool MainWindow::openPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("groom")) return openGroom(path);
    return newGroomOnMesh(path);
}

bool MainWindow::openGroom(const QString& path) {
    if (refuseWhileBusy(tr("opening a groom"))) return false;
    try {
        working(tr("Opening %1...").arg(QFileInfo(path).fileName()));
        const BusyCursor wait;
        setDocument(GroomDocument::open(path.toStdString()));
    } catch (const std::exception& e) {
        report(tr("Cannot open the groom: %1 (%2)").arg(QString::fromUtf8(e.what()), path));
        return false;
    }
    rememberRecentFile(path);
    host_->setRecentFiles(recentFiles());
    notify(tr("Opened %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool MainWindow::newGroomOnMesh(const QString& meshPath, const QString& group) {
    if (refuseWhileBusy(tr("starting a new groom"))) return false;
    try {
        working(tr("Creating the groom on %1...").arg(QFileInfo(meshPath).fileName()));
        const BusyCursor wait;
        // An Alembic mesh keeps its animation: the file becomes the groom's scalp cache.
        if (QFileInfo(meshPath).suffix().compare(QStringLiteral("abc"), Qt::CaseInsensitive) == 0) {
            setDocument(GroomDocument::newOnAlembic(meshPath.toStdString()));
        } else {
            const std::string mesh = meshPath.toStdString();
            setDocument(GroomDocument::newOnMesh(group.isEmpty() ? readMeshFile(mesh) : readObjMesh(mesh, group.toStdString()), mesh));
        }
    } catch (const std::exception& e) {
        report(tr("Cannot make a groom on the mesh: %1 (%2)").arg(QString::fromUtf8(e.what()), meshPath));
        return false;
    }
    if (group.isEmpty()) {
        rememberRecentFile(meshPath);
        host_->setRecentFiles(recentFiles());
    }
    notify(tr("New groom on %1").arg(group.isEmpty() ? QFileInfo(meshPath).fileName() : group));
    // A bald scalp: the Groom Tools' first-groom steps say what comes next (Place some guides).
    return true;
}

void MainWindow::paramDragged(bool on) {
    // A slider's drag evaluates each step here (or the GPU previews it); its end commits.
    if (on) settleCommit();
    paramDragging_ = on;
    viewport_->setInteracting(on);
}

void MainWindow::setDocument(std::unique_ptr<GroomDocument> document) {
    dropAutosave();  // the old groom's: saved or let go of before another takes its place
    landDab(false);  // the old document's dab and commit go with it
    landCommit(false);
    lastRequest_.reset();
    viewNode_.clear();  // (first: the graph's signal finds it cleared and evaluates nothing)
    graph_->setViewedNode({});
    const bool graphWasShown = graphShown();
    params_->setDocument(nullptr);
    graphParams_->setDocument(nullptr);
    graph_->setDocument(nullptr);
    brush_.setDocument(nullptr);
    guidesPanel_->setDocument(nullptr);
    correctives_->setDocument(nullptr);
    gpu_.setDocument(nullptr);
    maps_->setDocument(nullptr);
    if (shell_->timeline()->playing()) shell_->timeline()->setPlaying(false);
    shell_->cardsPanel()->setDocument(nullptr);
    document_ = std::move(document);
    framed_ = false;
    lastDabMs_ = 0.0;
    settings_.hiddenGuideGroups = document_ ? document_->hiddenGroupsText() : std::string();  // (saved with the groom)
    // The old groom's photo camera goes with it.
    if (!lookPhoto_.isEmpty()) {
        viewport_->camera().leaveLookThrough();
        lookPhoto_.clear();
    }
    viewport_->renderer().clearPhoto();
    if (headless_) headless_->renderer.clearPhoto();
    photos_->setDocument(document_.get());
    if (document_) document_->setRenderOverrides(renderOverrides_);  // (Properties > Rendering: kept across grooms)
    cardSurface_.reset();
    cardSurfaceMap_.clear();
    applyProps();
    viewport_->renderer().clear();  // the old items go
    if (headless_) headless_->renderer.clear();
    viewport_->setScalp(document_ ? document_->file().data.scalp : nullptr);
    if (headless_) headless_->renderer.setScalp(document_ ? document_->file().data.scalp : nullptr);
    gpu_.setDocument(document_.get());
    if (document_) {
        // A slider's drag: the GPU's while it takes it (the value held outside the graph, the
        // next frame runs the nodes), else the CPU's as ever; the release commits on the CPU.
        document_->livePreview = [this](const std::string& node, const std::string& name, const nlohmann::json& value) {
            std::string why;
            gpu::OverrideValue v;
            // A number, a vec3 or a profile's keys (anything else is the CPU's).
            if (!gpuOverrideFromJson(value, &v)) return false;
            if (!gpu_.drag(node, name, v, &why)) {
                // A Subdivide refining the output for the viewport ends the GPU's run: for the drag
                // the view hands the strands on unrefined (GroomSession::setLiveDrag) and the GPU
                // runs the nodes before it; the release refines them again. Not for a drag of the
                // Subdivide itself (its levels are what the drag shows).
                GroomSession& session = document_->session();
                std::string draggedType;
                for (const GroomDocument::NodeInfo& n : document_->nodes()) {
                    if (n.name == node) draggedType = n.type;
                }
                if (session.liveDrag() || !session.viewRefinesStrands() || draggedType == "Subdivide") return false;
                session.setLiveDrag(true);
                reevaluate(true);  // the view without the refinement
                if (!gpu_.drag(node, name, v, &why)) {
                    session.setLiveDrag(false);
                    return false;
                }
            }
            viewport_->update();
            return true;
        };
        document_->livePreviewEnded = [this] {
            gpu_.endDrag();
            if (document_ && document_->session().liveDrag()) {
                // Refined again: by the commit that follows, or (a release at the value it had) here.
                document_->session().setLiveDrag(false);
                QTimer::singleShot(0, this, [this] {
                    if (document_) reevaluate();
                });
            }
        };
    }
    params_->setDocument(document_.get());
    graphParams_->setDocument(document_.get());
    brush_.setDocument(document_.get());
    guidesPanel_->setDocument(document_.get());
    correctives_->setDocument(document_.get());
    maps_->setDocument(document_.get());
    if (transferWindow_) transferWindow_->reloadGroom();  // the new groom's scalp and markers
    if (referenceWindow_) referenceWindow_->reloadGroom();  // the new groom's scalp and photos
    shell_->cardsPanel()->setDocument(document_.get());
    shell_->cardsPanel()->setShowCards(shell_->mode() == OverlayHost::Mode::Cards && document_ && !document_->cardsNodes().empty());
    refreshNodes();
    refreshGroups();
    refreshTimeline();
    reevaluate();
    refreshDisplayChoices();
    refreshUndoActions();
    refreshTitle();
    refreshGroomColor();
    refreshProperties();
    shell_->setNodeCount(document_ ? static_cast<int>(document_->nodes().size()) : 0);
    if (graphWasShown) setGraphShown(document_ != nullptr);
}

ViewRequest MainWindow::viewRequest() const {
    ViewRequest request;
    request.viewport = true;  // renderOnly nodes off, viewportOnly ones on (exports: the other way)
    request.density = static_cast<float>(density_->value());
    request.pointBudget = budget_->value() * 1000.0;
    request.sculptDensity = brush_.sculptDensity;
    // The cards are built (at full density, on the session's own card document) while they
    // show, or while Cards mode is up so its panel's numbers are live.
    const CardsPanel* cardsPanel = shell_->cardsPanel();
    request.wantCards = settings_.cardsView || cardsPanel->showCards() || shell_->mode() == OverlayHost::Mode::Cards;
    request.cardDensity = cardDensity_;  // (Game Cards > Card Density; the bake and the export: every strand)
    // View Up to Here: the graph's result at that node (the session shows the output for a node
    // giving no strands); exports and renders build their own requests: always the output.
    if (!viewNode_.empty() && document_) {
        for (const GroomDocument::NodeInfo& n : document_->nodes()) {
            if (n.name == viewNode_) request.viewNode = viewNode_;
        }
    }
    return request;
}

void MainWindow::reevaluate(bool quiet) {
    if (!document_) return;
    ViewRequest request = viewRequest();
    const CardsPanel* cardsPanel = shell_->cardsPanel();
    // A live stroke the GPU draws: after its press was shown, each dab's evaluation stops at
    // the Interpolate the GPU runs from (the guides alone are evaluated, the view keeps its
    // strands) and the frame runs the dab's guides through the GPU. A dab the GPU cannot take
    // (and the rest of the stroke) is evaluated whole on the CPU.
    const bool stroking = brush_.stroking() && document_->session().hasPreview();
    if (!stroking) gpu_.endStroke();
    if (stroking && gpu_.strokeArmed()) request.stopAt = gpu_.stopAt();
    // (Quiet evaluations are a live drag's steps, a stroke's dabs, playback: never commits.)
    if (!quiet && !stroking && commitAsync(request)) return;
    // Evaluated here: a dab or a commit still on the worker is taken in first (its results reused).
    landDab(false);
    landCommit(false);
    QElapsedTimer timer;
    timer.start();
    const ViewResult* view = &document_->evaluate(request);
    if (!request.stopAt.empty() && !gpu_.strokeHolds()) {
        request.stopAt.clear();
        view = &document_->evaluate(request);
    }
    const double ms = timer.nsecsElapsed() / 1e6;
    lastEvaluateMs_ = ms;
    lastRequest_ = request;
    lastRequest_->time = document_->time();
    const ViewResult& shown = *view;
    for (const SessionMessage& m : shown.messages) report(QString::fromStdString(m.text), m.error);
    if (!shown.errors.empty()) report(QString::fromStdString(shown.errors.front()));
    // The groom scaled because the scalp's area changed (Surface Area): said once, in the status toast.
    if (const GroomSession& s = document_->session(); s.scaleNoticeSerial() != 0 &&
        (&s != scaleNoticeSession_ || s.scaleNoticeSerial() != scaleNoticeShown_)) {
        scaleNoticeSession_ = &s;
        scaleNoticeShown_ = s.scaleNoticeSerial();
        report(QString::fromStdString(s.scaleNotice()));
    }
    const MeshPtr scalp = shown.scalp ? shown.scalp : document_->file().data.scalp;
    viewport_->setScalp(scalp);
    // The cards instead of the strands: flat (the builder's item), or textured by the last
    // bake as a surface beside the props (the lookdev path draws no builder triangles).
    const bool showCards = cardsPanel->showCards() && shown.cards && !shown.cards->cards.empty();
    const CardBakeFiles* bake = document_->lastBake();
    const bool textured = showCards && cardsPanel->textures() && bake && !bake->preview.empty();
    settings_.cardsView = showCards;
    settings_.drawCards = showCards && !textured;
    // The cards are judged on their own, as a game sees them; the guides belong to Groom mode
    // (Look mode shows the hair alone).
    settings_.guides = guides_->isChecked() && !showCards && shell_->mode() == OverlayHost::Mode::Groom;
    settings_.tubes = tubes_->isChecked() && !showCards && shell_->mode() == OverlayHost::Mode::Groom;
    // How the strands differ from the ones shown before (a dab's incremental evaluation): the
    // renderers write only those.
    const HairChanges changes{shown.strandsSerial, shown.changesBase, shown.strandsChanges};
    // (The Groom Tools' eye off: the renderers hold them, drawing none.)
    viewport_->renderer().setHairStrands(showCards ? nullptr : shown.strands, shown.versions.strands, changes);
    if (headless_) {
        headless_->renderer.setScalp(scalp);
        headless_->renderer.setHairStrands(showCards ? nullptr : shown.strands, shown.versions.strands, changes);
    }
    if (textured) {
        if (!cardSurface_ || cardSurfaceVersion_ != shown.versions.cards || cardSurfaceMap_ != bake->preview) {
            cardSurface_ = makeCardSurface(*shown.cards, bake->preview, document_->session().hairLook().roughness);
            cardSurfaceVersion_ = shown.versions.cards;
            cardSurfaceMap_ = bake->preview;
            applyProps();
        }
    } else if (cardSurface_) {
        cardSurface_.reset();
        cardSurfaceMap_.clear();
        applyProps();
    }
    redisplay();
    if (request.wantCards) refreshCards();
    if (graphShown()) {
        graph_->setNodeTimes(document_->nodeTimes());
        graph_->setNodeNotes(document_->nodeNotes());
        graph_->setNodeErrors(document_->nodeErrors());
    }
    params_->refreshNote();
    graphParams_->refreshNote();
    // The press shown: the GPU takes the dabs from here (when it can).
    if (stroking && !gpu_.strokeArmed() && request.stopAt.empty()) {
        std::string why;
        const bool armed = gpu_.armStroke(&why);
        strokeCpuWhy_ = armed ? std::string() : why;
        if (!armed && !quiet) report(tr("The stroke previews on the CPU: %1").arg(QString::fromStdString(why)), false);
    }
    if (!framed_) {
        framed_ = true;
        frameAll();
    }
    if (shown.errors.empty()) {
        evaluation_ = tr("%1 strands, %2 points, %3 guides, evaluated in %4 ms")
                          .arg(shown.strands ? shown.strands->strandCount() : 0)
                          .arg(shown.strands ? shown.strands->pointCount() : 0)
                          .arg(shown.hierarchy ? shown.hierarchy->guides().size() : 0)
                          .arg(ms, 0, 'f', 1);
        if (!quiet) report(evaluation_, false);
        scheduleStats();
    }
    // Between strokes in brush mode: the stroke document evaluated ahead, so the first dab of
    // the next stroke makes again only what it changed.
    if (!quiet && viewport_->toolMode() == Viewport::ToolMode::Brush && !brush_.stroking()) warmStroke();
}

// ---- Commits off the UI thread ----------------------------------------------------------------

bool MainWindow::commitAsync(const ViewRequest& request) {
    // A commit (a stroke's release, a drag's end, a typed value, an edit of the graph) with the
    // same view as the last evaluation: its graph part detached and evaluated on a worker while
    // the window goes on drawing what it shows (the stroke's last dab, the drag's GPU preview).
    // What only the session's own evaluation does (a live stroke or preview, cards, Simulate
    // nodes: detach says null), another view, a live drag's steps, or a headless view drawn from
    // the result at once, evaluates here as before.
    if (!asyncCommits_ || landing_ || headless_ || paramDragging_ || brush_.stroking() || request.wantCards || !request.stopAt.empty()) return false;
    // A GPU drag or stroke in flight plans from the session's document: commits wait for its end.
    if (gpu_.dragging() || gpu_.strokeArmed()) return false;
    if (!lastRequest_ || lastRequest_->density != request.density || lastRequest_->pointBudget != request.pointBudget ||
        lastRequest_->sculptDensity != request.sculptDensity || lastRequest_->viewNode != request.viewNode ||
        lastRequest_->time != document_->time() || lastRequest_->wantCards != request.wantCards) {
        return false;
    }
    GroomSession& session = document_->session();
    if (!session.hasGraph() || session.hasPreview()) return false;
    ViewRequest timed = request;
    timed.time = document_->time();
    document_->scene().setTime(timed.time);  // the inputs of the time, as GroomScene::view feeds them
    std::shared_ptr<DetachedEvaluation> run = session.detach(timed, commits_.last());
    // Null too after a commit still running on another graph (a param's edit swaps the
    // session's): edits come faster than they evaluate (a drag), so this one is evaluated here,
    // the one before taken in first, and each shows.
    if (!run) return false;
    try {
        commits_.start(std::move(run));
    } catch (const std::exception&) {
        return false;  // no thread: evaluated here
    }
    commitTimer_->start();
    commitClock_.start();
    return true;
}

void MainWindow::landCommit(bool show) {
    // The commit evaluated: the session takes the copy it evaluated, and the evaluation that
    // follows finds every result made (the frame and its uploads as a synchronous commit's).
    if (!commits_.pending()) return;
    commitTimer_->stop();
    const std::shared_ptr<DetachedEvaluation> run = commits_.wait();
    if (run && run->warm()) {
        // A warm of the stroke document: nothing shows it.
        if (document_) document_->session().adoptWarm(*run);
        return;
    }
    if (!run || !run->warm()) {
        ++commitsLanded_;
        lastCommitMs_ = commitClock_.isValid() ? commitClock_.nsecsElapsed() / 1e6 : 0.0;
    }
    // The GPU preview's plans name the document the session lets go: planned again.
    if (run && document_ && document_->session().adopt(*run)) gpu_.sessionDocumentSwapped();
    if (show && document_) {
        landing_ = true;  // (evaluated here: the results are made)
        reevaluate();
        landing_ = false;
    }
}

void MainWindow::setAsyncCommits(bool on) {
    if (!on) landCommit(true);
    asyncCommits_ = on;
    viewport_->renderer().setBuildAhead(on);
}

void MainWindow::settleCommit() {
    landDab(false);
    landCommit(true);
}

// ---- Dabs off the UI thread ---------------------------------------------------------------------

bool MainWindow::dabAsync() {
    // A CPU stroke's dab after its press was shown (the press arms the GPU or says why not, and
    // takes in what was on the worker): its evaluation goes to a worker while the window goes on
    // drawing (the ring, the camera) and the tool goes on taking the mouse. One at a time: a dab
    // that comes while one evaluates is not queued; the next evaluation, once that one lands,
    // takes the stroke as it is then.
    if (!asyncDabs_ || headless_ || landingDab_ || !document_ || !brush_.stroking()) return false;
    if (dabs_.pending()) {
        dabWanted_ = true;
        return true;
    }
    return startDab();
}

bool MainWindow::startDab() {
    GroomSession& session = document_->session();
    if (!session.hasPreview() || gpu_.strokeArmed() || commits_.pending()) return false;
    ViewRequest request = viewRequest();
    request.time = document_->time();
    document_->scene().setTime(request.time);  // the inputs of the time, as GroomScene::view feeds them
    std::shared_ptr<DetachedEvaluation> run = session.detachStroke(request);
    if (!run) return false;  // (a map, part or flow stroke, cards...: evaluated here)
    try {
        dabs_.start(run);
    } catch (const std::exception&) {
        // No thread: the run here, taken in, and the dab evaluated here as ever.
        run->run();
        const std::shared_ptr<GuideHierarchy> live = session.previewHierarchy();
        const bool final = session.previewFinal();
        session.adoptStroke(*run);
        session.setPreviewHierarchy(live, final);
        return false;
    }
    dabWanted_ = false;
    dabClock_.start();
    dabTimer_->start();
    return true;
}

void MainWindow::landDab(bool show) {
    // The dab evaluated: the session takes its stroke document back with the guides the run
    // evaluated as the preview, and the evaluation here finds every result made (shown, unless
    // `show` is false: the caller evaluates next, or the stroke ends); then the tool's guides are
    // the preview again, and the dabs that came meanwhile go to the worker as one.
    if (!dabs_.pending()) return;
    dabTimer_->stop();
    const std::shared_ptr<DetachedEvaluation> run = dabs_.wait();
    if (!run || !document_) return;
    GroomSession& session = document_->session();
    const std::shared_ptr<GuideHierarchy> live = session.previewHierarchy();
    const bool final = session.previewFinal();
    session.adoptStroke(*run);
    ++dabsLanded_;
    lastDabRunMs_ = run->milliseconds();
    if (show && brush_.stroking()) {
        QElapsedTimer timer;
        timer.start();
        landingDab_ = true;
        reevaluate(true);
        landingDab_ = false;
        lastDabMs_ = timer.nsecsElapsed() / 1e6;
    }
    if (live) session.setPreviewHierarchy(live, final);
    const bool again = dabWanted_;
    dabWanted_ = false;
    // (Refused on the worker, the GPU armed at the landing say: evaluated here.)
    if (show && again && brush_.stroking() && !startDab()) reevaluate(true);
}

void MainWindow::warmStroke() {
    if (!document_ || brush_.stroking()) return;
    ViewRequest request;
    request.viewport = true;
    request.density = static_cast<float>(density_->value());
    request.pointBudget = budget_->value() * 1000.0;
    request.sculptDensity = brush_.sculptDensity;
    // With commits off the UI thread the warm goes there too (after any commit: they run in
    // order), taken in by landCommit; a stroke pressed first takes it in at its first dab.
    if (asyncCommits_ && !headless_) {
        if (commits_.pending()) return;  // (warmed once that lands: its evaluation warms after it)
        std::shared_ptr<DetachedEvaluation> run = document_->session().detachWarm(request);
        if (!run) return;
        try {
            commits_.start(std::move(run));
            commitTimer_->start();
            return;
        } catch (const std::exception&) {
            // No thread: warmed here.
        }
    }
    brush_.warm(request);
}

void MainWindow::guidesEdited() {
    reevaluate();
    refreshUndoActions();
    refreshGroups();
    refreshProperties();  // (Inputs > Guides)
    maps_->invalidate();  // (a stroke: rebuilt only when what it lists changed, and only while it shows)
}

std::string MainWindow::lastNodeOfType(const char* type) const {
    std::string found;
    if (!document_) return found;
    for (const GroomDocument::NodeInfo& node : document_->nodes()) {
        if (node.type == type) found = node.name;
    }
    return found;
}

void MainWindow::refreshGroomColor() {
    LookCard::GroomColor color;
    color.node = hairColorNode();
    if (!color.node.empty()) {
        auto number = [&](const char* name, float fallback) {
            const nlohmann::json v = document_->paramOrDefault(color.node, name);
            return v.is_number() ? v.get<float>() : fallback;  // a map or expression: the slider shows the default
        };
        color.melanin = number("melanin", 0.35f);
        color.redness = number("redness", 0.2f);
        color.roughness = number("roughness", 0.5f);
        color.tipStrength = number("tipStrength", 0.0f);
        color.hueRandom = number("hueRandom", 0.0f);
        color.saturationRandom = number("saturationRandom", 0.0f);
        color.picked = document_->paramOrDefault(color.node, "mode") == "color";
        auto rgb = [&](const char* name, RGB& out) {
            const nlohmann::json v = document_->paramOrDefault(color.node, name);
            if (v.is_array() && v.size() == 3) out = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
        };
        rgb("color", color.color);
        rgb("tipColor", color.tipColor);
    }
    shell_->lookCard()->setGroomColor(color);
    refreshHairColorRows();  // the Properties' Hair Color rows
}

void MainWindow::graphEdited() {
    reevaluate();
    refreshTimeline();  // a node reading the time (Wind, Simulate) shows it
    refreshUndoActions();
    refreshNodes();
    refreshGroomColor();
    refreshStarter();  // a look node added: the first-groom step ticks
    shell_->setNodeCount(document_ ? static_cast<int>(document_->nodes().size()) : 0);
    if (graphShown()) graph_->refresh();
    params_->refresh();
    graphParams_->refresh();
    maps_->invalidate();
    correctives_->invalidate();  // (a Sculpt node added or gone)
    groomTools_->refreshTubes();  // Fill Tubes and Tube Blend (the Interpolate nodes')
    refreshDisplayChoices();
    shell_->cardsPanel()->refresh();
    refreshCards();
}

void MainWindow::refreshStarter() {
    // The first-groom steps (palette.starter): a guide placed; a look of the groom's own in the
    // graph (one of palette.starter.lookTypes).
    bool guides = false, look = false;
    if (document_) {
        if (const HierarchyPtr h = document_->session().authoredHierarchy()) guides = !h->guides().empty();
        const ui::json& types = ui::Definitions::shipped().get("palette/starter/lookTypes");
        for (const GroomDocument::NodeInfo& n : document_->nodes()) {
            if (std::find(types.begin(), types.end(), n.type) != types.end()) look = true;
        }
    }
    host_->setStarterProgress(guides, look);
}

void MainWindow::refreshGroups() {
    // The Groom Tools' Group choice: the groups in tree order (children under their parents).
    std::vector<GroomTools::GroupRow> rows;
    if (document_) {
        if (const HierarchyPtr h = document_->session().authoredHierarchy()) {
            const std::vector<GuideGroup>& groups = h->groups();
            std::function<void(GroupId, int)> walk = [&](GroupId parent, int depth) {
                for (const GuideGroup& g : groups) {
                    if (g.parent != parent) continue;
                    rows.push_back({g.name, depth});
                    if (depth < 16) walk(g.id, depth + 1);
                }
            };
            walk(kNoGroup, 0);
        }
    }
    groomTools_->setGroups(rows);
    groomTools_->setParts(guidesPanel_->partNames());
    const auto [arrows, sinks] = guidesPanel_->flowCount();
    groomTools_->setFlowCount(arrows, sinks);
    refreshStarter();
    groomTools_->refresh();
    guidesPanel_->invalidate();
    correctives_->invalidate();
}

void MainWindow::askReload() {
    if (!document_ || document_->path().empty()) {
        report(tr("No groom file to reload from: save the groom first"));
        return;
    }
    if (!interactive_ || !document_->dirty()) {
        reloadGroom();
        return;
    }
    const QString name = QFileInfo(QString::fromStdString(document_->path())).fileName();
    host_->showPrompt(tr("Reload from File?"),
                      tr("%1 has unsaved changes. Reloading reads the file again: the changes and the undo history are lost.").arg(name),
                      {{tr("Reload"), true, [this] { reloadGroom(); }}, {tr("Cancel"), false, {}}});
}

void MainWindow::confirmDiscard(const QString& doing, std::function<void()> proceed) {
    if (!interactive_ || !document_ || !document_->dirty()) {
        if (proceed) proceed();
        return;
    }
    const QString name = document_->path().empty() ? tr("The new groom") : QFileInfo(QString::fromStdString(document_->path())).fileName();
    host_->showPrompt(tr("Unsaved Changes"), tr("%1 has changes that are not saved. Save them before %2?").arg(name, doing),
                      {{tr("Save"), true,
                        [this, proceed] {
                            save();  // (Save As when the groom has no file yet)
                            if (document_ && !document_->dirty() && proceed) proceed();
                        }},
                       {tr("Discard"), false, proceed},
                       {tr("Cancel"), false, {}}});
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // A job running: it stops first (its Cancel), the window closes once it has (endJob).
    if (busy()) {
        event->ignore();
        closeAfterJob_ = true;
        cancelJob();
        notify(tr("Stopping the %1 before closing...").arg(job_));
        return;
    }
    if (closing_ || !interactive_ || !document_ || !document_->dirty()) {
        if (interactive_) saveLayout();  // the docks as they are, for the next start
        dropAutosave();                  // (saved, discarded, or nothing to keep)
        QMainWindow::closeEvent(event);
        return;
    }
    event->ignore();
    confirmDiscard(tr("closing"), [this] {
        closing_ = true;
        close();
    });
}

// ---- A running job ---------------------------------------------------------------------------------

void MainWindow::working(const QString& text) {
    notify(text);
    // The line paints before the window blocks (the work is on this thread): no input taken.
    if (interactive_ && isVisible()) QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::beginJob(const QString& what, QObject* cancel) {
    job_ = what;
    jobCancel_ = cancel;
    exportCancel_ = false;
    closeAfterJob_ = false;
    QApplication::setOverrideCursor(Qt::BusyCursor);  // (the window still answers: Cancel)
}

void MainWindow::endJob() {
    if (job_.isEmpty()) return;
    QApplication::restoreOverrideCursor();
    job_.clear();
    jobCancel_ = nullptr;
    if (closeAfterJob_) {
        closeAfterJob_ = false;
        QTimer::singleShot(0, this, [this] { close(); });  // (asks about unsaved edits as ever)
    }
}

bool MainWindow::cancelJob() {
    if (!busy()) return false;
    exportCancel_ = true;
    return true;
}

bool MainWindow::refuseWhileBusy(const QString& doing) {
    if (!busy()) return false;
    report(tr("Wait for the %1 to finish (or Cancel it) before %2").arg(job_, doing));
    return true;
}

void MainWindow::pumpJobEvents() {
    // Every event, the user's too (eventFilter lets only the job's Cancel and Esc through).
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

namespace {

// The events a person makes: held back from everything but the job's Cancel while a job runs.
bool userInput(QEvent::Type type) {
    switch (type) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:
        case QEvent::ContextMenu:
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
        case QEvent::HoverLeave:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // A job running: a shortcut (an action's) does nothing; a widget's input goes to the job's
    // Cancel alone (a window passes it on to the widget under it: filtered there); Esc cancels.
    if (busy()) {
        if (event->type() == QEvent::Shortcut) return true;
        if (watched->isWidgetType() && userInput(event->type())) {
            if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                cancelJob();
                return true;
            }
            auto* widget = static_cast<QWidget*>(watched);
            auto* cancel = qobject_cast<QWidget*>(jobCancel_);
            const bool toCancel = cancel && (widget == cancel || cancel->isAncestorOf(widget));
            if (!toCancel) return true;
        }
    }
    // The window's Tab (the node graph) is a shortcut: a text field with the focus takes Tab
    // itself (the next field), as a dialog's would.
    if (event->type() == QEvent::ShortcutOverride) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if ((key == Qt::Key_Tab || key == Qt::Key_Backtab) &&
            (qobject_cast<QLineEdit*>(watched) || qobject_cast<QAbstractSpinBox*>(watched) || qobject_cast<QTextEdit*>(watched) ||
             qobject_cast<QPlainTextEdit*>(watched))) {
            event->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::reloadGroom() {
    if (!document_ || document_->path().empty()) {
        report(tr("No groom file to reload from: save the groom first"));
        return false;
    }
    return openGroom(QString::fromStdString(document_->path()));
}

bool MainWindow::transferToMesh(const QString& path, bool fit, bool maps) {
    TransferOptions options;
    options.method = fit ? TransferMethod::Bounds : TransferMethod::Closest;
    return transferToMesh(path, options, maps);
}

ReferenceWindow* MainWindow::openReferenceWindow() {
    if (!referenceWindow_) {
        referenceWindow_ = new ReferenceWindow(this, this);  // a window of its own (Qt::Window), owned here
        connect(referenceWindow_, &ReferenceWindow::guidesChanged, this, [this] { guidesEdited(); });
        // Its edits (landmarks, a solved camera) show in the Photos tab and through the photo.
        connect(referenceWindow_, &ReferenceWindow::photosStored, this, [this] {
            photos_->refresh();
            refreshPhotoBackdrop();
            refreshTitle();
        });
        fitWindowToScreen(referenceWindow_, referenceWindow_->size());  // a small screen: within it
    } else {
        referenceWindow_->reloadGroom();
    }
    referenceWindow_->show();
    referenceWindow_->raise();
    return referenceWindow_;
}

ReferenceWindow* MainWindow::openPhotoAlign(int index) {
    ReferenceWindow* w = openReferenceWindow();
    if (w) {
        if (index >= 0) w->selectPhoto(index);
        w->setStage(ReferenceWindow::Stage::Align);
    }
    return w;
}

TransferWindow* MainWindow::openTransferWindow() {
    if (!transferWindow_) {
        transferWindow_ = new TransferWindow(this, this);  // a window of its own (Qt::Window), owned here
        fitWindowToScreen(transferWindow_, transferWindow_->size());  // a small screen: within it
    } else {
        transferWindow_->reloadGroom();
    }
    transferWindow_->show();
    transferWindow_->raise();
    return transferWindow_;
}

bool MainWindow::readTransferMarkers(const QString& path, std::vector<TransferMarker>* markers) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        report(tr("Cannot read the markers file %1").arg(path));
        return false;
    }
    try {
        *markers = transferMarkersFromJson(file.readAll().toStdString());
    } catch (const std::exception& e) {
        report(tr("Cannot read the markers file %1: %2").arg(path, QString::fromUtf8(e.what())));
        return false;
    }
    return true;
}

bool MainWindow::transferToMesh(const QString& path, const TransferOptions& options, bool maps) {
    if (!document_) {
        report(tr("No groom open to transfer"));
        return false;
    }
    if (refuseWhileBusy(tr("transferring the groom"))) return false;
    settleCommit();
    lastRequest_.reset();
    working(tr("Transferring..."));
    const BusyCursor wait;
    MeshPtr mesh;
    try {
        mesh = readMeshFile(path.toStdString());
    } catch (const std::exception& e) {
        report(tr("Cannot read the mesh: %1 (%2)").arg(QString::fromUtf8(e.what()), path));
        return false;
    }
    std::vector<std::string> warnings;
    const std::string error = document_->transferToScalp(mesh, options, maps, &warnings);
    if (!error.empty()) {
        report(tr("Cannot transfer the groom: %1 (%2)").arg(QString::fromStdString(error), path));
        return false;
    }
    for (const std::string& warning : warnings) report(QString::fromStdString(warning), false);
    // The scene is another: everything that holds the scalp or the history starts over.
    viewport_->setScalp(document_->file().data.scalp);
    if (headless_) headless_->renderer.setScalp(document_->file().data.scalp);
    brush_.setDocument(document_.get());
    gpu_.setDocument(document_.get());
    params_->setDocument(document_.get());
    graphParams_->setDocument(document_.get());
    if (graphShown()) graph_->setDocument(document_.get());
    maps_->setDocument(document_.get());
    framed_ = false;
    refreshNodes();
    refreshGroups();
    refreshTimeline();
    reevaluate();
    refreshDisplayChoices();
    refreshUndoActions();
    refreshTitle();
    notify(tr("Transferred the groom onto %1 (%2)").arg(QFileInfo(path).fileName(), QString::fromLatin1(transferMethodName(options.method))));
    return true;
}

void MainWindow::chooseOpen() {
    confirmDiscard(tr("opening another groom"), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Groom"), QString(), tr("Groom files (*.groom)"));
        if (!path.isEmpty()) openGroom(path);
    });
}

void MainWindow::chooseMesh() {
    confirmDiscard(tr("starting a new groom"), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("New Groom on Mesh"), QString(), tr("Meshes (*.obj *.abc)"));
        if (!path.isEmpty()) newGroomOnMesh(path);
    });
}

void MainWindow::save() {
    if (!document_) return;
    if (document_->path().empty()) {
        saveAs();
        return;
    }
    try {
        document_->save(document_->path());
        dropAutosave();
        notify(tr("Saved %1").arg(QFileInfo(QString::fromStdString(document_->path())).fileName()));
    } catch (const std::exception& e) {
        report(tr("Cannot save: %1. Try Save As to another folder").arg(QString::fromUtf8(e.what())));
    }
    refreshTitle();
}

void MainWindow::saveAs() {
    if (!document_) return;
    QString path = QFileDialog::getSaveFileName(this, tr("Save Groom"), QString::fromStdString(document_->path()),
                                                tr("Groom files (*.groom)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".groom"), Qt::CaseInsensitive)) path += QStringLiteral(".groom");
    try {
        document_->save(path.toStdString());
        dropAutosave();
        rememberRecentFile(path);
        host_->setRecentFiles(recentFiles());
        notify(tr("Saved %1").arg(QFileInfo(path).fileName()));
    } catch (const std::exception& e) {
        report(tr("Cannot save: %1. Try Save As to another folder").arg(QString::fromUtf8(e.what())));
    }
    refreshTitle();
}

void MainWindow::exportAlembic() {
    // Ctrl+E: Export mode with its panel (every target, the frames, the output): no dialog.
    if (!document_) return;
    setUiMode(QStringLiteral("export"));
}

}  // namespace groomlab::app
