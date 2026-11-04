// SPDX-License-Identifier: Apache-2.0
#include "tube_shape_editor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "theme.h"

namespace groomlab::app {

namespace tube_editor {

namespace {
bool onAxis(const SectionPoint& p) { return std::fabs(p.u) <= kAxis; }
bool same(const SectionPoint& a, const SectionPoint& b) { return a.u == b.u && a.v == b.v; }
}  // namespace

std::vector<SectionPoint> symmetric(const std::vector<SectionPoint>& pts) {
    const int n = static_cast<int>(pts.size());
    if (n < 3) return pts;
    const float side = std::any_of(pts.begin(), pts.end(), [](const SectionPoint& p) { return p.u > kAxis; }) ? 1.0f : -1.0f;
    std::vector<bool> right(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) right[static_cast<std::size_t>(i)] = side * pts[static_cast<std::size_t>(i)].u > kAxis;
    const bool all = std::all_of(right.begin(), right.end(), [](bool r) { return r; });
    const bool any = std::any_of(right.begin(), right.end(), [](bool r) { return r; });
    if (all || !any) return pts;
    auto at = [&](int i) { return static_cast<std::size_t>(((i % n) + n) % n); };
    // The right chain: from the first right point after a point that is not, round the outline.
    int start = 0;
    while (!(right[at(start)] && !right[at(start - 1)])) ++start;
    std::vector<SectionPoint> chain;
    int i = start;
    while (right[at(i)]) {
        chain.push_back(pts[at(i)]);
        i = static_cast<int>(at(i + 1));
    }
    const SectionPoint before = pts[at(start - 1)], after = pts[at(i)];
    std::vector<SectionPoint> out;
    if (onAxis(before)) out.push_back({0.0f, before.v});
    out.insert(out.end(), chain.begin(), chain.end());
    if (onAxis(after) && (out.empty() || !same(after, before))) out.push_back({0.0f, after.v});
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) out.push_back({-it->u, it->v});
    if (side < 0.0f) {  // (the chain was the left one: the same outline, flipped back)
        std::vector<SectionPoint> flipped;
        for (auto it = out.rbegin(); it != out.rend(); ++it) flipped.push_back({-it->u, it->v});
        out = std::move(flipped);
    }
    return out.size() >= 3 ? out : pts;
}

std::vector<int> partners(const std::vector<SectionPoint>& points) {
    std::vector<int> out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SectionPoint& p = points[i];
        if (std::fabs(p.u) <= kAxis) {
            out.push_back(static_cast<int>(i));
            continue;
        }
        int best = -1;
        float dist = 1e9f;
        for (std::size_t j = 0; j < points.size(); ++j) {
            const float d = std::fabs(points[j].u + p.u) + std::fabs(points[j].v - p.v);
            if (j != i && d < dist) {
                best = static_cast<int>(j);
                dist = d;
            }
        }
        out.push_back(dist < 1e-3f ? best : -1);
    }
    return out;
}

}  // namespace tube_editor

using namespace tube_editor;

namespace {

QColor uiOr(const char* key, const QColor& fallback) {
    const QColor c = theme::uiColor(key);
    return c.isValid() ? c : fallback;
}

QString complaint(const std::string& what) {
    // "tube shape 'check': the outline crosses itself" -> what is wrong.
    const std::size_t at = what.find("': ");
    return QString::fromStdString(at == std::string::npos ? what : what.substr(at + 3));
}

}  // namespace

TubeShapeEditor::TubeShapeEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tubeShapeEditor"));
    setMinimumSize(150, 150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(190);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setToolTip(tr("The tube's cross-section: the guide runs through the cross, u across, v up (off the scalp). Drag a point to "
                  "move it, click the outline to add a point, right-click a point to remove it. Mirror keeps the left and right "
                  "sides alike."));
}

QSize TubeShapeEditor::sizeHint() const { return QSize(220, 190); }

void TubeShapeEditor::setShape(const std::vector<SectionPoint>& points, bool smooth, bool editable, const QString& note) {
    if (drag_ >= 0) return;
    points_ = points;
    smooth_ = smooth;
    editable_ = editable;
    note_ = note;
    hover_ = -1;
    updateCurve();
    update();
}

bool TubeShapeEditor::setMirror(bool on) {
    mirror_ = on;
    update();
    if (!on || !editable_) return false;
    // Already symmetric (every point has its mirror image): left as it is, wherever it starts.
    const std::vector<int> mates = partners(points_);
    if (std::all_of(mates.begin(), mates.end(), [](int m) { return m >= 0; })) return false;
    const std::vector<SectionPoint> made = symmetric(points_);
    const bool changed = made.size() != points_.size() ||
                         !std::equal(made.begin(), made.end(), points_.begin(),
                                     [](const SectionPoint& a, const SectionPoint& b) { return a.u == b.u && a.v == b.v; });
    if (!changed) return false;
    points_ = made;
    updateCurve();
    update();
    return true;
}

void TubeShapeEditor::updateCurve() {
    curve_.clear();
    problem_.clear();
    if (points_.size() >= 3) {
        try {
            curve_ = sampleTubeShapeCurve(points_, smooth_);
        } catch (const std::exception&) {
            curve_.clear();
        }
    }
    try {
        makeCustomTubeShape("check", points_, smooth_);
    } catch (const std::invalid_argument& e) {
        problem_ = complaint(e.what());
    }
}

QRectF TubeShapeEditor::square() const {
    const qreal side = std::min(width(), height()) - 12.0;
    return QRectF((width() - side) / 2.0, (height() - side) / 2.0, side, side);
}

QPointF TubeShapeEditor::toScreen(float u, float v) const {
    const QRectF r = square();
    return QPointF(r.center().x() + u / kExtent * r.width() / 2.0, r.center().y() - v / kExtent * r.height() / 2.0);
}

SectionPoint TubeShapeEditor::fromScreen(const QPointF& at) const {
    const QRectF r = square();
    const float u = static_cast<float>((at.x() - r.center().x()) / (r.width() / 2.0) * kExtent);
    const float v = static_cast<float>(-(at.y() - r.center().y()) / (r.height() / 2.0) * kExtent);
    return {std::clamp(u, -kExtent, kExtent), std::clamp(v, -kExtent, kExtent)};
}

int TubeShapeEditor::pointAt(const QPointF& at) const {
    for (std::size_t i = 0; i < points_.size(); ++i) {
        if (QLineF(toScreen(points_[i].u, points_[i].v), at).length() <= kHit) return static_cast<int>(i);
    }
    return -1;
}

std::optional<std::pair<int, SectionPoint>> TubeShapeEditor::edgeAt(const QPointF& at) const {
    const int n = static_cast<int>(points_.size());
    if (n < 3 || curve_.empty()) return std::nullopt;
    const int per = std::max(static_cast<int>(curve_.size()) / n, 1);
    int best = -1;
    qreal dist = kHit;
    for (std::size_t k = 0; k < curve_.size(); ++k) {
        const qreal d = QLineF(toScreen(curve_[k].u, curve_[k].v), at).length();
        if (d <= dist) {
            best = static_cast<int>(k);
            dist = d;
        }
    }
    if (best < 0) return std::nullopt;
    return std::make_pair(std::min(best / per, n - 1), curve_[static_cast<std::size_t>(best)]);
}

void TubeShapeEditor::movePoint(int index, float u, float v) {
    if (index < 0 || index >= static_cast<int>(points_.size())) return;
    if (mirror_) {
        const std::vector<int> mates = partners(points_);
        const int j = mates[static_cast<std::size_t>(index)];
        if (j == index) {
            u = 0.0f;
        } else if (j >= 0) {
            const float side = points_[static_cast<std::size_t>(index)].u > 0.0f ? 1.0f : -1.0f;
            u = side * std::max(side * u, kAxis * 2.0f);
            points_[static_cast<std::size_t>(j)] = {-u, v};
        }
    }
    points_[static_cast<std::size_t>(index)] = {u, v};
    updateCurve();
    update();
}

void TubeShapeEditor::addPoint(int span, SectionPoint at) {
    const int n = static_cast<int>(points_.size());
    if (n < 2 || span < 0 || span >= n) return;
    const int a = span, b = (span + 1) % n;
    std::vector<std::pair<int, SectionPoint>> inserts = {{b != 0 ? b : n, at}};
    if (mirror_) {
        const std::vector<int> mates = partners(points_);
        const int pa = mates[static_cast<std::size_t>(a)], pb = mates[static_cast<std::size_t>(b)];
        if (pa >= 0 && pb >= 0) {
            if (pa == b && pb == a) {  // the span crosses the axis: one point, on it
                inserts = {{b != 0 ? b : n, SectionPoint{0.0f, at.v}}};
            } else if (!(pa == a && pb == b)) {
                // The mirrored span runs from pb to pa (the outline goes round the other way there).
                inserts.push_back({pa != 0 ? pa : n, SectionPoint{-at.u, at.v}});
            }
        }
    }
    std::sort(inserts.begin(), inserts.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (const auto& [index, p] : inserts) points_.insert(points_.begin() + index, p);
    updateCurve();
    update();
}

bool TubeShapeEditor::removePoint(int index) {
    if (index < 0 || index >= static_cast<int>(points_.size())) return false;
    std::vector<int> gone = {index};
    if (mirror_) {
        const int mate = partners(points_)[static_cast<std::size_t>(index)];
        if (mate >= 0 && mate != index) gone.push_back(mate);
    }
    if (points_.size() - gone.size() < 3) return false;
    std::vector<SectionPoint> kept;
    for (std::size_t k = 0; k < points_.size(); ++k) {
        if (std::find(gone.begin(), gone.end(), static_cast<int>(k)) == gone.end()) kept.push_back(points_[k]);
    }
    points_ = std::move(kept);
    hover_ = -1;
    updateCurve();
    update();
    return true;
}

void TubeShapeEditor::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor accent = theme::glassy() ? theme::yellow() : theme::uiColor("accent");
    const QColor bad = theme::errorText();
    const QColor raised = uiOr("raised", theme::chip());
    painter.fillRect(rect(), theme::glassy() ? uiOr("panel", theme::glassPanel()) : theme::card());
    const QRectF r = square();
    painter.fillRect(r, theme::glassy() ? uiOr("background", theme::backdrop()) : theme::sunken());
    // The grid: the unit square (where the outline is fitted) and its halves; the axes stronger.
    painter.setPen(QPen(raised, 1));
    for (const float k : {-1.0f, -0.5f, 0.5f, 1.0f}) {
        painter.drawLine(toScreen(k, -kExtent), toScreen(k, kExtent));
        painter.drawLine(toScreen(-kExtent, k), toScreen(kExtent, k));
    }
    painter.setPen(QPen(theme::textMuted(), 1, mirror_ ? Qt::DashLine : Qt::SolidLine));
    painter.drawLine(toScreen(0.0f, -kExtent), toScreen(0.0f, kExtent));
    painter.setPen(QPen(raised, 1));
    painter.drawLine(toScreen(-kExtent, 0.0f), toScreen(kExtent, 0.0f));
    QColor line = problem_.isEmpty() ? accent : bad;
    if (!editable_) line.setAlpha(150);
    if (!curve_.empty()) {
        QPainterPath path(toScreen(curve_[0].u, curve_[0].v));
        for (std::size_t k = 1; k < curve_.size(); ++k) path.lineTo(toScreen(curve_[k].u, curve_[k].v));
        path.closeSubpath();
        QColor wash = line;
        wash.setAlpha(45);
        painter.fillPath(path, wash);
        painter.setPen(QPen(line, 2));
        painter.drawPath(path);
    }
    // The guide: a small cross at the center.
    painter.setPen(QPen(theme::text(), 1.5));
    const QPointF c = toScreen(0.0f, 0.0f);
    painter.drawLine(c + QPointF(-5, 0), c + QPointF(5, 0));
    painter.drawLine(c + QPointF(0, -5), c + QPointF(0, 5));
    if (editable_) {
        const std::vector<int> mates = mirror_ ? partners(points_) : std::vector<int>(points_.size(), -1);
        for (std::size_t i = 0; i < points_.size(); ++i) {
            const bool hot = static_cast<int>(i) == drag_ || static_cast<int>(i) == hover_;
            painter.setPen(QPen(uiOr("border", theme::divider()), 1));
            QColor dot = hot ? theme::text() : line;
            if (mirror_ && mates[i] >= 0 && mates[i] != static_cast<int>(i) && points_[i].u < 0.0f) dot.setAlpha(150);  // the mirrored side follows
            painter.setBrush(dot);
            const qreal radius = hot ? 5.0 : 4.0;
            painter.drawEllipse(toScreen(points_[i].u, points_[i].v), radius, radius);
        }
    }
    painter.setPen(problem_.isEmpty() ? theme::textMuted() : bad);
    QString text = !problem_.isEmpty() ? problem_ : note_;
    const int shown = drag_ >= 0 ? drag_ : hover_;
    if (text.isEmpty() && shown >= 0 && shown < static_cast<int>(points_.size())) {
        text = QStringLiteral("u %1  v %2").arg(points_[static_cast<std::size_t>(shown)].u, 0, 'f', 2).arg(points_[static_cast<std::size_t>(shown)].v, 0, 'f', 2);
    }
    if (!text.isEmpty()) {
        painter.drawText(QRectF(r.left() + 4, r.bottom() - 34, r.width() - 8, 32), Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, text);
    }
}

void TubeShapeEditor::mousePressEvent(QMouseEvent* event) {
    if (!editable_) return;
    const QPointF pos = event->position();
    const int i = pointAt(pos);
    if (event->button() == Qt::RightButton) {
        if (i >= 0 && removePoint(i)) emit committed(points_);
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    if (i >= 0) {
        drag_ = i;
        update();
        return;
    }
    if (const auto hit = edgeAt(pos)) {
        addPoint(hit->first, hit->second);
        emit committed(points_);
    }
}

void TubeShapeEditor::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (drag_ < 0) {
        const int hover = editable_ ? pointAt(pos) : -1;
        if (hover != hover_) {
            hover_ = hover;
            update();
        }
        return;
    }
    const SectionPoint p = fromScreen(pos);
    movePoint(drag_, p.u, p.v);
    emit dragged(points_);
}

void TubeShapeEditor::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ < 0) return;
    drag_ = -1;
    update();
    emit committed(points_);
}

void TubeShapeEditor::leaveEvent(QEvent* event) {
    if (hover_ >= 0 && drag_ < 0) {
        hover_ = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

}  // namespace groomlab::app
