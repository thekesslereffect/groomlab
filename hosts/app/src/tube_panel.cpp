// SPDX-License-Identifier: Apache-2.0
#include "tube_panel.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QIcon>
#include <QSize>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "groomlab/brush.h"
#include "groomlab/editor/guide_edits.h"
#include "groomlab/root_subdivision.h"
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

// Two widgets on a row: the lead at the left at its own width, the trail at the right, the space
// between them free. When the lead grows into the trail (more level buttons than fit), the trail
// goes down to a row of its own, still on the right (Maya's root_tube_card.LeadTrailLayout).
class LeadTrailLayout : public QLayout {
public:
    LeadTrailLayout(QWidget* lead, QWidget* trail, QWidget* parent) : QLayout(parent) {
        setContentsMargins(0, 0, 0, 0);
        addWidget(lead);
        addWidget(trail);
    }
    ~LeadTrailLayout() override {
        for (QLayoutItem* item : items_) delete item;
    }
    void addItem(QLayoutItem* item) override { items_.push_back(item); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem* itemAt(int index) const override {
        return index >= 0 && index < count() ? items_[static_cast<std::size_t>(index)] : nullptr;
    }
    QLayoutItem* takeAt(int index) override {
        if (index < 0 || index >= count()) return nullptr;
        QLayoutItem* item = items_[static_cast<std::size_t>(index)];
        items_.erase(items_.begin() + index);
        return item;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return place(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override {
        QSize size;
        for (QLayoutItem* item : items_) size = size.expandedTo(item->minimumSize());
        return size;
    }
    QSize sizeHint() const override { return minimumSize(); }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        place(rect, true);
    }

private:
    int place(const QRect& rect, bool apply) const {
        if (items_.size() < 2) return 0;
        const QSize lead = items_[0]->sizeHint(), trail = items_[1]->sizeHint();
        const int gap = 8, below = 4;
        const bool oneRow = lead.width() + gap + trail.width() <= rect.width();
        if (apply) {
            items_[0]->setGeometry(QRect(rect.x(), rect.y(), std::min(lead.width(), rect.width()), lead.height()));
            const int y = oneRow ? rect.y() : rect.y() + lead.height() + below;
            items_[1]->setGeometry(QRect(rect.right() + 1 - trail.width(), y, trail.width(), trail.height()));
        }
        return oneRow ? std::max(lead.height(), trail.height()) : lead.height() + below + trail.height();
    }
    std::vector<QLayoutItem*> items_;
};

}  // namespace


namespace {

using nlohmann::json;

constexpr double kPi = 3.14159265358979;
constexpr int kPreviewMs = 60;  // a drag's live preview at most this often
constexpr const char* kDrawn = "@drawn";  // the root picker's "Drawn root" entry (no shape is named so)

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

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

    // ---- Edit (the Tube brush): the Profile block (untitled, as Maya's palette.py): Root, Mid and
    // Tip as tiles with their outlines (the one picked shows in the editor); the editor beside the
    // picked profile's shape, Apply, Hold and the outline's Mirror / Smooth; a note, a hint and the
    // library's ⋯ under them.
    profileBlock_ = new CardBlock(tr("Profile"), this);
    profileBlock_->setObjectName(QStringLiteral("profileBlock"));
    profileBlock_->titleLabel()->hide();
    profileBlock_->setToolTip(tr("The tube's cross-section: its root, mid and tip profiles (the outline changes smoothly between "
                                 "them) and the groom's shape library"));
    auto glyph = [this](const char* icon, const char* name, const QString& tip, void (TubePanel::*run)()) {
        // The library's actions: kept as buttons (scripts and tests click them), shown in the ⋯ menu.
        QToolButton* b = makeGlyph(QLatin1String(icon), tip, profileBlock_);
        b->setObjectName(QLatin1String(name));
        connect(b, &QToolButton::clicked, this, [this, run] { (this->*run)(); });
        b->hide();
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
        // The tubes' look in the viewport (Maya's guides display's Tubes block): shaded surfaces,
        // wires, colors. Not on the card: how things draw lives with the other display settings,
        // under the Groom Tools' guides arrow (displayBlock; the Simple layout).
        display_ = new QWidget(this);
        display_->setObjectName(QStringLiteral("tubeDisplay"));
        auto* displayRows = new QVBoxLayout(display_);
        displayRows->setContentsMargins(0, 0, 0, 0);
        displayRows->setSpacing(4);
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
        levelColors_ = new QCheckBox(tr("Level colors"), profileBlock_);
        levelColors_->setObjectName(QStringLiteral("tubeLevelColors"));
        levelColors_->setToolTip(tr("Each tube of a tube hierarchy's levels (a split tube) in a shade of its own on top of its root "
                                    "tube's color, so the tubes of a level tell apart"));
        // As pills (the Simple layout's Chip, as the popup's other display toggles), each mirroring
        // its box (the boxes carry the settings, hidden).
        FlowLayout* pills = nullptr;
        QWidget* pillsRow = pillRow(display_, &pills);
        pillsRow->setObjectName(QStringLiteral("tubeDisplayPills"));
        for (QCheckBox* box : {shaded_, wireframe_, colors_, levelColors_}) {
            connect(box, &QCheckBox::toggled, this, [this] { emit displayChanged(); });
            box->hide();
            QToolButton* pill = makePill(box->text(), box->toolTip(), pillsRow);
            pill->setObjectName(box->objectName() + QStringLiteral("Pill"));
            pill->setChecked(box->isChecked());
            connect(pill, &QToolButton::toggled, box, [box](bool on) {
                if (box->isChecked() != on) box->setChecked(on);
            });
            connect(box, &QCheckBox::toggled, pill, [pill](bool on) {
                const QSignalBlocker block(pill);
                pill->setChecked(on);
            });
            pills->addWidget(pill);
        }
        displayRows->addWidget(pillsRow);
        display_->hide();  // (until the Groom Tools take it: displayBlock)
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
        // Root, Mid, Tip: tiles with their outline (refreshTiles), one at a time in the editor.
        auto* tiles = new QHBoxLayout();
        tiles->setContentsMargins(0, 0, 0, 0);
        tiles->setSpacing(4);
        profileBlock_->rows()->addLayout(tiles);
        // The editor (132 px square) and beside it the picked profile's page.
        auto* body = new QHBoxLayout();
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(8);
        editor_ = new TubeShapeEditor(profileBlock_);
        editor_->setFixedSize(132, 132);
        connect(editor_, &TubeShapeEditor::dragged, this, [this](const std::vector<SectionPoint>&) {
            if (!previewTimer_->isActive()) previewTimer_->start();
        });
        connect(editor_, &TubeShapeEditor::committed, this, [this](const std::vector<SectionPoint>& points) { shapeCommitted(points); });
        body->addWidget(editor_, 0, Qt::AlignTop);
        auto* side = new QVBoxLayout();
        side->setContentsMargins(0, 0, 0, 0);
        side->setSpacing(4);
        body->addLayout(side, 1);
        profileBlock_->rows()->addLayout(body);
        static const char* const kLabels[3] = {"Root", "Mid", "Tip"};
        static const char* const kApplyNames[3] = {"tubeShapeApply", "tubeMidShapeApply", "tubeTipShapeApply"};
        static const char* const kHoldNames[3] = {"tubeRootLength", "tubeMidLength", "tubeTipLength"};
        for (int k = 0; k < 3; ++k) {
            QToolButton* which = makePill(tr(kLabels[k]),
                                          tr("Show the %1 profile's shape in the editor (a library shape there is edited for every "
                                             "guide taking it)")
                                              .arg(tr(kLabels[k]).toLower()),
                                          profileBlock_);
            which->setObjectName(QStringLiteral("tubeProfile") + QLatin1String(kLabels[k]));
            which->setProperty("tile", true);
            which->setAutoExclusive(true);
            which->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            which->setIconSize(QSize(22, 22));
            which->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            which->setFixedHeight(48);
            which->setChecked(k == 0);
            connect(which, &QToolButton::clicked, this, [this, k] { setActiveProfile(k); });
            profileButtons_[static_cast<std::size_t>(k)] = which;
            tiles->addWidget(which, 1);
            auto* page = new QWidget(profileBlock_);
            page->setObjectName(QStringLiteral("tubeProfilePage") + QLatin1String(kLabels[k]));
            auto* pageColumn = new QVBoxLayout(page);
            pageColumn->setContentsMargins(0, 0, 0, 0);
            pageColumn->setSpacing(4);
            if (k == 0) editPicker_ = pageColumn;  // (setMode puts the root's picker here, over Apply)
            else pageColumn->addWidget(k == 1 ? mid_ : tip_);
            QPushButton* apply = new QPushButton(tr("Apply"), page);
            apply->setObjectName(QLatin1String(kApplyNames[k]));
            apply->setToolTip(tr("Give the selected guides this %1 profile and its Hold").arg(tr(kLabels[k]).toLower()));
            connect(apply, &QPushButton::clicked, this, [this, k] { applyProfile(k); });
            pageColumn->addWidget(apply);
            profileApply_[static_cast<std::size_t>(k)] = apply;
            if (k == 0) apply_ = apply;
            ValueSlider* hold = new ValueSlider(page);
            hold->setObjectName(QLatin1String(kHoldNames[k]));
            hold->setRange(0.0, 100.0);
            hold->setLimits(0.0, 100.0);
            hold->setDecimals(0);
            hold->setFill(theme::sliderFill(theme::blue()));
            hold->setCaption(tr("Hold"));
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
            pageColumn->addWidget(hold);
            page->setVisible(k == 0);
            profilePages_[static_cast<std::size_t>(k)] = page;
            side->addWidget(page);
        }
        // The outline's options: a library shape's (a built-in is read-only), as pills.
        FlowLayout* optionPills = nullptr;
        shapeOptions_ = pillRow(profileBlock_, &optionPills);
        shapeOptions_->setObjectName(QStringLiteral("tubeShapeOptions"));
        mirror_ = makePill(tr("Mirror"),
                           tr("Keep the outline's left and right sides alike (about the up axis): a point and its mirror image move, come "
                              "and go together"),
                           shapeOptions_);
        mirror_->setObjectName(QStringLiteral("tubeShapeMirror"));
        mirror_->setChecked(true);
        connect(mirror_, &QToolButton::toggled, this, [this](bool on) {
            if (!updating_) setMirror(on);
        });
        shapeSmooth_ = makePill(tr("Smooth"), tr("A smooth curve through the points; off: straight edges and sharp corners"), shapeOptions_);
        shapeSmooth_->setObjectName(QStringLiteral("tubeShapeSmooth"));
        shapeSmooth_->setChecked(true);
        connect(shapeSmooth_, &QToolButton::toggled, this, [this](bool on) {
            if (!updating_) setShapeSmooth(on);
        });
        optionPills->addWidget(mirror_);
        optionPills->addWidget(shapeSmooth_);
        side->addWidget(shapeOptions_);
        side->addStretch(1);
        // What the targets keep (the core's tube targets and editTubes' note): root tubes their
        // drawn root, split levels their own Mid and Tip.
        profileNote_ = new QLabel(profileBlock_);
        profileNote_->setObjectName(QStringLiteral("tubeProfileNote"));
        profileNote_->setProperty("role", "hint");
        profileNote_->setWordWrap(true);
        profileNote_->hide();
        profileBlock_->rows()->addWidget(profileNote_);
        // A hint, and the library's ⋯ (Rename..., New Shape, Duplicate, Delete Shape in red).
        auto* foot = new QHBoxLayout();
        foot->setContentsMargins(0, 0, 0, 0);
        foot->setSpacing(4);
        shapeHint_ = new QLabel(profileBlock_);
        shapeHint_->setObjectName(QStringLiteral("tubeShapeHint"));
        shapeHint_->setProperty("role", "hint");
        shapeHint_->setWordWrap(true);
        foot->addWidget(shapeHint_, 1);
        libraryMenu_ = rowMenuButton(tr("The shape library: rename, new, duplicate, delete"), [this](QMenu* menu) { fillLibraryMenu(menu); },
                                     nullptr, profileBlock_);
        libraryMenu_->setObjectName(QStringLiteral("tubeShapeLibrary"));
        foot->addWidget(libraryMenu_, 0, Qt::AlignTop);
        profileBlock_->rows()->addLayout(foot);
        renameField_ = new QLineEdit(profileBlock_);
        renameField_->setObjectName(QStringLiteral("tubeShapeName"));
        renameField_->setPlaceholderText(tr("New name, then Enter (Esc: keep it)"));
        connect(renameField_, &QLineEdit::returnPressed, this, [this] { finishRename(); });
        renameField_->installEventFilter(this);
        renameField_->hide();
        profileBlock_->rows()->addWidget(renameField_);
    }
    // ---- Tube roots (Roots: the Roots brush; untitled, the card's header names it) and the tube
    // hierarchy (the Tube and Roots brushes).
    rootsBlock_ = new CardBlock(tr("Tube roots"), this);
    rootsBlock_->setObjectName(QStringLiteral("rootTubes"));
    rootsBlock_->titleLabel()->hide();
    rootsBlock_->setToolTip(tr("Tubes drawn root first: click the root's outline on the scalp, one CV at a time, and close it on "
                               "its first CV. Each tube is its own region."));
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
                              "close it. Near a neighbouring root a CV snaps onto it, sealing the tubes. Again: the outline dropped"),
                           true, &TubePanel::drawRoot);
        editRoots_ = button(tr("Edit roots"), "rootTubeEdit",
                            tr("Show the root tubes' outlines with their CVs, the guides hidden; drag CVs on the scalp (Ctrl+click an "
                               "edge: a new CV there; a CV: taken out), then Finish edit"),
                            true, &TubePanel::editRoots);
        selectRoot_ = button(tr("Select root"), "rootTubeSelect",
                             tr("Show every root's middle: click one to select its root (Shift: several), drag it to move the root "
                                "over the scalp. Again: back to Draw root"),
                             true, &TubePanel::selectRoot);
        undoCv_ = button(tr("Undo CV"), "rootTubeUndoCv", tr("Take back the last CV (or the drawn path); in Edit roots, the last CV move"),
                         false, &TubePanel::undoCv);
        deleteRoots_ = button(tr("Delete"), "rootTubeDelete", tr("Delete the selected roots: their tubes and the tube hierarchy under them"),
                              false, &TubePanel::deleteRoots);
        duplicateRoots_ = button(tr("Duplicate"), "rootTubeDuplicate",
                                 tr("Copy the selected roots: the copies follow the cursor over the scalp, a click sets them down. "
                                    "Again: stop"),
                                 true, &TubePanel::duplicateRoots);
        subdivideRoots_ = button(tr("Subdivide"), "rootTubeSubdivide",
                                 tr("Cut the selected roots into Voronoi cells, each a root tube in the level below following the old "
                                    "tube, by the density map (tubeSubdivisionDensity, made at 0.5 the first time) and the "
                                    "multiplier. A cell cut again is a destructive cut: the map and Apply leave it be"),
                                 false, &TubePanel::subdivideRoots);
        paintDensity_ = button(QString(), "rootTubePaintDensity",
                               tr("Paint subdivision density: the density map on the scalp (Subdivide makes it): white, more and "
                                  "smaller roots; black, fewer and larger. Again, or another tool, stops painting; Apply cuts by it"),
                               true, &TubePanel::paintDensity);
        {
            // (Greyed, not gone, without a density map yet: Qt's own disabled tint all but hides it.)
            const QString brush = QStringLiteral(":/groomlab/icons/groomLab_paint.svg");
            const qreal dpr = paintDensity_->devicePixelRatioF();
            // (The Paint tile's two-tone artwork, both tones the dim text.)
            const QPixmap art = duotoneIcon(brush, theme::textDim(), theme::textDim(), 18, dpr);
            QIcon icon(art);
            icon.addPixmap(duotoneIcon(brush, theme::textMuted(), theme::textMuted(), 18, dpr), QIcon::Disabled);
            paintDensity_->setIcon(icon);
        }
        paintDensity_->setIconSize(QSize(18, 18));
        applyDensity_ = button(tr("Apply"), "rootTubeApplyDensity",
                               tr("Cut the subdivided roots again all over, by the density map and the multiplier as they are now (the "
                                  "cells' grooming and tube hierarchy kept)"),
                               false, &TubePanel::applyDensity);
        multiplier_ = committingSlider(0.25, 4.0, 0.05, 20.0, 2, &TubePanel::setMultiplier);
        multiplier_->setObjectName(QStringLiteral("rootSubdivisionMultiplier"));
        multiplier_->setCaption(tr("Multiplier"));
        multiplier_->setValueWidth(22);  // (a narrow field: its name whole)
        multiplier_->setValue(1.0);
        multiplier_->setToolTip(tr("Subdivision multiplier: how dense the cuts are overall, with the painted density: 1 as painted, "
                                   "2 about twice the roots (each smaller), 0.5 half. Apply cuts again at it"));
        // Three to a row, sharing it however narrow the card (Maya's): the modes; Undo CV, Delete,
        // Duplicate.
        for (const std::vector<QAbstractButton*>& row : {std::vector<QAbstractButton*>{drawRoot_, editRoots_, selectRoot_},
                                                         std::vector<QAbstractButton*>{undoCv_, deleteRoots_, duplicateRoots_}}) {
            auto* line = new QWidget(rootsBlock_);
            auto* lineLayout = new QHBoxLayout(line);
            lineLayout->setContentsMargins(0, 0, 0, 0);
            lineLayout->setSpacing(4);
            for (QAbstractButton* b : row) {
                b->setSizePolicy(QSizePolicy::Ignored, b->sizePolicy().verticalPolicy());
                b->setMinimumWidth(40);
                lineLayout->addWidget(b, 1);
            }
            rootsBlock_->rows()->addWidget(line);
        }
        // Subdivide | the paint brush | Multiplier | Apply: one row however narrow the card,
        // Subdivide and Apply their names' width, the brush a square, Multiplier the rest.
        auto* cutRow = new QWidget(rootsBlock_);
        auto* cutLayout = new QHBoxLayout(cutRow);
        cutLayout->setContentsMargins(0, 0, 0, 0);
        cutLayout->setSpacing(4);
        for (QAbstractButton* b : {subdivideRoots_, applyDensity_}) {
            b->setSizePolicy(QSizePolicy::Fixed, b->sizePolicy().verticalPolicy());
            b->setFixedWidth(QFontMetrics(theme::uiFont(theme::fontSize())).horizontalAdvance(b->text()) + 26);  // (the style sheet's font and padding)
        }
        paintDensity_->setSizePolicy(QSizePolicy::Fixed, paintDensity_->sizePolicy().verticalPolicy());
        paintDensity_->setFixedWidth(28);
        if (auto* tool = qobject_cast<QToolButton*>(paintDensity_)) tool->setToolButtonStyle(Qt::ToolButtonIconOnly);  // (a chip: text only)
        multiplier_->setSizePolicy(QSizePolicy::Ignored, multiplier_->sizePolicy().verticalPolicy());
        multiplier_->setMinimumWidth(40);
        cutLayout->addWidget(subdivideRoots_);
        cutLayout->addWidget(paintDensity_);
        cutLayout->addWidget(multiplier_, 1);
        cutLayout->addWidget(applyDensity_);
        rootsBlock_->rows()->addWidget(cutRow);
        // Snap and Draw guide: pills (the mock's), as the brushes' toggles.
        FlowLayout* rootPills = nullptr;
        QWidget* rootPillRow = pillRow(rootsBlock_, &rootPills);
        rootPillRow->setObjectName(QStringLiteral("rootTubePills"));
        rootSnap_ = makePill(tr("Snap to neighbours"), tr("A CV near a neighbouring root's CV or edge snaps onto it, so the tubes meet "
                                                         "with no gap"),
                             rootPillRow);
        rootSnap_->setObjectName(QStringLiteral("rootTubeSnap"));
        rootSnap_->setChecked(true);
        connect(rootSnap_, &QToolButton::toggled, this, &TubePanel::setRootSnap);
        rootDrawPath_ = makePill(tr("Draw guide"), tr("Draw guide before extrude: once the outline closes, drag from its middle out to "
                                                      "draw the tube's guide. Off: the tube goes straight off the scalp, Length long, in "
                                                      "Segments"),
                                 rootPillRow);
        rootDrawPath_->setObjectName(QStringLiteral("rootTubeDrawFirst"));
        rootDrawPath_->setChecked(true);
        connect(rootDrawPath_, &QToolButton::toggled, this, &TubePanel::setRootDrawPath);
        rootPills->addWidget(rootSnap_);
        rootPills->addWidget(rootDrawPath_);
        rootsBlock_->rows()->addWidget(rootPillRow);
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
    // The tube hierarchy: one untitled row under the card's header (Level 1 2 ... + - ⋯; Lock
    // view, Mixed view and Colors per level in the ⋯ menu), the Tube and Roots brushes' both.
    levelsBlock_ = new CardBlock(tr("Tube hierarchy"), this);
    levelsBlock_->setObjectName(QStringLiteral("tubeHierarchy"));
    levelsBlock_->titleLabel()->hide();
    const QString levelsTip = tr("Tube levels: each level splits the tubes above it in two. Shaping a tube carries the smaller tubes "
                                 "inside it; pick a level to edit its tubes");
    levelsBlock_->setToolTip(levelsTip);
    {
        addLevel_ = new QPushButton(QStringLiteral("+"), levelsBlock_);
        addLevel_->setObjectName(QStringLiteral("tubeLevelAdd"));
        addLevel_->setToolTip(tr("Split each selected tube in two (with none selected: every tube of the base group), sharing its "
                                 "root shape: a level deeper"));
        connect(addLevel_, &QPushButton::clicked, this, [this] { addLevel(); });
        removeLevel_ = new QPushButton(QString(QChar(0x2212)), levelsBlock_);
        removeLevel_->setObjectName(QStringLiteral("tubeLevelRemove"));
        removeLevel_->setToolTip(tr("Undo a split: with tubes selected, theirs only (a split tube's deepest split tubes, or a split "
                                    "tube back into its parent); with none, the deepest level everywhere"));
        connect(removeLevel_, &QPushButton::clicked, this, [this] { removeLevel(); });
        for (QPushButton* b : {addLevel_, removeLevel_}) {
            QFont font = b->font();
            font.setBold(true);
            b->setFont(font);
            b->setFixedWidth(30);
        }
        // One row: "Level" and the level buttons on the left (fillLevels fills them), + - ⋯ on the
        // right; levels reaching them push them down to a row of their own.
        levelLine_ = new QWidget(levelsBlock_);
        auto* lead = new QWidget(levelLine_);
        auto* leadRow = new QHBoxLayout(lead);
        leadRow->setContentsMargins(0, 0, 0, 0);
        leadRow->setSpacing(6);
        levelLabel_ = new QLabel(tr("Level"), lead);
        levelLabel_->setObjectName(QStringLiteral("tubeLevelLabel"));
        levelLabel_->setProperty("role", "hint");
        levelLabel_->setToolTip(levelsTip);
        leadRow->addWidget(levelLabel_);
        auto* levels = new QWidget(lead);
        levelRow_ = new QHBoxLayout(levels);
        levelRow_->setContentsMargins(0, 0, 0, 0);
        levelRow_->setSpacing(4);
        leadRow->addWidget(levels);
        auto* edits = new QWidget(levelLine_);
        auto* editRow = new QHBoxLayout(edits);
        editRow->setContentsMargins(0, 0, 0, 0);
        editRow->setSpacing(4);
        editRow->addWidget(addLevel_);
        editRow->addWidget(removeLevel_);
        levelMenu_ = rowMenuButton(tr("Level view: Lock view, Mixed view, colors per level"), [this](QMenu* menu) { fillLevelMenu(menu); },
                                   nullptr, edits);
        levelMenu_->setObjectName(QStringLiteral("tubeLevelView"));
        editRow->addWidget(levelMenu_);
        new LeadTrailLayout(lead, edits, levelLine_);
        levelsBlock_->rows()->addWidget(levelLine_);
        // Lock view and Mixed view: the ⋯ menu's (the boxes carry them, not shown).
        lockView_ = new QCheckBox(tr("Lock view"), levelsBlock_);
        lockView_->setObjectName(QStringLiteral("tubeLockView"));
        lockView_->setToolTip(tr("Keep the tubes of the level shown now in view while you pick other levels to work on, whose "
                                 "guides show. Only the tubes' view: the other visibility controls work as they do"));
        connect(lockView_, &QCheckBox::toggled, this, [this](bool on) {
            // (The document's tubeViewGroup: the guides display's Level and the list's tubes too.)
            const bool shown = shownLevel_ > 0 && shownLevel_ <= static_cast<int>(levels_.size());
            setTubeView(on && shown ? levels_[static_cast<std::size_t>(shownLevel_ - 1)] : std::string());
        });
        mixedView_ = new QCheckBox(tr("Mixed view"), levelsBlock_);
        mixedView_->setObjectName(QStringLiteral("tubeMixedView"));
        mixedView_->setToolTip(tr("Working on a level, also show the tubes (not the guides) of the levels above where no level below "
                                  "continues them, and their roots in Select root and Edit roots"));
        mixedView_->setChecked(true);
        // (A level picked: its view again, with or without the levels above' tubes.)
        connect(mixedView_, &QCheckBox::toggled, this, [this] { applyLevelView(session_ ? session_->settings().brush.group : std::string()); });
        lockView_->hide();
        mixedView_->hide();
    }
    column->insertWidget(column->indexOf(rootsBlock_), levelsBlock_);
    column->addWidget(profileBlock_);

    // ---- Edit: the selected tubes' sections (folded: "Selected tubes · N"); Smooth evens out what
    // the brush painted.
    selectedBlock_ = new CardBlock(tr("Selected tubes"), this);
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
        selectedBlock_->setFoldable(true);  // (folded, as the mock: "Selected tubes · 12")
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
    const bool edit = mode != Mode::New;
    if (edit != (mode_ != Mode::New) || placed_) {
        placed_ = false;
        // The picker and the fields move to the mode's block.
        if (edit) {
            editPicker_->insertWidget(0, shape_);  // (the Root page's, over Apply)
            editFields_->addWidget(fields_);
        } else {
            newPicker_->insertWidget(0, shape_);
            newFields_->addWidget(fields_);
        }
        shape_->show();
        fields_->show();
    }
    mode_ = mode;
    // New: the Tube block; Edit (the Tube brush): Profile and Selected tubes; Roots (the Roots
    // brush): the root block; the levels' row with both brushes.
    tubeBlock_->setVisible(mode == Mode::New);
    profileBlock_->setVisible(mode == Mode::Edit);
    selectedBlock_->setVisible(mode == Mode::Edit);
    rootsBlock_->setVisible(mode == Mode::Roots);
    levelsBlock_->setVisible(mode != Mode::New);
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

QAbstractButton* TubePanel::shadedBox() const { return shaded_; }
bool TubePanel::shaded() const { return shaded_ && shaded_->isChecked(); }
bool TubePanel::wireframe() const { return wireframe_ && wireframe_->isChecked(); }
bool TubePanel::colors() const { return colors_ && colors_->isChecked(); }
bool TubePanel::levelColors() const { return levelColors_ && levelColors_->isChecked(); }

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
    // New roots go into the picked group when it is a top group, else rootTubes (the core's rule).
    s.group = session_->settings().brush.group;
    rootsDone(session_->startRoots(mode));
}

bool TubePanel::rootsDone(bool ok) {
    if (!ok && session_) emit message(QString::fromStdString(session_->lastError()));
    refreshRoots();
    emit rootModeChanged();
    return ok;
}

void TubePanel::startRootMode(RootTubeTool::Mode mode) {
    if (mode == RootTubeTool::Mode::Place || mode == RootTubeTool::Mode::Draw) {
        // A deeper level picked: level 1 shown and picked instead (a new root tube is a level 1 guide).
        fillLevels();
        if (levels_.size() > 1 && shownLevel_ > 1) showLevel(1);
        mode = RootTubeTool::Mode::Place;
    }
    startRoots(mode);
}

void TubePanel::drawRoot() {
    // Again (Draw root on): the outline being drawn dropped, Draw root again (the core's start
    // leaves the one running first).
    startRootMode(RootTubeTool::Mode::Place);
}

void TubePanel::editRoots() {
    if (session_ && session_->rootMode() && session_->roots().mode() == RootTubeTool::Mode::Edit) {
        // Finish edit: the edits given to the roots (one undo step), Draw root again.
        const bool ok = session_->editRoots("roots", [](RootTubeTool& t, StrokeHost& h) { t.finishEdit(h); });
        emit guidesEdited();
        if (!ok) {
            rootsDone(false);
            return;
        }
        startRootMode(RootTubeTool::Mode::Place);
        return;
    }
    startRoots(RootTubeTool::Mode::Edit);
}

void TubePanel::selectRoot() {
    if (session_ && session_->rootMode() && (session_->roots().mode() == RootTubeTool::Mode::Select ||
                                             session_->roots().mode() == RootTubeTool::Mode::Duplicate)) {
        startRootMode(RootTubeTool::Mode::Place);  // again: back to Draw root
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

namespace {

// The density map Subdivide, Paint and Apply use: the one map, else an older numbered one
// (subdivisionDensityMapIn, every host's); "" when the groom has none.
std::string densityMapOf(const GroomDocument& doc) {
    std::vector<std::string> names;
    for (const PaintedMap& m : doc.maps()) names.push_back(m.name);
    return subdivisionDensityMapIn(names);
}

}  // namespace

void TubePanel::subdivideRoots() {
    GroomDocument* doc = document();
    if (!doc || !session_ || !session_->rootMode()) return;
    const std::vector<GuideId> roots = session_->roots().selected();
    if (roots.empty()) {
        emit message(tr("Select a root first (Select root), then Subdivide."));
        return;
    }
    // The density map, made at 0.5 the first time (paint it, then Apply).
    std::string map = densityMapOf(*doc);
    if (map.empty()) {
        const std::string error = doc->createMap(kSubdivisionDensityMap, "vertices", false, kDefaultSubdivisionDensity);
        if (!error.empty()) {
            emit message(QString::fromStdString(error));
            return;
        }
        map = kSubdivisionDensityMap;
    }
    const MeshPtr scalp = doc->session().scalp();  // (with the painted maps on it)
    RootSubdivisionOptions options;
    options.map = map;
    options.multiplier = static_cast<float>(multiplier_->value());
    SubdivisionResult done;
    const std::string error = doc->editGuides("subdivide root", [&](GuideHierarchy& g) {
        if (scalp) done = subdivideRootTubes(g, *scalp, roots, options);
    });
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit guidesEdited();
    // (The cells are in the level below the root cut: that level shown, so they are seen.)
    if (done.cellLevel > 0) {
        fillLevels();
        showLevel(done.cellLevel);
    }
    rootsDone(session_->editRoots("roots", [](RootTubeTool& t, StrokeHost& h) { t.sync(h); }));
    emit message(tr("Subdivided: %1 root(s) made.").arg(done.cells.size()));
}

void TubePanel::paintDensity() {
    GroomDocument* doc = document();
    if (!doc) return;
    if (paintingDensity_) {  // Finish painting: the Tube tool again; Apply cuts by the map
        paintingDensity_ = false;
        emit paintDensityFinished();
        refreshRoots();
        emit message(tr("Painted the density map: Apply cuts its roots by it."));
        return;
    }
    const std::string map = densityMapOf(*doc);
    if (map.empty()) {
        emit message(tr("No density map yet: Subdivide a root first (it makes the map)."));
        refreshRoots();
        return;
    }
    if (session_ && session_->rootMode()) session_->leaveRoots();
    paintingDensity_ = true;
    emit paintDensityRequested(map);
    rootsDone(true);
    emit message(tr("Painting the density map (white: more, smaller roots; Ctrl: toward black). The brush again (or another "
                    "tool) stops painting; then Apply to cut by it."));
}

void TubePanel::applyDensity() {
    GroomDocument* doc = document();
    if (!doc) return;
    const std::string map = densityMapOf(*doc);
    if (map.empty()) {
        emit message(tr("No density map yet: Subdivide a root first."));
        return;
    }
    const MeshPtr scalp = doc->session().scalp();
    RootSubdivisionOptions options;
    options.map = map;
    options.multiplier = static_cast<float>(multiplier_->value());
    options.force = true;
    SubdivisionResult done;
    const std::string error = doc->editGuides("apply root density", [&](GuideHierarchy& g) {
        if (scalp) done = applySubdivisionDensity(g, *scalp, {}, options);
    });
    refreshRoots();
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit guidesEdited();
    emit message(tr("Applied: %1 root(s) made%2.").arg(done.cells.size()).arg(done.kept ? tr(", %1 kept").arg(done.kept) : QString()));
}

void TubePanel::applyLevelView(const std::string& group) {
    GroomDocument* doc = document();
    if (!doc) return;
    std::string mixed;
    const HierarchyPtr h = group.empty() ? nullptr : doc->session().authoredHierarchy();
    if (h && h->findGroup(group) != kNoGroup) {
        TubeLevels info;
        try {
            info = tubeLevels(*h, {}, group);
        } catch (const std::exception&) {
        }
        const auto at = std::find(info.levels.begin(), info.levels.end(), group);
        if (info.levels.size() > 1 && at != info.levels.end()) {
            mixed = info.base + ":" + std::to_string(at - info.levels.begin() + 1) + (mixedView_->isChecked() ? "" : ":solo");
        }
    }
    if (mixed == doc->mixedView()) return;
    doc->setTubeViews(doc->tubeViewGroup(), mixed);  // (the level kept in view stays: setTubeView's)
    if (session_ && session_->rootMode()) session_->refreshRoots();
    emit displayChanged();
}

void TubePanel::setTubeView(const std::string& group) {
    GroomDocument* doc = document();
    if (!doc) {
        syncLock();
        return;
    }
    const bool changed = group != doc->tubeViewGroup();
    if (changed) {
        doc->setTubeViews(group, doc->mixedView());
        if (session_ && session_->rootMode()) session_->refreshRoots();
    }
    syncLock();
    if (!changed) return;
    const int level = group.empty() ? 0 : tubeLevelOf(group);
    emit message(group.empty() ? tr("Tubes follow the guides.")
                 : level      ? tr("Tubes of level %1 kept in view.").arg(level)
                              : tr("Tubes of %1 kept in view.").arg(QString::fromStdString(group)));
    emit tubeViewChanged(QString::fromStdString(group));
    emit displayChanged();
}

void TubePanel::syncLock() {
    const GroomDocument* doc = document();
    const QSignalBlocker block(lockView_);
    lockView_->setChecked(doc && !doc->tubeViewGroup().empty());
}

int TubePanel::tubeLevelOf(const std::string& group) const {
    GroomDocument* doc = document();
    const HierarchyPtr h = doc && !group.empty() ? doc->session().authoredHierarchy() : nullptr;
    if (!h || h->findGroup(group) == kNoGroup) return 0;
    TubeLevels info;
    try {
        info = tubeLevels(*h, {}, group);
    } catch (const std::exception&) {
        return 0;
    }
    if (info.levels.size() < 2) return 0;
    const auto at = std::find(info.levels.begin(), info.levels.end(), group);
    return at == info.levels.end() ? 0 : static_cast<int>(at - info.levels.begin()) + 1;
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
    // (The Roots brush picked with its root tool put away: its next click draws a root.)
    const bool waiting = session_ && !on && session_->settings().tool == BrushToolSettings::Tool::Roots;
    const bool placing = waiting || (on && (mode == RootTubeTool::Mode::Place || mode == RootTubeTool::Mode::Draw));
    const bool editing = on && mode == RootTubeTool::Mode::Edit;
    const bool selecting = on && (mode == RootTubeTool::Mode::Select || mode == RootTubeTool::Mode::Duplicate);
    const bool picked = selecting && !session_->roots().selected().empty();
    QSignalBlocker b1(drawRoot_), b2(editRoots_), b3(selectRoot_), b4(duplicateRoots_), b5(paintDensity_);
    drawRoot_->setChecked(placing);
    editRoots_->setChecked(editing);
    editRoots_->setText(editing ? tr("Finish edit") : tr("Edit roots"));
    selectRoot_->setChecked(selecting);
    undoCv_->setEnabled(placing || editing);
    deleteRoots_->setEnabled(picked);
    duplicateRoots_->setEnabled(picked);
    duplicateRoots_->setChecked(on && mode == RootTubeTool::Mode::Duplicate);
    subdivideRoots_->setEnabled(picked);
    paintDensity_->setChecked(paintingDensity_);
    const bool hasMap = document() && !densityMapOf(*document()).empty();
    paintDensity_->setEnabled(hasMap || paintingDensity_);
    applyDensity_->setEnabled(hasMap);
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
    levelLabel_->setToolTip(levelsBlock_->toolTip() + QStringLiteral("\n\n") +
                            (levelBase_.empty() ? tr("No guides yet.") : tr("Base: %1").arg(QString::fromStdString(levelBase_))));
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
            b->setFixedWidth(26);
            const int level = static_cast<int>(k + 1);
            connect(b, &QToolButton::clicked, this, [this, level] { showLevel(level); });
            levelRow_->addWidget(b);
            levelButtons_.push_back(b);
        }
        levelLine_->layout()->invalidate();
    }
    for (std::size_t k = 0; k < levelButtons_.size(); ++k) {
        QSignalBlocker block(levelButtons_[k]);
        levelButtons_[k]->setChecked(static_cast<int>(k + 1) == shownLevel_);
    }
    syncLock();  // (Lock view: the document's tubeViewGroup, set anywhere)
}

void TubePanel::showLevel(int level) {
    GroomDocument* doc = document();
    const HierarchyPtr h = doc ? doc->session().authoredHierarchy() : nullptr;
    if (!h || levelBase_.empty()) return;
    // Its guides alone through the level view (the groups' eyes left as the artist set them).
    std::string picked;
    try {
        for (const auto& [group, visible] : tubeLevelVisibility(*h, levelBase_, level)) {
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
    applyLevelView(picked);
    emit message(tr("Level %1: its tubes shown; the Tube tool and brushes edit them.").arg(level));
    emit guidesEdited();  // (the brush's group: the host shows it)
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
    owned_.clear();
    if (doc) {
        info = doc->tubeTargets();
        for (const GroomDocument::TubeShapeInfo& s : doc->tubeShapes()) {
            if (!s.builtin) library_.push_back({s.name, s.points, s.smooth, s.users});
        }
        // The targets' drawn roots (root tubes: no picker lists them; "Drawn root").
        for (const std::string& name : info.shapes) {
            if (const std::optional<GroomDocument::TubeShapeInfo> s = doc->ownedTubeShape(name)) {
                owned_.push_back({s->name, s->points, s->smooth, s->users});
            }
        }
    }
    rootTargets_ = info.rootTubes;
    splitTargets_ = info.splitTubes;
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
    // Selected tubes · N (folded).
    selectedBlock_->setHeading(targets_.empty() ? tr("Selected tubes") : tr("Selected tubes · %1").arg(targets_.size()));
    if (!info.tube) {
        lastNote_.clear();
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
        const TubeCardValues card = tubeCardValues(tube.width, tube.aspect, tube.twist);  // (every host's)
        reference_ = card.width * info.scale;
        // The choice follows the targets when they (or their shapes) change, and is left alone
        // otherwise (a shape made or picked here while they stay selected).
        std::string key;
        for (const GuideId id : targets_) key += std::to_string(id) + ",";
        key += "|";
        for (const std::string& s : shapes_) key += s + ",";
        for (const std::string& s : mids_) key += "m" + s + ",";
        for (const std::string& s : tips_) key += "t" + s + ",";
        if (key != targetsKey_) {
            lastNote_.clear();  // (other targets: the last edit's note was theirs)
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
        flatness_->setValue(card.flatness);
        twist_->setValue(card.twist * 180.0 / kPi);
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
        return library(name) || owned(name) || std::find(builtins.begin(), builtins.end(), name) != builtins.end();
    };
    if (!known(choice_)) choice_ = "round";  // (a library shape gone: renamed, deleted, another groom)
    if (!midChoice_.empty() && !known(midChoice_)) midChoice_.clear();
    if (!tipChoice_.empty() && !known(tipChoice_)) tipChoice_.clear();
    {
        // The root picker's "Drawn root" (first): there while the targets are root tubes, whose root
        // is their own drawn outline (no library shape: the core's isOwnedTubeShape).
        const QSignalBlocker blocker(shape_);
        const bool drawn = mode_ != Mode::New && !(mixed && active_ == 0) && owned(choice_) != nullptr;
        const int at = shape_->findData(QString::fromLatin1(kDrawn));
        if (drawn && at < 0) {
            shape_->insertItem(0, tr("Drawn root"), QString::fromLatin1(kDrawn));
            shape_->setItemData(0, tr("The root tube's own outline, drawn on the scalp: the Roots brush reshapes it; a Root profile "
                                      "picked goes to the other guides"),
                                Qt::ToolTipRole);
        } else if (!drawn && at >= 0) {
            shape_->removeItem(at);
        }
        if (drawn) shape_->setCurrentIndex(0);
        else shape_->setCurrentIndex(mixed && active_ == 0 ? 0 : std::max(0, shape_->findData(qs(choice_))));
        if (mixed && active_ != 0) shape_->setCurrentIndex(shapes_.size() > 1 ? 0 : std::max(0, shape_->findData(qs(choice_))));
    }
    for (int k = 1; k < 3; ++k) {
        QComboBox* box = k == 1 ? mid_ : tip_;
        const QSignalBlocker quiet(box);
        const bool differ = (k == 1 ? mids_ : tips_).size() > 1 && !targets_.empty();
        box->setCurrentIndex(differ ? 0 : std::max(0, box->findData(qs(k == 1 ? midChoice_ : tipChoice_))));
    }
    const std::string shown = activeChoice().empty() ? choice_ : activeChoice();  // (an unset mid or tip: the root's)
    const LibraryShape* custom = activeChoice().empty() ? nullptr : library(shown);
    if (custom) {
        editor_->setShape(custom->points, custom->smooth, true);
        if (mirror_->isChecked() && editor_->setMirror(true)) {
            const std::vector<SectionPoint> made = editor_->points();
            shapeCommitted(made);  // made symmetric: saved so
            return;
        }
    } else if (const LibraryShape* source = library(shown) ? library(shown) : owned(shown)) {
        // An unset mid or tip (the root's, read-only), or a root tube's drawn root (the Roots brush
        // reshapes it).
        const QString note = activeChoice().empty() ? (active_ == 1 ? tr("Between the root's and the tip's") : tr("The root's again"))
                                                    : tr("Drawn root: the Roots brush reshapes it");
        editor_->setShape(source->points, source->smooth, false, active_ == 0 && !owned(shown) ? QString() : note);
    } else {
        TubeShape shape = TubeShape::Round;
        parseTubeShape(shown, &shape);
        bool smooth = true;
        const std::vector<SectionPoint> points = builtinTubeShapePoints(shape, &smooth);
        const QString note = activeChoice().empty() ? (active_ == 1 ? tr("Between the root's and the tip's") : tr("The root's again"))
                                                    : tr("Built in: Duplicate it to edit a copy");
        editor_->setShape(points, smooth, false, note);
    }
    {
        // The Tube block's preview (Place, Draw): the placement shape.
        TubeShape shape = TubeShape::Round;
        bool smooth = true;
        if (const LibraryShape* placed = library(choice_)) {
            preview_->setShape(placed->points, placed->smooth, false);
        } else {
            parseTubeShape(choice_, &shape);
            const std::vector<SectionPoint> points = builtinTubeShapePoints(shape, &smooth);
            preview_->setShape(points, smooth, false);
        }
    }
    updating_ = true;
    shapeSmooth_->setChecked(editor_->smooth());
    updating_ = false;
    const bool hasGroom = document() != nullptr;
    shapeOptions_->setVisible(custom != nullptr);
    shapeHint_->setText(custom ? tr("Drag a point to edit; ⋯ for the library") : tr("⋯: duplicate it to edit a copy, or a new shape"));
    new_->setEnabled(hasGroom);
    duplicate_->setEnabled(hasGroom);
    for (QToolButton* b : {rename_, delete_}) b->setEnabled(custom != nullptr);
    for (QPushButton* b : profileApply_) {
        if (b) b->setEnabled(!targets_.empty());
    }
    for (int k = 0; k < 3; ++k) {
        if (QWidget* page = profilePages_[static_cast<std::size_t>(k)]) page->setVisible(k == active_);
    }
    refreshTiles();
    showNotes();
}

const TubePanel::LibraryShape* TubePanel::owned(const std::string& name) const {
    for (const LibraryShape& s : owned_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

namespace {

// A tube profile's outline as a small icon (the Root / Mid / Tip tiles, Maya's palette.shape_icon);
// none: a dash.
QIcon shapeIcon(const std::vector<SectionPoint>* points, int size, qreal dpr) {
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(theme::text());
    pen.setWidthF(size / 16.0);
    p.setPen(pen);
    if (!points || points->empty()) {
        p.drawLine(QPointF(size * 0.35, size * 0.5), QPointF(size * 0.65, size * 0.5));
    } else {
        float reach = 0.0f;
        for (const SectionPoint& q : *points) reach = std::max({reach, std::abs(q.u), std::abs(q.v)});
        if (reach <= 0.0f) reach = 1.0f;
        const double half = size * 0.42;
        QPolygonF poly;
        for (const SectionPoint& q : *points) poly << QPointF(size / 2.0 + q.u / reach * half, size / 2.0 - q.v / reach * half);
        QColor fill = theme::accentInk();
        fill.setAlpha(70);
        p.setBrush(fill);
        p.drawPolygon(poly);
    }
    p.end();
    return QIcon(pixmap);
}

}  // namespace

void TubePanel::refreshTiles() {
    const std::array<const std::string*, 3> names = {&choice_, &midChoice_, &tipChoice_};
    const std::array<bool, 3> differ = {shapes_.size() > 1 && !targets_.empty(), mids_.size() > 1 && !targets_.empty(),
                                        tips_.size() > 1 && !targets_.empty()};
    for (int k = 0; k < 3; ++k) {
        QToolButton* tile = profileButtons_[static_cast<std::size_t>(k)];
        if (!tile) continue;
        {
            const QSignalBlocker quiet(tile);
            tile->setChecked(k == active_);
        }
        const std::string& name = *names[static_cast<std::size_t>(k)];
        std::vector<SectionPoint> points;
        bool has = false;
        if (!name.empty() && !differ[static_cast<std::size_t>(k)]) {
            if (const LibraryShape* s = library(name) ? library(name) : owned(name)) {
                points = s->points;
                has = true;
            } else {
                TubeShape shape = TubeShape::Round;
                if (parseTubeShape(name, &shape)) {
                    bool smooth = true;
                    points = builtinTubeShapePoints(shape, &smooth);
                    has = true;
                }
            }
        }
        tile->setIcon(shapeIcon(has ? &points : nullptr, 22, devicePixelRatioF()));
    }
}

void TubePanel::showNotes() {
    // What the targets keep: root tubes their drawn root (the last edit's note, else the core's
    // words for the targets), split levels their own Mid and Tip.
    QStringList notes;
    if (!targets_.empty()) {
        const std::string kept = !lastNote_.empty() ? lastNote_ : rootTargets_ ? rootTubesKeptNote(rootTargets_) : std::string();
        if (!kept.empty()) notes << qs(kept) + (kept.back() == '.' ? QString() : QStringLiteral("."));
        if (splitTargets_ > 0) notes << tr("Mid and tip don't carry into split levels.");
    }
    profileNote_->setText(notes.join(QLatin1Char(' ')));
    profileNote_->setVisible(!notes.isEmpty());
}

void TubePanel::fillLevelMenu(QMenu* menu) {
    // Lock view, Mixed view and Colors per level, checked as they stand.
    const std::pair<QString, QCheckBox*> entries[] = {{tr("Lock view"), lockView_}, {tr("Mixed view"), mixedView_},
                                                      {tr("Colors per level"), levelColors_}};
    for (const auto& [text, box] : entries) {
        QAction* a = menu->addAction(text);
        a->setObjectName(box->objectName() + QStringLiteral("Action"));
        a->setCheckable(true);
        a->setChecked(box->isChecked());
        a->setToolTip(box->toolTip());
        connect(a, &QAction::toggled, box, &QCheckBox::setChecked);
    }
}

void TubePanel::fillLibraryMenu(QMenu* menu) {
    // Rename first, Delete Shape last in red (picking it is the confirmation), as the buttons stand.
    const std::pair<QString, QToolButton*> entries[] = {{tr("Rename..."), rename_}, {tr("New Shape"), new_}, {tr("Duplicate"), duplicate_}};
    for (const auto& [text, button] : entries) {
        QAction* a = menu->addAction(text, button, &QToolButton::click);
        a->setEnabled(button->isEnabled());
        a->setToolTip(button->toolTip());
    }
    menu->addSeparator();
    QAction* remove = ui::dangerAction(menu, tr("Delete Shape"), [this] {
        armed_ = true;
        deleteShape();
    }, tr("Delete this shape from the library (its guides become round)"));
    remove->setEnabled(delete_->isEnabled());
}

std::string TubePanel::placement() const {
    return json{{"shape", choice_},
                {"width", width_->value() / lengthFactor_},
                {"aspect", tubeAspectOfFlatness(static_cast<float>(flatness_->value()))},
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

void TubePanel::edited(const std::string& error, const std::string& note) {
    emit message(qs(error));
    if (error.empty()) lastNote_ = note;  // (what root tubes kept: the Profile block says it)
    refresh(true);
    if (error.empty()) emit guidesEdited();
}

void TubePanel::pick(const std::string& name) {
    if (name.empty() || name == kDrawn) return;  // "Mixed", "Drawn root": nothing to give
    choice_ = name;
    if (active_ != 0) setActiveProfile(0);
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) {
        std::string note;
        const std::string error = doc->editTubes(targets_, json{{"shape", name}}, "set tube", &note);
        edited(error, note);
    } else {
        showChoice();
    }
    pushPlacement();
}

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
    const std::string& picked = profile == 0 ? choice_ : profile == 1 ? midChoice_ : tipChoice_;
    if (among.size() <= 1 && !owned(picked)) edit[kShape[profile]] = picked;  // (a drawn root is its root tube's own)
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) {
        std::string note;
        const std::string error = doc->editTubes(targets_, edit, "set tube", &note);
        edited(error, note);
    }
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
    if (doc && !targets_.empty()) edited(doc->editTubes(targets_, json{{"aspect", tubeAspectOfFlatness(static_cast<float>(flatness))}}));
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
    std::vector<std::string> library;
    for (const LibraryShape& s : library_) library.push_back(s.name);
    return freeTubeShapeName(base, library);  // every host's (tube.h)
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
