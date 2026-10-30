// SPDX-License-Identifier: Apache-2.0
#include "export_panel.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "theme.h"

namespace groomlab::app {

namespace {

struct TargetInfo {
    const char* name;
    const char* title;
    const char* extension;
    const char* description;
};
const TargetInfo kTargets[] = {
    {"alembic", "Alembic curves", "abc", "Every strand as Alembic curves with its ID, channels, color and root UV: Maya, Houdini, "
                                         "Blender, Katana and any renderer read it."},
    {"usd", "USD curves", "usda", "BasisCurves in a text layer with the same primvars; usdview, Maya, Houdini and Solaris open it."},
    {"unreal", "Unreal groom", "abc", "Unreal Engine's groom importer: the strands with the primary guides it simulates, each groom "
                                      "group its own object with groom_* attributes. Import as a Groom."},
    {"cards", "Cards (glTF)", "glb", "Hair cards baked at this frame as glTF with the maps embedded (base color and coverage, "
                                     "occlusion), plus the hair cap. Unreal, Unity, Blender and Godot import it; FBX is not written "
                                     "(the Maya host writes FBX through Maya)."},
    {"unity", "Unity strands", "abc", "Alembic curves for Unity's hair package: root UVs per point, every root width positive, "
                                      "resampled to a fixed point count, by strand group."},
    {"maps", "Card maps", "", "The cards' maps as an engine wants them, in one click: pick a preset (Unreal, Unity HDRP, every map "
                              "on its own, your own packing under Advanced), the size and format. The output is <folder>/<name>."},
    {"vrscene", "V-Ray scene", "vrscene", "The strands and their V-Ray hair shader (BRDFHair4 fitted to the Arnold look, each "
                                          "strand's color): V-Ray Standalone renders it (#include it), V-Ray for Maya's "
                                          "VRayScene node loads it. A range is written as V-Ray's keys (motion blur)."},
};

constexpr const char* kCustomPreset = "Custom";
constexpr const char* kNoChannel = "—";  // a channel the file leaves out

// A thin glass bar filling from the left.
class ProgressBar : public QWidget {
public:
    explicit ProgressBar(QWidget* parent) : QWidget(parent) { setFixedHeight(8); }
    void set(double fraction) {
        fraction_ = std::clamp(fraction, 0.0, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::track());
        painter.drawRoundedRect(rect(), 4.0, 4.0);
        if (fraction_ > 0.0) {
            painter.setBrush(theme::emerald());
            painter.drawRoundedRect(QRectF(0.0, 0.0, std::max(8.0, width() * fraction_), height()), 4.0, 4.0);
        }
    }

private:
    double fraction_ = 0.0;
};

QLabel* captionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(theme::uiFont(12, 700));
    label->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    return label;
}

}  // namespace

const char* ExportPanel::targetName(Target target) { return kTargets[static_cast<int>(target)].name; }

bool ExportPanel::targetFromName(const std::string& name, Target* target) {
    for (int i = 0; i < static_cast<int>(Target::Count); ++i) {
        if (name == kTargets[i].name) {
            *target = static_cast<Target>(i);
            return true;
        }
    }
    return false;
}

ExportPanel::ExportPanel(QWidget* parent) : GlassPanel(parent, 24.0) {
    setFixedWidth(316);
    // The rows scroll when the panel is placed shorter than them (a small screen, large text).
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* body = new QWidget;
    outer->addWidget(new FitScrollArea(body, this));
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto* title = new QHBoxLayout();
    title->setSpacing(10);
    auto* dot = new QLabel(this);
    dot->setFixedSize(9, 9);
    dot->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(theme::emerald().name()));
    title->addWidget(dot);
    auto* titleLabel = new QLabel(tr("Export"), this);
    titleLabel->setFont(theme::uiFont(15, 800));
    titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    title->addWidget(titleLabel, 1);
    layout->addLayout(title);

    // ---- The targets as a column of pills (radio-like).
    auto* targetGrid = new QGridLayout();
    targetGrid->setSpacing(6);
    for (int i = 0; i < static_cast<int>(Target::Count); ++i) {
        auto* b = new RoundButton(this);
        b->setText(tr(kTargets[i].title));
        b->setDiameter(32);
        b->setRadius(11.0);
        b->setPadding(12);
        b->setFontSize(13);
        b->setFontWeight(700);
        b->setFill(theme::chip(), theme::checkedFill(theme::emerald()));
        b->setTextColor(theme::textSoft(), theme::checkedText(theme::ink()));
        b->setCheckable(true);
        b->setToolTip(tr(kTargets[i].description));
        connect(b, &QAbstractButton::clicked, this, [this, i] {
            setTarget(static_cast<Target>(i));
        });
        targets_.push_back(b);
        targetGrid->addWidget(b, i / 2, i % 2);
    }
    layout->addLayout(targetGrid);
    description_ = new QLabel(this);
    description_->setWordWrap(true);
    description_->setFont(theme::uiFont(12, 500));
    description_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    layout->addWidget(description_);

    // ---- The frames and the density.
    auto* frames = new QGridLayout();
    frames->setHorizontalSpacing(8);
    frames->setVerticalSpacing(6);
    range_ = new QCheckBox(tr("Frame range"), this);
    range_->setToolTip(tr("Off: the current frame alone (a static groom); on: every frame of the range (a cache)"));
    frames->addWidget(range_, 0, 0, 1, 4);
    auto spin = [this](int min, int max, const QString& tip) {
        auto* s = new QSpinBox(this);
        s->setRange(min, max);
        s->setFont(theme::numberFont(12));
        s->setButtonSymbols(QAbstractSpinBox::NoButtons);
        s->setToolTip(tip);
        return s;
    };
    first_ = spin(-100000, 1000000, tr("The first frame"));
    last_ = spin(-100000, 1000000, tr("The last frame"));
    step_ = new QDoubleSpinBox(this);
    step_->setRange(0.01, 1000.0);
    step_->setDecimals(2);
    step_->setValue(1.0);
    step_->setFont(theme::numberFont(12));
    step_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    step_->setToolTip(tr("Frames between samples (0.5: twice a frame)"));
    frames->addWidget(captionLabel(tr("From"), this), 1, 0);
    frames->addWidget(first_, 1, 1);
    frames->addWidget(captionLabel(tr("to"), this), 1, 2);
    frames->addWidget(last_, 1, 3);
    frames->addWidget(captionLabel(tr("Step"), this), 2, 0);
    frames->addWidget(step_, 2, 1);
    layout->addLayout(frames);
    density_ = new ValueSlider(this);
    density_->setStacked(true);
    density_->setCaption(tr("Density"));
    density_->setRange(0.01, 1.0);
    density_->setValue(1.0);
    density_->setFill(theme::yellow());
    density_->setFormat([](double v) { return QString::number(std::lround(v * 100.0)) + QStringLiteral("%"); });
    density_->setToolTip(tr("The share of the strands written (a lighter groom for a game's lower LOD)"));
    layout->addWidget(density_);

    // ---- Each target's own options. Unreal's and Unity's are the Maya host's export's
    // (engine_export.h): groom groups, the feathers' shafts and a lighter groom of barbs.
    engineOptions_ = new QWidget(this);
    auto* engineLayout = new QGridLayout(engineOptions_);
    engineLayout->setContentsMargins(0, 0, 0, 0);
    engineLayout->setHorizontalSpacing(8);
    engineLayout->setVerticalSpacing(6);
    groups_ = new QCheckBox(tr("Groom groups"), engineOptions_);
    groups_->setChecked(true);
    groups_->setToolTip(tr("Each groom group (the groupId channel) its own object: its own material, LODs and physics in Unreal, "
                           "a strand group in Unity"));
    engineLayout->addWidget(groups_, 0, 0, 1, 2);
    shafts_ = new QComboBox(engineOptions_);
    shafts_->addItem(tr("auto"), 0);
    shafts_->addItem(tr("as strands"), 1);
    shafts_->addItem(tr("left out"), 2);
    shafts_->setToolTip(tr("The feathers' shafts: auto leaves them out when the feathers have a rachis mesh (it is their geometry)"));
    engineLayout->addWidget(captionLabel(tr("Shafts"), engineOptions_), 1, 0);
    engineLayout->addWidget(shafts_, 1, 1);
    barbDensity_ = new QDoubleSpinBox(engineOptions_);
    barbDensity_->setRange(0.01, 1.0);
    barbDensity_->setDecimals(2);
    barbDensity_->setSingleStep(0.05);
    barbDensity_->setValue(1.0);
    barbDensity_->setFont(theme::numberFont(12));
    barbDensity_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    barbDensity_->setToolTip(tr("The share of the feathers' barbs written (the same barbs every frame): a lighter groom"));
    engineLayout->addWidget(captionLabel(tr("Barb density"), engineOptions_), 2, 0);
    engineLayout->addWidget(barbDensity_, 2, 1);
    barbWidth_ = new QDoubleSpinBox(engineOptions_);
    barbWidth_->setRange(0.01, 100.0);
    barbWidth_->setDecimals(2);
    barbWidth_->setValue(1.0);
    barbWidth_->setFont(theme::numberFont(12));
    barbWidth_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    barbWidth_->setToolTip(tr("Each barb kept this many times as wide, so the vanes keep their coverage"));
    engineLayout->addWidget(captionLabel(tr("Barb width"), engineOptions_), 3, 0);
    engineLayout->addWidget(barbWidth_, 3, 1);
    layout->addWidget(engineOptions_);

    unrealOptions_ = new QWidget(this);
    auto* unrealLayout = new QHBoxLayout(unrealOptions_);
    unrealLayout->setContentsMargins(0, 0, 0, 0);
    unrealLayout->setSpacing(12);
    guides_ = new QCheckBox(tr("Guides"), unrealOptions_);
    guides_->setChecked(true);
    guides_->setToolTip(tr("The primary guides as the guides Unreal simulates"));
    featherGuides_ = new QCheckBox(tr("Feather guides"), unrealOptions_);
    featherGuides_->setToolTip(tr("A feather group's guides are its shafts (with groom groups)"));
    iridescenceAo_ = new QCheckBox(tr("Iridescence as AO"), unrealOptions_);
    iridescenceAo_->setToolTip(tr("FeatherColor's iridescence in groom_ao too, where Unreal's Hair Attributes node gives it to a material"));
    unrealLayout->addWidget(guides_);
    unrealLayout->addWidget(featherGuides_);
    unrealLayout->addWidget(iridescenceAo_);
    unrealLayout->addStretch(1);
    layout->addWidget(unrealOptions_);

    unityOptions_ = new QWidget(this);
    auto* unityLayout = new QGridLayout(unityOptions_);
    unityLayout->setContentsMargins(0, 0, 0, 0);
    unityLayout->setHorizontalSpacing(8);
    unityLayout->setVerticalSpacing(6);
    scale_ = new QDoubleSpinBox(unityOptions_);
    scale_->setRange(0.0001, 1000.0);
    scale_->setDecimals(4);
    scale_->setValue(1.0);
    scale_->setFont(theme::numberFont(12));
    scale_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    scale_->setToolTip(tr("Points and widths times this (1 keeps centimeters, what Unity's importer expects; 0.01 writes meters)"));
    points_ = new QSpinBox(unityOptions_);
    points_->setRange(0, 128);
    points_->setValue(0);
    points_->setSpecialValueText(tr("as they are"));
    points_->setFont(theme::numberFont(12));
    points_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    points_->setToolTip(tr("Every strand resampled to this many points (3 to 128), so Unity can leave Resample Curves off"));
    unityLayout->addWidget(captionLabel(tr("Scale"), unityOptions_), 0, 0);
    unityLayout->addWidget(scale_, 0, 1);
    unityLayout->addWidget(captionLabel(tr("Points per strand"), unityOptions_), 1, 0);
    unityLayout->addWidget(points_, 1, 1);
    barbPoints_ = new QSpinBox(unityOptions_);
    barbPoints_->setRange(0, 128);
    barbPoints_->setValue(0);
    barbPoints_->setSpecialValueText(tr("as the rest"));
    barbPoints_->setFont(theme::numberFont(12));
    barbPoints_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    barbPoints_->setToolTip(tr("The feather groups' strands resampled to this many points (with groom groups)"));
    unityLayout->addWidget(captionLabel(tr("Barb points"), unityOptions_), 2, 0);
    unityLayout->addWidget(barbPoints_, 2, 1);
    layout->addWidget(unityOptions_);

    cardOptions_ = new QWidget(this);
    auto* cardLayout = new QVBoxLayout(cardOptions_);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(8);
    auto* resolutionRow = new QHBoxLayout();
    resolutionRow->setSpacing(8);
    resolutionRow->addWidget(captionLabel(tr("Resolution"), cardOptions_));
    resolution_ = new QComboBox(cardOptions_);
    resolution_->addItem(tr("the node's"), 0);
    for (const int size : {512, 1024, 2048, 4096}) resolution_->addItem(QStringLiteral("%1").arg(size), size);
    resolutionRow->addWidget(resolution_, 1);
    binary_ = new QCheckBox(tr("Binary .glb"), cardOptions_);
    binary_->setChecked(true);
    binary_->setToolTip(tr("One .glb with the buffer and images inside; off, a .gltf with its .bin and PNGs beside it"));
    resolutionRow->addWidget(binary_);
    cardLayout->addLayout(resolutionRow);
    occlusion_ = new ValueSlider(cardOptions_);
    occlusion_->setStacked(true);
    occlusion_->setCaption(tr("Occlusion"));
    occlusion_->setRange(0.0, 1.0);
    occlusion_->setValue(1.0);
    occlusion_->setFill(theme::purple());
    occlusion_->setFormat([](double v) { return QString::number(v, 'f', 2); });
    cardLayout->addWidget(occlusion_);
    layout->addWidget(cardOptions_);

    // ---- Card maps: the preset, the output's settings, and the packing under Advanced.
    mapOptions_ = new QWidget(this);
    auto* mapLayout = new QVBoxLayout(mapOptions_);
    mapLayout->setContentsMargins(0, 0, 0, 0);
    mapLayout->setSpacing(8);
    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(6);
    mapPreset_ = new QComboBox(mapOptions_);
    mapPreset_->setObjectName(QStringLiteral("mapPreset"));
    mapPreset_->setToolTip(tr("What goes in each file: an engine's layout, every map on its own, or your own"));
    presetRow->addWidget(mapPreset_, 1);
    mapDelete_ = new RoundButton(mapOptions_);
    mapDelete_->setText(tr("Delete"));
    mapDelete_->setDiameter(26);
    mapDelete_->setRadius(9.0);
    mapDelete_->setPadding(10);
    mapDelete_->setFontSize(12);
    mapDelete_->setFill(theme::chip(), theme::chip());
    mapDelete_->setTextColor(theme::text(), theme::text());
    mapDelete_->setToolTip(tr("Delete this preset of yours (built-in ones stay)"));
    presetRow->addWidget(mapDelete_);
    mapLayout->addLayout(presetRow);
    mapDescription_ = new QLabel(mapOptions_);
    mapDescription_->setWordWrap(true);
    mapDescription_->setFont(theme::uiFont(11, 500));
    mapDescription_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    mapLayout->addWidget(mapDescription_);
    auto* mapGrid = new QGridLayout();
    mapGrid->setHorizontalSpacing(8);
    mapGrid->setVerticalSpacing(6);
    mapResolution_ = new QComboBox(mapOptions_);
    mapResolution_->addItem(tr("the node's"), 0);
    for (const int size : {512, 1024, 2048, 4096, 8192}) mapResolution_->addItem(QStringLiteral("%1").arg(size), size);
    mapResolution_->setCurrentIndex(3);  // 2048
    mapResolution_->setToolTip(tr("Texels square (the atlas is square), up to 8192"));
    mapFormat_ = new QComboBox(mapOptions_);
    for (const char* f : {"tga", "png", "tiff", "exr"}) mapFormat_->addItem(QString::fromLatin1(f).toUpper(), QString::fromLatin1(f));
    mapBitDepth_ = new QComboBox(mapOptions_);
    mapBitDepth_->setToolTip(tr("Bits a channel: 8 or 16 (PNG, TIFF), 16 (half) or 32 (float) for EXR"));
    mapPadding_ = new QSpinBox(mapOptions_);
    mapPadding_->setRange(-1, 256);
    mapPadding_->setSpecialValueText(tr("Auto"));
    mapPadding_->setValue(-1);
    mapPadding_->setSuffix(QStringLiteral(" px"));
    mapPadding_->setFont(theme::numberFont(12));
    mapPadding_->setToolTip(tr("Texels the colors spread past the strands so mip maps don't bleed in black; Auto: 8 at 2K and "
                               "below, scaled above"));
    mapSupersample_ = new QComboBox(mapOptions_);
    mapSupersample_->addItem(tr("1x (off)"), 1);
    mapSupersample_->addItem(QStringLiteral("2x"), 2);
    mapSupersample_->addItem(QStringLiteral("4x"), 4);
    mapSupersample_->setToolTip(tr("Anti-aliasing: baked 2 or 4 times bigger, then brought down (slower; never bigger than 8192)"));
    mapGrid->addWidget(captionLabel(tr("Resolution"), mapOptions_), 0, 0);
    mapGrid->addWidget(mapResolution_, 0, 1);
    mapGrid->addWidget(captionLabel(tr("Format"), mapOptions_), 1, 0);
    mapGrid->addWidget(mapFormat_, 1, 1);
    mapGrid->addWidget(captionLabel(tr("Bit depth"), mapOptions_), 2, 0);
    mapGrid->addWidget(mapBitDepth_, 2, 1);
    mapGrid->addWidget(captionLabel(tr("Padding"), mapOptions_), 3, 0);
    mapGrid->addWidget(mapPadding_, 3, 1);
    mapGrid->addWidget(captionLabel(tr("Supersampling"), mapOptions_), 4, 0);
    mapGrid->addWidget(mapSupersample_, 4, 1);
    mapGrid->setColumnStretch(1, 1);
    mapLayout->addLayout(mapGrid);
    mapAdvanced_ = new QCheckBox(tr("Advanced: the packing"), mapOptions_);
    mapAdvanced_->setToolTip(tr("Each file's R, G, B and A: a map's channel, 0 or 1, inverted or not"));
    mapLayout->addWidget(mapAdvanced_);
    packing_ = new QWidget(mapOptions_);
    auto* packingLayout = new QVBoxLayout(packing_);
    packingLayout->setContentsMargins(0, 0, 0, 0);
    packingLayout->setSpacing(6);
    auto* packingGrid = new QGridLayout();
    packingGrid->setHorizontalSpacing(8);
    mapNormal_ = new QComboBox(packing_);
    mapNormal_->addItem(tr("OpenGL (Y up)"), QStringLiteral("opengl"));
    mapNormal_->addItem(tr("DirectX (Y down)"), QStringLiteral("directx"));
    mapNormal_->setToolTip(tr("The normal map's green: OpenGL for Unity, Blender, Maya; DirectX for Unreal, 3ds Max"));
    mapTemplate_ = new QLineEdit(QStringLiteral("{name}_{map}"), packing_);
    mapTemplate_->setFont(theme::monoFont(12));
    mapTemplate_->setToolTip(tr("File names: {name}, {map} (the file's name below) and {res}; the extension is added"));
    packingGrid->addWidget(captionLabel(tr("Normal map"), packing_), 0, 0);
    packingGrid->addWidget(mapNormal_, 0, 1);
    packingGrid->addWidget(captionLabel(tr("File names"), packing_), 1, 0);
    packingGrid->addWidget(mapTemplate_, 1, 1);
    packingGrid->setColumnStretch(1, 1);
    packingLayout->addLayout(packingGrid);
    packRowsLayout_ = new QVBoxLayout();
    packRowsLayout_->setSpacing(6);
    packingLayout->addLayout(packRowsLayout_);
    auto* packButtons = new QHBoxLayout();
    packButtons->setSpacing(6);
    auto smallButton = [](const QString& text, QWidget* parent) {
        auto* b = new RoundButton(parent);
        b->setText(text);
        b->setDiameter(26);
        b->setRadius(9.0);
        b->setPadding(10);
        b->setFontSize(12);
        b->setFill(theme::chip(), theme::chip());
        b->setTextColor(theme::text(), theme::text());
        return b;
    };
    RoundButton* addFile = smallButton(tr("Add file"), packing_);
    packButtons->addWidget(addFile);
    packButtons->addStretch(1);
    packingLayout->addLayout(packButtons);
    auto* saveRowMaps = new QHBoxLayout();
    saveRowMaps->setSpacing(6);
    mapSaveName_ = new QLineEdit(packing_);
    mapSaveName_->setPlaceholderText(tr("Preset name"));
    saveRowMaps->addWidget(mapSaveName_, 1);
    RoundButton* savePreset = smallButton(tr("Save as preset"), packing_);
    savePreset->setToolTip(tr("Save this packing as a preset of yours (Documents/GroomLab/card_presets: every host lists it)"));
    saveRowMaps->addWidget(savePreset);
    packingLayout->addLayout(saveRowMaps);
    packing_->setVisible(false);
    mapLayout->addWidget(packing_);
    layout->addWidget(mapOptions_);
    connect(mapPreset_, &QComboBox::currentIndexChanged, this, [this] {
        if (!filling_) setMapPreset(mapPreset_->currentText());
    });
    connect(mapFormat_, &QComboBox::currentIndexChanged, this, [this] { refreshBitDepths(mapBitDepth_->currentData().toInt()); });
    connect(mapAdvanced_, &QCheckBox::toggled, this, [this](bool on) {
        packing_->setVisible(on);
        adjustSize();
    });
    connect(mapNormal_, &QComboBox::currentIndexChanged, this, [this] { packingEdited(); });
    connect(addFile, &QAbstractButton::clicked, this, [this] { addMapFile(); });
    connect(savePreset, &QAbstractButton::clicked, this, [this] { saveMapPreset(mapSaveName_->text()); });
    connect(mapDelete_, &QAbstractButton::clicked, this, [this] { deleteMapPreset(); });
    refreshMapPresets(QString::fromLatin1(builtinCardMapPresets().front().name.c_str()));

    // ---- The output and the button.
    auto* pathRow = new QHBoxLayout();
    pathRow->setSpacing(6);
    path_ = new QLineEdit(this);
    path_->setPlaceholderText(tr("Output file"));
    path_->setFont(theme::monoFont(12));
    pathRow->addWidget(path_, 1);
    browse_ = new RoundButton(this);
    browse_->setText(QStringLiteral("..."));
    browse_->setDiameter(28);
    browse_->setRadius(9.0);
    browse_->setPadding(10);
    browse_->setFontSize(13);
    browse_->setFill(theme::chip(), theme::chip());
    browse_->setToolTip(tr("Choose the output file"));
    pathRow->addWidget(browse_);
    layout->addLayout(pathRow);
    export_ = new RoundButton(this);
    export_->setText(tr("Export"));
    export_->setIcon(QStringLiteral(":/groomlab/icons/ui_export.svg"), 16);
    export_->setIconColor(theme::actionText(theme::ink()), theme::actionText(theme::ink()));
    export_->setDiameter(38);
    export_->setRadius(13.0);
    export_->setPadding(16);
    export_->setFontSize(14);
    export_->setFill(theme::actionFill(theme::emerald()), theme::actionFill(theme::emerald()));
    export_->setTextColor(theme::actionText(theme::ink()), theme::actionText(theme::ink()));
    layout->addWidget(export_);
    progressBar_ = new ProgressBar(this);
    progressBar_->hide();
    layout->addWidget(progressBar_);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setFont(theme::uiFont(12, 500));
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    layout->addWidget(status_);

    // ---- Save, as before.
    auto* saveRow = new QHBoxLayout();
    saveRow->setSpacing(8);
    auto saveButton = [this](const QString& text, const QString& dim) {
        auto* b = new RoundButton(this);
        b->setText(text);
        b->setDimText(dim);
        b->setDiameter(32);
        b->setRadius(11.0);
        b->setPadding(12);
        b->setFontSize(13);
        b->setFill(theme::chip(), theme::chip());
        b->setTextColor(theme::text(), theme::text());
        return b;
    };
    RoundButton* save = saveButton(tr("Save"), QStringLiteral("Ctrl+S"));
    RoundButton* saveAs = saveButton(tr("Save as..."), QStringLiteral("Ctrl+Shift+S"));
    saveRow->addWidget(save);
    saveRow->addWidget(saveAs);
    saveRow->addStretch(1);
    layout->addLayout(saveRow);
    layout->addStretch(1);  // docked in a tall window: the rows at the top
    connect(save, &QAbstractButton::clicked, this, &ExportPanel::saveRequested);
    connect(saveAs, &QAbstractButton::clicked, this, &ExportPanel::saveAsRequested);

    connect(browse_, &QAbstractButton::clicked, this, &ExportPanel::browseRequested);
    connect(export_, &QAbstractButton::clicked, this, [this] {
        if (busy_) emit cancelRequested();
        else emit exportRequested();
    });
    connect(range_, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(binary_, &QCheckBox::toggled, this, [this] { setTarget(target_); });  // the extension follows
    setTarget(Target::Alembic);
}

void ExportPanel::setTarget(Target target) {
    target_ = target;
    // The path keeps its stem and takes the target's extension.
    const QString path = path_->text();
    if (!path.isEmpty()) {
        const QFileInfo info(path);
        const QString base = info.completeBaseName();
        QString stem = base;
        for (const TargetInfo& t : kTargets) {
            const QString suffix = QStringLiteral("_") + QLatin1String(t.name);
            if (stem.endsWith(suffix)) stem.chop(suffix.size());
        }
        const QString dir = info.path();
        // Card maps: <folder>/<name>, the files named after it (no suffix, no extension).
        const QString suffix = target == Target::CardMaps ? QString()
                                                           : QStringLiteral("_") + QLatin1String(targetName(target)) + QStringLiteral(".") +
                                                                 defaultExtension();
        path_->setText((dir.isEmpty() || dir == QStringLiteral(".") ? QString() : dir + QStringLiteral("/")) + stem + suffix);
    }
    path_->setPlaceholderText(target == Target::CardMaps ? tr("Output folder/name") : tr("Output file"));
    refresh();
}

QString ExportPanel::defaultExtension() const {
    if (target_ == Target::Cards && !binary_->isChecked()) return QStringLiteral("gltf");
    return QLatin1String(kTargets[static_cast<int>(target_)].extension);
}

ExportOptions ExportPanel::options() const {
    ExportOptions o;
    const bool range = range_->isChecked() && range_->isEnabled();
    o.frames.first = range ? first_->value() : frame_;
    o.frames.last = range ? std::max(first_->value(), last_->value()) : frame_;
    o.frames.step = range ? step_->value() : 1.0;
    o.density = static_cast<float>(density_->value());
    o.groups = groups_->isChecked();
    o.guides = guides_->isChecked();
    const int shafts = shafts_->currentData().toInt();
    if (shafts != 0) o.shafts = shafts == 1;
    o.featherGuides = featherGuides_->isChecked();
    o.iridescenceAo = iridescenceAo_->isChecked();
    o.barbDensity = static_cast<float>(barbDensity_->value());
    o.barbWidthScale = static_cast<float>(barbWidth_->value());
    o.barbPoints = barbPoints_->value();
    o.scale = static_cast<float>(scale_->value());
    o.pointsPerStrand = points_->value();
    o.resolution = resolution_->currentData().toInt();
    o.occlusion = static_cast<float>(occlusion_->value());
    o.binary = binary_->isChecked();
    o.mapPreset = mapPacking();
    o.mapSettings.resolution = mapResolution_->currentData().toInt();
    o.mapSettings.format = mapFormat_->currentData().toString().toStdString();
    o.mapSettings.bitDepth = mapBitDepth_->currentData().toInt();
    o.mapSettings.padding = mapPadding_->value();
    o.mapSettings.supersample = mapSupersample_->currentData().toInt();
    o.mapSettings.nameTemplate = mapTemplate_->text().trimmed().isEmpty() ? std::string("{name}_{map}") : mapTemplate_->text().trimmed().toStdString();
    return o;
}

// ---- Card maps

QString ExportPanel::mapPreset() const { return mapPreset_->currentText(); }

void ExportPanel::refreshMapPresets(const QString& select) {
    mapPresets_ = cardMapPresets();
    filling_ = true;
    mapPreset_->clear();
    for (const CardMapPreset& p : mapPresets_) {
        mapPreset_->addItem(QString::fromStdString(p.name));
        mapPreset_->setItemData(mapPreset_->count() - 1, QString::fromStdString(p.description), Qt::ToolTipRole);
    }
    filling_ = false;
    if (!setMapPreset(select)) setMapPreset(QString::fromStdString(mapPresets_.front().name));
}

bool ExportPanel::setMapPreset(const QString& name) {
    const auto it = std::find_if(mapPresets_.begin(), mapPresets_.end(), [&](const CardMapPreset& p) { return QString::fromStdString(p.name) == name; });
    if (it == mapPresets_.end()) return false;
    filling_ = true;
    const int custom = mapPreset_->findText(QLatin1String(kCustomPreset));
    if (custom >= 0) mapPreset_->removeItem(custom);
    mapPreset_->setCurrentIndex(mapPreset_->findText(name));
    mapDescription_->setText(QString::fromStdString(it->description));
    mapDelete_->setEnabled(!it->builtin);
    mapNormal_->setCurrentIndex(std::max(0, mapNormal_->findData(QString::fromStdString(it->normal))));
    mapFormat_->setCurrentIndex(std::max(0, mapFormat_->findData(QString::fromStdString(it->format))));
    refreshBitDepths(it->bitDepth);
    fillPacking(it->files);
    filling_ = false;
    return true;
}

void ExportPanel::refreshBitDepths(int prefer) {
    const bool was = mapBitDepth_->blockSignals(true);
    mapBitDepth_->clear();
    const std::string format = mapFormat_->currentData().toString().toStdString();
    for (const int depth : cardMapBitDepths(format)) {
        const QString label = depth == 32 ? tr("32-bit float") : depth == 16 && format == "exr" ? tr("16-bit half") : tr("%1-bit").arg(depth);
        mapBitDepth_->addItem(label, depth);
    }
    const int index = mapBitDepth_->findData(prefer);
    mapBitDepth_->setCurrentIndex(index >= 0 ? index : 0);
    mapBitDepth_->blockSignals(was);
}

void ExportPanel::fillPacking(const std::vector<CardMapFile>& files) {
    for (PackRow& row : packRows_) delete row.box;
    packRows_.clear();
    for (const CardMapFile& f : files) addPackRow(f);
    adjustSize();
}

void ExportPanel::addPackRow(const CardMapFile& file) {
    // A file: its name, color space and remove on top; R G B A in a 2 x 2 grid under it.
    PackRow row;
    row.box = new QWidget(packing_);
    row.box->setObjectName(QStringLiteral("mapFile"));
    auto* v = new QVBoxLayout(row.box);
    v->setContentsMargins(0, 4, 0, 4);
    v->setSpacing(4);
    auto* top = new QHBoxLayout();
    top->setSpacing(6);
    row.name = new QLineEdit(QString::fromStdString(file.map), row.box);
    row.name->setToolTip(tr("The file's {map} in the name template (hair_<this>.tga)"));
    top->addWidget(row.name, 1);
    row.space = new QComboBox(row.box);
    row.space->addItem(tr("Auto"), QStringLiteral("auto"));
    row.space->addItem(tr("sRGB"), QStringLiteral("srgb"));
    row.space->addItem(tr("Linear"), QStringLiteral("linear"));
    row.space->setCurrentIndex(std::max(0, row.space->findData(QString::fromStdString(file.colorSpace))));
    row.space->setToolTip(tr("How color channels are written: Auto (sRGB, linear in EXR), sRGB or Linear; data is always linear"));
    top->addWidget(row.space);
    auto* remove = new QPushButton(QStringLiteral("×"), row.box);
    remove->setFixedWidth(24);
    remove->setToolTip(tr("Remove this file"));
    top->addWidget(remove);
    v->addLayout(top);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(2);
    static const char* letters[] = {"R", "G", "B", "A"};
    for (int c = 0; c < 4; ++c) {
        auto* combo = new QComboBox(row.box);
        if (c > 0) combo->addItem(QString::fromUtf8(kNoChannel));
        for (const std::string& name : cardMapChannelNames()) combo->addItem(QString::fromStdString(name));
        combo->addItem(QStringLiteral("iridescence?"));
        combo->setToolTip(tr("The map channel this file channel holds (0 or 1: a constant; iridescence?: only when the groom has any)"));
        auto* invert = new QCheckBox(tr("inv"), row.box);
        invert->setToolTip(tr("Inverted: 1 - the value"));
        QString value = c < static_cast<int>(file.channels.size()) ? QString::fromStdString(file.channels[static_cast<std::size_t>(c)]) : QString();
        if (value.startsWith(QStringLiteral("1-"))) {
            invert->setChecked(true);
            value = value.mid(2);
        }
        int index = combo->findText(value.isEmpty() ? QString::fromUtf8(kNoChannel) : value);
        if (index < 0 && !value.isEmpty()) {
            combo->addItem(value);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(std::max(index, 0));
        grid->addWidget(captionLabel(QLatin1String(letters[c]), row.box), c / 2, (c % 2) * 3);
        grid->addWidget(combo, c / 2, (c % 2) * 3 + 1);
        grid->addWidget(invert, c / 2, (c % 2) * 3 + 2);
        row.channels[c] = combo;
        row.invert[c] = invert;
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { packingEdited(); });
        connect(invert, &QCheckBox::toggled, this, [this] { packingEdited(); });
    }
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(4, 1);
    v->addLayout(grid);
    connect(row.name, &QLineEdit::textEdited, this, [this] { packingEdited(); });
    connect(row.space, &QComboBox::currentIndexChanged, this, [this] { packingEdited(); });
    QWidget* box = row.box;
    connect(remove, &QPushButton::clicked, this, [this, box] {
        for (std::size_t i = 0; i < packRows_.size(); ++i) {
            if (packRows_[i].box == box) removeMapFile(static_cast<int>(i));
        }
    });
    packRowsLayout_->addWidget(row.box);
    packRows_.push_back(row);
}

void ExportPanel::packingEdited() {
    if (filling_) return;
    // Edited by hand: the preset is Custom now (save it to keep it).
    filling_ = true;
    if (mapPreset_->findText(QLatin1String(kCustomPreset)) < 0) mapPreset_->addItem(QLatin1String(kCustomPreset));
    mapPreset_->setCurrentIndex(mapPreset_->findText(QLatin1String(kCustomPreset)));
    mapDescription_->setText(tr("Your own packing: save it as a preset to keep it"));
    mapDelete_->setEnabled(false);
    filling_ = false;
}

CardMapPreset ExportPanel::mapPacking() const {
    CardMapPreset p;
    p.name = mapPreset_->currentText().toStdString();
    p.format = mapFormat_->currentData().toString().toStdString();
    p.bitDepth = mapBitDepth_->currentData().toInt();
    p.normal = mapNormal_->currentData().toString().toStdString();
    for (const PackRow& row : packRows_) {
        CardMapFile f;
        f.map = row.name->text().trimmed().toStdString();
        f.colorSpace = row.space->currentData().toString().toStdString();
        for (int c = 0; c < 4; ++c) {
            const QString text = row.channels[c]->currentText();
            if (text == QString::fromUtf8(kNoChannel)) {
                f.channels.emplace_back();
                continue;
            }
            const bool invert = row.invert[c]->isChecked() && text != QStringLiteral("0") && text != QStringLiteral("1");
            f.channels.push_back((invert ? "1-" : "") + text.toStdString());
        }
        while (!f.channels.empty() && f.channels.back().empty()) f.channels.pop_back();
        for (std::string& c : f.channels) {
            if (c.empty()) c = "0";  // a gap in the middle: 0
        }
        p.files.push_back(std::move(f));
    }
    return p;
}

void ExportPanel::setMapChannel(int file, int channel, const QString& value, bool invert) {
    if (file < 0 || file >= mapFileCount() || channel < 0 || channel > 3) return;
    QComboBox* combo = packRows_[static_cast<std::size_t>(file)].channels[channel];
    const int index = combo->findText(value.isEmpty() ? QString::fromUtf8(kNoChannel) : value);
    if (index >= 0) combo->setCurrentIndex(index);
    packRows_[static_cast<std::size_t>(file)].invert[channel]->setChecked(invert);
}

void ExportPanel::addMapFile() {
    CardMapFile f;
    f.map = "Map" + std::to_string(packRows_.size() + 1);
    f.channels = {"depth"};
    addPackRow(f);
    packingEdited();
    adjustSize();
}

void ExportPanel::removeMapFile(int file) {
    if (file < 0 || file >= mapFileCount()) return;
    packRows_[static_cast<std::size_t>(file)].box->deleteLater();
    packRows_.erase(packRows_.begin() + file);
    packingEdited();
    adjustSize();
}

bool ExportPanel::saveMapPreset(const QString& nameIn) {
    const QString name = nameIn.trimmed();
    if (name.isEmpty()) {
        setStatus(tr("Type the preset's name beside Save as preset"), true);
        return false;
    }
    CardMapPreset p = mapPacking();
    p.name = name.toStdString();
    try {
        const std::string path = saveCardMapPreset(p);
        refreshMapPresets(name);
        setStatus(tr("Saved the preset %1 (%2)").arg(name, QString::fromStdString(path)));
        return true;
    } catch (const std::exception& e) {
        setStatus(tr("Can't save: %1").arg(QString::fromUtf8(e.what())), true);
        return false;
    }
}

bool ExportPanel::deleteMapPreset() {
    const QString name = mapPreset_->currentText();
    const auto it = std::find_if(mapPresets_.begin(), mapPresets_.end(), [&](const CardMapPreset& p) { return QString::fromStdString(p.name) == name; });
    if (it == mapPresets_.end() || it->builtin) return false;
    const bool done = deleteCardMapPreset(name.toStdString());
    refreshMapPresets(QString::fromStdString(builtinCardMapPresets().front().name));
    setStatus(done ? tr("Deleted the preset %1").arg(name) : tr("Couldn't delete %1").arg(name), !done);
    return done;
}

void ExportPanel::setMapSettings(int resolution, const QString& format, int bitDepth, int padding, int supersample) {
    const int r = mapResolution_->findData(resolution);
    if (r >= 0) {
        mapResolution_->setCurrentIndex(r);
    } else {
        mapResolution_->addItem(QString::number(resolution), resolution);
        mapResolution_->setCurrentIndex(mapResolution_->count() - 1);
    }
    const int f = mapFormat_->findData(format.toLower() == QStringLiteral("tif") ? QStringLiteral("tiff") : format.toLower());
    if (f >= 0) mapFormat_->setCurrentIndex(f);
    refreshBitDepths(bitDepth);
    mapPadding_->setValue(padding);
    const int s = mapSupersample_->findData(supersample);
    if (s >= 0) mapSupersample_->setCurrentIndex(s);
}

QString ExportPanel::outputPath() const { return path_->text().trimmed(); }

void ExportPanel::setOutputPath(const QString& path) { path_->setText(path); }

void ExportPanel::setDocumentState(bool open, double frame, bool animated, double first, double last, const QString& groomPath) {
    updating_ = true;
    frame_ = frame;
    range_->setEnabled(open && animated);
    if (!animated) range_->setChecked(false);
    first_->setValue(static_cast<int>(std::lround(first)));
    last_->setValue(static_cast<int>(std::lround(last)));
    export_->setEnabled(open);
    // The output beside the groom: "<stem>_<target>.<ext>" (an unsaved groom: the user picks).
    const QString stem = groomPath.isEmpty() ? QString() : QFileInfo(groomPath).completeBaseName();
    if (stem != groomStem_) {
        groomStem_ = stem;
        if (!stem.isEmpty()) {
            const QString suffix = target_ == Target::CardMaps ? QString()
                                                                : QStringLiteral("_") + QLatin1String(targetName(target_)) + QStringLiteral(".") +
                                                                      defaultExtension();
            path_->setText(QFileInfo(groomPath).path() + QStringLiteral("/") + stem + suffix);
        }
    }
    updating_ = false;
    refresh();
}

void ExportPanel::setProgress(std::size_t done, std::size_t count) {
    done_ = done;
    count_ = count;
    static_cast<ProgressBar*>(progressBar_)->set(count > 0 ? static_cast<double>(done) / static_cast<double>(count) : 0.0);
    progressBar_->setVisible(busy_ || done > 0);
    if (busy_) status_->setText(tr("Frame %1 of %2...").arg(done).arg(count));
}

void ExportPanel::setBusy(bool busy) {
    busy_ = busy;
    export_->setText(busy ? tr("Cancel") : tr("Export"));
    const QColor fill = busy ? theme::chip() : theme::actionFill(theme::emerald());
    const QColor ink = busy ? theme::text() : theme::actionText(theme::ink());
    export_->setFill(fill, fill);
    export_->setTextColor(ink, ink);
    progressBar_->setVisible(busy);
    for (RoundButton* b : targets_) b->setEnabled(!busy);
    if (busy) static_cast<ProgressBar*>(progressBar_)->set(0.0);
}

void ExportPanel::setStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(QStringLiteral("color: %1;").arg((error ? theme::errorText() : theme::textDim()).name()));
}

void ExportPanel::refresh() {
    for (int i = 0; i < static_cast<int>(targets_.size()); ++i) targets_[static_cast<std::size_t>(i)]->setChecked(i == static_cast<int>(target_));
    description_->setText(tr(kTargets[static_cast<int>(target_)].description));
    const bool maps = target_ == Target::CardMaps;
    const bool cards = target_ == Target::Cards;
    const bool range = range_->isChecked() && range_->isEnabled() && !cards && !maps;
    range_->setVisible(!cards && !maps);
    first_->setEnabled(range);
    last_->setEnabled(range);
    step_->setEnabled(range);
    engineOptions_->setVisible(target_ == Target::Unreal || target_ == Target::Unity);
    unrealOptions_->setVisible(target_ == Target::Unreal);
    unityOptions_->setVisible(target_ == Target::Unity);
    cardOptions_->setVisible(cards);
    mapOptions_->setVisible(maps);
    density_->setVisible(!cards && !maps);
    adjustSize();
}

}  // namespace groomlab::app
