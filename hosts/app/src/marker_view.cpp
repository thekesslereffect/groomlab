// SPDX-License-Identifier: Apache-2.0
// The app's Wrap-style windows' mesh view (marker_view.h): a mesh, its orbit camera and one side's
// marker dots; the edits are its window's.
#include "marker_view.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include "offscreen.h"
#include "renderer.h"
#include "tool_view.h"

namespace groomlab::app {

namespace {

const QColor kHoverColor(255, 255, 255);
const QColor kBackground(46, 48, 53);

Bounds boundsOf(const ScalpMesh& mesh) {
    Bounds b;
    for (const Vec3f& p : mesh.points()) {
        if (!b.valid) {
            b.lo = b.hi = p;
            b.valid = true;
            continue;
        }
        b.lo = {std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
        b.hi = {std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
    }
    return b;
}

}  // namespace

QColor markerSideColor(MarkerSide side) { return side == MarkerSide::Old ? QColor(255, 217, 26) : QColor(38, 230, 255); }

QColor MarkerViewHost::markerColor(MarkerSide side) const { return markerSideColor(side); }

QString MarkerViewHost::emptyViewText(MarkerSide side) const {
    return side == MarkerSide::Old ? QObject::tr("The groom has no scalp") : QObject::tr("Browse for the new scalp");
}

void paintMarkerDot(QPainter& painter, const QPointF& at, int number, const QColor& sideColor, bool matched, bool lit,
                    bool faint) {
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(13);
    painter.setFont(font);
    QColor color = sideColor;
    if (faint) color.setAlpha(110);
    const double r = 5.5;
    if (lit) {
        painter.setPen(QPen(kHoverColor, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(at, r + 4.5, r + 4.5);
    }
    if (matched) {
        painter.setPen(QPen(QColor(0, 0, 0, faint ? 90 : 200), 1.5));
        painter.setBrush(color);
    } else {
        painter.setPen(QPen(color, 2.5));
        painter.setBrush(Qt::NoBrush);
    }
    painter.drawEllipse(at, r, r);
    QPainterPath path;
    path.addText(at + QPointF(r + 3.0, -r - 2.0), font, QString::number(number));
    painter.setPen(QPen(QColor(0, 0, 0, faint ? 90 : 200), 3.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.setPen(Qt::NoPen);
    painter.setBrush(lit ? kHoverColor : color);
    painter.drawPath(path);
}

// ---- MarkerView -------------------------------------------------------------------------------

MarkerView::MarkerView(MarkerViewHost* host, MarkerSide side, QWidget* parent)
    : QWidget(parent), host_(host), side_(side) {
    setMouseTracking(true);
    setMinimumSize(220, 180);  // two side by side on a 1366-wide (or 150% text) screen
    setFocusPolicy(Qt::ClickFocus);
    setObjectName(side == MarkerSide::Old ? QStringLiteral("transferOldView") : QStringLiteral("transferNewView"));
}

MarkerView::~MarkerView() { releaseGpu(); }

void MarkerView::releaseGpu() {
    if (renderer_) renderer_->releaseResources();
    target_.reset();
    renderer_.reset();
}

void MarkerView::setMesh(std::shared_ptr<const ScalpMesh> mesh, std::shared_ptr<const ScalpMesh> shown, bool reframe) {
    mesh_ = std::move(mesh);
    shown_ = mesh_ && shown ? std::move(shown) : mesh_;
    plan_.reset();
    if (shown_ != mesh_ && shown_->subdivisionLevel() > 0 && shown_->cage() && shown_->cage()->triangleCount() == mesh_->triangleCount()) {
        plan_ = refinementPlan(*shown_->cage(), *shown_);
    }
    if (!plan_) shown_ = mesh_;  // not this mesh refined: the mesh itself
    if (renderer_) renderer_->setScalp(shown_);
    if (reframe) frame();
    else cameraChanged();
}

Vec3f MarkerView::shownPoint(const MarkerPoint& p) const {
    if (!plan_ || !mesh_->contains(p.root)) return p.point;
    const StrandRoot r = plan_->refinedRoot(p.root);
    return shown_->contains(r) ? shown_->position(r) : p.point;
}

void MarkerView::frame() {
    if (shown_) camera_.frame(boundsOf(*shown_));
    cameraChanged();
}

void MarkerView::cameraChanged() {
    dirty_ = true;
    update();
}

ToolView MarkerView::toolView() const { return toolViewFor(camera_, std::max(width(), 1), std::max(height(), 1)); }

bool MarkerView::pick(const QPointF& position, MarkerPoint* hit) const {
    if (!mesh_) return false;
    const ToolView view = toolView();
    const auto port = toPort(position, std::max(height(), 1));
    Vec3f origin, direction;
    if (!view.ray(port[0], port[1], origin, direction)) return false;
    if (!pickMarkerPoint(*shown_, origin, direction, hit)) return false;
    if (plan_) {
        // Hit on the smooth surface: stored on the mesh where it shows (the point the one hit).
        const StrandRoot onMesh = plan_->cageRoot(hit->root);
        if (!mesh_->contains(onMesh)) return snapMarkerPoint(*mesh_, hit->point, hit);
        hit->root = onMesh;
    }
    return true;
}

bool MarkerView::project(const Vec3f& p, QPointF* position) const {
    float x = 0, y = 0;
    if (!toolView().project(p, x, y)) return false;
    *position = QPointF(x, static_cast<double>(std::max(height(), 1)) - y);
    return true;
}

int MarkerView::pointNear(const QPointF& position, float pixels) const {
    int best = -1;
    double bestD = static_cast<double>(pixels) * pixels;
    const auto& points = host_->markerLists().points(side_);
    for (std::size_t i = 0; i < points.size(); ++i) {
        QPointF at;
        if (!project(points[i].point, &at)) continue;
        const QPointF d = at - position;
        const double dd = d.x() * d.x() + d.y() * d.y();
        if (dd <= bestD) {
            bestD = dd;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool MarkerView::hidden(const Vec3f& p) const {
    if (!shown_) return false;
    const QVector3D e = camera_.eye();
    const Vec3f eye(e.x(), e.y(), e.z());
    const Vec3f d = p - eye;
    const float distance = d.length();
    if (distance <= 0.0f) return false;
    SurfacePoint hit;
    float t = 0.0f;
    if (!shown_->intersectRay(eye, d * (1.0f / distance), &hit, &t)) return false;
    return t < distance * (1.0f - 1e-3f);
}

QImage MarkerView::meshImage() {
    const qreal dpr = devicePixelRatioF();
    const QSize size(std::max(1, static_cast<int>(width() * dpr)), std::max(1, static_cast<int>(height() * dpr)));
    if (!dirty_ && image_.size() == size) return image_;
    QRhi* rhi = host_->rhi();
    if (!rhi || !mesh_) {
        image_ = QImage();
        return image_;
    }
    if (!renderer_) {
        renderer_ = std::make_unique<GroomRenderer>();
        renderer_->setLookdev(false);
        renderer_->setBackground(kBackground);
        renderer_->setScalp(shown_);
    }
    if (!target_ || target_->size() != size) {
        target_.reset();
        target_ = std::make_unique<OffscreenView>(size, rhi, 4);
    }
    if (!target_->ready()) {
        image_ = QImage();
        return image_;
    }
    image_ = target_->frame(*renderer_, camera_, true).image;
    if (!image_.isNull()) image_.setDevicePixelRatio(dpr);
    dirty_ = false;
    return image_;
}

void MarkerView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QImage image = meshImage();
    if (image.isNull()) {
        painter.fillRect(rect(), kBackground);
    } else {
        painter.drawImage(QPointF(0, 0), image);
    }
    if (!mesh_) {
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, host_->emptyViewText(side_));
        return;
    }
    if (!host_->markerEditing()) return;
    const MarkerLists& lists = host_->markerLists();
    const auto& points = lists.points(side_);
    const auto& hover = host_->hover();
    for (std::size_t i = 0; i < points.size(); ++i) {
        QPointF at;
        const Vec3f p = shownPoint(points[i]);
        if (!project(p, &at)) continue;
        const bool lit = hover && hover->second == i && (hover->first == side_ || lists.matched(hover->first, i));
        paintMarkerDot(painter, at, static_cast<int>(i) + 1, host_->markerColor(side_), lists.matched(side_, i), lit, hidden(p));
    }
}

void MarkerView::mousePressEvent(QMouseEvent* event) {
    last_ = event->position().toPoint();
    const bool alt = event->modifiers() & Qt::AltModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    if (event->button() == Qt::LeftButton && !alt && host_->markerEditing() && mesh_) {
        drag_ = Drag::Owned;
        const int near = pointNear(event->position());
        if (ctrl) {
            if (near >= 0) host_->deletePoint(side_, static_cast<std::size_t>(near));
            return;
        }
        if (near >= 0) {
            drag_ = Drag::Point;
            dragIndex_ = static_cast<std::size_t>(near);
            firstMove_ = true;
            return;
        }
        MarkerPoint hit;
        if (pick(event->position(), &hit)) {
            dragIndex_ = host_->addPicked(side_, hit);
            drag_ = Drag::Point;
            firstMove_ = false;  // sliding the new point joins its add
        }
        return;
    }
    if (event->button() == Qt::LeftButton) drag_ = Drag::Orbit;
    else if (event->button() == Qt::MiddleButton) drag_ = Drag::Pan;
    else if (event->button() == Qt::RightButton) drag_ = Drag::Dolly;
}

void MarkerView::mouseMoveEvent(QMouseEvent* event) {
    const QPoint at = event->position().toPoint();
    const QPoint delta = at - last_;
    last_ = at;
    switch (drag_) {
        case Drag::Orbit:
            camera_.orbit(-delta.x() * 0.4f, delta.y() * 0.4f);
            cameraChanged();
            return;
        case Drag::Pan:
            camera_.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()), height());
            cameraChanged();
            return;
        case Drag::Dolly:
            camera_.dolly(1.0f - (delta.x() + delta.y()) * 0.005f);  // as Maya: right or down zooms in
            cameraChanged();
            return;
        case Drag::Point: {
            MarkerPoint hit;
            if (pick(event->position(), &hit)) {
                host_->movePicked(side_, dragIndex_, hit, firstMove_);
                firstMove_ = false;
            }
            return;
        }
        case Drag::Owned:
            return;
        case Drag::None:
            break;
    }
    if (!host_->markerEditing()) return;
    const int near = pointNear(event->position());
    if (near >= 0) host_->setHover(std::make_pair(side_, static_cast<std::size_t>(near)));
    else if (host_->hover() && host_->hover()->first == side_) host_->setHover(std::nullopt);
}

void MarkerView::mouseReleaseEvent(QMouseEvent*) {
    const bool point = drag_ == Drag::Point;
    drag_ = Drag::None;
    if (point) host_->edited();
}

void MarkerView::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) return;
    for (int i = 0; i < std::abs(steps); ++i) camera_.dolly(steps > 0 ? 0.9f : 1.1f);
    cameraChanged();
}

void MarkerView::leaveEvent(QEvent*) {
    if (host_->hover() && host_->hover()->first == side_) host_->setHover(std::nullopt);
}

}  // namespace groomlab::app
