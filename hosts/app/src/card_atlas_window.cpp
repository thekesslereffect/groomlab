// SPDX-License-Identifier: Apache-2.0
#include "card_atlas_window.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "groom_document.h"
#include "groomlab/asset_files.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/transfer.h"
#include "card_parts.h"
#include "glass.h"
#include "main_window.h"
#include "theme.h"

namespace groomlab::app {

namespace {

constexpr const char* kMaps[] = {"Albedo", "Alpha", "Depth", "Normal", "AO", "Direction", "Root", "ID", "Seed"};
constexpr int kSizes[] = {512, 1024, 2048, 4096, 8192};

std::uint8_t byte(float v) { return static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(v, 0.0f, 1.0f))); }

float srgb(float linear) {
    linear = std::max(linear, 0.0f);
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// One map of a bake as an image to look at.
QImage mapImage(const CardMaps& maps, const QString& name) {
    const int n = maps.size;
    QImage image(n, n, QImage::Format_RGB888);
    if (n <= 0) return {};
    const std::size_t texels = static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    for (std::size_t i = 0; i < texels; ++i) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        auto gray = [&](const std::vector<float>& v) { r = g = b = i < v.size() ? v[i] : 0.0f; };
        auto vec = [&](const std::vector<float>& v) {
            if (3 * i + 2 < v.size()) r = 0.5f + 0.5f * v[3 * i], g = 0.5f + 0.5f * v[3 * i + 1], b = 0.5f + 0.5f * v[3 * i + 2];
        };
        if (name == "Albedo") {
            if (3 * i + 2 < maps.color.size()) r = srgb(maps.color[3 * i]), g = srgb(maps.color[3 * i + 1]), b = srgb(maps.color[3 * i + 2]);
            const float a = i < maps.coverage.size() ? maps.coverage[i] : 0.0f;
            r *= a, g *= a, b *= a;  // over black: what shows of the hair
        } else if (name == "Alpha") {
            gray(maps.coverage);
        } else if (name == "Depth") {
            gray(maps.depth);
        } else if (name == "Normal") {
            vec(maps.normal);
        } else if (name == "AO") {
            gray(maps.ao);
        } else if (name == "Direction") {
            vec(maps.tangent);
        } else if (name == "Root") {
            gray(maps.root);
        } else if (name == "ID") {
            gray(maps.id);
        } else if (name == "Seed") {
            gray(maps.seed);
        }
        uchar* p = image.scanLine(static_cast<int>(i / static_cast<std::size_t>(n))) + 3 * (i % static_cast<std::size_t>(n));
        p[0] = byte(r), p[1] = byte(g), p[2] = byte(b);
    }
    return image;
}

}  // namespace

// ---- The canvas ---------------------------------------------------------------------------------

AtlasCanvas::AtlasCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("cardAtlasCanvas"));
    setMinimumSize(320, 320);
    setMouseTracking(true);
}

void AtlasCanvas::setBlocks(const std::vector<std::array<float, 4>>& cells, const std::vector<QString>& names, int selected) {
    cells_ = cells;
    names_ = names;
    selected_ = selected;
    update();
}

void AtlasCanvas::setImage(QImage image) {
    image_ = std::move(image);
    update();
}

QRectF AtlasCanvas::square() const {
    const double side = std::max(1, std::min(width(), height()) - 2);
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

QRectF AtlasCanvas::rectOf(const std::array<float, 4>& c) const {
    const QRectF s = square();
    return {s.left() + c[0] * s.width(), s.top() + (1.0 - c[3]) * s.height(), (c[2] - c[0]) * s.width(), (c[3] - c[1]) * s.height()};
}

std::array<float, 4> AtlasCanvas::cellOf(const QRectF& r) const {
    const QRectF s = square();
    auto u = [&](double x) { return static_cast<float>(std::clamp((x - s.left()) / s.width(), 0.0, 1.0)); };
    auto v = [&](double y) { return static_cast<float>(std::clamp(1.0 - (y - s.top()) / s.height(), 0.0, 1.0)); };
    return {u(r.left()), v(r.bottom()), u(r.right()), v(r.top())};
}

void AtlasCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), theme::sunken());
    const QRectF s = square();
    p.fillRect(s, Qt::black);
    if (!image_.isNull()) p.drawImage(s, image_);
    p.setPen(theme::uiColor("border"));
    p.drawRect(s);
    const QColor accent = theme::uiColor("accent");
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const QRectF r = rectOf(cells_[i]);
        const bool on = static_cast<int>(i) == selected_;
        p.setPen(QPen(accent, on ? 2.5 : 1.2));
        p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), on ? 56 : 24));
        p.drawRect(r);
        p.save();
        p.setClipRect(r.adjusted(1, 1, -1, -1));  // a long name cut at its rectangle's edge
        QFont bold = font();
        bold.setWeight(QFont::DemiBold);
        p.setFont(bold);
        p.setPen(theme::text());
        p.drawText(r.adjusted(4, 2, -2, -2), Qt::AlignLeft | Qt::AlignTop, i < names_.size() ? names_[i] : QString());
        p.restore();
        if (on) p.fillRect(QRectF(r.right() - 6, r.bottom() - 6, 6, 6), accent.lighter(140));  // the resize handle
    }
}

void AtlasCanvas::mousePressEvent(QMouseEvent* event) {
    const QPointF at = event->position();
    dragging_ = -1;
    for (int i = static_cast<int>(cells_.size()) - 1; i >= 0; --i) {
        const QRectF r = rectOf(cells_[static_cast<std::size_t>(i)]);
        if (!r.adjusted(-4, -4, 4, 4).contains(at)) continue;
        dragging_ = i;
        resizing_ = std::abs(at.x() - r.right()) < 10 && std::abs(at.y() - r.bottom()) < 10;
        dragStart_ = at;
        dragRect_ = r;
        selected_ = i;
        emit picked(i);
        update();
        return;
    }
}

void AtlasCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ < 0) return;
    const QPointF d = event->position() - dragStart_;
    const QRectF s = square();
    QRectF r = dragRect_;
    if (resizing_) {
        r.setRight(std::clamp(r.right() + d.x(), r.left() + 6.0, s.right()));
        r.setBottom(std::clamp(r.bottom() + d.y(), r.top() + 6.0, s.bottom()));
    } else {
        r.translate(d);
        r.moveLeft(std::clamp(r.left(), s.left(), s.right() - r.width()));
        r.moveTop(std::clamp(r.top(), s.top(), s.bottom() - r.height()));
    }
    cells_[static_cast<std::size_t>(dragging_)] = cellOf(r);
    update();
}

void AtlasCanvas::mouseReleaseEvent(QMouseEvent*) {
    if (dragging_ < 0) return;
    const int block = dragging_;
    dragging_ = -1;
    if (rectOf(cells_[static_cast<std::size_t>(block)]) != dragRect_) emit moved(block, cells_[static_cast<std::size_t>(block)]);
}

// ---- The window --------------------------------------------------------------------------------

CardAtlasWindow::CardAtlasWindow(MainWindow* main, QWidget* parent) : QWidget(parent, Qt::Window), main_(main) {
    setObjectName(QStringLiteral("cardAtlasWindow"));
    setWindowTitle(tr("GroomLab Card Atlas"));
    resize(1000, 660);
    filling_ = true;  // (no edits while it is built: setting up a field may say it changed)
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(10);

    // ---- The atlas (left): what it shows and its two actions over the square.
    auto* left = new QVBoxLayout();
    left->setSpacing(6);
    auto* bar = new QHBoxLayout();
    bar->setSpacing(4);
    map_ = new QComboBox();
    map_->setObjectName(QStringLiteral("cardAtlasMap"));
    for (const char* m : kMaps) map_->addItem(QString::fromLatin1(m));
    map_->setToolTip(tr("The map the atlas shows (from the last Bake Preview)"));
    connect(map_, &QComboBox::currentIndexChanged, this, [this] { showMap(); });
    QFrame* showRow = fieldRow(this, new QLabel(tr("Show")), map_);
    showRow->setMaximumWidth(240);
    bar->addWidget(showRow);
    bar->addStretch(1);
    auto chip = [this](const QString& text, const char* name, const QString& tip, std::function<void()> run) {
        QToolButton* b = makeChip(text, tip, this);
        b->setCheckable(false);
        b->setObjectName(QLatin1String(name));
        connect(b, &QToolButton::clicked, this, [run = std::move(run)] { run(); });
        return b;
    };
    bar->addWidget(chip(tr("Auto Layout"), "cardAtlasLayout", tr("Every block packed again at one texel density (blocks placed by hand too)"),
                        [this] { autoLayout(); }));
    bar->addWidget(chip(tr("Bake Preview"), "cardAtlasBake", tr("Every block's hair baked small into its rectangle (the maps the exports write)"),
                        [this] { bakePreview(); }));
    left->addLayout(bar);
    canvas_ = new AtlasCanvas();
    connect(canvas_, &AtlasCanvas::picked, this, [this](int b) { pickBlock(b); });
    connect(canvas_, &AtlasCanvas::moved, this, [this](int b, std::array<float, 4> cell) { moveBlock(b, cell); });
    left->addWidget(canvas_, 1);
    auto* hint = new QLabel(tr("Drag a rectangle to move it, its corner to resize it: placed by hand from then on."));
    hint->setProperty("role", "hint");
    hint->setWordWrap(true);
    left->addWidget(hint);
    outer->addLayout(left, 3);

    // ---- The side column (right): the atlas, its blocks, the picked block, the exports.
    auto* inner = new QWidget();
    auto* scroll = new FitScrollArea(inner, this);
    scroll->setObjectName(QStringLiteral("cardAtlasSide"));
    scroll->setMinimumWidth(300);
    scroll->setMaximumWidth(380);
    auto* side = new QVBoxLayout(inner);
    side->setContentsMargins(0, 0, 4, 0);
    side->setSpacing(10);
    auto row = [this](const QString& text, QWidget* control) { return fieldRow(this, new QLabel(text), control); };
    // A number field (the library's slider): its name inside, `suffix` after the value; `commit`
    // runs once a drag is released or a value typed (not on every step of a drag).
    auto slider = [this](const QString& caption, const char* name, double lo, double hi, double min, double max, int decimals,
                         const QString& suffix, const QString& tip, std::function<void()> commit) {
        auto* s = new ValueSlider(this);
        s->setObjectName(QLatin1String(name));
        s->setCaption(caption);
        s->setRange(lo, hi);
        s->setLimits(min, max);
        s->setDecimals(decimals);
        s->setInteger(decimals == 0);
        s->setFormat([decimals, suffix](double v) { return trimmedNumber(v, decimals) + suffix; });
        s->setToolTip(tip);
        auto dragging = std::make_shared<bool>(false);
        connect(s, &ValueSlider::pressed, this, [dragging] { *dragging = true; });
        connect(s, &ValueSlider::released, this, [dragging, commit] {
            *dragging = false;
            commit();
        });
        connect(s, &ValueSlider::valueChanged, this, [this, dragging, commit] {
            if (!*dragging && !filling_) commit();
        });
        return s;
    };

    auto* head = new QHBoxLayout();
    auto* title = new QLabel(tr("Card Atlas"));
    title->setProperty("role", "title");
    head->addWidget(title, 1);
    head->addWidget(chip(tr("New Atlas"), "cardAtlasNew",
                         tr("A new document of one block (the open one is asked about first when it has unsaved edits)"),
                         [this] { newAtlas(); }));
    side->addLayout(head);

    // The atlas.
    auto* atlasBlock = new CardBlock(tr("Atlas"), inner);
    size_ = new QComboBox();
    size_->setObjectName(QStringLiteral("cardAtlasSize"));
    for (int s : kSizes) size_->addItem(QStringLiteral("%1 x %1").arg(s), s);
    size_->setToolTip(tr("The texture's size, texels square"));
    atlasBlock->rows()->addWidget(row(tr("Size"), size_));
    auto settingsChanged = [this] {
        if (filling_) return;
        CardAtlasSettings s = atlas().settings;
        s.size = size_->currentData().toInt();
        s.padding = static_cast<int>(std::lround(padding_->value()));
        s.preset = preset_->currentText().toStdString();
        s.rootAtTop = rootAtTop_->isChecked();
        if (!name_->text().trimmed().isEmpty()) s.name = name_->text().trimmed().toStdString();
        setSettings(s);
    };
    padding_ = slider(tr("Padding"), "cardAtlasPadding", 0.0, 32.0, 0.0, 256.0, 0, tr(" px"), tr("Texels between the blocks"), settingsChanged);
    atlasBlock->rows()->addWidget(padding_);
    preset_ = new QComboBox();
    preset_->setObjectName(QStringLiteral("cardAtlasPreset"));
    for (const CardMapPreset& p : cardMapPresets()) preset_->addItem(QString::fromStdString(p.name));
    preset_->setToolTip(tr("How Export Maps packs the maps (the Card Maps export's presets)"));
    atlasBlock->rows()->addWidget(row(tr("Preset"), preset_));
    name_ = new QLineEdit();
    name_->setObjectName(QStringLiteral("cardAtlasName"));
    name_->setToolTip(tr("The maps' file name ({name} of the preset's naming)"));
    atlasBlock->rows()->addWidget(row(tr("Name"), name_));
    rootAtTop_ = new QCheckBox(tr("Roots at Top"));
    rootAtTop_->setObjectName(QStringLiteral("cardAtlasRootAtTop"));
    rootAtTop_->setChecked(true);
    rootAtTop_->setToolTip(tr("Each block's root at the top of its rectangle and its tip at the bottom (as hand-made card atlases "
                              "lay hair out); off: at the bottom"));
    atlasBlock->rows()->addWidget(switchRow(this, rootAtTop_));
    connect(size_, &QComboBox::currentIndexChanged, this, settingsChanged);
    connect(preset_, &QComboBox::currentIndexChanged, this, settingsChanged);
    connect(name_, &QLineEdit::editingFinished, this, settingsChanged);
    connect(rootAtTop_, &QCheckBox::toggled, this, settingsChanged);
    side->addWidget(atlasBlock);

    // Its blocks: the list, its actions on the title row, the picked one's box under it.
    auto* blocksBlock = new CardBlock(tr("Blocks"), inner);
    auto glyph = [this, blocksBlock](const char* icon, const char* name, const QString& tip, std::function<void()> run) {
        QToolButton* b = makeGlyph(QLatin1String(icon), tip, blocksBlock);
        b->setObjectName(QLatin1String(name));
        connect(b, &QToolButton::clicked, this, [run = std::move(run)] { run(); });
        blocksBlock->addAction(b);
    };
    glyph("plus", "cardAtlasAdd", tr("Add a block beside the others: a root band, the card's graph and guides"), [this] { addBlock(); });
    glyph("copy", "cardAtlasDuplicate", tr("Duplicate this block (its card and graph)"), [this] { duplicateBlock(); });
    glyph("glyph_delete", "cardAtlasRemove", tr("Remove this block's groom from the document"), [this] { removeBlock(); });
    list_ = new QListWidget();
    list_->setObjectName(QStringLiteral("cardAtlasBlocks"));
    plainRows(list_);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!filling_ && row >= 0) pickBlock(row);
    });
    blocksBlock->rows()->addWidget(list_);
    {
        auto* empty = new QFrame(blocksBlock);
        empty->setProperty("role", "emptyQuiet");
        auto* column = new QVBoxLayout(empty);
        column->setContentsMargins(4, 6, 4, 4);
        column->setSpacing(4);
        column->addWidget(emptyIcon(QStringLiteral("layers"), empty), 0, Qt::AlignHCenter);
        auto* t = new QLabel(tr("No blocks yet"), empty);
        t->setProperty("role", "emptyTitle");
        t->setAlignment(Qt::AlignCenter);
        column->addWidget(t);
        auto* text = new QLabel(tr("Add one: a groom on a root band, baked into its own rectangle of the atlas."), empty);
        text->setProperty("role", "emptyText");
        text->setAlignment(Qt::AlignCenter);
        text->setWordWrap(true);
        column->addWidget(text);
        empty_ = empty;
        blocksBlock->rows()->addWidget(empty_);
    }
    QVBoxLayout* itemRows = nullptr;
    item_ = itemBox(blocksBlock, &itemRows);
    item_->setObjectName(QStringLiteral("cardAtlasBlockBox"));
    itemTitle_ = new QLabel(item_);
    itemTitle_->setObjectName(QStringLiteral("itemBoxTitle"));
    itemTitle_->setStyleSheet(QStringLiteral("font-weight: 600;"));
    itemRows->addWidget(itemTitle_);
    auto edited = [this] {
        if (filling_ || selected_ < 0) return;
        editBlock(selected_, static_cast<float>(width_->value()), static_cast<float>(length_->value()), static_cast<float>(thickness_->value()),
                  {static_cast<float>(tilt_->value()), static_cast<float>(turn_->value()), static_cast<float>(roll_->value())});
    };
    const QString degrees = QStringLiteral("\u00b0");
    width_ = slider(tr("Width"), "cardAtlas_width", 0.1, 10.0, 0.01, 1e4, 3, {}, tr("Across the card at its root (scene units)"), edited);
    length_ = slider(tr("Length"), "cardAtlas_length", 1.0, 40.0, 0.1, 1e5, 3, {}, tr("The card's length, root to tip"), edited);
    thickness_ = slider(tr("Root Height"), "cardAtlas_thickness", 0.05, 2.0, 0.001, 100.0, 3, {},
                        tr("The root band's height down the top of the card: where the roots spread"), edited);
    tilt_ = slider(tr("Root Tilt"), "cardAtlas_tilt", -90.0, 90.0, -90.0, 90.0, 1, degrees,
                   tr("The root band tilted back (its lower edge away from the viewer): the roots spread in depth, as a scalp seen at "
                      "an angle (90: lying flat). The guides move with it, not turn"),
                   edited);
    turn_ = slider(tr("Root Turn"), "cardAtlas_turn", -90.0, 90.0, -90.0, 90.0, 1, degrees, tr("The root band turned: one side of the card nearer"),
                   edited);
    roll_ = slider(tr("Root Roll"), "cardAtlas_roll", -90.0, 90.0, -90.0, 90.0, 1, degrees,
                   tr("The root band rolled in the card's plane: the hair leaving at a slant"), edited);
    for (ValueSlider* s : {width_, length_, thickness_, tilt_, turn_, roll_}) itemRows->addWidget(s);
    auto* sizeRow = new QHBoxLayout();
    sizeRow->setContentsMargins(2, 0, 0, 0);
    cardSize_ = new QLabel();
    cardSize_->setObjectName(QStringLiteral("cardAtlasCardSize"));
    cardSize_->setProperty("role", "hint");
    cardSize_->setToolTip(tr("The block's card as it bakes and exports: as wide and as long as its hair reaches (combed or noised "
                             "past its Width, longer or shorter than its Length), fitted before every bake, export and edit; Fit "
                             "refreshes it after grooming"));
    sizeRow->addWidget(cardSize_, 1);
    sizeRow->addWidget(chip(tr("Fit"), "cardAtlasFitNow", tr("Fit every block's card to its hair now (after combing or a graph change)"), [this] {
        say(fitCards() ? tr("Fitted the cards to the hair") : tr("The cards already fit the hair"));
    }));
    itemRows->addLayout(sizeRow);
    blocksBlock->rows()->addWidget(item_);
    side->addWidget(blocksBlock);

    // The exports.
    auto* exportBlock = new CardBlock(tr("Export"), inner);
    {
        auto* folderControls = new QWidget();
        auto* line = new QHBoxLayout(folderControls);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(4);
        folder_ = new QLineEdit();
        folder_->setObjectName(QStringLiteral("cardAtlasFolder"));
        folder_->setPlaceholderText(tr("a folder for the maps and cards"));
        folder_->setToolTip(tr("Where the maps and the card shapes go"));
        line->addWidget(folder_, 1);
        line->addWidget(chip(QStringLiteral("..."), "cardAtlasBrowse", tr("Pick the folder"), [this] {
            const QString picked = QFileDialog::getExistingDirectory(this, tr("Folder for the card atlas"), folder_->text());
            if (!picked.isEmpty()) folder_->setText(picked);
        }));
        exportBlock->rows()->addWidget(row(tr("Folder"), folderControls));
    }
    segments_ = slider(tr("Segments"), "cardAtlasSegments", 1.0, 32.0, 1.0, 64.0, 0, {}, tr("The card shapes' quads along"), [] {});
    segments_->setValue(8.0);
    exportBlock->rows()->addWidget(segments_);
    bend_ = slider(tr("Bend"), "cardAtlasBend", -90.0, 90.0, -180.0, 180.0, 1, degrees,
                   tr("The card shapes bent over their length (degrees, away from the head)"), [] {});
    exportBlock->rows()->addWidget(bend_);
    auto* exportRow = new QHBoxLayout();
    exportRow->setSpacing(6);
    auto* shapes = new QPushButton(tr("Export Card Shapes"));
    shapes->setObjectName(QStringLiteral("cardAtlasExportShapes"));
    shapes->setToolTip(tr("The blocks' cards as UV-ready meshes (FBX), one per block, to place by hand"));
    connect(shapes, &QPushButton::clicked, this, [this] {
        const QString name = QString::fromStdString(atlas().settings.name);
        exportShapes(QDir(folder_->text()).filePath(name + QStringLiteral("_cards.fbx")), static_cast<int>(std::lround(segments_->value())),
                     static_cast<float>(bend_->value()));
    });
    exportRow->addWidget(shapes, 1);
    auto* maps = new QPushButton(tr("Export Maps"));
    maps->setObjectName(QStringLiteral("cardAtlasExportMaps"));
    maps->setProperty("role", "primary");
    maps->setToolTip(tr("Every block baked into one texture set, written as the preset packs it"));
    connect(maps, &QPushButton::clicked, this, [this] { exportMaps(folder_->text()); });
    exportRow->addWidget(maps, 1);
    exportBlock->rows()->addLayout(exportRow);
    side->addWidget(exportBlock);
    status_ = new QLabel();
    status_->setObjectName(QStringLiteral("cardAtlasStatus"));
    status_->setWordWrap(true);
    status_->setProperty("role", "hint");
    status_->setVisible(false);
    side->addWidget(status_);
    side->addStretch(1);
    outer->addWidget(scroll, 2);
    // The app's look, as its other windows (Transfer, Photos) carry it: a window of its own.
    setFont(theme::uiFont(13));
    setStyleSheet(theme::styleSheet() + QStringLiteral("QWidget#cardAtlasWindow { background: %1; } QLabel { color: %2; }")
                                            .arg(theme::uiColor("panel").name(), theme::text().name()));
    setAttribute(Qt::WA_StyledBackground, true);
    filling_ = false;
    refresh();
}

QString CardAtlasWindow::status() const { return status_->text(); }

void CardAtlasWindow::say(const QString& text) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());  // (a message line that takes no room while it has nothing to say)
}

std::vector<int> CardAtlasWindow::blockGrooms() const {
    std::vector<int> out;
    for (int i = 0; i < main_->groomCount(); ++i) {
        const GroomDocument* g = main_->groomAt(i);
        if (g && g->cardBlock().find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(i);
    }
    return out;
}

CardBlockAtlas CardAtlasWindow::atlas() const {
    std::vector<std::pair<std::string, std::string>> named;
    for (const int i : blockGrooms()) {
        const GroomDocument* g = main_->groomAt(i);
        named.emplace_back(g->displayName(), g->cardBlock());
    }
    try {
        return cardBlockAtlasOf(named);
    } catch (const std::exception&) {
        return {};
    }
}

void CardAtlasWindow::writeAtlas(const CardBlockAtlas& a) {
    const std::vector<int> grooms = blockGrooms();
    for (std::size_t b = 0; b < a.blocks.size() && b < grooms.size(); ++b) {
        if (GroomDocument* g = main_->groomAt(grooms[b])) g->setCardBlock(cardBlockToJson(a.blocks[b], a.settings));
    }
}

void CardAtlasWindow::refresh() {
    const CardBlockAtlas a = atlas();
    filling_ = true;
    const int active = main_->activeGroom();
    const std::vector<int> grooms = blockGrooms();
    const auto it = std::find(grooms.begin(), grooms.end(), active);
    selected_ = it != grooms.end() ? static_cast<int>(it - grooms.begin()) : (a.blocks.empty() ? -1 : std::clamp(selected_, 0, static_cast<int>(a.blocks.size()) - 1));
    const int sizeIndex = size_->findData(a.settings.size);
    size_->setCurrentIndex(sizeIndex >= 0 ? sizeIndex : 2);
    padding_->setValue(a.settings.padding);
    const int presetIndex = preset_->findText(QString::fromStdString(a.settings.preset));
    if (presetIndex >= 0) preset_->setCurrentIndex(presetIndex);
    name_->setText(QString::fromStdString(a.settings.name));
    rootAtTop_->setChecked(a.settings.rootAtTop);
    list_->clear();
    std::vector<std::array<float, 4>> cells;
    std::vector<QString> names;
    for (const groomlab::CardBlock& b : a.blocks) {
        list_->addItem(QString::fromStdString(b.name));
        cells.push_back(b.cell);
        names.push_back(QString::fromStdString(b.name));
    }
    list_->setCurrentRow(selected_);
    fitRows(list_);
    list_->setVisible(!a.blocks.empty());
    empty_->setVisible(a.blocks.empty());
    canvas_->setBlocks(cells, names, selected_);
    filling_ = false;
    fillBlock();
}

void CardAtlasWindow::fillBlock() {
    const CardBlockAtlas a = atlas();
    const bool any = selected_ >= 0 && selected_ < static_cast<int>(a.blocks.size());
    filling_ = true;
    item_->setVisible(any);
    cardSize_->clear();
    if (any) {
        const groomlab::CardBlock& b = a.blocks[static_cast<std::size_t>(selected_)];
        itemTitle_->setText(QString::fromStdString(b.name));
        const CardFrame frame = cardBlockFrame(b);
        cardSize_->setText(tr("Card %1 x %2").arg(frame.width(), 0, 'f', 2).arg(frame.length, 0, 'f', 2));
        width_->setValue(b.width);
        length_->setValue(b.length);
        thickness_->setValue(b.thickness);
        tilt_->setValue(b.rootRotation.x);
        turn_->setValue(b.rootRotation.y);
        roll_->setValue(b.rootRotation.z);
    }
    filling_ = false;
}

bool CardAtlasWindow::newAtlas() {
    groomlab::CardBlock block;
    block.name = "block1";
    CardAtlasSettings settings;
    CardBlockAtlas a;
    a.settings = settings;
    a.blocks.push_back(block);
    updateCardBlockLayout(a);
    // (Now, or once an unsaved document is saved or let go of: the prompt over the viewport.)
    main_->confirmDiscard(tr("starting a card atlas"), [this, a] {
        if (!main_->newCardAtlas(a.blocks.front(), a.settings)) return;
        preview_.reset();
        canvas_->setImage({});
        selected_ = 0;
        refresh();
        say(tr("A new atlas of one block: Add more"));
    });
    return true;
}

bool CardAtlasWindow::addBlock() {
    if (blockGrooms().empty()) return newAtlas();
    CardBlockAtlas a = atlas();
    groomlab::CardBlock block;
    block.name = "block" + std::to_string(a.blocks.size() + 1);
    block.origin = nextCardBlockOrigin(a, block.width);
    if (!main_->addCardBlock(block, a.settings)) return false;
    CardBlockAtlas laid = atlas();
    updateCardBlockLayout(laid);
    writeAtlas(laid);
    fitCards();
    refresh();
    say(tr("Added %1").arg(QString::fromStdString(laid.blocks.back().name)));
    return true;
}

bool CardAtlasWindow::duplicateBlock() {
    const CardBlockAtlas a = atlas();
    const std::vector<int> grooms = blockGrooms();
    if (selected_ < 0 || selected_ >= static_cast<int>(a.blocks.size())) return false;
    groomlab::CardBlock block = a.blocks[static_cast<std::size_t>(selected_)];
    block.name += "_copy";
    block.placed = false;
    block.origin = nextCardBlockOrigin(a, block.width);
    const std::string graph = main_->groomAt(grooms[static_cast<std::size_t>(selected_)])->graphJson();
    if (!main_->addCardBlock(block, a.settings, graph)) return false;
    CardBlockAtlas laid = atlas();
    updateCardBlockLayout(laid);
    writeAtlas(laid);
    fitCards();
    refresh();
    return true;
}

bool CardAtlasWindow::removeBlock() {
    const std::vector<int> grooms = blockGrooms();
    if (selected_ < 0 || selected_ >= static_cast<int>(grooms.size())) return false;
    if (!main_->removeGroom(grooms[static_cast<std::size_t>(selected_)])) {
        say(tr("A document keeps one groom at least"));
        return false;
    }
    CardBlockAtlas laid = atlas();
    updateCardBlockLayout(laid);
    writeAtlas(laid);
    selected_ = -1;
    refresh();
    return true;
}

bool CardAtlasWindow::pickBlock(int block) {
    const std::vector<int> grooms = blockGrooms();
    if (block < 0 || block >= static_cast<int>(grooms.size())) return false;
    selected_ = block;
    if (main_->activeGroom() != grooms[static_cast<std::size_t>(block)]) main_->activateGroom(grooms[static_cast<std::size_t>(block)]);
    filling_ = true;
    list_->setCurrentRow(block);
    filling_ = false;
    refresh();
    return true;
}

bool CardAtlasWindow::moveBlock(int block, const std::array<float, 4>& cell) {
    CardBlockAtlas a = atlas();
    if (block < 0 || block >= static_cast<int>(a.blocks.size())) return false;
    a.blocks[static_cast<std::size_t>(block)].cell = cell;
    a.blocks[static_cast<std::size_t>(block)].placed = true;
    updateCardBlockLayout(a);
    writeAtlas(a);
    selected_ = block;
    refresh();
    return true;
}

bool CardAtlasWindow::editBlock(int block, float width, float length, float thickness, const Vec3f& rootRotation) {
    CardBlockAtlas a = atlas();
    const std::vector<int> grooms = blockGrooms();
    if (block < 0 || block >= static_cast<int>(a.blocks.size())) return false;
    groomlab::CardBlock& b = a.blocks[static_cast<std::size_t>(block)];
    const bool band = b.width != width || b.thickness != thickness || b.rootRotation.x != rootRotation.x ||
                      b.rootRotation.y != rootRotation.y || b.rootRotation.z != rootRotation.z;
    b.width = width;
    b.length = length;
    b.thickness = thickness;
    b.rootRotation = rootRotation;
    if (band) {
        // The groom onto its new band (the active groom's transfer), its guides moved, not turned:
        // each keeps its place on the band and hangs as it was shaped (card_blocks.h moveCardBlockGuides).
        if (main_->activeGroom() != grooms[static_cast<std::size_t>(block)]) main_->activateGroom(grooms[static_cast<std::size_t>(block)]);
        GroomDocument* doc = main_->groomAt(grooms[static_cast<std::size_t>(block)]);
        const groomlab::CardBlock from = atlas().blocks[static_cast<std::size_t>(block)];
        const auto authored = doc->session().authoredHierarchy();
        const std::string moved = moveCardBlockGuides(authored ? authored->toJson() : std::string(), from, b);
        TransferOptions options;
        options.method = TransferMethod::UV;
        if (!main_->transferOnto(cardBlockScalp(b), options, false, tr("its new root band"))) {
            say(tr("Can't change the root band"));
            return false;
        }
        if (!moved.empty()) {
            doc = main_->groomAt(grooms[static_cast<std::size_t>(block)]);
            doc->editGuides(tr("Move guides with the root band").toStdString(), [&moved](GuideHierarchy& h) { h = GuideHierarchy::fromJson(moved); });
        }
    }
    updateCardBlockLayout(a);
    writeAtlas(a);
    fitCards();
    refresh();
    return true;
}

bool CardAtlasWindow::fitCards() {
    CardBlockAtlas a = atlas();
    if (a.blocks.empty()) return false;
    std::vector<StrandsPtr> strands;
    for (const int i : blockGrooms()) strands.push_back(main_->groomAt(i)->fullDensityStrands());
    if (!fitCardBlocks(a, strands)) return false;
    writeAtlas(a);
    refresh();
    return true;
}

bool CardAtlasWindow::autoLayout() {
    fitCards();
    CardBlockAtlas a = atlas();
    if (a.blocks.empty()) return false;
    packCardBlocks(a);
    writeAtlas(a);
    refresh();
    return true;
}

bool CardAtlasWindow::setSettings(const CardAtlasSettings& settings) {
    CardBlockAtlas a = atlas();
    if (a.blocks.empty()) return false;
    const bool layout = settings.size != a.settings.size || settings.padding != a.settings.padding;
    a.settings = settings;
    if (layout) updateCardBlockLayout(a);
    writeAtlas(a);
    refresh();
    return true;
}

bool CardAtlasWindow::bakePreview(int resolution) {
    fitCards();
    const CardBlockAtlas a = atlas();
    if (a.blocks.empty()) return false;
    std::vector<StrandsPtr> strands;
    for (const int i : blockGrooms()) strands.push_back(main_->groomAt(i)->fullDensityStrands());
    try {
        CardMapExport settings;
        settings.resolution = resolution;
        preview_ = std::make_shared<const CardMaps>(bakeCardBlockMaps(a, strands, settings, 1.0f));
    } catch (const std::exception& e) {
        say(tr("Can't bake: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    showMap();
    say(tr("Baked the preview"));
    return true;
}

void CardAtlasWindow::showMap() { canvas_->setImage(preview_ ? mapImage(*preview_, map_->currentText()) : QImage()); }

std::vector<std::string> CardAtlasWindow::exportMaps(const QString& folder, int resolution) {
    fitCards();
    const CardBlockAtlas a = atlas();
    if (a.blocks.empty()) return {};
    if (folder.trimmed().isEmpty()) {
        say(tr("Pick a folder for the maps first"));
        return {};
    }
    const auto preset = findCardMapPreset(a.settings.preset);
    if (!preset) {
        say(tr("No card map preset named %1").arg(QString::fromStdString(a.settings.preset)));
        return {};
    }
    std::vector<StrandsPtr> strands;
    for (const int i : blockGrooms()) strands.push_back(main_->groomAt(i)->fullDensityStrands());
    try {
        CardMapExport settings;
        settings.resolution = resolution;
        const CardMaps maps = bakeCardBlockMaps(a, strands, settings, 1.0f);
        std::filesystem::create_directories(std::filesystem::u8path(folder.toStdString()));
        const std::vector<std::string> paths = writeCardMaps(maps, *preset, settings, folder.toStdString(), fileSafeName(a.settings.name));
        say(tr("Wrote %1 maps to %2").arg(paths.size()).arg(folder));
        return paths;
    } catch (const std::exception& e) {
        say(tr("Can't export: %1").arg(QString::fromUtf8(e.what())));
        return {};
    }
}

bool CardAtlasWindow::exportShapes(const QString& path, int segments, float bend) {
    fitCards();
    const CardBlockAtlas a = atlas();
    if (a.blocks.empty()) return false;
    try {
        CardShapeOptions options;
        options.segments = segments;
        options.bend = bend;
        std::filesystem::create_directories(std::filesystem::u8path(path.toStdString()).parent_path());
        writeCardBlockShapes(path.toStdString(), a, options);
    } catch (const std::exception& e) {
        say(tr("Can't export: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    say(tr("Wrote the card shapes to %1").arg(path));
    return true;
}

}  // namespace groomlab::app
