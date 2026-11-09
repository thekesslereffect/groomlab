// SPDX-License-Identifier: Apache-2.0
// The app's standalone renders' shared pieces: the stage's camera and environment, the renderer's
// process and progress, the window that shows the image (render_job.h).

#include "render_job.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace groomlab::app {

namespace fs = std::filesystem;

namespace {

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 normalized(const V3& v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return n > 0.0f ? V3{v[0] / n, v[1] / n, v[2] / n} : V3{0.0f, 0.0f, 1.0f};
}

}  // namespace

CameraAxes lookAt(const V3& eye, const V3& target) {
    const V3 f = normalized(sub(target, eye));
    V3 r = cross(f, {0.0f, 1.0f, 0.0f});
    if (std::abs(r[0]) + std::abs(r[1]) + std::abs(r[2]) < 1e-6f) r = {1.0f, 0.0f, 0.0f};  // straight up or down
    r = normalized(r);
    return {r, cross(r, f), f};
}

V3 orbitEye(const V3& eye, const V3& target, float degrees) {
    const V3 d = rotateY(sub(eye, target), degrees);
    return {target[0] + d[0], target[1] + d[1], target[2] + d[2]};
}

EnvironmentMap stageEnvironment(const LightingSetup& l, int width, int height) {
    const EnvironmentMap source = makeEnvironment(l.environment, width, height);
    EnvironmentMap map = source;
    if (l.environmentRotation != 0.0f) {
        for (int y = 0; y < map.height; ++y) {
            for (int x = 0; x < map.width; ++x) {
                // The world direction of this texel, looked up in the map turned back.
                const auto d = EnvironmentMap::direction((x + 0.5f) / map.width, (y + 0.5f) / map.height);
                const RGB c = environmentRadiance(source, rotateY(d, -l.environmentRotation));
                std::copy(c.begin(), c.end(), map.rgb.begin() + (static_cast<std::size_t>(y) * map.width + x) * 3);
            }
        }
    }
    return map;
}

void writeHdr(const std::string& path, const EnvironmentMap& map) {
    std::ofstream out(fs::u8path(path), std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << map.height << " +X " << map.width << "\n";
    std::vector<unsigned char> row(static_cast<std::size_t>(map.width) * 4);
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            const RGB c = map.at(x, y);
            const float m = std::max({c[0], c[1], c[2]});
            unsigned char* p = row.data() + static_cast<std::size_t>(x) * 4;
            if (!(m > 1e-32f)) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            int e = 0;
            const float scale = std::frexp(m, &e) * 256.0f / m;
            for (int k = 0; k < 3; ++k) p[k] = static_cast<unsigned char>(std::clamp(c[k] * scale, 0.0f, 255.0f));
            p[3] = static_cast<unsigned char>(e + 128);
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    if (!out) throw std::runtime_error("cannot write " + path);
}

std::string sceneNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.7g", std::abs(v) < 1e-12 ? 0.0 : v);
    return buf;
}

QString renderFrameImage(const QString& imagePath, int frame, int turntableFrames) {
    if (turntableFrames <= 0) return imagePath;
    const QFileInfo info(imagePath);
    return info.dir().filePath(QStringLiteral("%1.%2.%3").arg(info.completeBaseName()).arg(frame, 4, 10, QLatin1Char('0')).arg(info.suffix()));
}

// ---- The process ----------------------------------------------------------------------------------

RenderJob::RenderJob(QObject* parent) : QObject(parent) {}

RenderJob::~RenderJob() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        // A job going away reports nothing: its process' finished would reach done() and the job's
        // finished, whose receivers may be half destroyed already (the MainWindow that owns this job
        // deletes it as a child, after its own members are gone).
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(5000);
    }
}

bool RenderJob::start(const QString& name, const QString& program, const QStringList& arguments,
                      const QProcessEnvironment& environment, const QString& workingDirectory, const QString& imagePath,
                      int turntableFrames, std::unique_ptr<RenderProgress> progress) {
    name_ = name;
    image_ = imagePath;
    turntableFrames_ = std::max(turntableFrames, 0);
    frames_ = std::max(turntableFrames, 1);
    state_ = std::move(progress);
    // Last time's images must not count as this render's.
    for (int k = 1; k <= frames_; ++k) QFile::remove(renderFrameImage(image_, k, turntableFrames_));
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process_->setWorkingDirectory(workingDirectory);
    if (!environment.isEmpty()) process_->setProcessEnvironment(environment);
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] { read(); });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { done(code); });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) done(-1);
    });
    process_->start(program, arguments);
    emit this->progress(0.0, tr("Starting %1...").arg(name_));
    return true;
}

void RenderJob::cancel() {
    cancelled_ = true;
    if (process_ && process_->state() != QProcess::NotRunning) process_->kill();
}

bool RenderJob::running() const { return process_ && !ended_; }

bool RenderJob::wait() {
    while (process_ && !ended_) {
        process_->waitForFinished(200);
        read();
        if (process_->state() == QProcess::NotRunning && !ended_) done(process_->exitCode());
    }
    return ok_;
}

void RenderJob::read() {
    if (!process_ || !state_) return;
    pending_ += process_->readAll();
    // Lines end with \n; progress lines may end with \r alone.
    int start = 0;
    for (int i = 0; i < pending_.size(); ++i) {
        if (pending_[i] != '\n' && pending_[i] != '\r') continue;
        const QString line = QString::fromLocal8Bit(pending_.mid(start, i - start)).trimmed();
        start = i + 1;
        if (line.isEmpty()) continue;
        const int before = state_->imagesWritten;
        if (!state_->feed(line)) continue;
        const int frame = std::clamp(state_->frame, 1, frames_);
        const double fraction = std::clamp((static_cast<double>(frame - 1) + state_->percent / 100.0) / frames_, 0.0, 1.0);
        emit progress(fraction, frames_ > 1 ? tr("Frame %1 of %2: %3%").arg(frame).arg(frames_).arg(state_->percent, 0, 'f', 0)
                                             : tr("Rendering: %1%").arg(state_->percent, 0, 'f', 0));
        if (state_->imagesWritten != before) {
            const QString path = state_->written.isEmpty()
                                     ? QString()
                                     : QDir(QFileInfo(image_).absolutePath()).absoluteFilePath(state_->written);
            emit imageWritten(QFileInfo(path).isFile() ? path
                                                       : renderFrameImage(image_, std::min(state_->imagesWritten, frames_),
                                                                          turntableFrames_));
        }
    }
    pending_.remove(0, start);
}

void RenderJob::done(int exitCode) {
    if (ended_) return;
    read();
    ended_ = true;
    int images = 0;
    for (int k = 1; k <= frames_; ++k) images += QFileInfo(renderFrameImage(image_, k, turntableFrames_)).isFile() ? 1 : 0;
    ok_ = !cancelled_ && exitCode == 0 && images >= frames_;
    QString message;
    const QString error = state_ ? state_->error : QString();
    if (cancelled_) {
        message = tr("Cancelled");
    } else if (exitCode == -1) {
        message = tr("%1 did not start: %2").arg(name_, process_->errorString());
    } else if (!ok_) {
        message = tr("%1 stopped (exit code %2)%3").arg(name_).arg(exitCode).arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error);
    } else {
        message = frames_ > 1 ? tr("Rendered %1 frames: %2").arg(frames_).arg(QFileInfo(image_).absolutePath())
                              : tr("Rendered %1").arg(image_);
    }
    // The last image, whether or not the renderer said it wrote it (it is complete now).
    if (ok_) emit imageWritten(renderFrameImage(image_, frames_, turntableFrames_));
    emit progress(ok_ ? 1.0 : 0.0, message);
    emit finished(ok_, message);
}

// ---- The window -----------------------------------------------------------------------------------

RenderWindow::RenderWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("renderWindow"));
    setWindowTitle(tr("Render"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(6);
    view_ = new QLabel;
    view_->setAlignment(Qt::AlignCenter);
    view_->setMinimumSize(320, 180);
    view_->setStyleSheet(QStringLiteral("background: #111214;"));
    outer->addWidget(view_, 1);
    bar_ = new QProgressBar;
    bar_->setRange(0, 1000);
    bar_->setTextVisible(false);
    outer->addWidget(bar_);
    auto* row = new QHBoxLayout;
    status_ = new QLabel;
    status_->setWordWrap(true);
    row->addWidget(status_, 1);
    cancel_ = new QPushButton(tr("Cancel"));
    save_ = new QPushButton(tr("Save Image..."));
    folder_ = new QPushButton(tr("Show in Folder"));
    row->addWidget(cancel_);
    row->addWidget(save_);
    row->addWidget(folder_);
    outer->addLayout(row);
    save_->setEnabled(false);
    folder_->setEnabled(false);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (job_) job_->cancel();
    });
    connect(save_, &QPushButton::clicked, this, [this] {
        if (image_.isNull()) return;
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Image"), QFileInfo(imagePath_).fileName(),
                                                          tr("Images (*.png *.jpg *.tif *.exr)"));
        if (!path.isEmpty() && !image_.save(path)) status_->setText(tr("Could not write %1").arg(path));
    });
    connect(folder_, &QPushButton::clicked, this, [this] {
        if (!imagePath_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(imagePath_).absolutePath()));
    });
    resize(900, 600);
}

void RenderWindow::begin(RenderJob* job, const QString& title) {
    job_ = job;
    setWindowTitle(title);
    cancel_->setEnabled(true);
    bar_->setValue(0);
    connect(job, &RenderJob::progress, this, [this](double fraction, const QString& text) {
        bar_->setValue(static_cast<int>(std::round(fraction * 1000.0)));
        status_->setText(text);
    });
    connect(job, &RenderJob::imageWritten, this, [this](const QString& path) { showImage(path); });
    connect(job, &RenderJob::finished, this, [this, job](bool, const QString& text) {
        status_->setText(text);
        cancel_->setEnabled(false);
        if (job_ == job) job_ = nullptr;
    });
    connect(job, &QObject::destroyed, this, [this, job] {
        if (job_ == job) job_ = nullptr;
    });
}

void RenderWindow::showImage(const QString& path) {
    QImage image(path);
    if (image.isNull()) return;
    image_ = image;
    imagePath_ = path;
    save_->setEnabled(true);
    folder_->setEnabled(true);
    fit();
}

void RenderWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fit();
}

void RenderWindow::fit() {
    if (image_.isNull()) return;
    view_->setPixmap(QPixmap::fromImage(image_.scaled(view_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

}  // namespace groomlab::app
