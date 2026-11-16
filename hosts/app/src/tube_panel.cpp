// SPDX-License-Identifier: Apache-2.0
#include "tube_panel.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
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
#include "groomlab/root_tubes.h"
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
        colors_ = new QCheckBox(tr("Colors"), profileBlock_);
        colors_->setObjectName(QStringLiteral("tubeColors"));
        colors_->setToolTip(tr("Color each tube as the root tube it comes from (its split tubes and children in its color), a "
                               "color of its own without one"));
        colors_->setChecked(true);
        always_ = new QCheckBox(tr("Always visible"), profileBlock_);
        always_->setObjectName(QStringLiteral("tubesAlwaysVisible"));
        always_->setToolTip(tr("Keep the tubes drawn whichever tool is up (Comb, Select, ...), not only the Tube tool"));
        for (QCheckBox* box : {shaded_, wireframe_, colors_, always_}) {
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
    // ---- Tube roots and Tube hierarchy (Edit: the Tube tool), over the profile.
    rootsBlock_ = new CardBlock(tr("Tube roots"), this);
    rootsBlock_->setObjectName(QStringLiteral("rootTubes"));
    rootsBlock_->titleLabel()->setToolTip(tr("Tubes drawn root first: click the root's outline on the scalp, one CV at a time, "
                                             "and close it on its first CV. Each tube is its own region."));
    {
        auto button = [this](const QString& text, const char* name, const QString& tip, bool checkable, void (TubePanel::*run)()) {
            QAbstractButton* b = nullptr;
            if (checkable) {
                QToolButton* chip = makeChip(text, tip, rootsBlock_);  // (checked: the accent, as the card's chips)
                chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                b = chip;
            } else {
                b = new QPushButton(text, rootsBlock_);
                b->setToolTip(tip);
            }
            b->setObjectName(QLatin1String(name));
            connect(b, &QAbstractButton::clicked, this, [this, run] { (this->*run)(); });
            return b;
        };
        drawRoot_ = button(tr("Draw root"), "rootTubeDraw",
                           tr("Click the root's outline on the scalp, one CV at a time (drag to move the CV); click the first CV to "
                              "close it. Near a neighbouring root a CV snaps onto it, sealing the tubes. Again: leave"),
                           true, &TubePanel::drawRoot);
        editRoots_ = button(tr("Edit roots"), "rootTubeEdit",
                            tr("Show the root tubes' outlines with their CVs, the guides hidden; drag CVs on the scalp (Ctrl+click an "
                               "edge: a new CV there; a CV: taken out), then Finish edit"),
                            true, &TubePanel::editRoots);
        selectRoot_ = button(tr("Select root"), "rootTubeSelect",
                             tr("Show every root's middle: click one to select its root (Shift: several), drag it to move the root "
                                "over the scalp. Again: leave"),
                             true, &TubePanel::selectRoot);
        undoCv_ = button(tr("Undo CV"), "rootTubeUndoCv", tr("Take back the last CV (or the drawn path); in Edit roots, the last CV move"),
                         false, &TubePanel::undoCv);
        deleteRoots_ = button(tr("Delete"), "rootTubeDelete", tr("Delete the selected roots: their tubes and the tube hierarchy under them"),
                              false, &TubePanel::deleteRoots);
        duplicateRoots_ = button(tr("Duplicate"), "rootTubeDuplicate",
                                 tr("Copy the selected roots: the copies follow the cursor over the scalp, a click sets them down. "
                                    "Again: stop"),
                                 true, &TubePanel::duplicateRoots);
        // Two rows of chips that wrap on a narrow card (the modes; then Undo CV, Delete, Duplicate).
        for (const std::array<QAbstractButton*, 3>& row : {std::array<QAbstractButton*, 3>{drawRoot_, editRoots_, selectRoot_},
                                                           std::array<QAbstractButton*, 3>{undoCv_, deleteRoots_, duplicateRoots_}}) {
            FlowLayout* flow = nullptr;
            QWidget* line = chipRow(rootsBlock_, &flow);
            for (QAbstractButton* b : row) flow->addWidget(b);
            rootsBlock_->rows()->addWidget(line);
        }
        rootSnap_ = new QCheckBox(tr("Snap to neighbours"), rootsBlock_);
        rootSnap_->setObjectName(QStringLiteral("rootTubeSnap"));
        rootSnap_->setToolTip(tr("A CV near a neighbouring root's CV or edge snaps onto it, so the tubes meet with no gap"));
        rootSnap_->setChecked(true);
        connect(rootSnap_, &QCheckBox::toggled, this, &TubePanel::setRootSnap);
        rootDrawPath_ = new QCheckBox(tr("Draw guide before extrude"), rootsBlock_);
        rootDrawPath_->setObjectName(QStringLiteral("rootTubeDrawFirst"));
        rootDrawPath_->setToolTip(tr("Once the outline closes, drag from its middle out to draw the tube's guide. Off: the tube goes "
                                     "straight off the scalp, Length long, in Segments"));
        rootDrawPath_->setChecked(true);
        connect(rootDrawPath_, &QCheckBox::toggled, this, &TubePanel::setRootDrawPath);
        for (QCheckBox* box : {rootSnap_, rootDrawPath_}) rootsBlock_->rows()->addWidget(switchRow(rootsBlock_, box));
        rootDensity_ = committingSlider(0.25, 4.0, 0.05, 20.0, 2, &TubePanel::setRootDensity);
        rootDensity_->setObjectName(QStringLiteral("rootTubeDensity"));
        rootDensity_->setCaption(tr("Point density"));
        rootDensity_->setValue(1.0);
        rootDensity_->setToolTip(tr("How many points a drawn guide gets: 1, about one per root width along it; 2, twice as many"));
        rootLength_ = committingSlider(0.0, 10.0, 0.0001, 1e4, 3, &TubePanel::setRootLength);
        rootLength_->setObjectName(QStringLiteral("rootTubeLength"));
        rootLength_->setCaption(tr("Length"));
        rootLength_->setValue(1.0);
        rootLength_->setToolTip(tr("A tube's length when it goes straight off the scalp (Draw guide before extrude off)"));
        rootSegments_ = committingSlider(2.0, 32.0, 2.0, 256.0, 0, &TubePanel::setRootSegments);
        rootSegments_->setObjectName(QStringLiteral("rootTubeSegments"));
        rootSegments_->setCaption(tr("Segments"));
        rootSegments_->setValue(12.0);
        rootSegments_->setToolTip(tr("A straight tube guide's segments (Draw guide before extrude off)"));
        rootTaper_ = committingSlider(0.0, 1.0, 0.0, 4.0, 2, &TubePanel::setRootTaper);
        rootTaper_->setObjectName(QStringLiteral("rootTubeTaper"));
        rootTaper_->setCaption(tr("Tip width"));
        rootTaper_->setValue(0.5);
        rootTaper_->setToolTip(tr("The tube's width at its tip, as a share of its root's"));
        for (ValueSlider* s : {rootDensity_, rootLength_, rootSegments_, rootTaper_}) rootsBlock_->rows()->addWidget(s);
        setRootDrawPath(rootDrawPath_->isChecked());  // (Point density, or Length and Segments)
    }
    column->addWidget(rootsBlock_);
    levelsBlock_ = new CardBlock(tr("Tube hierarchy"), this);
    levelsBlock_->setObjectName(QStringLiteral("tubeHierarchy"));
    levelsBlock_->titleLabel()->setToolTip(tr("Tube levels: each level splits the tubes above it in two. Shaping a tube carries the "
                                              "smaller tubes inside it; pick a level to edit its tubes"));
    {
        addLevel_ = new QPushButton(tr("Add"), levelsBlock_);
        addLevel_->setObjectName(QStringLiteral("tubeLevelAdd"));
        addLevel_->setToolTip(tr("Split each selected tube in two (with none selected: every tube of the base group), sharing its "
                                 "root shape: a level deeper"));
        connect(addLevel_, &QPushButton::clicked, this, [this] { addLevel(); });
        removeLevel_ = new QPushButton(tr("Remove"), levelsBlock_);
        removeLevel_->setObjectName(QStringLiteral("tubeLevelRemove"));
        removeLevel_->setToolTip(tr("Undo a split: with tubes selected, theirs only (a split tube's deepest split tubes, or a split "
                                    "tube back into its parent); with none, the deepest level everywhere"));
        connect(removeLevel_, &QPushButton::clicked, this, [this] { removeLevel(); });
        auto* line = new QHBoxLayout();
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(4);
        line->addWidget(addLevel_, 1);
        line->addWidget(removeLevel_, 1);
        levelsBlock_->rows()->addLayout(line);
        levelBaseLabel_ = new QLabel(levelsBlock_);
        levelBaseLabel_->setObjectName(QStringLiteral("tubeLevelBase"));
        levelBaseLabel_->setProperty("role", "hint");
        levelsBlock_->rows()->addWidget(levelBaseLabel_);
        levelRow_ = new QHBoxLayout();
        levelRow_->setContentsMargins(0, 0, 0, 0);
        levelRow_->setSpacing(4);
        levelsBlock_->rows()->addLayout(levelRow_);
    }
    column->addWidget(levelsBlock_);
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
    rootsBlock_->setVisible(edit);
    levelsBlock_->setVisible(edit);
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
bool TubePanel::colors() const { return colors_ && colors_->isChecked(); }
bool TubePanel::alwaysVisible() const { return always_ && always_->isChecked(); }
QAbstractButton* TubePanel::colorsBox() const { return colors_; }
QAbstractButton* TubePanel::alwaysVisibleBox() const { return always_; }

// ---- Tube roots ------------------------------------------------------------------------------------

void TubePanel::startRoots(RootTubeTool::Mode mode) {
    if (!session_ || !session_->document()) {
        emit message(tr("Open a groom first."));
        refreshRoots();
        return;
    }
    RootTubeTool::Settings& s = session_->roots().settings;
    s.snap = rootSnap_->isChecked();
    s.drawPath = rootDrawPath_->isChecked();
    s.density = static_cast<float>(rootDensity_->value());
    s.length = static_cast<float>(rootLength_->value());
    s.segments = static_cast<int>(std::lround(rootSegments_->value()));
    s.taper = static_cast<float>(rootTaper_->value());
    // New roots go into the picked group when it is a top group, else rootTubes.
    s.group.clear();
    if (const HierarchyPtr h = session_->document()->session().authoredHierarchy()) {
        const std::string& picked = session_->settings().brush.group;
        const GroupId id = picked.empty() ? kNoGroup : h->findGroup(picked);
        if (id != kNoGroup && h->groupDepth(id) == 0) s.group = picked;
    }
    rootsDone(session_->startRoots(mode));
}

bool TubePanel::rootsDone(bool ok) {
    if (!ok && session_) emit message(QString::fromStdString(session_->lastError()));
    refreshRoots();
    emit rootModeChanged();
    return ok;
}

void TubePanel::drawRoot() {
    if (session_ && session_->rootMode() && (session_->roots().mode() == RootTubeTool::Mode::Place ||
                                             session_->roots().mode() == RootTubeTool::Mode::Draw)) {
        session_->leaveRoots();  // again: the outline being drawn dropped, the mode left
        rootsDone(true);
        return;
    }
    // A deeper level picked: level 1 shown and picked instead (a new root tube is a level 1 guide).
    fillLevels();
    if (levels_.size() > 1 && shownLevel_ > 1) showLevel(1);
    startRoots(RootTubeTool::Mode::Place);
}

void TubePanel::editRoots() {
    if (session_ && session_->rootMode() && session_->roots().mode() == RootTubeTool::Mode::Edit) {
        // Finish edit: the edits given to the roots (one undo step), the mode left.
        const bool ok = session_->editRoots("roots", [](RootTubeTool& t, StrokeHost& h) { t.finishEdit(h); });
        if (ok) session_->leaveRoots();
        rootsDone(ok);
        emit guidesEdited();
        return;
    }
    startRoots(RootTubeTool::Mode::Edit);
}

void TubePanel::selectRoot() {
    if (session_ && session_->rootMode() && (session_->roots().mode() == RootTubeTool::Mode::Select ||
                                             session_->roots().mode() == RootTubeTool::Mode::Duplicate)) {
        session_->leaveRoots();
        rootsDone(true);
        return;
    }
    startRoots(RootTubeTool::Mode::Select);
}

void TubePanel::undoCv() {
    if (!session_ || !session_->rootMode()) return;
    rootsDone(session_->editRoots("roots", [](RootTubeTool& t, StrokeHost& h) { t.undoCv(h); }));
}

void TubePanel::deleteRoots() {
    if (!session_ || !session_->rootMode()) return;
    rootsDone(session_->editRoots("roots", [](RootTubeTool& t, StrokeHost& h) { t.deleteSelected(h); }));
    emit guidesEdited();
}

void TubePanel::duplicateRoots() {
    if (!session_ || !session_->rootMode()) return;
    const bool duplicating = session_->roots().mode() == RootTubeTool::Mode::Duplicate;
    rootsDone(session_->editRoots("roots", [duplicating](RootTubeTool& t, StrokeHost& h) {
        if (duplicating) t.endDuplicate();
        else t.beginDuplicate(h);
    }));
}

void TubePanel::setRootSnap(bool on) {
    if (!session_) return;
    session_->roots().settings.snap = on;
    if (session_->rootMode()) rootsDone(session_->editRoots("roots", [](RootTubeTool& t, StrokeHost& h) { t.sync(h); }));
}

void TubePanel::setRootDrawPath(bool on) {
    rootDensity_->setVisible(on);
    rootLength_->setVisible(!on);
    rootSegments_->setVisible(!on);
    if (session_) session_->roots().settings.drawPath = on;
}

void TubePanel::setRootDensity(double density) {
    if (session_) session_->roots().settings.density = static_cast<float>(density);
}

void TubePanel::setRootLength(double length) {
    if (session_) session_->roots().settings.length = static_cast<float>(std::max(length, 1e-6));
}

void TubePanel::setRootSegments(double segments) {
    if (session_) session_->roots().settings.segments = std::max(2, static_cast<int>(std::lround(segments)));
}

void TubePanel::setRootTaper(double taper) {
    if (session_) session_->roots().settings.taper = static_cast<float>(taper);
}

void TubePanel::refreshRoots() {
    const bool on = session_ && session_->rootMode();
    const RootTubeTool::Mode mode = on ? session_->roots().mode() : RootTubeTool::Mode::Place;
    if (session_) session_->refreshRoots();
    const bool placing = on && (mode == RootTubeTool::Mode::Place || mode == RootTubeTool::Mode::Draw);
    const bool editing = on && mode == RootTubeTool::Mode::Edit;
    const bool selecting = on && (mode == RootTubeTool::Mode::Select || mode == RootTubeTool::Mode::Duplicate);
    const bool picked = selecting && !session_->roots().selected().empty();
    QSignalBlocker b1(drawRoot_), b2(editRoots_), b3(selectRoot_), b4(duplicateRoots_);
    drawRoot_->setChecked(placing);
    editRoots_->setChecked(editing);
    editRoots_->setText(editing ? tr("Finish edit") : tr("Edit roots"));
    selectRoot_->setChecked(selecting);
    undoCv_->setEnabled(placing || editing);
    deleteRoots_->setEnabled(picked);
    duplicateRoots_->setEnabled(picked);
    duplicateRoots_->setChecked(on && mode == RootTubeTool::Mode::Duplicate);
    if (on && session_->roots().messageCount() != rootsHeard_) {
        rootsHeard_ = session_->roots().messageCount();
        if (!session_->roots().message().empty()) emit message(QString::fromStdString(session_->roots().message()));
    }
    fillLevels();
}

// ---- The tube hierarchy ------------------------------------------------------------------------------

std::string TubePanel::levelPicked() const {
    const std::string group = session_ ? session_->settings().brush.group : std::string();
    return group.empty() ? levelBase_ : group;
}

void TubePanel::fillLevels() {
    const HierarchyPtr h = document() ? document()->session().authoredHierarchy() : nullptr;
    TubeLevels info;
    if (h) {
        try {
            info = tubeLevels(*h, {}, levelPicked());
        } catch (const std::exception&) {
        }
    }
    levelBase_ = info.base;
    const std::string picked = session_ ? session_->settings().brush.group : std::string();
    const auto at = std::find(info.levels.begin(), info.levels.end(), picked);
    shownLevel_ = at != info.levels.end() ? static_cast<int>(at - info.levels.begin()) + 1 : 1;
    levelBaseLabel_->setText(levelBase_.empty() ? tr("No guides yet.") : tr("Base: %1").arg(QString::fromStdString(levelBase_)));
    addLevel_->setEnabled(!levelBase_.empty() && info.levels.size() < static_cast<std::size_t>(kMaxTubeLevels));
    removeLevel_->setEnabled(info.levels.size() > 1);
    const std::size_t count = std::max<std::size_t>(info.levels.size(), 1);
    if (info.levels != levels_ || levelButtons_.size() != count) {
        levels_ = info.levels;
        for (QAbstractButton* b : levelButtons_) b->deleteLater();
        levelButtons_.clear();
        while (QLayoutItem* item = levelRow_->takeAt(0)) delete item;
        for (std::size_t k = 0; k < count; ++k) {
            QToolButton* b = makeChip(QString::number(k + 1),
                                      tr("Level %1%2: show its tubes and edit them").arg(k + 1).arg(k == 0 ? tr(" (the base)") : QString()),
                                      levelsBlock_);
            b->setObjectName(QStringLiteral("tubeLevel%1").arg(k + 1));
            b->setFixedWidth(30);
            const int level = static_cast<int>(k + 1);
            connect(b, &QToolButton::clicked, this, [this, level] { showLevel(level); });
            levelRow_->addWidget(b);
            levelButtons_.push_back(b);
        }
        levelRow_->addStretch(1);
    }
    for (std::size_t k = 0; k < levelButtons_.size(); ++k) {
        QSignalBlocker block(levelButtons_[k]);
        levelButtons_[k]->setChecked(static_cast<int>(k + 1) == shownLevel_);
    }
}

void TubePanel::showLevel(int level) {
    GroomDocument* doc = document();
    const HierarchyPtr h = doc ? doc->session().authoredHierarchy() : nullptr;
    if (!h || levelBase_.empty()) return;
    std::string picked;
    try {
        for (const auto& [group, visible] : tubeLevelVisibility(*h, levelBase_, level)) {
            doc->setGroupHidden(group, !visible);
            if (visible) picked = group;
        }
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return;
    }
    if (!picked.empty() && session_) {
        BrushSettings& b = session_->settings().brush;
        b.group = picked;
        const GroupId id = h->findGroup(picked);
        if (id != kNoGroup) b.level = h->groupDepth(id);
    }
    fillLevels();
    emit message(tr("Level %1: its tubes shown; the Tube tool and brushes edit them.").arg(level));
    emit guidesEdited();  // (the groups' eyes and the brush's group: the host shows them)
    emit settingsChanged();
}

void TubePanel::addLevel() {
    GroomDocument* doc = document();
    if (!doc) return;
    const std::vector<GuideId> selected = doc->selection();
    const std::string picked = levelPicked();
    std::size_t split = 0;
    const MeshPtr scalp = doc->session().scalp();
    const std::string error = doc->editGuides("tube level add", [&](GuideHierarchy& g) {
        split = addTubeLevel(g, scalp.get(), selected, picked);
    });
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit guidesEdited();
    fillLevels();
    showLevel(static_cast<int>(levels_.size()));
    emit message(tr("Split %1 tube(s): %2 has %3 levels; level %3 shown.")
                     .arg(split).arg(QString::fromStdString(levelBase_)).arg(levels_.size()));
}

void TubePanel::removeLevel() {
    GroomDocument* doc = document();
    if (!doc || levelBase_.empty()) return;
    const std::vector<GuideId> selected = doc->selection();
    const std::string base = levelBase_;
    const MeshPtr scalp = doc->session().scalp();
    const std::string error = doc->editGuides("tube level remove", [&](GuideHierarchy& g) {
        removeTubeLevel(g, scalp.get(), base, selected);
    });
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit guidesEdited();
    fillLevels();
    showLevel(static_cast<int>(levels_.size()));
}

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
    refreshRoots();  // (the root buttons as the tool stands; the levels)
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
