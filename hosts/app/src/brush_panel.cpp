// SPDX-License-Identifier: Apache-2.0
#include "brush_panel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "groomlab/brush_alpha.h"
#include "groomlab/brush_catalog.h"

#include "alpha_picker.h"
#include "brush_session.h"
#include "card_parts.h"
#include "glass.h"
#include "graph_presets.h"
#include "groom_document.h"
#include "param_editors.h"
#include "theme.h"
#include "tube_panel.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

QString settingLabel(const char* id) {
    const groomlab::BrushSettingSpec* spec = brushSettingSpec(id);
    return spec ? qs(spec->label) : QString::fromLatin1(id);
}

QString settingHint(const char* id) {
    const groomlab::BrushSettingSpec* spec = brushSettingSpec(id);
    return spec ? qs(spec->hint) : QString();
}

// A text of the Groom Tools (palette.json's texts): the app's own words when "labels" has them.
QString text(const char* name) {
    const ui::json* t = defs().find(std::string("palette/texts/labels/") + ui::kHost + "/" + name);
    if (!t) t = defs().find(std::string("palette/texts/") + name);
    return t && t->is_string() ? qs(t->get<std::string>()) : QString();
}

QLabel* hintLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setProperty("role", "hint");
    return label;
}

// A row's name in a label column (combos and toggles only: Stamp, Place, Mirror).
QHBoxLayout* labeledRow(QVBoxLayout* into, const QString& name, QWidget* parent) {
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    auto* label = new QLabel(name, parent);
    label->setProperty("role", "rowLabel");
    label->setMinimumWidth(58);
    row->addWidget(label);
    into->addLayout(row);
    return row;
}

// The ? glyph's tip: the catalog's full hint, then the gestures (palette texts.gestures, the
// app's words) as a small list, each "keys: what".
QString helpTip(const groomlab::BrushEntry& e, bool gestures) {
    QString html = QStringLiteral("<p style='margin:0'><b>%1</b></p><p style='margin:4px 0 0 0'>%2</p>").arg(qs(e.label).toHtmlEscaped(), qs(e.hint).toHtmlEscaped());
    if (!gestures) return html;
    html += QStringLiteral("<table style='margin-top:6px' cellspacing='0' cellpadding='1'>");
    for (const QString& part : text("gestures").split(QStringLiteral(" · "), Qt::SkipEmptyParts)) {
        const int colon = part.indexOf(QStringLiteral(": "));
        const QString keys = colon > 0 ? part.left(colon) : part, what = colon > 0 ? part.mid(colon + 2) : QString();
        html += QStringLiteral("<tr><td style='padding-right:8px; white-space:nowrap'><b>%1</b></td><td>%2</td></tr>")
                    .arg(keys.trimmed().toHtmlEscaped(), what.trimmed().toHtmlEscaped());
    }
    return html + QStringLiteral("</table>");
}

void collectBlocks(const QLayout* layout, const QWidget* top, std::vector<std::string>& out) {
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (QWidget* w = item->widget()) {
            if (w->isHidden()) continue;
            if (auto* block = qobject_cast<CardBlock*>(w)) out.push_back(block->title().toStdString());
            else if (w->layout()) collectBlocks(w->layout(), top, out);
        } else if (QLayout* inner = item->layout()) {
            collectBlocks(inner, top, out);
        }
    }
}

}  // namespace

BrushPanel::BrushPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("toolCard"));
    setProperty("role", "toolCard");
    card_ = new QVBoxLayout(this);
    card_->setContentsMargins(0, 8, 0, 2);
    card_->setSpacing(4);

    // ---- The title: the tool's icon and name, the ? (hint and gestures), the short line.
    auto* titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(6);
    icon_ = new QLabel(this);
    icon_->setObjectName(QStringLiteral("toolCardIcon"));
    icon_->setFixedSize(16, 16);
    titleRow->addWidget(icon_);
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("toolCardTitle"));
    title_->setProperty("role", "cardTitle");
    titleRow->addWidget(title_, 1);
    help_ = makeGlyph(QStringLiteral("help"), QString(), this);
    help_->setObjectName(QStringLiteral("toolCardHelp"));
    titleRow->addWidget(help_);
    card_->addLayout(titleRow);
    short_ = hintLabel(QString(), this);
    short_->setObjectName(QStringLiteral("toolCardShort"));
    card_->addWidget(short_);

    // ---- Brush: how the stroke feels.
    brush_ = new CardBlock(text("brushBlock"), this);
    brush_->setObjectName(QStringLiteral("brushBlock"));
    QVBoxLayout* rows = brush_->rows();
    radius_ = slider(rows, settingLabel("radius"), 5.0, 400.0, 1.0, 2000.0, 0, [this] { return session_->settings().screenRadius; },
                     [this](double v) { session_->settings().setScreenRadius(static_cast<float>(v)); },
                     withShortcut(tr("Screen pixels: the circle's radius on screen, whatever the zoom (middle-drag sets it too)"), "brush.radius"));
    radius_->setObjectName(QStringLiteral("radius"));
    radius_->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral(" px"); });
    strength_ = slider(rows, settingLabel("strength"), 0.0, 1.0, -10.0, 10.0, 2, [this] { return session_->settings().brush.strength; },
                       [this](double v) { session_->settings().brush.strength = static_cast<float>(v); },
                       withShortcut(settingHint("strength") + tr(" (Shift+middle-drag sets it too)"), "brush.strength"));
    strength_->setObjectName(QStringLiteral("strength"));
    // Falloff as the catalog names it in both hosts: 0 hard to the edge, 1 soft from the center
    // (the core's hardness is 1 - falloff).
    falloff_ = slider(rows, settingLabel("falloff"), 0.0, 1.0, 0.0, 1.0, 2, [this] { return 1.0 - session_->settings().brush.hardness; },
                      [this](double v) { session_->settings().setHardness(static_cast<float>(1.0 - v)); }, settingHint("falloff"));
    falloff_->setObjectName(QStringLiteral("falloff"));
    smooth_ = slider(rows, settingLabel("smoothStrength"), 0.0, 3.0, 0.0, 100.0, 2, [this] { return session_->settings().smoothStrength; },
                     [this](double v) { session_->settings().setSmoothStrength(static_cast<float>(v)); }, settingHint("smoothStrength"));
    smooth_->setObjectName(QStringLiteral("smoothStrength"));
    {
        // The chips, in a fixed order (a chip never moves between tools): Pressure, Off scalp,
        // Falloff curve, Root to tip, then the app's own Propagate and Pointwise.
        FlowLayout* chips = nullptr;
        rows->addWidget(chipRow(brush_, &chips));
        pressure_ = settingChip(chips, "pressure", tr("Pressure"), [this] { return session_->settings().pressure; },
                                [this](bool on) { session_->settings().pressure = on; });
        collide_ = settingChip(chips, "collide", tr("Off scalp"), [this] { return session_->settings().brush.collide; },
                               [this](bool on) { session_->settings().brush.collide = on; });
        auto* ramps = new QVBoxLayout();
        ramps->setContentsMargins(0, 0, 0, 0);
        ramps->setSpacing(4);
        curve(chips, ramps, "falloffProfile", tr("Falloff curve"), "[[0, 1], [0.5, 0.56], [1, 0]]",
              [this] { return session_->settings().falloffProfileJson(); }, [this](const std::string& v) { session_->settings().setFalloffProfile(v); });
        falloffCurve_ = findChild<QToolButton*>(QStringLiteral("falloffProfile"));
        falloffRamp_ = findChild<QWidget*>(QStringLiteral("falloffProfileCurve"));
        curve(chips, ramps, "strengthProfile", tr("Root to tip"), "[[0, 0.25], [1, 1]]",
              [this] { return session_->settings().strengthProfileJson(); }, [this](const std::string& v) { session_->settings().setStrengthProfile(v); });
        strengthCurve_ = findChild<QToolButton*>(QStringLiteral("strengthProfile"));
        strengthRamp_ = findChild<QWidget*>(QStringLiteral("strengthProfileCurve"));
        propagate_ = chip(chips, tr("Propagate"), [this] { return session_->settings().brush.propagate; },
                          [this](bool on) { session_->settings().brush.propagate = on; },
                          tr("Also edit the locked descendants (children that keep their own shape)"));
        propagate_->setObjectName(QStringLiteral("propagate"));
        pointwise_ = chip(chips, tr("Pointwise"), [this] { return session_->settings().brush.pointwise; },
                          [this](bool on) { session_->settings().brush.pointwise = on; },
                          tr("Paint a per-point layer along the guide instead of one value per guide"));
        pointwise_->setObjectName(QStringLiteral("pointwise"));
        rows->addLayout(ramps);
    }
    card_->addWidget(brush_);

    // ---- Paint: the maps panel's Mode, Value + Fill and color (setPaintControls), the stamp.
    if (shippedBrushAlphaDir().empty()) setShippedBrushAlphaDir(shippedAlphaFolder());
    paint_ = new CardBlock(tr("Paint"), this);
    paint_->setObjectName(QStringLiteral("paintBlock"));
    paintSlot_ = new QVBoxLayout();
    paintSlot_->setContentsMargins(0, 0, 0, 0);
    paintSlot_->setSpacing(4);
    paint_->rows()->addLayout(paintSlot_);
    alphaBox_ = new QWidget(paint_);
    alphaBox_->setObjectName(QStringLiteral("alphaBox"));
    {
        auto* column = new QVBoxLayout(alphaBox_);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);
        QHBoxLayout* row = labeledRow(column, tr("Stamp"), alphaBox_);
        alpha_ = new AlphaPicker(alphaBox_);
        alpha_->setToolTip(settingHint("alpha"));
        row->addWidget(alpha_, 1);
        connect(alpha_, &AlphaPicker::changed, this, [this](const QString& path) {
            alphaOptions_->setVisible(!path.isEmpty());
            if (updating_ || !session_) return;
            try {
                session_->settings().setAlpha(path.toStdString());
            } catch (const std::exception&) {
                alpha_->setPath({}, false);
                session_->settings().setAlpha({});
            }
            edited();
        });
        refreshers_.push_back([this] { alpha_->setPath(session_->settings().alphaPath, false); });
        alphaOptions_ = new QWidget(alphaBox_);
        auto* options = new QVBoxLayout(alphaOptions_);
        options->setContentsMargins(0, 0, 0, 0);
        options->setSpacing(4);
        QHBoxLayout* turn = labeledRow(options, tr("Turn"), alphaOptions_);
        alphaRotation_ = new QComboBox(alphaOptions_);
        alphaRotation_->setObjectName(QStringLiteral("alphaRotation"));
        for (const auto& [name, label, tip] : {std::tuple<const char*, QString, QString>{"fixed", tr("Fixed"), tr("Every stamp at the angle")},
                                               {"stroke", tr("Follow Stroke"), tr("Each stamp turned the way the stroke goes, plus the angle")},
                                               {"random", tr("Random"), tr("Each stamp at a random angle")}}) {
            alphaRotation_->addItem(label, QString::fromLatin1(name));
            alphaRotation_->setItemData(alphaRotation_->count() - 1, tip, Qt::ToolTipRole);
        }
        turn->addWidget(alphaRotation_, 1);
        connect(alphaRotation_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            const std::string name = alphaRotation_->currentData().toString().toStdString();
            alphaAngle_->setEnabled(name != "random");
            if (updating_ || !session_) return;
            session_->settings().setAlphaRotation(name);
            edited();
        });
        refreshers_.push_back([this] {
            alphaRotation_->setCurrentIndex(std::max(0, alphaRotation_->findData(qs(session_->settings().alphaRotationName()))));
            alphaAngle_->setEnabled(session_->settings().alphaRotation != BrushToolSettings::AlphaRotation::Random);
        });
        alphaAngle_ = slider(options, tr("Stamp angle"), 0.0, 360.0, -360.0, 360.0, 0, [this] { return session_->settings().alphaAngle; },
                             [this](double v) { session_->settings().alphaAngle = static_cast<float>(v); },
                             tr("The alpha's angle in degrees, counterclockwise (Follow Stroke: added to the stroke's way)"));
        alphaAngle_->setObjectName(QStringLiteral("alphaAngle"));
        alphaAngle_->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral("°"); });
        alphaSpacing_ = slider(options, tr("Stamp spacing"), 5.0, 100.0, 100.0 * BrushToolSettings::kMinAlphaSpacing,
                               100.0 * BrushToolSettings::kMaxAlphaSpacing, 0, [this] { return 100.0 * session_->settings().alphaSpacing; },
                               [this](double v) { session_->settings().setAlphaSpacing(static_cast<float>(v / 100.0)); },
                               tr("How far apart the stamps fall along a stroke, in percent of the brush's diameter: low for a smooth "
                                  "trail, 100 and up for separate stamps"));
        alphaSpacing_->setObjectName(QStringLiteral("alphaSpacing"));
        alphaSpacing_->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral("%"); });
        FlowLayout* invertChips = nullptr;
        options->addWidget(chipRow(alphaOptions_, &invertChips));
        QToolButton* invert = chip(invertChips, tr("Invert"), [this] { return session_->settings().alphaInvert; },
                                   [this](bool on) { session_->settings().alphaInvert = on; }, tr("Paint where the alpha is black instead"));
        invert->setObjectName(QStringLiteral("alphaInvert"));
        column->addWidget(alphaOptions_);
    }
    paint_->rows()->addWidget(alphaBox_);
    // Shown for Paint on a scalp map (a guide map takes no alpha), its rows while one is picked.
    refreshers_.push_back([this] {
        const groomlab::BrushEntry* entry = tool_.empty() ? nullptr : brushEntry(tool_);
        alphaBox_->setVisible(entry && entry->hasSetting("alpha") && session_->settings().paintKind != "guides");
        alphaOptions_->setVisible(!alpha_->path().empty());
    });
    card_->addWidget(paint_);

    // ---- New guides (Place, Draw).
    newGuides_ = new CardBlock(tr("New guides"), this);
    newGuides_->setObjectName(QStringLiteral("newGuidesBox"));
    rows = newGuides_->rows();
    ValueSlider* length = slider(rows, tr("Length"), 0.01, 50.0, 0.001, 1e4, 3, [this] { return session_->settings().place.length; },
                                 [this](double v) { session_->settings().place.length = static_cast<float>(v); },
                                 tr("New guides' length (standing, or without neighbors to match)"));
    length->setObjectName(QStringLiteral("newGuideLength"));
    ValueSlider* segments = slider(rows, tr("Segments"), 1.0, 32.0, 1.0, 256.0, 0, [this] { return double(session_->settings().place.segments); },
                                   [this](double v) { session_->settings().setSegments(static_cast<int>(std::lround(v))); },
                                   tr("Points along each new guide"));
    segments->setObjectName(QStringLiteral("segments"));
    segments->setInteger(true);
    spacing_ = slider(rows, settingLabel("spacing"), 0.0, 10.0, 0.0, 1e4, 3, [this] { return session_->settings().place.spacing; },
                      [this](double v) { session_->settings().place.spacing = static_cast<float>(v); },
                      tr("Place many: no new root closer than this to another of the group"));
    spacing_->setObjectName(QStringLiteral("spacing"));
    {
        FlowLayout* chips = nullptr;
        rows->addWidget(chipRow(newGuides_, &chips));
        QToolButton* match = chip(chips, tr("Match nearby"), [this] { return session_->settings().place.matchNeighbors; },
                                  [this](bool on) { session_->settings().place.matchNeighbors = on; },
                                  tr("New guides take the shape, length and painted maps of the nearest guides: the group's own, or the "
                                     "groups around it while it has none. Off: they stand straight up, with no maps"));
        match->setObjectName(QStringLiteral("matchNearby"));
        QToolButton* solo = settingChip(chips, "soloNewGuides", tr("Solo new"), [this] { return session_->settings().place.soloNewGuides; },
                                        [this](bool on) { session_->settings().place.soloNewGuides = on; });
        solo->setObjectName(QStringLiteral("soloNewGuides"));
        placeCollide_ = chip(chips, tr("Off scalp"), [this] { return session_->settings().brush.collide; },
                             [this](bool on) { session_->settings().brush.collide = on; }, settingHint("collide"));
        placeCollide_->setObjectName(QStringLiteral("placeCollide"));
    }
    card_->addWidget(newGuides_);

    // ---- Comb: keep lengths, or stretch.
    comb_ = new CardBlock(tr("Comb"), this);
    comb_->setObjectName(QStringLiteral("combBlock"));
    {
        FlowLayout* chips = nullptr;
        comb_->rows()->addWidget(chipRow(comb_, &chips));
        QToolButton* stretch = settingChip(chips, "stretch", tr("Stretch while combing"), [this] { return session_->settings().brush.stretch; },
                                           [this](bool on) { session_->settings().brush.stretch = on; });
        stretch->setObjectName(QStringLiteral("stretch"));
    }
    card_->addWidget(comb_);

    // ---- Paints (Shape): what it paints on the tubes (choices.tubeChannel), one at a time.
    paints_ = new CardBlock(settingLabel("tubeChannel"), this);
    paints_->setObjectName(QStringLiteral("tubeChannelBox"));
    paints_->setToolTip(settingHint("tubeChannel"));
    {
        FlowLayout* chips = nullptr;
        paints_->rows()->addWidget(chipRow(paints_, &chips));
        auto* group = new QButtonGroup(paints_);
        group->setExclusive(true);
        for (const ui::json& item : defs().choices("tubeChannel")) {
            const std::string value = item.value("value", std::string());
            QToolButton* c = makeChip(qs(item.value("label", std::string())), qs(item.value("tip", std::string())), paints_);
            c->setProperty("channel", qs(value));
            c->setObjectName(QStringLiteral("tubeChannel_") + qs(value));
            group->addButton(c);
            tubeChannels_.push_back(c);
            chips->addWidget(c);
            connect(c, &QToolButton::clicked, this, [this, value] {
                if (updating_ || !session_) return;
                try {
                    session_->settings().brush.tubeChannel = tubeChannelFromString(value);
                } catch (const std::invalid_argument&) {
                    return;
                }
                edited();
            });
        }
        refreshers_.push_back([this] {
            const QString current = QString::fromLatin1(tubeChannelName(session_->settings().brush.tubeChannel));
            for (QToolButton* c : tubeChannels_) c->setChecked(c->property("channel").toString() == current);
        });
    }
    card_->addWidget(paints_);

    // ---- Erases: what Erase removes, and a short note (the whole of it in the tip).
    erases_ = new CardBlock(settingLabel("erases"), this);
    erases_->setObjectName(QStringLiteral("eraseBox"));
    erases_->setToolTip(tr("What the Erase brush removes where it passes (one undo step)"));
    {
        FlowLayout* chips = nullptr;
        erases_->rows()->addWidget(chipRow(erases_, &chips));
        for (const auto& [label, field] : std::vector<std::pair<QString, bool BrushToolSettings::*>>{
                 {tr("Guides"), &BrushToolSettings::eraseGuides}, {tr("Parts"), &BrushToolSettings::eraseParts}, {tr("Flow"), &BrushToolSettings::eraseFlow}}) {
            QToolButton* c = chip(chips, label, [this, field = field] { return session_->settings().*field; },
                                  [this, field = field](bool on) { session_->settings().*field = on; });
            c->setObjectName(QStringLiteral("erase") + label);
        }
        QLabel* note = hintLabel(tr("Erased guides take their children with them; the scattered hair stays."), erases_);
        note->setToolTip(text("eraseNote"));
        erases_->rows()->addWidget(note);
    }
    card_->addWidget(erases_);

    // ---- Keys (Length Keys): the selected key, Apply and Cancel.
    keys_ = new CardBlock(settingLabel("lengthKeys"), this);
    keys_->setObjectName(QStringLiteral("lengthKeysBox"));
    {
        lengthClear_ = makeGhost(tr("Clear"), tr("Delete every key"), keys_);
        lengthClear_->setObjectName(QStringLiteral("lengthClear"));
        keys_->addAction(lengthClear_);
        rows = keys_->rows();
        auto field = [this, rows](const char* name, const QString& caption, double max, double low, double high, const QString& tip) {
            auto* s = new ValueSlider(keys_);
            s->setObjectName(QString::fromLatin1(name));
            s->setCaption(caption);
            s->setRange(0.0, max);
            s->setLimits(low, high);
            s->setDecimals(3);
            s->setFill(theme::sliderFill(theme::blue()));
            s->setToolTip(tip);
            rows->addWidget(s);
            return s;
        };
        lengthScale_ = field("lengthScale", tr("Key scale"), 3.0, 0.001, 1000.0,
                             tr("The selected key's scale: guides at it take their length times this (1: kept as they are, a hold), "
                                "blending smoothly toward the other keys (right-drag the key up or down in the viewport to set it there)"));
        lengthKey_ = field("lengthKey", tr("Key length"), 50.0, 0.0001, 1e6,
                           tr("The length the selected key gives the guide nearest it (its scale set to match)"));
        FlowLayout* chips = nullptr;
        rows->addWidget(chipRow(keys_, &chips, tr("Guides")));
        auto* group = new QButtonGroup(keys_);
        group->setExclusive(true);
        for (const auto& [value, label, tip] : {std::tuple<const char*, QString, QString>{
                                                    "scale", tr("Scale"), tr("The whole guide scaled about its root: curls and bends keep their shape")},
                                                {"trim", tr("Trim"), tr("Cut at the tip, or extended along its last direction: curls keep their size")}}) {
            QToolButton* c = makeChip(label, tip, keys_);
            c->setObjectName(QStringLiteral("lengthChange_") + QLatin1String(value));
            c->setProperty("change", QString::fromLatin1(value));
            group->addButton(c);
            chips->addWidget(c);
            lengthChanges_.push_back(c);
            connect(c, &QToolButton::clicked, this, [this, value = std::string(value)] {
                const LengthChange change = lengthChangeFromName(value);
                lengthsEdit("change", [change](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.setChange(h, s, change); });
            });
        }
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 2, 0, 0);
        row->setSpacing(4);
        lengthDeleteKey_ = makeGhost(tr("Delete key"), tr("Delete the selected key (or Ctrl+click it in the viewport)"), keys_);
        lengthDeleteKey_->setObjectName(QStringLiteral("lengthDeleteKey"));
        lengthCancel_ = new QPushButton(tr("Cancel"), keys_);
        lengthCancel_->setObjectName(QStringLiteral("lengthCancel"));
        lengthCancel_->setToolTip(tr("Drop the changes since the last Apply: the guides and keys as they were (Esc)"));
        lengthApply_ = new QPushButton(tr("Apply"), keys_);
        lengthApply_->setObjectName(QStringLiteral("lengthApply"));
        lengthApply_->setProperty("role", "primary");
        lengthApply_->setToolTip(tr("Set the guides to the lengths shown (one undo step; Enter); the keys stay with the groom at x1 to place again"));
        row->addWidget(lengthDeleteKey_);
        row->addStretch(1);
        row->addWidget(lengthCancel_);
        row->addWidget(lengthApply_);
        rows->addLayout(row);
        lengthStatus_ = hintLabel(QString(), keys_);
        lengthStatus_->setObjectName(QStringLiteral("lengthStatus"));
        rows->addWidget(lengthStatus_);

        connect(lengthScale_, &ValueSlider::valueChanged, this, [this](double v) {
            lengthsEdit("scale", [v](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.setKeyScale(h, s, float(v)); });
        });
        connect(lengthKey_, &ValueSlider::valueChanged, this, [this](double v) {
            lengthsEdit("length", [v](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.setKeyLength(h, s, float(v)); });
        });
        connect(lengthDeleteKey_, &QPushButton::clicked, this, [this] {
            lengthsEdit("delete key", [](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.deleteKey(h, s); });
        });
        connect(lengthClear_, &QPushButton::clicked, this, [this] {
            lengthsEdit("clear", [](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.clear(h, s); });
        });
        connect(lengthCancel_, &QPushButton::clicked, this, [this] {
            lengthsEdit("cancel", [](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings&) { lk.cancel(h); });
        });
        connect(lengthApply_, &QPushButton::clicked, this, [this] {
            lengthsEdit("apply", [](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.apply(h, s); }, true);
        });
        refreshers_.push_back([this] {
            if (tool_ != "lengths") return;
            LengthKeyTool& lk = session_->lengths();
            std::size_t count = 0;
            float scale = 1.0f, length = 0.0f;
            const int key = lk.activeKey();
            session_->editLengths("keys", [&](LengthKeyTool& t, StrokeHost& h, const BrushToolSettings& s) {
                const LengthKeySet& set = t.keys(h);
                count = set.keys.size();
                if (key >= 0 && key < static_cast<int>(count)) {
                    scale = set.keys[key].scale;
                    length = t.keyLength(h, s, key);
                }
            });
            const bool hasKey = key >= 0 && key < static_cast<int>(count);
            if (hasKey) {
                lengthScale_->setValue(scale);
                lengthKey_->setValue(length);
            }
            const QString change = QString::fromLatin1(lengthChangeName(lk.change));
            for (QToolButton* c : lengthChanges_) c->setChecked(c->property("change").toString() == change);
            for (QWidget* w : {static_cast<QWidget*>(lengthScale_), static_cast<QWidget*>(lengthKey_), static_cast<QWidget*>(lengthDeleteKey_)}) {
                w->setEnabled(hasKey);
            }
            lengthClear_->setEnabled(count > 0);
            lengthApply_->setEnabled(lk.edited());
            lengthCancel_->setEnabled(lk.edited());
            lengthStatus_->setText(count == 0     ? tr("Click on the scalp to place a key.")
                                   : lk.edited() ? tr("%1 keys. Previewing: Apply sets the guides.").arg(count)
                                                 : tr("%1 keys. Right-drag a key to scale the guides round it.").arg(count));
        });
    }
    card_->addWidget(keys_);

    // ---- Flow: what a click or drag lays, the count, Clear all.
    flow_ = new CardBlock(settingLabel("flow"), this);
    flow_->setObjectName(QStringLiteral("flowBox"));
    {
        clearFlow_ = makeGhost(tr("Clear all"), qs(defs().get("actions/flow.deleteAll").value("tip", std::string())), flow_);
        clearFlow_->setObjectName(QStringLiteral("clearFlow"));
        clearFlow_->setEnabled(false);
        connect(clearFlow_, &QPushButton::clicked, this, &BrushPanel::clearFlowRequested);
        flow_->addAction(clearFlow_);
        const ui::json& choice = defs().get("choices/flowSink");
        QHBoxLayout* row = labeledRow(flow_->rows(), qs(choice.value("label", std::string("Place"))), flow_);
        flowSink_ = new QComboBox(flow_);
        flowSink_->setObjectName(QStringLiteral("flowSink"));
        flowSink_->setToolTip(qs(choice.value("tip", std::string())));
        for (const ui::json& item : defs().choices("flowSink")) {
            flowSink_->addItem(qs(item.value("label", std::string())), qs(item.value("value", std::string())));
            flowSink_->setItemData(flowSink_->count() - 1, qs(item.value("tip", std::string())), Qt::ToolTipRole);
        }
        row->addWidget(flowSink_, 1);
        // Where the arrows live (on the groom) and how many.
        flowCount_ = hintLabel(flowCountText(0, 0), flow_);
        flowCount_->setObjectName(QStringLiteral("flowCount"));
        flowCount_->setToolTip(tr("The flow arrows and sinks kept on the groom (the Flow node lays the strands along them)"));
        flow_->rows()->addWidget(flowCount_);
        flowHint_ = hintLabel(QString(), flow_);
        flow_->rows()->addWidget(flowHint_);
        auto showHint = [this] { flowHint_->setText(text(flowSink_->currentData().toString().isEmpty() ? "flowArrows" : "flowSinks")); };
        connect(flowSink_, &QComboBox::currentIndexChanged, this, [this, showHint](int) {
            showHint();
            if (updating_ || !session_) return;
            session_->settings().flowSink = flowSink_->currentData().toString().toStdString();
            edited();
        });
        refreshers_.push_back([this, showHint] {
            const int index = flowSink_->findData(qs(session_->settings().flowSink));
            flowSink_->setCurrentIndex(std::max(index, 0));
            showHint();
        });
        showHint();
    }
    card_->addWidget(flow_);

    // ---- Parts: the next part's name and the parts drawn so far.
    parts_ = new CardBlock(settingLabel("parts"), this);
    parts_->setObjectName(QStringLiteral("partsBox"));
    {
        QHBoxLayout* row = labeledRow(parts_->rows(), tr("Name"), parts_);
        partName_ = new QLineEdit(parts_);
        partName_->setObjectName(QStringLiteral("partName"));
        partName_->setToolTip(tr("Name for the next part (made unique)"));
        row->addWidget(partName_, 1);
        connect(partName_, &QLineEdit::editingFinished, this, [this] {
            if (updating_ || !session_) return;
            const std::string name = partName_->text().trimmed().toStdString();
            session_->settings().partName = name.empty() ? std::string("part") : name;
            edited();
        });
        refreshers_.push_back([this] { partName_->setText(qs(session_->settings().partName)); });
        partList_ = new QListWidget(parts_);
        partList_->setObjectName(QStringLiteral("partList"));
        partList_->setFixedHeight(76);
        partList_->setToolTip(tr("The parts the Part brush drew (a Part node parts the hair along them)"));
        parts_->rows()->addWidget(partList_);
        partButtons_ = new QWidget(parts_);
        auto* buttons = new QHBoxLayout(partButtons_);
        buttons->setContentsMargins(0, 0, 0, 0);
        buttons->setSpacing(4);
        parts_->rows()->addWidget(partButtons_);
    }
    card_->addWidget(parts_);

    // ---- The tubes' blocks (setTubes).
    tubeSlot_ = new QVBoxLayout();
    tubeSlot_->setContentsMargins(0, 0, 0, 0);
    tubeSlot_->setSpacing(0);
    card_->addLayout(tubeSlot_);

    // ---- Applies to: Acts on, Mirror.
    applies_ = new CardBlock(text("appliesBlock"), this);
    applies_->setObjectName(QStringLiteral("appliesBlock"));
    {
        FlowLayout* chips = nullptr;
        actsOn_ = chipRow(applies_, &chips, settingLabel("targets"));
        actsOn_->setObjectName(QStringLiteral("targetsBox"));
        actsOn_->setToolTip(tr("What the brush shapes where it passes: guides, part lines, flow arrows and sinks (Comb slides and lays "
                               "arrows along the stroke; Twist turns arrows and whorls; Length sizes whorls; Smooth, Straighten and "
                               "Attract shape both; Cut splits parts)"));
        for (const auto& [label, field] : std::vector<std::pair<QString, bool BrushToolSettings::*>>{
                 {tr("Guides"), &BrushToolSettings::brushGuides}, {tr("Parts"), &BrushToolSettings::brushParts}, {tr("Flow"), &BrushToolSettings::brushFlow}}) {
            QToolButton* c = chip(chips, label, [this, field = field] { return session_->settings().*field; },
                                  [this, field = field](bool on) { session_->settings().*field = on; });
            c->setObjectName(QStringLiteral("target") + label);
        }
        applies_->rows()->addWidget(actsOn_);
        // Mirror: Symmetry on, and across what (World X and Topology as every host offers them;
        // World Y and Z the app's own, with the world plane's center).
        mirrorRow_ = new QWidget(applies_);
        auto* mirror = new QVBoxLayout(mirrorRow_);
        mirror->setContentsMargins(0, 0, 0, 0);
        mirror->setSpacing(4);
        const groomlab::BrushSettingSpec* spec = brushSettingSpec("symmetry");
        const QString tip = withShortcut(spec ? qs(spec->hint) : QString(), "brush.symmetry");
        QHBoxLayout* row = labeledRow(mirror, tr("Mirror"), mirrorRow_);
        symmetry_ = new QCheckBox(mirrorRow_);
        symmetry_->setObjectName(QStringLiteral("symmetryOn"));
        symmetry_->setToolTip(tip + tr(". While it is on, a red line on the scalp shows the mirror plane while a brush is on."));
        symmetryMode_ = new QComboBox(mirrorRow_);
        symmetryMode_->setObjectName(QStringLiteral("symmetry"));
        symmetryMode_->setToolTip(tip);
        for (const ui::json& item : defs().choices("symmetry")) {
            symmetryMode_->addItem(qs(item.value("label", std::string())), qs(item.value("value", std::string())));
            symmetryMode_->setItemData(symmetryMode_->count() - 1, qs(item.value("tip", std::string())), Qt::ToolTipRole);
            if (item.value("value", std::string()) == "x") {
                symmetryMode_->addItem(tr("World Y"), QStringLiteral("y"));
                symmetryMode_->setItemData(symmetryMode_->count() - 1, tr("Across the plane y = center"), Qt::ToolTipRole);
                symmetryMode_->addItem(tr("World Z"), QStringLiteral("z"));
                symmetryMode_->setItemData(symmetryMode_->count() - 1, tr("Across the plane z = center"), Qt::ToolTipRole);
            }
        }
        symmetryMode_->setEnabled(false);
        connect(symmetry_, &QCheckBox::toggled, this, [this](bool) { symmetryEdited(); });
        connect(symmetryMode_, &QComboBox::currentIndexChanged, this, [this](int) { symmetryEdited(); });
        row->addWidget(symmetry_);
        row->addWidget(symmetryMode_, 1);
        symmetryCenter_ = new ValueSlider(mirrorRow_);
        symmetryCenter_->setObjectName(QStringLiteral("symmetryCenter"));
        symmetryCenter_->setCaption(tr("Center"));
        symmetryCenter_->setRange(-10.0, 10.0);
        symmetryCenter_->setLimits(-1e6, 1e6);
        symmetryCenter_->setDecimals(3);
        symmetryCenter_->setFill(theme::sliderFill(theme::blue()));
        symmetryCenter_->setToolTip(tr("Where the world plane sits along its axis (0: the origin)"));
        connect(symmetryCenter_, &ValueSlider::valueChanged, this, [this](double v) {
            if (updating_ || !session_) return;
            session_->settings().brush.mirrorCenter = static_cast<float>(v);
            edited();
        });
        mirror->addWidget(symmetryCenter_);
        applies_->rows()->addWidget(mirrorRow_);
    }
    card_->addWidget(applies_);

    setTool({});
    setEnabled(false);
}

ValueSlider* BrushPanel::slider(QVBoxLayout* into, const QString& label, double min, double max, double limitMin, double limitMax, int decimals,
                                std::function<double()> get, std::function<void(double)> set, const QString& tip) {
    auto* editor = new ValueSlider(this);
    editor->setRange(min, max);
    editor->setLimits(limitMin, limitMax);
    editor->setDecimals(decimals);
    editor->setFill(theme::sliderFill(theme::blue()));
    editor->setCaption(label);
    if (!tip.isEmpty()) editor->setToolTip(tip);
    into->addWidget(editor);
    connect(editor, &ValueSlider::valueChanged, this, [this, set](double v) {
        if (updating_ || !session_) return;
        set(v);
        edited();
    });
    refreshers_.push_back([editor, get] { editor->setValue(get()); });
    return editor;
}

QToolButton* BrushPanel::chip(FlowLayout* into, const QString& label, std::function<bool()> get, std::function<void(bool)> set, const QString& tip) {
    QToolButton* c = makeChip(label, tip, this);
    into->addWidget(c);
    connect(c, &QToolButton::toggled, this, [this, set](bool on) {
        if (updating_ || !session_) return;
        set(on);
        edited();
    });
    refreshers_.push_back([c, get] { c->setChecked(get()); });
    return c;
}

QToolButton* BrushPanel::settingChip(FlowLayout* into, const char* id, const QString& label, std::function<bool()> get,
                                     std::function<void(bool)> set) {
    QToolButton* c = chip(into, label, std::move(get), std::move(set), settingHint(id));
    c->setObjectName(QString::fromLatin1(id));
    return c;
}

void BrushPanel::curve(FlowLayout* chips, QVBoxLayout* editors, const std::string& setting, const QString& label, const std::string& start,
                       std::function<std::string()> get, std::function<void(const std::string&)> set) {
    const QString hint = settingHint(setting.c_str());
    QToolButton* on = makeChip(label, hint, this);
    on->setObjectName(qs(setting));
    chips->addWidget(on);
    auto* holder = new QWidget(this);
    holder->setObjectName(qs(setting) + QStringLiteral("Curve"));
    auto* column = new QVBoxLayout(holder);
    column->setContentsMargins(0, 2, 0, 0);
    column->setSpacing(2);
    auto* name = new QLabel(setting == "falloffProfile" ? tr("Falloff: center to edge") : tr("Strength: root to tip"), holder);
    name->setProperty("role", "chipLabel");
    column->addWidget(name);
    auto* editor = new RampEditor(holder);
    editor->setValue(nlohmann::json::parse(start));
    editor->setToolTip(hint + tr(" (right-click: presets)"));
    column->addWidget(editor);
    holder->setVisible(false);
    editors->addWidget(holder);
    auto push = [this, on, editor, set] {
        if (updating_ || !session_) return;
        try {
            set(on->isChecked() ? editor->value().dump() : std::string());
        } catch (const std::invalid_argument&) {
            return;
        }
        edited();
    };
    connect(on, &QToolButton::toggled, this, [holder, push](bool checked) {
        holder->setVisible(checked);
        push();
    });
    connect(editor, &RampEditor::edited, this, [push](bool live) {
        if (!live) push();
    });
    refreshers_.push_back([on, holder, editor, get] {
        const std::string json = get();
        on->setChecked(!json.empty());
        holder->setVisible(!json.empty());
        if (!json.empty()) editor->setValue(nlohmann::json::parse(json, nullptr, false));
    });
}

void BrushPanel::edited() { emit settingsChanged(); }

void BrushPanel::setSession(BrushSession* session) {
    session_ = session;
    setEnabled(session_ != nullptr);
    refresh();
}

void BrushPanel::setTubes(TubePanel* tubes) {
    tubes_ = tubes;
    tubeSlot_->addWidget(tubes);
    setTool(tool_);
}

void BrushPanel::setPaintControls(QWidget* controls) {
    paintSlot_->addWidget(controls);
    controls->show();
}

void BrushPanel::refreshTitle() {
    const groomlab::BrushEntry* e = tool_.empty() ? nullptr : brushEntry(tool_);
    if (!e) return;
    title_->setText(qs(e->label));
    const ui::json* line = defs().find("palette/tools/" + tool_ + "/short");
    short_->setText(line && line->is_string() ? qs(line->get<std::string>()) : qs(e->hint));
    help_->setToolTip(helpTip(*e, e->hasSetting("radius")));
    const QString resource = QStringLiteral(":/groomlab/icons/groomLab_%1.svg").arg(qs(e->icon));
    icon_->setPixmap(duotoneIcon(resource, theme::toolIconColor(e->category, e->icon), theme::iconInk(), 16, devicePixelRatioF()));
}

void BrushPanel::setTool(const std::string& id) {
    tool_ = id;
    const groomlab::BrushEntry* entry = id.empty() ? nullptr : brushEntry(id);
    auto uses = [entry](const char* setting) { return entry && entry->hasSetting(setting); };
    hasSettings_ = entry != nullptr;
    refreshTitle();
    // Brush.
    const bool brush = uses("radius");
    for (ValueSlider* s : {radius_, strength_, falloff_}) s->setVisible(brush);
    smooth_->setVisible(uses("smoothStrength"));
    pressure_->setVisible(uses("pressure"));
    collide_->setVisible(brush && uses("collide"));
    falloffCurve_->setVisible(uses("falloffProfile"));
    strengthCurve_->setVisible(uses("strengthProfile"));
    if (!uses("falloffProfile")) falloffRamp_->hide();
    if (!uses("strengthProfile")) strengthRamp_->hide();
    propagate_->setVisible(uses("strengthProfile"));  // the shaping brushes (the app's own)
    pointwise_->setVisible(uses("map") && id == "paint");
    brush_->setVisible(brush || uses("smoothStrength"));
    // The strength's span: -1..1 for the tools that work both ways, else 0..1 (Maya's).
    if (entry) {
        strength_->setRange(entry->signedStrength ? -1.0 : 0.0, 1.0);
        strength_->setCaption(entry->signedStrength ? tr("Strength (±)") : settingLabel("strength"));
    }
    // The tool's own block.
    paint_->setVisible(uses("map"));
    newGuides_->setVisible(uses("newGuides"));
    spacing_->setVisible(uses("spacing"));
    placeCollide_->setVisible(uses("newGuides") && uses("collide"));
    comb_->setVisible(uses("stretch"));
    paints_->setVisible(uses("tubeChannel"));
    erases_->setVisible(uses("erases"));
    keys_->setVisible(uses("lengthKeys"));
    flow_->setVisible(uses("flow"));
    parts_->setVisible(uses("parts"));
    // The tubes: the tube new guides get (Place, Draw), the selected guides' (Shape, Select).
    if (tubes_) {
        const bool edits = id == "shape" || id == "select";
        tubes_->setVisible(uses("newGuides") || edits);
        tubes_->setMode(edits ? TubePanel::Mode::Edit : TubePanel::Mode::New);
    }
    // Applies to.
    actsOn_->setVisible(uses("targets"));
    mirrorRow_->setVisible(uses("symmetry"));
    applies_->setVisible(uses("targets") || uses("symmetry"));
    refresh();
}

std::vector<std::string> BrushPanel::blockTitles() const {
    std::vector<std::string> out;
    if (!isHidden()) collectBlocks(layout(), this, out);
    return out;
}

void BrushPanel::lengthsEdit(const char* what, const std::function<void(LengthKeyTool&, StrokeHost&, const BrushToolSettings&)>& fn, bool applies) {
    if (updating_ || !session_) return;
    session_->editLengths(what, fn);
    refresh();
    if (!session_->lastError().empty()) lengthStatus_->setText(qs(session_->lastError()));
    if (applies) emit guidesEdited();
    else emit lengthsPreviewed();
}

void BrushPanel::refresh() {
    if (!session_) return;
    updating_ = true;
    for (const auto& r : refreshers_) r();
    refreshSymmetry();
    updating_ = false;
}

void BrushPanel::refreshSymmetry() {
    const std::string name = session_->settings().symmetryName();
    const bool on = name != "none";
    const QSignalBlocker a(symmetry_), b(symmetryMode_);
    symmetry_->setChecked(on);
    if (on) {
        const int index = symmetryMode_->findData(qs(name));
        if (index >= 0) symmetryMode_->setCurrentIndex(index);
    }
    symmetryMode_->setEnabled(on);
    symmetryCenter_->setVisible(on && name != "topology");
    symmetryCenter_->setValue(session_->settings().brush.mirrorCenter);
}

void BrushPanel::symmetryEdited() {
    symmetryMode_->setEnabled(symmetry_->isChecked());
    if (updating_ || !session_) return;
    const std::string name = symmetry_->isChecked() ? symmetryMode_->currentData().toString().toStdString() : std::string("none");
    session_->settings().setSymmetry(name);
    updating_ = true;
    refreshSymmetry();
    updating_ = false;
    edited();
}

void BrushPanel::setParts(const std::vector<std::string>& names) {
    const QString current = partList_->currentItem() ? partList_->currentItem()->text() : QString();
    const QSignalBlocker blocker(partList_);
    partList_->clear();
    for (const std::string& name : names) partList_->addItem(qs(name));
    const auto found = partList_->findItems(current, Qt::MatchExactly);
    if (!found.isEmpty()) partList_->setCurrentItem(found.front());
    else if (partList_->count() > 0) partList_->setCurrentRow(partList_->count() - 1);
    for (QPushButton* b : partButtons_->findChildren<QPushButton*>()) b->setEnabled(partList_->count() > 0);
}

QString BrushPanel::flowCountText(std::size_t arrows, std::size_t sinks) {
    if (arrows == 0 && sinks == 0) return tr("No flow arrows yet");
    return tr("%1 arrow%2 · %3 sink%4")
        .arg(arrows)
        .arg(arrows == 1 ? QString() : QStringLiteral("s"))
        .arg(sinks)
        .arg(sinks == 1 ? QString() : QStringLiteral("s"));
}

void BrushPanel::setFlowCount(std::size_t arrows, std::size_t sinks) {
    flowCount_->setText(flowCountText(arrows, sinks));
    clearFlow_->setEnabled(arrows + sinks > 0);
}

std::string BrushPanel::selectedPart() const { return partList_->currentItem() ? partList_->currentItem()->text().toStdString() : std::string(); }

void BrushPanel::setBindings(const ui::Bindings& bindings) {
    for (QPushButton* b : partButtons_->findChildren<QPushButton*>()) delete b;
    ui::ResolveOptions options;
    options.state.reset();  // an action bar: every entry, enabled by its binding
    options.keep = [&bindings](const std::string& id) { return bindings.count(id) > 0; };
    for (const ui::Item& item : defs().resolve(defs().get("lists/parts/bar"), options)) {
        if (item.kind != ui::Item::Kind::Action) continue;
        auto* b = new QPushButton(qs(item.label), partButtons_);
        b->setObjectName(QStringLiteral("parts_") + qs(item.id).section(QLatin1Char('.'), 1));
        b->setToolTip(qs(item.tip));
        b->setEnabled(partList_->count() > 0);
        const ui::Binding binding = bindings.at(item.id);
        connect(b, &QPushButton::clicked, this, [binding] { binding.trigger(nullptr, false); });
        partButtons_->layout()->addWidget(b);
    }
}

}  // namespace groomlab::app
