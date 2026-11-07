// SPDX-License-Identifier: Apache-2.0
// MainWindow: headless and offscreen frames, and the benchmarks.

#include "main_window_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>

#include "offscreen.h"
#include "viewport.h"

namespace groomlab::app {

void MainWindow::benchmarkLive(int frames, const QString& mode, const std::function<void(const QString&)>& done) {
    // orbit: the camera turns a degree a frame. hover: the brush's ring follows a mouse moving
    // in a circle. stroke: one stroke of the current brush, a mouse move a frame. param: the
    // noise node's amplitude changes a frame, as a slider drag does.
    struct State {
        int left = 0, step = 0;
        QElapsedTimer clock;
        std::vector<double> intervals, render, handler, evaluate, dab, gpu;
        int gpuRuns = 0;  // frames the GPU look preview ran in
        nlohmann::json dragStart;  // param: the value before the drag
        // Dabs off the UI thread: when each landed (ms into the measured frames) and its run's time.
        QElapsedTimer measured;
        std::uint64_t landedSeen = 0;
        std::vector<double> landedAt, dabRuns, dabLandings;
    };
    auto state = std::make_shared<State>();
    state->left = frames + 10;  // the first ten settle (pipelines, first uploads)
    if (mode == QStringLiteral("hover") || mode == QStringLiteral("stroke")) setToolMode(QStringLiteral("brush"));
    // The mouse moves in a small circle around the top of the scalp as seen (a stroke must land
    // on the scalp to dab: maskByScalp), or around the view's center without a groom.
    QPointF around(viewport_->width() * 0.5, viewport_->height() * 0.4);
    if (document_ && document_->file().data.scalp) {
        const Bounds b = document_->sceneBounds();
        const MeshPtr& scalp = document_->file().data.scalp;
        Bounds s;
        for (const Vec3f& p : scalp->points()) {
            if (!s.valid) {
                s.lo = s.hi = p;
                s.valid = true;
            } else {
                s.lo = {std::min(s.lo.x, p.x), std::min(s.lo.y, p.y), std::min(s.lo.z, p.z)};
                s.hi = {std::max(s.hi.x, p.x), std::max(s.hi.y, p.y), std::max(s.hi.z, p.z)};
            }
        }
        const Bounds& use = s.valid ? s : b;
        QPointF at;
        if (use.valid && projectToView(QVector3D((use.lo.x + use.hi.x) * 0.5f, use.hi.y, (use.lo.z + use.hi.z) * 0.5f), viewport_->size(), &at)) around = at;
    }
    auto point = [this, state, around] {
        const double a = state->step * 0.05;
        const double r = std::min(viewport_->width(), viewport_->height()) * 0.03;
        return QPointF(around.x() + r * std::cos(a), around.y() + r * std::sin(a));
    };
    if ((mode == QStringLiteral("hover") || mode == QStringLiteral("stroke")) && document_) {
        // A stroke dabs only where there are guides: a groom made on a mesh has none, so a
        // Place stroke across the top of the scalp makes some first (committed, one undo step),
        // then the measured stroke is the Maya bench's comb (radius 120 px, strength 1).
        const HierarchyPtr h = document_->session().hierarchy();
        if (!h || h->guides().empty()) {
            setBrushText(QStringLiteral("mode=place"));
            setBrushText(QStringLiteral("radius=120"));
            std::vector<QPointF> across;
            for (int k = -6; k <= 6; ++k) across.emplace_back(around.x() + 30.0 * k, around.y());
            applyStroke(across, viewport_->size());
            report(tr("Placed %1 guides for the stroke benchmark").arg(document_->session().hierarchy() ? document_->session().hierarchy()->guides().size() : 0), false);
        }
        setBrushText(QStringLiteral("mode=comb"));
        setBrushText(QStringLiteral("radius=120"));
        setBrushText(QStringLiteral("strength=1"));
    }
    auto mouse = [this](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, viewport_->mapToGlobal(at), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(viewport_, &event);
    };
    auto step = [this, state, mode, point, mouse] {
        QElapsedTimer handler;
        handler.start();
        ++state->step;
        if (mode == QStringLiteral("hover")) {
            mouse(QEvent::MouseMove, point(), Qt::NoButton, Qt::NoButton);
        } else if (mode == QStringLiteral("stroke")) {
            if (state->step == 1) mouse(QEvent::MouseButtonPress, point(), Qt::LeftButton, Qt::LeftButton);
            else mouse(QEvent::MouseMove, point(), Qt::NoButton, Qt::LeftButton);
        } else if (mode == QStringLiteral("param")) {
            // As a slider's drag: each value a step without a record (the GPU preview's while
            // it takes it), the drag's end recorded once (benchmarkLiveEnd).
            if (document_) {
                const double value = 0.2 + 0.002 * (state->step % 50);
                if (state->step == 1) state->dragStart = document_->paramOrDefault("noise", "amplitude");
                const std::string error = document_->setParam("noise", "amplitude", value, false);
                if (!error.empty()) report(QString::fromStdString(error));
                else graphEdited();
            }
        } else if (mode == QStringLiteral("param-commit")) {
            // Each value committed and evaluated on the CPU (an undo step a frame).
            setParamText(QStringLiteral("noise.amplitude=%1").arg(0.2 + 0.002 * (state->step % 50)));
        } else {
            adjustCamera(1.0f, 0.0f, 0.0f);
        }
        return handler.nsecsElapsed() / 1e6;
    };
    auto* once = new QObject(this);
    connect(viewport_, &Viewport::frameDrawn, once, [this, once, state, frames, mode, step, point, mouse, done] {
        if (state->left <= frames) {
            state->intervals.push_back(state->clock.nsecsElapsed() / 1e6);
            state->render.push_back(viewport_->lastRenderCpuMs());
            state->evaluate.push_back(lastEvaluateMs_);
            state->dab.push_back(lastDabMs_);
            state->gpu.push_back(viewport_->lastGpuMs());
            if (viewport_->renderer().gpuRanLastFrame()) ++state->gpuRuns;
            if (!state->measured.isValid()) state->measured.start();
            if (dabsLanded_ != state->landedSeen) {
                state->landedAt.push_back(state->measured.nsecsElapsed() / 1e6);
                state->dabRuns.push_back(lastDabRunMs_);
                state->dabLandings.push_back(lastDabMs_);
            }
        }
        state->landedSeen = dabsLanded_;
        state->clock.start();
        if (--state->left > 0) {
            QTimer::singleShot(0, this, [state, step] { state->handler.push_back(step()); });
            return;
        }
        once->deleteLater();
        // The drag's end (a stroke's release, a slider's): what the UI thread spends on it.
        QElapsedTimer release;
        release.start();
        if (mode == QStringLiteral("stroke")) mouse(QEvent::MouseButtonRelease, point(), Qt::LeftButton, Qt::NoButton);
        if (mode == QStringLiteral("param") && document_) {
            document_->recordEdit("noise", "amplitude", state->dragStart, document_->paramOrDefault("noise", "amplitude"));
            graphEdited();
        }
        const double releaseMs = release.nsecsElapsed() / 1e6;
        auto stats = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            double sum = 0.0;
            for (double x : v) sum += x;
            return QStringLiteral("mean %1 ms, median %2, p95 %3")
                .arg(sum / v.size(), 0, 'f', 2)
                .arg(v[v.size() / 2], 0, 'f', 2)
                .arg(v[std::min(v.size() - 1, v.size() * 95 / 100)], 0, 'f', 2);
        };
        // The slowest frames with their parts: what is left over is QRhi's end of frame (the
        // recorded uploads executed, the present), the window's compositing and the event loop.
        QString slowest;
        std::vector<std::size_t> order(state->intervals.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return state->intervals[a] > state->intervals[b]; });
        for (std::size_t k = 0; k < std::min<std::size_t>(5, order.size()); ++k) {
            const std::size_t i = order[k];
            const double handler = i < state->handler.size() ? state->handler[i] : 0.0;
            const double render = i < state->render.size() ? state->render[i] : 0.0;
            slowest += QStringLiteral("%1frame %2: %3 ms (handler %4, render %5, the rest %6)")
                           .arg(k ? ", " : "")
                           .arg(i)
                           .arg(state->intervals[i], 0, 'f', 1)
                           .arg(handler, 0, 'f', 1)
                           .arg(render, 0, 'f', 1)
                           .arg(state->intervals[i] - handler - render, 0, 'f', 1);
        }
        // The parts of the handling (the last evaluation, a dab's whole handling), the GPU's own
        // time where the device has timestamps (QSG_RHI_PROFILE=1), and where the strands came from.
        const ViewResult* last = document_ ? &document_->lastView() : nullptr;
        const QString parts = QStringLiteral("; evaluate %1; dab %2; GPU %3; %4 strands shown, the GPU preview ran in %5 frames%6; release %7 ms")
                                  .arg(stats(state->evaluate), stats(state->dab), stats(state->gpu))
                                  .arg(last && last->strands ? last->strands->strandCount() : 0)
                                  .arg(state->gpuRuns)
                                  .arg(strokeCpuWhy_.empty() ? QString() : QStringLiteral(" (the stroke on the CPU: %1)").arg(QString::fromStdString(strokeCpuWhy_)))
                                  .arg(releaseMs, 0, 'f', 1);
        // Dabs off the UI thread: how often the hair updated, and the worker's time per dab.
        QString dabParts;
        if (state->landedAt.size() > 1) {
            std::vector<double> between;
            for (std::size_t i = 1; i < state->landedAt.size(); ++i) between.push_back(state->landedAt[i] - state->landedAt[i - 1]);
            dabParts = QStringLiteral("; dabs off the UI thread: the hair updated %1 times (between updates %2), the worker's dab %3, "
                                      "its landing on the UI thread %4")
                           .arg(state->landedAt.size())
                           .arg(stats(between), stats(state->dabRuns), stats(state->dabLandings));
        }
        const QString line = QStringLiteral("%1 %2 frames at %3x%4 (%5): between frames %6; input handling %7; render() CPU %8; slowest: %9")
                                 .arg(mode)
                                 .arg(frames)
                                 .arg(viewport_->width() * devicePixelRatioF())
                                 .arg(viewport_->height() * devicePixelRatioF())
                                 .arg(viewport_->apiName())
                                 .arg(stats(state->intervals))
                                 .arg(stats(state->handler))
                                 .arg(stats(state->render))
                                 .arg(slowest) +
                             parts + dabParts;
        // After the release, a second of the event loop: when a commit off the UI thread landed,
        // and the longest the UI thread went without turning (what a mouse would wait for).
        // The UI thread turning: each timer tick and each frame drawn (frames back to back, a
        // refine's, go before timers); the commit's landing as landCommit times it.
        struct Watch {
            QElapsedTimer clock, tick;
            qint64 longestNs = 0;
        };
        auto watch = std::make_shared<Watch>();
        watch->clock.start();
        watch->tick.start();
        const std::uint64_t landedBefore = commitsLanded_;
        auto turned = [watch] {
            watch->longestNs = std::max(watch->longestNs, watch->tick.nsecsElapsed());
            watch->tick.start();
        };
        auto* frames = new QObject(this);
        connect(viewport_, &Viewport::frameDrawn, frames, turned);
        auto* timer = new QTimer(this);
        timer->setInterval(1);
        connect(timer, &QTimer::timeout, this, [this, timer, frames, watch, turned, landedBefore, line, done] {
            turned();
            const bool landed = commitsLanded_ != landedBefore;
            if (watch->clock.elapsed() < 1500 && (commitPending() || watch->clock.elapsed() < 500)) return;
            timer->stop();
            timer->deleteLater();
            frames->deleteLater();
            done(line + QStringLiteral("; after the release: %1, the longest UI stall %2 ms")
                            .arg(landed ? QStringLiteral("the commit landed %1 ms after it started").arg(lastCommitMs_, 0, 'f', 1)
                                        : QStringLiteral("no commit off the UI thread"))
                            .arg(static_cast<double>(watch->longestNs) / 1e6, 0, 'f', 1));
        });
        timer->start();
    });
    QTimer::singleShot(0, this, [state, step] { state->handler.push_back(step()); });
}

bool MainWindow::ensureHeadless(const QSize& size) {
    if (headless_ && headless_->view && headless_->view->size() == size) return headless_->view->ready();
    headless_ = std::make_unique<Headless>();
    headless_->view = std::make_unique<OffscreenView>(size);
    if (!headless_->view->ready()) {
        report(tr("No offscreen device to draw into"));
        return false;
    }
    // Fed what the viewport's renderer has; from here reevaluate and redisplay keep it current.
    GroomRenderer& renderer = headless_->renderer;
    GroomRenderer& shown = viewport_->renderer();
    renderer.setGpuPreview(&gpu_);
    renderer.setShowScalp(shown.showScalp());
    renderer.setLookdev(shown.lookdev());
    renderer.setMaterial(shown.material());
    renderer.setLighting(shown.lighting());
    renderer.setScalpMaterial(shown.scalpMaterial());
    // The props, and the textured cards among them while they show (the cards in place of the strands).
    std::vector<SurfacePtr> props = props_;
    if (cardSurface_) props.push_back(cardSurface_);
    renderer.setProps(props);
    if (document_) {
        document_->display().invalidateAll();
        const ViewResult& view = document_->lastView();
        renderer.setScalp(view.scalp ? view.scalp : document_->file().data.scalp);
        renderer.setHairStrands(settings_.cardsView ? nullptr : view.strands, view.versions.strands);
        redisplay();
    }
    return true;
}

OffscreenResult MainWindow::headlessFrame(bool readback) {
    if (!headless_ || !headless_->view) return OffscreenResult{};
    return headless_->view->frame(headless_->renderer, viewport_->camera(), readback);
}

bool MainWindow::projectToView(const QVector3D& point, const QSize& size, QPointF* out) {
    brush_.setView(viewport_->camera(), size.width(), size.height());
    float x = 0.0f, y = 0.0f;
    if (!brush_.view().project({point.x(), point.y(), point.z()}, x, y)) return false;
    *out = QPointF(x, size.height() - y);  // the view counts pixels from the bottom
    return true;
}

namespace {

struct Steps {
    std::vector<double> ms;
    double median() const {
        if (ms.empty()) return 0.0;
        std::vector<double> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }
    double min() const { return ms.empty() ? 0.0 : *std::min_element(ms.begin(), ms.end()); }
    double max() const { return ms.empty() ? 0.0 : *std::max_element(ms.begin(), ms.end()); }
    QString text() const { return QStringLiteral("%1 ms [%2-%3]").arg(median(), 0, 'f', 2).arg(min(), 0, 'f', 2).arg(max(), 0, 'f', 2); }
};

}  // namespace

bool MainWindow::benchmarkDrag(const QString& node, const QString& param, double from, double to, int steps, const QSize& size) {
    if (!document_) {
        report(tr("No groom open to drag %1.%2").arg(node, param));
        return false;
    }
    if (!ensureHeadless(size)) return false;
    const std::string n = node.toStdString(), p = param.toStdString();
    const nlohmann::json before = document_->paramOrDefault(n, p);
    if (!before.is_number()) {
        report(tr("%1.%2 is not a number to drag").arg(node, param));
        return false;
    }
    headlessFrame();  // the CPU's strands drawn once (uploads done)
    // Each step as the slider makes it: setParam without a record (the GPU's while it takes it,
    // the document holding the value), the graph edited handlers as the panel's edited(), then
    // the frame drawn and the GPU waited for.
    // The upload's CPU side per step: the renderer's last upload's time where the step made one,
    // else 0 (the GPU preview's steps upload nothing).
    Steps toFrame, evaluating, drawing, uploading;
    QElapsedTimer timer, part;
    bool gpu = false;
    auto uploads = [this] {
        const GroomRenderer& r = headless_->renderer;
        return r.lookdevActive() ? r.lookdevRenderer().uploads() : r.uploads();
    };
    for (int i = 0; i < std::max(steps, 1); ++i) {
        const double value = steps > 1 ? from + (to - from) * i / (steps - 1) : to;
        timer.start();
        const std::string error = document_->setParam(n, p, value, false);
        if (!error.empty()) {
            report(tr("Cannot drag %1.%2: %3").arg(node, param, QString::fromStdString(error)));
            return false;
        }
        part.start();
        reevaluate(true);
        evaluating.ms.push_back(part.nsecsElapsed() / 1e6);
        part.start();
        const std::size_t uploadsBefore = uploads();
        headlessFrame();
        drawing.ms.push_back(part.nsecsElapsed() / 1e6);
        const GroomRenderer& drawn = headless_->renderer;
        uploading.ms.push_back(uploads() == uploadsBefore ? 0.0
                               : drawn.lookdevActive() ? drawn.lookdevRenderer().lastUploadMs() : drawn.lastUploadMs());
        toFrame.ms.push_back(timer.nsecsElapsed() / 1e6);
        gpu = headless_->renderer.gpuRanLastFrame();
    }
    // The release: the drag's end recorded (the held value committed on the CPU), drawn.
    timer.start();
    document_->recordEdit(n, p, before, document_->paramOrDefault(n, p));
    reevaluate(true);
    headlessFrame();
    const double releaseMs = timer.nsecsElapsed() / 1e6;
    refreshUndoActions();
    const GroomRenderer& r = headless_->renderer;
    const gpu::GpuTimes times = gpu_.gpuTimes(true);
    report(tr("Drag %1.%2 %3..%4 (%5 steps, %6, %7, %8 strands): %9 to the drawn frame (evaluating %10, drawing %11); the release "
              "(CPU commit) %12 ms; GPU kernels %13 ms, the GPU's CPU side %14 ms, base upload %15 ms (%16 strands%17); last step %18")
               .arg(node, param)
               .arg(from)
               .arg(to)
               .arg(steps)
               .arg(gpu ? tr("GPU preview") : tr("CPU"))
               .arg(r.lookdevActive() ? tr("lookdev") : tr("lines"))
               .arg(document_->lastView().strands ? document_->lastView().strands->strandCount() : 0)
               .arg(toFrame.text(), evaluating.text(), drawing.text())
               .arg(releaseMs, 0, 'f', 2)
               .arg(times.valid ? times.totalMs : 0.0, 0, 'f', 2)
               .arg(gpu_.lastRunMs(), 0, 'f', 2)
               .arg(gpu_.lastUploadMs(), 0, 'f', 2)
               .arg(gpu_.lastUploadStrands())
               .arg(gpu_.lastUploadPartial() ? tr(", partial") : QString())
               .arg(gpu ? tr("ran") : QString::fromStdString(gpu_.why())),
           false);
    std::printf("GroomLab: benchmark drag %s.%s %s: median %.2f ms to the drawn frame (evaluate %.2f, draw %.2f, of it the upload's CPU side %.2f), release %.2f ms\n",
                qPrintable(node), qPrintable(param), gpu ? "GPU" : "CPU", toFrame.median(), evaluating.median(), drawing.median(),
                uploading.median(), releaseMs);
    return true;
}

double MainWindow::benchmarkOrbit(const QSize& size, int frames) {
    if (!ensureHeadless(size)) return 0.0;
    headlessFrame();
    QElapsedTimer timer;
    timer.start();
    double gpuTotal = 0.0;
    int gpuFrames = 0;
    for (int i = 0; i < std::max(frames, 1); ++i) {
        viewport_->camera().orbit(6.0f, 0.0f);
        headless_->renderer.invalidateAccumulation();
        const OffscreenResult one = headlessFrame(false);
        if (i >= 2 && one.gpuMsPerFrame > 0.0) {
            gpuTotal += one.gpuMsPerFrame;
            ++gpuFrames;
        }
    }
    const double seconds = timer.nsecsElapsed() / 1e9;
    const double fps = seconds > 0.0 ? std::max(frames, 1) / seconds : 0.0;
    std::printf("GroomLab: benchmark orbit: %d frames at %dx%d in %.2f s: %.1f fps (%s; GPU %.2f ms a frame)\n", std::max(frames, 1),
                size.width(), size.height(), seconds, fps, headless_->renderer.lookdevActive() ? "lookdev" : "lines",
                gpuFrames > 0 ? gpuTotal / gpuFrames : 0.0);
    return fps;
}

QImage MainWindow::renderOffscreen(const QSize& size, int frames, double* msPerFrame, QString* api, int* surfaceDraws, double* firstFrameMs,
                                   double* gpuMsPerFrame) {
    // A renderer of its own on an offscreen device, fed what the viewport's has.
    GroomRenderer renderer;
    GroomRenderer& shown = viewport_->renderer();
    if (document_) {
        document_->display().invalidateAll();
        renderer.setFrame(document_->updateDisplay(settings_));
        renderer.setScalpMap(document_->shownScalpMap());
        const ViewResult& view = document_->lastView();
        renderer.setScalp(view.scalp ? view.scalp : document_->file().data.scalp);
        renderer.setHairStrands(settings_.cardsView ? nullptr : view.strands, view.versions.strands);
        renderer.setHairLook(document_->session().hairLook());
    }
    renderer.setShowScalp(shown.showScalp());
    renderer.setLookdev(shown.lookdev());
    renderer.setMaterial(shown.material());
    renderer.setLighting(shown.lighting());
    renderer.setScalpMaterial(shown.scalpMaterial());
    // The props, and the textured cards among them while they show (in place of the strands).
    std::vector<SurfacePtr> props = props_;
    if (cardSurface_) props.push_back(cardSurface_);
    renderer.setProps(props);
    renderer.setClumpLod(settings_.clumpLod, settings_.clumpLodError);
    renderer.setPhoto(shown.photo());  // a reference photo looked through
    OrbitCamera camera = viewport_->camera();
    const OffscreenResult result = groomlab::app::renderOffscreen(renderer, camera, size, frames);
    if (msPerFrame) *msPerFrame = result.msPerFrame;
    if (firstFrameMs) *firstFrameMs = result.firstFrameMs;
    if (gpuMsPerFrame) *gpuMsPerFrame = result.gpuMsPerFrame;
    if (api) *api = result.api;
    // The viewport's builder frame is stale now (invalidated above): it draws fresh next time.
    if (document_) viewport_->setFrame(document_->updateDisplay(settings_));
    if (surfaceDraws) *surfaceDraws = renderer.lookdevRenderer().lastSurfaceDraws();
    return result.image;
}

}  // namespace groomlab::app
