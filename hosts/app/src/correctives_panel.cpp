// SPDX-License-Identifier: Apache-2.0
#include "correctives_panel.h"

#include <algorithm>
#include <cmath>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
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
#include "theme.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr int kKindRole = Qt::UserRole;      // "layer", "node" or "base"
constexpr int kNameRole = Qt::UserRole + 1;  // the corrective's or the node's name

// A layer name not yet taken: base, base1, base2...
std::string uniqueLayerName(const Correctives& c, const std::string& base) {
    if (!c.layer(base)) return base;
    for (int k = 1;; ++k) {
        const std::string name = base + std::to_string(k);
        if (!c.layer(name)) return name;
    }
}

bool allFrames(const CorrectiveLayer& l) { return l.weightCurve.empty() && l.holdBefore && l.holdAfter; }

// The weight curve's value at a time put in (a key), the curve kept sorted.
void keyAt(CorrectiveLayer& l, double seconds, float w) {
    for (auto& [t, v] : l.weightCurve) {
        if (std::abs(t - seconds) < 1e-6) {
            v = w;
            return;
        }
    }
    l.weightCurve.emplace_back(seconds, w);
    std::sort(l.weightCurve.begin(), l.weightCurve.end());
}

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

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("correctivesTree"));
    tree_->setHeaderLabels({tr("Layer"), tr("Weight"), tr("Keys")});
    plainRows(tree_);
    tree_->setRootIsDecorated(false);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->setToolTip(tr("Click: what brushes edit. Checkbox: show or hide. Double-click the name: rename. Weight: now, on this "
                         "frame. Right-click: more"));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (!updating_) picked(item);
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (updating_ || !item || column != 0 || item->data(0, kKindRole).toString() != QLatin1String("layer")) return;
        const std::string name = item->data(0, kNameRole).toString().toStdString();
        const bool on = item->checkState(0) == Qt::Checked;
        const std::string text = item->text(0).trimmed().toStdString();
        // (Not inside the item's own signal: the stack is rebuilt.)
        QTimer::singleShot(0, this, [this, name, on, text] {
            if (stack_ && stack_->layer(name) && stack_->layer(name)->enabled != on) setLayerEnabled(name, on);
            if (!text.empty() && text != name && !renameLayer(name, text)) {
                shownKey_.clear();
                refresh();
            }
        });
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (QMenu* menu = contextMenu(pos)) menu->popup(tree_->viewport()->mapToGlobal(pos));
    });

    // The picked layer's box: up, down, solo, delete; its weight and the key.
    QVBoxLayout* rows = nullptr;
    box_ = itemBox(this, &rows);
    box_->setObjectName(QStringLiteral("layerBox"));
    itemBar_ = new ListBar(box_);
    itemBar_->setObjectName(QStringLiteral("layerItemBar"));
    rows->addWidget(itemBar_);
    auto* row = new QHBoxLayout();
    row->setSpacing(4);
    weight_ = new ValueSlider(box_);
    weight_->setCaption(tr("Weight"));
    weight_->setRange(0.0, 1.0);
    weight_->setLimits(0.0, 1.0);
    weight_->setDecimals(2);
    weight_->setFill(theme::purple());
    weight_->setToolTip(tr("The corrective's weight on this frame. A keyed weight is keyed here as you change it"));
    row->addWidget(weight_, 1);
    key_ = makeGlyph(QStringLiteral("key"), qs(defs().action("corrective.keyWeight").value("tip", std::string())), box_);
    key_->setObjectName(QStringLiteral("correctiveKey"));
    row->addWidget(key_);
    rows->addLayout(row);
    layout->addWidget(box_);
    connect(weight_, &ValueSlider::released, this, [this] {
        if (updating_) return;
        setLayerWeight(currentLayer(), static_cast<float>(weight_->value()));
    });
    connect(key_, &QToolButton::clicked, this, [this] { keyWeight(currentLayer()); });

    hint_ = new QLabel(tr("Strokes edit the guides."), this);
    hint_->setWordWrap(true);
    hint_->setProperty("role", "hint");
    layout->addWidget(hint_);
    fitRows(tree_);
    setEnabled(false);
}

void CorrectivesPanel::setActions(const ui::Bindings& bindings, const ui::FillOptions& options) {
    bindings_ = bindings;
    options_ = options;
    options_.liveShortcuts = false;
    // + New and Fix Frame in the header (glyphs), the picked layer's up, down, solo, delete in its box.
    bar_->build(defs().get("lists/correctives/bar"), bindings_, options_, ListBar::Part::Labeled);
    itemBar_->build(defs().get("lists/correctives/bar"), bindings_, options_, ListBar::Part::Glyphs);
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

std::string CorrectivesPanel::currentNode() const {
    if (stale_) const_cast<CorrectivesPanel*>(this)->refresh();
    QTreeWidgetItem* item = tree_->currentItem();
    return item && item->data(0, kKindRole).toString() == QLatin1String("node") ? item->data(0, kNameRole).toString().toStdString()
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
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
            item->setCheckState(0, l->enabled ? Qt::Checked : Qt::Unchecked);
            QString keys;
            if (!l->weightCurve.empty()) keys = QStringLiteral("◆ %1").arg(l->weightCurve.size());
            else if (allFrames(*l)) keys = tr("all frames");
            else if (document_) keys = tr("frame %1 ± %2").arg(std::lround(l->time * document_->framesPerSecond())).arg(std::lround(std::max(l->fadeIn, l->fadeOut) * document_->framesPerSecond()));
            item->setText(2, keys);
            QString tip = tr("%1: %n guide(s) corrected", "", static_cast<int>(l->offsets.size())).arg(qs(l->name));
            if (const std::optional<double> frame = layerFrame(l->name)) tip += tr("; strongest on frame %1").arg(*frame);
            item->setToolTip(0, tip);
        }
        // The Sculpt nodes: their guides as the graph makes them, brushed in the node.
        for (const auto& [n, edited] : nodes) {
            auto* item = new QTreeWidgetItem(tree_);
            item->setText(0, tr("Node: %1").arg(qs(n)));
            item->setText(2, edited ? tr("%1 edited").arg(edited) : QString());
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
            row->setForeground(2, theme::textMuted());
        }
        updating_ = false;
        header_->setCount(tree_->topLevelItemCount());
        fitRows(tree_);
    }
    selectTargetRow();
    refreshWeights();
    applyTarget();
    bar_->refresh();
    itemBar_->refresh();
    // The box is the picked row's (a corrective, the base groom or a Sculpt node).
    QTreeWidgetItem* picked = tree_->currentItem();
    itemBar_->setTitle(picked ? picked->text(0) : QString());
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
        item->setText(1, QStringLiteral("%1%").arg(std::lround(100.0f * w)));
    }
    const CorrectiveLayer* layer = target_.empty() ? nullptr : stack_->layer(target_);
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
                          : tr("Strokes edit the guides."));
    if (session_) session_->setShotSculpt(shot, document_ ? document_->time() : 0.0, document_ ? std::lround(document_->frame()) : 0, target_);
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
        CorrectiveLayer layer;
        layer.name = made = uniqueLayerName(c, allFrames ? "layer" : "shot" + std::to_string(frame));
        layer.time = seconds;
        layer.holdBefore = layer.holdAfter = allFrames;
        layer.fadeIn = layer.fadeOut = allFrames ? 0.5 : fade;
        c.layers.push_back(std::move(layer));
    });
    if (ok) setTargetLayer(made);  // a new layer is what the next strokes sculpt into
    return ok;
}

static auto findLayer(Correctives& c, const std::string& name) {
    const auto it = std::find_if(c.layers.begin(), c.layers.end(), [&](const CorrectiveLayer& l) { return l.name == name; });
    if (it == c.layers.end()) throw std::invalid_argument("no corrective named " + name);
    return it;
}

bool CorrectivesPanel::removeLayer(const std::string& name) {
    if (name.empty()) return false;
    return editLayers("remove corrective", [&](Correctives& c) { c.layers.erase(findLayer(c, name)); });
}

bool CorrectivesPanel::setLayerWeight(const std::string& name, float weight) {
    if (name.empty() || !document_) return false;
    const double now = document_->time();
    return editLayers("corrective weight", [&](Correctives& c) {
        CorrectiveLayer& l = *findLayer(c, name);
        const float w = std::clamp(weight, 0.0f, 1.0f);
        if (!l.weightCurve.empty()) keyAt(l, now, w);  // keyed: keyed here, as auto key does
        else l.weight = w;
    });
}

bool CorrectivesPanel::setLayerEnabled(const std::string& name, bool enabled) {
    if (name.empty()) return false;
    return editLayers(enabled ? "corrective on" : "corrective off", [&](Correctives& c) { findLayer(c, name)->enabled = enabled; });
}

bool CorrectivesPanel::renameLayer(const std::string& name, const std::string& to) {
    if (name.empty() || to.empty() || name == to) return false;
    const bool retarget = target_ == name;  // the refresh after the edit drops a target that is gone
    const bool ok = editLayers("rename corrective", [&](Correctives& c) {
        if (c.layer(to)) throw std::invalid_argument("a corrective named " + to + " exists");
        findLayer(c, name)->name = to;
    });
    if (ok && retarget) setTargetLayer(to);
    return ok;
}

bool CorrectivesPanel::moveLayer(const std::string& name, int step) {
    if (name.empty()) return false;
    return editLayers("move corrective", [&](Correctives& c) {
        const auto it = findLayer(c, name);
        const long i = static_cast<long>(it - c.layers.begin());
        const long j = std::clamp(i + step, 0L, static_cast<long>(c.layers.size()) - 1);
        if (i == j) return;
        CorrectiveLayer moved = std::move(*it);
        c.layers.erase(it);
        c.layers.insert(c.layers.begin() + j, std::move(moved));
    });
}

bool CorrectivesPanel::soloLayer(const std::string& name) {
    if (name.empty()) return false;
    return editLayers("solo corrective", [&](Correctives& c) {
        findLayer(c, name);
        const bool alone = std::all_of(c.layers.begin(), c.layers.end(), [&](const CorrectiveLayer& l) { return l.enabled == (l.name == name); });
        for (CorrectiveLayer& l : c.layers) l.enabled = alone || l.name == name;
    });
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
        CorrectiveLayer& l = *findLayer(c, name);
        const float w = picked ? shown : l.weightAt(now);
        if (l.weightCurve.empty() && !allFrames(l)) {
            // A fix's fades become keys first (Maya's Fix Frame keys them), so keying keeps its shape.
            if (!l.holdBefore) l.weightCurve.emplace_back(l.time - l.fadeIn, 0.0f);
            l.weightCurve.emplace_back(l.time, l.weight);
            if (!l.holdAfter) l.weightCurve.emplace_back(l.time + l.fadeOut, 0.0f);
        }
        keyAt(l, now, w);
        l.holdBefore = l.holdAfter = true;  // the curve holds past its ends
    });
}

bool CorrectivesPanel::clearKeys(const std::string& name) {
    if (name.empty() || !document_) return false;
    const double now = document_->time();
    return editLayers("clear corrective keys", [&](Correctives& c) {
        CorrectiveLayer& l = *findLayer(c, name);
        if (allFrames(l)) return;
        l.weight = l.weightAt(now);
        l.weightCurve.clear();
        l.holdBefore = l.holdAfter = true;
    });
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
    } else if (!allFrames(*l)) {
        seconds = l->time;
    } else {
        return std::nullopt;
    }
    return std::round(secondsToFrame(seconds, document_->framesPerSecond()));
}

}  // namespace groomlab::app
