// SPDX-License-Identifier: Apache-2.0
#include "correctives_panel.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "brush_session.h"
#include "glass.h"
#include "groom_document.h"
#include "groomlab/correctives.h"
#include "card_parts.h"
#include "list_bar.h"
#include "param_editors.h"
#include "theme.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr int kKindRole = Qt::UserRole;      // "layer", "node" or "base"
constexpr int kNameRole = Qt::UserRole + 1;  // the corrective's or the node's name

}  // namespace

CorrectivesPanel::CorrectivesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("correctivesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    const ui::json& lists = defs().get("lists/correctives");
    // The header: Layers, how many, ◆ Fix Frame and + New (glyphs); the about line in its tip.
    header_ = new ListHeader(tr("Correctives"), this);
    header_->setObjectName(QStringLiteral("correctivesHeader"));
    header_->setToolTip(QString::fromStdString(lists.value("about", std::string())).remove(QRegularExpression(QStringLiteral("<[^>]*>"))));
    bar_ = new ListBar(header_);
    bar_->setObjectName(QStringLiteral("correctivesBar"));
    header_->addGlyph(bar_);
    layout->addWidget(header_);

    // One line per row (the Simple layout's lists): the name, its keys (muted), then the icons:
    // the weight (a popup), Solo, the eye, the menu (a Sculpt node's row: its menu alone).
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("correctivesTree"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Layer"), tr("Keys"), QString()});
    plainRows(tree_);
    tree_->setRootIsDecorated(false);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    tree_->header()->resizeSection(2, 112);  // the row's icons: weight, solo, eye, menu
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->setToolTip(tr("Click: what brushes edit (again: the base groom). The weight, Solo, the eye and the menu (or right-click): "
                         "on the row. Double-click the name: rename"));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (updating_) return;
        justPicked_ = true;  // (itemClicked: this click picked it)
        picked(item);
    });
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        // The picked layer clicked again (the click didn't change the pick): the base groom.
        if (justPicked_) {
            justPicked_ = false;
            return;
        }
        if (!item || item->data(0, kKindRole).toString() == QLatin1String("base")) return;
        // (After the double-click time: a double-click renames it instead.)
        const int click = ++baseClick_;
        QTimer::singleShot(QApplication::doubleClickInterval(), this, [this, click] {
            if (click != baseClick_) return;
            setTargetNode({});
            setTargetLayer({});
        });
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) { ++baseClick_; });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (updating_ || !item || column != 0 || item->data(0, kKindRole).toString() != QLatin1String("layer")) return;
        const std::string name = item->data(0, kNameRole).toString().toStdString();
        const std::string text = item->text(0).trimmed().toStdString();
        // (Not inside the item's own signal: the stack is rebuilt. On / off: the row's eye.)
        QTimer::singleShot(0, this, [this, name, text] {
            if (!text.empty() && text != name && !renameLayer(name, text)) {
                shownKey_.clear();
                refresh();
            }
        });
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (QMenu* menu = contextMenu(pos)) menu->popup(tree_->viewport()->mapToGlobal(pos));
    });

    // A layer's weight (and its key) in a popup under the row's weight icon.
    weightPopup_ = new PopupPanel(this, 230);
    weightPopup_->setObjectName(QStringLiteral("correctiveWeightPopup"));
    box_ = weightPopup_;
    auto* row = new QHBoxLayout();
    row->setSpacing(4);
    weight_ = new ValueSlider(weightPopup_);
    weight_->setCaption(tr("Weight"));
    weight_->setRange(0.0, 1.0);
    weight_->setLimits(0.0, 1.0);
    weight_->setDecimals(2);
    weight_->setFill(theme::purple());
    weight_->setToolTip(tr("The corrective's weight on this frame. A keyed weight is keyed here as you change it"));
    row->addWidget(weight_, 1);
    key_ = makeGlyph(QStringLiteral("key"), qs(defs().action("corrective.keyWeight").value("tip", std::string())), weightPopup_);
    key_->setObjectName(QStringLiteral("correctiveKey"));
    row->addWidget(key_);
    weightPopup_->add(row);
    connect(weight_, &ValueSlider::released, this, [this] {
        if (updating_) return;
        setLayerWeight(currentLayer(), static_cast<float>(weight_->value()));
    });
    connect(key_, &QToolButton::clicked, this, [this] { keyWeight(currentLayer()); });

    emptyIcon_ = emptyIcon(QStringLiteral("layers"), this);
    emptyIcon_->hide();
    layout->addWidget(emptyIcon_, 0, Qt::AlignHCenter);
    hint_ = new QLabel(tr("Strokes edit the guides."), this);
    hint_->setWordWrap(true);
    hint_->setProperty("role", "hint");
    layout->addWidget(hint_);
    newButton_ = new QPushButton(this);
    newButton_->setObjectName(QStringLiteral("correctiveFirst"));
    newButton_->setProperty("role", "primary");
    newButton_->hide();
    connect(newButton_, &QPushButton::clicked, this, [this] {
        const auto it = bindings_.find("corrective.new");
        if (it != bindings_.end()) it->second.trigger(ui::json(), false);
    });
    layout->addWidget(newButton_, 0, Qt::AlignHCenter);
    // How the strands draw while sculpting (the window's controls: setSculptDisplay), folded under
    // the list.
    auto* display = new QWidget(this);
    displayRows_ = new QVBoxLayout(display);
    displayRows_->setContentsMargins(0, 0, 0, 0);
    displayRows_->setSpacing(4);
    displaySection_ = new Section(tr("Display while sculpting"), display, false, this);
    displaySection_->setObjectName(QStringLiteral("sculptDisplaySection"));
    displaySection_->hide();  // (until the window gives its controls)
    layout->addWidget(displaySection_);
    fitRows(tree_);
    setEnabled(false);
}

void CorrectivesPanel::setSculptDisplay(QComboBox* strands, ValueSlider* density) {
    auto* name = new QLabel(tr("Strands"), displaySection_->content());
    name->setProperty("role", "propLabel");
    strands->setToolTip(qs(defs().get("choices/sculptDisplay").value("tip", std::string("The strands while a stroke is live"))));
    name->setToolTip(strands->toolTip());
    displayRows_->addWidget(fieldRow(displaySection_->content(), name, strands));
    density->setParent(displaySection_->content());
    density->setCaption(tr("Strand density"));
    density->setToolTip(tr("The share of the strands evaluated while a stroke is live (all of them again when it ends)"));
    displayRows_->addWidget(density);
    strands->show();
    density->show();
    displaySection_->show();
}

QWidget* CorrectivesPanel::rowIcons(QTreeWidgetItem* item) {
    // A row's icons (ROW_ORDER): the weight (a popup), Solo, the eye, the menu; a Sculpt node's
    // row: its menu alone; the base groom: none.
    const QString kind = item->data(0, kKindRole).toString();
    const std::string name = item->data(0, kNameRole).toString().toStdString();
    auto pick = [this, item] {
        if (tree_->currentItem() != item) tree_->setCurrentItem(item);
        justPicked_ = false;
    };
    if (kind == QLatin1String("node")) {
        QToolButton* more = rowMenuButton(tr("Clear Edits"),
                                          [this, name](QMenu* menu) {
                                              ui::Bindings bindings = bindings_;
                                              bindings["sculpt.clearEdits"] = {[this, name](const ui::json&, bool) { clearNodeEdits(name); }};
                                              ui::FillOptions options = options_;
                                              options.resolve.state = std::set<std::string>{};
                                              ui::fillMenu(menu, defs().get("lists/correctives/nodeContext"), bindings, options);
                                          },
                                          pick, tree_);
        more->setObjectName(QStringLiteral("correctiveMenu_node_") + qs(name));
        return rowStrip({more}, tree_);
    }
    if (kind != QLatin1String("layer") || !stack_) return nullptr;
    const CorrectiveLayer* l = stack_->layer(name);
    if (!l) return nullptr;
    const double now = document_ ? document_->time() : 0.0;
    const float w = l->enabled ? l->weightAt(now) : 0.0f;
    QToolButton* weight = popupButton(QStringLiteral("weight"), tr("Weight %1 on this frame (click to change it, or key it)").arg(w, 0, 'f', 2),
                                      weightPopup_, pick, tree_, QString::number(w, 'f', 1));
    weight->setObjectName(QStringLiteral("correctiveWeight_") + qs(name));
    // Solo (lists.json correctives.row): on while it is the only layer on.
    bool solo = l->enabled;
    for (const CorrectiveLayer& other : stack_->layers) solo = solo && (other.name == name || !other.enabled);
    solo = solo && stack_->layers.size() > 1;
    std::vector<QWidget*> icons{weight};
    for (const ui::Item& entry : defs().resolve(defs().get("lists/correctives/row"))) {
        if (entry.kind != ui::Item::Kind::Action || !bindings_.count(entry.id)) continue;
        QToolButton* b = rowGlyph(QStringLiteral("glyph_solo"), qs(entry.tip), tree_, {}, solo);
        b->setObjectName(QStringLiteral("correctiveSolo_") + qs(name));
        const ui::Binding binding = bindings_.at(entry.id);
        connect(b, &QToolButton::clicked, this, [pick, binding] {
            pick();
            binding.trigger(ui::json(), false);
        });
        icons.push_back(b);
    }
    const bool on = l->enabled;
    QToolButton* eye = rowGlyph(on ? QStringLiteral("eye") : QStringLiteral("eye_off"),
                                on ? tr("Turn %1 off (it is kept)").arg(qs(name)) : tr("Turn %1 on").arg(qs(name)), tree_);
    eye->setObjectName(QStringLiteral("correctiveEye_") + qs(name));
    connect(eye, &QToolButton::clicked, this, [this, name, on] {
        QTimer::singleShot(0, this, [this, name, on] { setLayerEnabled(name, !on); });
    });
    icons.push_back(eye);
    QToolButton* more = rowMenuButton(tr("Rename, move, keys, delete"),
                                      [this](QMenu* menu) {
                                          ui::FillOptions options = options_;
                                          options.resolve.state = std::set<std::string>{"corrective"};
                                          ui::fillMenu(menu, defs().get("lists/correctives/context"), bindings_, options);
                                      },
                                      pick, tree_);
    more->setObjectName(QStringLiteral("correctiveMenu_") + qs(name));
    icons.push_back(more);
    return rowStrip(icons, tree_);
}

void CorrectivesPanel::setActions(const ui::Bindings& bindings, const ui::FillOptions& options) {
    bindings_ = bindings;
    options_ = options;
    options_.liveShortcuts = false;
    // Fix Frame and + New in the header (glyphs); a row's Solo and menu on the row.
    bar_->build(defs().get("lists/correctives/bar"), bindings_, options_, ListBar::Part::Labeled);
    shownKey_.clear();
    refresh();
}

void CorrectivesPanel::setDocument(GroomDocument* document) {
    document_ = document;
    target_.clear();
    stack_.reset();
    shownKey_.clear();
    setEnabled(document_ != nullptr);
    refresh();
}

void CorrectivesPanel::setSession(BrushSession* session) {
    session_ = session;
    applyTarget();
}

std::string CorrectivesPanel::currentLayer() const {
    if (stale_) const_cast<CorrectivesPanel*>(this)->refresh();
    QTreeWidgetItem* item = tree_->currentItem();
    return item && item->data(0, kKindRole).toString() == QLatin1String("layer") ? item->data(0, kNameRole).toString().toStdString()
                                                                                 : std::string();
}

std::vector<std::string> CorrectivesPanel::layerNames() const {
    std::vector<std::string> names;
    if (!document_) return names;
    try {
        for (const CorrectiveLayer& l : Correctives::fromJson(document_->storedText(StrokeField::Correctives)).layers) names.push_back(l.name);
    } catch (const std::exception&) {
    }
    return names;
}

void CorrectivesPanel::invalidate() {
    stale_ = true;
    if (!isVisible() || pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        if (stale_ && isVisible()) refresh();
    });
}

void CorrectivesPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (stale_) refresh();
}

void CorrectivesPanel::frameChanged() {
    applyTarget();  // a shot sculpt records at the frame shown
    if (isVisible() && !stale_) refreshWeights();
}

void CorrectivesPanel::refresh() {
    stale_ = false;
    stack_ = std::make_shared<Correctives>();
    std::string text;
    if (document_) {
        text = document_->storedText(StrokeField::Correctives);
        try {
            *stack_ = Correctives::fromJson(text);
        } catch (const std::exception&) {
        }
    }
    // A layer gone takes the target with it (a node gone took the document's).
    if (!target_.empty() && !stack_->layer(target_)) target_.clear();
    std::vector<std::pair<std::string, std::size_t>> nodes;
    if (document_) {
        for (const std::string& n : document_->sculptNodes()) nodes.emplace_back(n, document_->sculptedGuides(n));
    }
    std::string key = std::to_string(std::hash<std::string>{}(text)) + "#" + target_ + "#" + targetNode();
    for (const auto& [n, edited] : nodes) key += "|" + n + ":" + std::to_string(edited);
    if (key != shownKey_) {
        shownKey_ = key;
        ++rebuilds_;
        updating_ = true;
        tree_->clear();
        for (auto l = stack_->layers.rbegin(); l != stack_->layers.rend(); ++l) {  // the top of the stack first
            auto* item = new QTreeWidgetItem(tree_);
            item->setText(0, qs(l->name));
            item->setData(0, kKindRole, QStringLiteral("layer"));
            item->setData(0, kNameRole, qs(l->name));
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            if (!l->enabled) item->setForeground(0, theme::textMuted());  // (off: its eye says so too)
            QString keys;
            if (!l->weightCurve.empty()) keys = QStringLiteral("◆ %1").arg(l->weightCurve.size());
            else if (!l->allFrames() && document_) keys = tr("f%1").arg(std::lround(l->time * document_->framesPerSecond()));
            item->setText(1, keys);
            item->setToolTip(1, !l->weightCurve.empty() ? tr("Keyed on %1 frames").arg(l->weightCurve.size())
                                : l->allFrames()     ? tr("On every frame")
                                : document_ ? tr("Frame %1, fading over %2 frames").arg(std::lround(l->time * document_->framesPerSecond())).arg(std::lround(std::max(l->fadeIn, l->fadeOut) * document_->framesPerSecond()))
                                            : QString());
            QString tip = tr("%1: %n guide(s) corrected", "", static_cast<int>(l->offsets.size())).arg(qs(l->name));
            if (const std::optional<double> frame = layerFrame(l->name)) tip += tr("; strongest on frame %1").arg(*frame);
            item->setToolTip(0, tip);
        }
        // The Sculpt nodes: their guides as the graph makes them, brushed in the node.
        for (const auto& [n, edited] : nodes) {
            auto* item = new QTreeWidgetItem(tree_);
            item->setText(0, tr("Node: %1").arg(qs(n)));
            item->setText(1, edited ? tr("%1 edited").arg(edited) : QString());
            item->setData(0, kKindRole, QStringLiteral("node"));
            item->setData(0, kNameRole, qs(n));
            item->setToolTip(0, tr("Brushes shape %1's guides (the graph's, made again when what feeds it changes; the strokes follow them). "
                                   "Right-click: Clear Edits").arg(qs(n)));
        }
        // The base groom at the bottom: the stack's foundation.
        auto* base = new QTreeWidgetItem(tree_);
        base->setText(0, tr("Base groom"));
        base->setData(0, kKindRole, QStringLiteral("base"));
        base->setToolTip(0, tr("Brushes edit the guides themselves"));
        QFont font = base->font(0);
        font.setBold(true);
        base->setFont(0, font);
        for (QTreeWidgetItem* row : tree_->findItems(QStringLiteral("*"), Qt::MatchWildcard)) {
            row->setForeground(1, theme::textMuted());
            if (QWidget* icons = rowIcons(row)) tree_->setItemWidget(row, 2, icons);
        }
        updating_ = false;
        header_->setCount(static_cast<int>(stack_->layers.size()));
        fitRows(tree_);
    }
    selectTargetRow();
    refreshWeights();
    applyTarget();
    bar_->refresh();
}

void CorrectivesPanel::refreshWeights() {
    if (!stack_) return;
    const double now = document_ ? document_->time() : 0.0;
    const bool was = updating_;
    updating_ = true;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (item->data(0, kKindRole).toString() != QLatin1String("layer")) continue;
        const CorrectiveLayer* l = stack_->layer(item->data(0, kNameRole).toString().toStdString());
        if (!l) continue;
        const float w = l->enabled ? l->weightAt(now) : 0.0f;
        // The row's weight icon shows it (one decimal), its tip the exact one.
        if (auto* b = tree_->findChild<QToolButton*>(QStringLiteral("correctiveWeight_") + item->data(0, kNameRole).toString())) {
            b->setText(QString::number(w, 'f', 1));
            b->setToolTip(tr("Weight %1 on this frame (click to change it, or key it)").arg(w, 0, 'f', 2));
        }
    }
    const CorrectiveLayer* layer = target_.empty() ? nullptr : stack_->layer(target_);
    // The base groom has no weight (the popup is a corrective's).
    weight_->setEnabled(layer != nullptr);
    key_->setEnabled(layer != nullptr);
    if (layer) weight_->setValue(layer->weightCurve.empty() ? layer->weight : layer->weightAt(now));
    updating_ = was;
}

void CorrectivesPanel::selectTargetRow() {
    const std::string node = targetNode();
    QTreeWidgetItem* pick = nullptr;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        const QString kind = item->data(0, kKindRole).toString();
        const std::string name = item->data(0, kNameRole).toString().toStdString();
        if (!node.empty() ? kind == QLatin1String("node") && name == node
                          : !target_.empty() ? kind == QLatin1String("layer") && name == target_ : kind == QLatin1String("base")) {
            pick = item;
        }
    }
    const bool was = updating_;
    updating_ = true;
    tree_->setCurrentItem(pick);
    updating_ = was;
}

void CorrectivesPanel::picked(QTreeWidgetItem* item) {
    if (!document_ || !item) return;
    const QString kind = item->data(0, kKindRole).toString();
    const std::string name = item->data(0, kNameRole).toString().toStdString();
    if (kind == QLatin1String("node")) setTargetNode(name);
    else setTargetLayer(kind == QLatin1String("layer") ? name : std::string());
}

void CorrectivesPanel::setTargetLayer(const std::string& name) {
    const bool node = document_ && !document_->sculptTarget().empty();
    if (node) document_->setSculptTarget({});  // the base groom or a layer: no Sculpt node
    if (!node && name == target_) return;
    target_ = name;
    selectTargetRow();
    refreshWeights();
    applyTarget();
    emit targetChanged();
    if (node) emit edited();  // the view draws the guides again
}

std::string CorrectivesPanel::targetNode() const { return document_ ? document_->sculptTarget() : std::string(); }

void CorrectivesPanel::setTargetNode(const std::string& node) {
    if (!document_ || node == targetNode()) return;
    document_->setSculptTarget(node);
    if (!node.empty()) target_.clear();  // not a layer: the node's guides
    selectTargetRow();
    refreshWeights();
    applyTarget();
    emit targetChanged();
    emit edited();  // the view draws the node's guides (or the guides again)
}

bool CorrectivesPanel::clearNodeEdits(const std::string& given) {
    const std::string node = given.empty() ? targetNode() : given;
    if (!document_ || node.empty()) return false;
    const std::string error = document_->clearSculptEdits(node);
    if (!error.empty()) {
        emit message(tr("Cannot clear %1's edits: %2").arg(qs(node), qs(error)));
        return false;
    }
    refresh();
    emit edited();
    return true;
}

QMenu* CorrectivesPanel::contextMenu(const QPoint& pos) {
    if (stale_) refresh();
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (!item) return nullptr;
    const QString kind = item->data(0, kKindRole).toString();
    const std::string name = item->data(0, kNameRole).toString().toStdString();
    if (kind == QLatin1String("node")) {
        // A node's row: Clear Edits on that node (whatever the graph has selected).
        ui::Bindings bindings = bindings_;
        bindings["sculpt.clearEdits"] = {[this, name](const ui::json&, bool) { clearNodeEdits(name); }};
        return listMenu(this, defs().get("lists/correctives/nodeContext"), bindings, options_, {});
    }
    if (kind != QLatin1String("layer")) return nullptr;
    tree_->setCurrentItem(item);  // picked: what the menu acts on (and what brushes edit)
    return listMenu(this, defs().get("lists/correctives/context"), bindings_, options_, {"corrective"});
}

void CorrectivesPanel::editCurrentName() {
    if (QTreeWidgetItem* item = tree_->currentItem(); item && item->data(0, kKindRole).toString() == QLatin1String("layer")) {
        tree_->editItem(item, 0);
    }
}

void CorrectivesPanel::applyTarget() {
    const bool shot = document_ && !target_.empty();
    const std::string node = targetNode();
    hint_->setText(!node.empty() ? tr("Strokes shape the guides node %1 hands on; they are kept in the node and follow the guides when "
                                      "what feeds it changes.").arg(qs(node))
                   : shot ? tr("Strokes sculpt the groom as shown at frame %1 into the corrective %2.")
                                .arg(std::lround(document_->frame()))
                                .arg(qs(target_))
                          : stack_ && !stack_->layers.empty() ? tr("Strokes edit the guides. Pick a corrective to sculpt it at this frame.")
                                                                : tr("Correctives sculpt per-shot fixes on top of the groom, keyed over time."));
    if (session_) session_->setShotSculpt(shot, document_ ? document_->time() : 0.0, document_ ? std::lround(document_->frame()) : 0, target_);
    // No corrective yet: the first one, labeled.
    const bool first = document_ && (!stack_ || stack_->layers.empty());
    newButton_->setText(tr("New Corrective at Frame %1").arg(document_ ? std::lround(document_->frame()) : 0));
    newButton_->setVisible(first);
    // No corrective yet: a title, the line and the one button, centered under the base row (no disc:
    // the list has its base row).
    emptyIcon_->setVisible(false);
    hint_->setAlignment(first ? Qt::AlignHCenter : Qt::AlignLeft);
    if (first && !theme::glassy()) {
        hint_->setText(tr("<p style='margin:6px 0 4px 0; font-weight:600; font-size:13px; color:%1'>No correctives yet</p><p style='margin:0'>%2</p>")
                           .arg(theme::text().name(), hint_->text().toHtmlEscaped()));
    }
}

bool CorrectivesPanel::editLayers(const std::string& label, const std::function<void(Correctives&)>& edit) {
    if (!document_) return false;
    const std::string error = document_->editStoredText(label, StrokeField::Correctives, [&](std::string& text) {
        Correctives c = Correctives::fromJson(text);
        edit(c);
        text = c.layers.empty() ? std::string() : c.toJson();
    });
    if (!error.empty()) {
        emit message(tr("Cannot %1: %2").arg(qs(label), qs(error)));
        return false;
    }
    refresh();
    emit edited();
    return true;
}

bool CorrectivesPanel::addLayer(bool allFrames, double fadeFrames) {
    if (!document_) return false;
    const double seconds = document_->time();
    const long frame = std::lround(document_->frame());
    const double fade = std::max(0.0, fadeFrames) / document_->framesPerSecond();
    std::string made;
    const bool ok = editLayers("add corrective", [&](Correctives& c) {
        // Every host's (correctives.h addLayer): sculpt<N> or shot<F>, made free.
        made = c.addLayer(allFrames, seconds, static_cast<double>(frame), fade, fade);
    });
    if (ok) setTargetLayer(made);  // a new layer is what the next strokes sculpt into
    return ok;
}

bool CorrectivesPanel::removeLayer(const std::string& name) {
    if (name.empty()) return false;
    return editLayers("remove corrective", [&](Correctives& c) { c.removeLayer(name); });
}

bool CorrectivesPanel::setLayerWeight(const std::string& name, float weight) {
    if (name.empty() || !document_) return false;
    const double now = document_->time();
    return editLayers("corrective weight", [&](Correctives& c) { c.setLayerWeight(name, weight, now); });
}

bool CorrectivesPanel::setLayerEnabled(const std::string& name, bool enabled) {
    if (name.empty()) return false;
    return editLayers(enabled ? "corrective on" : "corrective off", [&](Correctives& c) { c.need(name).enabled = enabled; });
}

bool CorrectivesPanel::renameLayer(const std::string& name, const std::string& to) {
    if (name.empty() || to.empty() || name == to) return false;
    const bool retarget = target_ == name;  // the refresh after the edit drops a target that is gone
    // As every host renames it (correctives.h renameLayer): a taken or invalid name is refused,
    // with one to try.
    const bool ok = editLayers("rename corrective", [&](Correctives& c) { c.renameLayer(name, to); });
    if (ok && retarget) setTargetLayer(to);
    return ok;
}

bool CorrectivesPanel::moveLayer(const std::string& name, int step) {
    if (name.empty()) return false;
    return editLayers("move corrective", [&](Correctives& c) { c.moveLayer(name, step); });
}

bool CorrectivesPanel::soloLayer(const std::string& name) {
    if (name.empty()) return false;
    return editLayers("solo corrective", [&](Correctives& c) { c.soloLayer(name); });
}

bool CorrectivesPanel::keyWeight(const std::string& name) {
    if (name.empty() || !document_) {
        emit message(tr("Pick a corrective."));
        return false;
    }
    const double now = document_->time();
    const bool picked = name == target_;
    const float shown = static_cast<float>(weight_->value());
    return editLayers("key corrective weight", [&](Correctives& c) {
        c.keyLayerWeight(name, now, picked ? shown : c.need(name).weightAt(now));  // (correctives.h)
    });
}

bool CorrectivesPanel::clearKeys(const std::string& name) {
    if (name.empty() || !document_) return false;
    const double now = document_->time();
    return editLayers("clear corrective keys", [&](Correctives& c) { c.clearLayerKeys(name, now); });
}

std::optional<double> CorrectivesPanel::layerFrame(const std::string& name) const {
    if (!document_) return std::nullopt;
    const Correctives* c = stack_.get();
    Correctives read;
    if (!c || !c->layer(name)) {
        try {
            read = Correctives::fromJson(document_->storedText(StrokeField::Correctives));
        } catch (const std::exception&) {
            return std::nullopt;
        }
        c = &read;
    }
    const CorrectiveLayer* l = c->layer(name);
    if (!l) return std::nullopt;
    double seconds = 0.0;
    if (!l->weightCurve.empty()) {
        seconds = std::max_element(l->weightCurve.begin(), l->weightCurve.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    } else if (!l->allFrames()) {
        seconds = l->time;
    } else {
        return std::nullopt;
    }
    return std::round(secondsToFrame(seconds, document_->framesPerSecond()));
}

}  // namespace groomlab::app
