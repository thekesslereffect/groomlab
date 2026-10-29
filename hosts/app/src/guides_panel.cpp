// SPDX-License-Identifier: Apache-2.0
#include "guides_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "groom_document.h"
#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/flow.h"
#include "groomlab/guide_selection.h"
#include "groomlab/parts.h"
#include "list_bar.h"
#include "theme.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

// A group name not yet taken: base, base1, base2...
std::string uniqueName(const GuideHierarchy& h, const std::string& base) {
    if (h.findGroup(base) == kNoGroup) return base;
    for (int k = 1;; ++k) {
        const std::string name = base + std::to_string(k);
        if (h.findGroup(name) == kNoGroup) return name;
    }
}

// A linear color as the screen shows it (sRGB).
QColor shownColor(const std::array<float, 3>& linear) {
    auto channel = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<int>(std::lround(s * 255.0f));
    };
    return QColor(channel(linear[0]), channel(linear[1]), channel(linear[2]));
}

// The swatch: the color the group draws in, filled; a dashed outline when it is the automatic one
// (Maya's dashed swatch: no color of its own).
QIcon swatchIcon(const QColor& color, bool own, qreal dpr) {
    const int size = 16;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(1.5, 1.5, size - 3.0, size - 3.0);
    if (own) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(r, 4.0, 4.0);
    } else {
        p.setBrush(color);
        QPen pen(theme::textDim(), 1.0, Qt::DashLine);
        p.setPen(pen);
        p.drawRoundedRect(r, 4.0, 4.0);
    }
    p.end();
    return QIcon(pixmap);
}

}  // namespace

GuidesPanel::GuidesPanel(QWidget* parent) : QWidget(parent) {
    // The rows in a scroll area: a short window scrolls them rather than squeezing the tree.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* host = new QWidget(scroll);
    scroll->setWidget(host);
    outer->addWidget(scroll);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    const ui::json& lists = defs().get("lists/groups");
    about_ = aboutLabel(lists.value("about", std::string()), host);
    layout->addWidget(about_);
    bar_ = new ListBar(host);
    bar_->setObjectName(QStringLiteral("groupsBar"));
    layout->addWidget(bar_);

    tree_ = new QTreeWidget(host);
    tree_->setObjectName(QStringLiteral("groupsTree"));
    tree_->setMinimumHeight(120);
    tree_->setHeaderLabels({tr("Group"), QString(), tr("Guides"), tr("Level")});
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c) tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->setToolTip(tr("Click: brushes and new guides use this group. Double-click the name: rename. Checkbox: show or hide. "
                         "Swatch: its color. Right-click: more"));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::itemChanged, this, &GuidesPanel::itemChanged);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        const std::string name = groupOf(item);
        if (name.empty()) return;
        emit groupClicked(name);
        // The swatch: its color picked (the window's Color..., never a modal dialog).
        if (column == 1) {
            const auto it = bindings_.find("group.color");
            if (it != bindings_.end() && it->second.trigger) it->second.trigger(nullptr, false);
        }
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        if (!updating_) refreshRows();
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (QMenu* menu = contextMenu(pos)) menu->popup(tree_->viewport()->mapToGlobal(pos));
    });

    selection_ = new QLabel(tr("No guides selected"), host);
    selection_->setProperty("role", "about");
    layout->addWidget(selection_);

    // Edits on many guides: one menu, the children count beside it.
    auto* row = new QHBoxLayout();
    row->setSpacing(6);
    const ui::json& edits = lists.at("edits");
    edits_ = new QPushButton(qs(edits.value("label", std::string())) + QStringLiteral("  ▾"), host);
    edits_->setObjectName(QStringLiteral("editGuides"));
    edits_->setToolTip(qs(edits.value("tip", std::string())));
    editsMenu_ = new QMenu(edits_);
    editsMenu_->setObjectName(QStringLiteral("editGuidesMenu"));
    edits_->setMenu(editsMenu_);
    row->addWidget(edits_, 1);
    count_ = new QSpinBox(host);
    count_->setObjectName(QStringLiteral("childrenCount"));
    count_->setRange(1, 500);
    count_->setValue(5);
    count_->setPrefix(tr("x "));
    count_->setToolTip(tr("Children per guide for Edit Guides > Children (the selected guides, else the picked group)"));
    row->addWidget(count_);
    layout->addLayout(row);

    // Segments: the selected guides (else the picked group's, else every guide) resampled to a
    // point count, one undo step; a whole group's new guides get it too.
    auto* segRow = new QHBoxLayout();
    segRow->setSpacing(6);
    auto* segLabel = new QLabel(tr("Segments"), host);
    segRow->addWidget(segLabel);
    segments_ = new QSpinBox(host);
    segments_->setRange(1, 256);
    segments_->setValue(12);
    segments_->setToolTip(tr("Points along the guides, kept on their smooth curve: the selected guides (their own count from "
                             "then on, kept when the group's changes; their group's count makes them follow it again), else the "
                             "picked group's (its new guides get as many too). Frozen guides keep theirs. More segments let a "
                             "guide curl tighter."));
    segRow->addWidget(segments_, 1);
    auto* resample = new QPushButton(tr("Resample"), host);
    resample->setObjectName(QStringLiteral("resampleGuides"));
    resample->setToolTip(segments_->toolTip());
    segRow->addWidget(resample);
    layout->addLayout(segRow);
    connect(resample, &QPushButton::clicked, this, [this] { resampleGuides(segments_->value()); });

    // The parts and flow the brushes laid (parts.h, flow.h): listed by name, removed, cleared.
    auto* overlays = new QGroupBox(tr("Parts and flow"), host);
    overlays_ = overlays;
    auto* overlaysLayout = new QGridLayout(overlays);
    parts_ = new QListWidget(overlays);
    parts_->setFixedHeight(72);
    parts_->setToolTip(tr("The parts the Part brush drew (a Part node parts the hair along them)"));
    overlaysLayout->addWidget(parts_, 0, 0, 1, 2);
    removePart_ = new QPushButton(tr("Remove part"), overlays);
    clearParts_ = new QPushButton(tr("Clear parts"), overlays);
    overlaysLayout->addWidget(removePart_, 1, 0);
    overlaysLayout->addWidget(clearParts_, 1, 1);
    flow_ = new QLabel(tr("No flow"), overlays);
    clearFlow_ = new QPushButton(tr("Clear flow"), overlays);
    clearFlow_->setToolTip(tr("Removes every flow arrow and sink the Flow brush laid"));
    overlaysLayout->addWidget(flow_, 2, 0);
    overlaysLayout->addWidget(clearFlow_, 2, 1);
    layout->addWidget(overlays);
    connect(removePart_, &QPushButton::clicked, this, [this] {
        if (parts_->currentItem()) removePart(parts_->currentItem()->text().toStdString());
    });
    connect(clearParts_, &QPushButton::clicked, this, [this] { clearParts(); });
    connect(clearFlow_, &QPushButton::clicked, this, [this] { clearFlow(); });
    setEnabled(false);
}

void GuidesPanel::setPartsShown(bool shown) { overlays_->setVisible(shown); }

void GuidesPanel::setActions(const ui::Bindings& bindings, const ui::FillOptions& options) {
    bindings_ = bindings;
    options_ = options;
    options_.liveShortcuts = false;
    bar_->build(defs().get("lists/groups/bar"), bindings_, options_);
    editsMenu_->clear();
    ui::FillOptions edits = options_;
    edits.resolve.state.reset();  // every entry; the bindings say what is enabled
    ui::fillMenu(editsMenu_, defs().get("lists/groups/edits"), bindings_, edits);
    refreshRows();
}

void GuidesPanel::setDocument(GroomDocument* document) {
    document_ = document;
    setEnabled(document_ != nullptr);
    shownKey_.clear();
    refresh();
}

std::string GuidesPanel::groupOf(QTreeWidgetItem* item) const {
    return item ? item->data(0, Qt::UserRole).toString().toStdString() : std::string();
}

void GuidesPanel::fresh() const {
    if (stale_) const_cast<GuidesPanel*>(this)->refresh();
}

std::string GuidesPanel::currentGroup() const {
    fresh();
    return groupOf(tree_->currentItem());
}

void GuidesPanel::setCurrentGroup(const std::string& name) {
    fresh();
    const QList<QTreeWidgetItem*> found = tree_->findItems(qs(name), Qt::MatchExactly | Qt::MatchRecursive, 0);
    for (QTreeWidgetItem* item : found) {
        if (groupOf(item) == name) {
            tree_->setCurrentItem(item);
            return;
        }
    }
}

std::set<std::string> GuidesPanel::menuState() const {
    std::set<std::string> state;
    const std::string group = currentGroup();
    if (group.empty()) return state;
    state.insert("group");
    if (document_ && document_->groupColors().count(group)) state.insert("group.color");
    return state;
}

QMenu* GuidesPanel::contextMenu(const QPoint& pos) {
    fresh();
    if (QTreeWidgetItem* item = tree_->itemAt(pos)) tree_->setCurrentItem(item);
    else tree_->setCurrentItem(nullptr);  // the empty space: what needs no group (New Group)
    return listMenu(this, defs().get("lists/groups/context"), bindings_, options_, menuState());
}

void GuidesPanel::editCurrentName() {
    fresh();
    if (QTreeWidgetItem* item = tree_->currentItem()) tree_->editItem(item, 0);
}

int GuidesPanel::childrenCount() const { return count_->value(); }
void GuidesPanel::setChildrenCount(int count) { count_->setValue(count); }

void GuidesPanel::invalidate() {
    stale_ = true;
    if (!isVisible() || pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        if (stale_ && isVisible()) refresh();
    });
}

void GuidesPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (stale_) refresh();
}

void GuidesPanel::refresh() {
    stale_ = false;
    // What the tree shows, as text: rebuilt only when it changed (a comb stroke changes nothing here).
    HierarchyPtr h = document_ ? document_->session().authoredHierarchy() : nullptr;
    std::string key = document_ ? std::to_string(reinterpret_cast<std::uintptr_t>(document_)) : std::string("-");
    std::map<GroupId, int> counts;
    if (h) {
        for (const HierarchyGuide& g : h->guides()) ++counts[g.group];
        for (const GuideGroup& g : h->groups()) {
            key += "|" + g.name + ":" + std::to_string(g.parent) + ":" + std::to_string(counts[g.id]) + (g.hasColor ? "c" : "");
        }
        key += "#" + document_->hiddenGroupsText() + "#" + document_->groupColorsText();
    }
    const DisplayTheme look = theme::displayTheme();
    // The automatic colors as the viewport draws them: a hue per group from the groom's own.
    const float seed = guideHueSeed(document_ ? document_->guideColorSeed() : std::string());
    key += "#" + std::to_string(look.version) + "#" + std::to_string(seed);
    if (key != shownKey_) {
        shownKey_ = key;
        ++rebuilds_;
        updating_ = true;
        const std::string current = groupOf(tree_->currentItem());
        tree_->clear();
        QTreeWidgetItem* select = nullptr;
        if (h) {
            const std::set<std::string>& hidden = document_->hiddenGroups();
            const std::map<std::string, std::string>& colors = document_->groupColors();
            std::map<GroupId, QTreeWidgetItem*> items;
            const std::map<GroupId, std::array<float, 3>> automatic = autoGuideColors(*h, seed);
            for (const GuideGroup& group : h->groups()) {  // parents before children
                const auto parent = items.find(group.parent);
                auto* item = parent == items.end() ? new QTreeWidgetItem(tree_) : new QTreeWidgetItem(parent->second);
                const int depth = h->groupDepth(group.id);
                item->setText(0, qs(group.name));
                item->setText(2, QString::number(counts[group.id]));
                item->setText(3, QString::number(depth));
                item->setData(0, Qt::UserRole, qs(group.name));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
                item->setCheckState(0, hidden.count(group.name) ? Qt::Unchecked : Qt::Checked);
                const auto own = colors.find(group.name);
                const auto shown = automatic.find(group.id);
                const QColor color = own != colors.end() ? QColor(qs(own->second))
                                     : group.hasColor    ? shownColor(group.color)
                                     : shownColor(shown != automatic.end() ? shown->second : std::array<float, 3>{0.5f, 0.5f, 0.5f});
                item->setIcon(1, swatchIcon(color, own != colors.end() || group.hasColor, devicePixelRatioF()));
                item->setToolTip(1, own != colors.end() ? tr("%1's color: click to pick, right-click > Default Color to clear").arg(qs(group.name))
                                                        : tr("%1's color is automatic (a hue of its own, from the groom's): click to pick one").arg(qs(group.name)));
                QString tip = tr("%1: checked shows it in the viewport; double-click to rename").arg(qs(group.name));
                QStringList users;
                for (const auto& [node, param] : document_->groupUsers(group.name)) users << QStringLiteral("%1 (%2)").arg(qs(node), qs(param));
                if (!users.isEmpty()) tip += tr("\nNamed in the graph by %1").arg(users.join(QStringLiteral(", ")));
                if (group.segments > 0) tip += tr("\nNew guides: %1 segments").arg(group.segments);
                item->setToolTip(0, tip);
                item->setExpanded(true);
                items[group.id] = item;
                if (group.name == current) select = item;
            }
        }
        if (select) tree_->setCurrentItem(select);
        else if (tree_->topLevelItemCount() > 0) tree_->setCurrentItem(tree_->topLevelItem(0));
        updating_ = false;
    }
    // The parts by name and the flow's count, as the brushes left them.
    parts_->clear();
    for (const std::string& name : partNames()) parts_->addItem(qs(name));
    removePart_->setEnabled(parts_->count() > 0);
    clearParts_->setEnabled(parts_->count() > 0);
    const auto [pins, sinks] = flowCount();
    flow_->setText(pins + sinks == 0 ? tr("No flow") : tr("%1 arrows, %2 sinks").arg(pins).arg(sinks));
    clearFlow_->setEnabled(pins + sinks > 0);
    refreshRows();
}

void GuidesPanel::refreshRows() {
    const std::size_t n = document_ ? document_->selection().size() : 0;
    selection_->setText(n == 0 ? tr("No guides selected") : tr("%n guide(s) selected", "", static_cast<int>(n)));
    bar_->refresh();
    ui::refreshActions(editsMenu_);
}

void GuidesPanel::itemChanged(QTreeWidgetItem* item, int column) {
    if (updating_ || !document_ || column != 0) return;
    const std::string name = groupOf(item);
    const std::string text = item->text(0).trimmed().toStdString();
    if (!text.empty() && text != name) {  // renamed in place
        if (!renameGroup(name, text)) {
            updating_ = true;
            item->setText(0, qs(name));
            updating_ = false;
        }
        return;
    }
    const bool hidden = item->checkState(0) == Qt::Unchecked;
    if (document_->hiddenGroups().count(name) != (hidden ? 1u : 0u)) {
        document_->setGroupHidden(name, hidden);
        emit displayChanged();
    }
}

bool GuidesPanel::edit(const char* what, const std::string& label, const std::function<void(GuideHierarchy&)>& change) {
    if (!document_) return false;
    const std::string error = document_->editGuides(label, change);
    if (!error.empty()) {
        emit message(tr("Cannot %1: %2").arg(QLatin1String(what), qs(error)));
        return false;
    }
    emit edited();
    return true;
}

bool GuidesPanel::addGroup(const std::string& parent, const std::string& name) {
    std::string made;
    const bool ok = edit("add a group", "add group", [&](GuideHierarchy& h) {
        GroupId parentId = kNoGroup;
        if (!parent.empty()) {
            parentId = h.findGroup(parent);
            if (parentId == kNoGroup) throw std::invalid_argument("no group named " + parent);
        }
        made = name.empty() ? uniqueName(h, parent.empty() ? "group" : parent + "_children") : name;
        if (h.findGroup(made) != kNoGroup) throw std::invalid_argument("a group named " + made + " is there already");
        h.addGroup(made, parentId);
    });
    if (ok) {
        refresh();
        setCurrentGroup(made);
    }
    return ok;
}

bool GuidesPanel::removeGroup(const std::string& name) {
    if (name.empty()) return false;
    return edit("remove the group", "remove group", [&](GuideHierarchy& h) {
        const GroupId id = h.findGroup(name);
        if (id == kNoGroup) throw std::invalid_argument("no group named " + name);
        h.removeGroup(id);
    });
}

bool GuidesPanel::renameGroup(const std::string& name, const std::string& to) {
    if (!document_ || name.empty() || to.empty() || name == to) return false;
    const std::string error = document_->renameGroup(name, to);
    if (!error.empty()) {
        emit message(tr("Cannot rename the group: %1").arg(qs(error)));
        return false;
    }
    // (Not from inside the item's own signal: the tree is rebuilt on the next turn.)
    QTimer::singleShot(0, this, [this, to] {
        refresh();
        setCurrentGroup(to);
    });
    stale_ = true;
    emit edited();
    return true;
}

bool GuidesPanel::generateChildren(const std::string& parent, int count) {
    if (!document_) return false;
    const std::vector<GuideId> selected = document_->selection();
    return edit("generate children", "generate children", [&](GuideHierarchy& h) {
        // The selected guides of the first one's group, else every guide of `parent` (the last
        // group when none is given, as Maya's).
        GroupId id = kNoGroup;
        std::vector<GuideId> parents;
        for (const GuideId g : selected) {
            const HierarchyGuide* guide = h.guide(g);
            if (!guide) continue;
            if (id == kNoGroup) id = guide->group;
            if (guide->group == id) parents.push_back(g);
        }
        if (id == kNoGroup) {
            if (!parent.empty()) id = h.findGroup(parent);
            else if (!h.groups().empty()) id = h.groups().back().id;
            if (id == kNoGroup) throw std::invalid_argument(parent.empty() ? "the groom has no guides yet" : "no group named " + parent);
            if (h.guidesInGroup(id).empty()) throw std::invalid_argument("the group " + h.group(id)->name + " has no guides to be parents");
        }
        ChildGenerationParams params;
        params.count = static_cast<std::size_t>(std::max(count, 1));
        // The group's existing child group when it has one, else a new one named after it.
        std::string childName;
        for (const GuideGroup& g : h.groups()) {
            if (g.parent == id) {
                childName = g.name;
                break;
            }
        }
        if (childName.empty()) childName = uniqueName(h, h.group(id)->name + "_children");
        h.generateChildren(id, childName, params, parents.empty() ? nullptr : &parents);
    });
}

bool GuidesPanel::deleteSelected() {
    if (!document_ || document_->selection().empty()) return false;
    const std::vector<GuideId> ids = document_->selection();
    const bool ok = edit("delete the guides", "delete guides", [&](GuideHierarchy& h) { h.removeGuides(ids); });
    if (ok) document_->setSelection({});
    return ok;
}

void GuidesPanel::selectGroup(const std::string& name) {
    if (!document_ || name.empty()) return;
    const HierarchyPtr h = document_->session().authoredHierarchy();
    if (!h) return;
    document_->setSelection(guidesAt(*h, -1, name));
    emit displayChanged();
}

void GuidesPanel::clearSelection() {
    if (!document_) return;
    document_->setSelection({});
    emit displayChanged();
}

bool GuidesPanel::mirrorSelection() {
    if (!document_) return false;
    const HierarchyPtr h = document_->session().authoredHierarchy();
    if (!h || document_->selection().empty()) {
        emit message(tr("No guides selected: select some first (the Select tool, or a group's Select Guides)"));
        return false;
    }
    const std::vector<GuideId> picked = groomlab::mirrorSelection(*h, document_->selection(), 0, 0.0f, 1e30f);
    document_->setSelection(picked);
    emit displayChanged();
    return !picked.empty();
}

// ---- Shape edits ------------------------------------------------------------------------------------

bool GuidesPanel::mirrorGroup(const std::string& name, bool fromPositive) {
    if (name.empty()) return false;
    return edit("mirror the group", "mirror group", [&](GuideHierarchy& h) {
        const GroupId id = h.findGroup(name);
        if (id == kNoGroup) throw std::invalid_argument("no group named " + name);
        if (h.groupDepth(id) != 0) throw std::invalid_argument(name + " is not a root group (mirror its root group: the children follow)");
        h.mirrorGroup(id, 0, 0.0f, fromPositive);
    });
}

bool GuidesPanel::symmetrySync(bool fromPositive) {
    return edit("sync the symmetry", "symmetry sync", [&](GuideHierarchy& h) {
        if (h.guides().empty()) throw std::invalid_argument("the groom has no guides");
        h.mirrorAll(0, 0.0f, fromPositive);
    });
}

bool GuidesPanel::promoteGroup(const std::string& name) {
    if (name.empty()) return false;
    return edit("promote the group", "promote group", [&](GuideHierarchy& h) {
        const GroupId id = h.findGroup(name);
        if (id == kNoGroup) throw std::invalid_argument("no group named " + name);
        if (h.groupDepth(id) == 0) throw std::invalid_argument(name + " is a root group already");
        h.promoteGroup(id);
    });
}

bool GuidesPanel::demoteGroup(const std::string& name, const std::string& under) {
    if (name.empty() || under.empty()) return false;
    return edit("demote the group", "demote group", [&](GuideHierarchy& h) {
        const GroupId id = h.findGroup(name), parent = h.findGroup(under);
        if (id == kNoGroup) throw std::invalid_argument("no group named " + name);
        if (parent == kNoGroup) throw std::invalid_argument("no group named " + under);
        h.demoteGroup(id, parent);
    });
}

bool GuidesPanel::reparentSelected(GuideId parent) {
    if (!document_) return false;
    const std::vector<GuideId> ids = document_->selection();
    if (ids.empty()) {
        emit message(tr("No guides selected: select the children to move first"));
        return false;
    }
    return edit("reparent the guides", "reparent guides", [&](GuideHierarchy& h) { h.reparent(ids, parent); });
}

bool GuidesPanel::resampleGuides(int segments) {
    if (!document_) return false;
    const std::vector<GuideId> selected = document_->selection();
    const std::string group = selected.empty() ? currentGroup() : std::string();
    segments = std::clamp(segments, 1, 256);
    // Maya's -resample: along the smooth curve through their points, frozen guides kept. Selected
    // guides get segments of their own; a whole group (with none picked: every group) keeps the
    // count for its new guides and leaves guides with their own as they are.
    return edit("resample the guides", "resample guides", [&](GuideHierarchy& h) {
        std::vector<GuideId> ids = selected;
        if (ids.empty()) {
            const GroupId gid = group.empty() ? kNoGroup : h.findGroup(group);
            for (const GuideGroup& g : h.groups()) {
                if (gid == kNoGroup || g.id == gid) h.setGroupSegments(g.id, segments);
            }
            if (gid != kNoGroup) {
                ids = h.guidesInGroup(gid);
            } else {
                for (const HierarchyGuide& g : h.guides()) ids.push_back(g.id);
            }
        }
        if (h.guides().empty()) throw std::invalid_argument("the groom has no guides to resample");
        h.resample(ids, segments, selected.empty());  // a group: guides with segments of their own keep them
    });
}

bool GuidesPanel::resetGroupSegments(const std::string& name) {
    int segments = 0;
    const bool done = edit("reset the group's segments", "reset segments", [&](GuideHierarchy& h) {
        const GroupId gid = h.findGroup(name);
        if (gid == kNoGroup) throw std::invalid_argument("no group named " + name);
        segments = h.resetGroupSegments(gid);
        if (segments < 1) throw std::invalid_argument("the group has no guides");
    });
    if (done) emit message(tr("Every guide of %1 at %2 segments (frozen ones kept)").arg(qs(name)).arg(segments));
    return done;
}

bool GuidesPanel::setGroupColor(const std::string& name, const std::string& color) {
    if (!document_) return false;
    const std::string error = document_->setGroupColor(name, color);
    if (!error.empty()) {
        emit message(tr("Cannot color the group: %1").arg(qs(error)));
        return false;
    }
    refresh();
    emit displayChanged();
    return true;
}

std::string GuidesPanel::rootOf(const std::string& name) const {
    const HierarchyPtr h = document_ ? document_->session().authoredHierarchy() : nullptr;
    if (!h) return {};
    GroupId id = h->findGroup(name);
    if (id == kNoGroup) return {};
    for (int guard = 0; guard < 64; ++guard) {
        const GuideGroup* g = h->group(id);
        if (!g || g->parent == kNoGroup) break;
        id = g->parent;
    }
    const GuideGroup* root = h->group(id);
    return root ? root->name : std::string();
}

std::vector<std::string> GuidesPanel::siblingsOf(const std::string& name) const {
    std::vector<std::string> out;
    const HierarchyPtr h = document_ ? document_->session().authoredHierarchy() : nullptr;
    if (!h) return out;
    const GroupId id = h->findGroup(name);
    const GuideGroup* group = id == kNoGroup ? nullptr : h->group(id);
    if (!group) return out;
    for (const GuideGroup& g : h->groups()) {
        if (g.parent == group->parent && g.id != id) out.push_back(g.name);
    }
    return out;
}

// ---- Parts and flow -------------------------------------------------------------------------------

std::vector<std::string> GuidesPanel::partNames() const {
    std::vector<std::string> names;
    if (!document_) return names;
    try {
        for (const Part& p : PartSet::fromJson(document_->storedText(StrokeField::Parts)).parts) names.push_back(p.name);
    } catch (const std::exception&) {
    }
    return names;
}

std::pair<std::size_t, std::size_t> GuidesPanel::flowCount() const {
    if (!document_) return {0, 0};
    try {
        const FlowSet flow = FlowSet::fromJson(document_->storedText(StrokeField::Flow));
        return {flow.pins.size(), flow.sinks.size()};
    } catch (const std::exception&) {
        return {0, 0};
    }
}

bool GuidesPanel::removePart(const std::string& name) {
    if (!document_ || name.empty()) return false;
    const std::string error = document_->editStoredText("remove part", StrokeField::Parts, [&](std::string& text) {
        PartSet parts = PartSet::fromJson(text);
        const auto it = std::find_if(parts.parts.begin(), parts.parts.end(), [&](const Part& p) { return p.name == name; });
        if (it == parts.parts.end()) throw std::invalid_argument("no part named " + name);
        parts.parts.erase(it);
        text = parts.empty() ? std::string() : parts.toJson();
    });
    if (!error.empty()) {
        emit message(tr("Cannot remove the part: %1").arg(qs(error)));
        return false;
    }
    emit edited();
    return true;
}

bool GuidesPanel::clearParts() {
    if (!document_) return false;
    const std::string error = document_->editStoredText("clear parts", StrokeField::Parts, [](std::string& text) { text.clear(); });
    if (!error.empty()) {
        emit message(tr("Cannot clear the parts: %1").arg(qs(error)));
        return false;
    }
    emit edited();
    return true;
}

bool GuidesPanel::clearFlow() {
    if (!document_) return false;
    const std::string error = document_->editStoredText("clear flow", StrokeField::Flow, [](std::string& text) { text.clear(); });
    if (!error.empty()) {
        emit message(tr("Cannot clear the flow: %1").arg(qs(error)));
        return false;
    }
    emit edited();
    return true;
}

}  // namespace groomlab::app
