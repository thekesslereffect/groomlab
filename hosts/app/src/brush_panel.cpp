// SPDX-License-Identifier: Apache-2.0
#include "brush_panel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

#include <QButtonGroup>
#include <QCheckBox>
#include <QToolButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "groomlab/brush_alpha.h"
#include "groomlab/brush_catalog.h"

#include "alpha_picker.h"
#include "brush_session.h"
#include "glass.h"
#include "graph_presets.h"
#include "groom_document.h"
#include "param_editors.h"
#include "theme.h"

namespace groomlab::app {

namespace {

QString settingLabel(const char* id) {
    const groomlab::BrushSettingSpec* spec = brushSettingSpec(id);
    return spec ? QString::fromStdString(spec->label) : QString::fromLatin1(id);
}

QString settingHint(const char* id) {
    const groomlab::BrushSettingSpec* spec = brushSettingSpec(id);
    return spec ? QString::fromStdString(spec->hint) : QString();
}

// A text of the Groom Tools (palette.json's texts): the app's own words when "labels" has them.
QString text(const char* name) {
    const ui::Definitions& d = ui::Definitions::shipped();
    const ui::json* t = d.find(std::string("palette/texts/labels/") + ui::kHost + "/" + name);
    if (!t) t = d.find(std::string("palette/texts/") + name);
    return t && t->is_string() ? QString::fromStdString(t->get<std::string>()) : QString();
}

QFormLayout* makeForm() {
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return form;
}

QLabel* hintLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setProperty("role", "hint");
    return label;
}

}  // namespace

BrushPanel::BrushPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("toolCard"));
    setProperty("role", "card");
    auto* card = new QVBoxLayout(this);
    card->setContentsMargins(10, 10, 10, 10);
    card->setSpacing(8);

    // Radius, Strength, Falloff, Smooth: the brushes' numbers.
    brushForm_ = new QWidget(this);
    auto* form = makeForm();
    brushForm_->setLayout(form);
    radius_ = slider(form, settingLabel("radius"), 5.0, 400.0, 1.0, 2000.0, 0, [this] { return session_->settings().screenRadius; },
                     [this](double v) { session_->settings().setScreenRadius(static_cast<float>(v)); },
                     withShortcut(tr("Screen pixels: the circle's radius on screen, whatever the zoom (middle-drag sets it too)"),
                                  "brush.radius"));
    radius_->setObjectName(QStringLiteral("radius"));
    radius_->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral(" px"); });
    strength_ = slider(form, settingLabel("strength"), 0.0, 1.0, -10.0, 10.0, 2, [this] { return session_->settings().brush.strength; },
                       [this](double v) { session_->settings().brush.strength = static_cast<float>(v); },
                       withShortcut(settingHint("strength") + tr(" (Shift+middle-drag sets it too)"), "brush.strength"));
    strength_->setObjectName(QStringLiteral("strength"));
    // Falloff as the catalog names it in both hosts: 0 hard to the edge, 1 soft from the center
    // (the core's hardness is 1 - falloff).
    falloff_ = slider(form, settingLabel("falloff"), 0.0, 1.0, 0.0, 1.0, 2, [this] { return 1.0 - session_->settings().brush.hardness; },
                      [this](double v) { session_->settings().setHardness(static_cast<float>(1.0 - v)); }, settingHint("falloff"));
    falloff_->setObjectName(QStringLiteral("falloff"));
    smooth_ = slider(form, settingLabel("smoothStrength"), 0.0, 3.0, 0.0, 100.0, 2, [this] { return session_->settings().smoothStrength; },
                     [this](double v) { session_->settings().setSmoothStrength(static_cast<float>(v)); }, settingHint("smoothStrength"));
    smooth_->setObjectName(QStringLiteral("smoothStrength"));
    card->addWidget(brushForm_);

    // The curves: the falloff drawn instead of the slider's, and the strength root to tip.
    falloffCurve_ = curve("falloffProfile", "[[0, 1], [0.5, 0.56], [1, 0]]", [this] { return session_->settings().falloffProfileJson(); },
                          [this](const std::string& v) { session_->settings().setFalloffProfile(v); });
    strengthCurve_ = curve("strengthProfile", "[[0, 0.25], [1, 1]]", [this] { return session_->settings().strengthProfileJson(); },
                           [this](const std::string& v) { session_->settings().setStrengthProfile(v); });
    card->addWidget(falloffCurve_);
    card->addWidget(strengthCurve_);

    // Paint's alpha on a scalp map: the picker, and while one is picked its rotation, angle,
    // spacing and invert (Maya's Alpha rows).
    if (shippedBrushAlphaDir().empty()) setShippedBrushAlphaDir(shippedAlphaFolder());
    QVBoxLayout* alphaColumn = nullptr;
    alphaBox_ = box({}, &alphaColumn);
    alphaBox_->setObjectName(QStringLiteral("alphaBox"));
    auto* alphaRow = new QHBoxLayout();
    alphaRow->setSpacing(10);
    auto* alphaLabel = new QLabel(settingLabel("alpha"), alphaBox_);
    alphaLabel->setToolTip(settingHint("alpha"));
    alpha_ = new AlphaPicker(alphaBox_);
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
    alphaRow->addWidget(alphaLabel);
    alphaRow->addWidget(alpha_, 1);
    alphaColumn->addLayout(alphaRow);
    alphaOptions_ = new QWidget(alphaBox_);
    auto* alphaForm = makeForm();
    alphaOptions_->setLayout(alphaForm);
    alphaRotation_ = new QComboBox(alphaOptions_);
    alphaRotation_->setObjectName(QStringLiteral("alphaRotation"));
    for (const auto& [name, label, tip] : {std::tuple<const char*, QString, QString>{"fixed", tr("Fixed"), tr("Every stamp at the angle")},
                                           {"stroke", tr("Follow Stroke"), tr("Each stamp turned the way the stroke goes, plus the angle")},
                                           {"random", tr("Random"), tr("Each stamp at a random angle")}}) {
        alphaRotation_->addItem(label, QString::fromLatin1(name));
        alphaRotation_->setItemData(alphaRotation_->count() - 1, tip, Qt::ToolTipRole);
    }
    alphaForm->addRow(tr("Rotation"), alphaRotation_);
    connect(alphaRotation_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const std::string name = alphaRotation_->currentData().toString().toStdString();
        alphaAngle_->setEnabled(name != "random");
        if (updating_ || !session_) return;
        session_->settings().setAlphaRotation(name);
        edited();
    });
    refreshers_.push_back([this] {
        alphaRotation_->setCurrentIndex(std::max(0, alphaRotation_->findData(QString::fromStdString(session_->settings().alphaRotationName()))));
        alphaAngle_->setEnabled(session_->settings().alphaRotation != BrushToolSettings::AlphaRotation::Random);
    });
    alphaAngle_ = slider(alphaForm, tr("Angle"), 0.0, 360.0, -360.0, 360.0, 0, [this] { return session_->settings().alphaAngle; },
                         [this](double v) { session_->settings().alphaAngle = static_cast<float>(v); },
                         tr("The alpha's angle in degrees, counterclockwise (Follow Stroke: added to the stroke's way)"));
    alphaAngle_->setObjectName(QStringLiteral("alphaAngle"));
    alphaSpacing_ = slider(alphaForm, tr("Spacing"), 5.0, 100.0, 100.0 * BrushToolSettings::kMinAlphaSpacing,
                           100.0 * BrushToolSettings::kMaxAlphaSpacing, 0, [this] { return 100.0 * session_->settings().alphaSpacing; },
                           [this](double v) { session_->settings().setAlphaSpacing(static_cast<float>(v / 100.0)); },
                           tr("How far apart the stamps fall along a stroke, in percent of the brush's diameter: low for a smooth "
                              "trail, 100 and up for separate stamps"));
    alphaSpacing_->setObjectName(QStringLiteral("alphaSpacing"));
    alphaSpacing_->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral(" %"); });
    alphaInvert_ = check(tr("Invert"), [this] { return session_->settings().alphaInvert; },
                         [this](bool on) { session_->settings().alphaInvert = on; }, tr("Paint where the alpha is black instead"));
    alphaInvert_->setObjectName(QStringLiteral("alphaInvert"));
    alphaForm->addRow(QString(), alphaInvert_);
    alphaColumn->addWidget(alphaOptions_);
    card->addWidget(alphaBox_);
    // Shown for Paint on a scalp map (a guide map takes no alpha), its rows while one is picked.
    refreshers_.push_back([this] {
        const groomlab::BrushEntry* entry = tool_.empty() ? nullptr : brushEntry(tool_);
        alphaBox_->setVisible(entry && entry->hasSetting("alpha") && session_->settings().paintKind != "guides");
        alphaOptions_->setVisible(!alpha_->path().empty());
    });

    // The tool's check boxes, two to a row (packed again for each tool: no holes).
    selectedOnly_ = setting("selectedOnly", [this] { return session_->settings().selectedOnly; },
                            [this](bool on) { session_->settings().selectedOnly = on; });
    pressure_ = setting("pressure", [this] { return session_->settings().pressure; }, [this](bool on) { session_->settings().pressure = on; });
    collide_ = setting("collide", [this] { return session_->settings().brush.collide; },
                       [this](bool on) { session_->settings().brush.collide = on; });
    frontFacing_ = setting("frontFacingOnly", [this] { return session_->settings().maskByScalp; },
                           [this](bool on) { session_->settings().maskByScalp = on; });
    propagate_ = check(tr("Propagate"), [this] { return session_->settings().brush.propagate; },
                       [this](bool on) { session_->settings().brush.propagate = on; },
                       tr("Also edit the locked descendants (children that keep their own shape)"));
    propagate_->setObjectName(QStringLiteral("propagate"));
    pointwise_ = check(tr("Pointwise"), [this] { return session_->settings().brush.pointwise; },
                       [this](bool on) { session_->settings().brush.pointwise = on; },
                       tr("Paint a per-point layer along the guide instead of one value per guide"));
    pointwise_->setObjectName(QStringLiteral("pointwise"));
    checkOrder_ = {selectedOnly_, pressure_, collide_, frontFacing_, propagate_, pointwise_};
    checks_ = new QGridLayout();
    checks_->setContentsMargins(0, 0, 0, 0);
    checks_->setHorizontalSpacing(10);
    checks_->setVerticalSpacing(6);
    for (QCheckBox* c : checkOrder_) c->setParent(this);
    card->addLayout(checks_);

    // Place and Draw: the new guides.
    QVBoxLayout* column = nullptr;
    createBox_ = box(settingLabel("newGuides"), &column);
    createBox_->setObjectName(QStringLiteral("newGuidesBox"));
    auto* createForm = makeForm();
    column->addLayout(createForm);
    QCheckBox* match = check(tr("Match nearby guides"), [this] { return session_->settings().place.matchNeighbors; },
                             [this](bool on) { session_->settings().place.matchNeighbors = on; },
                             tr("New guides take the shape, length and painted maps of the nearest guides: the group's own, or the "
                                "groups around it while it has none. Off: they stand straight up, with no maps"));
    match->setParent(createBox_);
    createForm->addRow(match);
    slider(createForm, tr("Length"), 0.01, 50.0, 0.001, 1e4, 3, [this] { return session_->settings().place.length; },
           [this](double v) { session_->settings().place.length = static_cast<float>(v); },
           tr("New guides' length (standing, or without neighbors to match)"));
    auto* segments = new QSpinBox(createBox_);
    segments->setObjectName(QStringLiteral("segments"));
    segments->setRange(1, 256);
    segments->setToolTip(tr("Points along each new guide"));
    createForm->addRow(tr("Segments"), segments);
    connect(segments, &QSpinBox::valueChanged, this, [this](int v) {
        if (updating_ || !session_) return;
        session_->settings().setSegments(v);
        edited();
    });
    refreshers_.push_back([this, segments] { segments->setValue(session_->settings().place.segments); });
    spacing_ = slider(createForm, settingLabel("spacing"), 0.0, 10.0, 0.0, 1e4, 3, [this] { return session_->settings().place.spacing; },
                      [this](double v) { session_->settings().place.spacing = static_cast<float>(v); },
                      tr("Place many: no new root closer than this to another of the group"));
    QCheckBox* solo = setting("soloNewGuides", [this] { return session_->settings().place.soloNewGuides; },
                              [this](bool on) { session_->settings().place.soloNewGuides = on; });
    solo->setObjectName(QStringLiteral("soloNewGuides"));
    solo->setParent(createBox_);
    createForm->addRow(solo);
    card->addWidget(createBox_);

    // Part: the next part's name and the parts drawn so far.
    partBox_ = box(settingLabel("parts"), &column);
    partBox_->setObjectName(QStringLiteral("partsBox"));
    auto* partForm = makeForm();
    column->addLayout(partForm);
    partName_ = new QLineEdit(partBox_);
    partName_->setObjectName(QStringLiteral("partName"));
    partName_->setToolTip(tr("Name for the next part (made unique)"));
    partForm->addRow(tr("Name"), partName_);
    connect(partName_, &QLineEdit::editingFinished, this, [this] {
        if (updating_ || !session_) return;
        const std::string name = partName_->text().trimmed().toStdString();
        session_->settings().partName = name.empty() ? std::string("part") : name;
        edited();
    });
    refreshers_.push_back([this] { partName_->setText(QString::fromStdString(session_->settings().partName)); });
    parts_ = new QListWidget(partBox_);
    parts_->setObjectName(QStringLiteral("partList"));
    parts_->setFixedHeight(76);
    parts_->setToolTip(tr("The parts the Part brush drew (a Part node parts the hair along them)"));
    column->addWidget(parts_);
    partButtons_ = new QWidget(partBox_);
    auto* partRow = new QHBoxLayout(partButtons_);
    partRow->setContentsMargins(0, 0, 0, 0);
    partRow->setSpacing(6);
    column->addWidget(partButtons_);
    card->addWidget(partBox_);

    // Flow: what a click or drag lays, Clear All, and how.
    flowBox_ = box(settingLabel("flow"), &column);
    flowBox_->setObjectName(QStringLiteral("flowBox"));
    {
        const ui::Definitions& d = ui::Definitions::shipped();
        const ui::json& choice = d.get("choices/flowSink");
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        auto* label = new QLabel(QString::fromStdString(choice.value("label", std::string("Place"))), flowBox_);
        flowSink_ = new QComboBox(flowBox_);
        flowSink_->setObjectName(QStringLiteral("flowSink"));
        flowSink_->setToolTip(QString::fromStdString(choice.value("tip", std::string())));
        for (const ui::json& item : d.choices("flowSink")) {
            flowSink_->addItem(QString::fromStdString(item.value("label", std::string())), QString::fromStdString(item.value("value", std::string())));
            flowSink_->setItemData(flowSink_->count() - 1, QString::fromStdString(item.value("tip", std::string())), Qt::ToolTipRole);
        }
        row->addWidget(label);
        row->addWidget(flowSink_, 1);
        column->addLayout(row);
        // Where the arrows live (on the groom) and how many, and Clear All (GroomLab > Utilities >
        // Delete All Flow Arrows).
        auto* counted = new QHBoxLayout();
        counted->setSpacing(6);
        flowCount_ = hintLabel(flowCountText(0, 0), flowBox_);
        flowCount_->setObjectName(QStringLiteral("flowCount"));
        flowCount_->setToolTip(tr("The flow arrows and sinks kept on the groom (the Flow node lays the strands along them)"));
        clearFlow_ = new QPushButton(tr("Clear All"), flowBox_);
        clearFlow_->setObjectName(QStringLiteral("clearFlow"));
        clearFlow_->setToolTip(QString::fromStdString(d.get("actions/flow.deleteAll").value("tip", std::string())));
        clearFlow_->setEnabled(false);
        connect(clearFlow_, &QPushButton::clicked, this, &BrushPanel::clearFlowRequested);
        counted->addWidget(flowCount_, 1);
        counted->addWidget(clearFlow_);
        column->addLayout(counted);
        flowHint_ = hintLabel(QString(), flowBox_);
        column->addWidget(flowHint_);
        auto showHint = [this] { flowHint_->setText(text(flowSink_->currentData().toString().isEmpty() ? "flowArrows" : "flowSinks")); };
        connect(flowSink_, &QComboBox::currentIndexChanged, this, [this, showHint](int) {
            showHint();
            if (updating_ || !session_) return;
            session_->settings().flowSink = flowSink_->currentData().toString().toStdString();
            edited();
        });
        refreshers_.push_back([this, showHint] {
            const int index = flowSink_->findData(QString::fromStdString(session_->settings().flowSink));
            flowSink_->setCurrentIndex(std::max(index, 0));
            showHint();
        });
        showHint();
    }
    card->addWidget(flowBox_);

    // Length Keys: the selected key, Apply and Cancel.
    lengthsBox_ = box(settingLabel("lengthKeys"), &column);
    lengthsBox_->setObjectName(QStringLiteral("lengthKeysBox"));
    {
        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight);
        auto number = [this](const char* name, double low, double high, const QString& tip) {
            auto* spin = new QDoubleSpinBox(lengthsBox_);
            spin->setObjectName(QString::fromLatin1(name));
            spin->setRange(low, high);
            spin->setDecimals(3);
            spin->setKeyboardTracking(false);
            spin->setToolTip(tip);
            return spin;
        };
        lengthScale_ = number("lengthScale", 0.001, 1000.0,
                              tr("The selected key's scale: guides at it take their length times this (1: kept as they "
                                 "are, a hold), blending smoothly toward the other keys (right-drag the key up or down in "
                                 "the viewport to set it there)"));
        lengthScale_->setSingleStep(0.1);
        lengthDeleteKey_ = new QPushButton(tr("Delete Key"), lengthsBox_);
        lengthDeleteKey_->setObjectName(QStringLiteral("lengthDeleteKey"));
        lengthDeleteKey_->setToolTip(tr("Delete the selected key (or Ctrl+click it in the viewport)"));
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        row->addWidget(lengthScale_, 1);
        row->addWidget(lengthDeleteKey_);
        form->addRow(tr("Key scale"), row);
        lengthKey_ = number("lengthKey", 0.0001, 1e6, tr("The length the selected key gives the guide nearest it (its scale set to match)"));
        form->addRow(tr("Key length"), lengthKey_);
        row = new QHBoxLayout();
        row->setSpacing(6);
        lengthChange_ = new QComboBox(lengthsBox_);
        lengthChange_->setObjectName(QStringLiteral("lengthChange"));
        lengthChange_->addItem(tr("Scale"), QStringLiteral("scale"));
        lengthChange_->addItem(tr("Trim"), QStringLiteral("trim"));
        lengthChange_->setToolTip(tr("How a guide takes its new length: Scale (the whole guide scaled about its root: curls "
                                     "and bends keep their shape) or Trim (cut at the tip, or extended along its last "
                                     "direction: curls keep their size)"));
        lengthClear_ = new QPushButton(tr("Clear"), lengthsBox_);
        lengthClear_->setObjectName(QStringLiteral("lengthClear"));
        lengthClear_->setToolTip(tr("Delete every key"));
        row->addWidget(lengthChange_, 1);
        row->addWidget(lengthClear_);
        form->addRow(tr("Guides"), row);
        column->addLayout(form);
        row = new QHBoxLayout();
        row->setSpacing(6);
        lengthStatus_ = hintLabel(QString(), lengthsBox_);
        lengthStatus_->setObjectName(QStringLiteral("lengthStatus"));
        lengthCancel_ = new QPushButton(tr("Cancel"), lengthsBox_);
        lengthCancel_->setObjectName(QStringLiteral("lengthCancel"));
        lengthCancel_->setToolTip(tr("Drop the changes since the last Apply: the guides and keys as they were (Esc)"));
        lengthApply_ = new QPushButton(tr("Apply"), lengthsBox_);
        lengthApply_->setObjectName(QStringLiteral("lengthApply"));
        lengthApply_->setToolTip(tr("Set the guides to the lengths shown (one undo step; Enter); the keys stay with the groom "
                                    "at x1 to place again"));
        row->addWidget(lengthStatus_, 1);
        row->addWidget(lengthCancel_);
        row->addWidget(lengthApply_);
        column->addLayout(row);

        connect(lengthScale_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            lengthsEdit("scale", [v](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.setKeyScale(h, s, float(v)); });
        });
        connect(lengthKey_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            lengthsEdit("length", [v](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.setKeyLength(h, s, float(v)); });
        });
        connect(lengthChange_, &QComboBox::currentIndexChanged, this, [this](int) {
            const LengthChange change = lengthChangeFromName(lengthChange_->currentData().toString().toStdString());
            lengthsEdit("change", [change](LengthKeyTool& lk, StrokeHost& h, const BrushToolSettings& s) { lk.setChange(h, s, change); });
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
            if (!lengthsBox_->isVisible() && tool_ != "lengths") return;
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
            lengthChange_->setCurrentIndex(std::max(0, lengthChange_->findData(QString::fromLatin1(lengthChangeName(lk.change)))));
            for (QWidget* w : {static_cast<QWidget*>(lengthScale_), static_cast<QWidget*>(lengthKey_),
                               static_cast<QWidget*>(lengthDeleteKey_)}) {
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
    card->addWidget(lengthsBox_);

    // Comb: keep lengths, or stretch.
    stretchBox_ = box(QString(), &column);
    QCheckBox* stretch = setting("stretch", [this] { return session_->settings().brush.stretch; },
                                 [this](bool on) { session_->settings().brush.stretch = on; });
    stretch->setObjectName(QStringLiteral("stretch"));
    stretch->setParent(stretchBox_);
    column->addWidget(stretch);
    card->addWidget(stretchBox_);

    // Shape: what it paints on the tubes (choices.tubeChannel: Width, Flatness, Twist), one at a
    // time; the tubes as a whole are the Tubes section's (tube_panel.h).
    tubeChannelBox_ = box(QString(), &column);
    tubeChannelBox_->setObjectName(QStringLiteral("tubeChannelBox"));
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        auto* paints = new QLabel(settingLabel("tubeChannel"), tubeChannelBox_);
        paints->setToolTip(settingHint("tubeChannel"));
        row->addWidget(paints);
        auto* group = new QButtonGroup(tubeChannelBox_);
        group->setExclusive(true);
        for (const ui::json& item : ui::Definitions::shipped().choices("tubeChannel")) {
            const std::string value = item.value("value", std::string());
            auto* chip = new QToolButton(tubeChannelBox_);
            chip->setText(QString::fromStdString(item.value("label", std::string())));
            chip->setToolTip(QString::fromStdString(item.value("tip", std::string())));
            chip->setCheckable(true);
            chip->setProperty("role", "chip");
            chip->setProperty("channel", QString::fromStdString(value));
            chip->setObjectName(QStringLiteral("tubeChannel_") + QString::fromStdString(value));
            chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            group->addButton(chip);
            tubeChannels_.push_back(chip);
            row->addWidget(chip);
            connect(chip, &QToolButton::clicked, this, [this, value] {
                if (updating_ || !session_) return;
                try {
                    session_->settings().brush.tubeChannel = tubeChannelFromString(value);
                } catch (const std::invalid_argument&) {
                    return;
                }
                edited();
            });
        }
        column->addLayout(row);
        refreshers_.push_back([this] {
            const QString current = QString::fromLatin1(tubeChannelName(session_->settings().brush.tubeChannel));
            for (QToolButton* chip : tubeChannels_) chip->setChecked(chip->property("channel").toString() == current);
        });
    }
    card->addWidget(tubeChannelBox_);

    // Erase: what it removes, and what it does not.
    auto threeChecks = [this](QWidget* owner, QVBoxLayout* into, const char* prefix,
                              std::vector<std::pair<QString, bool BrushToolSettings::*>> fields) {
        auto* row = new QHBoxLayout();
        row->setSpacing(12);
        for (const auto& [label, field] : fields) {
            QCheckBox* c = check(label, [this, field = field] { return session_->settings().*field; },
                                 [this, field = field](bool on) { session_->settings().*field = on; });
            c->setParent(owner);
            c->setObjectName(QString::fromLatin1(prefix) + label);
            row->addWidget(c);
        }
        row->addStretch(1);
        into->addLayout(row);
    };
    eraseBox_ = box(settingLabel("erases"), &column);
    eraseBox_->setObjectName(QStringLiteral("eraseBox"));
    eraseBox_->setToolTip(tr("What the Erase brush removes where it passes (one undo step)"));
    threeChecks(eraseBox_, column, "erase",
                {{tr("Guides"), &BrushToolSettings::eraseGuides}, {tr("Parts"), &BrushToolSettings::eraseParts},
                 {tr("Flow"), &BrushToolSettings::eraseFlow}});
    column->addWidget(hintLabel(text("eraseNote"), eraseBox_));
    card->addWidget(eraseBox_);

    // The shaping brushes: what they act on.
    targetBox_ = box(settingLabel("targets"), &column);
    targetBox_->setObjectName(QStringLiteral("targetsBox"));
    targetBox_->setToolTip(tr("What the brush shapes where it passes: guides, part lines, flow arrows and sinks (Comb slides and lays "
                              "arrows along the stroke; Twist turns arrows and whorls; Length sizes whorls; Smooth, Straighten and "
                              "Attract shape both; Cut splits parts)"));
    threeChecks(targetBox_, column, "target",
                {{tr("Guides"), &BrushToolSettings::brushGuides}, {tr("Parts"), &BrushToolSettings::brushParts},
                 {tr("Flow"), &BrushToolSettings::brushFlow}});
    card->addWidget(targetBox_);

    // Freeze: the mask as a whole.
    freezeBox_ = box(settingLabel("freezeMask"), &column);
    auto* unfreeze = new QPushButton(tr("Unfreeze All"), freezeBox_);
    unfreeze->setObjectName(QStringLiteral("unfreezeAll"));
    unfreeze->setToolTip(tr("Clear the freeze mask: every guide can be brushed again (a brush's Shift+click where no guide is does the same)"));
    connect(unfreeze, &QPushButton::clicked, this, [this] {
        if (!session_ || !session_->document()) return;
        session_->document()->editGuides("unfreeze all", [](GuideHierarchy& h) { unfreezeAll(h); });
        emit guidesEdited();
    });
    column->addWidget(unfreeze);
    card->addWidget(freezeBox_);

    gestures_ = hintLabel(text("gestures"), this);
    gestures_->setObjectName(QStringLiteral("gestures"));
    card->addWidget(gestures_);

    setTool({});
    setEnabled(false);
}

ValueSlider* BrushPanel::slider(QFormLayout* form, const QString& label, double min, double max, double limitMin, double limitMax,
                                int decimals, std::function<double()> get, std::function<void(double)> set, const QString& tip) {
    auto* editor = new ValueSlider(this);
    editor->setRange(min, max);
    editor->setLimits(limitMin, limitMax);
    editor->setDecimals(decimals);
    editor->setFill(theme::sliderFill(theme::blue()));
    editor->setCaption(label);
    if (!tip.isEmpty()) editor->setToolTip(tip);
    form->addRow(editor);
    connect(editor, &ValueSlider::valueChanged, this, [this, set](double v) {
        if (updating_ || !session_) return;
        set(v);
        edited();
    });
    refreshers_.push_back([editor, get] { editor->setValue(get()); });
    return editor;
}

QCheckBox* BrushPanel::check(const QString& label, std::function<bool()> get, std::function<void(bool)> set, const QString& tip) {
    auto* editor = new QCheckBox(label, this);
    if (!tip.isEmpty()) editor->setToolTip(tip);
    connect(editor, &QCheckBox::toggled, this, [this, set](bool on) {
        if (updating_ || !session_) return;
        set(on);
        edited();
    });
    refreshers_.push_back([editor, get] { editor->setChecked(get()); });
    return editor;
}

QCheckBox* BrushPanel::setting(const char* id, std::function<bool()> get, std::function<void(bool)> set) {
    QCheckBox* c = check(settingLabel(id), std::move(get), std::move(set), settingHint(id));
    c->setObjectName(QString::fromLatin1(id));
    return c;
}

QWidget* BrushPanel::box(const QString& title, QVBoxLayout** column) {
    auto* w = new QWidget(this);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, title.isEmpty() ? 0 : 4, 0, 0);
    layout->setSpacing(6);
    if (!title.isEmpty()) {
        auto* heading = new QLabel(title.toUpper(), w);
        heading->setProperty("role", "subheading");
        layout->addWidget(heading);
    }
    *column = layout;
    return w;
}

QWidget* BrushPanel::curve(const std::string& setting, const std::string& start, std::function<std::string()> get,
                           std::function<void(const std::string&)> set) {
    auto* holder = new QWidget(this);
    auto* layout = new QVBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    const QString hint = settingHint(setting.c_str());
    auto* on = new QCheckBox(settingLabel(setting.c_str()), holder);
    on->setObjectName(QString::fromStdString(setting));
    on->setToolTip(hint);
    auto* editor = new RampEditor(holder);
    editor->setObjectName(QString::fromStdString(setting) + QStringLiteral("Curve"));
    editor->setValue(nlohmann::json::parse(start));
    editor->setVisible(false);
    editor->setToolTip(hint + tr(" (right-click: presets)"));
    layout->addWidget(on);
    layout->addWidget(editor);
    auto push = [this, on, editor, set] {
        if (updating_ || !session_) return;
        try {
            set(on->isChecked() ? editor->value().dump() : std::string());
        } catch (const std::invalid_argument&) {
            return;
        }
        edited();
    };
    connect(on, &QCheckBox::toggled, this, [editor, push](bool checked) {
        editor->setVisible(checked);
        push();
    });
    connect(editor, &RampEditor::edited, this, [push](bool live) {
        if (!live) push();
    });
    refreshers_.push_back([on, editor, get] {
        const std::string json = get();
        on->setChecked(!json.empty());
        editor->setVisible(!json.empty());
        if (!json.empty()) editor->setValue(nlohmann::json::parse(json, nullptr, false));
    });
    return holder;
}

void BrushPanel::edited() { emit settingsChanged(); }

void BrushPanel::setSession(BrushSession* session) {
    session_ = session;
    setEnabled(session_ != nullptr);
    refresh();
}

void BrushPanel::setTool(const std::string& id) {
    tool_ = id;
    const groomlab::BrushEntry* entry = id.empty() ? nullptr : brushEntry(id);
    auto uses = [entry](const char* setting) { return entry && entry->hasSetting(setting); };
    const bool brush = uses("radius");
    for (ValueSlider* s : {radius_, strength_, falloff_}) s->setVisible(brush);
    smooth_->setVisible(uses("smoothStrength"));
    brushForm_->setVisible(brush || uses("smoothStrength"));
    falloffCurve_->setVisible(uses("falloffProfile"));
    strengthCurve_->setVisible(uses("strengthProfile"));
    // The strength's span: -1..1 for the tools that work both ways, else 0..1 (Maya's).
    if (entry) {
        strength_->setRange(entry->signedStrength ? -1.0 : 0.0, 1.0);
        strength_->setCaption(entry->signedStrength ? tr("Strength (±)") : settingLabel("strength"));
    }
    selectedOnly_->setVisible(uses("selectedOnly"));
    pressure_->setVisible(uses("pressure"));
    collide_->setVisible(uses("collide"));
    frontFacing_->setVisible(uses("frontFacingOnly"));
    propagate_->setVisible(uses("strengthProfile"));  // the shaping brushes (the app's own)
    pointwise_->setVisible(uses("map") && id == "paint");
    // The shown check boxes two to a row, no holes where others are hidden.
    for (QCheckBox* c : checkOrder_) checks_->removeWidget(c);
    int shown = 0;
    for (QCheckBox* c : checkOrder_) {
        if (c->isHidden()) continue;
        checks_->addWidget(c, shown / 2, shown % 2);
        ++shown;
    }
    createBox_->setVisible(uses("newGuides"));
    spacing_->setVisible(uses("spacing"));
    partBox_->setVisible(uses("parts"));
    flowBox_->setVisible(uses("flow"));
    lengthsBox_->setVisible(uses("lengthKeys"));
    stretchBox_->setVisible(uses("stretch"));
    tubeChannelBox_->setVisible(uses("tubeChannel"));
    eraseBox_->setVisible(uses("erases"));
    targetBox_->setVisible(uses("targets"));
    freezeBox_->setVisible(uses("freezeMask"));
    gestures_->setVisible(brush);
    hasSettings_ = brush || shown > 0 || uses("newGuides") || uses("parts") || uses("flow") || uses("lengthKeys") || uses("stretch") || uses("erases") ||
                   uses("targets") || uses("freezeMask") || uses("smoothStrength") || uses("tubeChannel");
    refresh();
}

void BrushPanel::lengthsEdit(const char* what,
                             const std::function<void(LengthKeyTool&, StrokeHost&, const BrushToolSettings&)>& fn,
                             bool applies) {
    if (updating_ || !session_) return;
    session_->editLengths(what, fn);
    refresh();
    if (!session_->lastError().empty()) lengthStatus_->setText(QString::fromStdString(session_->lastError()));
    if (applies) emit guidesEdited();
    else emit lengthsPreviewed();
}

void BrushPanel::refresh() {
    if (!session_) return;
    updating_ = true;
    for (const auto& r : refreshers_) r();
    updating_ = false;
}

void BrushPanel::setParts(const std::vector<std::string>& names) {
    const QString current = parts_->currentItem() ? parts_->currentItem()->text() : QString();
    const QSignalBlocker blocker(parts_);
    parts_->clear();
    for (const std::string& name : names) parts_->addItem(QString::fromStdString(name));
    const auto found = parts_->findItems(current, Qt::MatchExactly);
    if (!found.isEmpty()) parts_->setCurrentItem(found.front());
    else if (parts_->count() > 0) parts_->setCurrentRow(parts_->count() - 1);
    for (QPushButton* b : partButtons_->findChildren<QPushButton*>()) b->setEnabled(parts_->count() > 0);
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

std::string BrushPanel::selectedPart() const { return parts_->currentItem() ? parts_->currentItem()->text().toStdString() : std::string(); }

void BrushPanel::setBindings(const ui::Bindings& bindings) {
    for (QPushButton* b : partButtons_->findChildren<QPushButton*>()) delete b;
    const ui::Definitions& d = ui::Definitions::shipped();
    ui::ResolveOptions options;
    options.state.reset();  // an action bar: every entry, enabled by its binding
    options.keep = [&bindings](const std::string& id) { return bindings.count(id) > 0; };
    for (const ui::Item& item : d.resolve(d.get("lists/parts/bar"), options)) {
        if (item.kind != ui::Item::Kind::Action) continue;
        auto* b = new QPushButton(QString::fromStdString(item.label), partButtons_);
        b->setObjectName(QStringLiteral("parts_") + QString::fromStdString(item.id).section(QLatin1Char('.'), 1));
        b->setToolTip(QString::fromStdString(item.tip));
        b->setEnabled(parts_->count() > 0);
        const ui::Binding binding = bindings.at(item.id);
        connect(b, &QPushButton::clicked, this, [binding] { binding.trigger(nullptr, false); });
        partButtons_->layout()->addWidget(b);
    }
}

}  // namespace groomlab::app
