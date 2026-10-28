// SPDX-License-Identifier: Apache-2.0
// MainWindow: the Properties dock's Groom workspace, as Maya's Attribute Editor for the groom
// (engine/ui/properties.json through properties_panel.h: the sections, the labels and tips, the
// order), the controls and what each edits; the groom's HairColor edits shared with the look card;
// and the Photos tab's Look Through (photos_panel.h: the viewport's camera and the photo).

#include "main_window_internal.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "groomlab/hair_color.h"
#include "groomlab/subdivision.h"

#include "glass.h"
#include "groom_tools.h"
#include "guides_panel.h"
#include "param_editors.h"
#include "photos_panel.h"
#include "properties_panel.h"
#include "reference_window.h"
#include "theme.h"
#include "transfer_window.h"
#include "viewport.h"

namespace groomlab::app {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// A slider in a row (the label is the row's): its track as wide as the row leaves, the value after.
ValueSlider* rowSlider(double min, double max, int decimals, double step, const QColor& fill) {
    auto* s = new ValueSlider();
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setStep(step);
    s->setTrackWidth(0);
    s->setValueWidth(52);
    s->setFill(fill);
    s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return s;
}

QCheckBox* check(bool on) {
    auto* c = new QCheckBox();
    c->setChecked(on);
    return c;
}

QSpinBox* spin(int min, int max, int value) {
    auto* s = new QSpinBox();
    s->setRange(min, max);
    s->setValue(value);
    s->setKeyboardTracking(false);
    s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return s;
}

QComboBox* combo(const QStringList& items) {
    auto* c = new QComboBox();
    c->addItems(items);
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    c->setMinimumContentsLength(6);
    c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return c;
}

QPushButton* smallButton(const QString& text, const QString& tip) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return b;
}

// A value as the rows show it, muted (an input's name, a count).
QLabel* valueLabel(const QString& text = {}) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textSoft().name()));
    return l;
}

QWidget* hbox(std::initializer_list<QWidget*> widgets, int stretchFirst = 1) {
    auto* w = new QWidget();
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    bool first = true;
    for (QWidget* x : widgets) {
        row->addWidget(x, first ? stretchFirst : 0);
        first = false;
    }
    return w;
}

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        if (QLayout* l = item->layout()) clearLayout(l);
        delete item;
    }
}

// A rotation (camera axes to world: its columns) as a quaternion, and back.
QQuaternion quaternionOf(const CameraMat3& r) {
    const float m[9] = {static_cast<float>(r[0][0]), static_cast<float>(r[0][1]), static_cast<float>(r[0][2]),
                        static_cast<float>(r[1][0]), static_cast<float>(r[1][1]), static_cast<float>(r[1][2]),
                        static_cast<float>(r[2][0]), static_cast<float>(r[2][1]), static_cast<float>(r[2][2])};
    return QQuaternion::fromRotationMatrix(QMatrix3x3(m));
}

CameraMat3 matrixOf(const QQuaternion& q) {
    const QMatrix3x3 m = q.toRotationMatrix();
    CameraMat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) r[i][k] = m(i, k);
    return r;
}

}  // namespace

void MainWindow::buildProperties() {
    properties_ = new PropertiesPanel();
    PropertiesPanel* p = properties_;

    // ---- Groom: the two windows, the preview's density and budget, the strand count, Locked.
    {
        auto* tools = smallButton(PropertiesPanel::partLabel("groomButtons", "groomTools"), PropertiesPanel::partTip("groomButtons", "groomTools"));
        auto* graph = smallButton(PropertiesPanel::partLabel("groomButtons", "graphEditor"), PropertiesPanel::partTip("groomButtons", "graphEditor"));
        tools->setObjectName(QStringLiteral("propGroomTools"));
        graph->setObjectName(QStringLiteral("propGraphEditor"));
        connect(tools, &QPushButton::clicked, this, [this] { showUiPanel(QStringLiteral("brush")); });
        connect(graph, &QPushButton::clicked, this, [this] { setGraphShown(true); });
        auto* row = hbox({tools, graph}, 0);
        static_cast<QHBoxLayout*>(row->layout())->addStretch(1);
        p->setBlock("groomButtons", row);
    }
    density_ = rowSlider(0.001, 1.0, 3, 0.01, theme::purple());
    density_->setLimits(0.001, 1.0);
    density_->setValue(1.0);
    p->setControl("density", density_);
    budget_ = rowSlider(0.0, 2000.0, 0, 50.0, theme::purple());
    budget_->setInteger(true);
    budget_->setLimits(0.0, 1000000.0);
    budget_->setValue(kDefaultPointBudget);
    budget_->setValueWidth(64);
    budget_->setFormat([](double v) { return v < 0.5 ? tr("no limit") : QString::number(std::lround(v)) + tr(" k"); });
    p->setControl("budget", budget_);
    strandCount_ = valueLabel(QStringLiteral("0"));
    p->setControl("strandCount", strandCount_);
    locked_ = check(false);
    p->setControl("locked", locked_);
    connect(locked_, &QCheckBox::toggled, this, [this](bool on) { setLocked(on); });
    connect(density_, &ValueSlider::released, this, [this] { reevaluate(); });
    connect(budget_, &ValueSlider::released, this, [this] { reevaluate(); });

    // ---- Inputs: the scalp, its rest pose, the other scalps, the guides, the colliders, the file.
    {
        inputsBlock_ = new QWidget();
        auto* grid = new QGridLayout(inputsBlock_);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(5);
        grid->setColumnStretch(0, 2);
        grid->setColumnStretch(1, 3);
        int row = 0;
        auto add = [&](const char* part, std::initializer_list<QPushButton*> buttons) {
            auto* label = new QLabel(PropertiesPanel::partLabel("inputs", part));
            label->setProperty("role", "propLabel");
            label->setToolTip(PropertiesPanel::partTip("inputs", part));
            auto* value = valueLabel();
            value->setObjectName(QStringLiteral("inputs_") + QLatin1String(part));
            value->setToolTip(label->toolTip());
            auto* host = new QWidget();
            auto* line = new QHBoxLayout(host);
            line->setContentsMargins(0, 0, 0, 0);
            line->setSpacing(4);
            line->addWidget(value, 1);
            for (QPushButton* b : buttons) line->addWidget(b);
            grid->addWidget(label, row, 0, Qt::AlignTop);
            grid->addWidget(host, row, 1);
            ++row;
        };
        auto* transfer = smallButton(tr("Transfer..."), tr("Move the groom onto another mesh (guides, parts and flow re-rooted, the maps projected)"));
        connect(transfer, &QPushButton::clicked, this, [this] { openTransferWindow(); });
        add("scalp", {transfer});
        add("rest", {});
        add("scalps", {});
        auto* edit = smallButton(tr("Edit"), tr("Edit the guides in the Groom Tools"));
        connect(edit, &QPushButton::clicked, this, [this] { showUiPanel(QStringLiteral("guides")); });
        add("guides", {edit});
        auto* prop = smallButton(tr("Add..."), tr("Add a prop mesh beside the groom (an .obj): a collider by its name for ColliderMesh nodes"));
        connect(prop, &QPushButton::clicked, this, [this] { chooseProp(); });
        add("colliders", {prop});
        auto* reload = smallButton(tr("Reload"), tr("Read the groom file again (another host's edits); unsaved edits are asked about first"));
        auto* save = smallButton(tr("Save"), tr("Write the groom to its file"));
        connect(reload, &QPushButton::clicked, this, [this] { askReload(); });
        connect(save, &QPushButton::clicked, this, [this] { this->save(); });
        add("file", {reload, save});
        p->setBlock("inputs", inputsBlock_);
    }
    // Scalp Subdivision: the scalp's Catmull-Clark levels, and the triangles that makes.
    {
        scalpSubdivision_ = combo({});
        scalpSubdivision_->setObjectName(QStringLiteral("scalpSubdivision"));
        scalpSubdivision_->addItem(tr("Off"), 0);
        for (int level = 1; level <= kMaxSubdivisionLevels; ++level) scalpSubdivision_->addItem(QString::number(level), level);
        scalpTriangles_ = valueLabel();
        scalpTriangles_->setObjectName(QStringLiteral("scalpSubdivisionTriangles"));
        scalpTriangles_->setWordWrap(false);
        auto* row = hbox({scalpSubdivision_, scalpTriangles_});
        p->setControl("scalpSubdivision", row);
        scalpSubdivision_->setToolTip(row->toolTip());
        connect(scalpSubdivision_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { setScalpSubdivision(scalpSubdivision_->currentData().toInt()); });
    }
    followScalp_ = check(false);
    p->setControl("followScalp", followScalp_);
    connect(followScalp_, &QCheckBox::toggled, this, [this](bool on) {
        if (document_ && document_->followScalp() != on) setFollowScalp(on);
    });
    // Scaling with the scalp: what the groom's scale follows, its scale now and Set Scale Reference.
    scaleSource_ = combo({});
    scaleSource_->addItem(tr("Surface Area"), static_cast<int>(ScaleSource::Area));
    scaleSource_->addItem(tr("Transform Only"), static_cast<int>(ScaleSource::Transform));
    scaleSource_->addItem(tr("Off"), static_cast<int>(ScaleSource::Off));
    p->setControl("scaleSource", scaleSource_);
    connect(scaleSource_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!document_) return;
        document_->setScaleSource(static_cast<ScaleSource>(scaleSource_->currentData().toInt()));
        refreshDisplayChoices();
        reevaluate();
    });
    {
        groomScale_ = valueLabel(QStringLiteral("1.00x"));
        auto* setReference = smallButton(tr("Set Scale Reference"), QString());
        setReference->setObjectName(QStringLiteral("propSetScaleReference"));
        connect(setReference, &QPushButton::clicked, this, [this] {
            if (!document_) return;
            document_->setScaleReference();
            refreshDisplayChoices();
            reevaluate();
        });
        auto* row = hbox({groomScale_, setReference});
        p->setControl("groomScale", row);
        setReference->setToolTip(row->toolTip());
    }

    // ---- Controls (the Control nodes' sliders) and Hair Color (the HairColor node's rows): built
    // from the graph as it changes (refreshControlSliders, refreshHairColorRows).
    controlsBlock_ = new QWidget();
    auto* controlsLayout = new QGridLayout(controlsBlock_);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    p->setBlock("controls", controlsBlock_);
    hairColorBlock_ = new QWidget();
    auto* colorLayout = new QGridLayout(hairColorBlock_);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    p->setBlock("hairColor", hairColorBlock_);

    // ---- Display: what shows, the strand colors.
    guides_ = check(settings_.guides);
    p->setControl("guides", guides_);
    flow_ = check(settings_.flow);
    p->setControl("flow", flow_);
    tubes_ = check(settings_.tubes);
    p->setControl("tubes", tubes_);
    instances_ = check(settings_.instances);
    p->setControl("instances", instances_);
    scalp_ = check(true);
    p->setControl("scalp", scalp_);
    gpuPreview_ = check(gpu_.enabled());
    p->setControl("gpuPreview", gpuPreview_);
    // The strand colors (Maya's strandColorMode): solid or lit under the look, a color set of the
    // scalp at each root, a strand channel over a range, a random color per strand, by clump,
    // by density. The colored modes draw lines (the lookdev ribbons take the material's pigment).
    colorMode_ = combo({});
    for (const auto& [label, mode] : {std::pair<QString, int>{tr("Solid / lit"), kSolidColors}, {tr("Color set"), kColorSetColors},
                                      {tr("Attribute"), kAttributeColors}, {tr("Random"), kRandomColors}, {tr("Clump"), kClumpColors},
                                      {tr("Density"), kDensityColors}}) {
        colorMode_->addItem(label, mode);
    }
    p->setControl("colorMode", colorMode_);
    colorSource_ = combo({});
    colorSource_->setEditable(true);
    p->setControl("colorSource", colorSource_);
    colorMin_ = new QDoubleSpinBox();
    colorMin_->setRange(-1e6, 1e6);
    colorMin_->setDecimals(3);
    colorMin_->setKeyboardTracking(false);
    colorMin_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);  // not the width of "-1000000.000"
    p->setControl("colorMin", colorMin_);
    colorMax_ = new QDoubleSpinBox();
    colorMax_->setRange(-1e6, 1e6);
    colorMax_->setDecimals(3);
    colorMax_->setValue(1.0);
    colorMax_->setKeyboardTracking(false);
    colorMax_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    p->setControl("colorMax", colorMax_);
    // A painted map drawn on the scalp (the Maya plugin's show_map_on_scalp).
    scalpMap_ = combo({});
    p->setControl("scalpMap", scalpMap_);
    scalpMapChannel_ = combo({QStringLiteral("rgb"), QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b"), QStringLiteral("a")});
    p->setControl("scalpMapChannel", scalpMapChannel_);

    // ---- Display > Guides.
    guideWidth_ = rowSlider(1.0, 12.0, 1, 0.5, theme::emerald());
    guideWidth_->setLimits(1.0, 16.0);
    guideWidth_->setValue(settings_.guideWidth);
    p->setControl("guideWidth", guideWidth_);
    guidePoints_ = combo({tr("Off"), tr("Roots and tips"), tr("All")});
    guidePoints_->setCurrentIndex(settings_.guidePoints);
    p->setControl("guidePoints", guidePoints_);
    guideSmoothing_ = spin(1, 16, settings_.guideSmoothing);
    p->setControl("guideSmoothing", guideSmoothing_);
    isolateGroup_ = new QLineEdit(QString::fromStdString(settings_.isolateGroup));
    isolateGroup_->setPlaceholderText(tr("None"));
    p->setControl("isolateGroup", isolateGroup_);
    guideColorMode_ = combo({tr("Group"), tr("Painted Layer")});
    guideColorMode_->setCurrentIndex(settings_.guideColorMode == 1 ? 1 : 0);
    p->setControl("guideColorMode", guideColorMode_);

    // ---- Display > Strand Shading.
    lighting_ = check(settings_.lighting);
    p->setControl("lighting", lighting_);
    lookdev_ = check(true);
    p->setControl("lookdev", lookdev_);
    widthCoverage_ = check(settings_.widthCoverage);
    p->setControl("widthCoverage", widthCoverage_);
    coverageDensity_ = check(settings_.coverageDensity);
    p->setControl("coverageDensity", coverageDensity_);
    clumpLod_ = check(settings_.clumpLod);
    p->setControl("clumpLod", clumpLod_);
    auto* lodError = rowSlider(0.01, 0.45, 3, 0.01, theme::blue());
    lodError->setLimits(0.01, 0.45);
    lodError->setValue(settings_.clumpLodError);
    p->setControl("clumpLodError", lodError);
    auto* ribbonWidth = rowSlider(0.0, 10.0, 2, 0.05, theme::blue());
    ribbonWidth->setLimits(0.0, 1000.0);
    ribbonWidth->setValue(settings_.ribbonWidthScale);
    p->setControl("ribbonWidthScale", ribbonWidth);

    // ---- Sculpting: the strands while a stroke is live, the density they evaluate at.
    sculptDisplay_ = combo({});
    for (const ui::json& c : ui::Definitions::shipped().choices("sculptDisplay")) {
        sculptDisplay_->addItem(qs(c.value("label", std::string())));
        sculptDisplay_->setItemData(sculptDisplay_->count() - 1, qs(c.value("tip", std::string())), Qt::ToolTipRole);
    }
    sculptDisplay_->setCurrentIndex(std::clamp(settings_.sculptDisplay, 0, std::max(0, sculptDisplay_->count() - 1)));
    p->setControl("sculptDisplay", sculptDisplay_);
    sculptDensity_ = rowSlider(0.01, 1.0, 2, 0.05, theme::emerald());
    sculptDensity_->setLimits(0.01, 1.0);
    sculptDensity_->setValue(brush_.sculptDensity);
    p->setControl("sculptDensity", sculptDensity_);

    // ---- Game Cards.
    cardsView_ = check(false);
    cardsView_->setEnabled(false);
    p->setControl("cardsView", cardsView_);
    cardDensitySlider_ = rowSlider(0.01, 1.0, 2, 0.05, theme::yellow());
    cardDensitySlider_->setLimits(0.01, 1.0);
    cardDensitySlider_->setValue(cardDensity_);
    p->setControl("cardDensity", cardDensitySlider_);

    // ---- Rendering > Render Overrides and Level of Detail (the V-Ray and RenderMan renders, the exports).
    auto overrideSlider = [&](const char* key, float RenderOverrides::* member, double max, double limit) {
        auto* s = rowSlider(0.0, max, 3, 0.05, theme::red());
        s->setLimits(0.0, limit);
        s->setValue(renderOverrides_.*member);
        p->setControl(key, s);
        connect(s, &ValueSlider::valueChanged, this, [this, member](double v) {
            renderOverrides_.*member = static_cast<float>(v);
            if (document_) document_->setRenderOverrides(renderOverrides_);  // its exports and renders
        });
    };
    overrideSlider("renderDensity", &RenderOverrides::density, 1.0, 1.0);
    overrideSlider("renderWidthScale", &RenderOverrides::widthScale, 4.0, 1000.0);
    overrideSlider("renderLengthScale", &RenderOverrides::lengthScale, 4.0, 1000.0);
    auto lodCheck = [&](const char* key, bool LodSettings::* member) {
        QCheckBox* c = check(renderLod_.*member);
        p->setControl(key, c);
        connect(c, &QCheckBox::toggled, this, [this, member](bool on) { renderLod_.*member = on; });
    };
    auto lodSlider = [&](const char* key, float LodSettings::* member, double max, int decimals) {
        auto* s = rowSlider(0.0, max, decimals, max / 100.0, theme::red());
        s->setLimits(0.0, 1e9);
        s->setValue(renderLod_.*member);
        p->setControl(key, s);
        connect(s, &ValueSlider::valueChanged, this, [this, member](double v) { renderLod_.*member = static_cast<float>(v); });
    };
    lodCheck("lodDistance", &LodSettings::distance);
    lodSlider("lodNear", &LodSettings::nearDistance, 100.0, 2);
    lodSlider("lodFar", &LodSettings::farDistance, 1000.0, 1);
    lodSlider("lodMinDensity", &LodSettings::minDensity, 1.0, 3);
    lodSlider("lodWidthCompensation", &LodSettings::widthCompensation, 1.0, 3);
    lodCheck("cameraCull", &LodSettings::frustum);
    lodSlider("cullMargin", &LodSettings::margin, 1.0, 3);
    lodCheck("backfaceCull", &LodSettings::backface);
    lodSlider("backfaceAngle", &LodSettings::backfaceAngle, 90.0, 1);

    p->finish();
    shell_->setPanelContent(AppShell::Panel::Display, tr("Properties"), theme::purple(), p);

    // ---- What the display controls edit.
    auto toggle = [this](QCheckBox* box, bool DisplaySettings::* member, bool coverage = false) {
        connect(box, &QCheckBox::toggled, this, [this, member, coverage](bool on) {
            settings_.*member = on;
            redisplay();
            if (coverage) viewport_->update();
        });
    };
    toggle(guides_, &DisplaySettings::guides);
    connect(guides_, &QCheckBox::toggled, groomTools_, &GroomTools::setGuidesVisible);  // the guides' eye follows
    toggle(flow_, &DisplaySettings::flow);
    toggle(tubes_, &DisplaySettings::tubes);
    toggle(instances_, &DisplaySettings::instances);
    toggle(lighting_, &DisplaySettings::lighting);
    toggle(widthCoverage_, &DisplaySettings::widthCoverage, true);
    toggle(coverageDensity_, &DisplaySettings::coverageDensity, true);
    toggle(clumpLod_, &DisplaySettings::clumpLod, true);
    connect(lookdev_, &QCheckBox::toggled, this, [this](bool on) { setLookdev(on); });
    connect(gpuPreview_, &QCheckBox::toggled, this, [this](bool on) { setGpuPreview(on); });
    connect(scalp_, &QCheckBox::toggled, this, [this](bool on) {
        viewport_->renderer().setShowScalp(on);
        viewport_->renderer().invalidateAccumulation();
        viewport_->update();
    });
    connect(colorMode_, &QComboBox::currentIndexChanged, this, [this](int) { applyDisplayOptions(); });
    connect(colorSource_, &QComboBox::currentTextChanged, this, [this](const QString&) { applyDisplayOptions(); });
    connect(colorMin_, &QDoubleSpinBox::valueChanged, this, [this](double) { applyDisplayOptions(); });
    connect(colorMax_, &QDoubleSpinBox::valueChanged, this, [this](double) { applyDisplayOptions(); });
    connect(guidePoints_, &QComboBox::currentIndexChanged, this, [this](int) { applyDisplayOptions(); });
    connect(guideWidth_, &ValueSlider::valueChanged, this, [this](double) { applyDisplayOptions(); });
    connect(scalpMap_, &QComboBox::currentIndexChanged, this, [this](int) { applyDisplayOptions(); });
    connect(scalpMapChannel_, &QComboBox::currentIndexChanged, this, [this](int) { applyDisplayOptions(); });
    connect(cardsView_, &QCheckBox::toggled, this, [this](bool) { applyDisplayOptions(); });
    auto number = [this](QSpinBox* box, int DisplaySettings::* member) {
        connect(box, &QSpinBox::valueChanged, this, [this, member](int v) {
            settings_.*member = v;
            redisplay();
        });
    };
    number(guideSmoothing_, &DisplaySettings::guideSmoothing);
    connect(isolateGroup_, &QLineEdit::editingFinished, this, [this] {
        settings_.isolateGroup = isolateGroup_->text().trimmed().toStdString();
        redisplay();
    });
    connect(guideColorMode_, &QComboBox::currentIndexChanged, this, [this](int mode) {
        settings_.guideColorMode = mode;
        redisplay();
    });
    connect(lodError, &ValueSlider::valueChanged, this, [this](double v) {
        settings_.clumpLodError = static_cast<float>(v);
        redisplay();
        viewport_->update();
    });
    connect(ribbonWidth, &ValueSlider::valueChanged, this, [this](double v) {
        settings_.ribbonWidthScale = static_cast<float>(v);
        redisplay();
        viewport_->update();
    });
    connect(sculptDisplay_, &QComboBox::currentIndexChanged, this, [this](int v) {
        settings_.sculptDisplay = v;
        groomTools_->setSculptDisplay(v);
        redisplay();
    });
    connect(sculptDensity_, &ValueSlider::valueChanged, this, [this](double v) { brush_.sculptDensity = static_cast<float>(v); });
    connect(cardDensitySlider_, &ValueSlider::valueChanged, this, [this](double v) { cardDensity_ = static_cast<float>(v); });
    connect(cardDensitySlider_, &ValueSlider::released, this, [this] {
        if (document_ && settings_.cardsView) reevaluate();
    });

    // ---- The Photos tab (photos_panel.h): the viewport looks through a photo's camera.
    photos_ = new PhotosPanel();
    groomTools_->setPhotosContent(photos_);
    connect(photos_, &PhotosPanel::lookThroughRequested, this, [this](int index, bool on) { lookThroughPhoto(index, on); });
    connect(photos_, &PhotosPanel::displayChanged, this, [this] { refreshPhotoBackdrop(); });
    connect(photos_, &PhotosPanel::message, this, [this](const QString& text) { report(text); });
    connect(photos_, &PhotosPanel::edited, this, [this] {
        refreshTitle();
        if (referenceWindow_) referenceWindow_->photosChanged();
    });
    connect(photos_, &PhotosPanel::alignRequested, this, [this](int index) { openPhotoAlign(index); });
    lookStore_ = new QTimer(this);
    lookStore_->setSingleShot(true);
    lookStore_->setInterval(250);
    connect(lookStore_, &QTimer::timeout, this, [this] { storeLookThroughCamera(); });
    connect(viewport_, &Viewport::cameraChanged, this, [this] {
        if (!lookPhoto_.isEmpty() && viewport_->camera().lookingThrough() && !viewport_->camera().lookThroughLocked()) lookStore_->start();
    });
}

void MainWindow::refreshProperties() {
    if (!properties_) return;
    const bool open = document_ != nullptr;
    properties_->setEnabled(true);
    // The inputs.
    auto set = [this](const char* part, const QString& text) {
        if (auto* l = inputsBlock_->findChild<QLabel*>(QStringLiteral("inputs_") + QLatin1String(part))) l->setText(text);
    };
    if (open) {
        const GroomFile& file = document_->file();
        const MeshPtr& scalp = file.data.scalp;
        set("scalp", scalp ? tr("%n vertices", "", static_cast<int>(scalp->pointCount())) : tr("none"));
        set("rest", scalp && scalp->hasRest() ? tr("the scalp's rest pose") : tr("none"));
        QStringList scalps;
        for (const auto& [name, mesh] : file.data.scalps) scalps << qs(name);
        set("scalps", scalps.isEmpty() ? tr("none") : scalps.join(QStringLiteral(", ")));
        std::size_t guides = 0, groups = 0;
        if (const HierarchyPtr h = document_->session().authoredHierarchy()) {
            guides = h->guides().size();
            groups = h->groups().size();
        }
        set("guides", guides ? tr("%1 guides in %2 group%3").arg(guides).arg(groups).arg(groups == 1 ? QString() : QStringLiteral("s"))
                             : tr("none yet: place or draw them"));
        QStringList colliders;
        for (const auto& [name, mesh] : file.data.meshes) colliders << qs(name);
        for (const SurfacePtr& prop : props_) {
            if (prop->mesh && !prop->isEye && !colliders.contains(qs(prop->name))) colliders << qs(prop->name);
        }
        set("colliders", colliders.isEmpty() ? tr("none") : colliders.join(QStringLiteral(", ")));
        set("file", document_->path().empty() ? tr("(not saved yet)") : QFileInfo(qs(document_->path())).fileName());
        if (auto* l = inputsBlock_->findChild<QLabel*>(QStringLiteral("inputs_file"))) l->setToolTip(qs(document_->path()));
        const QSignalBlocker block(followScalp_);
        followScalp_->setChecked(document_->followScalp());
        followScalp_->setEnabled((scalp && scalp->hasRest()) || document_->followScalp());
    } else {
        for (const char* part : {"scalp", "rest", "scalps", "guides", "colliders", "file"}) set(part, QStringLiteral("—"));
    }
    for (QWidget* w : {static_cast<QWidget*>(inputsBlock_), static_cast<QWidget*>(followScalp_), static_cast<QWidget*>(scaleSource_),
                       static_cast<QWidget*>(locked_)}) {
        w->setEnabled(open && (w != followScalp_ || followScalp_->isEnabled()));
    }
    refreshScalpSubdivision();
    // The display's own state as other panels change it (Isolate, While sculpting).
    {
        const QSignalBlocker a(isolateGroup_), b(guideColorMode_), c(sculptDisplay_);
        isolateGroup_->setText(QString::fromStdString(settings_.isolateGroup));
        guideColorMode_->setCurrentIndex(settings_.guideColorMode == 1 ? 1 : 0);
        sculptDisplay_->setCurrentIndex(std::clamp(settings_.sculptDisplay, 0, std::max(0, sculptDisplay_->count() - 1)));
        if (!sculptDensity_->editing()) sculptDensity_->setValue(brush_.sculptDensity);
    }
    // Source applies to the modes that read one (a color set, a channel).
    const int mode = colorMode_->currentData().toInt();
    properties_->setRowVisible("colorSource", mode == kColorSetColors || mode == kAttributeColors);
    properties_->setRowVisible("colorMin", mode == kAttributeColors);
    properties_->setRowVisible("colorMax", mode == kAttributeColors);
    refreshControlSliders();
    refreshHairColorRows();
}

namespace {

// Past this many triangles the refined scalp slows the brushes, the picks and the viewport.
constexpr std::size_t kLargeScalpTriangles = 2000000;

QString triangleCountText(std::size_t n) {
    if (n >= 1000000) return QString::number(static_cast<double>(n) / 1e6, 'f', 1) + QStringLiteral("M");
    if (n >= 10000) return QString::number(std::lround(static_cast<double>(n) / 1e3)) + QStringLiteral("k");
    return QString::number(n);
}

}  // namespace

void MainWindow::setScalpSubdivision(int levels) {
    if (!document_ || document_->scalpSubdivision() == levels) return;
    if (refuseWhileBusy(tr("changing Scalp Subdivision"))) {
        refreshScalpSubdivision();
        return;
    }
    const std::string error = document_->setScalpSubdivision(levels);
    if (!error.empty()) {
        report(QString::fromStdString(error));
        refreshScalpSubdivision();
        return;
    }
    const MeshPtr& scalp = document_->file().data.scalp;
    const std::size_t triangles = scalp ? GroomDocument::refinedTriangleCount(*scalp, document_->scalpLevels()) : 0;
    if (triangles > kLargeScalpTriangles) {
        report(tr("Scalp Subdivision %1: the scalp has %2 triangles now; brushes, picks and the viewport slow down (a lower level, "
                  "or a lighter scalp, keeps them quick)")
                   .arg(levels)
                   .arg(triangleCountText(triangles)),
               false);
    }
    refreshUndoActions();
    reevaluate();
    refreshScalpSubdivision();
}

void MainWindow::refreshScalpSubdivision() {
    if (!scalpSubdivision_) return;
    const MeshPtr scalp = document_ ? document_->file().data.scalp : nullptr;
    {
        const QSignalBlocker block(scalpSubdivision_);
        scalpSubdivision_->setCurrentIndex(std::max(0, scalpSubdivision_->findData(document_ ? document_->scalpSubdivision() : 0)));
    }
    scalpSubdivision_->setEnabled(scalp != nullptr);
    // The Transfer and Guides from Photos windows show (and pick on) the scalp at its level.
    if (transferWindow_) transferWindow_->refreshScalp();
    if (referenceWindow_) referenceWindow_->refreshScalp();
    if (!scalp) {
        scalpTriangles_->clear();
        scalpTriangles_->setToolTip(QString());
        return;
    }
    // The triangles the hair grows on: the scalp refined by the levels the groom uses (a Scalp node
    // may ask for other than this setting).
    const int levels = document_->scalpLevels();
    const std::size_t triangles = GroomDocument::refinedTriangleCount(*scalp, levels);
    QString text = tr("%1 triangles").arg(triangleCountText(triangles));
    QString tip = tr("The scalp the hair grows on: %1 triangles (the mesh itself: %2)")
                      .arg(QLocale().toString(static_cast<qulonglong>(triangles)))
                      .arg(QLocale().toString(static_cast<qulonglong>(scalp->triangleCount())));
    if (levels != document_->scalpSubdivision()) {
        text += tr(" (Scalp node: %1)").arg(levels == 0 ? tr("off") : QString::number(levels));
        tip += tr(". A Scalp node in the graph refines it by %1, not Match Render").arg(levels);
    }
    if (triangles > kLargeScalpTriangles) {
        tip += tr(". That many triangles slow the brushes, the picks and the viewport");
    }
    scalpTriangles_->setText(text);
    scalpTriangles_->setToolTip(tip);
    scalpTriangles_->setStyleSheet(QStringLiteral("color: %1;").arg(
        (triangles > kLargeScalpTriangles ? theme::warningText() : theme::textSoft()).name()));
}

void MainWindow::refreshControlSliders() {
    std::vector<GroomDocument::ControlSlider> sliders;
    if (document_) sliders = document_->controlSliders();
    std::string key;
    for (const auto& c : sliders) key += c.node + "|" + c.name + "|" + std::to_string(c.min) + "|" + std::to_string(c.max) + ";";
    if (key == controlsKey_ && controlsBlock_->layout()->count() > 0) {
        // The same sliders: their values (a reload, an undo) unless one is held.
        for (const auto& c : sliders) {
            auto* s = controlsBlock_->findChild<ValueSlider*>(QStringLiteral("control_") + qs(c.name));
            if (s && !s->editing() && std::abs(s->value() - c.value) > 1e-9) s->setValue(c.value);
        }
        return;
    }
    controlsKey_ = key;
    auto* grid = static_cast<QGridLayout*>(controlsBlock_->layout());
    clearLayout(grid);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(5);
    grid->setColumnStretch(0, 2);
    grid->setColumnStretch(1, 3);
    if (sliders.empty()) {
        auto* none = new QLabel(tr("No sliders yet: in the Graph Editor, right-click a param > Drive with Slider."), controlsBlock_);
        none->setProperty("role", "hint");
        none->setWordWrap(true);
        grid->addWidget(none, 0, 0, 1, 2);
        return;
    }
    int row = 0;
    for (const auto& c : sliders) {
        auto* label = new QLabel(qs(c.name), controlsBlock_);
        label->setProperty("role", "propLabel");
        label->setToolTip(tr("The Control node %1's value (its slider spans %2 to %3; values past it are not clamped)")
                              .arg(qs(c.node))
                              .arg(c.min)
                              .arg(c.max));
        auto* s = rowSlider(c.min, c.max, 3, (c.max - c.min) / 100.0, theme::purple());
        s->setParent(controlsBlock_);
        s->setObjectName(QStringLiteral("control_") + qs(c.name));
        s->setValue(c.value);
        s->setToolTip(label->toolTip());
        const std::string name = c.name;
        connect(s, &ValueSlider::valueChanged, this, [this, name](double v) {
            if (!document_ || !document_->setControlValue(name, v)) return;
            reevaluate(true);
            refreshTitle();
        });
        connect(s, &ValueSlider::released, this, [this] {
            if (document_) reevaluate();
        });
        grid->addWidget(label, row, 0);
        grid->addWidget(s, row, 1);
        ++row;
    }
}

void MainWindow::refreshHairColorRows() {
    if (groomColorDragging_) return;  // (a slider of these rows is held: its own edit)
    const std::string node = hairColorNode();
    nlohmann::json params = nlohmann::json::object();
    if (document_ && !node.empty()) {
        for (const nlohmann::json& n : document_->graph().value("nodes", nlohmann::json::array())) {
            if (n.value("name", std::string()) == node) params = n.value("params", nlohmann::json::object());
        }
    }
    const std::string key = (document_ ? std::string("open") : std::string()) + "|" + node + "|" + params.dump();
    if (key == hairColorKey_ && hairColorBlock_->layout()->count() > 0) return;
    hairColorKey_ = key;
    auto* grid = static_cast<QGridLayout*>(hairColorBlock_->layout());
    clearLayout(grid);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(5);
    grid->setColumnStretch(0, 2);
    grid->setColumnStretch(1, 3);
    int row = 0;
    auto note = [&](const QString& text) {
        auto* l = new QLabel(text, hairColorBlock_);
        l->setProperty("role", "hint");
        l->setWordWrap(true);
        grid->addWidget(l, row++, 0, 1, 2);
    };
    if (!document_) {
        note(tr("Open a groom to color it."));
        return;
    }
    if (node.empty()) {
        note(tr("No HairColor node colors this groom: add one in the Graph Editor (Add Node > Color > HairColor)."));
    } else {
        auto value = [&](const std::string& param) { return document_->paramOrDefault(node, param); };
        const std::string mode = value("mode") == "color" ? "color" : "melanin";
        const std::vector<PropertiesPanel::Item> items = PropertiesPanel::appItems();
        const auto item = std::find_if(items.begin(), items.end(), [](const PropertiesPanel::Item& i) { return i.key == "hairColor"; });
        for (const ui::json& part : item != items.end() ? item->parts : ui::json::array()) {
            const std::string param = part.value("id", std::string()), kind = part.value("kind", std::string());
            const std::string only = part.value("only", std::string());
            if (!only.empty() && only != mode) continue;
            const QString label = qs(ui::hostText("label", ui::kHost, {&part})), tip = qs(ui::hostText("tip", ui::kHost, {&part}));
            if (kind == "heading") {
                auto* h = new QLabel(label.toUpper(), hairColorBlock_);
                h->setProperty("role", "subheading");
                grid->addWidget(h, row++, 0, 1, 2);
                continue;
            }
            const nlohmann::json v = kind == "result" ? nlohmann::json() : value(param);
            auto* l = new QLabel(label, hairColorBlock_);
            l->setProperty("role", "propLabel");
            l->setToolTip(tip);
            QWidget* control = nullptr;
            if (v.is_object()) {
                control = valueLabel(tr("mapped (Graph Editor)"));  // a map or an expression: the Graph Editor edits those
            } else if (kind == "mode") {
                auto* c = combo({tr("Natural"), tr("Picked color")});
                c->setCurrentIndex(mode == "color" ? 1 : 0);
                connect(c, &QComboBox::currentIndexChanged, this, [this](int i) { chooseGroomMode(i == 1 ? "color" : "melanin"); });
                control = c;
            } else if (kind == "color" || kind == "result") {
                auto* e = new ColorEditor();
                RGB rgb{1.0f, 1.0f, 1.0f};
                if (kind == "result") {
                    const nlohmann::json m = value("melanin"), r = value("redness"), d = value("dye");
                    if (!m.is_number() || !r.is_number()) {
                        delete e;
                        delete l;
                        continue;  // mapped: no one color
                    }
                    RGB dye{1.0f, 1.0f, 1.0f};
                    if (d.is_array() && d.size() == 3) dye = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
                    rgb = melaninColor(m.get<float>(), r.get<float>(), dye);
                    e->setEnabled(false);
                } else if (v.is_array() && v.size() == 3) {
                    rgb = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                }
                e->setLinear(rgb);
                if (kind == "color") {
                    connect(e, &ColorEditor::edited, this, [this, e, param](bool live) { pickGroomColor(param, e->linear(), live); });
                }
                control = e;
            } else {
                auto* s = rowSlider(0.0, 1.0, 3, 0.01, theme::yellow());
                s->setLimits(0.0, 1.0);
                s->setValue(v.is_number() ? v.get<double>() : 0.0);
                connect(s, &ValueSlider::valueChanged, this, [this, param](double x) { editGroomColor(param, x, true); });
                connect(s, &ValueSlider::released, this, [this, param, s] { editGroomColor(param, s->value(), false); });
                control = s;
            }
            control->setParent(hairColorBlock_);
            control->setObjectName(QStringLiteral("hairColor_") + qs(param));
            control->setToolTip(tip);
            grid->addWidget(l, row, 0);
            grid->addWidget(control, row++, 1);
        }
    }
    auto* edit = smallButton(node.empty() ? tr("Graph Editor") : tr("Edit in Graph Editor"),
                             tr("The HairColor node's every param (a map, tipProfile, the seed, the highlight's randomness...)"));
    edit->setParent(hairColorBlock_);
    connect(edit, &QPushButton::clicked, this, [this, node] {
        setGraphShown(true);
        if (!node.empty()) selectUiNode(qs(node));
    });
    grid->addWidget(edit, row, 1, Qt::AlignLeft);
}

// ---- The groom's HairColor ---------------------------------------------------------------------------

void MainWindow::editGroomColor(const std::string& param, double value, bool live) {
    const std::string node = hairColorNode();
    if (!document_ || node.empty()) return;
    if (live && !groomColorDragging_) {
        groomColorDragging_ = true;
        groomColorBefore_ = document_->paramOrDefault(node, param);
    }
    const std::string error = document_->setParam(node, param, value, !live && !groomColorDragging_);
    if (!error.empty()) {
        groomColorDragging_ = false;
        report(QString::fromStdString(error));
        refreshGroomColor();
        return;
    }
    if (!live && groomColorDragging_) {
        groomColorDragging_ = false;
        document_->recordEdit(node, param, groomColorBefore_, document_->paramOrDefault(node, param));
    }
    if (live) reevaluate(true);
    else graphEdited();
}

void MainWindow::pickGroomColor(const std::string& param, const RGB& c, bool live) {
    const std::string node = hairColorNode();
    if (!document_ || node.empty()) return;
    const nlohmann::json value = {c[0], c[1], c[2]};
    if (live && !groomColorDragging_) {
        groomColorDragging_ = true;
        groomColorBefore_ = document_->paramOrDefault(node, param);
    }
    std::string error = document_->setParam(node, param, value, !live && !groomColorDragging_);
    if (error.empty() && !live && groomColorDragging_) {
        groomColorDragging_ = false;
        document_->recordEdit(node, param, groomColorBefore_, document_->paramOrDefault(node, param));
    }
    // A tip color picked while the tips are at 0 strength raises them to 1, so the pick shows.
    if (error.empty() && !live && param == "tipColor") {
        const nlohmann::json strength = document_->paramOrDefault(node, "tipStrength");
        if (strength.is_number() && strength.get<double>() <= 0.0) error = document_->setParam(node, "tipStrength", 1.0, true);
    }
    if (!error.empty()) {
        groomColorDragging_ = false;
        report(QString::fromStdString(error));
        refreshGroomColor();
        return;
    }
    if (live) reevaluate(true);
    else graphEdited();
}

void MainWindow::chooseGroomMode(const std::string& mode) {
    const std::string node = hairColorNode();
    if (!document_ || node.empty()) return;
    const std::string error = document_->setParam(node, "mode", mode, true);
    if (!error.empty()) report(QString::fromStdString(error));
    graphEdited();
}

// ---- Locked ------------------------------------------------------------------------------------------

void MainWindow::setLocked(bool on) {
    if (locked_->isChecked() != on) {
        locked_->setChecked(on);  // comes back here through the toggle
        return;
    }
    if (on && viewport_->toolMode() == Viewport::ToolMode::Brush) viewport_->setToolMode(Viewport::ToolMode::Camera);
    if (on) notify(tr("The groom is locked: brushes and part strokes leave it as it is (Properties > Groom > Locked)"));
}

// ---- Looking through a reference photo -------------------------------------------------------------

void MainWindow::lookThroughPhoto(int index, bool on) {
    OrbitCamera& camera = viewport_->camera();
    if (!on || index < 0 || index >= photos_->count()) {
        if (lookStore_->isActive()) {
            lookStore_->stop();
            storeLookThroughCamera();
        }
        camera.leaveLookThrough();
        lookPhoto_.clear();
        photos_->setLookingThrough({});
        refreshPhotoBackdrop();
        viewport_->syncView();
        viewport_->update();
        return;
    }
    const QSize size = photos_->imageSize(index);
    std::optional<RefCamera> c = photos_->camera(index);
    if (!c && photos_->placeCamera(index)) c = photos_->camera(index);  // never lined up: in front of the groom
    if (!c) {
        report(tr("%1 has no size: import it again").arg(photos_->name(index)));
        photos_->setLookingThrough(lookPhoto_);
        return;
    }
    if (lookStore_->isActive() && lookPhoto_ != photos_->name(index)) {  // another photo: the one looked through keeps its tumble
        lookStore_->stop();
        storeLookThroughCamera();
    }
    const PhotosPanel::Display display = photos_->display(index);
    const QVector3D eye(static_cast<float>(c->pose.position[0]), static_cast<float>(c->pose.position[1]), static_cast<float>(c->pose.position[2]));
    // The orbit's pivot: the groom's center along the view (else its distance from the camera).
    const Bounds b = sceneBounds();
    float pivot = 10.0f;
    if (b.valid) {
        const QVector3D center = QVector3D(b.lo.x + b.hi.x, b.lo.y + b.hi.y, b.lo.z + b.hi.z) * 0.5f;
        pivot = std::max((center - eye).length(), 1e-3f);
    }
    camera.setLookThrough(eye, quaternionOf(c->pose.rotation), c->focalPx, size.width(), size.height(), pivot, display.locked);
    lookPhoto_ = photos_->name(index);
    photos_->setLookingThrough(lookPhoto_);
    refreshPhotoBackdrop();
    viewport_->renderer().invalidateAccumulation();
    viewport_->syncView();
    viewport_->update();
}

void MainWindow::refreshPhotoBackdrop() {
    const int index = photos_->lookingIndex();
    if (index < 0 || lookPhoto_.isEmpty()) {
        if (!lookPhoto_.isEmpty()) {
            // The photo looked through went (removed, renamed elsewhere): back to the camera before.
            viewport_->camera().leaveLookThrough();
            lookPhoto_.clear();
            photos_->setLookingThrough({});
        }
        viewport_->renderer().clearPhoto();
        if (headless_) headless_->renderer.clearPhoto();
        viewport_->update();
        return;
    }
    const PhotosPanel::Display d = photos_->display(index);
    OrbitCamera& camera = viewport_->camera();
    camera.setLookThroughLocked(d.locked);
    // A solve or another camera stored: the view follows it.
    if (const std::optional<RefCamera> c = photos_->camera(index)) {
        const QVector3D eye(static_cast<float>(c->pose.position[0]), static_cast<float>(c->pose.position[1]), static_cast<float>(c->pose.position[2]));
        if ((eye - camera.lookThroughEye()).length() > 1e-5f || !qFuzzyCompare(quaternionOf(c->pose.rotation), camera.lookThroughRotation())) {
            const QSize size = photos_->imageSize(index);
            camera.setLookThrough(eye, quaternionOf(c->pose.rotation), c->focalPx, size.width(), size.height(), camera.distance(), d.locked);
        }
    }
    GroomRenderer::Photo photo;
    if (d.visible) {
        photo.image = photos_->picture(index);
        photo.key = photo.image.cacheKey();
    }
    photo.opacity = static_cast<float>(d.opacity);
    photo.front = d.front;
    viewport_->renderer().setPhoto(photo);
    if (headless_) headless_->renderer.setPhoto(photo);
    viewport_->renderer().invalidateAccumulation();
    viewport_->syncView();
    viewport_->update();
}

void MainWindow::storeLookThroughCamera() {
    const int index = photos_->lookingIndex();
    const OrbitCamera& camera = viewport_->camera();
    if (index < 0 || !camera.lookingThrough() || camera.lookThroughLocked()) return;
    std::optional<RefCamera> c = photos_->camera(index);
    if (!c) return;
    const QVector3D eye = camera.lookThroughEye();
    c->pose.position = {eye.x(), eye.y(), eye.z()};
    c->pose.rotation = matrixOf(camera.lookThroughRotation());
    photos_->setCamera(index, *c);
}

}  // namespace groomlab::app
