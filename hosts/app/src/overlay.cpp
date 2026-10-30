// SPDX-License-Identifier: Apache-2.0
#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <nlohmann/json.hpp>

#include "groomlab/brush_catalog.h"
#include "groomlab/graph_io.h"

#include "param_editors.h"
#include "param_panel.h"
#include "theme.h"
#include "viewport.h"

namespace groomlab::app {

namespace {

// How the Look card shows a HairColor param: with the spec's decimals from the core's catalog
// (displayUnit, as the Node panel), trailing zeros dropped (0.35, not 0.350). The card's sliders
// hold the stored value (their inline edit types it), so a fraction is not shown as a percentage.
std::function<QString(double)> hairColorFormat(const std::string& param) {
    static const nlohmann::json hairColor = [] {
        for (const nlohmann::json& spec : nlohmann::json::parse(operatorSpecsJson())) {
            if (spec.value("type", std::string()) == "HairColor") return spec;
        }
        return nlohmann::json::object();
    }();
    DisplayUnit unit = displayUnit(std::string(), false, 2);
    for (const nlohmann::json& p : hairColor.value("params", nlohmann::json::array())) {
        if (p.value("name", std::string()) == param) {
            const std::string kind = p.value("unit", std::string());
            unit = displayUnit(kind == "fraction" ? std::string() : kind, false, p.value("decimals", -1));
        }
    }
    return [unit](double v) { return trimmedNumber(v, unit.decimals) + unit.suffix; };
}

QString iconPath(const char* name) { return QStringLiteral(":/groomlab/icons/ui_%1.svg").arg(QLatin1String(name)); }

QLabel* dotLabel(const QColor& color, int size, QWidget* parent) {
    auto* dot = new QLabel(parent);
    dot->setFixedSize(size, size);
    dot->setStyleSheet(QStringLiteral("background: %1; border-radius: %2px;").arg(color.name()).arg(size / 2));
    return dot;
}

// The hair color presets as swatches (the mockup's colors), then the groom's own pigment.
struct Swatch {
    const char* preset;
    QColor color;
    const char* tip;
};
const Swatch kSwatches[] = {
    {"groom", QColor(), "GroomLab's own pigment: HairColor's default color, no dye"},
    {"black", QColor(0x1B, 0x17, 0x15), "Black"},
    {"brown", QColor(0x5A, 0x3A, 0x22), "Brown"},
    {"blonde", QColor(0xD9, 0xB9, 0x7A), "Blonde"},
    {"red", QColor(0x9C, 0x3B, 0x1E), "Red"},
    {"white", QColor(0xE8, 0xE2, 0xD6), "White"},
    {"dyed", QColor(0xA3, 0x6B, 0xFF), "Dyed"},
};
const char* kLightingChips[] = {"studio", "daylight", "overcast", "sunset", "rim"};

}  // namespace

// ---- LookCard ------------------------------------------------------------------------------------

namespace {

constexpr float kDegrees = 180.0f / 3.14159265358979f;

// A light's direction as the card edits it: the azimuth around y and the elevation.
void lightAngles(const std::array<float, 3>& d, double& azimuth, double& elevation) {
    elevation = std::asin(std::clamp(d[1], -1.0f, 1.0f)) * kDegrees;
    azimuth = std::atan2(d[0], d[2]) * kDegrees;
}

Segmented* chipSegments(std::vector<Segmented::Item> items, QWidget* parent, const QString& tip = {}) {
    auto* s = new Segmented(parent);
    s->setItems(std::move(items));
    s->setBackground(theme::chip());
    s->setSegmentHeight(26);
    s->setPadding(3);
    s->setSegmentPadding(10);
    s->setFontSize(12);
    s->setStretch(true);
    if (!tip.isEmpty()) s->setToolTip(tip);
    return s;
}

QComboBox* comboBox(QWidget* parent, const QStringList& items, const QString& tip = {}) {
    auto* c = new QComboBox(parent);
    c->addItems(items);
    if (!tip.isEmpty()) c->setToolTip(tip);
    return c;
}

}  // namespace

LookCard::LookCard(QWidget* parent) : GlassPanel(parent, 24.0) {
    material_ = hairPreset("groom");
    lighting_ = lightingPreset("studio");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    tabs_ = new Segmented(this);
    tabs_->setItems({{tr("Material"), theme::red(), Qt::white}, {tr("Lighting"), theme::red(), Qt::white}, {tr("Render"), theme::red(), Qt::white}});
    tabs_->setBackground(theme::chip());
    tabs_->setSegmentHeight(34);
    tabs_->setPadding(3);
    tabs_->setSegmentPadding(12);
    tabs_->setFontSize(13);
    tabs_->setStretch(true);
    layout->addWidget(tabs_);

    // The pages in a scroll area: a tab taller than the view scrolls.
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    pages_ = new QWidget(scroll_);
    auto* pagesLayout = new QVBoxLayout(pages_);
    pagesLayout->setContentsMargins(0, 0, 0, 0);
    pagesLayout->setSpacing(0);
    scroll_->setWidget(pages_);
    layout->addWidget(scroll_, 1);

    auto page = [&] {
        auto* w = new QWidget(pages_);
        auto* l = new QVBoxLayout(w);
        l->setContentsMargins(0, 0, 4, 0);
        l->setSpacing(12);
        pagesLayout->addWidget(w);
        return w;
    };
    auto layoutOf = [](QWidget* w) { return static_cast<QVBoxLayout*>(w->layout()); };

    // ---- Material: the swatches, the three sliders that matter most, then everything.
    materialPage_ = page();
    QVBoxLayout* materialLayout = layoutOf(materialPage_);
    auto* swatchRow = new QWidget(materialPage_);
    auto* swatchLayout = new QHBoxLayout(swatchRow);
    swatchLayout->setContentsMargins(0, 0, 0, 0);
    swatchLayout->setSpacing(3);
    for (const Swatch& s : kSwatches) {
        auto* b = new RoundButton(swatchRow);
        b->setDiameter(28);  // seven of them fit beside the scroll bar
        b->setCheckable(true);
        b->setToolTip(tr(s.tip));
        if (s.color.isValid()) {
            b->setFill(s.color, s.color);
        } else {
            b->setFill(theme::chip(), theme::chip());
            b->setIcon(iconPath("logo"), 20);
            b->setIconColored(true);
        }
        b->setCheckedRing(theme::text(), 3.0);
        const std::string preset = s.preset;
        connect(b, &QAbstractButton::clicked, this, [this, preset] {
            if (!groomColor_.node.empty()) {
                // The groom's own color: the swatch's pigment (and dye) onto its HairColor node
                // (saved with the groom), the viewport showing the groom's channels; GroomLab's
                // own swatch puts HairColor's defaults back.
                const HairMaterial p = hairPreset(preset);
                groomColor_.melanin = p.melanin;
                groomColor_.redness = p.melaninRedness;
                groomColor_.picked = false;  // a swatch is a pigment: the natural mode
                material_.pigment = HairMaterial::Pigment::GroomMelanin;
                material_.presetName = preset;
                refresh();
                emit groomSwatchChosen(preset);
                emit materialEdited();
                return;
            }
            // The viewport's own settings (the width scale, the shadow density) are the user's.
            const float widthScale = material_.widthScale, shadowDensity = material_.shadowDensity;
            material_ = hairPreset(preset);
            material_.widthScale = widthScale;
            material_.shadowDensity = shadowDensity;
            refresh();
            emit materialEdited();
        });
        swatches_.push_back(b);
        swatchLayout->addWidget(b);
    }
    swatchLayout->addStretch(1);
    materialLayout->addWidget(swatchRow);
    auto m = [this](auto member) { return [this, member]() -> double { return material_.*member; }; };
    auto ms = [this](auto member, bool pigment = false) {
        return [this, member, pigment](double v) {
            material_.*member = static_cast<float>(v);
            // A drag on the pigment's own values: the pigment comes from here now.
            if (pigment) material_.pigment = HairMaterial::Pigment::Material;
            material_.presetName.clear();
        };
    };
    auto mc = [this](auto member) {
        return std::make_pair([this, member]() -> RGB { return material_.*member; },
                              [this, member](const RGB& c) {
                                  material_.*member = c;
                                  material_.presetName.clear();
                              });
    };
    // The three sliders that matter most edit the groom's HairColor node (its melanin, redness and
    // roughness): an undo step each, saved with the groom; without the node they edit the viewport
    // material, and the note under them says so.
    auto groomSlider = [this, materialLayout](const QString& caption, const QColor& fill, double min, double max, const char* param,
                                              float GroomColor::* member, auto materialSet) {
        auto* s = new ValueSlider(materialPage_);
        s->setStacked(true);
        s->setCaption(caption);
        s->setRange(min, max);
        s->setFill(fill);
        const std::string name = param;
        s->setFormat(hairColorFormat(name));
        materialLayout->addWidget(s);
        auto apply = [this, name, member, materialSet, s](bool live) {
            if (updating_) return;
            if (groomColor_.node.empty()) {
                materialSet(s->value());
                edited(Target::Material);
                return;
            }
            groomColor_.*member = static_cast<float>(s->value());
            material_.presetName.clear();
            updating_ = true;
            for (RoundButton* b : swatches_) b->setChecked(false);
            updating_ = false;
            emit groomColorEdited(name, s->value(), live);
        };
        connect(s, &ValueSlider::valueChanged, this, [apply](double) { apply(true); });
        connect(s, &ValueSlider::released, this, [apply] { apply(false); });
        return s;
    };
    // A color param of the groom's HairColor: a row with its editor, edits sent as picks.
    auto groomColorRow = [this, materialLayout](const QString& text, const char* param, QWidget** rowOut) {
        auto* host = new QWidget(materialPage_);
        auto* row = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto* label = new QLabel(text, host);
        label->setFont(theme::uiFont(13, 600));
        label->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setMinimumWidth(40);
        row->addWidget(label, 1);
        auto* editor = new ColorEditor(host);
        editor->setFixedWidth(112);
        row->addWidget(editor);
        materialLayout->addWidget(host);
        const std::string name = param;
        connect(editor, &ColorEditor::edited, this, [this, editor, name](bool live) {
            if (updating_ || groomColor_.node.empty()) return;
            (name == "tipColor" ? groomColor_.tipColor : groomColor_.color) = editor->linear();
            emit groomColorPicked(name, editor->linear(), live);
        });
        *rowOut = host;
        return editor;
    };
    // HairColor's mode: natural hair (melanin, redness) or a color picked as it renders.
    groomMode_ = new Segmented(materialPage_);
    groomMode_->setItems({{tr("Natural"), theme::red(), Qt::white}, {tr("Picked color"), theme::red(), Qt::white}});
    groomMode_->setBackground(theme::chip());
    groomMode_->setSegmentHeight(28);
    groomMode_->setPadding(3);
    groomMode_->setSegmentPadding(10);
    groomMode_->setFontSize(12);
    groomMode_->setStretch(true);
    groomMode_->setToolTip(tr("HairColor's mode. Natural (melanin): the pigment (melanin, redness), as real hair. "
                              "Picked color (color): the color you pick, rendered as picked (white hair: white)"));
    materialLayout->addWidget(groomMode_);
    connect(groomMode_, &Segmented::changed, this, [this](int index) {
        if (updating_ || groomColor_.node.empty()) return;
        groomColor_.picked = index == 1;
        refreshGroomColor();
        emit groomModeChosen(index == 1 ? "color" : "melanin");
    });
    groomPicked_ = groomColorRow(tr("Color"), "color", &groomPickedRow_);
    melanin_ = groomSlider(tr("Melanin"), theme::yellow(), 0.0, 1.0, "melanin", &GroomColor::melanin, ms(&HairMaterial::melanin, true));
    redness_ = groomSlider(tr("Redness"), theme::red(), 0.0, 1.0, "redness", &GroomColor::redness, ms(&HairMaterial::melaninRedness, true));
    roughness_ = groomSlider(tr("Roughness"), theme::blue(), 0.0, 1.0, "roughness", &GroomColor::roughness, ms(&HairMaterial::roughness));
    tipColor_ = groomColorRow(tr("Tip color"), "tipColor", &tipColorRow_);
    tipStrength_ = groomSlider(tr("Tip strength"), theme::yellow(), 0.0, 1.0, "tipStrength", &GroomColor::tipStrength, [](double) {});
    // Each strand's hue and saturation spread (HairColor's randomness, in either mode).
    hueRandom_ = groomSlider(tr("Hue random"), theme::red(), 0.0, 1.0, "hueRandom", &GroomColor::hueRandom, [](double) {});
    saturationRandom_ = groomSlider(tr("Saturation random"), theme::blue(), 0.0, 1.0, "saturationRandom",
                                    &GroomColor::saturationRandom, [](double) {});
    colorNote_ = new QLabel(materialPage_);
    colorNote_->setWordWrap(true);
    colorNote_->setFont(theme::uiFont(12, 500));
    colorNote_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    materialLayout->addWidget(colorNote_);

    moreButton_ = new RoundButton(materialPage_);
    moreButton_->setText(tr("More settings"));
    moreButton_->setDiameter(34);
    moreButton_->setRadius(12.0);
    moreButton_->setFontSize(13);
    moreButton_->setFontWeight(600);
    moreButton_->setFill(theme::chipSoft(), theme::chipSoft());
    moreButton_->setTextColor(theme::textDim(), theme::textDim());
    moreButton_->setToolTip(tr("Every hair material parameter, and the surfaces (the scalp, the props, the eyes)"));
    materialLayout->addWidget(moreButton_);
    connect(moreButton_, &QAbstractButton::clicked, this, [this] { setMoreShown(!moreShown_); });

    more_ = new QWidget(materialPage_);
    auto* moreLayout = new QVBoxLayout(more_);
    moreLayout->setContentsMargins(0, 0, 0, 0);
    moreLayout->setSpacing(12);
    materialLayout->addWidget(more_);
    caption(moreLayout, tr("Pigment"));
    pigment_ = comboBox(more_, {tr("Groom melanin channels"), tr("Groom RGB channels"), tr("This material")},
                        tr("Where the pigment comes from: the groom's melanin and melaninRedness channels (as the Arnold "
                           "translator reads them), its colorR/G/B channels as Arnold's base color, or the values here"));
    moreLayout->addWidget(pigment_);
    connect(pigment_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        material_.pigment = static_cast<HairMaterial::Pigment>(index);
        material_.presetName.clear();
        emit materialEdited();
    });
    materialRefreshers_.push_back([this] { pigment_->setCurrentIndex(static_cast<int>(material_.pigment)); });
    // The viewport's own pigment (the "This material" source): a try-on that is never saved.
    {
        QLabel* override = caption(moreLayout, tr("Viewport override"));
        override->setText(QStringLiteral("%1 <span style=\"color:%3; font-weight:600; font-size:11px\">%2</span>")
                              .arg(tr("Viewport override"), tr("viewport only"), theme::textMuted().name()));
        override->setToolTip(tr("A pigment tried in the viewport alone (the pigment source \"This material\"): not the "
                                "groom's, not saved; the sliders above edit the groom's HairColor node"));
    }
    slider(moreLayout, tr("Melanin (viewport)"), theme::yellow(), 0.0, 1.0, 2, m(&HairMaterial::melanin), ms(&HairMaterial::melanin, true), Target::Material);
    slider(moreLayout, tr("Redness (viewport)"), theme::red(), 0.0, 1.0, 2, m(&HairMaterial::melaninRedness), ms(&HairMaterial::melaninRedness, true), Target::Material);
    slider(moreLayout, tr("Roughness (viewport)"), theme::blue(), 0.02, 1.0, 2, m(&HairMaterial::roughness), ms(&HairMaterial::roughness), Target::Material);
    slider(moreLayout, tr("Randomize"), theme::yellow(), 0.0, 1.0, 2, m(&HairMaterial::melaninRandomize), ms(&HairMaterial::melaninRandomize), Target::Material);
    {
        auto [get, set] = mc(&HairMaterial::baseColor);
        color(moreLayout, tr("Dye (base color)"), get, set, Target::Material);
    }
    slider(moreLayout, tr("Azimuthal roughness"), theme::blue(), 0.0, 1.0, 2, m(&HairMaterial::azimuthalRoughness), ms(&HairMaterial::azimuthalRoughness), Target::Material);
    slider(moreLayout, tr("IOR"), theme::blue(), 1.0, 2.5, 2, m(&HairMaterial::ior), ms(&HairMaterial::ior), Target::Material);
    slider(moreLayout, tr("Shift (degrees)"), theme::blue(), 0.0, 10.0, 1, m(&HairMaterial::shift), ms(&HairMaterial::shift), Target::Material);
    slider(moreLayout, tr("Specular (R)"), theme::emerald(), 0.0, 2.0, 2, m(&HairMaterial::specular), ms(&HairMaterial::specular), Target::Material);
    {
        auto [get, set] = mc(&HairMaterial::specularTint);
        color(moreLayout, tr("Specular tint"), get, set, Target::Material);
    }
    slider(moreLayout, tr("Specular 2 (TRT)"), theme::emerald(), 0.0, 2.0, 2, m(&HairMaterial::specular2), ms(&HairMaterial::specular2), Target::Material);
    {
        auto [get, set] = mc(&HairMaterial::specular2Tint);
        color(moreLayout, tr("Specular 2 tint"), get, set, Target::Material);
    }
    slider(moreLayout, tr("Transmission (TT)"), theme::emerald(), 0.0, 2.0, 2, m(&HairMaterial::transmission), ms(&HairMaterial::transmission), Target::Material);
    {
        auto [get, set] = mc(&HairMaterial::transmissionTint);
        color(moreLayout, tr("Transmission tint"), get, set, Target::Material);
    }
    slider(moreLayout, tr("Diffuse"), theme::purple(), 0.0, 1.0, 2, m(&HairMaterial::diffuse), ms(&HairMaterial::diffuse), Target::Material);
    slider(moreLayout, tr("Multiple scattering"), theme::purple(), 0.0, 2.0, 2, m(&HairMaterial::multipleScattering), ms(&HairMaterial::multipleScattering), Target::Material);
    slider(moreLayout, tr("Width scale"), theme::yellow(), 0.05, 4.0, 2, m(&HairMaterial::widthScale), ms(&HairMaterial::widthScale), Target::Material);
    slider(moreLayout, tr("Shadow density"), theme::yellow(), 0.0, 4.0, 2, m(&HairMaterial::shadowDensity), ms(&HairMaterial::shadowDensity), Target::Material);

    // ---- Surfaces (in the material's full settings): the scalp, the props, the eyes.
    caption(moreLayout, tr("Surfaces"));
    surface_ = comboBox(more_, {}, tr("The surface edited below: the scalp, then the props and eyes as they were loaded"));
    moreLayout->addWidget(surface_);
    connect(surface_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updating_) return;
        refreshSurfaceRows();
    });
    QStringList presets;
    for (const std::string& name : surfacePresetNames()) presets << QString::fromStdString(name);
    surfacePreset_ = comboBox(more_, presets, tr("A preset: how the surface is shaded (its texture files stay)"));
    moreLayout->addWidget(surfacePreset_);
    connect(surfacePreset_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (updating_) return;
        SurfaceMaterial* m = currentSurface();
        if (!m) return;
        SurfaceMaterial preset = surfacePreset(name.toStdString());
        preset.albedoMap = m->albedoMap;
        preset.normalMap = m->normalMap;
        preset.roughnessMap = m->roughnessMap;
        preset.metallicMap = m->metallicMap;
        preset.occlusionMap = m->occlusionMap;
        preset.specularMap = m->specularMap;
        *m = preset;
        refreshSurfaceRows();
        emit surfaceEdited(surface_->currentIndex(), *m);
    });
    surfaceKind_ = comboBox(more_, {tr("Default (metal/roughness)"), tr("Skin"), tr("Eye")}, tr("What the surface is shaded as"));
    moreLayout->addWidget(surfaceKind_);
    connect(surfaceKind_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        if (SurfaceMaterial* m = currentSurface()) {
            m->kind = static_cast<SurfaceKind>(index);
            m->presetName.clear();
            refreshSurfaceRows();
            emit surfaceEdited(surface_->currentIndex(), *m);
        }
    });
    const int kDefault = 1, kSkin = 2, kEye = 4, kAll = 7;
    auto sg = [this](auto member) {
        return [this, member]() -> double {
            SurfaceMaterial* m = currentSurface();
            return m ? m->*member : 0.0;
        };
    };
    auto ss = [this](auto member) {
        return [this, member](double v) {
            if (SurfaceMaterial* m = currentSurface()) m->*member = static_cast<float>(v);
        };
    };
    auto sc = [this](auto member) {
        return std::make_pair([this, member]() -> RGB { SurfaceMaterial* m = currentSurface(); return m ? m->*member : RGB{}; },
                              [this, member](const RGB& c) {
                                  if (SurfaceMaterial* m = currentSurface()) m->*member = c;
                              });
    };
    {
        auto [get, set] = sc(&SurfaceMaterial::baseColor);
        color(moreLayout, tr("Base color"), get, set, Target::Surface, kDefault | kSkin);
    }
    slider(moreLayout, tr("Roughness"), theme::blue(), 0.02, 1.0, 2, sg(&SurfaceMaterial::roughness), ss(&SurfaceMaterial::roughness), Target::Surface, kAll);
    slider(moreLayout, tr("Metallic"), theme::blue(), 0.0, 1.0, 2, sg(&SurfaceMaterial::metallic), ss(&SurfaceMaterial::metallic), Target::Surface, kDefault);
    slider(moreLayout, tr("Specular"), theme::emerald(), 0.0, 1.0, 2, sg(&SurfaceMaterial::specular), ss(&SurfaceMaterial::specular), Target::Surface, kAll);
    slider(moreLayout, tr("Occlusion"), theme::purple(), 0.0, 1.0, 2, sg(&SurfaceMaterial::occlusion), ss(&SurfaceMaterial::occlusion), Target::Surface, kDefault | kSkin);
    slider(moreLayout, tr("Normal strength"), theme::purple(), 0.0, 3.0, 2, sg(&SurfaceMaterial::normalStrength), ss(&SurfaceMaterial::normalStrength), Target::Surface, kDefault | kSkin);
    slider(moreLayout, tr("Detail scale"), theme::purple(), 0.0, 400.0, 0, sg(&SurfaceMaterial::detailScale), ss(&SurfaceMaterial::detailScale), Target::Surface, kDefault | kSkin);
    slider(moreLayout, tr("Detail strength"), theme::purple(), 0.0, 2.0, 2, sg(&SurfaceMaterial::detailStrength), ss(&SurfaceMaterial::detailStrength), Target::Surface, kDefault | kSkin);
    slider(moreLayout, tr("Cavity"), theme::purple(), 0.0, 1.0, 2, sg(&SurfaceMaterial::cavity), ss(&SurfaceMaterial::cavity), Target::Surface, kDefault | kSkin);
    // The skin's scattering.
    caption(moreLayout, tr("Subsurface scattering"), kSkin, Target::Surface);
    sssMode_ = chipSegments({{tr("Screen space"), theme::emerald(), theme::ink()}, {tr("Pre-integrated"), theme::emerald(), theme::ink()}}, more_,
                            tr("Screen space: the irradiance blurred by the diffusion profile (Jimenez 2015). Pre-integrated: the "
                               "profile looked up by curvature (Penner 2011), the fallback"));
    moreLayout->addWidget(sssMode_);
    surfaceRows_.push_back({sssMode_, kSkin});
    connect(sssMode_, &Segmented::changed, this, [this](int index) {
        if (updating_) return;
        if (SurfaceMaterial* m = currentSurface()) {
            m->sssMode = index == 0 ? SurfaceMaterial::SssMode::ScreenSpace : SurfaceMaterial::SssMode::Preintegrated;
            m->presetName.clear();
            emit surfaceEdited(surface_->currentIndex(), *m);
        }
    });
    surfaceRefreshers_.push_back([this] {
        if (SurfaceMaterial* m = currentSurface()) sssMode_->setCurrent(m->sssMode == SurfaceMaterial::SssMode::ScreenSpace ? 0 : 1);
    });
    slider(moreLayout, tr("Scatter radius"), theme::red(), 0.05, 5.0, 2, sg(&SurfaceMaterial::sssRadius), ss(&SurfaceMaterial::sssRadius), Target::Surface, kSkin);
    slider(moreLayout, tr("Scatter strength"), theme::red(), 0.0, 1.0, 2, sg(&SurfaceMaterial::sssStrength), ss(&SurfaceMaterial::sssStrength), Target::Surface, kSkin);
    slider(moreLayout, tr("Texturing (pre-scatter)"), theme::red(), 0.0, 1.0, 2, sg(&SurfaceMaterial::sssTexturing), ss(&SurfaceMaterial::sssTexturing), Target::Surface, kSkin);
    for (int k = 0; k < 3; ++k) {
        const QString names[3] = {tr("Mean free path R (cm)"), tr("Mean free path G (cm)"), tr("Mean free path B (cm)")};
        const QColor fills[3] = {theme::red(), theme::emerald(), theme::blue()};
        slider(moreLayout, names[k], fills[k], 0.001, 2.0, 3, [this, k] { SurfaceMaterial* m = currentSurface(); return m ? m->sssMeanFreePath[k] : 0.0; },
               [this, k](double v) { if (SurfaceMaterial* m = currentSurface()) m->sssMeanFreePath[k] = static_cast<float>(v); }, Target::Surface, kSkin);
    }
    slider(moreLayout, tr("Transmission"), theme::red(), 0.0, 4.0, 2, sg(&SurfaceMaterial::transmission), ss(&SurfaceMaterial::transmission), Target::Surface, kSkin);
    slider(moreLayout, tr("Roughness 2"), theme::blue(), 0.02, 1.0, 2, sg(&SurfaceMaterial::roughness2), ss(&SurfaceMaterial::roughness2), Target::Surface, kSkin);
    slider(moreLayout, tr("Lobe 2 weight"), theme::blue(), 0.0, 1.0, 2, sg(&SurfaceMaterial::lobe2Weight), ss(&SurfaceMaterial::lobe2Weight), Target::Surface, kSkin);
    // The eyes.
    {
        auto [get, set] = sc(&SurfaceMaterial::irisColor);
        color(moreLayout, tr("Iris color"), get, set, Target::Surface, kEye);
    }
    {
        auto [get, set] = sc(&SurfaceMaterial::scleraColor);
        color(moreLayout, tr("Sclera color"), get, set, Target::Surface, kEye);
    }
    slider(moreLayout, tr("Iris radius"), theme::blue(), 0.2, 0.7, 2, sg(&SurfaceMaterial::irisRadius), ss(&SurfaceMaterial::irisRadius), Target::Surface, kEye);
    slider(moreLayout, tr("Pupil radius"), theme::blue(), 0.05, 0.8, 2, sg(&SurfaceMaterial::pupilRadius), ss(&SurfaceMaterial::pupilRadius), Target::Surface, kEye);
    slider(moreLayout, tr("Limbal width"), theme::blue(), 0.0, 0.5, 2, sg(&SurfaceMaterial::limbalWidth), ss(&SurfaceMaterial::limbalWidth), Target::Surface, kEye);
    slider(moreLayout, tr("Cornea IOR"), theme::blue(), 1.0, 2.0, 3, sg(&SurfaceMaterial::corneaIor), ss(&SurfaceMaterial::corneaIor), Target::Surface, kEye);
    slider(moreLayout, tr("Iris detail"), theme::purple(), 0.0, 2.0, 2, sg(&SurfaceMaterial::irisDetail), ss(&SurfaceMaterial::irisDetail), Target::Surface, kEye);
    slider(moreLayout, tr("Veins"), theme::red(), 0.0, 2.0, 2, sg(&SurfaceMaterial::veins), ss(&SurfaceMaterial::veins), Target::Surface, kEye);
    slider(moreLayout, tr("Lid occlusion"), theme::purple(), 0.0, 6.0, 2, sg(&SurfaceMaterial::eyeOcclusion), ss(&SurfaceMaterial::eyeOcclusion), Target::Surface, kEye);
    slider(moreLayout, tr("Tear line"), theme::emerald(), 0.0, 2.0, 2, sg(&SurfaceMaterial::tearLine), ss(&SurfaceMaterial::tearLine), Target::Surface, kEye);
    slider(moreLayout, tr("Wetness"), theme::emerald(), 0.0, 2.0, 2, sg(&SurfaceMaterial::wetness), ss(&SurfaceMaterial::wetness), Target::Surface, kEye);
    auto* all = new RoundButton(more_);
    all->setText(tr("All settings (texture files)..."));
    all->setDiameter(30);
    all->setRadius(10.0);
    all->setFontSize(12);
    all->setFontWeight(600);
    all->setFill(theme::chipSoft(), theme::chipSoft());
    all->setTextColor(theme::textDim(), theme::textDim());
    moreLayout->addWidget(all);
    connect(all, &QAbstractButton::clicked, this, &LookCard::moreRequested);
    more_->hide();

    // ---- Lighting: the presets as chips, the lights, the environment, exposure, the display.
    lightingPage_ = page();
    QVBoxLayout* lightingLayout = layoutOf(lightingPage_);
    auto* chips = new QWidget(lightingPage_);
    auto* chipGrid = new QGridLayout(chips);
    chipGrid->setContentsMargins(0, 0, 0, 0);
    chipGrid->setSpacing(6);
    int k = 0;
    for (const char* name : kLightingChips) {
        auto* b = new RoundButton(chips);
        b->setText(theme::brushTitle(name));
        b->setDiameter(28);
        b->setPadding(9);
        b->setFontSize(11);
        b->setFontWeight(700);
        b->setFill(theme::chip(), theme::checkedFill(theme::yellow()));
        b->setTextColor(theme::textSoft(), theme::checkedText(theme::ink()));
        b->setCheckable(true);
        const std::string preset = name;
        connect(b, &QAbstractButton::clicked, this, [this, preset] {
            // The background and the render settings are the user's, not the preset's.
            lighting_ = withLightingPreset(lighting_, preset);
            refresh();
            emit lightingEdited();
        });
        lightingChips_.push_back(b);
        chipGrid->addWidget(b, k / 3, k % 3);
        ++k;
    }
    lightingLayout->addWidget(chips);
    auto l = [this](auto member) { return [this, member]() -> double { return lighting_.*member; }; };
    auto ls = [this](auto member) {
        return [this, member](double v) {
            lighting_.*member = static_cast<float>(v);
            lighting_.presetName.clear();
        };
    };
    slider(lightingLayout, tr("Exposure (EV)"), theme::yellow(), -4.0, 4.0, 2, l(&LightingSetup::exposure), ls(&LightingSetup::exposure), Target::Lighting);
    for (int i = 0; i < 3; ++i) {
        LightRow row;
        row.host = new QWidget(lightingPage_);
        auto* v = new QVBoxLayout(row.host);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);
        auto* head = new QHBoxLayout();
        head->setSpacing(8);
        row.enabled = new QCheckBox(tr("Light %1").arg(i + 1), row.host);
        row.enabled->setFont(theme::uiFont(13, 700));
        head->addWidget(row.enabled, 1);
        row.color = new ColorEditor(row.host);
        row.color->setFixedWidth(112);
        row.color->setToolTip(tr("The light's color"));
        head->addWidget(row.color);
        v->addLayout(head);
        auto lightSlider = [&](const QString& name, const QColor& fill, double min, double max, int decimals) {
            auto* s = new ValueSlider(row.host);
            s->setStacked(true);
            s->setCaption(name);
            s->setRange(min, max);
            s->setFill(fill);
            s->setFormat([decimals](double x) { return QString::number(x, 'f', decimals); });
            v->addWidget(s);
            return s;
        };
        row.azimuth = lightSlider(tr("Azimuth"), theme::blue(), -180.0, 180.0, 0);
        row.elevation = lightSlider(tr("Elevation"), theme::blue(), -90.0, 90.0, 0);
        row.intensity = lightSlider(tr("Intensity"), theme::yellow(), 0.0, 10.0, 2);
        row.shadows = new QCheckBox(tr("Shadows (deep opacity map)"), row.host);
        row.shadows->setToolTip(tr("Casts a deep opacity map: hair-on-hair shadows, the scalp in them"));
        v->addWidget(row.shadows);
        lightingLayout->addWidget(row.host);
        const std::size_t index = lightRows_.size();
        auto read = [this, index](bool live) {
            if (updating_ || index >= lighting_.lights.size()) return;
            const LightRow& r = lightRows_[index];
            DirectionalLight& light = lighting_.lights[index];
            light.enabled = r.enabled->isChecked();
            light.direction = lightDirection(static_cast<float>(r.azimuth->value()), static_cast<float>(r.elevation->value()));
            light.intensity = static_cast<float>(r.intensity->value());
            light.color = r.color->linear();
            light.shadows = r.shadows->isChecked();
            lighting_.presetName.clear();
            if (!live) refresh();
            emit lightingEdited();
        };
        connect(row.enabled, &QCheckBox::toggled, this, [read](bool) { read(false); });
        connect(row.shadows, &QCheckBox::toggled, this, [read](bool) { read(false); });
        connect(row.azimuth, &ValueSlider::valueChanged, this, [read](double) { read(true); });
        connect(row.elevation, &ValueSlider::valueChanged, this, [read](double) { read(true); });
        connect(row.intensity, &ValueSlider::valueChanged, this, [read](double) { read(true); });
        connect(row.color, &ColorEditor::edited, this, [read](bool live) { read(live); });
        lightRows_.push_back(row);
    }
    caption(lightingLayout, tr("Environment"), 7, Target::Lighting);
    environment_ = comboBox(lightingPage_, {tr("None"), tr("Studio"), tr("Sky"), tr("Overcast"), tr("Sunset")},
                            tr("A generated environment: its light on the hair, and the background when chosen"));
    lightingLayout->addWidget(environment_);
    connect(environment_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        lighting_.environment = static_cast<Environment>(index);
        lighting_.presetName.clear();
        emit lightingEdited();
    });
    lightingRefreshers_.push_back([this] { environment_->setCurrentIndex(static_cast<int>(lighting_.environment)); });
    slider(lightingLayout, tr("Intensity"), theme::blue(), 0.0, 4.0, 2, l(&LightingSetup::environmentIntensity), ls(&LightingSetup::environmentIntensity), Target::Lighting);
    slider(lightingLayout, tr("Rotation"), theme::blue(), -180.0, 180.0, 0, l(&LightingSetup::environmentRotation), ls(&LightingSetup::environmentRotation), Target::Lighting);
    caption(lightingLayout, tr("Tone map"), 7, Target::Lighting);
    toneMap_ = chipSegments({{tr("None"), theme::emerald(), theme::ink()}, {tr("ACES"), theme::emerald(), theme::ink()}, {tr("AgX"), theme::emerald(), theme::ink()}},
                            lightingPage_, tr("The display transform after exposure"));
    lightingLayout->addWidget(toneMap_);
    connect(toneMap_, &Segmented::changed, this, [this](int index) {
        if (updating_) return;
        lighting_.toneMap = static_cast<ToneMap>(index);
        emit lightingEdited();
    });
    lightingRefreshers_.push_back([this] { toneMap_->setCurrent(static_cast<int>(lighting_.toneMap)); });
    caption(lightingLayout, tr("Background"), 7, Target::Lighting);
    background_ = chipSegments({{tr("Solid"), theme::emerald(), theme::ink()}, {tr("Gradient"), theme::emerald(), theme::ink()}, {tr("Environment"), theme::emerald(), theme::ink()}},
                               lightingPage_, tr("Behind the hair: a color (deep charcoal by default), a gradient of it, or the environment"));
    lightingLayout->addWidget(background_);
    connect(background_, &Segmented::changed, this, [this](int index) {
        if (updating_) return;
        lighting_.background = static_cast<Background>(index);
        emit lightingEdited();
    });
    lightingRefreshers_.push_back([this] { background_->setCurrent(static_cast<int>(lighting_.background)); });
    color(lightingLayout, tr("Background color"), [this] { return lighting_.backgroundColor; },
          [this](const RGB& c) { lighting_.backgroundColor = c; }, Target::Lighting);
    slider(lightingLayout, tr("Background intensity"), theme::blue(), 0.0, 4.0, 2, l(&LightingSetup::backgroundIntensity), ls(&LightingSetup::backgroundIntensity), Target::Lighting);
    slider(lightingLayout, tr("Shadow softness"), theme::purple(), 0.05, 10.0, 2, l(&LightingSetup::shadowSoftness), ls(&LightingSetup::shadowSoftness), Target::Lighting);
    ambientOcclusion_ = new QCheckBox(tr("Ambient occlusion"), lightingPage_);
    ambientOcclusion_->setToolTip(tr("A map from above: what the hair above hides of the sky"));
    lightingLayout->addWidget(ambientOcclusion_);
    connect(ambientOcclusion_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) return;
        lighting_.ambientOcclusion = on;
        emit lightingEdited();
    });
    lightingRefreshers_.push_back([this] { ambientOcclusion_->setChecked(lighting_.ambientOcclusion); });

    // ---- Render: the quality, the sample count, the resolution scale.
    renderPage_ = page();
    QVBoxLayout* renderLayout = layoutOf(renderPage_);
    caption(renderLayout, tr("Quality"), 7, Target::Lighting);
    quality_ = chipSegments({{tr("Interactive"), theme::emerald(), theme::ink()}, {tr("Refine"), theme::emerald(), theme::ink()}}, renderPage_,
                            tr("Refine: once nothing moves, the viewport keeps accumulating jittered frames (anti-aliasing, exact "
                               "strand transparency, every environment light, softer shadows) until the image converges"));
    renderLayout->addWidget(quality_);
    connect(quality_, &Segmented::changed, this, [this](int index) {
        if (updating_) return;
        lighting_.quality = index == 0 ? Quality::Interactive : Quality::Refine;
        emit lightingEdited();
    });
    lightingRefreshers_.push_back([this] { quality_->setCurrent(lighting_.quality == Quality::Refine ? 1 : 0); });
    slider(renderLayout, tr("Refine samples"), theme::emerald(), 1.0, 256.0, 0, [this] { return lighting_.refineFrames; },
           [this](double v) { lighting_.refineFrames = static_cast<int>(std::lround(v)); }, Target::Lighting);
    slider(renderLayout, tr("Resolution scale"), theme::blue(), 0.25, 2.0, 2, l(&LightingSetup::resolutionScale),
           [this](double v) { lighting_.resolutionScale = static_cast<float>(v); }, Target::Lighting);
    caption(renderLayout, tr("Interactive quality"), 7, Target::Lighting);
    interactiveQuality_ = chipSegments({{tr("Adaptive"), theme::emerald(), theme::ink()}, {tr("Full"), theme::emerald(), theme::ink()}}, renderPage_,
                                       tr("Adaptive: while the camera, a slider or a stroke moves, the frames draw at a resolution scaled "
                                          "down until one fits in a display's refresh, the full picture back once nothing moves. Full: every "
                                          "frame as the idle one"));
    renderLayout->addWidget(interactiveQuality_);
    connect(interactiveQuality_, &Segmented::changed, this, [this](int index) {
        if (updating_) return;
        lighting_.interactiveQuality = index == 0 ? InteractiveQuality::Adaptive : InteractiveQuality::Full;
        emit lightingEdited();
    });
    lightingRefreshers_.push_back([this] { interactiveQuality_->setCurrent(lighting_.interactiveQuality == InteractiveQuality::Full ? 1 : 0); });
    auto* note = new QLabel(tr("Render (top right) refines from a clean slate; the line at the bottom right counts the frames."), renderPage_);
    note->setWordWrap(true);
    note->setFont(theme::uiFont(12, 500));
    note->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    renderLayout->addWidget(note);
    renderLayout->addStretch(1);

    connect(tabs_, &Segmented::changed, this, [this](int index) {
        materialPage_->setVisible(index == 0);
        lightingPage_->setVisible(index == 1);
        renderPage_->setVisible(index == 2);
        updateGeometry();
    });
    lightingPage_->hide();
    renderPage_->hide();
    // A page taller than its rows (the card docked in a tall window) keeps them at the top.
    static_cast<QVBoxLayout*>(pages_->layout())->addStretch(1);
    refresh();
}

ValueSlider* LookCard::slider(QBoxLayout* into, const QString& text, const QColor& fill, double min, double max, int decimals,
                              std::function<double()> get, std::function<void(double)> set, Target target, int kinds) {
    auto* s = new ValueSlider(into->parentWidget());
    s->setStacked(true);
    s->setCaption(text);
    s->setRange(min, max);
    s->setFill(fill);
    s->setFormat([decimals](double v) { return QString::number(v, 'f', decimals); });
    into->addWidget(s);
    connect(s, &ValueSlider::valueChanged, this, [this, set, target](double v) {
        if (updating_) return;
        set(v);
        edited(target);
    });
    auto refresher = [s, get] { s->setValue(get()); };
    (target == Target::Material ? materialRefreshers_ : target == Target::Lighting ? lightingRefreshers_ : surfaceRefreshers_).push_back(refresher);
    if (target == Target::Surface) surfaceRows_.push_back({s, kinds});
    return s;
}

ColorEditor* LookCard::color(QBoxLayout* into, const QString& text, std::function<RGB()> get, std::function<void(const RGB&)> set,
                             Target target, int kinds) {
    auto* host = new QWidget(into->parentWidget());
    auto* row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    auto* label = new QLabel(text, host);
    label->setFont(theme::uiFont(13, 600));
    label->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    // The label takes what the editor leaves (a long name clips rather than widen the card).
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setMinimumWidth(40);
    row->addWidget(label, 1);
    auto* editor = new ColorEditor(host);
    editor->setFixedWidth(112);
    row->addWidget(editor);
    into->addWidget(host);
    connect(editor, &ColorEditor::edited, this, [this, set, editor, target](bool) {
        if (updating_) return;
        set(editor->linear());
        edited(target);
    });
    auto refresher = [editor, get] { editor->setLinear(get()); };
    (target == Target::Material ? materialRefreshers_ : target == Target::Lighting ? lightingRefreshers_ : surfaceRefreshers_).push_back(refresher);
    if (target == Target::Surface) surfaceRows_.push_back({host, kinds});
    return editor;
}

QLabel* LookCard::caption(QBoxLayout* into, const QString& text, int kinds, Target target) {
    auto* label = new QLabel(text, into->parentWidget());
    label->setFont(theme::uiFont(13, 800));
    label->setStyleSheet(QStringLiteral("color: %1; margin-top: 4px;").arg(theme::text().name()));
    into->addWidget(label);
    if (target == Target::Surface) surfaceRows_.push_back({label, kinds});
    return label;
}

void LookCard::edited(Target target) {
    if (target == Target::Material) {
        material_.presetName.clear();
        updating_ = true;
        for (std::size_t i = 0; i < swatches_.size(); ++i) swatches_[i]->setChecked(false);
        updating_ = false;
        emit materialEdited();
    } else if (target == Target::Lighting) {
        lighting_.presetName.clear();
        updating_ = true;
        for (RoundButton* b : lightingChips_) b->setChecked(false);
        updating_ = false;
        emit lightingEdited();
    } else if (SurfaceMaterial* m = currentSurface()) {
        m->presetName.clear();
        updating_ = true;
        surfacePreset_->setCurrentIndex(-1);
        updating_ = false;
        emit surfaceEdited(surface_->currentIndex(), *m);
    }
}

SurfaceMaterial* LookCard::currentSurface() {
    const int index = surface_ ? surface_->currentIndex() : -1;
    return index >= 0 && static_cast<std::size_t>(index) < surfaces_.size() ? &surfaces_[index].second : nullptr;
}

const SurfaceMaterial* LookCard::surfaceMaterial(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < surfaces_.size() ? &surfaces_[index].second : nullptr;
}

void LookCard::setSurfaces(const std::vector<std::pair<QString, SurfaceMaterial>>& surfaces) {
    const int current = surface_->currentIndex();
    surfaces_ = surfaces;
    updating_ = true;
    surface_->clear();
    for (const auto& s : surfaces_) surface_->addItem(s.first);
    surface_->setCurrentIndex(surfaces_.empty() ? -1 : std::clamp(current, 0, static_cast<int>(surfaces_.size()) - 1));
    updating_ = false;
    refreshSurfaceRows();
}

void LookCard::refreshSurfaceRows() {
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
    for (const SurfaceRow& row : surfaceRows_) row.widget->setVisible(has && (row.kinds & kindBit));
    updating_ = false;
    updateGeometry();
}

void LookCard::setMaterial(const HairMaterial& material) {
    material_ = material;
    refresh();
}

void LookCard::setLighting(const LightingSetup& lighting) {
    lighting_ = lighting;
    refresh();
}

void LookCard::setTabsShown(bool shown) {
    tabs_->setVisible(shown);
    updateGeometry();
}

void LookCard::setTab(Tab tab) {
    tabs_->setCurrent(static_cast<int>(tab));
    materialPage_->setVisible(tab == Tab::Material);
    lightingPage_->setVisible(tab == Tab::Lighting);
    renderPage_->setVisible(tab == Tab::Render);
    updateGeometry();
}

void LookCard::setMoreShown(bool shown) {
    moreShown_ = shown;
    more_->setVisible(shown);
    moreButton_->setText(shown ? tr("Fewer settings") : tr("More settings"));
    updateGeometry();
}

void LookCard::setMaxHeight(int maxHeight) {
    maxHeight_ = maxHeight;
    updateGeometry();
}

QSize LookCard::sizeHint() const {
    // The tabs, the shown page's content and the margins; the page scrolls past maxHeight_.
    QWidget* page = materialPage_->isVisibleTo(pages_) ? materialPage_ : lightingPage_->isVisibleTo(pages_) ? lightingPage_ : renderPage_;
    const int content = page->sizeHint().height();
    int height = 18 + tabs_->sizeHint().height() + 12 + content + 18;
    if (maxHeight_ > 0) height = std::min(height, maxHeight_);
    return QSize(theme::OverlayMetrics().lookCardWidth, height);
}

QString LookCard::colorNote() const { return colorNote_->text(); }

void LookCard::setGroomColor(const GroomColor& color) {
    groomColor_ = color;
    refreshGroomColor();
}

void LookCard::refreshGroomColor() {
    const bool was = updating_;
    updating_ = true;
    const bool groom = !groomColor_.node.empty();
    melanin_->setValue(groom ? groomColor_.melanin : material_.melanin);
    redness_->setValue(groom ? groomColor_.redness : material_.melaninRedness);
    roughness_->setValue(groom ? groomColor_.roughness : material_.roughness);
    // The groom's mode: its picked color, or the pigment's sliders; the tip in either (the node's
    // alone: without one there is nothing to save them to).
    const bool picked = groom && groomColor_.picked;
    groomMode_->setVisible(groom);
    groomMode_->setCurrent(picked ? 1 : 0);
    groomPickedRow_->setVisible(picked);
    groomPicked_->setLinear(groomColor_.color);
    melanin_->setVisible(!picked);
    redness_->setVisible(!picked);
    tipColorRow_->setVisible(groom);
    tipColor_->setLinear(groomColor_.tipColor);
    tipStrength_->setVisible(groom);
    tipStrength_->setValue(groomColor_.tipStrength);
    hueRandom_->setVisible(groom);
    hueRandom_->setValue(groomColor_.hueRandom);
    saturationRandom_->setVisible(groom);
    saturationRandom_->setValue(groomColor_.saturationRandom);
    colorNote_->setText(groom ? tr("The groom's %1 node, saved with it").arg(QString::fromStdString(groomColor_.node))
                              : tr("No HairColor node in the graph: viewport only"));
    updating_ = was;
}

void LookCard::refresh() {
    updating_ = true;
    for (std::size_t i = 0; i < swatches_.size(); ++i) swatches_[i]->setChecked(material_.presetName == kSwatches[i].preset);
    refreshGroomColor();
    for (auto& r : materialRefreshers_) r();
    for (std::size_t i = 0; i < lightingChips_.size(); ++i) lightingChips_[i]->setChecked(lighting_.presetName == kLightingChips[i]);
    for (auto& r : lightingRefreshers_) r();
    for (std::size_t i = 0; i < lightRows_.size(); ++i) {
        LightRow& row = lightRows_[i];
        const bool has = i < lighting_.lights.size();
        row.host->setVisible(has);
        if (!has) continue;
        const DirectionalLight& light = lighting_.lights[i];
        double azimuth = 0.0, elevation = 0.0;
        lightAngles(light.direction, azimuth, elevation);
        row.enabled->setChecked(light.enabled);
        row.enabled->setText(light.name.empty() ? tr("Light %1").arg(i + 1) : theme::brushTitle(light.name));
        row.azimuth->setValue(azimuth);
        row.elevation->setValue(elevation);
        row.intensity->setValue(light.intensity);
        row.color->setLinear(light.color);
        row.shadows->setChecked(light.shadows);
    }
    updating_ = false;
    updateGeometry();
}

// ---- OverlayHost ---------------------------------------------------------------------------------

namespace {

// A dock or splitter drag resizes the host on every move: the viewport catches up this long after
// the last one (placeViewport).
constexpr int kViewportResizeMs = 60;

// The panels over the viewport: solid in a solid theme (its panel color, a thin edge), glass else.
GlassPanel* overlayPanel(QWidget* parent, qreal radius) {
    auto* panel = new GlassPanel(parent, theme::glassy() ? radius : std::min<qreal>(radius, 8.0));
    panel->setColor(theme::glassSolid());
    return panel;
}

}  // namespace

OverlayHost::OverlayHost(Viewport* viewport, QWidget* parent) : QWidget(parent), viewport_(viewport) {
    // Behind the viewport (shown for a moment while it catches up with a drag): its clear color.
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("OverlayHost { background: %1; }").arg(theme::backdrop().name()));
    viewport_->setParent(this);
    viewport_->show();
    viewportTimer_ = new QTimer(this);
    viewportTimer_->setSingleShot(true);
    viewportTimer_->setInterval(kViewportResizeMs);
    connect(viewportTimer_, &QTimer::timeout, this, [this] {
        // Still dragging: wait for the next quiet moment.
        if (QGuiApplication::mouseButtons() != Qt::NoButton && viewport_->size() != size()) viewportTimer_->start();
        viewport_->setGeometry(rect());
    });

    // ---- Top-left: the active tool's name.
    toolLabel_ = overlayPanel(this, 10.0);
    toolLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* labelLayout = new QHBoxLayout(toolLabel_);
    labelLayout->setContentsMargins(10, 5, 10, 5);
    toolName_ = new QLabel(toolLabel_);
    toolName_->setObjectName(QStringLiteral("toolName"));
    toolName_->setFont(theme::uiFont(12, 700));
    toolName_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    labelLayout->addWidget(toolName_);
    toolLabel_->hide();

    // ---- Top-right: Undo and Redo (the window has no tool bar: the menus, the keys and these).
    history_ = overlayPanel(this, 10.0);
    history_->setObjectName(QStringLiteral("history"));
    auto* historyLayout = new QHBoxLayout(history_);
    historyLayout->setContentsMargins(4, 4, 4, 4);
    historyLayout->setSpacing(2);
    auto historyButton = [this, historyLayout](const char* icon, const char* name) {
        auto* b = new RoundButton(history_);
        b->setObjectName(QString::fromLatin1(name));
        b->setIcon(QStringLiteral(":/groomlab/icons/ui_%1.svg").arg(QLatin1String(icon)), 15);
        b->setDiameter(26);
        if (!theme::glassy()) b->setRadius(5.0);
        b->setFill(QColor(0, 0, 0, 0), theme::chipChecked());
        historyLayout->addWidget(b);
        return b;
    };
    undo_ = historyButton("undo", "undoButton");
    redo_ = historyButton("redo", "redoButton");
    connect(undo_, &QAbstractButton::clicked, this, &OverlayHost::undoRequested);
    connect(redo_, &QAbstractButton::clicked, this, &OverlayHost::redoRequested);
    setUndoRedo(false, false, tr("Undo (Ctrl+Z)"), tr("Redo (Ctrl+Y)"));

    // ---- Bottom: the stats, the refine line, the toast.
    stats_ = new ShadowText(this);
    stats_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    refine_ = new ShadowText(this);
    refine_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    toast_ = overlayPanel(this, 12.0);
    toast_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* toastLayout = new QHBoxLayout(toast_);
    toastLayout->setContentsMargins(14, 8, 16, 8);
    toastLayout->setSpacing(10);
    toastDot_ = dotLabel(theme::red(), 8, toast_);
    toastLayout->addWidget(toastDot_, 0, Qt::AlignTop);
    toastText_ = new QLabel(toast_);
    toastText_->setFont(theme::uiFont(13, 600));
    toastText_->setWordWrap(true);
    toastText_->setTextFormat(Qt::PlainText);
    toastLayout->addWidget(toastText_, 1);
    toast_->hide();
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    connect(toastTimer_, &QTimer::timeout, toast_, &QWidget::hide);

    // The prompt (showPrompt): filled when asked, centered over everything.
    prompt_ = overlayPanel(this, 18.0);
    prompt_->setObjectName(QStringLiteral("prompt"));
    prompt_->hide();

    // ---- The middle, with no groom open: what to do first, and the recent files.
    emptyCard_ = overlayPanel(this, 24.0);
    {
        auto* card = new QVBoxLayout(emptyCard_);
        card->setContentsMargins(28, 24, 28, 24);
        card->setSpacing(10);
        auto* title = new QLabel(tr("No groom open"), emptyCard_);
        title->setFont(theme::uiFont(18, 800));
        title->setStyleSheet(QStringLiteral("color: %1; font-size: 18px; font-weight: 800;").arg(theme::text().name()));
        card->addWidget(title);
        auto* hint = new QLabel(emptyCard_);
        hint->setObjectName(QStringLiteral("emptyHint"));
        hint->setWordWrap(true);
        hint->setFixedWidth(320);
        hint->setFont(theme::uiFont(13, 500));
        hint->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
        card->addWidget(hint);
        emptyHint_ = hint;
        auto action = [this, card](const QString& text, const QString& key, const QColor& fill, const QColor& ink, auto slot) {
            auto* b = new RoundButton(emptyCard_);
            b->setText(text);
            b->setDimText(key);
            b->setDiameter(38);
            b->setRadius(theme::glassy() ? 14.0 : 6.0);
            b->setPadding(16);
            b->setFontSize(14);
            b->setFontWeight(700);
            b->setFill(fill, fill);
            b->setTextColor(ink, ink);
            connect(b, &QAbstractButton::clicked, this, slot);
            card->addWidget(b);
            return b;
        };
        action(tr("Open groom"), QStringLiteral("Ctrl+O"), theme::actionFill(theme::yellow()), theme::actionText(theme::ink()),
               [this] { emit openRequested(); });
        action(tr("New groom on mesh"), QStringLiteral("Ctrl+N"), theme::chip(), theme::text(), [this] { emit newRequested(); });
        action(tr("Start from a preset"), QString(), theme::chip(), theme::text(), [this] { emit presetsRequested(); });
        auto* recent = new QWidget(emptyCard_);
        recentLayout_ = new QVBoxLayout(recent);
        recentLayout_->setContentsMargins(0, 6, 0, 0);
        recentLayout_->setSpacing(4);
        recentCaption_ = new QLabel(tr("Recent"), recent);
        recentCaption_->setFont(theme::uiFont(12, 700));
        recentCaption_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
        recentCaption_->hide();
        recentLayout_->addWidget(recentCaption_);
        card->addWidget(recent);
    }
    emptyCard_->hide();
    setAcceptDrops(true);

    built_ = true;
    setMode(Mode::Groom);
    refreshEmptyCard();
}

void OverlayHost::setRecentFiles(const QStringList& paths) {
    recentFiles_ = paths.mid(0, 5);
    for (RoundButton* b : recentButtons_) delete b;
    recentButtons_.clear();
    recentCaption_->setVisible(!recentFiles_.isEmpty());
    for (const QString& path : recentFiles_) {
        auto* b = new RoundButton(recentCaption_->parentWidget());
        // (Long names elided in the middle: the card stays its width.)
        b->setText(QFontMetrics(theme::uiFont(13, 600)).elidedText(QFileInfo(path).fileName(), Qt::ElideMiddle, 300));
        b->setDiameter(30);
        b->setRadius(theme::glassy() ? 10.0 : 5.0);
        b->setPadding(12);
        b->setFontSize(13);
        b->setFontWeight(600);
        b->setFill(QColor(0, 0, 0, 0), theme::chip());
        b->setTextColor(theme::textSoft(), theme::text());
        b->setToolTip(path);
        connect(b, &QAbstractButton::clicked, this, [this, path] { emit fileDropped(path); });
        recentLayout_->addWidget(b);
        b->show();  // (now: the layout would show it later, after the card took its size without it)
        recentButtons_.push_back(b);
    }
    refreshEmptyCard();
}

QString OverlayHost::emptyText() const {
    switch (mode_) {
        case Mode::Look: return tr("Look mode lights the groom and sets its hair material. Open a groom, or make one on a mesh:");
        case Mode::Cards: return tr("Cards mode turns a groom into hair cards for game engines and bakes their textures. Open a groom first:");
        case Mode::Export: return tr("Export mode writes the groom as Alembic, USD, Unreal or Unity strands, or cards. Open a groom first:");
        case Mode::Groom: break;
    }
    return tr("Drop a .groom, .obj or .abc file on the window, or");
}

void OverlayHost::setStatsWanted(bool on) {
    if (statsWanted_ == on) return;
    statsWanted_ = on;
    refreshEmptyCard();
    relayout();
}

bool OverlayHost::statsShown() const { return stats_ && stats_->isVisibleTo(this); }

void OverlayHost::refreshEmptyCard() {
    if (!built_) return;
    const bool open = groomOpen();
    emptyCard_->setVisible(!open);
    emptyHint_->setText(emptyText());
    stats_->setVisible(open && mode_ == Mode::Groom && statsWanted_);
    refine_->setVisible(open && mode_ == Mode::Look);
    if (!open) {
        // The recent files just added counted (their own layout first, then the card's): without
        // it the card kept the height it had without them until a later relayout.
        recentLayout_->activate();
        recentCaption_->parentWidget()->updateGeometry();
        if (emptyCard_->layout()) emptyCard_->layout()->activate();
        emptyCard_->adjustSize();
        emptyCard_->raise();
    }
}

void OverlayHost::dragEnterEvent(QDragEnterEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
        if (suffix == QStringLiteral("groom") || suffix == QStringLiteral("obj") || suffix == QStringLiteral("abc")) {
            event->acceptProposedAction();
            return;
        }
    }
}

void OverlayHost::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QStringLiteral("groom") || suffix == QStringLiteral("obj") || suffix == QStringLiteral("abc")) {
            event->acceptProposedAction();
            emit fileDropped(path);
            return;
        }
    }
}

void OverlayHost::setFileName(const QString& name, bool dirty) {
    if (name == name_ && dirty == dirty_) return;
    name_ = name;
    dirty_ = dirty;
    refreshEmptyCard();
    refreshToolLabel();
    relayout();
}

void OverlayHost::setTool(const std::string& brushMode, bool select) {
    toolMode_ = brushMode;
    toolSelect_ = select;
    refreshToolLabel();
}

QString OverlayHost::toolText() const { return toolLabel_->isVisibleTo(this) ? toolName_->text() : QString(); }

void OverlayHost::refreshToolLabel() {
    // The name and the key that brings the tool back: S for Select, B for the brushes (Groom mode).
    const bool shown = groomOpen() && mode_ == Mode::Groom && (toolSelect_ || !toolMode_.empty());
    if (shown) {
        toolName_->setText(QStringLiteral("%1 <span style=\"color:%3; font-weight:600\">%2</span>")
                               .arg(toolSelect_ ? tr("Select") : theme::brushTitle(toolMode_), toolSelect_ ? QStringLiteral("S") : QStringLiteral("B"),
                                    theme::textMuted().name()));
        toolLabel_->adjustSize();
    }
    if (toolLabel_->isVisibleTo(this) != shown) {
        toolLabel_->setVisible(shown);
        relayout();
    } else if (shown) {
        relayout();
    }
}

QString OverlayHost::statsText() const {
    if (!stats_->isVisibleTo(this)) return {};
    QStringList words;
    for (const ShadowText::Run& r : stats_->runs()) words << r.text;
    return words.join(QLatin1Char(' '));
}

void OverlayHost::setStats(std::size_t strands, double msPerDab, const QString& badge, const QColor& badgeColor, std::size_t guides) {
    std::vector<ShadowText::Run> runs;
    const QFont words = theme::uiFont(12, 500), numbers = theme::numberFont(12);
    auto run = [&](const QString& text, const QFont& font, const QColor& color, int gap) {
        ShadowText::Run r;
        r.text = text;
        r.font = font;
        r.color = color;
        r.gapBefore = gap;
        return r;
    };
    ShadowText::Run count = run(QLocale(QLocale::English).toString(static_cast<qulonglong>(strands)), numbers, theme::textSoft(), 0);
    count.dot = theme::emerald();
    runs.push_back(count);
    runs.push_back(run(tr("strands"), words, theme::textSoft(), 5));
    if (guides != static_cast<std::size_t>(-1)) {
        runs.push_back(run(QStringLiteral("·"), words, theme::textMuted(), 10));
        runs.push_back(run(QLocale(QLocale::English).toString(static_cast<qulonglong>(guides)), numbers, theme::textSoft(), 10));
        runs.push_back(run(guides == 1 ? tr("guide") : tr("guides"), words, theme::textSoft(), 5));
    }
    runs.push_back(run(QStringLiteral("·"), words, theme::textMuted(), 10));
    runs.push_back(run(QString::number(msPerDab, 'f', msPerDab < 10.0 ? 1 : 0) + tr(" ms"), numbers, theme::textSoft(), 10));
    runs.push_back(run(tr("per dab"), words, theme::textSoft(), 5));
    if (!badge.isEmpty()) {
        ShadowText::Run pill = run(badge, theme::uiFont(11, 700), badgeColor.lighter(150), 10);
        pill.pill = QColor(badgeColor.red(), badgeColor.green(), badgeColor.blue(), 72);
        runs.push_back(pill);
    }
    // Set after every frame: the same line changes nothing, and a new one lays the overlay out
    // again only when its size changed.
    const QSize before = stats_->sizeHint();
    if (stats_->setRuns(runs) && stats_->sizeHint() != before) relayout();
}

void OverlayHost::setRefine(bool refining, int frames, int target, double msPerFrame) {
    std::vector<ShadowText::Run> runs;
    ShadowText::Run lead;
    lead.dot = refining && frames < target ? theme::yellow() : theme::emerald();
    lead.text = refining ? tr("Refining") : tr("Interactive");
    lead.font = theme::uiFont(12, 500);
    lead.color = theme::textSoft();
    runs.push_back(lead);
    ShadowText::Run numbers;
    numbers.text = refining ? QStringLiteral("%1 / %2").arg(frames).arg(target) : QStringLiteral("%1 ms").arg(msPerFrame, 0, 'f', 1);
    numbers.font = theme::numberFont(12);
    numbers.color = theme::textSoft();
    numbers.gapBefore = 6;
    runs.push_back(numbers);
    const QSize before = refine_->sizeHint();
    if (refine_->setRuns(runs) && refine_->sizeHint() != before) relayout();
}

void OverlayHost::toast(const QString& text, bool error) {
    toastError_ = error;
    toastText_->setText(text);
    // An error tinted red with a red dot, its text bright; an info message plain, smaller and
    // softer, with a green dot.
    const QColor base = theme::glassSolid(), tint = theme::red();
    constexpr float t = 0.3f;
    const QColor errorFill = QColor::fromRgbF(base.redF() + (tint.redF() - base.redF()) * t, base.greenF() + (tint.greenF() - base.greenF()) * t,
                                              base.blueF() + (tint.blueF() - base.blueF()) * t, std::max(base.alphaF(), 0.92f));
    toast_->setColor(error ? errorFill : base);
    toastText_->setFont(theme::uiFont(error ? 13 : 12, error ? 600 : 500));
    toastText_->setStyleSheet(QStringLiteral("color: %1; font-size: %2px; font-weight: %3;")
                                  .arg((error ? theme::text() : theme::textSoft()).name())
                                  .arg(error ? 13 : 12)
                                  .arg(error ? 600 : 500));
    toastDot_->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg((error ? theme::redLight() : theme::emerald()).name()));
    sizeToast();
    toast_->show();
    relayout();
    toast_->raise();
    // One timer, restarted: a new toast gets its whole time (an error long enough to read it).
    toastTimer_->start(toastMilliseconds());
}

int OverlayHost::toastMilliseconds() const {
    const int length = static_cast<int>(toastText_->text().size());
    return toastError_ ? std::clamp(2500 + 45 * length, 4500, 10000) : std::clamp(1500 + 30 * length, 2500, 4500);
}

bool OverlayHost::toastShown() const { return toast_->isVisible(); }

QString OverlayHost::toastText() const { return toastText_->text(); }

void OverlayHost::sizeToast() {
    // As wide as its text up to 560 px (within the view), wrapped past that.
    const int maxWidth = std::max(160, std::min(560, width() - 48));
    toastText_->ensurePolished();  // (its style sheet sets the font measured here)
    const int chrome = 14 + 16 + 8 + 10;  // the margins, the dot, the gap
    const int textWidth = std::clamp(QFontMetrics(toastText_->font()).horizontalAdvance(toastText_->text()) + 4, 40, maxWidth - chrome);
    toastText_->setFixedWidth(textWidth);
    toast_->adjustSize();
}

void OverlayHost::showPrompt(const QString& title, const QString& text, std::vector<PromptChoice> choices) {
    // Filled again each time: its title, its sentence, its buttons with their actions.
    delete prompt_->layout();
    for (QWidget* child : prompt_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) delete child;
    auto* box = new QVBoxLayout(prompt_);
    box->setContentsMargins(24, 20, 24, 20);
    box->setSpacing(10);
    auto* heading = new QLabel(title, prompt_);
    heading->setObjectName(QStringLiteral("promptTitle"));
    heading->setFont(theme::uiFont(16, 800));
    heading->setStyleSheet(QStringLiteral("color: %1; font-size: 16px; font-weight: 800;").arg(theme::text().name()));
    box->addWidget(heading);
    auto* body = new QLabel(text, prompt_);
    body->setObjectName(QStringLiteral("promptText"));
    body->setTextFormat(Qt::PlainText);
    body->setWordWrap(true);
    body->setFont(theme::uiFont(13, 500));
    body->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 500;").arg(theme::textSoft().name()));
    box->addWidget(body);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 8, 0, 0);
    row->setSpacing(8);
    row->addStretch(1);
    RoundButton* focus = nullptr;
    for (PromptChoice& choice : choices) {
        auto* b = new RoundButton(prompt_);
        b->setText(choice.label);
        b->setDiameter(34);
        b->setRadius(theme::glassy() ? 12.0 : 5.0);
        b->setPadding(16);
        b->setFontSize(13);
        b->setFontWeight(700);
        const QColor fill = choice.primary ? theme::actionFill(theme::yellow()) : theme::chip();
        const QColor ink = choice.primary ? theme::actionText(theme::ink()) : theme::text();
        b->setFill(fill, fill);
        b->setTextColor(ink, ink);
        b->setFocusPolicy(Qt::StrongFocus);
        if (choice.primary) focus = b;
        connect(b, &QAbstractButton::clicked, this, [this, action = std::move(choice.action)] {
            hidePrompt();
            if (action) action();
        });
        row->addWidget(b);
    }
    box->addLayout(row);
    prompt_->setFixedWidth(std::max(200, std::min(440, width() - 48)));
    prompt_->adjustSize();
    prompt_->show();
    relayout();
    prompt_->raise();
    if (focus) focus->setFocus();
}

void OverlayHost::showTextPrompt(const QString& title, const QString& text, const QString& initial, const QString& accept,
                                 std::function<void(const QString&)> done) {
    std::vector<TextChoice> accepts;
    accepts.emplace_back(accept, std::move(done));
    showTextPrompt(title, text, initial, std::move(accepts));
}

void OverlayHost::showTextPrompt(const QString& title, const QString& text, const QString& initial, std::vector<TextChoice> accepts) {
    if (accepts.empty()) return;
    auto* line = new QLineEdit(initial);
    line->setObjectName(QStringLiteral("promptLine"));
    line->setFont(theme::monoFont(13));
    const QString accept = accepts.back().first;
    std::vector<PromptChoice> choices{{tr("Cancel"), false, {}}};
    for (std::size_t i = 0; i < accepts.size(); ++i) {
        auto take = std::make_shared<std::function<void(const QString&)>>(std::move(accepts[i].second));
        choices.push_back({accepts[i].first, i + 1 == accepts.size(), [line, take] {
                               if (*take) (*take)(line->text().trimmed());
                           }});
    }
    showPrompt(title, text, std::move(choices));
    // The line under the sentence, before the buttons; Enter accepts, Escape cancels.
    auto* box = static_cast<QVBoxLayout*>(prompt_->layout());
    line->setParent(prompt_);
    box->insertWidget(2, line);
    connect(line, &QLineEdit::returnPressed, this, [this, accept] { choosePrompt(accept); });
    line->installEventFilter(this);
    prompt_->adjustSize();
    relayout();
    line->setFocus();
    line->selectAll();
}

QString OverlayHost::promptLine() const {
    const auto* line = prompt_->findChild<QLineEdit*>(QStringLiteral("promptLine"));
    return line && prompt_->isVisible() ? line->text() : QString();
}

void OverlayHost::setPromptLine(const QString& text) {
    if (auto* line = prompt_->findChild<QLineEdit*>(QStringLiteral("promptLine"))) line->setText(text);
}

bool OverlayHost::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape &&
        watched->objectName() == QStringLiteral("promptLine")) {
        hidePrompt();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void OverlayHost::hidePrompt() { prompt_->hide(); }

bool OverlayHost::promptShown() const { return prompt_->isVisible(); }

bool OverlayHost::choosePrompt(const QString& label) {
    if (!prompt_->isVisible()) return false;
    for (RoundButton* b : prompt_->findChildren<RoundButton*>()) {
        if (b->text() == label) {
            b->click();
            return true;
        }
    }
    return false;
}

void OverlayHost::setMode(Mode mode) {
    mode_ = mode;
    refreshEmptyCard();
    refreshToolLabel();
    relayout();
}

bool OverlayHost::viewportResizePending() const { return viewportTimer_->isActive(); }

void OverlayHost::placeViewport() {
    // At once, unless a mouse button is held (a dock's or a splitter's drag): then a moment after
    // the last move (the timer), so the viewport's texture is not made again for every pixel.
    if (viewport_->size() == size() && viewport_->pos() == QPoint(0, 0)) return;
    if (QGuiApplication::mouseButtons() != Qt::NoButton && isVisible()) {
        viewportTimer_->start();
        return;
    }
    viewportTimer_->stop();
    viewport_->setGeometry(rect());
}

void OverlayHost::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (built_ && toast_->isVisible()) sizeToast();
    if (built_ && prompt_->isVisible()) {
        prompt_->setFixedWidth(std::max(200, std::min(440, width() - 48)));
        prompt_->adjustSize();
    }
    relayout();
}

void OverlayHost::setUndoRedo(bool canUndo, bool canRedo, const QString& undoTip, const QString& redoTip) {
    // Dim while there is nothing to undo or redo; the tip names the step and its keys.
    undo_->setEnabled(canUndo);
    redo_->setEnabled(canRedo);
    undo_->setIconColor(canUndo ? theme::text() : theme::textMuted(), theme::text());
    redo_->setIconColor(canRedo ? theme::text() : theme::textMuted(), theme::text());
    undo_->setToolTip(undoTip);
    redo_->setToolTip(redoTip);
}

void OverlayHost::relayout() {
    const QSize size = this->size();
    if (!built_ || size.isEmpty()) return;
    placeViewport();
    theme::ViewportOverlayRequest request;
    request.view = size;
    if (toolLabel_->isVisibleTo(this)) request.toolLabel = toolLabel_->sizeHint();
    if (history_->isVisibleTo(this)) request.history = history_->sizeHint();
    if (stats_->isVisibleTo(this)) request.stats = stats_->sizeHint();
    if (refine_->isVisibleTo(this)) request.refine = refine_->sizeHint();
    if (toast_->isVisibleTo(this)) request.toast = toast_->size();
    if (prompt_->isVisibleTo(this)) request.prompt = prompt_->size();
    if (emptyCard_->isVisibleTo(this)) request.emptyCard = emptyCard_->size();
    const theme::ViewportOverlayLayout layout = theme::layoutViewportOverlay(request);
    auto place = [](QWidget* w, const QRect& r) {
        if (!r.isNull() && w->geometry() != r) w->setGeometry(r);
    };
    place(toolLabel_, layout.toolLabel);
    place(history_, layout.history);
    place(stats_, layout.stats);
    place(refine_, layout.refine);
    place(toast_, layout.toast);
    place(prompt_, layout.prompt);
    place(emptyCard_, layout.emptyCard);
}

}  // namespace groomlab::app
