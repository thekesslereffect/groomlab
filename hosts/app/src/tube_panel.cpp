// SPDX-License-Identifier: Apache-2.0
#include "tube_panel.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "groomlab/brush.h"
#include "groomlab/tube.h"

#include "brush_session.h"
#include "card_parts.h"
#include "definitions.h"
#include "glass.h"
#include "groom_document.h"
#include "param_panel.h"
#include "theme.h"
#include "tube_shape_editor.h"

namespace groomlab::app {

namespace {

using nlohmann::json;

constexpr double kPi = 3.14159265358979;
constexpr int kPreviewMs = 60;  // a drag's live preview at most this often

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

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

std::vector<std::string> builtinNames() {
    std::vector<std::string> out;
    for (const ui::json& item : defs().choices("tubeShape")) out.push_back(item.value("value", std::string()));
    return out;
}

void restyle(QWidget* w) {
    w->style()->unpolish(w);
    w->style()->polish(w);
}

}  // namespace

TubePanel::TubePanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tubePanel"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    targetsLabel_ = new QLabel(this);
    targetsLabel_->setObjectName(QStringLiteral("tubeTargets"));
    targetsLabel_->setWordWrap(true);
    targetsLabel_->setProperty("role", "hint");
    targetsLabel_->hide();  // (its words are the fields' tips)

    shape_ = new QComboBox(this);
    shape_->setObjectName(QStringLiteral("tubeShape"));
    shape_->setToolTip(tr("The tube shape: picked, it goes to the targeted guides and to the guides Place and Draw make. The "
                          "built-ins, then the groom's library of custom shapes"));
    shape_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    shape_->setMinimumContentsLength(6);
    connect(shape_, &QComboBox::activated, this, [this](int) { pick(shape_->currentData().toString().toStdString()); });

    // Width, Flatness, Twist: the sections the same root to tip (in New's block or Edit's).
    fields_ = new QWidget(this);
    auto* fields = new QVBoxLayout(fields_);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setSpacing(4);
    width_ = committingSlider(0.0, 1.0, 0.0, 1e4, 3, &TubePanel::setWidth);
    width_->setObjectName(QStringLiteral("tubeWidth"));
    width_->setValue(0.1);
    flatness_ = committingSlider(0.0, 0.99, 0.0, 0.99, 2, &TubePanel::setFlatness);
    flatness_->setObjectName(QStringLiteral("tubeFlatness"));
    flatness_->setValue(0.0);
    twist_ = committingSlider(-180.0, 180.0, -180.0, 180.0, 1, &TubePanel::setTwist);
    twist_->setObjectName(QStringLiteral("tubeTwist"));
    twist_->setValue(0.0);
    // The names inside the fields.
    width_->setCaption(tr("Width"));
    fields->addWidget(width_);
    refreshUnits();
    flatness_->setCaption(tr("Flatness"));
    fields->addWidget(flatness_);
    twist_->setCaption(tr("Twist (°)"));
    fields->addWidget(twist_);
    refreshTips();

    // ---- New (Place, Draw): the tube new guides get.
    tubeBlock_ = new CardBlock(tr("Tube"), this);
    tubeBlock_->setObjectName(QStringLiteral("tubeBlock"));
    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        preview_ = new TubeShapeEditor(tubeBlock_);
        preview_->setObjectName(QStringLiteral("tubePreview"));
        preview_->setMinimumSize(56, 56);
        preview_->setFixedSize(56, 56);
        preview_->setAttribute(Qt::WA_TransparentForMouseEvents);
        preview_->setFocusPolicy(Qt::NoFocus);
        preview_->setToolTip(QString());
        row->addWidget(preview_);
        newPicker_ = new QVBoxLayout();
        newPicker_->setContentsMargins(0, 0, 0, 0);
        newPicker_->setSpacing(4);
        auto* hint = new QLabel(qs(defs().get("palette/texts").value("profileHint", std::string())), tubeBlock_);
        hint->setObjectName(QStringLiteral("tubeProfileHint"));
        hint->setProperty("role", "hint");
        hint->setWordWrap(true);
        newPicker_->addWidget(hint);
        newPicker_->addStretch(1);
        row->addLayout(newPicker_, 1);
        tubeBlock_->rows()->addLayout(row);
        newFields_ = new QVBoxLayout();
        newFields_->setContentsMargins(0, 0, 0, 0);
        tubeBlock_->rows()->addLayout(newFields_);
    }
    column->addWidget(tubeBlock_);

    // ---- Edit (Shape, Select): the profile, the library's glyphs in the header.
    profileBlock_ = new CardBlock(tr("Profile"), this);
    profileBlock_->setObjectName(QStringLiteral("profileBlock"));
    auto glyph = [this](const char* icon, const char* name, const QString& tip, void (TubePanel::*run)()) {
        QToolButton* b = makeGlyph(QLatin1String(icon), tip, profileBlock_);
        b->setObjectName(QLatin1String(name));
        connect(b, &QToolButton::clicked, this, [this, run] { (this->*run)(); });
        profileBlock_->addAction(b);
        return b;
    };
    new_ = glyph("plus", "tubeShapeNew", tr("New shape: a library shape (a circle to start from)"), &TubePanel::newShape);
    rename_ = glyph("glyph_rename", "tubeShapeRename", tr("Rename this library shape (its guides keep it)"), &TubePanel::startRename);
    duplicate_ = glyph("copy", "tubeShapeDuplicate",
                       tr("Duplicate: a copy of this shape in the groom's library, to change on its own (the targeted guides take it): "
                          "how a built-in shape is edited"),
                       &TubePanel::duplicateShape);
    delete_ = glyph("glyph_delete", "tubeShapeDelete", tr("Delete this shape from the library: click twice (its guides become round)"),
                    &TubePanel::deleteShape);
    delete_->setText(tr("Delete"));
    {
        // The brush's tubes in the viewport (Maya's Tube card): shaded surfaces, wires.
        shaded_ = new QCheckBox(tr("Shaded"), profileBlock_);
        shaded_->setObjectName(QStringLiteral("tubeShaded"));
        shaded_->setToolTip(tr("Draw the Tube brush's tubes as shaded surfaces"));
        wireframe_ = new QCheckBox(tr("Wireframe"), profileBlock_);
        wireframe_->setObjectName(QStringLiteral("tubeWireframe"));
        wireframe_->setToolTip(tr("Draw the Tube brush's tubes as wires (the selected guides' warm)"));
        wireframe_->setChecked(true);
        for (QCheckBox* box : {shaded_, wireframe_}) {
            connect(box, &QCheckBox::toggled, this, [this] { emit displayChanged(); });
            profileBlock_->rows()->addWidget(switchRow(profileBlock_, box));
        }
        // Root, Mid and Tip (Maya's rows): a button choosing what the editor shows, the picker
        // and Apply (the shape and its Length to every target) in one field row, its Length under it.
        mid_ = new QComboBox(this);
        mid_->setObjectName(QStringLiteral("tubeMidShape"));
        mid_->setToolTip(tr("The tube's profile at its middle: picked, it goes to the targeted guides (Between: halfway from the "
                            "root's to the tip's)"));
        tip_ = new QComboBox(this);
        tip_->setObjectName(QStringLiteral("tubeTipShape"));
        tip_->setToolTip(tr("The tube's profile at its tip: picked, it goes to the targeted guides (As root: the root's again)"));
        for (QComboBox* box : {mid_, tip_}) {
            box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            box->setMinimumContentsLength(6);
        }
        connect(mid_, &QComboBox::activated, this, [this](int) {
            const QVariant v = mid_->currentData();
            if (v.toString() != QStringLiteral("*")) pickProfile(1, v.toString().toStdString());
        });
        connect(tip_, &QComboBox::activated, this, [this](int) {
            const QVariant v = tip_->currentData();
            if (v.toString() != QStringLiteral("*")) pickProfile(2, v.toString().toStdString());
        });
        static const char* const kLabels[3] = {"Root", "Mid", "Tip"};
        static const char* const kApplyNames[3] = {"tubeShapeApply", "tubeMidShapeApply", "tubeTipShapeApply"};
        static const char* const kHoldNames[3] = {"tubeRootLength", "tubeMidLength", "tubeTipLength"};
        for (int k = 0; k < 3; ++k) {
            auto* pickBox = new QFrame(profileBlock_);
            pickBox->setProperty("role", "propField");
            pickBox->setMinimumHeight(theme::rowHeight());
            auto* line = new QHBoxLayout(pickBox);
            line->setContentsMargins(4, 0, 2, 0);
            line->setSpacing(4);
            QToolButton* which = makeChip(tr(kLabels[k]), tr("Show this profile in the editor (a library shape there is edited for "
                                                             "every guide taking it)"),
                                          profileBlock_);
            which->setObjectName(QStringLiteral("tubeProfile") + QLatin1String(kLabels[k]));
            which->setAutoExclusive(true);
            which->setFixedWidth(44);  // (the three pickers start at one edge)
            which->setChecked(k == 0);
            connect(which, &QToolButton::clicked, this, [this, k] { setActiveProfile(k); });
            profileButtons_[static_cast<std::size_t>(k)] = which;
            line->addWidget(which);
            if (k == 0) {
                editPicker_ = line;  // (setMode puts the root's picker here, before Apply)
            } else {
                line->addWidget(k == 1 ? mid_ : tip_, 1);
            }
            QPushButton* apply = new QPushButton(tr("Apply"), profileBlock_);
            apply->setObjectName(QLatin1String(kApplyNames[k]));
            apply->setToolTip(tr("Give the selected guides this %1 profile and its Length").arg(tr(kLabels[k]).toLower()));
            connect(apply, &QPushButton::clicked, this, [this, k] { applyProfile(k); });
            line->addWidget(apply);
            profileApply_[static_cast<std::size_t>(k)] = apply;
            if (k == 0) apply_ = apply;
            profileBlock_->rows()->addWidget(pickBox);
            ValueSlider* hold = new ValueSlider(this);
            hold->setObjectName(QLatin1String(kHoldNames[k]));
            hold->setRange(0.0, 100.0);
            hold->setLimits(0.0, 100.0);
            hold->setDecimals(0);
            hold->setFill(theme::sliderFill(theme::blue()));
            hold->setCaption(tr("Length"));
            hold->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral("%"); });
            hold->setToolTip(tr("How much of the tube is exactly the %1 profile's shape (%2); 0%: only at its own point. Between "
                                "these stretches the shape changes smoothly")
                                 .arg(tr(kLabels[k]).toLower(),
                                      k == 0 ? tr("from the root") : k == 1 ? tr("centered on the middle") : tr("up to the tip")));
            // A drag commits at its release (one undo step); a typed value or a wheel step at once.
            sliders_.push_back(hold);
            connect(hold, &ValueSlider::pressed, this, [this, hold] {
                if (std::find(held_.begin(), held_.end(), hold) == held_.end()) held_.push_back(hold);
            });
            connect(hold, &ValueSlider::released, this, [this, k, hold] {
                const auto at = std::find(held_.begin(), held_.end(), hold);
                if (at == held_.end()) return;
                held_.erase(at);
                if (!updating_) setHold(k, hold->value());
            });
            connect(hold, &ValueSlider::valueChanged, this, [this, k, hold](double v) {
                if (updating_ || std::find(held_.begin(), held_.end(), hold) != held_.end()) return;
                setHold(k, v);
            });
            holds_[static_cast<std::size_t>(k)] = hold;
            profileBlock_->rows()->addWidget(hold);
        }
        // The editor: the chosen shape's outline (at most 200 px square), and the library's edits.
        editor_ = new TubeShapeEditor(profileBlock_);
        editor_->setMaximumWidth(200);
        connect(editor_, &TubeShapeEditor::dragged, this, [this](const std::vector<SectionPoint>&) {
            if (!previewTimer_->isActive()) previewTimer_->start();
        });
        connect(editor_, &TubeShapeEditor::committed, this, [this](const std::vector<SectionPoint>& points) { shapeCommitted(points); });
        profileBlock_->rows()->addWidget(editor_, 0, Qt::AlignHCenter);
        // Under the drawing, never over it.
        builtinNote_ = new QLabel(tr("Built-in shape: Duplicate to edit"), profileBlock_);
        builtinNote_->setProperty("role", "hint");
        builtinNote_->setAlignment(Qt::AlignHCenter);
        builtinNote_->hide();
        profileBlock_->rows()->addWidget(builtinNote_);
        // The outline's options: a library shape's (a built-in is read-only).
        shapeOptions_ = new QWidget(profileBlock_);
        shapeOptions_->setObjectName(QStringLiteral("tubeShapeOptions"));
        auto* options = new QHBoxLayout(shapeOptions_);
        options->setContentsMargins(0, 0, 0, 0);
        options->setSpacing(4);
        mirror_ = makeChip(tr("Mirror"),
                           tr("Keep the outline's left and right sides alike (about the up axis): a point and its mirror image move, come "
                              "and go together"),
                           profileBlock_);
        mirror_->setObjectName(QStringLiteral("tubeShapeMirror"));
        mirror_->setChecked(true);
        connect(mirror_, &QToolButton::toggled, this, [this](bool on) {
            if (!updating_) setMirror(on);
        });
        shapeSmooth_ = makeChip(tr("Smooth"), tr("A smooth curve through the points; off: straight edges and sharp corners"), profileBlock_);
        shapeSmooth_->setObjectName(QStringLiteral("tubeShapeSmooth"));
        shapeSmooth_->setChecked(true);
        connect(shapeSmooth_, &QToolButton::toggled, this, [this](bool on) {
            if (!updating_) setShapeSmooth(on);
        });
        options->addWidget(mirror_);
        options->addWidget(shapeSmooth_);
        options->addStretch(1);
        profileBlock_->rows()->addWidget(shapeOptions_);
        renameField_ = new QLineEdit(profileBlock_);
        renameField_->setObjectName(QStringLiteral("tubeShapeName"));
        renameField_->setPlaceholderText(tr("New name, then Enter (Esc: keep it)"));
        connect(renameField_, &QLineEdit::returnPressed, this, [this] { finishRename(); });
        renameField_->installEventFilter(this);
        renameField_->hide();
        profileBlock_->rows()->addWidget(renameField_);
    }
    column->addWidget(profileBlock_);

    // ---- Edit: the selected guides' sections; Smooth evens out what the brush painted.
    selectedBlock_ = new CardBlock(tr("Selected guides"), this);
    selectedBlock_->setObjectName(QStringLiteral("selectedGuidesBlock"));
    {
        smooth_ = new QToolButton(selectedBlock_);
        smooth_->setObjectName(QStringLiteral("tubeSmooth"));
        smooth_->setProperty("role", "ghost");
        smooth_->setProperty("outlined", true);  // a block header's action: an outlined split button
        smooth_->setText(tr("Smooth"));
        smooth_->setAutoRaise(true);
        smooth_->setPopupMode(QToolButton::MenuButtonPopup);
        smooth_->setToolTip(tr("Smooth the targeted guides' tubes along each guide: what the brush painted evened out, the root's and "
                               "tip's sections kept (Shift+drag with the brush smooths under it). Its arrow: which channels"));
        auto* menu = new QMenu(smooth_);
        menu->setObjectName(QStringLiteral("tubeSmoothChannels"));
        menu->setToolTipsVisible(true);
        auto* group = new QActionGroup(menu);
        group->setExclusive(true);
        const QString channelsTip = qs(defs().get("choices/tubeSmooth").value("tip", std::string()));
        for (const ui::json& item : defs().choices("tubeSmooth")) {
            const std::string value = item.value("value", std::string());
            QAction* a = menu->addAction(qs(item.value("label", std::string())));
            a->setCheckable(true);
            a->setChecked(value == smoothChannels_);
            a->setToolTip(channelsTip);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, value] { smoothChannels_ = value; });
        }
        smooth_->setMenu(menu);
        connect(smooth_, &QToolButton::clicked, this, [this] { smoothTargets(); });
        selectedBlock_->addAction(smooth_);
        editFields_ = new QVBoxLayout();
        editFields_->setContentsMargins(0, 0, 0, 0);
        selectedBlock_->rows()->addLayout(editFields_);
    }
    column->addWidget(selectedBlock_);

    disarm_ = new QTimer(this);
    disarm_->setSingleShot(true);
    disarm_->setInterval(4000);
    connect(disarm_, &QTimer::timeout, this, [this] { disarmDelete(); });
    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(kPreviewMs);
    connect(previewTimer_, &QTimer::timeout, this, [this] { sendPreview(); });

    placed_ = true;
    setMode(Mode::Edit);
    fillShapes();
    showChoice();
    refresh();
}

void TubePanel::setMode(Mode mode) {
    const bool edit = mode == Mode::Edit;
    if (mode != mode_ || placed_) {
        placed_ = false;
        // The picker and the fields move to the mode's block.
        if (edit) {
            editPicker_->insertWidget(1, shape_, 1);  // (after its Root button)
            editFields_->addWidget(fields_);
        } else {
            newPicker_->insertWidget(0, shape_);
            newFields_->addWidget(fields_);
        }
        shape_->show();
        fields_->show();
    }
    mode_ = mode;
    tubeBlock_->setVisible(!edit);
    profileBlock_->setVisible(edit);
    selectedBlock_->setVisible(edit);
    refreshTips();
}

void TubePanel::refreshTips() {
    // What the fields change: the targets' tubes, else the tube new guides get.
    const QString targets = targetsLabel_->text();
    auto tip = [&targets](const QString& own) { return targets.isEmpty() ? own : own + QStringLiteral("\n\n") + targets; };
    width_->setToolTip(tip(tr("The widest section's half-width (a round tube's radius): the targeted tubes scaled to it, their taper "
                              "and what the brush painted kept; new guides' where none are near")));
    flatness_->setToolTip(tip(tr("0: as the shape is drawn; toward 1 a flat ribbon (the sections' aspect 1 - this, the same root to tip)")));
    twist_->setToolTip(tip(tr("Degrees: the sections turned about the guide from lying flat on the scalp, the same root to tip (the "
                              "brush's Twist paints it along the guide)")));
    if (selectedBlock_) selectedBlock_->titleLabel()->setToolTip(targets);
}

QAbstractButton* TubePanel::mirrorBox() const { return mirror_; }
QAbstractButton* TubePanel::shadedBox() const { return shaded_; }
QAbstractButton* TubePanel::wireframeBox() const { return wireframe_; }
bool TubePanel::shaded() const { return shaded_ && shaded_->isChecked(); }
bool TubePanel::wireframe() const { return wireframe_ && wireframe_->isChecked(); }

std::string& TubePanel::activeChoice() { return active_ == 1 ? midChoice_ : active_ == 2 ? tipChoice_ : choice_; }
const std::string& TubePanel::activeChoice() const { return active_ == 1 ? midChoice_ : active_ == 2 ? tipChoice_ : choice_; }

void TubePanel::setActiveProfile(int profile) {
    active_ = std::clamp(profile, 0, 2);
    for (int k = 0; k < 3; ++k) {
        if (QToolButton* b = profileButtons_[static_cast<std::size_t>(k)]) {
            const QSignalBlocker quiet(b);
            b->setChecked(k == active_);
        }
    }
    showChoice(active_ == 0 && shapes_.size() > 1 && !targets_.empty());
}
QAbstractButton* TubePanel::shapeSmoothBox() const { return shapeSmooth_; }

QString TubePanel::widthCaption() const { return width_->caption(); }

ValueSlider* TubePanel::committingSlider(double min, double max, double limitMin, double limitMax, int decimals,
                                         void (TubePanel::*commit)(double)) {
    auto* s = new ValueSlider(this);
    s->setRange(min, max);
    s->setLimits(limitMin, limitMax);
    s->setDecimals(decimals);
    s->setFill(theme::sliderFill(theme::blue()));
    s->setValueWidth(46);
    sliders_.push_back(s);
    // A drag commits at its release (one undo step); a typed value or a wheel step at once.
    connect(s, &ValueSlider::pressed, this, [this, s] {
        if (std::find(held_.begin(), held_.end(), s) == held_.end()) held_.push_back(s);
    });
    connect(s, &ValueSlider::released, this, [this, s, commit] {
        const auto at = std::find(held_.begin(), held_.end(), s);
        if (at == held_.end()) return;
        held_.erase(at);
        if (!updating_) (this->*commit)(s->value());
    });
    connect(s, &ValueSlider::valueChanged, this, [this, s, commit](double v) {
        if (updating_ || std::find(held_.begin(), held_.end(), s) != held_.end()) return;
        (this->*commit)(v);
    });
    return s;
}

void TubePanel::setSession(BrushSession* session) {
    session_ = session;
    refresh(true);
}

GroomDocument* TubePanel::document() const { return session_ ? session_->document() : nullptr; }

bool TubePanel::busy() const {
    if (!held_.empty() || editor_->busy() || !renameField_->isHidden()) return true;
    return std::any_of(sliders_.begin(), sliders_.end(), [](ValueSlider* s) { return s->editing(); });
}

const TubePanel::LibraryShape* TubePanel::library(const std::string& name) const {
    for (const LibraryShape& s : library_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

bool TubePanel::libraryShape(const std::string& name) const { return library(name) != nullptr; }

void TubePanel::refresh(bool force) {
    if (force ? editor_->busy() : busy()) return;
    GroomDocument* doc = document();
    GroomDocument::TubeTargets info;
    library_.clear();
    if (doc) {
        info = doc->tubeTargets();
        for (const GroomDocument::TubeShapeInfo& s : doc->tubeShapes()) {
            if (!s.builtin) library_.push_back({s.name, s.points, s.smooth, s.users});
        }
    }
    targets_ = info.guides;
    source_ = info.source;
    shapes_ = info.shapes;
    mids_ = info.midShapes;
    tips_ = info.tipShapes;
    updating_ = true;
    fillShapes();
    for (QWidget* w : {static_cast<QWidget*>(smooth_), static_cast<QWidget*>(apply_)}) {
        w->setEnabled(info.tube.has_value());
    }
    for (QPushButton* b : profileApply_) {
        if (b) b->setEnabled(info.tube.has_value());
    }
    if (!info.tube) {
        targetsLabel_->setText(tr("No guides selected or soloed: the shape and values here go to the guides Place and Draw make. "
                                  "Select guides (Select tool), or Shift+click one with a brush, to set theirs."));
        targetsKey_.clear();
    } else {
        const int count = static_cast<int>(targets_.size());
        const QString how = source_ == "selection" ? tr("selected") : tr("soloed");
        targetsLabel_->setText(tr("%1 guide%2 %3: changes here go to %4, and to the guides Place and Draw make.")
                                   .arg(count)
                                   .arg(count == 1 ? QString() : QStringLiteral("s"), how, count == 1 ? tr("it") : tr("them")));
        const TubeProfile& tube = *info.tube;
        const float widest = tube.width.empty() ? 0.1f : *std::max_element(tube.width.begin(), tube.width.end());
        reference_ = widest * info.scale;
        float aspect = 0.0f, twist = 0.0f;
        for (const float a : tube.aspect) aspect += a;
        for (const float t : tube.twist) twist += t;
        aspect = tube.aspect.empty() ? 1.0f : aspect / static_cast<float>(tube.aspect.size());
        twist = tube.twist.empty() ? 0.0f : twist / static_cast<float>(tube.twist.size());
        // The choice follows the targets when they (or their shapes) change, and is left alone
        // otherwise (a shape made or picked here while they stay selected).
        std::string key;
        for (const GuideId id : targets_) key += std::to_string(id) + ",";
        key += "|";
        for (const std::string& s : shapes_) key += s + ",";
        for (const std::string& s : mids_) key += "m" + s + ",";
        for (const std::string& s : tips_) key += "t" + s + ",";
        if (key != targetsKey_) {
            if (shapes_.size() == 1) choice_ = shapes_.front();
            if (mids_.size() == 1) midChoice_ = mids_.front();
            if (tips_.size() == 1) tipChoice_ = tips_.front();
        }
        targetsKey_ = key;
        const float held[3] = {tube.rootHold, tube.midHold, tube.tipHold};
        for (int k = 0; k < 3; ++k) {
            ValueSlider* hold = holds_[static_cast<std::size_t>(k)];
            if (hold && std::find(held_.begin(), held_.end(), hold) == held_.end() && !hold->editing()) hold->setValue(held[k] * 100.0);
        }
        width_->setValue(reference_ * lengthFactor_);
        flatness_->setValue(std::clamp(1.0 - aspect, 0.0, 0.99));
        twist_->setValue(twist * 180.0 / kPi);
    }
    updating_ = false;
    refreshTips();
    showChoice(info.tube.has_value() && shapes_.size() > 1);
    pushPlacement();
}

void TubePanel::fillShapes() {
    // The built-ins (choices.tubeShape), then the library; "Mixed" first while the targets differ.
    const bool mixed = shapes_.size() > 1 && !targets_.empty();
    // Rebuilt only when what it lists changed (the users counts in the tips too).
    std::string key = mixed ? "mixed|" : "|";
    for (const LibraryShape& s : library_) key += s.name + ":" + std::to_string(s.users) + "|";
    key += mids_.size() > 1 && !targets_.empty() ? "midMixed|" : "|";
    key += tips_.size() > 1 && !targets_.empty() ? "tipMixed|" : "|";
    if (key == shapesKey_ && shape_->count() > 0) return;
    shapesKey_ = key;
    // The mid's and the tip's: unset first (Between, As root), Mixed while the targets differ.
    for (int k = 1; k < 3; ++k) {
        QComboBox* box = k == 1 ? mid_ : tip_;
        const QSignalBlocker quiet(box);
        box->clear();
        if ((k == 1 ? mids_ : tips_).size() > 1 && !targets_.empty()) {
            box->addItem(tr("Mixed"), QStringLiteral("*"));
            box->setItemData(0, tr("The targeted guides have different profiles here: pick one to give them all"), Qt::ToolTipRole);
        }
        box->addItem(k == 1 ? tr("Between") : tr("As root"), QString());
        box->setItemData(box->count() - 1, k == 1 ? tr("Halfway between the root's profile and the tip's") : tr("The root's profile again at the tip"),
                         Qt::ToolTipRole);
        for (const ui::json& item : defs().choices("tubeShape")) {
            box->addItem(qs(item.value("label", std::string())), qs(item.value("value", std::string())));
            box->setItemData(box->count() - 1, qs(item.value("tip", std::string())), Qt::ToolTipRole);
        }
        if (!library_.empty()) box->insertSeparator(box->count());
        for (const LibraryShape& s : library_) box->addItem(qs(s.name), qs(s.name));
    }
    const QSignalBlocker blocker(shape_);
    shape_->clear();
    if (mixed) {
        shape_->addItem(tr("Mixed"), QString());
        shape_->setItemData(0, tr("The targeted guides have different shapes: pick one to give them all"), Qt::ToolTipRole);
    }
    for (const ui::json& item : defs().choices("tubeShape")) {
        shape_->addItem(qs(item.value("label", std::string())), qs(item.value("value", std::string())));
        shape_->setItemData(shape_->count() - 1, qs(item.value("tip", std::string())), Qt::ToolTipRole);
    }
    if (!library_.empty()) shape_->insertSeparator(shape_->count());
    for (const LibraryShape& s : library_) {
        shape_->addItem(qs(s.name), qs(s.name));
        shape_->setItemData(shape_->count() - 1,
                            tr("A custom shape of this groom's library (%1 guide%2)").arg(s.users).arg(s.users == 1 ? QString() : QStringLiteral("s")),
                            Qt::ToolTipRole);
    }
}

void TubePanel::showChoice(bool mixed) {
    const std::vector<std::string> builtins = builtinNames();
    auto known = [&](const std::string& name) {
        return library(name) || std::find(builtins.begin(), builtins.end(), name) != builtins.end();
    };
    if (!known(choice_)) choice_ = "round";  // (a library shape gone: renamed, deleted, another groom)
    if (!midChoice_.empty() && !known(midChoice_)) midChoice_.clear();
    if (!tipChoice_.empty() && !known(tipChoice_)) tipChoice_.clear();
    {
        const QSignalBlocker blocker(shape_);
        shape_->setCurrentIndex(mixed && active_ == 0 ? 0 : std::max(0, shape_->findData(qs(choice_))));
        if (mixed && active_ != 0) shape_->setCurrentIndex(shapes_.size() > 1 ? 0 : std::max(0, shape_->findData(qs(choice_))));
    }
    for (int k = 1; k < 3; ++k) {
        QComboBox* box = k == 1 ? mid_ : tip_;
        const QSignalBlocker quiet(box);
        const bool differ = (k == 1 ? mids_ : tips_).size() > 1 && !targets_.empty();
        box->setCurrentIndex(differ ? 0 : std::max(0, box->findData(qs(k == 1 ? midChoice_ : tipChoice_))));
    }
    const std::string shown = activeChoice().empty() ? choice_ : activeChoice();  // (an unset mid or tip: the root's)
    if (const LibraryShape* custom = activeChoice().empty() ? nullptr : library(shown)) {
        editor_->setShape(custom->points, custom->smooth, true);
        builtinNote_->hide();
        preview_->setShape(custom->points, custom->smooth, false);
        if (mirror_->isChecked() && editor_->setMirror(true)) {
            const std::vector<SectionPoint> made = editor_->points();
            shapeCommitted(made);  // made symmetric: saved so
            return;
        }
    } else if (const LibraryShape* root = library(shown)) {  // an unset mid or tip: the root's library shape, read-only
        editor_->setShape(root->points, root->smooth, false, QString());
        builtinNote_->hide();
        preview_->setShape(root->points, root->smooth, false);
    } else {
        TubeShape shape = TubeShape::Round;
        parseTubeShape(shown, &shape);
        bool smooth = true;
        const std::vector<SectionPoint> points = builtinTubeShapePoints(shape, &smooth);
        editor_->setShape(points, smooth, false, QString());
        builtinNote_->setVisible(!activeChoice().empty());
        preview_->setShape(points, smooth, false);
    }
    updating_ = true;
    shapeSmooth_->setChecked(editor_->smooth());
    updating_ = false;
    const bool custom = !activeChoice().empty() && library(activeChoice()) != nullptr;
    const bool hasGroom = document() != nullptr;
    shapeOptions_->setVisible(custom);
    new_->setEnabled(hasGroom);
    duplicate_->setEnabled(hasGroom);
    for (QToolButton* b : {rename_, delete_}) b->setEnabled(custom);
    for (QPushButton* b : profileApply_) {
        if (b) b->setEnabled(!targets_.empty());
    }
}

std::string TubePanel::placement() const {
    return json{{"shape", choice_},
                {"width", width_->value() / lengthFactor_},
                {"aspect", std::max(1.0 - flatness_->value(), 0.01)},
                {"twist", twist_->value() * kPi / 180.0}}
        .dump();
}

void TubePanel::pushPlacement() {
    if (!session_) return;
    const std::string tube = placement();
    if (tube == pushed_) return;
    try {
        setPlacementTube(session_->settings().place, tube);
        pushed_ = tube;
        emit settingsChanged();
    } catch (const std::invalid_argument& e) {
        emit message(QString::fromUtf8(e.what()));
    }
}

void TubePanel::edited(const std::string& error) {
    emit message(qs(error));
    refresh(true);
    if (error.empty()) emit guidesEdited();
}

void TubePanel::pick(const std::string& name) {
    if (name.empty()) return;  // "Mixed": nothing to give
    choice_ = name;
    if (active_ != 0) setActiveProfile(0);
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) {
        edited(doc->editTubes(targets_, json{{"shape", name}}));
    } else {
        showChoice();
    }
    pushPlacement();
}

void TubePanel::applyShape() { applyProfile(0); }

void TubePanel::pickProfile(int profile, const std::string& name) {
    (profile == 1 ? midChoice_ : tipChoice_) = name;
    setActiveProfile(profile);
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) edited(doc->editTubes(targets_, json{{profile == 1 ? "midShape" : "tipShape", name}}));
}

void TubePanel::applyProfile(int profile) {
    // Its shape (unless Mixed: the targets differ there) and its Length, to every target.
    static const char* const kShape[3] = {"shape", "midShape", "tipShape"};
    static const char* const kHold[3] = {"rootHold", "midHold", "tipHold"};
    json edit{{kHold[profile], holds_[static_cast<std::size_t>(profile)]->value() / 100.0}};
    const std::vector<std::string>& among = profile == 0 ? shapes_ : profile == 1 ? mids_ : tips_;
    if (among.size() <= 1) edit[kShape[profile]] = profile == 0 ? choice_ : profile == 1 ? midChoice_ : tipChoice_;
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) edited(doc->editTubes(targets_, edit));
    if (profile == 0) pushPlacement();
}

void TubePanel::setHold(int profile, double percent) {
    static const char* const kHold[3] = {"rootHold", "midHold", "tipHold"};
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) edited(doc->editTubes(targets_, json{{kHold[profile], std::clamp(percent, 0.0, 100.0) / 100.0}}));
}

void TubePanel::setWidth(double shown) {
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) {
        const double scene = shown / lengthFactor_;  // (shown in the Units' lengths)
        if (reference_ > 0.0f) edited(doc->scaleTubes(targets_, static_cast<float>(scene / reference_)));
        else edited(doc->editTubes(targets_, json{{"width", scene}}));
    }
    pushPlacement();
}

void TubePanel::refreshUnits() {
    // Width in the lengths' unit (the Graph Editor's Edit > Units, as Maya's View > Units): "Width
    // (mm)", the slider in mm; scene units as stored ("units").
    const std::string key = unitChoice("length");
    double factor = 1.0;
    QString unit = tr("units");
    for (const UnitChoice& c : unitChoices("length")) {
        if (c.key != key) continue;
        factor = c.factor;
        if (key != "scene") unit = QString::fromStdString(c.label);
    }
    width_->setCaption(tr("Width (%1)").arg(unit));
    if (factor == lengthFactor_) return;
    const double scene = width_->value() / lengthFactor_;
    lengthFactor_ = factor;
    const QSignalBlocker quiet(width_);
    width_->setLimits(0.0, 1e4 * factor);
    width_->setRange(0.0, factor);
    width_->setDecimals(factor >= 10.0 ? 2 : 3);
    width_->setValue(scene * factor);
}

void TubePanel::setFlatness(double flatness) {
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) edited(doc->editTubes(targets_, json{{"aspect", std::max(1.0 - flatness, 0.01)}}));
    pushPlacement();
}

void TubePanel::setTwist(double degrees) {
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) edited(doc->editTubes(targets_, json{{"twist", degrees * kPi / 180.0}}));
    pushPlacement();
}

void TubePanel::smoothTargets() {
    GroomDocument* doc = document();
    if (!doc || targets_.empty()) return;
    const std::string channel = smoothChannels_ == "current" && session_
                                    ? std::string(tubeChannelName(session_->settings().brush.tubeChannel))
                                    : std::string("all");
    edited(doc->smoothTubes(targets_, channel));
}

std::string TubePanel::uniqueName(const std::string& base) const {
    std::set<std::string> taken;
    for (const std::string& n : builtinNames()) taken.insert(n);
    for (const LibraryShape& s : library_) taken.insert(s.name);
    int k = 1;
    while (taken.count(base + " " + std::to_string(k))) ++k;
    return base + " " + std::to_string(k);
}

void TubePanel::editAsCustom() {
    GroomDocument* doc = document();
    if (!doc) return;
    const std::string base = activeChoice().empty() ? choice_ : activeChoice();  // (an unset mid or tip: the root's)
    TubeShape shape = TubeShape::Round;
    parseTubeShape(base, &shape);
    bool smooth = true;
    const std::vector<SectionPoint> points = builtinTubeShapePoints(shape, &smooth);
    const std::string name = uniqueName(base);
    static const char* const kShape[3] = {"shape", "midShape", "tipShape"};
    const std::string error = doc->addTubeShape(name, points, smooth, targets_, kShape[active_]);
    if (!error.empty()) {
        emit message(qs(error));
        return;
    }
    activeChoice() = name;
    refresh(true);
    pushPlacement();
    emit guidesEdited();
    emit message(tr("%1: a copy of the built-in to edit (drag its points).").arg(qs(name)));
}

void TubePanel::newShape() {
    GroomDocument* doc = document();
    if (!doc) return;
    bool smooth = true;
    const std::vector<SectionPoint> points = builtinTubeShapePoints(TubeShape::Round, &smooth);
    const std::string name = uniqueName("shape");
    const std::string error = doc->addTubeShape(name, points, true);
    if (error.empty()) activeChoice() = name;
    edited(error);
    pushPlacement();
}

void TubePanel::duplicateShape() {
    // A copy of the shape in the library, chosen: a built-in's is how it is edited.
    GroomDocument* doc = document();
    const LibraryShape* custom = library(activeChoice());
    if (doc && !custom) {
        editAsCustom();
        return;
    }
    if (!doc) return;
    const LibraryShape copy = *custom;
    const std::string name = uniqueName(copy.name);
    const std::string error = doc->addTubeShape(name, copy.points, copy.smooth);
    if (error.empty()) activeChoice() = name;
    edited(error);
    pushPlacement();
}

void TubePanel::startRename() {
    if (!library(activeChoice())) return;
    renameField_->setText(qs(activeChoice()));
    renameField_->show();
    renameField_->setFocus();
    renameField_->selectAll();
}

void TubePanel::finishRename() {
    GroomDocument* doc = document();
    const std::string to = renameField_->text().trimmed().toStdString();
    const std::string from = activeChoice();
    cancelRename();
    if (!doc || to.empty() || to == from || !library(from)) return;
    const std::string error = doc->renameTubeShape(from, to);
    if (error.empty()) {
        for (std::string* c : {&choice_, &midChoice_, &tipChoice_}) {  // every profile on it keeps it
            if (*c == from) *c = to;
        }
    }
    edited(error);
    pushPlacement();
}

void TubePanel::cancelRename() {
    // The focus on the editor (not passed on to a slider's field, which would read as typing there).
    editor_->setFocus();
    renameField_->hide();
}

bool TubePanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == renameField_) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            cancelRename();
            return true;
        }
        if (event->type() == QEvent::FocusOut && renameField_->isVisible()) renameField_->hide();
    }
    return QWidget::eventFilter(watched, event);
}

void TubePanel::deleteShape() {
    GroomDocument* doc = document();
    const LibraryShape* custom = library(activeChoice());
    if (!doc || !custom) return;
    if (!armed_) {
        armed_ = true;
        const std::size_t users = custom->users;
        delete_->setText(tr("Delete? %1 go%2 round").arg(users).arg(users == 1 ? QStringLiteral("es") : QString()));
        delete_->setProperty("armed", true);
        delete_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        delete_->setMinimumSize(24, 24);
        delete_->setMaximumSize(QWIDGETSIZE_MAX, 24);
        restyle(delete_);
        disarm_->start();
        return;
    }
    const std::string name = custom->name;
    disarmDelete();
    const std::string error = doc->removeTubeShape(name);
    if (error.empty()) {
        if (choice_ == name) choice_ = "round";
        if (midChoice_ == name) midChoice_.clear();
        if (tipChoice_ == name) tipChoice_.clear();
    }
    edited(error);
    pushPlacement();
}

void TubePanel::disarmDelete() {
    armed_ = false;
    disarm_->stop();
    delete_->setText(tr("Delete"));
    delete_->setProperty("armed", QVariant());
    delete_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    delete_->setFixedSize(24, 24);
    restyle(delete_);
}

void TubePanel::sendPreview() {
    GroomDocument* doc = document();
    if (!doc || !editor_->problem().isEmpty() || !library(activeChoice())) return;
    if (doc->previewTubeShape(activeChoice(), editor_->points(), editor_->smooth()).empty()) {
        previewing_ = true;
        emit previewed();
    }
}

void TubePanel::shapeCommitted(const std::vector<SectionPoint>& points) {
    previewTimer_->stop();
    GroomDocument* doc = document();
    if (!doc || !library(activeChoice())) return;
    const std::string error = doc->setTubeShape(activeChoice(), &points, editor_->smooth());
    const bool wasPreviewing = previewing_;
    previewing_ = false;
    if (!error.empty()) {
        doc->endTubePreview();
        emit message(qs(error));
        refresh(true);
        if (wasPreviewing) emit previewed();  // the groom as it was
        return;
    }
    edited(error);
}

void TubePanel::setMirror(bool on) {
    if (editor_->setMirror(on) && library(activeChoice())) {
        const std::vector<SectionPoint> made = editor_->points();
        shapeCommitted(made);
    }
}

void TubePanel::setShapeSmooth(bool on) {
    GroomDocument* doc = document();
    if (!doc || !library(activeChoice())) return;
    edited(doc->setTubeShape(activeChoice(), nullptr, on));
}

}  // namespace groomlab::app
