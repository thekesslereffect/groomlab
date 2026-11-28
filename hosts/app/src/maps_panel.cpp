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
#include <QFrame>
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
#include "groomlab/editor/map_edits.h"
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
constexpr int kSettingKindRole = Qt::UserRole + 4;  // New Map > For: where a setting's map lives by default

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
                         "param > Paint). Double-click: rename. Its info (what reads it, Use for, where it shows) and its menu: on the row"));
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
        refreshRows();  // the paint and show rows and the info follow the picked map (the list itself stays)
        refreshShowOn();
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
        paintLayout->addWidget(segmentRow(paintBox, &row));
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
        // Every mode on one row while each still fits (Replace, Add, Subtract, Min, Max).
        row->setMaxColumns(static_cast<int>(paintModes_.size()));
    }
    {
        // Whole Faces: each dab fills the scalp's faces under it, no falloff (BrushToolSettings::paintWholeFaces),
        // a switch row as every toggle.
        wholeFaces_ = new QCheckBox(tr("Whole Faces"), paintBox);
        wholeFaces_->setToolTip(tr("The brush fills the scalp's faces under it all the way, no falloff: masks by face (a vertex or "
                                   "texture map; Shift+drag still smooths)"));
        wholeFaces_->setObjectName(QStringLiteral("paintWholeFaces"));
        wholeFaces_->setChecked(false);
        connect(wholeFaces_, &QCheckBox::toggled, this, [this] { applyPaintSettings(); });
        paintLayout->addWidget(switchRow(paintBox, wholeFaces_));
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

    // ---- The picked map's info, under the list (issue #138: no ⓘ to open): what reads it (Show in
    // Graph), Use for (drive a setting with it), where it shows; its menu (Rename, Convert to, the
    // image, Delete) is the row's icon and the list's right-click.
    infoBlock_ = new QFrame(this);
    infoBlock_->setObjectName(QStringLiteral("mapInfoBlock"));
    infoBlock_->setProperty("role", "options");
    box_ = infoBlock_;
    auto* boxRows = new QVBoxLayout(infoBlock_);
    boxRows->setContentsMargins(6, 4, 6, 4);
    boxRows->setSpacing(4);
    layout->insertWidget(layout->indexOf(list_) + 1, infoBlock_);
    infoBlock_->hide();  // (a map picked: refreshRows)
    infoTitle_ = new QLabel(box_);
    infoTitle_->setProperty("role", "listTitle");
    boxRows->addWidget(infoTitle_);
    kindLabel_ = new QLabel(box_);
    kindLabel_->setObjectName(QStringLiteral("mapKind"));
    kindLabel_->setProperty("role", "hint");
    kindLabel_->setWordWrap(true);
    kindLabel_->hide();  // (the kind's description: not needed, issue #138)
    boxRows->addWidget(kindLabel_);
    // Show on scalp: a switch (a check box, not a chip that reads as a choice), the channel beside it.
    // A field row: the name, the channel at the value column, the switch at the row's end.
    auto* showBox = new QFrame(box_);
    showBox->setProperty("role", "propField");
    showBox->setMinimumHeight(theme::rowHeight());
    auto* showRow = new QHBoxLayout(showBox);
    showRow->setContentsMargins(8, 0, 2, 0);
    showRow->setSpacing(8);
    auto* showLabel = new QLabel(tr("Show on scalp"), showBox);
    showLabel->setProperty("role", "propLabel");
    showLabel->setToolTip(tr("Show the map on the scalp (black to white), whatever its kind"));
    showRow->addWidget(showLabel);
    show_ = new QCheckBox(showBox);
    show_->setToolTip(showLabel->toolTip());
    show_->setObjectName(QStringLiteral("paintShowOnScalp"));
    boxRows->addWidget(showBox);
    channel_ = new QComboBox(box_);
    channel_->setObjectName(QStringLiteral("paintScalpChannel"));
    for (const ui::json& c : defs().choices("scalpChannel")) channel_->addItem(qs(c.value("label", std::string())), qs(c.value("value", std::string())));
    channel_->setToolTip(qs(defs().get("choices/scalpChannel").value("tip", std::string())));
    showRow->addWidget(channel_, 1);
    showRow->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));  // (no channel: the switch still at the end)
    show_->setProperty("role", "switch");
    showRow->addWidget(show_);
    showRow->addSpacing(6);
    connect(show_, &QCheckBox::toggled, this, [this](bool on) {
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
    boxRows->insertWidget(boxRows->indexOf(kindLabel_) + 1, users_);
    {
        // Read by: the Graph Editor on the node reading it; Use for ▾: a setting wired to it.
        auto* links = new QHBoxLayout();
        links->setContentsMargins(0, 0, 0, 0);
        links->setSpacing(4);
        openReader_ = new QPushButton(tr("Show in Graph"), box_);
        openReader_->setObjectName(QStringLiteral("mapShowInGraph"));
        openReader_->setToolTip(tr("Open the Graph Editor on the node that reads this map"));
        connect(openReader_, &QPushButton::clicked, this, [this] {
            const std::string picked = selectedMap();
            if (!document_ || picked.empty()) return;
            const auto users = document_->mapUsers(picked);
            if (!users.empty()) emit readerRequested(users.front().first);
        });
        links->addWidget(openReader_);
        useFor_ = new QPushButton(tr("Use for  \u25BE"), box_);
        useFor_->setObjectName(QStringLiteral("mapUseFor"));
        useFor_->setToolTip(tr("Drive a setting with this map (the param reads it: its value at white, 0 at black)"));
        useForMenu_ = new QMenu(useFor_);
        useForMenu_->setObjectName(QStringLiteral("mapUseForMenu"));
        useForMenu_->setToolTipsVisible(true);
        connect(useForMenu_, &QMenu::aboutToShow, this, [this] { fillUseFor(useForMenu_); });
        useFor_->setMenu(useForMenu_);
        links->addWidget(useFor_);
        links->addStretch(1);
        boxRows->insertLayout(boxRows->indexOf(users_) + 1, links);
        // Show on: the guides (a guide map's layer colors them), the strands (a scalp map read at
        // each root), the scalp (the switch below, its channel beside it).
        auto* showOn = new QLabel(tr("Show on"), box_);
        showOn->setProperty("role", "hint");
        boxRows->addWidget(showOn);
        FlowLayout* chips = nullptr;
        boxRows->addWidget(chipRow(box_, &chips));
        const std::pair<const char*, QString> places[] = {{"guides", tr("Guides")}, {"strands", tr("Strands")}, {"scalp", tr("Scalp")}};
        for (const auto& [key, label] : places) {
            QToolButton* c = makeChip(label, key == std::string("guides")    ? tr("Color the guides by the map (a guide map)")
                                             : key == std::string("strands") ? tr("Color the strands by the map at their roots (a scalp map)")
                                                                              : tr("Show the map on the scalp (black to white)"),
                                      box_);
            c->setObjectName(QStringLiteral("paintShowOn_") + QLatin1String(key));
            chips->addWidget(c);
            showOn_[key] = c;
            const std::string where = key;
            connect(c, &QToolButton::clicked, this, [this, where](bool on) {
                const std::string picked = selectedMap();
                if (picked.empty()) return;
                if (where == "scalp") show_->setChecked(on);
                else emit showOnRequested(QString::fromStdString(where), picked, on);
                refreshShowOn();
            });
        }
    }
    useMap_ = new QPushButton(tr("Add Map Node"), box_);
    useMap_->setObjectName(QStringLiteral("mapAddNode"));
    useMap_->setToolTip(tr("A Map node reading this map in the graph: wire it into the setting it should drive"));
    useMap_->hide();
    connect(useMap_, &QPushButton::clicked, this, [this] {
        const std::string picked = selectedMap();
        if (!picked.empty()) emit mapNodeRequested(picked);
    });
    boxRows->addWidget(useMap_, 0, Qt::AlignLeft);
    // Show on scalp is the Scalp chip (its switch kept, hidden); a color map's channel the scalp
    // shows, last.
    showLabel->setText(tr("Scalp shows"));
    showLabel->setToolTip(qs(defs().get("choices/scalpChannel").value("tip", std::string())));
    show_->hide();
    boxRows->removeWidget(showBox);
    boxRows->addWidget(showBox);
    fitRows(list_);
    setEnabled(false);
}

void MapsPanel::setActions(const ui::Bindings& bindings, const ui::FillOptions& options) {
    bindings_ = bindings;
    options_ = options;
    options_.liveShortcuts = false;
    // + New Map and Import in the header (glyphs); a map's info and menu on its row.
    bar_->build(defs().get("lists/maps/bar"), bindings_, options_, ListBar::Part::Labeled);
    shownKey_.clear();
    refresh();
}

void MapsPanel::fillRowMenu(QMenu* menu) {
    // A map's menu (lists.json's maps context): Rename first, Convert to, the image, Delete last.
    ui::FillOptions options = options_;
    options.resolve.state = menuState();
    ui::fillMenu(menu, defs().get("lists/maps/context"), bindings_, options);
}

void MapsPanel::fillUseFor(QMenu* menu) {
    // Use for: the common settings the graph has, then every other mappable param (the core's
    // list, every host's: stroke.h settingTargetsJson); a param already reading it greyed.
    menu->clear();
    const std::string picked = selectedMap();
    if (!document_ || picked.empty()) return;
    nlohmann::json targets;
    try {
        targets = nlohmann::json::parse(settingTargetsJson(document_->graph().dump()));
    } catch (const std::exception&) {
        return;
    }
    std::set<std::pair<std::string, std::string>> used;
    for (const auto& u : document_->mapUsers(picked)) used.insert(u);
    auto add = [this, &used, &picked](QMenu* into, const nlohmann::json& t, const QString& text) {
        const std::string node = t.value("node", std::string()), param = t.value("param", std::string());
        QAction* a = into->addAction(text);
        a->setToolTip(qs(t.value("hint", std::string())));
        a->setEnabled(!node.empty() && !used.count({node, param}));
        connect(a, &QAction::triggered, this, [this, node, param, picked] { useMapFor(picked, node, param); });
    };
    for (const nlohmann::json& t : targets.value("settings", nlohmann::json::array())) {
        if (t.value("node", std::string()).empty()) continue;
        add(menu, t, QStringLiteral("%1  (%2.%3)").arg(qs(t.value("label", std::string())), qs(t.value("node", std::string())),
                                                       qs(t.value("param", std::string()))));
    }
    const nlohmann::json& others = targets.value("others", nlohmann::json::array());
    if (!others.empty()) {
        QMenu* sub = menu->addMenu(tr("Other"));
        sub->setToolTipsVisible(true);
        for (const nlohmann::json& t : others) add(sub, t, qs(t.value("id", std::string())));
    }
    if (menu->isEmpty()) menu->addAction(tr("The graph has no param a map can drive"))->setEnabled(false);
}

std::string MapsPanel::useMapFor(const std::string& map, const std::string& node, const std::string& param) {
    // node.param reads the map (the editor's paramPaint with the map by name: every host's link),
    // one undo step.
    if (!document_) return "no groom";
    ParamPaintRequest request;
    request.name = map;
    request.kind = selectedKind().empty() ? std::string("vertices") : selectedKind();
    const std::string error = document_->paintParam(node, param, request);
    if (!error.empty()) {
        emit message(tr("Cannot use %1 for %2.%3: %4").arg(qs(map), qs(node), qs(param), qs(error)));
        return error;
    }
    emit message(tr("%1.%2 reads %3 now").arg(qs(node), qs(param), qs(map)));
    emit edited();
    shownKey_.clear();
    refresh();
    return {};
}

void MapsPanel::refreshShowOn() {
    const std::string picked = selectedMap();
    const std::string kind = selectedKind();
    for (const auto& [key, chip] : showOn_) {
        const QSignalBlocker block(chip);
        const bool can = !picked.empty() && (key == "guides" ? kind == "guides" : key == "strands" ? kind != "guides" : true);
        chip->setEnabled(can);
        chip->setChecked(can && (key == "scalp" ? show_->isChecked() : showState_ && showState_(key, picked)));
    }
}

QWidget* MapsPanel::rowFor(QListWidgetItem* item, const QString& text, bool color, bool unused) {
    // A map's row: its name (a lost image marked), RGBA on a color map, then ⓘ (a dot: nothing reads
    // it yet) and the menu.
    const std::string name = item->data(Qt::UserRole).toString().toStdString();
    auto pick = [this, item] {
        if (list_->currentItem() != item) list_->setCurrentItem(item);
    };
    QToolButton* more = rowMenuButton(tr("Rename, convert, the image, delete"), [this](QMenu* menu) { fillRowMenu(menu); }, pick, list_);
    more->setObjectName(QStringLiteral("mapMenu_") + qs(name));
    auto* row = new ListRow(text, color ? QStringLiteral("RGBA") : QString(), {more}, list_);
    if (unused) row->setToolTip(tr("Nothing reads %1 yet: Use for picks a setting").arg(qs(name)));
    if (item->data(kMissingRole).isValid()) row->label()->setStyleSheet(QStringLiteral("color: %1;").arg(theme::warningText().name()));
    return row;
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
    QListWidgetItem* item = list_->itemAt(pos);
    if (!item) return listMenu(this, defs().get("lists/maps/bar"), bindings_, options_, {});  // (no map: New and Import)
    list_->setCurrentItem(item);
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("listContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    fillRowMenu(menu);
    return menu;
}

void MapsPanel::editSelectedName() {
    fresh();
    if (QListWidgetItem* item = list_->currentItem()) {
        // The name alone while it is edited (the row shows its kind beside it).
        updating_ = true;
        item->setText(item->data(Qt::UserRole).toString());
        updating_ = false;
        if (QWidget* row = list_->itemWidget(item)) row->hide();  // (the editor over the bare name)
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
    if (document_) {
        for (const PaintedMap& m : maps) key += document_->mapUsers(m.name).empty() ? "-" : "+";  // (the ⓘ's dot)
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
            // One line: the row widget shows the name (the item's own text empty, but while renamed).
            auto* item = new QListWidgetItem(QString(), list_);
            item->setSizeHint(QSize(0, 26));
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
            list_->setItemWidget(item, rowFor(item, (lost ? QStringLiteral("⚠ ") : QString()) + qs(m.name), color,
                                              document_ && document_->mapUsers(m.name).empty()));
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
            newFor_->setItemData(row, QString::fromUtf8(s.kind), kSettingKindRole);
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
    // A setting's map is named for it (<node>_<param>), gray, and lives on the scalp unless picked
    // otherwise (the regions: on the guides, a value a group of guides: PaintableSetting::kind).
    newName_->setEnabled(!forSetting);
    newColor_->setEnabled(!forSetting && newKind_->currentData().toString() != QStringLiteral("guides"));
    if (forSetting) {
        newColor_->setChecked(false);
        const QString settingKind = newFor_->currentData(kSettingKindRole).toString();
        const QString wanted = settingKind.isEmpty() ? QStringLiteral("vertices") : settingKind;
        if (wanted == QStringLiteral("guides") || newKind_->currentData().toString() == QStringLiteral("guides")) {
            newKind_->setCurrentIndex(std::max(0, newKind_->findData(wanted)));
        }
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
    infoBlock_->setVisible(!picked.empty());  // (the picked map's info, under the list: issue #138)
    colorRow_->setVisible(color);
    valueRow_->setVisible(!color);
    if (document_) {
        show_->setChecked(!picked.empty() && document_->scalpMapDisplay() == picked);
        channel_->setVisible(color);
        channel_->parentWidget()->setVisible(color);  // (the Scalp shows row)
        const int index = channel_->findData(qs(document_->scalpMapChannel()));
        channel_->setCurrentIndex(std::max(0, index));
        // What reads it (Maya's map_users).
        QStringList users;
        if (!picked.empty()) {
            for (const auto& [node, param] : document_->mapUsers(picked)) users << qs(node) + QLatin1Char('.') + qs(param);
        }
        users_->setText(!users.isEmpty() ? tr("Read by %1").arg(users.join(QStringLiteral(", ")))
                        : !picked.empty() ? tr("Not used by the graph yet: Use for picks a setting.")
                                          : QString());
        openReader_->setEnabled(!users.isEmpty());
        infoTitle_->setText(qs(picked));
        const QString kindTip = choiceTip("mapKind", selectedKind());
        kindLabel_->setText(picked.empty() ? QString()
                                           : kindTip.section(QLatin1Char('.'), 0, 0) + (color ? tr(", in color") : QString()));
        users_->setToolTip(users.isEmpty() && !picked.empty()
                               ? tr("+ New Map > For a setting makes a map a setting reads; or in the Graph Editor, right-click a param > "
                                    "Paint, or add a Map node.")
                               : QString());
        useMap_->hide();  // (Use for wires it)
    } else {
        users_->clear();
        useMap_->hide();
    }
    const bool has = !picked.empty();
    paint_->setEnabled(has);
    show_->setEnabled(has);
    valueRow_->setEnabled(has);
    colorRow_->setEnabled(has);
    useFor_->setEnabled(has);
    bar_->refresh();
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
    s.paintWholeFaces = wholeFaces_ && wholeFaces_->isChecked();
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
    // A scalp map shows on the scalp while it is painted (Maya's paint_map): a texture map as its
    // image, a vertex map per vertex.
    // (Not while a Layers node's eye holds the scalp: the map is painted, the view stays.)
    if ((map->kind == "texture" || map->kind == "vertices") && document_->scalpViewNode().empty() &&
        document_->scalpMapDisplay() != name) {
        showSelected(true);
        paintShown_ = name;
    }
    return true;
}

void MapsPanel::endPainting() {
    emit paintingEnded();
    const std::string shown = paintShown_;
    paintShown_.clear();
    if (!document_ || shown.empty() || document_->scalpMapDisplay() != shown) return;
    document_->setScalpMapDisplay({}, document_->scalpMapChannel());
    {
        const QSignalBlocker block(show_);
        show_->setChecked(false);
    }
    emit displayChanged();
}

void MapsPanel::showSelected(bool show) {
    if (!document_) return;
    paintShown_.clear();  // shown (or hidden) on purpose: painting ending leaves it
    const std::string name = selectedMap();
    const QString channel = channel_->currentData().toString();
    document_->setScalpMapDisplay(show ? name : std::string(), channel.isEmpty() ? std::string("rgb") : channel.toStdString());
    {
        const QSignalBlocker block(show_);
        show_->setChecked(show && !name.empty() && document_->scalpMapDisplay() == name);  // (a node's eye may hold it)
    }
    emit displayChanged();
}

}  // namespace groomlab::app
