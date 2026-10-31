// SPDX-License-Identifier: Apache-2.0
#include "param_editors.h"

#include <algorithm>
#include <cmath>

#include <QColorDialog>
#include <QComboBox>
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

#include "glass.h"
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
    smooth_->setToolTip(tr("A monotone cubic through the keys: no overshoot, flat at peaks and valleys"));
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
        if (straight_ == straight) {
            smooth_->setChecked(!straight_);
            linear_->setChecked(straight_);
            return;
        }
        straight_ = straight;
        smooth_->setChecked(!straight_);
        linear_->setChecked(straight_);
        update();
        emit edited(false);
    };
    connect(smooth_, &QAbstractButton::clicked, this, [setStraight] { setStraight(false); });
    connect(linear_, &QAbstractButton::clicked, this, [setStraight] { setStraight(true); });
    connect(presets_, &QAbstractButton::clicked, this, [this] {
        QMenu menu(this);
        for (const std::string& name : presetNames()) {
            const QString title = theme::brushTitle(name);
            menu.addAction(title, this, [this, name] {
                const bool straight = straight_;
                setValue(preset(name));
                straight_ = straight;  // the interpolation is the user's, not the preset's
                update();
                emit edited(false);
            });
        }
        menu.exec(presets_->mapToGlobal(QPoint(0, presets_->height() + 4)));
    });
    setValue(json::array({json::array({0.0, 1.0})}));
}

QSize RampEditor::sizeHint() const { return QSize(220, 110 + kRampButtons); }

std::vector<std::string> RampEditor::presetNames() { return {"constant", "up", "down", "ease", "bell", "taper"}; }

json RampEditor::preset(const std::string& name) {
    auto keys = [](std::initializer_list<std::pair<float, float>> list) {
        json out = json::array();
        for (const auto& [t, v] : list) out.push_back(json::array({t, v}));
        return out;
    };
    if (name == "up") return keys({{0.0f, 0.0f}, {1.0f, 1.0f}});
    if (name == "down") return keys({{0.0f, 1.0f}, {1.0f, 0.0f}});
    if (name == "ease") return keys({{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 1.0f}});
    if (name == "bell") return keys({{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}});
    if (name == "taper") return keys({{0.0f, 1.0f}, {0.7f, 0.9f}, {1.0f, 0.2f}});
    return keys({{0.0f, 1.0f}});
}

void RampEditor::setValue(const json& value) {
    keys_.clear();
    straight_ = false;
    const json* list = &value;
    if (value.is_object() && value.contains("keys")) {
        list = &value["keys"];
        straight_ = value.value("linear", false);
    }
    if (list->is_array()) {
        for (const json& k : *list) {
            if (k.is_array() && k.size() == 2 && k[0].is_number() && k[1].is_number()) keys_.emplace_back(k[0].get<float>(), k[1].get<float>());
        }
    }
    if (keys_.empty()) keys_.emplace_back(0.0f, 1.0f);
    std::stable_sort(keys_.begin(), keys_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    selected_ = -1;
    smooth_->setChecked(!straight_);
    linear_->setChecked(straight_);
    fitRange();
    update();
}

json RampEditor::value() const {
    json keys = json::array();
    for (const auto& [t, v] : keys_) keys.push_back(json::array({t, v}));
    if (straight_) return json{{"keys", keys}, {"linear", true}};
    return keys;
}

void RampEditor::fitRange() {
    // At least 0..1; a ramp above 1 (a width profile times its width) or below 0 widens it.
    float lo = 0.0f, hi = 1.0f;
    for (const auto& [t, v] : keys_) {
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
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        const QPointF q = toScreen(keys_[i].first, keys_[i].second);
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
    Ramp ramp;
    ramp.keys = keys_;
    ramp.straight = straight_;
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
    // The keys.
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        const QPointF p = toScreen(keys_[i].first, keys_[i].second);
        const bool on = static_cast<int>(i) == selected_;
        painter.setPen(QPen(on ? theme::text() : theme::ink(), on ? 2.0 : 1.5));
        painter.setBrush(on ? theme::text() : curve);
        painter.drawEllipse(p, kKeyRadius, kKeyRadius);
    }
    // The dragged key's numbers.
    if (dragging_ && selected_ >= 0) {
        painter.setFont(theme::numberFont(11));
        painter.setPen(theme::textSoft());
        painter.drawText(plot.adjusted(8, 4, -8, -4), Qt::AlignTop | Qt::AlignRight,
                         QStringLiteral("%1, %2").arg(keys_[static_cast<std::size_t>(selected_)].first, 0, 'f', 2)
                             .arg(keys_[static_cast<std::size_t>(selected_)].second, 0, 'f', 2));
    }
}

void RampEditor::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !plotRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    selected_ = keyAt(event->position());
    dragging_ = selected_ >= 0;
    update();
}

void RampEditor::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || selected_ < 0) return;
    auto [t, v] = fromScreen(event->position());
    // A key keeps its order: between its neighbors (the first and last keys may still slide).
    const auto i = static_cast<std::size_t>(selected_);
    if (i > 0) t = std::max(t, keys_[i - 1].first);
    if (i + 1 < keys_.size()) t = std::min(t, keys_[i + 1].first);
    keys_[i] = {t, v};
    update();
    emit edited(true);
}

void RampEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_) return;
    dragging_ = false;
    fitRange();
    update();
    emit edited(false);
}

void RampEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !plotRect().contains(event->position())) return;
    const int hit = keyAt(event->position());
    if (hit >= 0) {
        if (keys_.size() > 1) {
            keys_.erase(keys_.begin() + hit);
            selected_ = -1;
            fitRange();
            update();
            emit edited(false);
        }
        return;
    }
    const auto [t, v] = fromScreen(event->position());
    keys_.emplace_back(t, v);
    std::stable_sort(keys_.begin(), keys_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    selected_ = static_cast<int>(std::find(keys_.begin(), keys_.end(), std::make_pair(t, v)) - keys_.begin());
    update();
    emit edited(false);
}

void RampEditor::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_ >= 0 && keys_.size() > 1) {
        keys_.erase(keys_.begin() + selected_);
        selected_ = -1;
        fitRange();
        update();
        emit edited(false);
        return;
    }
    QWidget::keyPressEvent(event);
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
