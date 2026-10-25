// SPDX-License-Identifier: Apache-2.0
#include "glass.h"

#include <algorithm>
#include <cmath>

#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QToolTip>
#include <QScreen>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "theme.h"

#include "groomlab/shortcuts.h"

namespace groomlab::app {

QString trimmedNumber(double value, int decimals) {
    decimals = std::clamp(decimals, 0, 12);
    // A small number keeps two significant digits (0.035, not 0: a density fitted to a large
    // scalp), six decimals at most.
    if (value != 0.0 && std::abs(value) < 1.0)
        decimals = std::max(decimals, std::min(6, static_cast<int>(std::ceil(-std::log10(std::abs(value)))) + 1));
    QString text = QString::number(value, 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    }
    if (text == QStringLiteral("-0")) text = QStringLiteral("0");
    return text;
}

QString withShortcut(const QString& tip, const char* id) {
    const std::string keys = shortcutKeys(id, kShortcutApp);
    return keys.empty() ? tip : tip + QStringLiteral(" (") + QString::fromStdString(keys) + QStringLiteral(")");
}

// The icons are drawn from their SVGs once per resource, color, size and scale, then kept
// (QPixmapCache): a button repaints without rendering its SVG again.
QPixmap coloredIcon(const QString& resource, int size, qreal devicePixelRatio) {
    const QString key = QStringLiteral("groomlab-icon:%1:%2:%3").arg(resource).arg(size).arg(devicePixelRatio);
    QPixmap pixmap;
    if (QPixmapCache::find(key, &pixmap)) return pixmap;
    pixmap = QIcon(resource).pixmap(QSize(size, size), devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    QPixmapCache::insert(key, pixmap);
    return pixmap;
}

QPixmap tintedIcon(const QString& resource, const QColor& color, int size, qreal devicePixelRatio) {
    const QString key =
        QStringLiteral("groomlab-tint:%1:%2:%3:%4").arg(resource).arg(color.rgba(), 8, 16, QLatin1Char('0')).arg(size).arg(devicePixelRatio);
    QPixmap pixmap;
    if (QPixmapCache::find(key, &pixmap)) return pixmap;
    // The SVG's shapes as a mask: the color fills them where the icon is opaque.
    pixmap = coloredIcon(resource, size, devicePixelRatio);
    if (pixmap.isNull()) return pixmap;
    pixmap.detach();  // (the colored icon stays in the cache as drawn)
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRectF(0, 0, size, size), color);
    painter.end();
    QPixmapCache::insert(key, pixmap);
    return pixmap;
}

void paintGlass(QPainter& painter, const QRectF& rect, qreal radius, const QColor& color, const QColor& edge) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(color);
    if (edge.isValid() && edge.alpha() > 0) {
        // A solid theme's panel: a thin border inside the rect (theme::glassEdge).
        painter.setPen(QPen(edge, 1.0));
        const QRectF inner = rect.adjusted(0.5, 0.5, -0.5, -0.5);
        painter.drawRoundedRect(inner, std::max<qreal>(0.0, radius - 0.5), std::max<qreal>(0.0, radius - 0.5));
        return;
    }
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(rect, radius, radius);
}

// ---- GlassPanel -------------------------------------------------------------------------------------

GlassPanel::GlassPanel(QWidget* parent, qreal radius) : QWidget(parent), radius_(radius), color_(theme::glass()) {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_StyledBackground, false);
}

void GlassPanel::setRadius(qreal radius) {
    radius_ = radius;
    update();
}

void GlassPanel::setColor(const QColor& color) {
    color_ = color;
    update();
}

void GlassPanel::setDocked(bool docked) {
    docked_ = docked;
    if (docked) {
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
    }
    update();
}

void GlassPanel::paintEvent(QPaintEvent* /*event*/) {
    if (docked_) return;
    QPainter painter(this);
    paintGlass(painter, QRectF(rect()), radius_, color_, theme::glassEdge());
}

// ---- Windows on small screens -------------------------------------------------------------------

QRect availableGeometry(const QWidget* widget) {
    QScreen* screen = widget ? widget->screen() : nullptr;
    if (!screen) screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect();
}

void fitWindowToScreen(QWidget* window, QSize wanted) {
    const QRect available = availableGeometry(window);
    const QSize size = theme::fitToAvailable(wanted, available);
    window->resize(size);
    if (!available.isEmpty()) window->move(available.center() - QPoint(size.width() / 2, size.height() / 2));
}

// ---- FitScrollArea ------------------------------------------------------------------------------

FitScrollArea::FitScrollArea(QWidget* content, QWidget* parent) : QScrollArea(parent) {
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setAttribute(Qt::WA_TranslucentBackground, true);
    viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    setWidget(content);
}

QSize FitScrollArea::sizeHint() const {
    const QWidget* content = widget();
    if (!content) return QScrollArea::sizeHint();
    const int frame = 2 * frameWidth();
    return content->sizeHint() + QSize(frame, frame);
}

QSize FitScrollArea::minimumSizeHint() const {
    const QWidget* content = widget();
    const int width = content ? content->minimumSizeHint().width() : 0;
    return {width + 2 * frameWidth(), 48};  // as short as a row: the rest scrolls
}

// ---- RoundButton --------------------------------------------------------------------------------

RoundButton::RoundButton(QWidget* parent)
    : QAbstractButton(parent),
      iconRest_(theme::text()),
      iconChecked_(Qt::white),
      fillRest_(theme::glass()),
      fillChecked_(theme::blue()),
      textRest_(theme::text()),
      textChecked_(Qt::white) {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);  // the viewport keeps the keys
}

// The setters change nothing when given what the button has: the floating widgets are set
// often, and any change repaints every one of them (glass.h).
void RoundButton::setIcon(const QString& resource, int size) {
    if (resource == icon_ && size == iconSize_) return;
    icon_ = resource;
    iconSize_ = size;
    updateGeometry();
    update();
}

void RoundButton::setIconColor(const QColor& rest, const QColor& checked) {
    if (rest == iconRest_ && checked == iconChecked_) return;
    iconRest_ = rest;
    iconChecked_ = checked;
    update();
}

void RoundButton::setFill(const QColor& rest, const QColor& checked) {
    if (rest == fillRest_ && checked == fillChecked_) return;
    fillRest_ = rest;
    fillChecked_ = checked;
    update();
}

void RoundButton::setTextColor(const QColor& rest, const QColor& checked) {
    if (rest == textRest_ && checked == textChecked_) return;
    textRest_ = rest;
    textChecked_ = checked;
    update();
}

void RoundButton::setDiameter(int diameter) {
    if (diameter == diameter_) return;
    diameter_ = diameter;
    updateGeometry();
    update();
}

void RoundButton::setPadding(int padding) {
    padding_ = padding;
    updateGeometry();
}

void RoundButton::setFontWeight(int weight) {
    fontWeight_ = weight;
    updateGeometry();
}

void RoundButton::setFontSize(int pixelSize) {
    fontSize_ = pixelSize;
    updateGeometry();
}

void RoundButton::setDimText(const QString& dim) {
    if (dim == dim_) return;
    dim_ = dim;
    updateGeometry();
    update();
}

void RoundButton::setIconColored(bool colored) {
    colored_ = colored;
    update();
}

void RoundButton::setRadius(qreal radius) {
    radius_ = radius;
    update();
}

void RoundButton::setCheckedRing(const QColor& color, qreal width) {
    ring_ = color;
    ringWidth_ = width;
    update();
}

void RoundButton::setTileLabel(const QString& name, int labelX, qreal opacity) {
    if (name == tileLabel_ && labelX == tileLabelX_ && opacity == tileOpacity_) return;
    tileLabel_ = name;
    tileLabelX_ = labelX;
    tileOpacity_ = opacity;
    update();
}

QSize RoundButton::sizeHint() const {
    if (text().isEmpty()) return QSize(diameter_, diameter_);
    const QFontMetrics metrics(theme::uiFont(fontSize_, fontWeight_));
    int width = padding_ * 2 + metrics.horizontalAdvance(text());
    if (!icon_.isEmpty()) width += iconSize_ + 8;
    if (!dim_.isEmpty()) width += 6 + QFontMetrics(theme::uiFont(fontSize_, 600)).horizontalAdvance(dim_);
    return QSize(width, diameter_);
}

void RoundButton::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool on = isChecked();
    QColor fill = on ? fillChecked_ : fillRest_;
    if (!isEnabled()) {
        fill.setAlpha(fill.alpha() / 2);
    } else if (hover_ && fill.alpha() > 0) {
        fill = fill.lighter(on ? 108 : 125);
    } else if (hover_) {
        fill = theme::chip();
    }
    const qreal radius = radius_ >= 0.0 ? radius_ : height() / 2.0;
    const bool tile = !tileLabel_.isEmpty();
    // A tile hovers as one row (the circle and the name); its circle keeps the fill.
    QRectF circle(rect());
    if (tile) {
        circle.setWidth(std::min<qreal>(diameter_, width()));
        if (hover_ && isEnabled()) paintGlass(painter, QRectF(rect()), radius, theme::chip());
        if (!on && hover_) fill = fillRest_;
    }
    // A floating button at rest wears the panels' edge on a solid theme.
    if (fill.alpha() > 0) paintGlass(painter, circle, radius, fill, !on && fillRest_ == theme::glass() ? theme::glassEdge() : QColor());
    if (on && ringWidth_ > 0.0 && ring_.isValid()) {
        painter.setPen(QPen(ring_, ringWidth_));
        painter.setBrush(Qt::NoBrush);
        const qreal inset = ringWidth_ / 2.0;
        painter.drawRoundedRect(circle.adjusted(inset, inset, -inset, -inset), radius - inset, radius - inset);
    }
    QColor iconColor = on ? iconChecked_ : iconRest_;
    QColor textColor = on ? textChecked_ : textRest_;
    if (!isEnabled()) {
        iconColor = theme::textMuted();
        textColor = theme::textMuted();
    }
    qreal x = text().isEmpty() ? (circle.width() - iconSize_) / 2.0 : padding_;
    if (!icon_.isEmpty()) {
        const QPixmap pixmap = colored_ ? coloredIcon(icon_, iconSize_, devicePixelRatioF())
                                        : tintedIcon(icon_, iconColor, iconSize_, devicePixelRatioF());
        painter.drawPixmap(QPointF(x, (height() - iconSize_) / 2.0), pixmap);
        x += iconSize_ + 8;
    }
    if (tile && tileOpacity_ > 0.0 && tileLabelX_ < width()) {
        // The name beside the circle (the active one bright), as the rail's font.
        QColor color = on ? theme::text() : theme::textSoft();
        color.setAlphaF(color.alphaF() * tileOpacity_);
        painter.setFont(theme::uiFont(13, 700));
        painter.setPen(color);
        painter.drawText(QRectF(tileLabelX_, 0, width() - tileLabelX_, height()), Qt::AlignVCenter | Qt::AlignLeft, tileLabel_);
    }
    if (!text().isEmpty()) {
        painter.setFont(theme::uiFont(fontSize_, fontWeight_));
        painter.setPen(textColor);
        const QFontMetrics metrics(painter.font());
        const int width = metrics.horizontalAdvance(text());
        painter.drawText(QRectF(x, 0, width, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
        x += width;
        if (!dim_.isEmpty()) {
            // The dim part in the text's own color, faded: muted gray reads on glass, not on a fill.
            QColor dimColor = textColor;
            dimColor.setAlpha(140);
            painter.setFont(theme::uiFont(fontSize_, 600));
            painter.setPen(dimColor);
            painter.drawText(QRectF(x + 6, 0, width + 100, height()), Qt::AlignVCenter | Qt::AlignLeft, dim_);
        }
    }
}

void RoundButton::enterEvent(QEnterEvent* /*event*/) {
    hover_ = true;
    update();
}

void RoundButton::leaveEvent(QEvent* /*event*/) {
    hover_ = false;
    update();
}

// ---- ValueSlider ----------------------------------------------------------------------------------

namespace {

// The metrics: a row 28 px tall with the 8 px track in the middle; stacked, a 20 px caption
// line, the track under it and 6 px below, 40 in all (a comfortable target, not a hairline).
constexpr int kTrack = 8;
constexpr int kRowHeight = 28;
constexpr int kCaptionLine = 20;
constexpr int kStackedHeight = kCaptionLine + 6 + kTrack + 6;

}  // namespace

ValueSlider::ValueSlider(QWidget* parent) : QWidget(parent), fill_(theme::sliderFill(theme::blue())) {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::NoFocus);
    format_ = [this](double v) { return integer_ ? QString::number(std::lround(v)) : trimmedNumber(v, decimals_); };
}

void ValueSlider::setRange(double min, double max) {
    min_ = min;
    max_ = std::max(max, min + 1e-9);
    value_ = snapped(value_);
    update();
}

void ValueSlider::setLimits(double min, double max) {
    lowLimit_ = min;
    highLimit_ = std::max(max, min);
    value_ = snapped(value_);
    update();
}

void ValueSlider::setValue(double value) {
    value_ = snapped(value);
    // A value past the track's span (typed, or set by the host): the span grows to show it.
    if (value_ < min_) min_ = value_;
    if (value_ > max_) max_ = value_;
    update();
}

void ValueSlider::setFill(const QColor& color) {
    fill_ = theme::sliderFill(color);  // the theme's one slider color, if it has one
    update();
}

void ValueSlider::setTrackWidth(int width) {
    trackWidth_ = width;
    updateGeometry();
}

void ValueSlider::setValueWidth(int width) {
    valueWidth_ = width;
    updateGeometry();
}

void ValueSlider::setFormat(std::function<QString(double)> format) {
    format_ = std::move(format);
    update();
}

void ValueSlider::setDecimals(int decimals) {
    decimals_ = std::max(0, decimals);
    update();
}

void ValueSlider::setStep(double step) { step_ = step; }

void ValueSlider::setInteger(bool integer) {
    integer_ = integer;
    if (integer) {
        if (step_ <= 0.0) step_ = 1.0;
        value_ = snapped(value_);
    }
    update();
}

void ValueSlider::setCaption(const QString& caption) {
    caption_ = caption;
    captionWidth_ = caption.isEmpty() ? 0 : QFontMetrics(theme::uiFont(14, 600)).horizontalAdvance(caption) + 10;
    updateGeometry();
}

void ValueSlider::setStacked(bool stacked) {
    stacked_ = stacked;
    updateGeometry();
    update();
}

QSize ValueSlider::sizeHint() const {
    if (stacked_) return QSize(trackWidth_, kStackedHeight);
    return QSize(captionWidth_ + (trackWidth_ > 0 ? trackWidth_ : 96) + (valueWidth_ > 0 ? 10 + valueWidth_ : 0), kRowHeight);
}

QRect ValueSlider::trackRect() const {
    if (stacked_) return QRect(0, kCaptionLine + 6, width(), kTrack);
    // A track width of 0: the track takes the row's width the caption and the value leave.
    const int track = trackWidth_ > 0 ? trackWidth_ : std::max(24, width() - captionWidth_ - (valueWidth_ > 0 ? 10 + valueWidth_ : 0));
    return QRect(captionWidth_, (height() - kTrack) / 2, track, kTrack);
}

QRect ValueSlider::valueRect() const {
    // Where the number is drawn (and where the inline edit opens): the caption line's right
    // half stacked, after the track in a row.
    if (stacked_) {
        // The number's width (at least a third of the line): the caption takes the rest.
        const int w = std::max(width() / 3, QFontMetrics(theme::monoFont(13)).horizontalAdvance(format_(value_)) + 12);
        return QRect(width() - w, 0, w, kCaptionLine);
    }
    if (valueWidth_ <= 0) return {};
    return QRect(trackRect().right() + 10, 0, valueWidth_, height());
}

double ValueSlider::snapped(double value) const {
    value = std::clamp(value, lowLimit_, highLimit_);
    if (integer_) value = std::round(value);
    return value;
}

void ValueSlider::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool editingNow = editing();
    if (stacked_) {
        painter.setFont(theme::uiFont(13, 600));
        painter.setPen(theme::textDim());
        painter.drawText(QRect(0, 0, valueRect().left() - 6, kCaptionLine), Qt::AlignVCenter | Qt::AlignLeft, caption_);
        if (!editingNow) {
            painter.setFont(theme::monoFont(13));
            painter.setPen(theme::text());
            painter.drawText(valueRect(), Qt::AlignVCenter | Qt::AlignRight, format_(value_));
        }
    } else if (!caption_.isEmpty()) {
        painter.setFont(theme::uiFont(14, 600));
        painter.setPen(theme::textDim());
        painter.drawText(QRect(0, 0, captionWidth_ - 10, height()), Qt::AlignVCenter | Qt::AlignLeft, caption_);
    }
    const QRect track = trackRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::track());
    painter.drawRoundedRect(track, kTrack / 2.0, kTrack / 2.0);
    const double t = (value_ - min_) / (max_ - min_);
    const int filled = static_cast<int>(std::lround(track.width() * std::clamp(t, 0.0, 1.0)));
    if (filled > 0) {
        painter.setBrush(fill_);
        painter.drawRoundedRect(QRect(track.x(), track.y(), std::max(filled, kTrack), track.height()), kTrack / 2.0, kTrack / 2.0);
    }
    if (valueWidth_ > 0 && !stacked_ && !editingNow) {
        painter.setFont(theme::monoFont(13));
        painter.setPen(theme::text());
        painter.drawText(valueRect(), Qt::AlignVCenter | Qt::AlignLeft, format_(value_));
    }
}

void ValueSlider::setLive(double value) {
    value = snapped(std::clamp(value, min_, max_));
    if (value == value_) return;
    value_ = value;
    update();
    emit valueChanged(value_);
}

void ValueSlider::setFromX(qreal x) {
    const QRect track = trackRect();
    const double t = std::clamp((x - track.x()) / static_cast<double>(std::max(track.width(), 1)), 0.0, 1.0);
    double value = min_ + (max_ - min_) * t;
    if (step_ > 0.0) value = min_ + std::round((value - min_) / step_) * step_;
    setLive(value);
}

void ValueSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (editing()) commitEdit();
    dragging_ = true;
    moved_ = false;
    lastX_ = event->position().x();
    const QRect track = trackRect().adjusted(0, -6, 0, 6);
    pressOnValue_ = valueRect().contains(event->position().toPoint());
    // On the track the value jumps to the mouse; anywhere else (the caption, the number) it
    // follows the mouse's travel, so pressing a label never kicks the value away.
    relative_ = !track.contains(event->position().toPoint());
    emit pressed();
    if (!relative_) setFromX(lastX_);
    event->accept();
}

void ValueSlider::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    const qreal x = event->position().x();
    if (std::abs(x - lastX_) >= 1.0) moved_ = true;
    if (relative_) {
        const double perPixel = (max_ - min_) / static_cast<double>(std::max(trackRect().width(), 1));
        // Shift: a tenth of the travel, for fine values.
        const double scale = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 1.0;
        double value = value_ + (x - lastX_) * perPixel * scale;
        if (step_ > 0.0 && !integer_) value = min_ + std::round((value - min_) / step_) * step_;
        setLive(value);
    } else {
        setFromX(x);
    }
    lastX_ = x;
    event->accept();
}

void ValueSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_) return;
    dragging_ = false;
    emit released();
    // A click on the number (no travel) edits it.
    if (pressOnValue_ && !moved_) beginEdit();
    event->accept();
}

void ValueSlider::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = false;
    beginEdit();
    event->accept();
}

void ValueSlider::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) return;
    const double step = step_ > 0.0 ? step_ : (max_ - min_) / 50.0;
    const double value = snapped(std::clamp(value_ + steps * step, min_, max_));
    if (value != value_) {
        value_ = value;
        update();
        emit pressed();
        emit valueChanged(value_);
        emit released();
    }
    event->accept();
}

// ---- The inline edit ------------------------------------------------------------------------------

bool ValueSlider::editing() const { return editor_ && !editor_->isHidden(); }  // open, whether or not the slider is on screen

void ValueSlider::placeEditor() {
    if (!editor_) return;
    QRect at = valueRect();
    if (stacked_) at = QRect(width() / 2, 0, width() / 2, kCaptionLine);
    else at = QRect(at.x() - 4, (height() - 22) / 2, std::max(valueWidth_ + 8, 56), 22);
    editor_->setGeometry(at);
}

void ValueSlider::beginEdit() {
    if (!editor_) {
        editor_ = new QLineEdit(this);
        editor_->setFont(theme::monoFont(13));
        editor_->setAlignment(stacked_ ? Qt::AlignRight : Qt::AlignLeft);
        editor_->setStyleSheet(QStringLiteral("QLineEdit { background: rgba(255,255,255,0.10); border: 1px solid %1; border-radius: 6px; "
                                              "padding: 0 4px; color: %2; }")
                                   .arg(fill_.name(), theme::text().name()));
        editor_->installEventFilter(this);
        connect(editor_, &QLineEdit::returnPressed, this, [this] { commitEdit(); });
    }
    // The value as held (no rounding to the shown decimals: what is typed back is what was there).
    editor_->setText(integer_ ? QString::number(std::lround(value_)) : trimmedNumber(value_, std::max(decimals_, 6)));
    placeEditor();
    editor_->show();
    editor_->raise();
    editor_->selectAll();
    editor_->setFocus();
    update();
}

bool ValueSlider::commitEdit() {
    if (!editing()) return false;
    bool ok = false;
    const double typed = editor_->text().trimmed().toDouble(&ok);
    if (!ok) return false;
    editor_->hide();
    const double value = snapped(typed);
    if (value != value_) {
        // Typed past the span: the span grows to hold it (within the limits).
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
        value_ = value;
        update();
        emit pressed();
        emit valueChanged(value_);
        emit released();
    }
    update();
    return true;
}

void ValueSlider::cancelEdit() {
    if (editor_) editor_->hide();
    update();
}

QString ValueSlider::editText() const { return editing() ? editor_->text() : QString(); }

void ValueSlider::setEditText(const QString& text) {
    if (editing()) editor_->setText(text);
}

bool ValueSlider::eventFilter(QObject* watched, QEvent* event) {
    if (watched == editor_) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            cancelEdit();
            return true;
        }
        if (event->type() == QEvent::FocusOut) {
            // Leaving the field keeps a number typed, drops anything else.
            if (!commitEdit()) cancelEdit();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ValueSlider::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placeEditor();
}

// ---- Segmented ------------------------------------------------------------------------------------

Segmented::Segmented(QWidget* parent) : QWidget(parent), background_(theme::glass()) {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void Segmented::setItems(std::vector<Item> items) {
    items_ = std::move(items);
    current_ = std::clamp(current_, 0, std::max(0, static_cast<int>(items_.size()) - 1));
    updateGeometry();
    update();
}

void Segmented::setCurrent(int index) {
    current_ = std::clamp(index, 0, std::max(0, static_cast<int>(items_.size()) - 1));
    update();
}

void Segmented::setBackground(const QColor& color) {
    background_ = color;
    update();
}

void Segmented::setSegmentHeight(int height) {
    segmentHeight_ = height;
    updateGeometry();
}

void Segmented::setPadding(int padding) {
    padding_ = padding;
    updateGeometry();
}

void Segmented::setSegmentPadding(int padding) {
    segmentPadding_ = padding;
    updateGeometry();
}

void Segmented::setFontSize(int pixelSize) {
    fontSize_ = pixelSize;
    updateGeometry();
}

void Segmented::setStretch(bool stretch) {
    stretch_ = stretch;
    update();
}

QSize Segmented::sizeHint() const {
    const QFontMetrics metrics(theme::uiFont(fontSize_, 800));
    int width = padding_ * 2 + std::max(0, static_cast<int>(items_.size()) - 1) * 2;
    for (const Item& item : items_) width += metrics.horizontalAdvance(item.label) + segmentPadding_ * 2;
    return QSize(width, segmentHeight_ + padding_ * 2);
}

std::vector<QRect> Segmented::segmentRects() const {
    std::vector<QRect> rects;
    if (items_.empty()) return rects;
    const QFontMetrics metrics(theme::uiFont(fontSize_, 800));
    int x = padding_;
    const int inner = width() - padding_ * 2 - std::max(0, static_cast<int>(items_.size()) - 1) * 2;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const int w = stretch_ ? inner / static_cast<int>(items_.size())
                               : metrics.horizontalAdvance(items_[i].label) + segmentPadding_ * 2;
        rects.emplace_back(x, padding_, w, segmentHeight_);
        x += w + 2;
    }
    return rects;
}

void Segmented::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    if (!isEnabled()) painter.setOpacity(0.45);  // (the modes before a groom is open)
    paintGlass(painter, QRectF(rect()), height() / 2.0, background_, background_ == theme::glass() ? theme::glassEdge() : QColor());
    const std::vector<QRect> rects = segmentRects();
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const bool on = static_cast<int>(i) == current_;
        if (on) paintGlass(painter, QRectF(rects[i]), rects[i].height() / 2.0, theme::segmentFill(items_[i].fill));
        painter.setFont(theme::uiFont(fontSize_, on ? 800 : 600));
        painter.setPen(on ? theme::segmentText(items_[i].text) : theme::textSoft());
        painter.drawText(rects[i], Qt::AlignCenter, items_[i].label);
    }
}

void Segmented::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const std::vector<QRect> rects = segmentRects();
    for (std::size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(event->position().toPoint())) continue;
        emit clicked(static_cast<int>(i));
        if (static_cast<int>(i) != current_) {
            current_ = static_cast<int>(i);
            update();
            emit changed(current_);
        }
        break;
    }
    event->accept();
}

bool Segmented::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const std::vector<QRect> rects = segmentRects();
        for (std::size_t i = 0; i < rects.size(); ++i) {
            if (rects[i].contains(help->pos()) && !items_[i].tip.isEmpty()) {
                QToolTip::showText(help->globalPos(), items_[i].tip, this, rects[i]);
                return true;
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QWidget::event(event);
}

// ---- ShadowText -----------------------------------------------------------------------------------

ShadowText::ShadowText(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);  // the viewport under the words gets the mouse
}

bool ShadowText::setRuns(std::vector<Run> runs) {
    if (runs == runs_) return false;
    runs_ = std::move(runs);
    updateGeometry();
    update();
    return true;
}

void ShadowText::setShadow(bool shadow) {
    shadow_ = shadow;
    update();
}

QSize ShadowText::sizeHint() const {
    int width = 0, height = 0;
    for (const Run& run : runs_) {
        const QFontMetrics metrics(run.font);
        width += run.gapBefore + metrics.horizontalAdvance(run.text) + (run.dot.isValid() ? 17 : 0) + (run.pill.isValid() ? 16 : 0);
        height = std::max(height, metrics.height() + (run.pill.isValid() ? 6 : 0));
    }
    return QSize(width + 2, height + 2);
}

void ShadowText::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    qreal x = 0;
    const int h = height();
    for (const Run& run : runs_) {
        x += run.gapBefore;
        const QFontMetrics metrics(run.font);
        if (run.dot.isValid()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(run.dot);
            painter.drawEllipse(QRectF(x, h / 2.0 - 3.5, 7, 7));
            x += 17;
        }
        const int w = metrics.horizontalAdvance(run.text);
        painter.setFont(run.font);
        if (run.pill.isValid()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(run.pill);
            const QRectF pill(x, h / 2.0 - metrics.height() / 2.0 - 2, w + 16, metrics.height() + 4);
            painter.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            x += 8;
        } else if (shadow_) {
            // A soft shadow from a few offset copies: no blur effect over the viewport's texture.
            painter.setPen(QColor(0, 0, 0, 70));
            for (const QPointF& d : {QPointF(0, 1), QPointF(1, 1), QPointF(-1, 1), QPointF(0, 2)}) {
                painter.drawText(QRectF(x + d.x(), d.y(), w + 4, h), Qt::AlignVCenter | Qt::AlignLeft, run.text);
            }
        }
        painter.setPen(run.color);
        painter.drawText(QRectF(x, 0, w + 4, h), Qt::AlignVCenter | Qt::AlignLeft, run.text);
        x += w + (run.pill.isValid() ? 8 : 0);
    }
}

}  // namespace groomlab::app
