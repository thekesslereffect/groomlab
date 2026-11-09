// SPDX-License-Identifier: Apache-2.0
// MainWindow: Render > ... in V-Ray (vray_render.h) and in RenderMan (prman_render.h), through the
// shared render job and window (render_job.h).

#include "main_window_internal.h"

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>

#include "groom_document.h"
#include "prman_render.h"
#include "render_job.h"
#include "renderer.h"
#include "viewport.h"
#include "vray_render.h"

namespace groomlab::app {

bool MainWindow::prepareRender(const QString& renderer, const QString& imagePath, int turntableFrames, const QSize& size,
                               RenderStage* stage) {
    if (refuseWhileBusy(tr("rendering in %1").arg(renderer))) return false;
    settleCommit();
    if (!document_) {
        report(tr("No groom open to render in %1").arg(renderer));
        return false;
    }
    if (renderJob_ && renderJob_->running()) {
        report(tr("A %1 render is running: wait for it or cancel it").arg(renderJob_->name()));
        if (renderWindow_) renderWindow_->raise();
        return false;
    }
    const QFileInfo image(imagePath);
    if (!QDir().mkpath(image.absolutePath())) {
        report(tr("Cannot make the folder %1").arg(image.absolutePath()));
        return false;
    }
    const OrbitCamera& camera = viewport_->camera();
    stage->eye = camera.eye();
    stage->target = camera.target();
    stage->fovDegrees = camera.fovDegrees();
    GroomRenderer& renderer3d = viewport_->renderer();
    stage->lighting = renderer3d.lighting();
    stage->showScalp = renderer3d.showScalp();
    stage->scalpColor = renderer3d.scalpMaterial().baseColor;
    QSize pixels = size;
    if (!pixels.isValid() || pixels.isEmpty()) pixels = viewport_->size() * viewport_->devicePixelRatioF();
    if (pixels.width() < 16 || pixels.height() < 16) pixels = QSize(1280, 720);
    stage->width = pixels.width();
    stage->height = pixels.height();
    stage->turntableFrames = std::max(turntableFrames, 0);
    return true;
}

bool MainWindow::runRender(const QString& renderer, const QString& program, const QStringList& arguments,
                           const QProcessEnvironment& environment, const QString& imagePath, const RenderStage& stage,
                           std::unique_ptr<RenderProgress> progress, bool wait) {
    const QFileInfo image(imagePath);
    delete renderJob_;
    renderJob_ = new RenderJob(this);
    const QString title = stage.turntableFrames > 0 ? tr("%1 Turntable: %2").arg(renderer, image.completeBaseName())
                                                    : tr("%1 Render: %2").arg(renderer, image.completeBaseName());
    if (!wait) {
        if (!renderWindow_) renderWindow_ = new RenderWindow(this);
        renderWindow_->begin(renderJob_, title);
        renderWindow_->show();
        renderWindow_->raise();
    }
    connect(renderJob_, &RenderJob::finished, this, [this, renderer](bool ok, const QString& message) {
        if (ok) notify(message);
        else report(tr("%1: %2").arg(renderer, message), message != tr("Cancelled"));
    });
    notify(tr("Rendering in %1 (%2x%3%4)...").arg(renderer).arg(stage.width).arg(stage.height)
               .arg(stage.turntableFrames > 0 ? tr(", %n frames", "", stage.turntableFrames) : QString()));
    renderJob_->start(renderer, program, arguments, environment, image.absolutePath(), image.absoluteFilePath(),
                      stage.turntableFrames, std::move(progress));
    if (!wait) return true;
    const bool ok = renderJob_->wait();
    if (ok) {
        std::printf("GroomLab: wrote %s (%s%s)\n", qPrintable(image.absoluteFilePath()), qPrintable(renderer),
                    stage.turntableFrames > 0 ? qPrintable(QStringLiteral(", %1 frames").arg(stage.turntableFrames)) : "");
    }
    return ok;
}

bool MainWindow::renderVray(const QString& imagePath, int turntableFrames, const QSize& size, bool wait) {
    const QString name = tr("V-Ray");
    RenderStage stage;
    if (!prepareRender(name, imagePath, turntableFrames, size, &stage)) return false;
    const QString vray = findVrayStandalone();
    if (vray.isEmpty()) {
        report(tr("V-Ray Standalone was not found: pick its vray.exe with Render > V-Ray Standalone Location... "
                  "(or set GROOMLAB_VRAY)"));
        return false;
    }
    const QFileInfo image(imagePath);
    const QString stem = QDir(image.absolutePath()).filePath(image.completeBaseName());
    const QString groomPath = stem + QStringLiteral("_groom.vrscene");
    const QString scenePath = stem + QStringLiteral("_scene.vrscene");

    // The groom at this frame, every strand (as a final render gets it), and its scalp there.
    ExportOptions options;
    options.frames.first = options.frames.last = document_->frame();
    options.density = 1.0f;  // (the render overrides' density on top: GroomDocument::renderOverrides)
    document_->setRenderOverrides(renderOverrides_);
    // Level of detail through the render's camera (a single frame: a turntable's camera moves).
    if (stage.turntableFrames == 0 && (renderLod_.distance || renderLod_.frustum || renderLod_.backface)) {
        const QVector3D forward = (stage.target - stage.eye).normalized();
        QVector3D right = QVector3D::crossProduct(forward, QVector3D(0.0f, 1.0f, 0.0f));
        if (right.lengthSquared() < 1e-8f) right = QVector3D(1.0f, 0.0f, 0.0f);
        right.normalize();
        const QVector3D up = QVector3D::crossProduct(right, forward);
        LodCamera c;
        const QVector3D rows[4] = {right, up, -forward, stage.eye};
        for (int r = 0; r < 4; ++r) {
            c.matrix[r][0] = rows[r].x();
            c.matrix[r][1] = rows[r].y();
            c.matrix[r][2] = rows[r].z();
            c.matrix[r][3] = r == 3 ? 1.0f : 0.0f;
        }
        c.tanHalfFovY = std::tan(stage.fovDegrees * 0.5f * 3.14159265f / 180.0f);
        c.tanHalfFovX = c.tanHalfFovY * static_cast<float>(std::max(stage.width, 1)) / static_cast<float>(std::max(stage.height, 1));
        options.lod = renderLod_;
        options.lodCamera = c;
    }
    MeshPtr scalp;
    try {
        working(tr("Writing the groom for V-Ray..."));
        const BusyCursor busy;
        document_->exportVrscene(groomPath.toStdString(), options, {}, &scalp);
    } catch (const std::exception& e) {
        report(tr("Cannot write the groom for V-Ray: %1 (%2)").arg(QString::fromUtf8(e.what()), groomPath));
        reevaluate(true);
        return false;
    }
    reevaluate(true);  // (the view back at its own density)
    stage.scalp = scalp;
    try {
        writeVrayScene(scenePath.toStdString(), groomPath.toStdString(), stage);
    } catch (const std::exception& e) {
        report(tr("Cannot write the V-Ray scene: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    return runRender(name, vray, vrayArguments(scenePath, image.absoluteFilePath(), stage.turntableFrames),
                     QProcessEnvironment(), image.absoluteFilePath(), stage, std::make_unique<VrayProgress>(), wait);
}

bool MainWindow::renderPrman(const QString& imagePath, int turntableFrames, const QSize& size, bool wait) {
    const QString name = tr("RenderMan");
    RenderStage stage;
    if (!prepareRender(name, imagePath, turntableFrames, size, &stage)) return false;
    const QString prman = findPrman();
    if (prman.isEmpty()) {
        report(tr("RenderMan's prman was not found: install RenderMan Pro Server (free for non-commercial use), or pick "
                  "its prman with Render > RenderMan Location... (or set GROOMLAB_PRMAN or RMANTREE)"));
        return false;
    }
    const QString procedurals = findRenderManProcedural();
    if (procedurals.isEmpty()) {
        report(tr("GroomLab's RenderMan procedural (groomlab_renderman) was not found beside the app in "
                  "procedurals/renderman (or set GROOMLAB_RENDERMAN_PROCEDURAL)"));
        return false;
    }
    const QFileInfo image(imagePath);
    const QString stem = QDir(image.absolutePath()).filePath(image.completeBaseName());
    PrmanScene scene;
    scene.groomPath = (stem + QStringLiteral("_groom.groom")).toStdString();
    scene.frame = document_->frame();
    scene.hairTilt = document_->session().hairLook().tilt;
    scene.procedurals = procedurals.toStdString();
    scene.imagePath = image.absoluteFilePath().toStdString();
    scene.density = renderOverrides_.densityTimes(1.0f);
    scene.widthScale = renderOverrides_.widthScale;
    scene.lengthScale = renderOverrides_.lengthScale;
    const QString scenePath = stem + QStringLiteral("_scene.rib");
    // The groom as it is now, which the procedural evaluates (every strand) at the frame; the scalp
    // as the view has it.
    try {
        working(tr("Writing the groom for RenderMan..."));
        const BusyCursor busy;
        document_->writeRenderGroom(scene.groomPath, &stage.scalp);
        scene.environmentTexture = prmanEnvironmentTexture(prman, scenePath.toStdString(), stage);
        writePrmanScene(scenePath.toStdString(), scene, stage);
    } catch (const std::exception& e) {
        report(tr("Cannot write the RenderMan scene: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    return runRender(name, prman, prmanArguments(scenePath), prmanEnvironment(prman), image.absoluteFilePath(), stage,
                     std::make_unique<PrmanProgress>(), wait);
}

void MainWindow::chooseVrayStandalone() {
    const QString current = findVrayStandalone();
    const QString path = QFileDialog::getOpenFileName(this, tr("V-Ray Standalone (vray.exe)"), current,
#ifdef Q_OS_WIN
                                                      tr("V-Ray Standalone (vray.exe);;Programs (*.exe)")
#elif defined(Q_OS_MACOS)
                                                      tr("V-Ray Standalone (VRay.app vray.bin)")
#else
                                                      tr("V-Ray Standalone (vray)")
#endif
    );
    if (path.isEmpty()) return;
    setVrayStandalone(path);
#ifdef Q_OS_MACOS
    notify(tr("V-Ray Standalone: %1").arg(findVrayStandalone()));  // the vray.bin inside what was picked
#else
    notify(tr("V-Ray Standalone: %1").arg(QDir::toNativeSeparators(path)));
#endif
}

void MainWindow::choosePrman() {
    const QString current = findPrman();
    const QString path = QFileDialog::getOpenFileName(this, tr("RenderMan's prman"), current,
#ifdef Q_OS_WIN
                                                      tr("RenderMan (prman.exe);;Programs (*.exe)")
#else
                                                      tr("RenderMan (prman)")
#endif
    );
    if (path.isEmpty()) return;
    setPrman(path);
    notify(tr("RenderMan: %1").arg(QDir::toNativeSeparators(path)));
}

}  // namespace groomlab::app
