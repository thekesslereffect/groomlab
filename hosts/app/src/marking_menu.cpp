// SPDX-License-Identifier: Apache-2.0
#include "marking_menu.h"

#include <algorithm>
#include <cmath>

#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>

#include "glass.h"
#include "theme.h"

namespace groomlab::app {

namespace {

constexpr const char* kPositions[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
constexpr qreal kPi = 3.14159265358979323846;
// The pills' anchors from the middle: across (E, W) and up and down (N, S); the diagonals between.
constexpr qreal kReachX = 100.0, kReachY = 72.0;
constexpr qreal kPillHeight = 28.0, kIcon = 16.0, kPad = 11.0, kGap = 7.0, kCheck = 7.0;
constexpr qreal kMargin = 8.0;  // from the host's edges
// A held drag opens a submenu once it is this far toward its pill (a share of the pill's distance).
constexpr qreal kOpenShare = 0.8;

// A compass point's direction (y down): N up, then clockwise.
QPointF direction(int position) {
    const qreal a = position * kPi / 4.0;
    return {std::sin(a), -std::cos(a)};
}

QFont labelFont() { return theme::uiFont(12, QFont::Medium); }

QColor hoverFill(const MarkingMenu::Item& item) { return item.color.isValid() ? item.color : theme::chipChecked(); }

qreal pillWidth(const MarkingMenu::Item& item) {
    const QFontMetricsF fm(labelFont());
    qreal w = fm.horizontalAdvance(item.label) + 2.0 * kPad;
    if (!item.icon.isEmpty()) w += kIcon + kGap;
    if (item.checkable) w += kCheck * 2.0 + kGap;
    if (item.kind == MarkingMenu::Item::Kind::Submenu) w += 10.0;  // its chevron
    return std::ceil(w);
}

qreal between(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

}  // namespace

int MarkingMenu::positionIndex(const std::string& name) {
    for (int i = 0; i < 8; ++i) {
        if (name == kPositions[i]) return i;
    }
    return -1;
}

MarkingMenu::MarkingMenu(QWidget* host) : QWidget(host) {
    setObjectName(QStringLiteral("markingMenu"));
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    if (host) {
        setGeometry(host->rect());
        host->installEventFilter(this);
    }
    hide();
}

void MarkingMenu::setItems(std::vector<Item> items) {
    items_ = std::move(items);
    const QPointF at = levels_.empty() ? QPointF() : levels_.front().center;
    levels_.assign(1, Level{});
    levels_.front().center = at;
    hovered_ = -1;
    layoutLevel(levels_.front());
    update();
}

const std::vector<MarkingMenu::Item>& MarkingMenu::itemsOf(const Level& level) const {
    const std::vector<Item>* items = &items_;
    for (const int i : level.path) items = &(*items)[static_cast<std::size_t>(i)].items;
    return *items;
}

const std::vector<MarkingMenu::Item>& MarkingMenu::ring() const {
    static const std::vector<Item> none;
    return levels_.empty() ? none : itemsOf(levels_.back());
}

QPointF MarkingMenu::center() const { return levels_.empty() ? QPointF() : levels_.back().center; }

void MarkingMenu::layoutLevel(Level& level) {
    const std::vector<Item>& items = itemsOf(level);
    level.rects.assign(items.size(), QRectF());
    // The pills around the middle (0, 0 first): N and S centered over and under it, the east
    // side's starting near their anchors, the west side's ending near theirs.
    QRectF bounds(QPointF(-deadZone(), -deadZone()), QPointF(deadZone(), deadZone()));
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        if (item.position < 0) continue;
        const QPointF d = direction(item.position);
        const QPointF anchor(d.x() * kReachX, d.y() * kReachY);
        const qreal w = pillWidth(item);
        qreal left = anchor.x() - w / 2.0;
        if (d.x() > 0.3) left = anchor.x() - 18.0;
        else if (d.x() < -0.3) left = anchor.x() - w + 18.0;
        const QRectF r(left, anchor.y() - kPillHeight / 2.0, w, kPillHeight);
        level.rects[i] = QRectF(r.topLeft() * scale_, r.size() * scale_);  // (the size: setScale)
        bounds = bounds.united(level.rects[i]);
    }
    // Near the host's edges the middle moves in so every pill shows (as Blender's pie does); the
    // directions are from where it is drawn.
    const QRectF area = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    if (area.width() > bounds.width() && area.height() > bounds.height()) {
        level.center.setX(std::clamp(level.center.x(), area.left() - bounds.left(), area.right() - bounds.right()));
        level.center.setY(std::clamp(level.center.y(), area.top() - bounds.top(), area.bottom() - bounds.bottom()));
    }
    for (QRectF& r : level.rects) {
        if (r.isValid()) r.translate(level.center);
    }
}

QRectF MarkingMenu::itemRect(int index) const {
    if (levels_.empty()) return {};
    const std::vector<QRectF>& rects = levels_.back().rects;
    return index >= 0 && static_cast<std::size_t>(index) < rects.size() ? rects[static_cast<std::size_t>(index)] : QRectF();
}

int MarkingMenu::itemAt(QPointF at) const {
    if (levels_.empty()) return -1;
    const Level& level = levels_.back();
    const std::vector<Item>& items = itemsOf(level);
    for (std::size_t i = 0; i < level.rects.size(); ++i) {
        if (level.rects[i].contains(at)) return static_cast<int>(i);
    }
    const QPointF d = at - level.center;
    const qreal length = std::hypot(d.x(), d.y());
    if (length < deadZone()) return -1;
    // The item whose direction is closest (within the eighth around it).
    int best = -1;
    qreal bestDot = std::cos(kPi / 8.0) - 1e-6;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].position < 0) continue;
        const QPointF dir = direction(items[i].position);
        const qreal dot = (d.x() * dir.x() + d.y() * dir.y()) / length;
        if (dot > bestDot) {
            bestDot = dot;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int MarkingMenu::indexOf(const std::string& id) const {
    const std::vector<Item>& items = ring();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        if (item.id == id || (item.kind == Item::Kind::Submenu && item.label.toStdString() == id)) return static_cast<int>(i);
    }
    return -1;
}

void MarkingMenu::popup(QPointF at, bool held) {
    if (parentWidget()) setGeometry(parentWidget()->rect());
    cursor_ = pressAt_ = at;
    held_ = held;
    key_ = 0;
    moved_ = false;
    hovered_ = -1;
    levels_.assign(1, Level{});
    levels_.front().center = at;
    layoutLevel(levels_.front());
    show();
    raise();
    if (!held) setFocus(Qt::PopupFocusReason);
    update();
}

void MarkingMenu::popupHeldKey(QPointF at, int key) {
    popup(at, false);
    key_ = key;
    hover(at);
}

void MarkingMenu::open(int index) { open(index, itemRect(index).center()); }

void MarkingMenu::open(int index, QPointF at) {
    if (levels_.empty()) return;
    const std::vector<Item>& items = ring();
    if (index < 0 || static_cast<std::size_t>(index) >= items.size()) return;
    const Item& item = items[static_cast<std::size_t>(index)];
    if (item.kind != Item::Kind::Submenu || !item.enabled || item.items.empty()) return;
    Level& from = levels_.back();
    from.opened = index;
    Level next;
    next.path = from.path;
    next.path.push_back(index);
    next.center = at;
    levels_.push_back(std::move(next));
    layoutLevel(levels_.back());
    hovered_ = -1;
    update();
}

void MarkingMenu::back() {
    if (levels_.size() <= 1) {
        dismiss();
        return;
    }
    levels_.pop_back();
    levels_.back().opened = -1;
    hovered_ = -1;
    update();
}

void MarkingMenu::hover(QPointF at) {
    cursor_ = at;
    // Back into the middle of the ring this one came from: that ring again.
    if (levels_.size() > 1 && between(at, levels_[levels_.size() - 2].center) < deadZone()) {
        back();
        cursor_ = at;
    }
    hovered_ = itemAt(at);
    // A submenu opens once the cursor is over its pill, or (a held drag) most of the way to it.
    if (hovered_ >= 0) {
        const Item& item = ring()[static_cast<std::size_t>(hovered_)];
        const QRectF r = itemRect(hovered_);
        const bool reached = r.contains(at) || ((held_ || key_ != 0) && between(at, center()) >= kOpenShare * between(r.center(), center()));
        if (item.kind == Item::Kind::Submenu && item.enabled && !item.items.empty() && reached) {
            open(hovered_, at);  // around the cursor: the drag goes on from there
            hovered_ = itemAt(at);
        }
    }
    update();
}

void MarkingMenu::track(QPointF at) {
    if (!isVisible()) return;
    if (QLineF(at, pressAt_).length() >= deadZone()) moved_ = true;
    hover(at);
}

void MarkingMenu::release(QPointF at) {
    if (!isVisible() || !held_) return;
    held_ = false;
    const int index = itemAt(at);
    if (index >= 0 && (moved_ || itemRect(index).contains(at))) {
        const Item& item = ring()[static_cast<std::size_t>(index)];
        if (item.kind == Item::Kind::Submenu) {
            open(index, at);
            setFocus(Qt::PopupFocusReason);
            return;
        }
        if (item.enabled) {
            pick(index);
            return;
        }
    }
    if (moved_ && depth() == 0) {
        dismiss();  // let go over nothing
        return;
    }
    // A click, or a drag into a submenu let go over nothing: the menu stays for clicks.
    setFocus(Qt::PopupFocusReason);
    hover(at);
}

void MarkingMenu::pick(int index) {
    const Item item = ring()[static_cast<std::size_t>(index)];
    dismiss();
    emit picked(item);
}

void MarkingMenu::dismiss() {
    if (!isVisible()) return;
    held_ = false;
    key_ = 0;
    hovered_ = -1;
    hide();
    if (parentWidget()) parentWidget()->update();
    emit dismissed();
}

void MarkingMenu::mousePressEvent(QMouseEvent* event) {
    event->accept();
    if (event->button() == Qt::RightButton) {
        back();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        dismiss();
        return;
    }
    const QPointF at = event->position();
    const int index = itemAt(at);
    if (index >= 0) {
        const Item& item = ring()[static_cast<std::size_t>(index)];
        if (item.kind == Item::Kind::Submenu) open(index, at);
        else if (item.enabled) pick(index);
        return;
    }
    // The middle (or the submenu's own pill there): back a ring; anywhere else: closed.
    if (between(at, center()) < deadZone() * 2.0) back();
    else dismiss();
}

void MarkingMenu::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
    if (QLineF(event->position(), pressAt_).length() >= deadZone()) moved_ = true;
    if (!held_) hover(event->position());
}

void MarkingMenu::mouseReleaseEvent(QMouseEvent* event) { event->accept(); }

void MarkingMenu::setScale(qreal scale) {
    scale_ = std::clamp(scale, kMinScale, kMaxScale);
    for (Level& level : levels_) layoutLevel(level);
    update();
}

void MarkingMenu::wheelEvent(QWheelEvent* event) {
    // Up: bigger, down: smaller, a tenth a notch (the rings stay where they are).
    event->accept();
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) return;
    const qreal before = scale_;
    setScale(std::round((scale_ + 0.1 * notches) * 10.0) / 10.0);
    if (scale_ != before) emit scaleChanged(scale_);
}

void MarkingMenu::keyReleaseEvent(QKeyEvent* event) {
    if (key_ == 0 || event->key() != key_ || event->isAutoRepeat()) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    // The hotkey let go: what the cursor moved onto is picked (a submenu stays open), else the menu
    // stays for clicks.
    event->accept();
    key_ = 0;
    if (!moved_ || hovered_ < 0) return;
    const Item& item = ring()[static_cast<std::size_t>(hovered_)];
    if (item.kind != Item::Kind::Submenu && item.enabled) pick(hovered_);
}

void MarkingMenu::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Backspace) {
        back();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool MarkingMenu::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        if (isVisible()) {
            for (Level& level : levels_) layoutLevel(level);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MarkingMenu::paintEvent(QPaintEvent*) {
    if (levels_.empty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(labelFont());
    const qreal dpr = devicePixelRatioF();
    const QColor panel = theme::glassPanel(), edge = theme::glassEdge().isValid() ? theme::glassEdge() : theme::divider();
    const QColor shadow(0, 0, 0, 90);
    const qreal radius = 7.0;
    QColor guide = theme::text();
    guide.setAlphaF(0.55f);

    // A pill in its box `box` (this widget's pixels), drawn at the menu's own size and scaled to it.
    auto drawPill = [&](const Item& item, const QRectF& box, bool hovered, bool hub) {
        p.save();
        p.translate(box.topLeft());
        p.scale(scale_, scale_);
        const QRectF r(0.0, 0.0, box.width() / scale_, box.height() / scale_);
        const bool lit = hovered || hub;
        const QColor fill = hub ? theme::chipChecked() : hoverFill(item);
        QColor text = lit ? theme::readableOn(fill) : theme::text();
        p.setPen(Qt::NoPen);
        p.setBrush(shadow);
        p.drawRoundedRect(r.translated(0.0, 2.0), radius, radius);
        p.setBrush(lit ? fill : panel);
        p.setPen(QPen(lit ? fill : edge, 1.0));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        if (!item.enabled) text.setAlphaF(0.4f);
        qreal x = r.left() + kPad;
        if (item.checkable) {
            // The state: a ring, filled when on.
            const QPointF c(x + kCheck, r.center().y());
            const QColor mark = lit ? text : theme::chipChecked();
            p.setPen(QPen(mark, 1.4));
            p.setBrush(item.checked ? QBrush(mark) : QBrush(Qt::NoBrush));
            p.drawEllipse(c, kCheck - 2.0, kCheck - 2.0);
            x += kCheck * 2.0 + kGap;
        }
        if (!item.icon.isEmpty()) {
            QColor tint = lit ? text : (item.color.isValid() ? item.color.lighter(125) : theme::textDim());
            if (!item.enabled) tint.setAlphaF(0.4f);
            const QPixmap icon = tintedIcon(item.icon, tint, static_cast<int>(kIcon), dpr * scale_);
            p.drawPixmap(QPointF(x, r.center().y() - kIcon / 2.0), icon);
            x += kIcon + kGap;
        }
        const qreal right = r.right() - kPad + 2.0 - (item.kind == Item::Kind::Submenu && !hub ? 10.0 : 0.0);
        p.setPen(text);
        p.drawText(QRectF(x, r.top(), right - x, r.height()), Qt::AlignVCenter | Qt::AlignLeft, item.label);
        if (item.kind == Item::Kind::Submenu && !hub) {
            // A small chevron: it opens a ring.
            const qreal cx = r.right() - kPad + 1.0, cy = r.center().y();
            p.setPen(QPen(text, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(QPolygonF({QPointF(cx - 3.0, cy - 3.5), QPointF(cx + 0.5, cy), QPointF(cx - 3.0, cy + 3.5)}));
        }
        p.restore();
    };

    // The trail: each ring left behind as a faint dot, a line on to the next ring's middle.
    for (std::size_t l = 0; l + 1 < levels_.size(); ++l) {
        QColor trail = guide;
        trail.setAlphaF(0.3f);
        p.setPen(QPen(trail, 1.5 * scale_, Qt::DashLine, Qt::RoundCap));
        p.drawLine(levels_[l].center, levels_[l + 1].center);
        p.setPen(QPen(trail, 1.5 * scale_));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(levels_[l].center, 4.0 * scale_, 4.0 * scale_);
    }

    const Level& level = levels_.back();
    const std::vector<Item>& items = itemsOf(level);
    // The line from the middle to the cursor while it points somewhere.
    if (between(cursor_, level.center) >= deadZone()) {
        QColor line = guide;
        line.setAlphaF(0.35f);
        p.setPen(QPen(line, 1.5 * scale_, Qt::SolidLine, Qt::RoundCap));
        const QPointF d = cursor_ - level.center;
        const qreal len = std::hypot(d.x(), d.y());
        p.drawLine(level.center + d * (6.0 * scale_ / len), cursor_);
    }
    if (levels_.size() > 1) {
        // The submenu's own pill as the ring's hub (a click on it goes back).
        const Level& from = levels_[levels_.size() - 2];
        const Item& submenu = itemsOf(from)[static_cast<std::size_t>(from.opened)];
        const qreal w = (pillWidth(submenu) - 10.0) * scale_, h = kPillHeight * scale_;
        drawPill(submenu, QRectF(level.center.x() - w / 2.0, level.center.y() - h / 2.0, w, h), false, true);
    } else {
        p.setPen(QPen(guide, 1.5 * scale_));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(level.center, 5.0 * scale_, 5.0 * scale_);
        if (!context_.isEmpty()) {
            // What it works on, over the middle (the root ring keeps N free for it).
            p.save();
            p.translate(level.center);
            p.scale(scale_, scale_);
            QFont font = labelFont();
            font.setItalic(true);
            p.setFont(font);
            p.setPen(guide);
            p.drawText(QRectF(-160.0, -kReachY - kPillHeight / 2.0, 320.0, kPillHeight), Qt::AlignCenter, context_);
            p.restore();
        }
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!level.rects[i].isValid()) continue;
        drawPill(items[i], level.rects[i], static_cast<int>(i) == hovered_ && items[i].enabled, false);
    }
}

}  // namespace groomlab::app
