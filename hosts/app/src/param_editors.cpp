// SPDX-License-Identifier: Apache-2.0
#include "param_editors.h"

#include <algorithm>
#include <cmath>

#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "definitions.h"
#include "glass.h"
#include "groomlab/graph_io.h"
#include "groomlab/map_ref.h"
#include "groomlab/ramp.h"
#include "look.h"
#include "theme.h"

namespace groomlab::app {

using nlohmann::json;

QString TrimmedSpinBox::textFromValue(double value) const { return trimmedNumber(value, decimals()); }

namespace {

constexpr int kRampButtons = 30;  // the row of buttons under the plot
constexpr qreal kKeyRadius = 5.0;
constexpr qreal kHandleLength = 30.0;  // a key's handle on screen
constexpr float kMaxSlope = 50.0f;     // a handle's slope at most this steep either way

RoundButton* chipButton(const QString& text, QWidget* parent) {
    auto* b = new RoundButton(parent);
    b->setText(text);
    b->setDiameter(24);
    b->setRadius(8.0);
    b->setPadding(10);
    b->setFontSize(12);
    b->setFontWeight(700);
    b->setFill(theme::chip(), theme::checkedFill(theme::blue()));
    b->setTextColor(theme::textSoft(), theme::checkedText(Qt::white));
    return b;
}

}  // namespace

// ---- RampEditor ---------------------------------------------------------------------------------

RampEditor::RampEditor(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(96 + kRampButtons);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addStretch(1);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 4, 0, 0);
    row->setSpacing(6);
    smooth_ = chipButton(tr("Smooth"), this);
    smooth_->setCheckable(true);
    smooth_->setToolTip(tr("A smooth curve through the keys (each key's slope its handles', or automatic: no overshoot, flat at peaks and valleys)"));
    linear_ = chipButton(tr("Straight"), this);
    linear_->setCheckable(true);
    linear_->setToolTip(tr("Straight segments between the keys"));
    presets_ = chipButton(tr("Presets"), this);
    presets_->setToolTip(tr("Replace the curve with a preset"));
    row->addWidget(smooth_);
    row->addWidget(linear_);
    row->addStretch(1);
    row->addWidget(presets_);
    layout->addLayout(row);
    auto setStraight = [this](bool straight) {
        if (ramp_.straight == straight) {
            smooth_->setChecked(!ramp_.straight);
            linear_->setChecked(ramp_.straight);
            return;
        }
        ramp_.straight = straight;
        smooth_->setChecked(!ramp_.straight);
        linear_->setChecked(ramp_.straight);
        update();
        emit edited(false);
    };
    connect(smooth_, &QAbstractButton::clicked, this, [setStraight] { setStraight(false); });
    connect(linear_, &QAbstractButton::clicked, this, [setStraight] { setStraight(true); });
    connect(presets_, &QAbstractButton::clicked, this, [this] {
        QMenu menu(this);
        menu.setToolTipsVisible(true);
        auto apply = [this](const json& keys) {
            const bool straight = ramp_.straight;
            setValue(keys);
            ramp_.straight = straight;  // the interpolation is the user's, not the preset's
            smooth_->setChecked(!ramp_.straight);
            linear_->setChecked(ramp_.straight);
            update();
            emit edited(false);
        };
        for (const json& c : ui::Definitions::shipped().choices("rampPresets")) {
            QAction* action = menu.addAction(QString::fromStdString(c.at("label").get<std::string>()), this,
                                             [apply, keys = c.at("keys")] { apply(keys); });
            action->setToolTip(QString::fromStdString(c.value("tip", std::string())));
        }
        menu.addSeparator();
        menu.addAction(tr("Flip"), this, [this, apply] { apply(flipped(value())); })
            ->setToolTip(tr("The curve mirrored end to end (what was at the start now at the end)"));
        menu.exec(presets_->mapToGlobal(QPoint(0, presets_->height() + 4)));
    });
    setValue(json::array({json::array({0.0, 1.0})}));
}

QSize RampEditor::sizeHint() const { return QSize(220, 110 + kRampButtons); }

std::vector<std::string> RampEditor::presetNames() {
    std::vector<std::string> names;
    for (const json& c : ui::Definitions::shipped().choices("rampPresets")) names.push_back(c.at("value").get<std::string>());
    return names;
}

json RampEditor::preset(const std::string& name) {
    for (const json& c : ui::Definitions::shipped().choices("rampPresets")) {
        if (c.at("value") == name) return c.at("keys");
    }
    return json::array({json::array({0.0, 1.0})});
}

json RampEditor::flipped(const json& value) {
    Ramp ramp;
    try {
        ramp = rampFromJson(value.dump());
    } catch (const std::exception&) {
        return value;
    }
    // Each key at 1 - its position, its handles mirrored (in and out swapped, each slope turned).
    Ramp out;
    out.straight = ramp.straight;
    const std::size_t n = ramp.keys.size();
    for (std::size_t k = 0; k < n; ++k) out.keys.emplace_back(1.0f - ramp.keys[n - 1 - k].first, ramp.keys[n - 1 - k].second);
    for (std::size_t k = 0; k < n; ++k) {
        const RampHandle h = ramp.handle(n - 1 - k);
        if (h.set) out.setHandle(k, RampHandle{true, -h.out, -h.in});
    }
    return json::parse(rampToJson(out));
}

void RampEditor::setValue(const json& value) {
    try {
        ramp_ = rampFromJson(value.dump());
    } catch (const std::exception&) {
        ramp_ = Ramp{};
    }
    if (ramp_.keys.empty()) ramp_ = Ramp::constant(1.0f);
    // (The value shown again after an edit of its own: the key picked keeps its handles shown.)
    if (selected_ >= static_cast<int>(ramp_.keys.size())) selected_ = -1;
    handleDrag_ = -1;
    smooth_->setChecked(!ramp_.straight);
    linear_->setChecked(ramp_.straight);
    fitRange();
    update();
}

json RampEditor::value() const { return json::parse(rampToJson(ramp_)); }

void RampEditor::select(int key) {
    selected_ = key >= 0 && static_cast<std::size_t>(key) < ramp_.keys.size() ? key : -1;
    update();
}

void RampEditor::setHandle(std::size_t key, const RampHandle& handle) {
    if (key >= ramp_.keys.size()) return;
    ramp_.setHandle(key, handle);
    update();
    emit edited(false);
}

void RampEditor::corner(std::size_t key) {
    const auto& keys = ramp_.keys;
    if (key >= keys.size()) return;
    auto secant = [&](std::size_t a, std::size_t b) {
        const float span = keys[b].first - keys[a].first;
        return span > 0.0f ? (keys[b].second - keys[a].second) / span : 0.0f;
    };
    const bool next = key + 1 < keys.size();
    const float in = key > 0 ? secant(key - 1, key) : (next ? secant(key, key + 1) : 0.0f);
    const float out = next ? secant(key, key + 1) : in;
    setHandle(key, RampHandle{true, in, out});
}

std::pair<QPointF, QPointF> RampEditor::handlePoints(std::size_t key) const {
    const QRectF r = plotRect().adjusted(kKeyRadius + 2, kKeyRadius + 2, -kKeyRadius - 2, -kKeyRadius - 2);
    const float span = std::max(hi_ - lo_, 1e-6f);
    const QPointF at = toScreen(ramp_.keys[key].first, ramp_.keys[key].second);
    const QRectF box = QRectF(rect()).adjusted(4, 4, -4, -4);  // (kept where a click reaches it)
    auto along = [&](float slope, qreal sign) {  // a handle's length along `slope`, on screen
        const qreal dx = r.width(), dy = -slope * r.height() / span;
        const qreal norm = std::max(std::hypot(dx, dy), 1e-6);
        const qreal ux = sign * dx / norm, uy = sign * dy / norm;
        qreal reach = kHandleLength;
        if (ux > 1e-6) reach = std::min(reach, (box.right() - at.x()) / ux);
        if (ux < -1e-6) reach = std::min(reach, (box.left() - at.x()) / ux);
        if (uy > 1e-6) reach = std::min(reach, (box.bottom() - at.y()) / uy);
        if (uy < -1e-6) reach = std::min(reach, (box.top() - at.y()) / uy);
        reach = std::max(reach, 8.0);
        return QPointF(at.x() + ux * reach, at.y() + uy * reach);
    };
    return {along(ramp_.tangentIn(key), -1.0), along(ramp_.tangentOut(key), 1.0)};
}

int RampEditor::handleAt(const QPointF& p) const {
    if (ramp_.straight || selected_ < 0 || static_cast<std::size_t>(selected_) >= ramp_.keys.size()) return -1;
    const auto [in, out] = handlePoints(static_cast<std::size_t>(selected_));
    if (std::hypot(out.x() - p.x(), out.y() - p.y()) < 8.0) return 1;
    if (std::hypot(in.x() - p.x(), in.y() - p.y()) < 8.0) return 0;
    return -1;
}

float RampEditor::slopeToward(std::size_t key, int side, const QPointF& p) const {
    const QRectF r = plotRect().adjusted(kKeyRadius + 2, kKeyRadius + 2, -kKeyRadius - 2, -kKeyRadius - 2);
    const QPointF at = toScreen(ramp_.keys[key].first, ramp_.keys[key].second);
    float dp = static_cast<float>((p.x() - at.x()) / std::max(r.width(), 1.0));
    float dv = static_cast<float>((at.y() - p.y()) / std::max(r.height(), 1.0)) * (hi_ - lo_);
    if (side == 0) {  // the in handle points back along the curve
        dp = -dp;
        dv = -dv;
    }
    return std::clamp(dv / std::max(dp, 1e-4f), -kMaxSlope, kMaxSlope);
}

void RampEditor::fitRange() {
    // At least 0..1; a ramp above 1 (a width profile times its width) or below 0 widens it.
    float lo = 0.0f, hi = 1.0f;
    for (const auto& [t, v] : ramp_.keys) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (!dragging_) {
        lo_ = lo < 0.0f ? std::floor(lo * 2.0f) / 2.0f : 0.0f;
        hi_ = hi > 1.0f ? std::ceil(hi * 2.0f) / 2.0f : 1.0f;
    } else {
        lo_ = std::min(lo_, lo);
        hi_ = std::max(hi_, hi);
    }
}

QRectF RampEditor::plotRect() const { return QRectF(1.0, 1.0, width() - 2.0, height() - kRampButtons - 2.0); }

QPointF RampEditor::toScreen(float t, float v) const {
    const QRectF r = plotRect().adjusted(kKeyRadius + 2, kKeyRadius + 2, -kKeyRadius - 2, -kKeyRadius - 2);
    const float span = std::max(hi_ - lo_, 1e-6f);
    return QPointF(r.left() + r.width() * t, r.bottom() - r.height() * ((v - lo_) / span));
}

std::pair<float, float> RampEditor::fromScreen(const QPointF& p) const {
    const QRectF r = plotRect().adjusted(kKeyRadius + 2, kKeyRadius + 2, -kKeyRadius - 2, -kKeyRadius - 2);
    const float t = std::clamp(static_cast<float>((p.x() - r.left()) / std::max(r.width(), 1.0)), 0.0f, 1.0f);
    const float v = lo_ + (hi_ - lo_) * static_cast<float>((r.bottom() - p.y()) / std::max(r.height(), 1.0));
    return {t, std::clamp(v, lo_, hi_)};
}

int RampEditor::keyAt(const QPointF& p) const {
    int best = -1;
    qreal bestDistance = 12.0;
    for (std::size_t i = 0; i < ramp_.keys.size(); ++i) {
        const QPointF q = toScreen(ramp_.keys[i].first, ramp_.keys[i].second);
        const qreal d = std::hypot(q.x() - p.x(), q.y() - p.y());
        if (d < bestDistance) {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void RampEditor::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF plot = plotRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 14));
    painter.drawRoundedRect(plot, 10, 10);
    // The grid: quarters across, the 0 and 1 lines up (and the halves when the range is wider).
    painter.setPen(QPen(theme::divider(), 1.0));
    for (int i = 1; i < 4; ++i) {
        const QPointF a = toScreen(0.25f * i, lo_), b = toScreen(0.25f * i, hi_);
        painter.drawLine(a, b);
    }
    for (float v = std::ceil(lo_ * 2.0f) / 2.0f; v <= hi_ + 1e-4f; v += 0.5f) {
        painter.setPen(QPen(v == 0.0f || v == 1.0f ? theme::textMuted() : theme::divider(), 1.0));
        painter.drawLine(toScreen(0.0f, v), toScreen(1.0f, v));
    }
    // The curve as the core evaluates it, filled under.
    const Ramp& ramp = ramp_;
    QPainterPath path;
    const int samples = 96;
    for (int i = 0; i <= samples; ++i) {
        const float t = static_cast<float>(i) / samples;
        const QPointF p = toScreen(t, ramp(t));
        if (i == 0) path.moveTo(p);
        else path.lineTo(p);
    }
    QPainterPath fill = path;
    fill.lineTo(toScreen(1.0f, lo_));
    fill.lineTo(toScreen(0.0f, lo_));
    fill.closeSubpath();
    const QColor curve = theme::sliderFill(theme::yellow());
    QColor under = curve;
    under.setAlpha(40);
    painter.setPen(Qt::NoPen);
    painter.setBrush(under);
    painter.drawPath(fill);
    painter.setPen(QPen(curve, 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    // The picked key's handles: a line each side, a dot at its end (hollow while automatic).
    if (!ramp_.straight && selected_ >= 0 && static_cast<std::size_t>(selected_) < ramp_.keys.size()) {
        const auto k = static_cast<std::size_t>(selected_);
        const QPointF at = toScreen(ramp_.keys[k].first, ramp_.keys[k].second);
        const auto [in, out] = handlePoints(k);
        const bool set = ramp_.handle(k).set;
        for (int side = 0; side < 2; ++side) {
            const QPointF end = side == 0 ? in : out;
            painter.setPen(QPen(theme::textMuted(), 1.0));
            painter.drawLine(at, end);
            painter.setPen(QPen(handleDrag_ == side ? theme::text() : curve, 1.5));
            painter.setBrush(handleDrag_ == side ? QBrush(theme::text()) : set ? QBrush(curve) : QBrush(Qt::NoBrush));
            painter.drawEllipse(end, 3.5, 3.5);
        }
    }
    // The keys.
    for (std::size_t i = 0; i < ramp_.keys.size(); ++i) {
        const QPointF p = toScreen(ramp_.keys[i].first, ramp_.keys[i].second);
        const bool on = static_cast<int>(i) == selected_;
        painter.setPen(QPen(on ? theme::text() : theme::ink(), on ? 2.0 : 1.5));
        painter.setBrush(on ? theme::text() : curve);
        painter.drawEllipse(p, kKeyRadius, kKeyRadius);
    }
    // The dragged key's numbers, or the dragged handle's slope.
    if (selected_ >= 0 && (dragging_ || handleDrag_ >= 0)) {
        const auto k = static_cast<std::size_t>(selected_);
        painter.setFont(theme::numberFont(11));
        painter.setPen(theme::textSoft());
        const QString text = handleDrag_ >= 0
                                 ? (handleDrag_ == 0 ? tr("slope in %1") : tr("slope out %1"))
                                       .arg(handleDrag_ == 0 ? ramp_.tangentIn(k) : ramp_.tangentOut(k), 0, 'f', 3)
                                 : QStringLiteral("%1, %2").arg(ramp_.keys[k].first, 0, 'f', 2).arg(ramp_.keys[k].second, 0, 'f', 2);
        painter.drawText(plot.adjusted(8, 4, -8, -4), Qt::AlignTop | Qt::AlignRight, text);
    }
}

void RampEditor::mousePressEvent(QMouseEvent* event) {
    // A handle of the picked key (it may reach past the plot: a steep one).
    handleDrag_ = event->button() == Qt::LeftButton ? handleAt(event->position()) : -1;
    if (handleDrag_ >= 0) {
        setFocus();
        update();
        return;
    }
    if (event->button() != Qt::LeftButton || !plotRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    selected_ = keyAt(event->position());
    dragging_ = selected_ >= 0;
    update();
}

namespace {

// Key k of `ramp` and its handles out of it (a key removed: the rest keep theirs).
void eraseKey(Ramp& ramp, std::size_t k) {
    std::vector<RampHandle> handles;
    for (std::size_t i = 0; i < ramp.keys.size(); ++i) {
        if (i != k) handles.push_back(ramp.handle(i));
    }
    ramp.keys.erase(ramp.keys.begin() + static_cast<std::ptrdiff_t>(k));
    ramp.handles = std::move(handles);
    if (!ramp.hasHandles()) ramp.handles.clear();
}

}  // namespace

void RampEditor::mouseMoveEvent(QMouseEvent* event) {
    if (handleDrag_ >= 0 && selected_ >= 0) {
        const auto k = static_cast<std::size_t>(selected_);
        const float slope = slopeToward(k, handleDrag_, event->position());
        const RampHandle had = ramp_.handle(k);
        // Alt (or a corner already): that side alone; else both sides one slope.
        if ((event->modifiers() & Qt::AltModifier) || (had.set && had.in != had.out)) {
            RampHandle h{true, ramp_.tangentIn(k), ramp_.tangentOut(k)};
            (handleDrag_ == 0 ? h.in : h.out) = slope;
            ramp_.setHandle(k, h);
        } else {
            ramp_.setHandle(k, RampHandle{true, slope, slope});
        }
        update();
        emit edited(true);
        return;
    }
    if (!dragging_ || selected_ < 0) return;
    auto [t, v] = fromScreen(event->position());
    // A key keeps its order: between its neighbors (the first and last keys may still slide).
    const auto i = static_cast<std::size_t>(selected_);
    if (i > 0) t = std::max(t, ramp_.keys[i - 1].first);
    if (i + 1 < ramp_.keys.size()) t = std::min(t, ramp_.keys[i + 1].first);
    ramp_.keys[i] = {t, v};
    update();
    emit edited(true);
}

void RampEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (handleDrag_ >= 0) {
        handleDrag_ = -1;
        update();
        emit edited(false);
        return;
    }
    if (!dragging_) return;
    dragging_ = false;
    fitRange();
    update();
    emit edited(false);
}

void RampEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const int side = handleAt(event->position());
    if (side >= 0) {  // a handle: the automatic slope again
        handleDrag_ = -1;
        setHandle(static_cast<std::size_t>(selected_), RampHandle{});
        return;
    }
    if (!plotRect().contains(event->position())) return;
    const int hit = keyAt(event->position());
    if (hit >= 0) {
        if (ramp_.keys.size() > 1) {
            eraseKey(ramp_, static_cast<std::size_t>(hit));
            selected_ = -1;
            fitRange();
            update();
            emit edited(false);
        }
        return;
    }
    const auto [t, v] = fromScreen(event->position());
    ramp_.keys.emplace_back(t, v);
    if (ramp_.hasHandles()) ramp_.handles.resize(ramp_.keys.size());
    ramp_.sort();
    selected_ = static_cast<int>(std::find(ramp_.keys.begin(), ramp_.keys.end(), std::make_pair(t, v)) - ramp_.keys.begin());
    update();
    emit edited(false);
}

void RampEditor::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_ >= 0 && ramp_.keys.size() > 1) {
        eraseKey(ramp_, static_cast<std::size_t>(selected_));
        selected_ = -1;
        fitRange();
        update();
        emit edited(false);
        return;
    }
    QWidget::keyPressEvent(event);
}

void RampEditor::contextMenuEvent(QContextMenuEvent* event) {
    const int hit = keyAt(event->pos());
    if (hit < 0) {
        QWidget::contextMenuEvent(event);
        return;
    }
    const auto k = static_cast<std::size_t>(hit);
    select(hit);
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    QAction* corner = menu.addAction(tr("Corner"), this, [this, k] { this->corner(k); });
    corner->setToolTip(tr("Straight toward each neighbor: a sharp turn at the key"));
    corner->setEnabled(!ramp_.straight);
    QAction* automatic = menu.addAction(tr("Auto Handles"), this, [this, k] { setHandle(k, RampHandle{}); });
    automatic->setToolTip(tr("The key's slope worked out from its neighbors again"));
    automatic->setEnabled(ramp_.handle(k).set);
    menu.addSeparator();
    QAction* remove = menu.addAction(tr("Remove Key"), this, [this, k] {
        eraseKey(ramp_, k);
        selected_ = -1;
        fitRange();
        update();
        emit edited(false);
    });
    remove->setEnabled(ramp_.keys.size() > 1);
    menu.exec(event->globalPos());
}

// ---- ColorEditor --------------------------------------------------------------------------------

ColorEditor::ColorEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    swatch_ = new RoundButton(this);
    swatch_->setDiameter(26);
    swatch_->setRadius(9.0);
    swatch_->setFixedWidth(36);
    swatch_->setToolTip(tr("Pick the color (shown in sRGB, stored linear)"));
    if (!theme::glassy()) swatch_->setEdge(theme::swatchEdge());  // a black or white color keeps its shape
    layout->addWidget(swatch_);
    hex_ = new QLineEdit(this);
    hex_->setMaxLength(7);
    hex_->setFont(theme::monoFont(12));
    hex_->setToolTip(tr("The color as #RRGGBB (sRGB)"));
    layout->addWidget(hex_, 1);
    connect(hex_, &QLineEdit::editingFinished, this, [this] {
        if (updating_) return;
        const QColor c(hex_->text().trimmed());
        if (c.isValid()) setShown(c, false);
        else refresh();
    });
    connect(swatch_, &QAbstractButton::clicked, this, [this] {
        // Non-modal: the dialog previews while it is open, records when it closes with OK (a
        // scripted run never clicks the swatch, so no run ever waits on it).
        auto* dialog = new QColorDialog(shown(), this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setOption(QColorDialog::DontUseNativeDialog, true);
        const std::array<float, 3> before = linear_;
        connect(dialog, &QColorDialog::currentColorChanged, this, [this](const QColor& c) { setShown(c, true); });
        connect(dialog, &QColorDialog::colorSelected, this, [this](const QColor& c) { setShown(c, false); });
        connect(dialog, &QDialog::rejected, this, [this, before] {
            linear_ = before;
            refresh();
            emit edited(false);
        });
        dialog->open();
    });
    refresh();
}

void ColorEditor::setReadOnly(bool readOnly) {
    hex_->setReadOnly(readOnly);
    swatch_->setAttribute(Qt::WA_TransparentForMouseEvents, readOnly);
    swatch_->setCursor(readOnly ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

QColor ColorEditor::shown() const {
    const RGB s = linearToSrgb(linear_);
    return QColor::fromRgbF(s[0], s[1], s[2]);
}

void ColorEditor::setLinear(const std::array<float, 3>& rgb) {
    linear_ = rgb;
    refresh();
}

void ColorEditor::setShown(const QColor& color, bool live) {
    linear_ = srgbToLinear(RGB{static_cast<float>(color.redF()), static_cast<float>(color.greenF()), static_cast<float>(color.blueF())});
    refresh();
    emit edited(live);
}

void ColorEditor::refresh() {
    updating_ = true;
    const QColor c = shown();
    swatch_->setFill(c, c);
    hex_->setText(c.name().toUpper());
    updating_ = false;
}

// ---- MapSourceEditor ----------------------------------------------------------------------------

MapSourceEditor::MapSourceEditor(QWidget* constant, QWidget* parent) : QWidget(parent), constant_(constant) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);
    sourceBox_ = new QComboBox(this);
    sourceBox_->addItems({tr("Value"), tr("Painted map"), tr("Texture file"), tr("Color set"), tr("Expression"), tr("Port")});
    sourceBox_->setToolTip(tr("Where the value comes from: a number, a map painted on this groom, an image at the scalp's "
                              "UVs, a color set of the scalp mesh, an expression of u, v and the strand's channels, or a "
                              "field port the graph wires"));
    sourceBox_->setFixedWidth(96);
    top->addWidget(sourceBox_);
    pages_ = new QStackedWidget(this);
    constant_->setParent(pages_);
    pages_->addWidget(constant_);
    painted_ = new QComboBox(pages_);
    pages_->addWidget(painted_);
    texture_ = new QLineEdit(pages_);
    texture_->setPlaceholderText(tr("image file"));
    pages_->addWidget(texture_);
    colorSet_ = new QComboBox(pages_);
    pages_->addWidget(colorSet_);
    expression_ = new QLineEdit(pages_);
    expression_->setPlaceholderText(tr("0.5 + 0.5 * sin(u * 6.2832)"));
    expression_->setFont(theme::monoFont(12));
    pages_->addWidget(expression_);
    port_ = new QLabel(pages_);
    port_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    // Its sentence must not widen the panel (a stacked page's minimum is every page's): it
    // takes the width the row has and clips.
    port_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    port_->setMinimumWidth(40);
    pages_->addWidget(port_);
    top->addWidget(pages_, 1);
    layout->addLayout(top);
    // The component read and the range the map's 0..1 maps onto, under the map sources.
    rangeRow_ = new QWidget(this);
    auto* range = new QHBoxLayout(rangeRow_);
    range->setContentsMargins(0, 0, 0, 0);
    range->setSpacing(6);
    component_ = new QComboBox(rangeRow_);
    for (const auto c : {MapRef::Component::R, MapRef::Component::G, MapRef::Component::B, MapRef::Component::A, MapRef::Component::Luminance}) {
        component_->addItem(QLatin1String(MapRef::componentName(c)));
    }
    component_->setToolTip(tr("The channel read of a color map or image"));
    range->addWidget(component_);
    auto* arrow = new QLabel(tr("range"), rangeRow_);
    arrow->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    range->addWidget(arrow);
    min_ = new TrimmedSpinBox(rangeRow_);
    min_->setRange(-1e6, 1e6);
    min_->setDecimals(3);
    min_->setKeyboardTracking(false);
    min_->setToolTip(tr("What the map's 0 becomes"));
    max_ = new TrimmedSpinBox(rangeRow_);
    max_->setRange(-1e6, 1e6);
    max_->setDecimals(3);
    max_->setValue(1.0);
    max_->setKeyboardTracking(false);
    max_->setToolTip(tr("What the map's 1 becomes"));
    range->addWidget(min_, 1);
    range->addWidget(max_, 1);
    layout->addWidget(rangeRow_);
    rangeRow_->hide();

    connect(sourceBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        const Source was = source_;
        const auto chosen = static_cast<Source>(index);
        if (was == Source::Port && chosen != Source::Port) {
            source_ = chosen;
            refreshFields();
            emit portRequested(false);
            if (chosen != Source::Constant) emit mapEdited();
            else emit sourceChanged(chosen);
            return;
        }
        source_ = chosen;
        if (chosen == Source::Port) {
            refreshFields();
            emit portRequested(true);
            return;
        }
        if (chosen == Source::Constant) {
            refreshFields();
            emit sourceChanged(chosen);
            return;
        }
        // A map source: the map kept from before comes back when it is of this source, else the
        // first choice of the source (a blank file or expression waits for the user).
        if (sourceOfMap(lastMap_) != chosen) {
            updating_ = true;
            if (chosen == Source::Painted && painted_->count() > 0) painted_->setCurrentIndex(0);
            if (chosen == Source::ColorSet && colorSet_->count() > 0) colorSet_->setCurrentIndex(0);
            if (chosen == Source::Texture) texture_->clear();
            if (chosen == Source::Expression) expression_->clear();
            updating_ = false;
        }
        refreshFields();
        const json value = mapJson();
        if (!value.is_null()) {
            lastMap_ = value;
            emit mapEdited();
        }
    });
    auto changed = [this] {
        if (updating_) return;
        const json value = mapJson();
        if (value.is_null()) return;
        lastMap_ = value;
        refreshFields();
        emit mapEdited();
    };
    connect(painted_, &QComboBox::currentIndexChanged, this, changed);
    connect(colorSet_, &QComboBox::currentIndexChanged, this, changed);
    connect(component_, &QComboBox::currentIndexChanged, this, changed);
    connect(texture_, &QLineEdit::editingFinished, this, changed);
    connect(expression_, &QLineEdit::editingFinished, this, changed);
    connect(min_, &QDoubleSpinBox::valueChanged, this, changed);
    connect(max_, &QDoubleSpinBox::valueChanged, this, changed);
    refreshFields();
}

void MapSourceEditor::chooseSource(Source source) { sourceBox_->setCurrentIndex(static_cast<int>(source)); }

void MapSourceEditor::setChoices(const Choices& choices) {
    choices_ = choices;
    updating_ = true;
    const QString painted = painted_->currentData().toString(), colorSet = colorSet_->currentText();
    painted_->clear();
    for (const Choices::Map& m : choices_.maps) {
        painted_->addItem(QStringLiteral("%1  (%2%3)").arg(QString::fromStdString(m.name), QString::fromStdString(m.kind),
                                                            m.channels == 4 ? tr(", color") : QString()),
                          QString::fromStdString(m.name));
    }
    colorSet_->clear();
    for (const std::string& s : choices_.colorSets) colorSet_->addItem(QString::fromStdString(s));
    if (!painted.isEmpty()) painted_->setCurrentIndex(std::max(0, painted_->findData(painted)));
    if (!colorSet.isEmpty()) colorSet_->setCurrentIndex(std::max(0, colorSet_->findText(colorSet)));
    updating_ = false;
}

MapSourceEditor::Source MapSourceEditor::sourceOfMap(const json& map) const {
    if (!map.is_object()) return Source::Constant;
    if (map.contains("expression")) return Source::Expression;
    if (map.contains("guide")) return Source::Painted;
    if (map.contains("colorSet")) {
        const std::string name = map["colorSet"].is_string() ? map["colorSet"].get<std::string>() : std::string();
        for (const Choices::Map& m : choices_.maps) {
            if (m.name == name && m.kind == "vertices") return Source::Painted;
        }
        return Source::ColorSet;
    }
    if (map.contains("texture")) {
        const std::string file = map["texture"].is_string() ? map["texture"].get<std::string>() : std::string();
        for (const Choices::Map& m : choices_.maps) {
            if (m.kind == "texture" && m.texture == file) return Source::Painted;
        }
        return Source::Texture;
    }
    return Source::Constant;
}

void MapSourceEditor::setValue(const json& value, bool exposed, const std::string& fedBy) {
    updating_ = true;
    exposed_ = exposed;
    if (value.is_object()) {
        lastMap_ = value;
        source_ = sourceOfMap(value);
        const std::string component = value.value("component", std::string("r"));
        component_->setCurrentIndex(std::max(0, component_->findText(QString::fromStdString(component))));
        min_->setValue(value.value("min", 0.0));
        max_->setValue(value.value("max", 1.0));
        if (source_ == Source::Painted) {
            std::string name;
            for (const char* key : {"guide", "colorSet", "texture"}) {
                if (!value.contains(key) || !value[key].is_string()) continue;
                name = value[key].get<std::string>();
                if (std::string(key) == "texture") {
                    for (const Choices::Map& m : choices_.maps) {
                        if (m.kind == "texture" && m.texture == name) name = m.name;
                    }
                }
            }
            int index = painted_->findData(QString::fromStdString(name));
            if (index < 0) {
                // A map the groom no longer has: listed as it is named, so nothing is lost.
                painted_->addItem(QString::fromStdString(name) + tr("  (missing)"), QString::fromStdString(name));
                index = painted_->count() - 1;
            }
            painted_->setCurrentIndex(index);
        } else if (source_ == Source::ColorSet) {
            const QString name = QString::fromStdString(value["colorSet"].get<std::string>());
            int index = colorSet_->findText(name);
            if (index < 0) {
                colorSet_->addItem(name);
                index = colorSet_->count() - 1;
            }
            colorSet_->setCurrentIndex(index);
        } else if (source_ == Source::Texture) {
            texture_->setText(QString::fromStdString(value["texture"].get<std::string>()));
        } else if (source_ == Source::Expression) {
            expression_->setText(QString::fromStdString(value["expression"].get<std::string>()));
        }
    } else {
        source_ = Source::Constant;
        if (value.is_number()) lastConstant_ = value;
    }
    if (exposed) source_ = Source::Port;
    port_->setText(fedBy.empty() ? tr("a Field port on the node: wire it in the graph")
                                 : tr("fed by %1").arg(QString::fromStdString(fedBy)));
    updating_ = false;
    refreshFields();
}

void MapSourceEditor::refreshFields() {
    updating_ = true;
    sourceBox_->setCurrentIndex(static_cast<int>(source_));
    pages_->setCurrentIndex(static_cast<int>(source_));
    const bool mapped = source_ == Source::Painted || source_ == Source::Texture || source_ == Source::ColorSet;
    rangeRow_->setVisible(mapped);
    // The component matters for a color map or image alone; a gray painted map reads its one value.
    bool color = source_ != Source::Painted;
    if (source_ == Source::Painted) {
        const std::string name = painted_->currentData().toString().toStdString();
        for (const Choices::Map& m : choices_.maps) {
            if (m.name == name) color = m.channels == 4;
        }
    }
    component_->setVisible(mapped && color);
    updating_ = false;
}

std::string MapSourceEditor::componentName() const { return component_->currentText().toStdString(); }

json MapSourceEditor::mapJson() const {
    json out = json::object();
    switch (source_) {
        case Source::Painted: {
            const std::string name = painted_->currentData().toString().toStdString();
            if (name.empty()) return nullptr;
            std::string key = "colorSet", stored = name;
            for (const Choices::Map& m : choices_.maps) {
                if (m.name != name) continue;
                if (m.kind == "guides") key = "guide";
                if (m.kind == "texture") {
                    key = "texture";
                    stored = m.texture;
                }
            }
            if (key == "colorSet" && lastMap_.is_object() && lastMap_.contains("guide") && lastMap_["guide"] == name) key = "guide";
            out[key] = stored;
            break;
        }
        case Source::Texture: {
            const std::string file = texture_->text().trimmed().toStdString();
            if (file.empty()) return nullptr;
            out["texture"] = file;
            break;
        }
        case Source::ColorSet: {
            const std::string name = colorSet_->currentText().toStdString();
            if (name.empty()) return nullptr;
            out["colorSet"] = name;
            break;
        }
        case Source::Expression: {
            const std::string text = expression_->text().trimmed().toStdString();
            if (text.empty()) return nullptr;
            out["expression"] = text;
            return out;  // an expression gives its value as it is
        }
        default:
            return nullptr;
    }
    out["component"] = componentName();
    out["min"] = min_->value();
    out["max"] = max_->value();
    return out;
}

json MapSourceEditor::value() const {
    if (source_ == Source::Constant || source_ == Source::Port) return lastConstant_;
    const json map = mapJson();
    return map.is_null() ? lastMap_ : map;
}

}  // namespace groomlab::app
