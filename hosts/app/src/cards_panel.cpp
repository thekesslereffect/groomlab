// SPDX-License-Identifier: Apache-2.0
#include "cards_panel.h"

#include <cmath>

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPixmap>
#include <QVBoxLayout>

#include "groomlab/card_presets.h"

#include "groom_document.h"
#include "theme.h"

namespace groomlab::app {

using nlohmann::json;

namespace {

QLabel* dotLabel(const QColor& color, int size, QWidget* parent) {
    auto* dot = new QLabel(parent);
    dot->setFixedSize(size, size);
    dot->setStyleSheet(QStringLiteral("background: %1; border-radius: %2px;").arg(color.name()).arg(size / 2));
    return dot;
}

QLabel* captionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(theme::uiFont(12, 700));
    label->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    return label;
}

// The levels of detail are the core's (card_presets.h, Maya's cards_preset too): hero 6 cards x 2
// layers a clump with flyaway cards, mid 2 x 1, low one flat card a clump.
// The LOD segmented shows them in the table's order, then Custom.
int lodCount() { return static_cast<int>(cardLodPresets().size()); }

}  // namespace

CardsPanel::CardsPanel(QWidget* parent) : GlassPanel(parent, 24.0) {
    setFixedWidth(316);
    // The rows scroll when the panel is placed shorter than them (a small screen, large text).
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* body = new QWidget;
    outer->addWidget(new FitScrollArea(body, this));
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);

    // ---- The title row: the node to edit.
    auto* title = new QHBoxLayout();
    title->setSpacing(10);
    title->addWidget(dotLabel(theme::blue(), 9, this));
    auto* titleLabel = new QLabel(tr("Cards"), this);
    titleLabel->setFont(theme::uiFont(15, 800));
    titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    title->addWidget(titleLabel);
    title->addStretch(1);
    nodes_ = new QComboBox(this);
    nodes_->setToolTip(tr("The Cards node the panel edits"));
    nodes_->setMinimumWidth(110);
    title->addWidget(nodes_);
    add_ = new RoundButton(this);
    add_->setText(tr("Add Cards node"));
    add_->setDiameter(30);
    add_->setRadius(10.0);
    add_->setPadding(12);
    add_->setFontSize(12);
    add_->setFill(theme::actionFill(theme::blue()), theme::actionFill(theme::blue()));
    add_->setTextColor(theme::actionText(Qt::white), theme::actionText(Qt::white));
    add_->setToolTip(tr("A Cards node between the groom and its output: the cards are made from every strand"));
    title->addWidget(add_);
    layout->addLayout(title);

    // ---- The preview switch.
    auto* previewRow = new QHBoxLayout();
    previewRow->setSpacing(8);
    preview_ = new Segmented(this);
    preview_->setItems({{tr("Strands"), theme::yellow(), theme::ink()}, {tr("Cards"), theme::blue(), Qt::white}});
    preview_->setBackground(theme::chip());
    preview_->setSegmentHeight(30);
    preview_->setPadding(3);
    preview_->setFontSize(12);
    preview_->setStretch(true);
    preview_->setToolTip(tr("What the viewport shows: the strands, or the cards made from them"));
    previewRow->addWidget(preview_, 1);
    textures_ = new RoundButton(this);
    textures_->setText(tr("Maps"));
    textures_->setDiameter(30);
    textures_->setRadius(10.0);
    textures_->setPadding(12);
    textures_->setFontSize(12);
    textures_->setFontWeight(800);
    textures_->setFill(theme::chip(), theme::emeraldChip());
    textures_->setTextColor(theme::textMuted(), theme::emeraldSoft());
    textures_->setCheckable(true);
    textures_->setChecked(true);
    textures_->setToolTip(tr("The cards with their baked maps (after a bake), else flat in the theme's card color"));
    previewRow->addWidget(textures_);
    layout->addLayout(previewRow);

    settings_ = new QWidget(this);
    auto* settingsLayout = new QVBoxLayout(settings_);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(12);

    // ---- The level of detail.
    auto* lodRow = new QHBoxLayout();
    lodRow->setSpacing(10);
    lodRow->addWidget(captionLabel(tr("LOD"), settings_));
    lod_ = new Segmented(settings_);
    lod_->setItems({{tr("Hero"), theme::red(), Qt::white},
                    {tr("Mid"), theme::yellow(), theme::ink()},
                    {tr("Low"), theme::emerald(), theme::ink()},
                    {tr("Custom"), theme::chip(), theme::text()}});
    lod_->setBackground(theme::chip());
    lod_->setSegmentHeight(28);
    lod_->setPadding(3);
    lod_->setFontSize(12);
    lod_->setStretch(true);
    lod_->setToolTip(tr("Hero: 6 cards x 2 layers a clump with flyaway cards; Mid: 2 cards a clump; Low: one flat card a clump "
                        "(the settings below follow)"));
    lodRow->addWidget(lod_, 1);
    settingsLayout->addLayout(lodRow);

    // ---- The main settings.
    layout->addWidget(settings_);
    addSlider(tr("Cards per clump"), "cardsPerClump", 1, 8, true, theme::blue(), tr("Each clump's strands split into this many cards"));
    addSlider(tr("Layers"), "layers", 1, 3, true, theme::blue(), tr("Depth layers per clump: inner and outer shells of cards"));
    addSlider(tr("Width scale"), "widthScale", 0.2, 3.0, false, theme::yellow(), tr("Card width, times its strands' spread"));
    addSlider(tr("Segments"), "segments", 1, 32, true, theme::yellow(), tr("Quads along each card"));
    addSlider(tr("Variations"), "variations", 0, 64, true, theme::emerald(), tr("Textures the cards share (0: every card its own)"));
    addSlider(tr("Max cards"), "maxCards", 0, 20000, true, theme::red(), tr("The card budget (0: no limit): neighbouring clumps merge to keep within it; lower LODs use fewer"));
    addSlider(tr("Thinnest strand"), "thinnestStrand", 0.0, 2.0, false, theme::emerald(),
              tr("The narrowest a strand bakes, in the atlas's pixels: 1 keeps every strand solid and at least a pixel wide (cards "
                 "cut out at a cutoff don't break up, but read denser than the groom); 0 to scale, thin strands fainter (for "
                 "blended or dithered cards)"));
    auto* atlasRow = new QHBoxLayout();
    atlasRow->setSpacing(10);
    atlasRow->addWidget(captionLabel(tr("Atlas"), settings_));
    atlas_ = new QComboBox(settings_);
    for (const int size : {512, 1024, 2048, 4096, 8192}) atlas_->addItem(QStringLiteral("%1 px").arg(size), size);
    atlas_->setToolTip(tr("The atlas resolution the bake takes (the node's atlasSize)"));
    atlasRow->addWidget(atlas_, 1);
    settingsLayout->addLayout(atlasRow);

    // ---- The stats and the bake.
    stats_ = new QLabel(this);
    stats_->setFont(theme::uiFont(12, 600));
    stats_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    stats_->setWordWrap(true);
    layout->addWidget(stats_);
    auto* bakeRow = new QHBoxLayout();
    bakeRow->setSpacing(8);
    bake_ = new RoundButton(this);
    bake_->setText(tr("Bake maps"));
    bake_->setDiameter(36);
    bake_->setRadius(12.0);
    bake_->setPadding(14);
    bake_->setFontSize(13);
    bake_->setFill(theme::actionFill(theme::yellow()), theme::actionFill(theme::yellow()));
    bake_->setTextColor(theme::actionText(theme::ink()), theme::actionText(theme::ink()));
    bake_->setToolTip(tr("Bake the cards' maps (depth, coverage, tangent, attribute, color, occlusion, normal) as TGA files beside the "
                         "groom, in Unreal's hair card layout; the viewport shows them on the cards"));
    bakeRow->addWidget(bake_, 1);
    bakeResolution_ = new QComboBox(this);
    bakeResolution_->addItem(tr("node's"), 0);
    for (const int size : {256, 512, 1024, 2048, 4096}) bakeResolution_->addItem(QStringLiteral("%1").arg(size), size);
    bakeResolution_->setToolTip(tr("The bake's resolution (the node's atlasSize by default)"));
    bakeRow->addWidget(bakeResolution_);
    layout->addLayout(bakeRow);
    bakeStatus_ = new QLabel(this);
    bakeStatus_->setFont(theme::uiFont(12, 500));
    bakeStatus_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    bakeStatus_->setWordWrap(true);
    layout->addWidget(bakeStatus_);
    atlasImage_ = new QLabel(this);
    atlasImage_->setFixedSize(200, 200);
    atlasImage_->setAlignment(Qt::AlignCenter);
    atlasImage_->setStyleSheet(QStringLiteral("background: rgba(0,0,0,0.35); border-radius: 12px;"));
    atlasImage_->setToolTip(tr("The baked atlas: the color map with the coverage as alpha"));
    atlasImage_->hide();
    layout->addWidget(atlasImage_, 0, Qt::AlignHCenter);
    layout->addStretch(1);  // docked in a tall window: the rows at the top

    connect(nodes_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        node_ = index >= 0 ? nodes_->itemData(index).toString().toStdString() : std::string();
        refresh();
    });
    connect(add_, &QAbstractButton::clicked, this, &CardsPanel::addNodeRequested);
    connect(preview_, &Segmented::changed, this, [this](int) {
        if (!updating_) emit viewChanged();
    });
    connect(textures_, &QAbstractButton::toggled, this, [this](bool) {
        if (!updating_) emit viewChanged();
    });
    connect(lod_, &Segmented::changed, this, [this](int index) {
        if (updating_ || index < 0 || index >= lodCount()) return;
        applyLod(cardLodPresets()[static_cast<std::size_t>(index)].lod);
    });
    connect(atlas_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_ || !document_ || node_.empty() || index < 0) return;
        document_->setParam(node_, "atlasSize", static_cast<std::int64_t>(atlas_->itemData(index).toInt()));
        emit edited(false);
    });
    connect(bake_, &QAbstractButton::clicked, this, &CardsPanel::bakeRequested);
    refresh();
}

void CardsPanel::addSlider(const QString& caption, const std::string& param, double min, double max, bool integer, const QColor& fill,
                           const QString& tip) {
    auto* s = new ValueSlider(settings_);
    s->setStacked(true);
    s->setCaption(caption);
    s->setRange(min, max);
    s->setFill(fill);
    s->setStep(integer ? 1.0 : 0.05);
    s->setFormat([integer](double v) { return integer ? QString::number(std::lround(v)) : QString::number(v, 'f', 2); });
    s->setToolTip(tip);
    static_cast<QVBoxLayout*>(settings_->layout())->addWidget(s);
    sliders_.push_back({param, s, integer, nullptr});
    const std::size_t index = sliders_.size() - 1;
    // A drag's steps go to the document unrecorded; the release records one edit from where
    // the drag started (as the look card's sliders do through the param panel).
    connect(s, &ValueSlider::pressed, this, [this, index] {
        if (document_ && !node_.empty()) sliders_[index].dragStart = document_->paramOrDefault(node_, sliders_[index].param);
    });
    connect(s, &ValueSlider::valueChanged, this, [this, index](double v) {
        if (updating_ || !document_ || node_.empty()) return;
        Slider& entry = sliders_[index];
        const json value = entry.integer ? json(static_cast<std::int64_t>(std::lround(v))) : json(v);
        if (document_->setParam(node_, entry.param, value, false).empty()) {
            if (entry.dragStart.is_null()) entry.dragStart = value;
            lod_->setCurrent(lodCount());  // custom now
            emit edited(true);
        }
    });
    connect(s, &ValueSlider::released, this, [this, index] {
        if (!document_ || node_.empty()) return;
        Slider& entry = sliders_[index];
        const json now = document_->paramOrDefault(node_, entry.param);
        if (!entry.dragStart.is_null()) document_->recordEdit(node_, entry.param, entry.dragStart, now);
        entry.dragStart = nullptr;
        emit edited(false);
    });
}

void CardsPanel::setDocument(GroomDocument* document) {
    document_ = document;
    node_.clear();
    atlasShown_.clear();
    refresh();
}

bool CardsPanel::showCards() const { return preview_->current() == 1; }

bool CardsPanel::textures() const { return textures_->isChecked(); }

void CardsPanel::setShowCards(bool on) {
    updating_ = true;
    preview_->setCurrent(on ? 1 : 0);
    updating_ = false;
}

int CardsPanel::bakeResolution() const { return bakeResolution_->currentData().toInt(); }

float CardsPanel::bakeOcclusion() const { return 1.0f; }  // the full occlusion (an export may lower it)

void CardsPanel::setBakeStatus(const QString& text) { bakeStatus_->setText(text); }

std::string CardsPanel::lod() const {
    const int index = lod_->current();
    return index >= 0 && index < lodCount() ? cardLodPresets()[static_cast<std::size_t>(index)].lod : std::string();
}

void CardsPanel::setStats(std::size_t cards, std::size_t triangles, std::size_t textures, bool stale) {
    const QLocale locale(QLocale::English);
    QString text = tr("%1 cards · %2 triangles · %3 textures")
                       .arg(locale.toString(static_cast<qulonglong>(cards)), locale.toString(static_cast<qulonglong>(triangles)),
                            locale.toString(static_cast<qulonglong>(textures)));
    if (stale) text += tr("  ·  the cards changed since the bake");
    stats_->setText(text);
}

void CardsPanel::applyLod(const std::string& lod) {
    if (!document_ || node_.empty() || !findCardLodPreset(lod)) return;
    // The preset's params as Maya's cards_preset sets them (maxCards from its triangle budget),
    // one undo step.
    const std::string error = document_->applyCardPreset(node_, lod);
    if (!error.empty()) setBakeStatus(QString::fromStdString(error));
    emit edited(false);
}

void CardsPanel::refresh() {
    updating_ = true;
    const std::vector<std::string> nodes = document_ ? document_->cardsNodes() : std::vector<std::string>();
    if (node_.empty() || std::find(nodes.begin(), nodes.end(), node_) == nodes.end()) node_ = nodes.empty() ? std::string() : nodes.front();
    nodes_->clear();
    for (const std::string& n : nodes) nodes_->addItem(QString::fromStdString(n), QString::fromStdString(n));
    nodes_->setCurrentIndex(static_cast<int>(std::find(nodes.begin(), nodes.end(), node_) - nodes.begin()));
    nodes_->setVisible(!nodes.empty());
    add_->setVisible(document_ && nodes.empty());
    settings_->setVisible(!node_.empty());
    bake_->setEnabled(!node_.empty());
    if (!node_.empty()) {
        for (Slider& s : sliders_) {
            const json v = document_->paramOrDefault(node_, s.param);
            if (v.is_number()) s.slider->setValue(v.get<double>());
        }
        const json atlas = document_->paramOrDefault(node_, "atlasSize");
        const int size = atlas.is_number() ? atlas.get<int>() : 2048;
        int best = 0;
        for (int i = 0; i < atlas_->count(); ++i) {
            if (std::abs(atlas_->itemData(i).toInt() - size) < std::abs(atlas_->itemData(best).toInt() - size)) best = i;
        }
        atlas_->setCurrentIndex(best);
        // The LOD whose numbers the node has, else Custom.
        int lod = lodCount();
        for (int i = 0; i < lodCount(); ++i) {
            const CardLodPreset& l = cardLodPresets()[static_cast<std::size_t>(i)];
            auto is = [&](const char* name, std::int64_t value) {
                const json v = document_->paramOrDefault(node_, name);
                return v.is_number() && v.get<std::int64_t>() == value;
            };
            if (is("cardsPerClump", l.cardsPerClump) && is("layers", l.layers) && is("segments", l.segments) &&
                is("variations", l.variations) && is("flyawayCards", l.flyawayCards)) {
                lod = i;
                break;
            }
        }
        lod_->setCurrent(lod);
    }
    if (!document_) {
        stats_->setText(tr("No groom open"));
    } else if (node_.empty()) {
        stats_->setText(tr("Hair cards for game engines are made by a Cards node: add one to the graph."));
    }
    refreshAtlas();
    updating_ = false;
    adjustSize();
}

void CardsPanel::refreshAtlas() {
    const CardBakeFiles* bake = document_ ? document_->lastBake() : nullptr;
    if (!bake || bake->preview.empty()) {
        atlasImage_->hide();
        atlasShown_.clear();
        if (document_ && !node_.empty()) bakeStatus_->setText(tr("Not baked yet: the cards draw flat until Bake maps"));
        return;
    }
    if (atlasShown_ != bake->preview || !atlasImage_->isVisible()) {
        // The preview map as the picture (its alpha over the dark box: the coverage shows).
        QPixmap pixmap(QString::fromStdString(bake->preview));
        atlasShown_ = bake->preview;
        if (!pixmap.isNull()) {
            atlasImage_->setPixmap(pixmap.scaled(192, 192, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            atlasImage_->show();
        } else {
            atlasImage_->hide();
        }
    }
    bakeStatus_->setText(tr("Baked %1 px: %2").arg(bake->size).arg(QFileInfo(QString::fromStdString(bake->atlas)).dir().dirName()));
}

}  // namespace groomlab::app
