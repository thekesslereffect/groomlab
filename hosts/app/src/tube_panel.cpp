// SPDX-License-Identifier: Apache-2.0
#include "tube_panel.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "groomlab/brush.h"
#include "groomlab/tube.h"

#include "brush_session.h"
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
    column->setContentsMargins(0, 4, 0, 0);
    column->setSpacing(6);
    targetsLabel_ = new QLabel(this);
    targetsLabel_->setObjectName(QStringLiteral("tubeTargets"));
    targetsLabel_->setWordWrap(true);
    targetsLabel_->setProperty("role", "hint");
    column->addWidget(targetsLabel_);

    auto* form = makeForm();
    column->addLayout(form);
    shape_ = new QComboBox(this);
    shape_->setObjectName(QStringLiteral("tubeShape"));
    shape_->setToolTip(tr("The tube shape: picked, it goes to the targeted guides and to the guides Place and Draw make. The "
                          "built-ins, then the groom's library of custom shapes"));
    connect(shape_, &QComboBox::activated, this, [this](int) { pick(shape_->currentData().toString().toStdString()); });
    form->addRow(qs(defs().get("choices/tubeShape").value("label", std::string("Shape"))), shape_);

    // The editor: the chosen shape's outline, and the library's edits.
    editor_ = new TubeShapeEditor(this);
    connect(editor_, &TubeShapeEditor::dragged, this, [this](const std::vector<SectionPoint>&) {
        if (!preview_->isActive()) preview_->start();
    });
    connect(editor_, &TubeShapeEditor::committed, this, [this](const std::vector<SectionPoint>& points) { shapeCommitted(points); });
    column->addWidget(editor_);
    auto* options = new QHBoxLayout();
    options->setSpacing(6);
    editCustom_ = new QPushButton(tr("Edit as Custom"), this);
    editCustom_->setObjectName(QStringLiteral("tubeEditAsCustom"));
    editCustom_->setToolTip(tr("Copy this built-in shape into the groom's library as a new shape you can edit (the targeted guides "
                               "take it)"));
    connect(editCustom_, &QPushButton::clicked, this, [this] { editAsCustom(); });
    mirror_ = new QCheckBox(tr("Mirror"), this);
    mirror_->setObjectName(QStringLiteral("tubeShapeMirror"));
    mirror_->setChecked(true);
    mirror_->setToolTip(tr("Keep the outline's left and right sides alike (about the up axis): a point and its mirror image move, "
                           "come and go together"));
    connect(mirror_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) setMirror(on);
    });
    shapeSmooth_ = new QCheckBox(tr("Smooth"), this);
    shapeSmooth_->setObjectName(QStringLiteral("tubeShapeSmooth"));
    shapeSmooth_->setChecked(true);
    shapeSmooth_->setToolTip(tr("A smooth curve through the points; off: straight edges and sharp corners"));
    connect(shapeSmooth_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) setShapeSmooth(on);
    });
    options->addWidget(editCustom_);
    options->addStretch(1);
    options->addWidget(mirror_);
    options->addWidget(shapeSmooth_);
    column->addLayout(options);
    auto* libraryRow = new QHBoxLayout();
    libraryRow->setSpacing(4);
    auto button = [this, libraryRow](const QString& text, const char* name, const QString& tip, void (TubePanel::*run)()) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QLatin1String(name));
        b->setToolTip(tip);
        b->setMinimumWidth(0);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(b, &QPushButton::clicked, this, [this, run] { (this->*run)(); });
        libraryRow->addWidget(b);
        return b;
    };
    new_ = button(tr("New"), "tubeShapeNew", tr("A new library shape (a circle to start from)"), &TubePanel::newShape);
    rename_ = button(tr("Rename"), "tubeShapeRename", tr("Rename this library shape (its guides keep it)"), &TubePanel::startRename);
    duplicate_ = button(tr("Duplicate"), "tubeShapeDuplicate", tr("A copy of this shape in the library, to change on its own"),
                        &TubePanel::duplicateShape);
    delete_ = button(tr("Delete"), "tubeShapeDelete", tr("Remove this shape from the library: click twice (its guides become round)"),
                     &TubePanel::deleteShape);
    column->addLayout(libraryRow);
    renameField_ = new QLineEdit(this);
    renameField_->setObjectName(QStringLiteral("tubeShapeName"));
    renameField_->setPlaceholderText(tr("New name, then Enter (Esc: keep it)"));
    connect(renameField_, &QLineEdit::returnPressed, this, [this] { finishRename(); });
    renameField_->installEventFilter(this);
    renameField_->hide();
    column->addWidget(renameField_);
    apply_ = new QPushButton(tr("Apply to Selected"), this);
    apply_->setObjectName(QStringLiteral("tubeShapeApply"));
    apply_->setToolTip(tr("Give the targeted guides this shape"));
    connect(apply_, &QPushButton::clicked, this, [this] { applyShape(); });
    column->addWidget(apply_);
    disarm_ = new QTimer(this);
    disarm_->setSingleShot(true);
    disarm_->setInterval(4000);
    connect(disarm_, &QTimer::timeout, this, [this] { disarmDelete(); });
    preview_ = new QTimer(this);
    preview_->setSingleShot(true);
    preview_->setInterval(kPreviewMs);
    connect(preview_, &QTimer::timeout, this, [this] { sendPreview(); });

    // Width, Flatness, Twist: the sections the same root to tip.
    auto* sections = makeForm();
    column->addLayout(sections);
    width_ = committingSlider(0.0, 1.0, 0.0, 1e4, 3, &TubePanel::setWidth);
    width_->setObjectName(QStringLiteral("tubeWidth"));
    width_->setValue(0.1);
    width_->setToolTip(tr("The widest section's half-width (a round tube's radius): the targeted tubes scaled to it, their taper "
                          "and what the brush painted kept; new guides' where none are near"));
    flatness_ = committingSlider(0.0, 0.99, 0.0, 0.99, 2, &TubePanel::setFlatness);
    flatness_->setObjectName(QStringLiteral("tubeFlatness"));
    flatness_->setValue(0.0);
    flatness_->setToolTip(tr("0: as the shape is drawn; toward 1 a flat ribbon (the sections' aspect 1 - this, the same root to tip)"));
    twist_ = committingSlider(-180.0, 180.0, -180.0, 180.0, 1, &TubePanel::setTwist);
    twist_->setObjectName(QStringLiteral("tubeTwist"));
    twist_->setValue(0.0);
    twist_->setToolTip(tr("Degrees: the sections turned about the guide from lying flat on the scalp, the same root to tip (the "
                          "brush's Twist paints it along the guide)"));
    // The names inside the fields (the whole row each).
    width_->setCaption(tr("Width"));
    sections->addRow(width_);
    refreshUnits();
    flatness_->setCaption(tr("Flatness"));
    sections->addRow(flatness_);
    twist_->setCaption(tr("Twist (°)"));
    sections->addRow(twist_);
    // Smooth: the targeted tubes' painted sections evened out along each guide (one undo step).
    auto* smoothRow = new QHBoxLayout();
    smoothRow->setSpacing(6);
    smooth_ = new QPushButton(tr("Smooth"), this);
    smooth_->setObjectName(QStringLiteral("tubeSmooth"));
    smooth_->setToolTip(tr("Smooth the targeted guides' tubes along each guide: what the brush painted evened out, the root's and "
                           "tip's sections kept (Shift+drag with the brush smooths under it)"));
    connect(smooth_, &QPushButton::clicked, this, [this] { smoothTargets(); });
    smoothWhat_ = new QComboBox(this);
    smoothWhat_->setObjectName(QStringLiteral("tubeSmoothChannels"));
    for (const ui::json& item : defs().choices("tubeSmooth")) {
        smoothWhat_->addItem(qs(item.value("label", std::string())), qs(item.value("value", std::string())));
    }
    smoothWhat_->setToolTip(qs(defs().get("choices/tubeSmooth").value("tip", std::string())));
    smoothRow->addWidget(smooth_);
    smoothRow->addWidget(smoothWhat_, 1);
    sections->addRow(QString(), smoothRow);

    // Fill Tubes and Tube Blend: the groom's Interpolate nodes'.
    // (in the sections' form: the labels line up)
    fill_ = new QCheckBox(tr("Fill Tubes"), this);
    fill_->setObjectName(QStringLiteral("fillTubes"));
    fill_->setToolTip(tr("Strands rooted inside a guide's tube ride it to the tip, flattening, twisting and tapering with it (the "
                         "Interpolate nodes' fillTubes; the tubes of every guide level fill, the deepest winning)"));
    connect(fill_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) setFill(on);
    });
    blend_ = committingSlider(0.0, 100.0, 0.0, 100.0, 0, &TubePanel::setBlend);
    blend_->setObjectName(QStringLiteral("tubeBlend"));
    blend_->setValue(25.0);
    blend_->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral(" %"); });
    blend_->setToolTip(tr("Tube Blend: the tubes' soft edge, a share of each tube's half-width. Strands rooted just outside a tube "
                          "lean toward riding it, so no seam shows round it (0: a hard edge)"));
    blend_->setCaption(tr("Tube Blend"));
    sections->addRow(QString(), fill_);
    sections->addRow(blend_);

    fillShapes();
    showChoice();
    refresh();
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
    GroomDocument* doc = document();
    GroomDocument::TubeTargets info;
    library_.clear();
    std::optional<GroomDocument::FillTubes> fill;
    if (doc) {
        info = doc->tubeTargets();
        for (const GroomDocument::TubeShapeInfo& s : doc->tubeShapes()) {
            if (!s.builtin) library_.push_back({s.name, s.points, s.smooth, s.users});
        }
        fill = doc->fillTubes();
    }
    targets_ = info.guides;
    source_ = info.source;
    shapes_ = info.shapes;
    updating_ = true;
    fillShapes();
    for (QWidget* w : {static_cast<QWidget*>(smooth_), static_cast<QWidget*>(smoothWhat_), static_cast<QWidget*>(apply_)}) {
        w->setEnabled(info.tube.has_value());
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
        if (shapes_.size() == 1 && key != targetsKey_) choice_ = shapes_.front();
        targetsKey_ = key;
        width_->setValue(reference_ * lengthFactor_);
        flatness_->setValue(std::clamp(1.0 - aspect, 0.0, 0.99));
        twist_->setValue(twist * 180.0 / kPi);
    }
    if (fill) {
        fill_->setEnabled(true);
        fill_->setChecked(fill->fill);
        fill_->setToolTip(tr("Strands rooted inside a guide's tube ride it to the tip, flattening, twisting and tapering with it (the "
                             "Interpolate nodes' fillTubes; the tubes of every guide level fill, the deepest winning)"));
        blend_->setValue(fill->blend * 100.0);
    } else {
        fill_->setEnabled(false);
        fill_->setChecked(false);
        fill_->setToolTip(tr("The groom's graph has no Interpolate node: nothing to fill the tubes"));
    }
    blend_->setEnabled(fill && fill->fill);
    updating_ = false;
    showChoice(info.tube.has_value() && shapes_.size() > 1);
    pushPlacement();
}

void TubePanel::fillShapes() {
    // The built-ins (choices.tubeShape), then the library; "Mixed" first while the targets differ.
    const bool mixed = shapes_.size() > 1 && !targets_.empty();
    // Rebuilt only when what it lists changed (the users counts in the tips too).
    std::string key = mixed ? "mixed|" : "|";
    for (const LibraryShape& s : library_) key += s.name + ":" + std::to_string(s.users) + "|";
    if (key == shapesKey_ && shape_->count() > 0) return;
    shapesKey_ = key;
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
    const bool builtin = std::find(builtins.begin(), builtins.end(), choice_) != builtins.end();
    if (!library(choice_) && !builtin) choice_ = "round";  // (a library shape gone: renamed, deleted, another groom)
    {
        const QSignalBlocker blocker(shape_);
        shape_->setCurrentIndex(mixed ? 0 : std::max(0, shape_->findData(qs(choice_))));
    }
    if (const LibraryShape* custom = library(choice_)) {
        editor_->setShape(custom->points, custom->smooth, true);
        if (mirror_->isChecked() && editor_->setMirror(true)) {
            const std::vector<SectionPoint> made = editor_->points();
            shapeCommitted(made);  // made symmetric: saved so
            return;
        }
    } else {
        TubeShape shape = TubeShape::Round;
        parseTubeShape(choice_, &shape);
        bool smooth = true;
        const std::vector<SectionPoint> points = builtinTubeShapePoints(shape, &smooth);
        editor_->setShape(points, smooth, false, tr("Built in: Edit as Custom to change it"));
    }
    updating_ = true;
    shapeSmooth_->setChecked(editor_->smooth());
    updating_ = false;
    const bool custom = library(choice_) != nullptr;
    const bool hasGroom = document() != nullptr;
    editCustom_->setVisible(!custom);
    editCustom_->setEnabled(hasGroom);
    shapeSmooth_->setEnabled(custom);
    new_->setEnabled(hasGroom);
    for (QPushButton* b : {rename_, duplicate_, delete_}) b->setEnabled(custom);
    apply_->setEnabled(!targets_.empty());
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
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) {
        edited(doc->editTubes(targets_, json{{"shape", name}}));
    } else {
        showChoice();
    }
    pushPlacement();
}

void TubePanel::applyShape() {
    GroomDocument* doc = document();
    if (doc && !targets_.empty()) edited(doc->editTubes(targets_, json{{"shape", choice_}}));
    pushPlacement();
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
    const std::string channel = smoothWhat_->currentData().toString() == QLatin1String("current") && session_
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
    TubeShape shape = TubeShape::Round;
    parseTubeShape(choice_, &shape);
    bool smooth = true;
    const std::vector<SectionPoint> points = builtinTubeShapePoints(shape, &smooth);
    const std::string name = uniqueName(choice_);
    const std::string error = doc->addTubeShape(name, points, smooth, targets_);
    if (!error.empty()) {
        emit message(qs(error));
        return;
    }
    choice_ = name;
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
    if (error.empty()) choice_ = name;
    edited(error);
    pushPlacement();
}

void TubePanel::duplicateShape() {
    GroomDocument* doc = document();
    const LibraryShape* custom = library(choice_);
    if (!doc || !custom) return;
    const LibraryShape copy = *custom;
    const std::string name = uniqueName(copy.name);
    const std::string error = doc->addTubeShape(name, copy.points, copy.smooth);
    if (error.empty()) choice_ = name;
    edited(error);
    pushPlacement();
}

void TubePanel::startRename() {
    if (!library(choice_)) return;
    renameField_->setText(qs(choice_));
    renameField_->show();
    renameField_->setFocus();
    renameField_->selectAll();
}

void TubePanel::finishRename() {
    GroomDocument* doc = document();
    const std::string to = renameField_->text().trimmed().toStdString();
    const std::string from = choice_;
    cancelRename();
    if (!doc || to.empty() || to == from || !library(from)) return;
    const std::string error = doc->renameTubeShape(from, to);
    if (error.empty()) choice_ = to;
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
    const LibraryShape* custom = library(choice_);
    if (!doc || !custom) return;
    if (!armed_) {
        armed_ = true;
        const std::size_t users = custom->users;
        delete_->setText(tr("Delete? %1 go%2 round").arg(users).arg(users == 1 ? QStringLiteral("es") : QString()));
        delete_->setProperty("role", "danger");
        restyle(delete_);
        disarm_->start();
        return;
    }
    const std::string name = custom->name;
    disarmDelete();
    const std::string error = doc->removeTubeShape(name);
    if (error.empty()) choice_ = "round";
    edited(error);
    pushPlacement();
}

void TubePanel::disarmDelete() {
    armed_ = false;
    disarm_->stop();
    delete_->setText(tr("Delete"));
    delete_->setProperty("role", QVariant());
    restyle(delete_);
}

void TubePanel::sendPreview() {
    GroomDocument* doc = document();
    if (!doc || !editor_->problem().isEmpty() || !library(choice_)) return;
    if (doc->previewTubeShape(choice_, editor_->points(), editor_->smooth()).empty()) {
        previewing_ = true;
        emit previewed();
    }
}

void TubePanel::shapeCommitted(const std::vector<SectionPoint>& points) {
    preview_->stop();
    GroomDocument* doc = document();
    if (!doc || !library(choice_)) return;
    const std::string error = doc->setTubeShape(choice_, &points, editor_->smooth());
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
    if (editor_->setMirror(on) && library(choice_)) {
        const std::vector<SectionPoint> made = editor_->points();
        shapeCommitted(made);
    }
}

void TubePanel::setShapeSmooth(bool on) {
    GroomDocument* doc = document();
    if (!doc || !library(choice_)) return;
    edited(doc->setTubeShape(choice_, nullptr, on));
}

void TubePanel::setFill(bool on) {
    GroomDocument* doc = document();
    if (!doc) return;
    const std::string error = doc->setFillTubes(on, std::nullopt);
    emit message(qs(error));
    refresh(true);
    if (error.empty()) emit graphEdited();
}

void TubePanel::setBlend(double percent) {
    GroomDocument* doc = document();
    if (!doc) return;
    const std::string error = doc->setFillTubes(std::nullopt, static_cast<float>(percent / 100.0));
    emit message(qs(error));
    refresh(true);
    if (error.empty()) emit graphEdited();
}

}  // namespace groomlab::app
