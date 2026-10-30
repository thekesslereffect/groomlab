// SPDX-License-Identifier: Apache-2.0
#include "glass.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <QBuffer>
#include <QEnterEvent>
#include <QFile>
#include <QImageReader>
#include <QFocusEvent>
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

QPixmap duotoneIcon(const QString& resource, const QColor& main, const QColor& ink, int size, qreal devicePixelRatio) {
    const QString key = QStringLiteral("groomlab-duotone:%1:%2:%3:%4:%5")
                            .arg(resource)
                            .arg(main.rgba(), 8, 16, QLatin1Char('0'))
                            .arg(ink.rgba(), 8, 16, QLatin1Char('0'))
                            .arg(size)
                            .arg(devicePixelRatio);
    QPixmap pixmap;
    if (QPixmapCache::find(key, &pixmap)) return pixmap;
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) return coloredIcon(resource, size, devicePixelRatio);
    QString svg = QString::fromUtf8(file.readAll());
    auto mixed = [](const QColor& a, const QColor& b, double t) {
        return QColor(static_cast<int>(std::lround(a.red() + (b.red() - a.red()) * t)), static_cast<int>(std::lround(a.green() + (b.green() - a.green()) * t)),
                      static_cast<int>(std::lround(a.blue() + (b.blue() - a.blue()) * t)));
    };
    // widgets.py's PLACEHOLDERS, in two passes (a new color may equal another placeholder).
    const std::vector<std::pair<QString, QColor>> roles = {
        {QStringLiteral("#3D7EFF"), main},
        {QStringLiteral("#1F4FB3"), mixed(main, Qt::black, 0.38)},
        {QStringLiteral("#A8C7FF"), mixed(main, Qt::white, 0.5)},
        {QStringLiteral("#F2F2F5"), ink},
        {QStringLiteral("#FF4D50"), theme::red()},
        {QStringLiteral("#FFC53E"), theme::yellow()},
        {QStringLiteral("#2F7BF7"), theme::blue()},
        {QStringLiteral("#2ECC90"), theme::emerald()},
    };
    for (std::size_t i = 0; i < roles.size(); ++i) svg.replace(roles[i].first, QStringLiteral("@role%1@").arg(i), Qt::CaseInsensitive);
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const QColor& c = roles[i].second;
        svg.replace(QStringLiteral("@role%1@").arg(i), c.isValid() ? c.name() : roles[i].first);
    }
    QByteArray bytes = svg.toUtf8();
    QBuffer buffer(&bytes);
    QImageReader reader(&buffer, "svg");
    const int pixels = std::max(1, static_cast<int>(std::lround(size * devicePixelRatio)));
    reader.setScaledSize(QSize(pixels, pixels));
    const QImage image = reader.read();
    if (image.isNull()) return coloredIcon(resource, size, devicePixelRatio);
    pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    QPixmapCache::insert(key, pixmap);
    return pixmap;
}

void setSectionHeader(QAbstractButton* header, const QString& title, bool expanded) {
    header->setText(title.toUpper().replace(QLatin1Char('&'), QStringLiteral("&&")));
    const qreal dpr = header->devicePixelRatioF();
    const QString chevron = expanded ? QStringLiteral(":/groomlab/icons/ui_chevron_down.svg") : QStringLiteral(":/groomlab/icons/ui_chevron_right.svg");
    QIcon icon;
    const QPixmap rest = tintedIcon(chevron, theme::textMuted(), 10, dpr);
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected}) {
        icon.addPixmap(rest, mode, QIcon::Off);
        icon.addPixmap(rest, mode, QIcon::On);
    }
    header->setIcon(icon);
    header->setIconSize(QSize(10, 10));
    // The capitals spaced out (0.08 em), as the design's caps headers; the size and weight are the
    // style sheet's.
    QFont font = header->font();
    if (font.letterSpacingType() != QFont::PercentageSpacing || font.letterSpacing() != 108.0) {
        font.setLetterSpacing(QFont::PercentageSpacing, 108.0);
        header->setFont(font);
    }
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

// A recursive descent over the text: expr = term (+|- term)*, term = factor (*|/ factor)*,
// factor = (+|-) factor | number | ( expr ).
class Arithmetic {
public:
    explicit Arithmetic(const QString& text) : s_(text) {}
    bool parse(double* out) {
        double v = 0.0;
        if (!expr(&v)) return false;
        skip();
        if (i_ != s_.size() || !std::isfinite(v)) return false;
        *out = v;
        return true;
    }

private:
    void skip() {
        while (i_ < s_.size() && s_[i_].isSpace()) ++i_;
    }
    bool peek(QChar c) {
        skip();
        return i_ < s_.size() && s_[i_] == c;
    }
    bool expr(double* out) {
        if (!term(out)) return false;
        while (peek(QLatin1Char('+')) || peek(QLatin1Char('-'))) {
            const bool plus = s_[i_++] == QLatin1Char('+');
            double r = 0.0;
            if (!term(&r)) return false;
            *out = plus ? *out + r : *out - r;
        }
        return true;
    }
    bool term(double* out) {
        if (!factor(out)) return false;
        while (peek(QLatin1Char('*')) || peek(QLatin1Char('/'))) {
            const bool times = s_[i_++] == QLatin1Char('*');
            double r = 0.0;
            if (!factor(&r)) return false;
            if (!times && r == 0.0) return false;
            *out = times ? *out * r : *out / r;
        }
        return true;
    }
    bool factor(double* out) {
        if (++depth_ > 64) return false;  // (a pathological nesting)
        bool ok = false;
        if (peek(QLatin1Char('+')) || peek(QLatin1Char('-'))) {
            const bool minus = s_[i_++] == QLatin1Char('-');
            ok = factor(out);
            if (ok && minus) *out = -*out;
        } else if (peek(QLatin1Char('('))) {
            ++i_;
            ok = expr(out) && peek(QLatin1Char(')'));
            if (ok) ++i_;
        } else {
            ok = number(out);
        }
        --depth_;
        return ok;
    }
    bool number(double* out) {
        skip();
        const qsizetype start = i_;
        bool digits = false;
        while (i_ < s_.size() && s_[i_].isDigit()) {
            ++i_;
            digits = true;
        }
        if (i_ < s_.size() && s_[i_] == QLatin1Char('.')) {
            ++i_;
            while (i_ < s_.size() && s_[i_].isDigit()) {
                ++i_;
                digits = true;
            }
        }
        if (!digits) return false;
        if (i_ < s_.size() && (s_[i_] == QLatin1Char('e') || s_[i_] == QLatin1Char('E'))) {
            qsizetype j = i_ + 1;
            if (j < s_.size() && (s_[j] == QLatin1Char('+') || s_[j] == QLatin1Char('-'))) ++j;
            if (j < s_.size() && s_[j].isDigit()) {
                while (j < s_.size() && s_[j].isDigit()) ++j;
                i_ = j;
            }
        }
        bool ok = false;
        *out = s_.mid(start, i_ - start).toDouble(&ok);
        return ok;
    }
    QString s_;
    qsizetype i_ = 0;
    int depth_ = 0;
};

constexpr int kPad = 8;            // the texts' inset from the field's sides
constexpr int kGap = 8;            // between the caption and the value
constexpr qreal kThreshold = 3.0;  // the travel that makes a press a drag

// A step of 1, 2 or 5 times a power of ten near `raw`.
double niceStep(double raw) {
    if (!(raw > 0.0) || !std::isfinite(raw)) return 1.0;
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    const double m = raw / p;
    return (m < 1.5 ? 1.0 : m < 3.5 ? 2.0 : m < 7.5 ? 5.0 : 10.0) * p;
}

QColor withAlpha(QColor c, int alpha) {
    c.setAlpha(alpha);
    return c;
}

QColor mixed(const QColor& a, const QColor& b, double t) {
    const auto lerp = [t](float x, float y) { return static_cast<float>(x + (y - x) * t); };
    return QColor::fromRgbF(lerp(a.redF(), b.redF()), lerp(a.greenF(), b.greenF()), lerp(a.blueF(), b.blueF()), lerp(a.alphaF(), b.alphaF()));
}

// The field's tones: a solid theme's raised field, hover and edge; a glass theme's white washes.
QColor fieldBase() { return theme::glassy() ? QColor(255, 255, 255, 20) : theme::uiColor("raised"); }
QColor fieldHover() { return theme::glassy() ? QColor(255, 255, 255, 36) : theme::uiColor("hover"); }
QColor fieldEdge() {
    const QColor edge = theme::glassEdge();
    return edge.isValid() ? edge : QColor(255, 255, 255, 26);
}
QColor fieldAccent() {
    const QColor accent = theme::uiColor("accent");
    return accent.isValid() ? accent : theme::blue();
}

}  // namespace

bool parseNumberExpression(const QString& text, double* value) {
    QString s = text.trimmed();
    s.replace(QLatin1Char(','), QLatin1Char('.'));
    if (s.isEmpty() || !value) return false;
    return Arithmetic(s).parse(value);
}

ValueSlider::ValueSlider(QWidget* parent) : QWidget(parent), fill_(theme::blue()) {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
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
    value = snapped(value);
    // A value past the range (typed, or set by the host): the range grows to show it.
    if (value == value_ && value >= min_ && value <= max_) return;
    value_ = value;
    if (value_ < min_) min_ = value_;
    if (value_ > max_) max_ = value_;
    update();
}

void ValueSlider::setFill(const QColor& color) {
    fill_ = color;
    update();
}

QColor ValueSlider::fillColor() const {
    if (theme::glassy()) return withAlpha(theme::sliderFill(fill_), 120);
    const QColor selection = theme::uiColor("selection");
    return selection.isValid() ? selection : withAlpha(theme::sliderFill(fill_), 90);
}

void ValueSlider::setTrackWidth(int width) {
    trackWidth_ = std::max(0, width);
    updateGeometry();
}

void ValueSlider::setValueWidth(int width) {
    valueWidth_ = std::max(0, width);
    updateGeometry();
}

void ValueSlider::setFormat(std::function<QString(double)> format) {
    format_ = std::move(format);
    updateGeometry();
    update();
}

void ValueSlider::setDecimals(int decimals) {
    decimals_ = std::max(0, decimals);
    update();
}

void ValueSlider::setStep(double step) { step_ = std::max(0.0, step); }

double ValueSlider::step() const {
    double s = step_ > 0.0 ? step_ : niceStep((max_ - min_) / 100.0);
    if (integer_) s = std::max(1.0, std::round(s));
    return s;
}

void ValueSlider::setInteger(bool integer) {
    integer_ = integer;
    if (integer) value_ = snapped(value_);
    update();
}

void ValueSlider::setCaption(const QString& caption) {
    if (caption == caption_) return;
    caption_ = caption;
    updateGeometry();
    update();
    placeEditor();
}

void ValueSlider::setStateColor(const QColor& color) {
    if (color == state_) return;
    state_ = color;
    update();
}

int ValueSlider::captionAdvance() const {
    return caption_.isEmpty() ? 0 : QFontMetrics(theme::uiFont(theme::fontSize(), QFont::Normal)).horizontalAdvance(caption_);
}

int ValueSlider::valueAdvance() const {
    return std::max(valueWidth_, QFontMetrics(theme::numberFont(theme::fontSize())).horizontalAdvance(format_(value_)));
}

QSize ValueSlider::sizeHint() const {
    const int texts = kPad + captionAdvance() + (caption_.isEmpty() ? 0 : kGap) + valueAdvance() + kPad;
    return QSize(std::max({trackWidth_, texts, 64}), theme::rowHeight());
}

QSize ValueSlider::minimumSizeHint() const { return QSize(48, theme::rowHeight()); }

double ValueSlider::snapped(double value) const {
    value = std::clamp(value, lowLimit_, highLimit_);
    if (integer_) value = std::round(value);
    return value;
}

void ValueSlider::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool enabled = isEnabled();
    const bool editingNow = editing();
    const bool focusShown = keyFocus_ && hasFocus();
    if (!enabled) painter.setOpacity(0.45);
    const QRectF field = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = theme::controlRadius();
    QPainterPath shape;
    shape.addRoundedRect(field, radius, radius);

    // The field, the fill clipped to its corners, then the edge over both.
    QColor base = editingNow ? theme::backdrop() : (hover_ && enabled ? fieldHover() : fieldBase());
    QColor fill = fillColor();
    QColor edge = editingNow || focusShown ? fieldAccent() : fieldEdge();
    if (state_.isValid() && !editingNow) {
        base = mixed(base, withAlpha(state_, 255), 0.22);
        fill = mixed(fill, withAlpha(state_, 255), 0.45);
        if (!focusShown) edge = state_;
    }
    painter.fillPath(shape, base);
    if (!editingNow) {
        const double t = std::clamp((value_ - min_) / (max_ - min_), 0.0, 1.0);
        const qreal filled = std::round(width() * t);
        if (filled > 0.0) {
            painter.save();
            painter.setClipPath(shape);
            painter.fillRect(QRectF(0.0, 0.0, filled, height()), fill);
            painter.restore();
        }
    }
    painter.setPen(QPen(edge, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);

    // The texts: the caption at the left (elided to what the value leaves), the value at the right
    // (centered without a caption); the inline edit takes the value's place.
    const QString shown = format_(value_);
    const QFont numbers = theme::numberFont(theme::fontSize());
    const int valueW = QFontMetrics(numbers).horizontalAdvance(shown);
    const QRect inner = rect().adjusted(kPad, 0, -kPad, 0);
    if (!caption_.isEmpty()) {
        const QFont words = theme::uiFont(theme::fontSize(), QFont::Normal);
        const int room = editingNow ? std::max(0, inner.width() / 2 - kGap) : std::max(0, inner.width() - valueW - kGap);
        painter.setFont(words);
        painter.setPen(theme::textDim());
        painter.drawText(QRect(inner.x(), 0, room, height()), Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(words).elidedText(caption_, Qt::ElideRight, room));
    }
    if (!editingNow) {
        painter.setFont(numbers);
        painter.setPen(theme::text());
        painter.drawText(inner, Qt::AlignVCenter | (caption_.isEmpty() ? Qt::AlignHCenter : Qt::AlignRight), shown);
    }
}

void ValueSlider::setLive(double value) {
    value = snapped(std::clamp(value, min_, max_));
    if (value == value_) return;
    value_ = value;
    update();
    emit valueChanged(value_);
}

void ValueSlider::stepBy(double steps) {
    const double s = step();
    double value = std::round((value_ + steps * s) / s) * s;  // (on the step's grid)
    value = snapped(std::clamp(value, min_, max_));
    if (value == value_) return;
    value_ = value;
    update();
    emit pressed();
    emit valueChanged(value_);
    emit released();
}

void ValueSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (editing()) commitEdit();
    dragging_ = true;
    moved_ = false;
    pressX_ = lastX_ = event->position().x();
    raw_ = value_;
    event->accept();
}

void ValueSlider::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    const qreal x = event->position().x();
    if (!moved_) {
        if (std::abs(x - pressX_) < kThreshold) return;
        moved_ = true;  // (the travel counts from the press: the threshold is no dead zone in the value)
        emit pressed();
    }
    // The field's width is the range, Shift a tenth of the speed: the value moves by the mouse's
    // travel (no jump to where it is), kept unsnapped in raw_ so small moves add up.
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool fine = mods.testFlag(Qt::ShiftModifier), coarse = mods.testFlag(Qt::ControlModifier);
    const double perPixel = (max_ - min_) / static_cast<double>(std::max(width(), 1));
    raw_ = std::clamp(raw_ + (x - lastX_) * perPixel * (fine ? 0.1 : 1.0), min_, max_);
    lastX_ = x;
    // Ctrl: steps of ten; else the step set (a tenth of it with Shift), none for a free slider.
    double snap = coarse ? step() * 10.0 : step_ > 0.0 ? (fine ? step_ / 10.0 : step_) : 0.0;
    double value = snap > 0.0 ? std::round(raw_ / snap) * snap : raw_;
    setLive(value);
    event->accept();
}

void ValueSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_ || event->button() != Qt::LeftButton) return;
    dragging_ = false;
    if (moved_) emit released();
    else beginEdit();  // a click: type the value
    event->accept();
}

void ValueSlider::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = false;
    if (!editing()) beginEdit();
    event->accept();
}

void ValueSlider::wheelEvent(QWheelEvent* event) {
    // Only a focused field takes the wheel: over a panel it scrolls the panel.
    const int steps = event->angleDelta().y() / 120;
    if (!hasFocus() || editing() || steps == 0) {
        event->ignore();
        return;
    }
    stepBy(event->modifiers().testFlag(Qt::ShiftModifier) ? steps * 10.0 : steps);
    event->accept();
}

void ValueSlider::keyPressEvent(QKeyEvent* event) {
    const double times = event->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_F2:
        beginEdit();
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        stepBy(times);
        break;
    case Qt::Key_Left:
    case Qt::Key_Down:
        stepBy(-times);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void ValueSlider::focusInEvent(QFocusEvent* event) {
    // The accent edge for a field reached by the keyboard (as :focus-visible), kept when the edit
    // hands the focus back.
    const Qt::FocusReason reason = event->reason();
    keyFocus_ = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason ||
                (reason == Qt::OtherFocusReason && keyFocus_);
    QWidget::focusInEvent(event);
    update();
}

void ValueSlider::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    update();
}

void ValueSlider::enterEvent(QEnterEvent* event) {
    hover_ = true;
    update();
    QWidget::enterEvent(event);
}

void ValueSlider::leaveEvent(QEvent* event) {
    hover_ = false;
    update();
    QWidget::leaveEvent(event);
}

void ValueSlider::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange) {
        if (!isEnabled() && editing()) cancelEdit();
        update();
    }
    QWidget::changeEvent(event);
}

// ---- The inline edit ------------------------------------------------------------------------------

bool ValueSlider::editing() const { return editor_ && !editor_->isHidden(); }  // open, whether or not the slider is on screen

void ValueSlider::placeEditor() {
    if (!editor_) return;
    // Over the value's half (the caption keeps the left one), else the whole field; inside the edge.
    const QRect inner = rect().adjusted(kPad - 2, 2, -(kPad - 2), -2);
    QRect at = inner;
    if (!caption_.isEmpty()) at.setLeft(inner.x() + inner.width() / 2);
    editor_->setGeometry(at);
}

void ValueSlider::beginEdit() {
    if (!isEnabled()) return;
    if (!editor_) {
        editor_ = new QLineEdit(this);
        editor_->setFrame(false);
        editor_->installEventFilter(this);
        connect(editor_, &QLineEdit::returnPressed, this, [this] { commitEdit(); });
    }
    // The theme's now (a theme switch restyles the next edit).
    editor_->setFont(theme::numberFont(theme::fontSize()));
    editor_->setAlignment((caption_.isEmpty() ? Qt::AlignHCenter : Qt::AlignRight) | Qt::AlignVCenter);
    const QColor accent = fieldAccent();
    editor_->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 0 2px; color: %1; "
                                          "selection-background-color: %2; selection-color: %3; }")
                               .arg(theme::text().name(), accent.name(), theme::readableOn(accent).name()));
    // The value as held (no rounding to the shown decimals: what is typed back is what was there).
    editor_->setText(integer_ ? QString::number(std::lround(value_)) : trimmedNumber(value_, std::max(decimals_, 6)));
    placeEditor();
    editor_->show();
    editor_->raise();
    editor_->selectAll();
    editor_->setFocus(Qt::OtherFocusReason);
    update();
}

void ValueSlider::closeEditor() {
    if (!editor_) return;
    // The focus back on the field (after Enter or Escape the arrows go on working) before the edit
    // hides, so it does not wander to the next widget; that focus-out is no commit.
    closing_ = true;
    if (editor_->hasFocus()) setFocus(Qt::OtherFocusReason);
    editor_->hide();
    closing_ = false;
    update();
}

bool ValueSlider::commitEdit() {
    if (!editing()) return false;
    double typed = 0.0;
    if (!parseNumberExpression(editor_->text(), &typed)) return false;
    closeEditor();
    const double value = snapped(typed);
    if (value != value_) {
        // Typed past the range: the range grows to hold it (within the limits).
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
        value_ = value;
        updateGeometry();
        update();
        emit pressed();
        emit valueChanged(value_);
        emit released();
    }
    return true;
}

void ValueSlider::cancelEdit() { closeEditor(); }

QString ValueSlider::editText() const { return editing() ? editor_->text() : QString(); }

void ValueSlider::setEditText(const QString& text) {
    if (editing()) editor_->setText(text);
}

bool ValueSlider::eventFilter(QObject* watched, QEvent* event) {
    if (watched == editor_) {
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Escape) {
                cancelEdit();
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                commitEdit();  // (no number: the edit stays open)
                return true;
            }
        }
        if (event->type() == QEvent::FocusOut && !closing_ && editing()) {
            // Leaving the field (Tab, a click elsewhere) keeps a number typed, drops anything else.
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
