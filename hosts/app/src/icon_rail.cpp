// SPDX-License-Identifier: Apache-2.0
#include "icon_rail.h"

#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "glass.h"
#include "theme.h"

namespace groomlab::app {

namespace {
int railSize(const char* key, int fallback, int index = -1) { return theme::component("iconRail", key, fallback, index); }
QColor railColor(const char* key, const QColor& fallback) { return theme::componentColor("iconRail", key, fallback); }
}  // namespace

IconRail::IconRail(Side side, QWidget* parent) : QWidget(parent), side_(side) {
    setObjectName(side == Side::Left ? QStringLiteral("leftRail") : QStringLiteral("rightRail"));
    setProperty("role", "iconRail");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(railWidth());
    column_ = new QVBoxLayout(this);
    const int padX = std::max(0, (railWidth() - railSize("button", 36)) / 2);
    const int padY = railSize("padding", 10, 1);
    column_->setContentsMargins(padX, padY, padX, padY);
    column_->setSpacing(railSize("gap", 4));
    column_->addStretch(1);
    setSide(side);
}

int IconRail::railWidth() { return std::clamp(railSize("width", 44), 28, 64); }

QToolButton* IconRail::addPage(const QString& icon, const QString& name, const QString& objectName) {
    auto* b = new QToolButton(this);
    b->setObjectName(objectName);
    b->setProperty("role", "railButton");
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setAccessibleName(name);
    const int size = railSize("button", 36), glyph = railSize("icon", 20);
    b->setFixedSize(size, size);
    b->setIconSize(QSize(glyph, glyph));
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("border-radius: %1px;").arg(railSize("radius", 8)));
    const int index = static_cast<int>(buttons_.size());
    connect(b, &QToolButton::clicked, this, [this, index] {
        // The open page's icon: fold (or unfold) the dock; another: open its page (unfolding it).
        if (index == current_) {
            emit collapseRequested(!collapsed_);
        } else {
            emit picked(index);
            if (collapsed_) emit collapseRequested(false);
        }
        refreshChecks();
    });
    column_->insertWidget(column_->count() - 1, b);  // (before the stretch)
    buttons_.push_back(b);
    icons_.push_back(icon);
    names_.push_back(name);
    keys_.emplace_back();
    refreshIcons();
    refreshChecks();
    return b;
}

QToolButton* IconRail::button(int index) const {
    return index >= 0 && index < count() ? buttons_[static_cast<std::size_t>(index)] : nullptr;
}

void IconRail::setPageKey(int index, const QString& key) {
    if (index < 0 || index >= count()) return;
    keys_[static_cast<std::size_t>(index)] = key;
    refreshChecks();
}

void IconRail::setFoldKey(const QString& key) {
    foldKey_ = key;
    refreshChecks();
}

void IconRail::setCurrent(int index) {
    current_ = index;
    refreshChecks();
}

void IconRail::setCollapsed(bool collapsed) {
    collapsed_ = collapsed;
    refreshChecks();
}

void IconRail::setSide(Side side) {
    side_ = side;
    setProperty("side", side == Side::Left ? QStringLiteral("left") : QStringLiteral("right"));
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void IconRail::refreshChecks() {
    // The open page's icon lit; folded, none (the dock shows no page: a mark keeps where it opens).
    // The tooltips say what a click does: the open page's folds the dock, a folded one opens it.
    for (int i = 0; i < count(); ++i) {
        QToolButton* b = buttons_[static_cast<std::size_t>(i)];
        const bool open = i == current_;
        b->setChecked(!collapsed_ && open);
        const QString& key = keys_[static_cast<std::size_t>(i)];
        const QString name = key.isEmpty() ? names_[static_cast<std::size_t>(i)] : tr("%1 (%2)").arg(names_[static_cast<std::size_t>(i)], key);
        const QString fold = foldKey_.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(foldKey_);
        b->setToolTip(!open ? name : collapsed_ ? tr("%1: click to show the panel%2").arg(name, fold) : tr("%1: click again to fold the panel%2").arg(name, fold));
    }
    update();
}

void IconRail::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QToolButton* b = button(current_);
    if (!collapsed_ || !b) return;
    // Folded: a quiet bar on the dock's side of the page that opens again.
    QPainter p(this);
    QColor marker = railColor("marker", theme::uiColor("accent"));
    marker.setAlphaF(std::clamp(theme::componentReal("iconRail", "markerAlpha", 0.9), 0.0, 1.0));
    const int w = std::clamp(railSize("markerWidth", 3), 1, 6);
    const int h = std::clamp(railSize("markerHeight", 24), 8, b->height());
    const int inset = std::clamp(railSize("markerInset", 3), 0, 8);  // (off the rail's rule)
    const int x = side_ == Side::Left ? width() - w - inset : inset;
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(marker);
    p.drawRoundedRect(QRectF(x, b->geometry().center().y() - h / 2.0 + 0.5, w, h), w / 2.0, w / 2.0);
}

bool IconRail::event(QEvent* event) {
    // A move to a screen of another pixel ratio, or a new style sheet: the icons drawn again (sharp).
    if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::StyleChange) refreshIcons();
    return QWidget::event(event);
}

void IconRail::refreshIcons() {
    const qreal dpr = devicePixelRatioF();
    const int glyph = railSize("icon", 20);
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        // At rest muted (the lit page stands out), hovered the text, on the accent's ink.
        QIcon icon;
        const QPixmap on = tintedIcon(icons_[i], railColor("inkOn", theme::accentInk()), glyph, dpr);
        const QPixmap off = tintedIcon(icons_[i], railColor("inkOff", theme::textDisabled()), glyph, dpr);
        icon.addPixmap(tintedIcon(icons_[i], railColor("ink", theme::textMuted()), glyph, dpr), QIcon::Normal, QIcon::Off);
        icon.addPixmap(tintedIcon(icons_[i], railColor("inkHover", theme::text()), glyph, dpr), QIcon::Active, QIcon::Off);
        icon.addPixmap(on, QIcon::Normal, QIcon::On);
        icon.addPixmap(on, QIcon::Active, QIcon::On);
        icon.addPixmap(off, QIcon::Disabled, QIcon::Off);
        icon.addPixmap(off, QIcon::Disabled, QIcon::On);
        buttons_[i]->setIcon(icon);
    }
}

}  // namespace groomlab::app
