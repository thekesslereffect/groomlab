// SPDX-License-Identifier: Apache-2.0
#include "maps_panel.h"

#include <cstdint>

#include <QAbstractItemDelegate>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "brush_session.h"
#include "glass.h"
#include "groom_document.h"
#include "groomlab/stroke.h"
#include "card_parts.h"
#include "list_bar.h"
#include "param_editors.h"
#include "theme.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kChannelsRole = Qt::UserRole + 2;
constexpr int kMissingRole = Qt::UserRole + 3;  // a texture map's expected image when it is missing

RoundButton* actionButton(const QString& text, const QColor& fill, const QColor& textColor, QWidget* parent) {
    auto* b = new RoundButton(parent);
    b->setText(text);
    b->setDiameter(30);
    b->setRadius(10.0);
    b->setPadding(12);
    b->setFontSize(12);
    b->setFontWeight(700);
    b->setFill(fill, fill);
    b->setTextColor(textColor, textColor);
    return b;
}

QString kindLabel(const std::string& kind) {
    if (kind == "vertices") return MapsPanel::tr("vertices");
    if (kind == "texture") return MapsPanel::tr("texture");
    return MapsPanel::tr("guides");
}

// The name rule every host keeps (scalp_maps.h validMapName), so a map made here reads in Maya.
QString nameRule() {
    return MapsPanel::tr("A map's name: letters, digits and _, not starting with a digit (as Maya names them)");
}

// A small swatch for the list (Maya's _map_icon): a gray ramp for a gray map, red green blue for a
// color map.
QIcon mapIcon(bool color, qreal dpr) {
    const int size = 12;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setPen(Qt::NoPen);
    if (color) {
        const int third = size / 3;
        const QColor colors[] = {QColor(0xe0, 0x46, 0x4b), QColor(0x3f, 0xbf, 0x5f), QColor(0x3d, 0x7e, 0xff)};
        for (int i = 0; i < 3; ++i) p.fillRect(QRect(i * third, 0, i < 2 ? third : size - 2 * third, size), colors[i]);
    } else {
        QLinearGradient ramp(0, 0, size, 0);
        ramp.setColorAt(0.0, QColor(0x10, 0x10, 0x10));
        ramp.setColorAt(1.0, QColor(0xf0, 0xf0, 0xf0));
        p.fillRect(QRect(0, 0, size, size), QBrush(ramp));
    }
    p.end();
    return QIcon(pixmap);
}

QString choiceTip(const char* list, const std::string& value) {
    for (const ui::json& c : defs().choices(list)) {
        if (c.value("value", std::string()) == value) return qs(c.value("tip", std::string()));
    }
    return {};
}

}  // namespace

MapsPanel::MapsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("mapsPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    QWidget* host = this;

    // The header: Maps, how many, + New Map and Import (glyphs).
    header_ = new ListHeader(tr("Maps"), host);
    header_->setObjectName(QStringLiteral("mapsHeader"));
    bar_ = new ListBar(header_);
    bar_->setObjectName(QStringLiteral("mapsBar"));
    header_->addGlyph(bar_);
    layout->addWidget(header_);
    list_ = new QListWidget(host);
    list_->setObjectName(QStringLiteral("mapsList"));
    plainRows(list_);
    list_->setToolTip(tr("A map is a 0-1 value painted on the scalp that any param can read (in the Graph Editor, right-click a "
                         "param > Paint). Double-click: rename; right-click: more"));
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);  // a double-click edits the name alone (editSelectedName)
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_);
    hint_ = new QLabel(tr("No maps yet: + New Map (For: a setting such as Density, or blank), or in the Graph Editor right-click a "
                          "param > Paint."),
                       host);
    hint_->setWordWrap(true);
    hint_->setProperty("role", "hint");
    layout->addWidget(hint_);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem*, QListWidgetItem*) {
        if (updating_) return;
        refreshRows();  // the paint and show rows follow the picked map (the list itself stays)
    });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) { itemRenamed(item); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { editSelectedName(); });
    // The edit closed (taken or dropped): the rows show their kinds again.
    connect(list_->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this] {
        QTimer::singleShot(0, this, [this] {
            shownKey_.clear();
            refresh();
        });
    });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (QMenu* menu = contextMenu(pos)) menu->popup(list_->viewport()->mapToGlobal(pos));
    });

    // ---- A new map (+ New Map shows it).
    newBox_ = new QGroupBox(tr("New map"), host);
    newBox_->setObjectName(QStringLiteral("newMapForm"));
    auto* newForm = new QFormLayout(newBox_);
    newFor_ = new QComboBox(newBox_);
    newFor_->setObjectName(QStringLiteral("newMapFor"));
    newFor_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    newFor_->setMinimumContentsLength(12);
    newFor_->setToolTip(qs(defs().get("lists/maps").value("forTip", std::string())));
    newForm->addRow(tr("For"), newFor_);
    newOther_ = new QComboBox(newBox_);
    newOther_->setObjectName(QStringLiteral("newMapOther"));
    newOther_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    newOther_->setMinimumContentsLength(12);
    newOther_->setToolTip(tr("Every mappable param of the graph"));
    newForm->addRow(tr("Setting"), newOther_);
    connect(newFor_, &QComboBox::currentIndexChanged, this, [this](int) { targetChanged(); });
    newName_ = new QLineEdit(newBox_);
    newName_->setPlaceholderText(tr("density, mask, tint..."));
    newName_->setToolTip(nameRule());
    newForm->addRow(tr("Name"), newName_);
    newKind_ = new QComboBox(newBox_);
    for (const ui::json& c : defs().choices("mapKind")) {
        newKind_->addItem(qs(c.value("label", std::string())), qs(c.value("value", std::string())));
        newKind_->setItemData(newKind_->count() - 1, qs(c.value("tip", std::string())), Qt::ToolTipRole);
    }
    if (newKind_->findData(QStringLiteral("vertices")) >= 0) newKind_->setCurrentIndex(newKind_->findData(QStringLiteral("vertices")));
    newKind_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    newKind_->setMinimumContentsLength(12);
    newKind_->setToolTip(tr("Where the map lives: the scalp's vertices (saved with the groom), an image file beside the groom (the "
                            "finest detail; the scalp needs UVs), or the guides (children inherit)"));
    newForm->addRow(tr("Kind"), newKind_);
    newColor_ = new QCheckBox(tr("color (r g b a)"), newBox_);
    newColor_->setToolTip(choiceTip("mapValues", "color"));
    newForm->addRow(QString(), newColor_);
    newValue_ = new ValueSlider(newBox_);
    newValue_->setCaption(tr("Fill"));
    newValue_->setRange(0.0, 1.0);
    newValue_->setLimits(0.0, 1.0);
    newValue_->setDecimals(3);
    newValue_->setValue(1.0);  // a mask that lets everything through until painted (Maya's default too)
    newValue_->setFill(theme::yellow());
    newValue_->setToolTip(tr("What the map starts filled with"));
    newForm->addRow(newValue_);
    newResolution_ = new QSpinBox(newBox_);
    newResolution_->setRange(16, 8192);
    newResolution_->setSingleStep(256);
    newResolution_->setValue(1024);
    newResolution_->setToolTip(tr("A texture map's image size (square)"));
    newForm->addRow(tr("Resolution"), newResolution_);
    RoundButton* create = actionButton(tr("Create"), theme::actionFill(theme::yellow()), theme::actionText(theme::ink()), newBox_);
    newForm->addRow(QString(), create);
    layout->addWidget(newBox_);
    newBox_->setVisible(false);
    auto kindChanged = [this] {
        const QString kind = newKind_->currentData().toString();
        newResolution_->setEnabled(kind == QStringLiteral("texture"));
        newColor_->setEnabled(kind != QStringLiteral("guides") && newMapTarget().first.empty());
    };
    connect(newKind_, &QComboBox::currentIndexChanged, this, kindChanged);
    kindChanged();
    connect(create, &QAbstractButton::clicked, this, [this] {
        const std::string error = createFromFields();
        if (!error.empty()) emit message(QString::fromStdString(error));
        else setNewMapFormShown(false);
    });

    // ---- Painting the picked map: the mode, the value (a gray map) or the color and channels, Fill
    // (paintControls: the Groom Tools' Paint card shows them).
    paintControls_ = new QWidget(host);
    paintControls_->setObjectName(QStringLiteral("paintControls"));
    QWidget* paintBox = paintControls_;
    auto* paintLayout = new QVBoxLayout(paintBox);
    paintLayout->setContentsMargins(0, 0, 0, 0);
    paintLayout->setSpacing(4);
    {
        FlowLayout* row = nullptr;
        paintLayout->addWidget(chipRow(paintBox, &row, tr("Mode")));
        auto* group = new QButtonGroup(paintBox);
        group->setExclusive(true);
        for (const ui::json& choice : defs().choices("paintMode")) {
            const std::string value = choice.value("value", std::string());
            auto* button = new QToolButton(paintBox);
            button->setText(qs(choice.value("label", std::string())));
            button->setToolTip(qs(choice.value("tip", std::string())));
            button->setCheckable(true);
            button->setProperty("role", "chip");
            button->setProperty("paintMode", qs(value));
            group->addButton(button);
            paintModes_.push_back(button);
            row->addWidget(button);
            connect(button, &QToolButton::clicked, this, [this, value] {
                if (session_) session_->settings().brush.paintMode = paintModeFromString(value);
                applyPaintSettings();
            });
            if (value == "replace") button->setChecked(true);
        }
    }
    valueRow_ = new QWidget(paintBox);
    auto* valueLine = new QHBoxLayout(valueRow_);
    valueLine->setContentsMargins(0, 0, 0, 0);
    valueLine->setSpacing(6);
    paintValue_ = new ValueSlider(valueRow_);
    paintValue_->setCaption(tr("Value"));
    paintValue_->setRange(0.0, 1.0);
    paintValue_->setLimits(0.0, 1.0);
    paintValue_->setDecimals(3);
    paintValue_->setValue(1.0);
    paintValue_->setFill(theme::yellow());
    paintValue_->setToolTip(tr("What the Paint brush paints toward, and what Fill sets (Ctrl: toward 0, or toward 1 when the value is "
                               "0; on a density map, Ctrl removes hair)"));
    valueLine->addWidget(paintValue_, 1);
    auto* fillValue = new QPushButton(tr("Fill"), valueRow_);
    fillValue->setObjectName(QStringLiteral("paintFill"));
    fillValue->setToolTip(tr("Set the map to the value: on the selected guides when some are (a guide map), else everywhere"));
    valueLine->addWidget(fillValue);
    paintLayout->addWidget(valueRow_);
    colorRow_ = new QWidget(paintBox);
    auto* colorLayout = new QVBoxLayout(colorRow_);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->setSpacing(4);
    paintColor_ = new ColorEditor(colorRow_);
    paintColor_->setToolTip(tr("The color the Paint brush paints on a color map (Ctrl: its inverse)"));
    colorLayout->addWidget(paintColor_);
    auto* channelsRow = new QHBoxLayout();
    channelsRow->setContentsMargins(0, 0, 0, 0);
    channelsRow->setSpacing(3);
    paintAlpha_ = new ValueSlider(colorRow_);
    paintAlpha_->setCaption(tr("a"));
    paintAlpha_->setRange(0.0, 1.0);
    paintAlpha_->setLimits(0.0, 1.0);
    paintAlpha_->setDecimals(2);
    paintAlpha_->setValue(1.0);
    paintAlpha_->setTrackWidth(60);
    paintAlpha_->setValueWidth(34);
    paintAlpha_->setFill(theme::yellow());
    paintAlpha_->setToolTip(tr("The alpha painted toward"));
    channelsRow->addWidget(paintAlpha_);
    for (const char* c : {"r", "g", "b", "a"}) {
        QToolButton* box = makeChip(QString::fromLatin1(c).toUpper(), choiceTip("paintChannel", c), colorRow_);
        box->setObjectName(QStringLiteral("paintChannel_") + QLatin1String(c));
        box->setChecked(true);
        channelsRow->addWidget(box);
        paintChannels_.push_back(box);
        connect(box, &QToolButton::toggled, this, [this] { applyPaintSettings(); });
    }
    channelsRow->addStretch(1);
    auto* fillColor = new QPushButton(tr("Fill"), colorRow_);
    fillColor->setObjectName(QStringLiteral("paintFillColor"));
    fillColor->setToolTip(tr("Fill the map with the color, in the channels ticked"));
    channelsRow->addWidget(fillColor);
    colorLayout->addLayout(channelsRow);
    paintLayout->addWidget(colorRow_);
    paint_ = actionButton(tr("Paint this map"), theme::actionFill(theme::yellow()), theme::actionText(theme::ink()), paintBox);
    paint_->setToolTip(tr("The Paint brush on this map, the viewport in brush mode (B): drag to paint, Ctrl the opposite, Shift "
                          "smooths"));
    paintLayout->addWidget(paint_);
    paint_->hide();  // (the Paint tile over the card does it)
    connect(paintValue_, &ValueSlider::valueChanged, this, [this] { applyPaintSettings(); });
    connect(paintAlpha_, &ValueSlider::valueChanged, this, [this] { applyPaintSettings(); });
    connect(paintColor_, &ColorEditor::edited, this, [this](bool) { applyPaintSettings(); });
    connect(paint_, &QAbstractButton::clicked, this, [this] { paintSelected(); });
    for (QPushButton* fill : {fillValue, fillColor}) {
        connect(fill, &QPushButton::clicked, this, [this] {
            const std::string error = fillSelected();
            if (!error.empty()) emit message(QString::fromStdString(error));
        });
    }

    // ---- The picked map's box: rename, convert, delete; where it shows; what reads it.
    QVBoxLayout* boxRows = nullptr;
    box_ = itemBox(host, &boxRows);
    box_->setObjectName(QStringLiteral("mapBox"));
    itemBar_ = new ListBar(box_);
    itemBar_->setObjectName(QStringLiteral("mapItemBar"));
    boxRows->addWidget(itemBar_);
    FlowLayout* showChips = nullptr;
    boxRows->addWidget(chipRow(box_, &showChips, tr("Show on")));
    show_ = makeChip(tr("Scalp"), tr("Show the map on the scalp (black to white), whatever its kind"), box_);
    show_->setObjectName(QStringLiteral("paintShowOnScalp"));
    showChips->addWidget(show_);
    channel_ = new QComboBox(box_);
    channel_->setObjectName(QStringLiteral("paintScalpChannel"));
    for (const ui::json& c : defs().choices("scalpChannel")) channel_->addItem(qs(c.value("label", std::string())), qs(c.value("value", std::string())));
    channel_->setToolTip(qs(defs().get("choices/scalpChannel").value("tip", std::string())));
    showChips->addWidget(channel_);
    connect(show_, &QToolButton::toggled, this, [this](bool on) {
        if (updating_) return;
        showSelected(on);
    });
    connect(channel_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updating_ || !show_->isChecked()) return;
        showSelected(true);
    });
    users_ = new QLabel(box_);
    users_->setObjectName(QStringLiteral("mapUsers"));
    users_->setWordWrap(true);
    users_->setProperty("role", "hint");
    boxRows->addWidget(users_);
    layout->addWidget(box_);
    fitRows(list_);
    setEnabled(false);
}

void MapsPanel::setActions(const ui::Bindings& bindings, const ui::FillOptions& options) {
    bindings_ = bindings;
    options_ = options;
    options_.liveShortcuts = false;
    // + New Map and Import in the header (glyphs), the picked map's rename, convert, delete in its box.
    bar_->build(defs().get("lists/maps/bar"), bindings_, options_, ListBar::Part::Labeled);
    itemBar_->build(defs().get("lists/maps/bar"), bindings_, options_, ListBar::Part::Glyphs);
    if (QToolButton* convert = itemBar_->button("map.convert")) convert->setObjectName(QStringLiteral("paintConvert"));
}

void MapsPanel::setDocument(GroomDocument* document) {
    document_ = document;
    setEnabled(document_ != nullptr);
    shownKey_.clear();
    targetsKey_.clear();
    refresh();
}

void MapsPanel::setSession(BrushSession* session) { session_ = session; }

void MapsPanel::fresh() const {
    if (stale_) const_cast<MapsPanel*>(this)->refresh();
}

std::string MapsPanel::selectedMap() const {
    fresh();
    return list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString().toStdString() : std::string();
}

std::string MapsPanel::selectedKind() const {
    fresh();
    return list_->currentItem() ? list_->currentItem()->data(kKindRole).toString().toStdString() : std::string();
}

int MapsPanel::selectedChannels() const {
    fresh();
    return list_->currentItem() ? std::max(1, list_->currentItem()->data(kChannelsRole).toInt()) : 1;
}

std::string MapsPanel::selectedFile() const {
    const std::string name = selectedMap();
    return document_ && !name.empty() ? document_->textureMapFile(name) : std::string();
}

std::string MapsPanel::selectedMissing() const {
    fresh();
    return list_->currentItem() ? list_->currentItem()->data(kMissingRole).toString().toStdString() : std::string();
}

QString MapsPanel::usersText() const {
    fresh();
    return users_->text();
}

void MapsPanel::selectMap(const std::string& name) {
    fresh();
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString().toStdString() == name) {
            list_->setCurrentRow(i);
            return;
        }
    }
}

std::set<std::string> MapsPanel::menuState() const {
    std::set<std::string> state;
    if (selectedMap().empty()) return state;
    state.insert("map");
    const std::string kind = selectedKind();
    if (kind == "vertices" || kind == "texture") {
        state.insert("map.scalp");
        if (selectedChannels() == kColorMap) state.insert("map.color");
    }
    if (kind == "texture") state.insert("map.texture");
    return state;
}

QMenu* MapsPanel::contextMenu(const QPoint& pos) {
    fresh();
    if (QListWidgetItem* item = list_->itemAt(pos)) list_->setCurrentItem(item);
    return listMenu(this, defs().get("lists/maps/context"), bindings_, options_, menuState());
}

void MapsPanel::editSelectedName() {
    fresh();
    if (QListWidgetItem* item = list_->currentItem()) {
        // The name alone while it is edited (the row shows its kind beside it).
        updating_ = true;
        item->setText(item->data(Qt::UserRole).toString());
        updating_ = false;
        list_->editItem(item);
    }
}

void MapsPanel::itemRenamed(QListWidgetItem* item) {
    if (updating_ || !item) return;
    const QString name = item->data(Qt::UserRole).toString();
    const QString text = item->text().trimmed();
    if (text.isEmpty() || text == name || text.startsWith(name + QStringLiteral("   ")) || text.contains(QStringLiteral("   ("))) return;
    // (After the item's own signal: the list is rebuilt.)
    QTimer::singleShot(0, this, [this, text] {
        const std::string error = renameSelected(text);
        if (!error.empty()) {
            emit message(QString::fromStdString(error));
            shownKey_.clear();
            refresh();
        }
    });
}

void MapsPanel::setNewMapFormShown(bool shown) {
    if (shown) {
        targetsKey_.clear();  // the choices as the graph has them now
        refreshTargets();
    }
    newBox_->setVisible(shown);
}

bool MapsPanel::newMapFormShown() const { return newBox_->isVisibleTo(const_cast<MapsPanel*>(this)); }

void MapsPanel::invalidate() {
    stale_ = true;
    if (!isVisible() || pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        if (stale_ && isVisible()) refresh();
    });
}

void MapsPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (stale_) refresh();
}

void MapsPanel::refresh() {
    stale_ = false;
    std::vector<PaintedMap> maps;
    std::vector<std::string> missing;
    std::string key = document_ ? std::to_string(reinterpret_cast<std::uintptr_t>(document_)) : std::string("-");
    if (document_) {
        maps = document_->maps();
        for (const PaintedMap& m : maps) {
            missing.push_back(m.kind == "texture" ? document_->missingMapFile(m.name) : std::string());
            key += "|" + m.name + ":" + m.kind + ":" + std::to_string(m.channels) + ":" + m.texture + ":" + missing.back();
        }
    }
    if (key != shownKey_) {
        shownKey_ = key;
        ++rebuilds_;
        updating_ = true;
        const std::string current = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString().toStdString() : std::string();
        list_->clear();
        for (std::size_t i = 0; i < maps.size(); ++i) {
            const PaintedMap& m = maps[i];
            const bool color = m.channels == kColorMap;
            const bool lost = !missing[i].empty();
            auto* item = new QListWidgetItem(QStringLiteral("%1%2   (%3%4)")
                                                 .arg(lost ? QStringLiteral("⚠ ") : QString(), qs(m.name), kindLabel(m.kind),
                                                      color ? tr(", color") : QString()),
                                             list_);
            item->setIcon(mapIcon(color, devicePixelRatioF()));
            item->setData(Qt::UserRole, qs(m.name));
            item->setData(kKindRole, qs(m.kind));
            item->setData(kChannelsRole, color ? kColorMap : kGrayMap);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            QString tip = choiceTip("mapKind", m.kind) + QLatin1Char('\n') + choiceTip("mapValues", color ? "color" : "gray");
            if (!m.texture.empty()) tip += QLatin1Char('\n') + qs(m.texture);
            if (lost) {
                item->setData(kMissingRole, qs(missing[i]));
                item->setForeground(theme::warningText());
                tip = tr("The image is missing: %1\nRight-click > Relink Image... to point the map at it.").arg(qs(missing[i]));
            }
            item->setToolTip(tip);
            if (m.name == current) list_->setCurrentItem(item);
        }
        if (!list_->currentItem() && list_->count() > 0) list_->setCurrentRow(0);
        updating_ = false;
    }
    hint_->setVisible(list_->count() == 0);
    header_->setCount(list_->count());
    fitRows(list_);
    refreshTargets();
    refreshRows();
}

void MapsPanel::refreshTargets() {
    // The common settings (core's list), each enabled while the graph has a node of its type; then
    // Other: every mappable param of the graph. Made again only when the graph's nodes changed;
    // the choices kept across refreshes.
    std::string key = document_ ? std::to_string(reinterpret_cast<std::uintptr_t>(document_)) : std::string("-");
    if (document_) {
        for (const nlohmann::json& n : document_->graph().value("nodes", nlohmann::json::array())) {
            key += "|" + n.value("name", std::string()) + ":" + n.value("type", std::string());
        }
    }
    if (key == targetsKey_) return;
    targetsKey_ = key;
    const QVariant wasFor = newFor_->currentData(), wasOther = newOther_->currentData();
    {
        const QSignalBlocker blockFor(newFor_), blockOther(newOther_);
        newFor_->clear();
        newOther_->clear();
        newFor_->addItem(tr("Nothing yet (a blank map)"));
        std::string graphJson;
        if (document_) graphJson = document_->graph().dump();
        for (const PaintableSetting& s : paintableSettings()) {
            std::string node;
            try {
                if (document_) node = firstNodeOfType(graphJson, s.type);
            } catch (const std::exception&) {
            }
            newFor_->addItem(tr("Setting: %1").arg(QString::fromUtf8(s.label)),
                             node.empty() ? QVariant() : QVariant(QStringList{QString::fromStdString(node), QString::fromUtf8(s.param)}));
            const int row = newFor_->count() - 1;
            newFor_->setItemData(row, node.empty() ? tr("Add a %1 node first").arg(QString::fromUtf8(s.type)) : QString::fromUtf8(s.hint),
                                 Qt::ToolTipRole);
            if (node.empty()) newFor_->setItemData(row, 0, Qt::UserRole - 1);  // disabled: the graph has none
        }
        if (document_) {
            for (const nlohmann::json& n : document_->graph().value("nodes", nlohmann::json::array())) {
                const nlohmann::json* spec = document_->spec(n.value("type", std::string()));
                if (!spec || !spec->contains("params")) continue;
                for (const nlohmann::json& p : (*spec)["params"]) {
                    if (!p.value("mappable", false)) continue;
                    const QString node = QString::fromStdString(n.value("name", std::string()));
                    const QString param = QString::fromStdString(p.value("name", std::string()));
                    newOther_->addItem(node + QLatin1Char('.') + param, QStringList{node, param});
                    newOther_->setItemData(newOther_->count() - 1, QString::fromStdString(p.value("description", std::string())),
                                           Qt::ToolTipRole);
                }
            }
        }
        if (newOther_->count() > 0) newFor_->addItem(tr("Other setting..."), QStringLiteral("other"));
        const int f = wasFor.isValid() ? newFor_->findData(wasFor) : 0;
        newFor_->setCurrentIndex(std::max(0, f));
        if (wasOther.isValid()) newOther_->setCurrentIndex(std::max(0, newOther_->findData(wasOther)));
    }
    targetChanged();
}

void MapsPanel::targetChanged() {
    const bool other = newFor_->currentData().toString() == QStringLiteral("other");
    newOther_->setVisible(other);
    if (auto* form = qobject_cast<QFormLayout*>(newOther_->parentWidget()->layout())) {
        if (QWidget* label = form->labelForField(newOther_)) label->setVisible(other);
    }
    const bool forSetting = !newMapTarget().first.empty();
    // A setting's map is named for it (<node>_<param>), gray, and lives on the scalp.
    newName_->setEnabled(!forSetting);
    newColor_->setEnabled(!forSetting && newKind_->currentData().toString() != QStringLiteral("guides"));
    if (forSetting) {
        newColor_->setChecked(false);
        if (newKind_->currentData().toString() == QStringLiteral("guides")) newKind_->setCurrentIndex(newKind_->findData(QStringLiteral("vertices")));
    }
    newValue_->setEnabled(!forSetting);
}

bool MapsPanel::setNewMapTarget(const QString& node, const QString& param) {
    refreshTargets();
    if (node.isEmpty()) {
        newFor_->setCurrentIndex(0);
        return true;
    }
    const QStringList wanted{node, param};
    int index = newFor_->findData(wanted);
    if (index < 0) {
        const int other = newOther_->findData(wanted);
        index = newFor_->findData(QStringLiteral("other"));
        if (other < 0 || index < 0) return false;
        newOther_->setCurrentIndex(other);
    }
    newFor_->setCurrentIndex(index);
    targetChanged();
    return true;
}

std::pair<std::string, std::string> MapsPanel::newMapTarget() const {
    QVariant data = newFor_->currentData();
    if (data.toString() == QStringLiteral("other")) data = newOther_->currentData();
    const QStringList pair = data.toStringList();
    if (pair.size() != 2) return {};
    return {pair[0].toStdString(), pair[1].toStdString()};
}

void MapsPanel::setPaintValue(double value) { paintValue_->setValue(value); }

void MapsPanel::refreshRows() {
    updating_ = true;
    const std::string picked = document_ && list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString().toStdString() : std::string();
    const bool color = list_->currentItem() && list_->currentItem()->data(kChannelsRole).toInt() == kColorMap;
    colorRow_->setVisible(color);
    valueRow_->setVisible(!color);
    if (document_) {
        show_->setChecked(!picked.empty() && document_->scalpMapDisplay() == picked);
        channel_->setVisible(color);
        const int index = channel_->findData(qs(document_->scalpMapChannel()));
        channel_->setCurrentIndex(std::max(0, index));
        // What reads it (Maya's map_users).
        QStringList users;
        if (!picked.empty()) {
            for (const auto& [node, param] : document_->mapUsers(picked)) users << qs(node) + QLatin1Char('.') + qs(param);
        }
        users_->setText(!users.isEmpty() ? tr("Read by %1").arg(users.join(QStringLiteral(", ")))
                        : !picked.empty() ? tr("Not read by the graph yet")
                                          : QString());
        users_->setToolTip(users.isEmpty() && !picked.empty()
                               ? tr("+ New Map > For a setting makes a map a setting reads; or in the Graph Editor, right-click a param > "
                                    "Paint, or add a Map node.")
                               : QString());
    } else {
        users_->clear();
    }
    const bool has = !picked.empty();
    paint_->setEnabled(has);
    show_->setEnabled(has);
    valueRow_->setEnabled(has);
    colorRow_->setEnabled(has);
    box_->setVisible(has);
    bar_->refresh();
    itemBar_->refresh();
    updating_ = false;
}

void MapsPanel::setNewMapFields(const QString& name, const QString& kind, bool color, double value, int resolution) {
    newName_->setText(name);
    newKind_->setCurrentIndex(std::max(0, newKind_->findData(kind)));
    newColor_->setChecked(color);
    newValue_->setValue(value);
    newResolution_->setValue(resolution);
}

double MapsPanel::newMapFill() const { return newValue_->value(); }

bool MapsPanel::hintShown() const { return hint_->isVisibleTo(const_cast<MapsPanel*>(this)); }

std::string MapsPanel::done(const std::string& error, const std::string& select) {
    if (!error.empty()) return error;
    refresh();
    if (!select.empty()) selectMap(select);
    emit edited();
    return {};
}

std::string MapsPanel::createFromFields() {
    if (!document_) return "no groom open";
    const std::string kind = newKind_->currentData().toString().toStdString();
    const auto [node, param] = newMapTarget();
    if (!node.empty()) {  // for a setting: the host makes or finds it, links it, paints it
        emit paintParamRequested(node, param, kind);
        return {};
    }
    const std::string name = newName_->text().trimmed().toStdString();
    const bool color = newColor_->isChecked() && kind != "guides";
    const auto v = static_cast<float>(newValue_->value());
    const std::string error = document_->createMap(name, kind, color, v, newResolution_->value(), PaintColor{v, v, v, 1.0f});
    if (!error.empty()) return error;
    newName_->clear();
    return done({}, name);
}

std::string MapsPanel::renameSelected(const QString& to) {
    if (!document_) return "no groom open";
    const std::string from = selectedMap();
    if (from.empty()) return "pick a map to rename";
    const std::string error = document_->renameMap(from, to.toStdString());
    if (!error.empty()) return error;
    if (document_->scalpMapDisplay() == from) document_->setScalpMapDisplay(to.toStdString(), document_->scalpMapChannel());
    if (session_ && session_->settings().brush.attribute == from) {
        session_->settings().brush.attribute = to.toStdString();
        if (session_->settings().paintKind == "texture") session_->settings().paintTexture = document_->textureMapFile(to.toStdString());
    }
    return done({}, to.toStdString());
}

std::string MapsPanel::removeSelected() {
    if (!document_) return "no groom open";
    const std::string name = selectedMap();
    if (name.empty()) return "pick a map to remove";
    const bool shown = document_->scalpMapDisplay() == name;
    bool deleted = false;
    const std::string error = document_->removeMap(name, deleteFiles_, &deleted);
    if (!error.empty()) return error;
    if (shown) {
        document_->setScalpMapDisplay({});
        emit displayChanged();
    }
    if (deleted) emit message(tr("Deleted %1 and its file.").arg(qs(name)));
    return done({});
}

std::string MapsPanel::importImage(const QString& path) {
    if (!document_) return "no groom open";
    std::string made;
    const std::string error = document_->importMap(path.toStdString(), &made);
    return done(error, made);
}

std::string MapsPanel::relinkSelected(const QString& path) {
    if (!document_) return "no groom open";
    const std::string name = selectedMap();
    if (name.empty()) return "pick a map to relink";
    const std::string error = document_->relinkMap(name, path.toStdString());
    if (error.empty() && session_ && session_->settings().brush.attribute == name) {
        session_->settings().paintKind = "texture";
        session_->settings().paintTexture = document_->textureMapFile(name);
    }
    return done(error, name);
}

std::string MapsPanel::reloadSelected() {
    if (!document_) return "no groom open";
    const std::string name = selectedMap();
    if (name.empty()) return "pick a map to reload";
    const std::string error = document_->reloadMap(name);
    if (!error.empty()) return error;
    shownKey_.clear();  // its file may be back (or gone)
    return done({}, name);
}

std::string MapsPanel::convertSelected(const std::string& kind, int resolution) {
    if (!document_) return "no groom open";
    const std::string name = selectedMap();
    if (name.empty()) return "pick a map to convert";
    if (selectedKind() == kind) return name + " is " + kind + " already";
    const std::string error = document_->convertMap(name, kind, resolution);
    if (error.empty() && session_ && session_->settings().brush.attribute == name) {
        session_->settings().paintKind = kind;
        session_->settings().paintTexture = kind == "texture" ? document_->textureMapFile(name) : std::string();
    }
    return done(error, name);
}

std::string MapsPanel::setSelectedChannels(int channels) {
    if (!document_) return "no groom open";
    const std::string name = selectedMap();
    if (name.empty()) return "pick a map";
    return done(document_->setMapChannels(name, channels), name);
}

std::string MapsPanel::fillSelected() {
    if (!document_) return "no groom open";
    const std::string name = selectedMap();
    if (name.empty()) return "pick a map to fill";
    std::string error;
    if (selectedChannels() == kColorMap) {
        const auto rgb = paintColor_->linear();
        const PaintColor color{rgb[0], rgb[1], rgb[2], static_cast<float>(paintAlpha_->value())};
        unsigned mask = 0;
        for (std::size_t i = 0; i < paintChannels_.size(); ++i) {
            if (paintChannels_[i]->isChecked()) mask |= 1u << i;
        }
        error = document_->fillMap(name, 0.0f, false, &color, mask == 0 ? kAllChannels : mask,
                                   session_ ? session_->settings().brush.paintMode : PaintMode::Replace);
    } else {
        const bool selected = selectedKind() == "guides" && !document_->selection().empty();
        error = document_->fillMap(name, static_cast<float>(paintValue_->value()), selected, nullptr, kAllChannels,
                                   session_ ? session_->settings().brush.paintMode : PaintMode::Replace);
    }
    return done(error, name);
}

void MapsPanel::applyPaintSettings() {
    if (!session_ || updating_) return;
    BrushToolSettings& s = session_->settings();
    s.brush.value = static_cast<float>(paintValue_->value());
    for (QToolButton* button : paintModes_) {
        if (button->isChecked()) s.brush.paintMode = paintModeFromString(button->property("paintMode").toString().toStdString());
    }
    const auto rgb = paintColor_->linear();
    s.paintColor = {rgb[0], rgb[1], rgb[2], static_cast<float>(paintAlpha_->value())};
    unsigned mask = 0;
    for (std::size_t i = 0; i < paintChannels_.size(); ++i) {
        if (paintChannels_[i]->isChecked()) mask |= 1u << i;
    }
    s.paintChannels = mask == 0 ? kAllChannels : mask;
    emit brushChanged();
}

bool MapsPanel::paintSelected() {
    if (!document_ || !session_) return false;
    const std::string name = selectedMap();
    const PaintedMap* map = document_->map(name);
    if (!map) {
        emit message(tr("Pick a map to paint"));
        return false;
    }
    BrushToolSettings& s = session_->settings();
    try {
        s.setMode("paint");
    } catch (const std::invalid_argument& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    s.brush.attribute = name;
    s.paintKind = map->kind;
    s.paintTexture = map->kind == "texture" ? document_->textureMapFile(name) : std::string();
    applyPaintSettings();
    emit paintRequested();
    return true;
}

void MapsPanel::showSelected(bool show) {
    if (!document_) return;
    const std::string name = selectedMap();
    const QString channel = channel_->currentData().toString();
    document_->setScalpMapDisplay(show ? name : std::string(), channel.isEmpty() ? std::string("rgb") : channel.toStdString());
    {
        const QSignalBlocker block(show_);
        show_->setChecked(show && !name.empty());
    }
    emit displayChanged();
}

}  // namespace groomlab::app
