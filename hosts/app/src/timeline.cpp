// SPDX-License-Identifier: Apache-2.0
#include "timeline.h"

#include <algorithm>
#include <cmath>

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSpinBox>
#include <QTimer>

#include "theme.h"

namespace groomlab::app {

namespace {

QString iconPath(const char* name) { return QStringLiteral(":/groomlab/icons/ui_%1.svg").arg(QLatin1String(name)); }

RoundButton* transportButton(const char* icon, int size, const QString& tip, QWidget* parent) {
    auto* b = new RoundButton(parent);
    b->setIcon(iconPath(icon), size);
    b->setIconColor(theme::text(), theme::checkedText(theme::ink()));
    b->setFill(theme::chip(), theme::checkedFill(theme::yellow()));
    b->setDiameter(32);
    b->setToolTip(tip);
    return b;
}

RoundButton* chipButton(const QString& text, const QString& tip, QWidget* parent) {
    auto* b = new RoundButton(parent);
    b->setText(text);
    b->setDiameter(32);
    b->setRadius(10.0);
    b->setPadding(12);
    b->setFontSize(13);
    b->setFontWeight(800);
    b->setFill(theme::chip(), theme::emeraldChip());
    b->setTextColor(theme::textMuted(), theme::emeraldSoft());
    b->setCheckable(true);
    b->setToolTip(tip);
    return b;
}

// A chip that runs something (not a toggle): `fill` behind its text.
RoundButton* actionChip(const QString& text, const QString& tip, const QColor& fill, const QColor& textColor, QWidget* parent) {
    auto* b = new RoundButton(parent);
    b->setText(text);
    b->setDiameter(32);
    b->setRadius(10.0);
    b->setPadding(12);
    b->setFontSize(13);
    b->setFontWeight(800);
    b->setFill(fill, fill);
    b->setTextColor(textColor, textColor);
    b->setToolTip(tip);
    return b;
}

}  // namespace

// ---- FrameScrubber ----------------------------------------------------------------------------

FrameScrubber::FrameScrubber(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setMinimumWidth(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void FrameScrubber::setRange(double first, double last) {
    first_ = first;
    last_ = std::max(first, last);
    frame_ = std::clamp(frame_, first_, last_);
    update();
}

void FrameScrubber::setFrame(double frame) {
    frame_ = std::clamp(frame, first_, last_);
    update();
}

void FrameScrubber::setSimulatedFrames(Runs cached, Runs baked) {
    if (cached == cached_ && baked == baked_) return;
    cached_ = std::move(cached);
    baked_ = std::move(baked);
    update();
}

QSize FrameScrubber::sizeHint() const { return QSize(260, 24); }

void FrameScrubber::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // The track with a tick every tenth of the range (every frame when there are few), the
    // played part filled in yellow and the playhead as a round knob.
    const QRectF track(2.0, height() / 2.0 - 4.0, width() - 4.0, 8.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::track());
    painter.drawRoundedRect(track, 4.0, 4.0);
    const double span = std::max(1.0, last_ - first_);
    const double t = std::clamp((frame_ - first_) / span, 0.0, 1.0);
    painter.setBrush(theme::yellow());
    painter.drawRoundedRect(QRectF(track.left(), track.top(), std::max(8.0, track.width() * t), track.height()), 4.0, 4.0);
    painter.setPen(QPen(theme::divider(), 1.0));
    const int ticks = span <= 24.0 ? static_cast<int>(span) : 10;
    for (int k = 1; k < ticks; ++k) {
        const qreal x = track.left() + track.width() * k / ticks;
        painter.drawLine(QPointF(x, track.top() - 3.0), QPointF(x, track.top()));
    }
    // The simulation's frames under the track: baked in blue, cached in memory in emerald over it.
    auto strip = [&](const Runs& runs, const QColor& color) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (const auto& [a, b] : runs) {
            const double from = std::clamp((a - first_) / span, 0.0, 1.0), to = std::clamp((b - first_) / span, 0.0, 1.0);
            if (b < first_ || a > last_) continue;
            const qreal x0 = track.left() + track.width() * from, x1 = track.left() + track.width() * to;
            painter.drawRoundedRect(QRectF(x0, track.bottom() + 3.0, std::max(3.0, x1 - x0), 3.0), 1.5, 1.5);
        }
    };
    strip(baked_, theme::blue());
    strip(cached_, theme::emerald());
    const qreal x = track.left() + track.width() * t;
    painter.setPen(QPen(theme::ink(), 1.5));
    painter.setBrush(theme::text());
    painter.drawEllipse(QPointF(x, track.center().y()), 7.0, 7.0);
}

void FrameScrubber::setFromX(qreal x) {
    const double span = std::max(1.0, last_ - first_);
    const double t = std::clamp((x - 2.0) / std::max(1.0, width() - 4.0), 0.0, 1.0);
    const double frame = std::round(first_ + t * span);
    if (frame != frame_) {
        frame_ = frame;
        update();
        emit scrubbed(frame_);
    }
}

void FrameScrubber::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = true;
    setFromX(event->position().x());
}

void FrameScrubber::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) setFromX(event->position().x());
}

void FrameScrubber::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

// ---- Timeline ---------------------------------------------------------------------------------

Timeline::Timeline(QWidget* parent) : GlassPanel(parent, 24.0) {
    wall_.start();
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 8, 14, 8);
    layout->setSpacing(10);

    toStart_ = transportButton("skip_back", 16, tr("To the first frame"), this);
    play_ = transportButton("play", 16, tr("Play (the clock skips frames the groom cannot keep up with)"), this);
    play_->setDiameter(36);
    play_->setFill(theme::actionFill(theme::yellow()), theme::actionFill(theme::yellow()));
    play_->setIconColor(theme::actionText(theme::ink()), theme::actionText(theme::ink()));
    toEnd_ = transportButton("skip_forward", 16, tr("To the last frame"), this);
    layout->addWidget(toStart_);
    layout->addWidget(play_);
    layout->addWidget(toEnd_);

    scrubber_ = new FrameScrubber(this);
    scrubber_->setToolTip(tr("Drag or click to scrub"));
    layout->addWidget(scrubber_, 1);

    frameLabel_ = new QLabel(this);
    frameLabel_->setFont(theme::numberFont(14, 600));
    frameLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    frameLabel_->setMinimumWidth(44);
    frameLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(frameLabel_);

    auto rangeBox = [this](const QString& tip) {
        auto* box = new QSpinBox(this);
        box->setRange(-100000, 1000000);
        box->setFont(theme::numberFont(12));
        box->setFixedWidth(64);
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        box->setToolTip(tip);
        return box;
    };
    auto* range = new QWidget(this);
    auto* rangeLayout = new QHBoxLayout(range);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    rangeLayout->setSpacing(4);
    firstBox_ = rangeBox(tr("The first frame of the range"));
    lastBox_ = rangeBox(tr("The last frame of the range"));
    auto* dash = new QLabel(QStringLiteral("–"), range);
    dash->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    rangeLayout->addWidget(firstBox_);
    rangeLayout->addWidget(dash);
    rangeLayout->addWidget(lastBox_);
    layout->addWidget(range);

    fpsLabel_ = new QLabel(this);
    fpsLabel_->setFont(theme::uiFont(12, 600));
    fpsLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    layout->addWidget(fpsLabel_);

    loop_ = chipButton(tr("Loop"), tr("Past the last frame, around to the first"), this);
    loop_->setChecked(true);
    layout->addWidget(loop_);
    follow_ = chipButton(tr("Follow"), tr("Follow the scalp: the groom evaluated on the rest pose and carried by the scalp "
                                           "(fast playback of a deforming scalp)"), this);
    follow_->hide();
    layout->addWidget(follow_);

    // The simulation: Simulate, Bake and Clear, behind a divider (shown with a Simulate node).
    sim_ = new QWidget(this);
    auto* simLayout = new QHBoxLayout(sim_);
    simLayout->setContentsMargins(0, 0, 0, 0);
    simLayout->setSpacing(6);
    auto* divider = new QWidget(sim_);
    divider->setFixedSize(1, 22);
    divider->setStyleSheet(QStringLiteral("background: %1;").arg(theme::divider().name(QColor::HexArgb)));
    simLayout->addWidget(divider);
    simulate_ = actionChip(tr("Simulate"), tr("Simulate the range: every frame into the cache (the emerald strip); scrubbing "
                                               "back reads it"), theme::emeraldChip(), theme::emeraldSoft(), sim_);
    bakeSim_ = actionChip(tr("Bake"), tr("Simulate and write the frames beside the groom (the blue strip): what renders and "
                                         "farms read once the groom is saved"), theme::chip(), theme::text(), sim_);
    clearSim_ = actionChip(tr("Clear"), tr("Drop the simulated frames and the bakes: the next frame shown simulates again"),
                           theme::chip(), theme::textMuted(), sim_);
    simLayout->addWidget(simulate_);
    simLayout->addWidget(bakeSim_);
    simLayout->addWidget(clearSim_);
    sim_->hide();
    layout->addWidget(sim_);
    connect(simulate_, &QAbstractButton::clicked, this, [this] {
        if (simBusy_) emit simulationCancelRequested();
        else emit simulateRequested();
    });
    connect(bakeSim_, &QAbstractButton::clicked, this, [this] { emit bakeRequested(); });
    connect(clearSim_, &QAbstractButton::clicked, this, [this] { emit invalidateRequested(); });

    timer_ = new QTimer(this);
    timer_->setInterval(4);  // the clock is read often; frames show only when it moved
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });

    connect(toStart_, &QAbstractButton::clicked, this, [this] {
        setFrame(first_);
        emit frameChanged(frame_);
    });
    connect(toEnd_, &QAbstractButton::clicked, this, [this] {
        setFrame(last_);
        emit frameChanged(frame_);
    });
    connect(play_, &QAbstractButton::clicked, this, [this] { setPlaying(!playing()); });
    connect(scrubber_, &FrameScrubber::scrubbed, this, [this](double frame) {
        if (playing()) setPlaying(false);
        frame_ = frame;
        refresh();
        emit frameChanged(frame_);
    });
    auto rangeEdited = [this] {
        if (updating_) return;
        const double first = firstBox_->value(), last = std::max<double>(firstBox_->value(), lastBox_->value());
        if (first == first_ && last == last_) return;
        setRange(first, last);
        emit rangeChanged(first_, last_);
        emit frameChanged(frame_);
    };
    connect(firstBox_, &QSpinBox::editingFinished, this, rangeEdited);
    connect(lastBox_, &QSpinBox::editingFinished, this, rangeEdited);
    connect(loop_, &QAbstractButton::toggled, this, [this](bool on) {
        // The clock takes the new rule from where it is now.
        if (playing()) {
            PlaybackRange r = clock_.range();
            r.loop = on;
            clock_.start(r, frame_, now());
        }
    });
    connect(follow_, &QAbstractButton::toggled, this, [this](bool on) {
        if (!updating_) emit followScalpChanged(on);
    });
    refresh();
}

void Timeline::setRange(double first, double last) {
    first_ = first;
    last_ = std::max(first, last);
    frame_ = std::clamp(frame_, first_, last_);
    scrubber_->setRange(first_, last_);
    if (playing()) {
        PlaybackRange r = clock_.range();
        r.first = first_;
        r.last = last_;
        clock_.start(r, frame_, now());
    }
    refresh();
}

void Timeline::setFps(double fps) {
    fps_ = fps > 0.0 ? fps : 24.0;
    refresh();
}

void Timeline::setFrame(double frame) {
    frame_ = std::clamp(frame, first_, last_);
    refresh();
}

bool Timeline::loop() const { return loop_->isChecked(); }

void Timeline::setPlaying(bool playing) {
    if (playing == this->playing()) return;
    if (playing) {
        PlaybackRange r;
        r.first = first_;
        r.last = last_;
        r.fps = fps_;
        r.loop = loop();
        // At the end without loop, play starts over from the first frame.
        const double from = (!r.loop && frame_ >= last_) ? first_ : frame_;
        dropped_ = 0;
        clock_.start(r, from, now());
        timer_->start();
    } else {
        clock_.stop();
        timer_->stop();
    }
    refresh();
    emit playingChanged(playing);
}

void Timeline::tick() {
    bool finished = false;
    const double frame = clock_.frameAt(now(), &finished);
    if (frame != frame_) {
        // Frames the clock passed since the last shown one (around the loop: not counted).
        const double passed = frame - frame_;
        if (passed > 1.0) dropped_ += static_cast<std::size_t>(passed - 1.0);
        frame_ = frame;
        refresh();
        emit frameChanged(frame_);
    }
    if (finished) setPlaying(false);
}

void Timeline::setFollowScalp(bool on, bool available) {
    updating_ = true;
    follow_->setVisible(available);
    follow_->setChecked(on);
    updating_ = false;
}

bool Timeline::followScalp() const { return follow_->isChecked(); }

void Timeline::setSimulationShown(bool shown) {
    sim_->setVisible(shown);
    if (!shown) scrubber_->setSimulatedFrames({}, {});
}

bool Timeline::simulationShown() const { return !sim_->isHidden(); }

void Timeline::setSimulatedFrames(FrameScrubber::Runs cached, FrameScrubber::Runs baked) {
    scrubber_->setSimulatedFrames(std::move(cached), std::move(baked));
}

void Timeline::setSimulationBusy(bool busy) {
    // Busy, the Simulate button is the run's Cancel (the window lets its click through).
    simBusy_ = busy;
    bakeSim_->setEnabled(!busy);
    clearSim_->setEnabled(!busy);
    simulate_->setText(busy ? tr("Cancel") : tr("Simulate"));
    simulate_->setToolTip(busy ? tr("Stop the simulation (Esc): the frames made so far stay in the cache")
                               : tr("Simulate the range: every frame into the cache (the emerald strip); scrubbing back reads it"));
}

void Timeline::refresh() {
    updating_ = true;
    scrubber_->setFrame(frame_);
    frameLabel_->setText(QString::number(std::lround(frame_)));
    firstBox_->setValue(static_cast<int>(std::lround(first_)));
    lastBox_->setValue(static_cast<int>(std::lround(last_)));
    fpsLabel_->setText(QStringLiteral("%1 fps").arg(fps_, 0, 'g', 4));
    play_->setIcon(iconPath(playing() ? "pause" : "play"), 16);
    play_->setToolTip(playing() ? tr("Pause") : tr("Play (the clock skips frames the groom cannot keep up with)"));
    updating_ = false;
}

}  // namespace groomlab::app
