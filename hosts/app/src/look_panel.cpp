// SPDX-License-Identifier: Apache-2.0
#include "look_panel.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QVBoxLayout>

#include "glass.h"
#include "theme.h"

namespace groomlab::app {

namespace {

constexpr float kPi = 3.14159265358979f;

// A light's direction as the panel edits it: the azimuth around y and the elevation.
void anglesOf(const std::array<float, 3>& d, double& azimuth, double& elevation) {
    elevation = std::asin(std::clamp(d[1], -1.0f, 1.0f)) * 180.0 / kPi;
    azimuth = std::atan2(d[0], d[2]) * 180.0 / kPi;
}

// A fill field (glass.h): the caption and the number inside it, the number edited inline.
ValueSlider* makeSlider(const QString& caption, double min, double max, double step, int decimals, QWidget* parent) {
    auto* s = new ValueSlider(parent);
    s->setCaption(caption);
    s->setRange(min, max);
    s->setLimits(min, max);
    s->setStep(step);
    s->setDecimals(decimals);
    s->setFill(theme::blue());
    return s;
}

}  // namespace

LookPanel::LookPanel(QWidget* parent) : QWidget(parent) {
    material_ = hairPreset("groom");
    lighting_ = lightingPreset("studio");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget(scroll);
    auto* layout = new QVBoxLayout(host);
    scroll->setWidget(host);
    outer->addWidget(scroll);

    auto* materialBox = new QGroupBox(tr("Hair material (aiStandardHair)"), host);
    auto* materialForm = new QFormLayout(materialBox);
    layout->addWidget(materialBox);
    auto* surfaceBox = new QGroupBox(tr("Surfaces (scalp, props, eyes)"), host);
    surfaceForm_ = new QFormLayout(surfaceBox);
    layout->addWidget(surfaceBox);
    auto* lightingBox = new QGroupBox(tr("Lighting"), host);
    auto* lightingForm = new QFormLayout(lightingBox);
    layout->addWidget(lightingBox);
    layout->addStretch(1);

    // ---- Material ---------------------------------------------------------------------------
    materialPreset_ = new QComboBox(materialBox);
    for (const std::string& name : hairPresetNames()) materialPreset_->addItem(QString::fromStdString(name));
    materialForm->addRow(tr("Preset"), materialPreset_);
    connect(materialPreset_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (updating_) return;
        applyMaterialPreset(name);
    });
    pigment_ = new QComboBox(materialBox);
    pigment_->addItems({tr("Groom melanin channels"), tr("Groom RGB channels"), tr("This material")});
    pigment_->setToolTip(tr("Where the pigment comes from: the groom's melanin and melaninRedness channels (as the Arnold "
                            "translator reads them), its colorR/G/B channels as Arnold's base color, or the values below"));
    materialForm->addRow(tr("Pigment"), pigment_);
    connect(pigment_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        material_.pigment = static_cast<HairMaterial::Pigment>(index);
        material_.presetName.clear();
        emit materialChanged();
    });
    auto m = [this](auto member) { return [this, member]() -> double { return material_.*member; }; };
    auto ms = [this](auto member) {
        return [this, member](double v) {
            material_.*member = static_cast<float>(v);
            material_.presetName.clear();
        };
    };
    slider(materialForm, tr("Melanin"), 0.0, 1.0, 0.05, 3, m(&HairMaterial::melanin), ms(&HairMaterial::melanin), Target::Hair);
    slider(materialForm, tr("Melanin redness"), 0.0, 1.0, 0.05, 3, m(&HairMaterial::melaninRedness), ms(&HairMaterial::melaninRedness), Target::Hair);
    slider(materialForm, tr("Melanin randomize"), 0.0, 1.0, 0.05, 3, m(&HairMaterial::melaninRandomize), ms(&HairMaterial::melaninRandomize), Target::Hair);
    for (int k = 0; k < 3; ++k) {
        const QString names[3] = {tr("Base color R"), tr("Base color G"), tr("Base color B")};
        slider(materialForm, names[k], 0.0, 1.0, 0.05, 3, [this, k] { return material_.baseColor[k]; },
             [this, k](double v) {
                 material_.baseColor[k] = static_cast<float>(v);
                 material_.presetName.clear();
             }, Target::Hair);
    }
    slider(materialForm, tr("Roughness"), 0.02, 1.0, 0.05, 3, m(&HairMaterial::roughness), ms(&HairMaterial::roughness), Target::Hair);
    slider(materialForm, tr("Roughness azimuthal"), 0.0, 1.0, 0.05, 3, m(&HairMaterial::azimuthalRoughness), ms(&HairMaterial::azimuthalRoughness), Target::Hair);
    slider(materialForm, tr("IOR"), 1.0, 2.5, 0.01, 3, m(&HairMaterial::ior), ms(&HairMaterial::ior), Target::Hair);
    slider(materialForm, tr("Shift (degrees)"), 0.0, 10.0, 0.5, 2, m(&HairMaterial::shift), ms(&HairMaterial::shift), Target::Hair);
    slider(materialForm, tr("Specular (R)"), 0.0, 2.0, 0.1, 3, m(&HairMaterial::specular), ms(&HairMaterial::specular), Target::Hair);
    slider(materialForm, tr("Specular 2 (TRT)"), 0.0, 2.0, 0.1, 3, m(&HairMaterial::specular2), ms(&HairMaterial::specular2), Target::Hair);
    slider(materialForm, tr("Transmission (TT)"), 0.0, 2.0, 0.1, 3, m(&HairMaterial::transmission), ms(&HairMaterial::transmission), Target::Hair);
    slider(materialForm, tr("Diffuse"), 0.0, 1.0, 0.05, 3, m(&HairMaterial::diffuse), ms(&HairMaterial::diffuse), Target::Hair);
    slider(materialForm, tr("Multiple scattering"), 0.0, 2.0, 0.1, 3, m(&HairMaterial::multipleScattering), ms(&HairMaterial::multipleScattering), Target::Hair);
    slider(materialForm, tr("Width scale"), 0.05, 20.0, 0.25, 3, m(&HairMaterial::widthScale), ms(&HairMaterial::widthScale), Target::Hair);
    slider(materialForm, tr("Shadow density"), 0.0, 4.0, 0.1, 3, m(&HairMaterial::shadowDensity), ms(&HairMaterial::shadowDensity), Target::Hair);

    // ---- Surfaces ---------------------------------------------------------------------------
    surface_ = new QComboBox(surfaceBox);
    surface_->setToolTip(tr("The surface edited below: the scalp, then the props and eyes in the order they were loaded"));
    surfaceForm_->addRow(tr("Surface"), surface_);
    connect(surface_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updating_) return;
        refreshSurface();
    });
    surfacePreset_ = new QComboBox(surfaceBox);
    for (const std::string& name : surfacePresetNames()) surfacePreset_->addItem(QString::fromStdString(name));
    surfaceForm_->addRow(tr("Preset"), surfacePreset_);
    connect(surfacePreset_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (updating_) return;
        applySurfacePreset(surface_->currentIndex(), name);
    });
    surfaceKind_ = new QComboBox(surfaceBox);
    surfaceKind_->addItems({tr("Default (metal/roughness)"), tr("Skin"), tr("Eye")});
    surfaceForm_->addRow(tr("Kind"), surfaceKind_);
    connect(surfaceKind_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        if (SurfaceMaterial* m = currentSurface()) {
            m->kind = static_cast<SurfaceKind>(index);
            surfaceEdited();
            refreshSurface();
        }
    });
    const int kDefault = 1, kSkin = 2, kEye = 4, kAll = 7;
    auto sg = [this](auto member) { return [this, member]() -> double { SurfaceMaterial* m = currentSurface(); return m ? m->*member : 0.0; }; };
    auto ss = [this](auto member) { return [this, member](double v) { if (SurfaceMaterial* m = currentSurface()) m->*member = static_cast<float>(v); }; };
    auto color = [&](const QString& label, RGB SurfaceMaterial::*member, int kinds) {
        for (int k = 0; k < 3; ++k) {
            const QString names[3] = {label + tr(" R"), label + tr(" G"), label + tr(" B")};
            slider(surfaceForm_, names[k], 0.0, 1.0, 0.05, 3, [this, member, k] { SurfaceMaterial* m = currentSurface(); return m ? (m->*member)[k] : 0.0; },
                 [this, member, k](double v) { if (SurfaceMaterial* m = currentSurface()) (m->*member)[k] = static_cast<float>(v); }, Target::Surface, kinds);
        }
    };
    color(tr("Base color"), &SurfaceMaterial::baseColor, kDefault | kSkin);
    slider(surfaceForm_, tr("Roughness"), 0.02, 1.0, 0.05, 3, sg(&SurfaceMaterial::roughness), ss(&SurfaceMaterial::roughness), Target::Surface, kAll);
    slider(surfaceForm_, tr("Metallic"), 0.0, 1.0, 0.05, 3, sg(&SurfaceMaterial::metallic), ss(&SurfaceMaterial::metallic), Target::Surface, kDefault);
    slider(surfaceForm_, tr("Specular (F0 scale)"), 0.0, 1.0, 0.05, 3, sg(&SurfaceMaterial::specular), ss(&SurfaceMaterial::specular), Target::Surface, kAll);
    slider(surfaceForm_, tr("Occlusion"), 0.0, 1.0, 0.1, 2, sg(&SurfaceMaterial::occlusion), ss(&SurfaceMaterial::occlusion), Target::Surface, kDefault | kSkin);
    slider(surfaceForm_, tr("Normal strength"), 0.0, 3.0, 0.1, 2, sg(&SurfaceMaterial::normalStrength), ss(&SurfaceMaterial::normalStrength), Target::Surface, kDefault | kSkin);
    slider(surfaceForm_, tr("Detail scale"), 0.0, 400.0, 5.0, 1, sg(&SurfaceMaterial::detailScale), ss(&SurfaceMaterial::detailScale), Target::Surface, kDefault | kSkin);
    slider(surfaceForm_, tr("Detail strength"), 0.0, 2.0, 0.05, 2, sg(&SurfaceMaterial::detailStrength), ss(&SurfaceMaterial::detailStrength), Target::Surface, kDefault | kSkin);
    slider(surfaceForm_, tr("Specular map gain"), 0.0, 8.0, 0.25, 2, sg(&SurfaceMaterial::specularMapGain), ss(&SurfaceMaterial::specularMapGain), Target::Surface, kDefault | kSkin);
    slider(surfaceForm_, tr("Cavity"), 0.0, 1.0, 0.1, 2, sg(&SurfaceMaterial::cavity), ss(&SurfaceMaterial::cavity), Target::Surface, kDefault | kSkin);
    slider(surfaceForm_, tr("Scatter radius"), 0.05, 5.0, 0.1, 2, sg(&SurfaceMaterial::sssRadius), ss(&SurfaceMaterial::sssRadius), Target::Surface, kSkin);
    slider(surfaceForm_, tr("Scatter strength"), 0.0, 1.0, 0.1, 2, sg(&SurfaceMaterial::sssStrength), ss(&SurfaceMaterial::sssStrength), Target::Surface, kSkin);
    slider(surfaceForm_, tr("Roughness 2"), 0.02, 1.0, 0.05, 3, sg(&SurfaceMaterial::roughness2), ss(&SurfaceMaterial::roughness2), Target::Surface, kSkin);
    slider(surfaceForm_, tr("Lobe 2 weight"), 0.0, 1.0, 0.05, 2, sg(&SurfaceMaterial::lobe2Weight), ss(&SurfaceMaterial::lobe2Weight), Target::Surface, kSkin);
    slider(surfaceForm_, tr("Transmission"), 0.0, 4.0, 0.1, 2, sg(&SurfaceMaterial::transmission), ss(&SurfaceMaterial::transmission), Target::Surface, kSkin);
    color(tr("Iris color"), &SurfaceMaterial::irisColor, kEye);
    color(tr("Sclera color"), &SurfaceMaterial::scleraColor, kEye);
    slider(surfaceForm_, tr("Iris radius"), 0.2, 0.7, 0.01, 3, sg(&SurfaceMaterial::irisRadius), ss(&SurfaceMaterial::irisRadius), Target::Surface, kEye);
    slider(surfaceForm_, tr("Pupil radius"), 0.05, 0.8, 0.02, 3, sg(&SurfaceMaterial::pupilRadius), ss(&SurfaceMaterial::pupilRadius), Target::Surface, kEye);
    slider(surfaceForm_, tr("Limbal width"), 0.0, 0.5, 0.02, 3, sg(&SurfaceMaterial::limbalWidth), ss(&SurfaceMaterial::limbalWidth), Target::Surface, kEye);
    slider(surfaceForm_, tr("Cornea IOR"), 1.0, 2.0, 0.01, 3, sg(&SurfaceMaterial::corneaIor), ss(&SurfaceMaterial::corneaIor), Target::Surface, kEye);
    slider(surfaceForm_, tr("Iris detail"), 0.0, 2.0, 0.1, 2, sg(&SurfaceMaterial::irisDetail), ss(&SurfaceMaterial::irisDetail), Target::Surface, kEye);
    slider(surfaceForm_, tr("Veins"), 0.0, 2.0, 0.1, 2, sg(&SurfaceMaterial::veins), ss(&SurfaceMaterial::veins), Target::Surface, kEye);
    slider(surfaceForm_, tr("Lid occlusion"), 0.0, 6.0, 0.25, 2, sg(&SurfaceMaterial::eyeOcclusion), ss(&SurfaceMaterial::eyeOcclusion), Target::Surface, kEye);
    slider(surfaceForm_, tr("Tear line"), 0.0, 2.0, 0.1, 2, sg(&SurfaceMaterial::tearLine), ss(&SurfaceMaterial::tearLine), Target::Surface, kEye);
    slider(surfaceForm_, tr("Wetness"), 0.0, 2.0, 0.1, 2, sg(&SurfaceMaterial::wetness), ss(&SurfaceMaterial::wetness), Target::Surface, kEye);
    auto pg = [this](auto member) { return [this, member]() -> std::string { SurfaceMaterial* m = currentSurface(); return m ? m->*member : std::string(); }; };
    auto ps = [this](auto member) { return [this, member](const std::string& v) { if (SurfaceMaterial* m = currentSurface()) m->*member = v; }; };
    pathEdit(surfaceForm_, tr("Albedo map"), pg(&SurfaceMaterial::albedoMap), ps(&SurfaceMaterial::albedoMap), kDefault | kSkin);
    pathEdit(surfaceForm_, tr("Normal map"), pg(&SurfaceMaterial::normalMap), ps(&SurfaceMaterial::normalMap), kDefault | kSkin);
    pathEdit(surfaceForm_, tr("Roughness map"), pg(&SurfaceMaterial::roughnessMap), ps(&SurfaceMaterial::roughnessMap), kDefault | kSkin);
    pathEdit(surfaceForm_, tr("Metallic map"), pg(&SurfaceMaterial::metallicMap), ps(&SurfaceMaterial::metallicMap), kDefault);
    pathEdit(surfaceForm_, tr("Occlusion map"), pg(&SurfaceMaterial::occlusionMap), ps(&SurfaceMaterial::occlusionMap), kDefault | kSkin);
    pathEdit(surfaceForm_, tr("Specular map"), pg(&SurfaceMaterial::specularMap), ps(&SurfaceMaterial::specularMap), kDefault | kSkin);

    // ---- Lighting ---------------------------------------------------------------------------
    lightingPreset_ = new QComboBox(lightingBox);
    for (const std::string& name : lightingPresetNames()) lightingPreset_->addItem(QString::fromStdString(name));
    lightingForm->addRow(tr("Preset"), lightingPreset_);
    connect(lightingPreset_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (updating_) return;
        applyLightingPreset(name);
    });
    quality_ = new QComboBox(lightingBox);
    quality_->addItems({tr("Interactive"), tr("Refine when idle")});
    quality_->setToolTip(tr("Refine: once nothing moves, the viewport keeps accumulating jittered frames (anti-aliasing, "
                            "exact strand transparency, every environment light, softer shadows) until the image converges"));
    lightingForm->addRow(tr("Quality"), quality_);
    connect(quality_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        lighting_.quality = static_cast<Quality>(index);
        emit lightingChanged();
    });
    for (int i = 0; i < 3; ++i) {
        LightRow row;
        row.host = new QWidget(lightingBox);
        auto* h = new QHBoxLayout(row.host);
        h->setContentsMargins(0, 0, 0, 0);
        row.enabled = new QCheckBox(row.host);
        row.enabled->setToolTip(tr("On"));
        row.azimuth = makeSlider(tr("Azimuth"), -180.0, 180.0, 5.0, 0, row.host);
        row.azimuth->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral("°"); });
        row.azimuth->setToolTip(tr("Azimuth around the vertical, degrees"));
        row.elevation = makeSlider(tr("Elevation"), -90.0, 90.0, 5.0, 0, row.host);
        row.elevation->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral("°"); });
        row.elevation->setToolTip(tr("Elevation above the horizon, degrees"));
        row.intensity = makeSlider(tr("Intensity"), 0.0, 50.0, 0.25, 2, row.host);
        row.intensity->setFill(theme::yellow());
        row.intensity->setToolTip(tr("Intensity"));
        row.shadows = new QCheckBox(tr("shadows"), row.host);
        row.shadows->setToolTip(tr("Casts a deep opacity map: hair-on-hair shadows, the scalp in them"));
        h->addWidget(row.enabled);
        h->addWidget(row.azimuth);
        h->addWidget(row.elevation);
        h->addWidget(row.intensity);
        h->addWidget(row.shadows);
        lightingForm->addRow(tr("Light %1").arg(i + 1), row.host);
        auto changed = [this] {
            if (updating_) return;
            readLights();
            lighting_.presetName.clear();
            emit lightingChanged();
        };
        connect(row.enabled, &QCheckBox::toggled, this, changed);
        connect(row.shadows, &QCheckBox::toggled, this, changed);
        connect(row.azimuth, &ValueSlider::valueChanged, this, changed);
        connect(row.elevation, &ValueSlider::valueChanged, this, changed);
        connect(row.intensity, &ValueSlider::valueChanged, this, changed);
        lightRows_.push_back(row);
    }
    environment_ = new QComboBox(lightingBox);
    environment_->addItems({tr("None"), tr("Studio"), tr("Sky"), tr("Overcast"), tr("Sunset")});
    lightingForm->addRow(tr("Environment"), environment_);
    connect(environment_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        lighting_.environment = static_cast<Environment>(index);
        lighting_.presetName.clear();
        emit lightingChanged();
    });
    auto l = [this](auto member) { return [this, member]() -> double { return lighting_.*member; }; };
    auto ls = [this](auto member) {
        return [this, member](double v) {
            lighting_.*member = static_cast<float>(v);
            lighting_.presetName.clear();
        };
    };
    slider(lightingForm, tr("Environment intensity"), 0.0, 10.0, 0.1, 2, l(&LightingSetup::environmentIntensity), ls(&LightingSetup::environmentIntensity), Target::Lighting);
    slider(lightingForm, tr("Environment rotation"), -180.0, 180.0, 10.0, 0, l(&LightingSetup::environmentRotation), ls(&LightingSetup::environmentRotation), Target::Lighting);
    slider(lightingForm, tr("Exposure (EV)"), -10.0, 10.0, 0.25, 2, l(&LightingSetup::exposure), ls(&LightingSetup::exposure), Target::Lighting);
    toneMap_ = new QComboBox(lightingBox);
    toneMap_->addItems({tr("None"), tr("ACES"), tr("AgX")});
    lightingForm->addRow(tr("Tone map"), toneMap_);
    connect(toneMap_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        lighting_.toneMap = static_cast<ToneMap>(index);
        emit lightingChanged();
    });
    background_ = new QComboBox(lightingBox);
    background_->addItems({tr("Solid color"), tr("Gradient"), tr("Environment")});
    lightingForm->addRow(tr("Background"), background_);
    connect(background_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        lighting_.background = static_cast<Background>(index);
        emit lightingChanged();
    });
    slider(lightingForm, tr("Background intensity"), 0.0, 4.0, 0.1, 2, l(&LightingSetup::backgroundIntensity), ls(&LightingSetup::backgroundIntensity), Target::Lighting);
    slider(lightingForm, tr("Shadow softness"), 0.05, 10.0, 0.25, 2, l(&LightingSetup::shadowSoftness), ls(&LightingSetup::shadowSoftness), Target::Lighting);
    ambientOcclusion_ = new QCheckBox(lightingBox);
    lightingForm->addRow(tr("Ambient occlusion"), ambientOcclusion_);
    connect(ambientOcclusion_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) return;
        lighting_.ambientOcclusion = on;
        emit lightingChanged();
    });

    refreshMaterial();
    refreshLighting();
    refreshSurface();
}

ValueSlider* LookPanel::slider(QFormLayout* form, const QString& label, double min, double max, double step, int decimals,
                               std::function<double()> get, std::function<void(double)> set, Target target, int kinds) {
    auto* s = makeSlider(label, min, max, step, decimals, form->parentWidget());
    form->addRow(s);  // the slider carries its caption: the row's whole width
    connect(s, &ValueSlider::valueChanged, this, [this, set, target](double v) {
        if (updating_) return;
        set(v);
        updating_ = true;
        if (target == Target::Hair) {
            materialPreset_->setCurrentIndex(-1);
        } else if (target == Target::Lighting) {
            lightingPreset_->setCurrentIndex(-1);
        } else {
            surfacePreset_->setCurrentIndex(-1);
        }
        updating_ = false;
        if (target == Target::Hair) emit materialChanged();
        else if (target == Target::Lighting) emit lightingChanged();
        else surfaceEdited();
    });
    auto refresher = [s, get] { s->setValue(get()); };
    (target == Target::Hair ? materialRefreshers_ : target == Target::Lighting ? lightingRefreshers_ : surfaceRefreshers_).push_back(refresher);
    if (target == Target::Surface) surfaceRows_.push_back({s, kinds});
    return s;
}

QLineEdit* LookPanel::pathEdit(QFormLayout* form, const QString& label, std::function<std::string()> get,
                               std::function<void(const std::string&)> set, int kinds) {
    auto* e = new QLineEdit(form->parentWidget());
    e->setPlaceholderText(tr("image file (none)"));
    form->addRow(label, e);
    connect(e, &QLineEdit::editingFinished, this, [this, e, set, get] {
        if (updating_) return;
        const std::string value = e->text().trimmed().toStdString();
        if (value == get()) return;
        set(value);
        surfaceEdited();
    });
    auto refresher = [e, get] { e->setText(QString::fromStdString(get())); };
    surfaceRefreshers_.push_back(refresher);
    surfaceRows_.push_back({e, kinds});
    return e;
}

SurfaceMaterial* LookPanel::currentSurface() {
    const int index = surface_ ? surface_->currentIndex() : -1;
    return index >= 0 && static_cast<std::size_t>(index) < surfaces_.size() ? &surfaces_[index].second : nullptr;
}

const SurfaceMaterial* LookPanel::surfaceMaterial(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < surfaces_.size() ? &surfaces_[index].second : nullptr;
}

void LookPanel::surfaceEdited() {
    if (SurfaceMaterial* m = currentSurface()) {
        m->presetName.clear();
        emit surfaceChanged(surface_->currentIndex(), *m);
    }
}

void LookPanel::setSurfaces(const std::vector<std::pair<QString, SurfaceMaterial>>& surfaces) {
    const int current = surface_->currentIndex();
    surfaces_ = surfaces;
    updating_ = true;
    surface_->clear();
    for (const auto& s : surfaces_) surface_->addItem(s.first);
    surface_->setCurrentIndex(surfaces_.empty() ? -1 : std::clamp(current, 0, static_cast<int>(surfaces_.size()) - 1));
    updating_ = false;
    refreshSurface();
}

void LookPanel::applySurfacePreset(int index, const QString& name) {
    if (index < 0 || static_cast<std::size_t>(index) >= surfaces_.size()) return;
    SurfaceMaterial preset = surfacePreset(name.toStdString());
    // The maps stay: a preset changes how the surface is shaded, not what it was loaded with.
    const SurfaceMaterial& was = surfaces_[index].second;
    preset.albedoMap = was.albedoMap;
    preset.normalMap = was.normalMap;
    preset.roughnessMap = was.roughnessMap;
    preset.metallicMap = was.metallicMap;
    preset.occlusionMap = was.occlusionMap;
    preset.specularMap = was.specularMap;
    surfaces_[index].second = preset;
    if (index == surface_->currentIndex()) refreshSurface();
    emit surfaceChanged(index, preset);
}

void LookPanel::refreshSurface() {
    updating_ = true;
    const SurfaceMaterial* m = currentSurface();
    const bool has = m != nullptr;
    surfacePreset_->setEnabled(has);
    surfaceKind_->setEnabled(has);
    if (has) {
        surfacePreset_->setCurrentText(QString::fromStdString(m->presetName));
        if (m->presetName.empty()) surfacePreset_->setCurrentIndex(-1);
        surfaceKind_->setCurrentIndex(static_cast<int>(m->kind));
        for (auto& r : surfaceRefreshers_) r();
    }
    const int kindBit = has ? 1 << static_cast<int>(m->kind) : 0;
    for (const SurfaceRow& row : surfaceRows_) {
        const bool shown = has && (row.kinds & kindBit);
        row.editor->setVisible(shown);
        if (QWidget* label = surfaceForm_->labelForField(row.editor)) label->setVisible(shown);
    }
    updating_ = false;
}

void LookPanel::refreshMaterial() {
    updating_ = true;
    materialPreset_->setCurrentText(QString::fromStdString(material_.presetName));
    if (material_.presetName.empty()) materialPreset_->setCurrentIndex(-1);
    pigment_->setCurrentIndex(static_cast<int>(material_.pigment));
    for (auto& r : materialRefreshers_) r();
    updating_ = false;
}

void LookPanel::refreshLighting() {
    updating_ = true;
    lightingPreset_->setCurrentText(QString::fromStdString(lighting_.presetName));
    if (lighting_.presetName.empty()) lightingPreset_->setCurrentIndex(-1);
    quality_->setCurrentIndex(static_cast<int>(lighting_.quality));
    environment_->setCurrentIndex(static_cast<int>(lighting_.environment));
    toneMap_->setCurrentIndex(static_cast<int>(lighting_.toneMap));
    background_->setCurrentIndex(static_cast<int>(lighting_.background));
    ambientOcclusion_->setChecked(lighting_.ambientOcclusion);
    for (std::size_t i = 0; i < lightRows_.size(); ++i) {
        LightRow& row = lightRows_[i];
        const bool has = i < lighting_.lights.size();
        row.host->setEnabled(has);
        if (!has) {
            row.enabled->setChecked(false);
            continue;
        }
        const DirectionalLight& light = lighting_.lights[i];
        double azimuth = 0.0, elevation = 0.0;
        anglesOf(light.direction, azimuth, elevation);
        row.enabled->setChecked(light.enabled);
        row.enabled->setToolTip(QString::fromStdString(light.name));
        row.azimuth->setValue(azimuth);
        row.elevation->setValue(elevation);
        row.intensity->setValue(light.intensity);
        row.shadows->setChecked(light.shadows);
    }
    for (auto& r : lightingRefreshers_) r();
    updating_ = false;
}

void LookPanel::readLights() {
    for (std::size_t i = 0; i < lightRows_.size() && i < lighting_.lights.size(); ++i) {
        const LightRow& row = lightRows_[i];
        DirectionalLight& light = lighting_.lights[i];
        light.enabled = row.enabled->isChecked();
        light.direction = lightDirection(static_cast<float>(row.azimuth->value()), static_cast<float>(row.elevation->value()));
        light.intensity = static_cast<float>(row.intensity->value());
        light.shadows = row.shadows->isChecked();
    }
}

void LookPanel::setMaterial(const HairMaterial& material) {
    material_ = material;
    refreshMaterial();
}

void LookPanel::setLighting(const LightingSetup& lighting) {
    lighting_ = lighting;
    refreshLighting();
}

void LookPanel::applyMaterialPreset(const QString& name) {
    material_ = hairPreset(name.toStdString());
    refreshMaterial();
    emit materialChanged();
}

void LookPanel::applyLightingPreset(const QString& name) {
    lighting_ = withLightingPreset(lighting_, name.toStdString());
    refreshLighting();
    emit lightingChanged();
}

}  // namespace groomlab::app
