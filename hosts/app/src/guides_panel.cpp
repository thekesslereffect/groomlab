// SPDX-License-Identifier: Apache-2.0
#include "guides_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "groom_document.h"
#include "groomlab/editor/guide_edits.h"
#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/flow.h"
#include "groomlab/brush.h"
#include "groomlab/guide_selection.h"
#include "groomlab/parts.h"
#include "card_parts.h"
#include "glass.h"
#include "list_bar.h"
#include "theme.h"
#include "groomlab/color.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

// A group name not yet taken: base, base2, base3... (every host's rule, GuideHierarchy::freeGroupName).
std::string uniqueName(const GuideHierarchy& h, const std::string& base) { return h.freeGroupName(base); }

// A linear color as the screen shows it (sRGB).
QColor shownColor(const std::array<float, 3>& linear) {
    auto channel = [](float c) {
        const float s = groomlab::linearToSrgb01(c);  // (color.h)
        return static_cast<int>(std::lround(s * 255.0f));
    };
    return QColor(channel(linear[0]), channel(linear[1]), channel(linear[2]));
}

// The swatch: the color the group draws in, filled; a dashed outline when it is the automatic one
// (Maya's dashed swatch: no color of its own).
QIcon swatchIcon(const QColor& color, bool own, qreal dpr) {
    const int size = 12;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(1.5, 1.5, size - 3.0, size - 3.0);
    if (own) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(r);
    } else {
        p.setBrush(color);
        QPen pen(theme::textDim(), 1.0, Qt::DotLine);
        p.setPen(pen);
        p.drawEllipse(r);
    }
    p.end();
    return QIcon(pixmap);
}

}  // namespace

GuidesPanel::GuidesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("guidesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    const ui::json& lists = defs().get("lists/groups");
    // The header: Guide groups (palette.sections' label), how many, Isolate (the Groom Tools'),
    // + New Group.
    QString title = tr("Guide groups");
    for (const ui::json& s : defs().get("palette/sections")) {
        if (s.value("id", std::string()) == "groups") title = qs(s.value("label", title.toStdString()));
    }
    header_ = new ListHeader(title, this);
    header_->setObjectName(QStringLiteral("groupsHeader"));
    header_->setToolTip(QString::fromStdString(lists.value("about", std::string())).remove(QRegularExpression(QStringLiteral("<[^>]*>"))));
    bar_ = new ListBar(header_);
    bar_->setObjectName(QStringLiteral("groupsBar"));
    header_->addGlyph(bar_);
    layout->addWidget(header_);

    // The groups as plain rows: the color dot and name, the guides it holds, the eye.
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("groupsTree"));
    tree_->setColumnCount(3);
    plainRows(tree_);
    tree_->setIndentation(12);
    tree_->setRootIsDecorated(false);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    tree_->header()->resizeSection(2, 24);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->setToolTip(tr("Click: brushes and new guides use this group. Double-click the name: rename. Eye: show or hide. "
                         "Right-click: more (its color...)"));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_);
    empty_ = new QLabel(tr("No editable guides yet: Place or Draw starts the first group."), this);
    empty_->setObjectName(QStringLiteral("groupsEmpty"));
    empty_->setProperty("role", "hint");
    empty_->setWordWrap(true);
    empty_->setAlignment(Qt::AlignHCenter);  // the shared empty state: icon, line, one button, centered
    emptyIcon_ = emptyIcon(QStringLiteral("spline"), this);
    emptyIcon_->hide();
    layout->addWidget(emptyIcon_, 0, Qt::AlignHCenter);
    layout->addWidget(empty_);
    bake_ = new QPushButton(tr("Bake to Guides"), this);
    bake_->setObjectName(QStringLiteral("groupsBake"));
    bake_->setToolTip(tr("Turn the guides the graph makes into a group you can brush"));
    bake_->hide();
    layout->addWidget(bake_, 0, Qt::AlignHCenter);
    connect(bake_, &QPushButton::clicked, this, [this] {
        const std::string node = graphGuidesNode();
        if (!node.empty()) emit bakeRequested(node);
    });
    connect(tree_, &QTreeWidget::itemChanged, this, &GuidesPanel::itemChanged);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        const std::string name = groupOf(item);
        if (name.empty()) return;
        // The eye: shown or hidden (not a pick).
        if (column == 2) {
            if (!document_) return;
            document_->setGroupHidden(name, !document_->hiddenGroups().count(name));
            emit displayChanged();
            return;
        }
        emit groupClicked(name);
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        if (!updating_) refreshRows();
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (QMenu* menu = contextMenu(pos)) menu->popup(tree_->viewport()->mapToGlobal(pos));
    });

    // The picked group's box: rename, select, delete; Segments; Edit Guides ▾ and its children count.
    QVBoxLayout* rows = nullptr;
    box_ = itemBox(this, &rows);
    box_->setObjectName(QStringLiteral("groupBox"));
    itemBar_ = new ListBar(box_);
    itemBar_->setObjectName(QStringLiteral("groupItemBar"));
    rows->addWidget(itemBar_);
    selection_ = new QLabel(tr("No guides selected"), box_);
    selection_->setProperty("role", "hint");
    selection_->hide();  // (the viewport's status says it)

    // Segments: the selected guides (else the picked group's, else every guide) resampled to a
    // point count, one undo step; a whole group's new guides get it too.
    auto* segRow = new QHBoxLayout();
    segRow->setSpacing(4);
    segments_ = new ValueSlider(box_);
    segments_->setObjectName(QStringLiteral("groupSegments"));
    segments_->setCaption(tr("Segments"));
    segments_->setRange(2.0, 64.0);
    segments_->setLimits(1.0, 256.0);
    segments_->setInteger(true);
    segments_->setDecimals(0);
    segments_->setValue(12.0);
    segments_->setFill(theme::sliderFill(theme::blue()));
    segments_->setToolTip(tr("Points along the guides, kept on their smooth curve: the selected guides (their own count from "
                             "then on, kept when the group's changes; their group's count makes them follow it again), else the "
                             "picked group's (its new guides get as many too). Frozen guides keep theirs. More segments let a "
                             "guide curl tighter. Resample applies it."));
    segRow->addWidget(segments_, 1);
    auto* resample = new QPushButton(tr("Resample"), box_);
    resample->setObjectName(QStringLiteral("resampleGuides"));
    resample->setToolTip(segments_->toolTip());
    segRow->addWidget(resample);
    rows->addLayout(segRow);
    connect(resample, &QPushButton::clicked, this, [this] { resampleGuides(static_cast<int>(std::lround(segments_->value()))); });
    // Segments by Length (issue #98): toggled, the targets resampled at the slider's count by their
    // lengths (on) or to it (off), one undo step.
    byLength_ = new QCheckBox(qs(defs().get("palette/texts/segmentsByLength").get<std::string>()), box_);
    byLength_->setObjectName(QStringLiteral("segmentsByLength"));
    byLength_->setToolTip(qs(defs().get("palette/texts/segmentsByLengthTip").get<std::string>()));
    rows->addWidget(switchRow(box_, byLength_));
    connect(byLength_, &QCheckBox::toggled, this, [this](bool on) {
        resampleGuides(static_cast<int>(std::lround(segments_->value())), on ? 1 : 0);
    });

    // Edits on many guides: one menu, the children count beside it.
    auto* row = new QHBoxLayout();
    row->setSpacing(4);
    const ui::json& edits = lists.at("edits");
    edits_ = new QPushButton(qs(edits.value("label", std::string())) + QStringLiteral("  ▾"), box_);
    edits_->setObjectName(QStringLiteral("editGuides"));
    edits_->setProperty("role", "disclosure");  // opens a menu: outlined, the row's width
    edits_->setToolTip(qs(edits.value("tip", std::string())));
    editsMenu_ = new QMenu(edits_);
    editsMenu_->setObjectName(QStringLiteral("editGuidesMenu"));
    edits_->setMenu(editsMenu_);
    row->addWidget(edits_, 1);
    rows->addLayout(row);
    // Edit Guides > Children's count, on a line of its own (beside the menu it would widen a
    // narrow dock).
    count_ = new ValueSlider(box_);
    count_->setObjectName(QStringLiteral("childrenCount"));
    count_->setCaption(tr("Children per guide"));
    count_->setInteger(true);
    count_->setRange(1, 50);
    count_->setLimits(1, 500);
    count_->setValue(5);
    count_->setFormat([](double v) { return QString::number(std::lround(v)); });
    count_->setToolTip(tr("Children per guide for Edit Guides > Children (the selected guides, else the picked group)"));
    rows->addWidget(count_);
    layout->addWidget(box_);

    // The parts and flow the brushes laid (parts.h, flow.h): listed by name, removed, cleared
    // (off in the Groom Tools: the Part and Flow tools' cards hold them).
    auto* overlays = new QGroupBox(tr("Parts and flow"), this);
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
    fitRows(tree_);
    setEnabled(false);
}

void GuidesPanel::setPartsShown(bool shown) { overlays_->setVisible(shown); }

void GuidesPanel::setActions(const ui::Bindings& bindings, const ui::FillOptions& options) {
    bindings_ = bindings;
    options_ = options;
    options_.liveShortcuts = false;
    // + New Group in the header (a + glyph), the picked group's glyphs in its box.
    bar_->build(defs().get("lists/groups/bar"), bindings_, options_, ListBar::Part::Labeled);
    itemBar_->build(defs().get("lists/groups/bar"), bindings_, options_, ListBar::Part::Glyphs);
    editsMenu_->clear();
    ui::FillOptions edits = options_;
    edits.resolve.state.reset();  // every entry; the bindings say what is enabled
    // Move to Group and Duplicate to Group: the groups beside the selected guides' (filled again as
    // the menu opens).
    edits.resolve.generators["guideGroups"] = [this](const ui::json& entry) {
        ui::json out = ui::json::array();
        for (const std::string& name : groupsBesideSelection()) {
            out.push_back({{"id", entry.value("action", std::string())}, {"value", name}, {"label", name}});
        }
        return out;
    };
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

int GuidesPanel::childrenCount() const { return static_cast<int>(std::lround(count_->value())); }

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
                const bool off = hidden.count(group.name) > 0;
                item->setText(0, qs(group.name));
                item->setText(1, QString::number(counts[group.id]));
                item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                item->setForeground(1, theme::textDim());  // the count: readable on the selection tone too
                if (off) item->setForeground(0, theme::textMuted());
                item->setIcon(2, lineIcon(off ? QStringLiteral("eye_off") : QStringLiteral("eye"), devicePixelRatioF(), 14));
                item->setToolTip(2, off ? tr("%1 is hidden: click to show it").arg(qs(group.name)) : tr("Hide %1 in the viewport").arg(qs(group.name)));
                item->setData(0, Qt::UserRole, qs(group.name));
                item->setFlags(item->flags() | Qt::ItemIsEditable);
                const auto own = colors.find(group.name);
                const auto shown = automatic.find(group.id);
                const QColor color = own != colors.end() ? QColor(qs(own->second))
                                     : group.hasColor    ? shownColor(group.color)
                                     : shownColor(shown != automatic.end() ? shown->second : std::array<float, 3>{0.5f, 0.5f, 0.5f});
                item->setIcon(0, swatchIcon(color, own != colors.end() || group.hasColor, devicePixelRatioF()));
                QString tip = tr("%1: click to brush it and place into it; double-click to rename").arg(qs(group.name));
                tip += own != colors.end() ? tr("\nIts own color (right-click > Default Color to clear)")
                                           : tr("\nAn automatic color (right-click > Color... to pick one)");
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
        header_->setCount(document_ ? (h ? static_cast<int>(h->groups().size()) : 0) : -1);
        fitRows(tree_);
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
    itemBar_->refresh();
    // The box is the picked group's (none picked: every group's, Segments on every guide).
    const std::string group = currentGroup();
    // (A picked group: the row above names it, the drawer's bar holds its actions alone.)
    itemBar_->setMutedTitle(true);
    itemBar_->setTitle(group.empty() ? tr("All groups") : tr("%1 segments per guide").arg(std::lround(segments_->value())));
    // Segments by Length: on where the picked group goes by length (a selection keeps the switch).
    if (byLength_ && document_ && n == 0 && !group.empty()) {
        const HierarchyPtr authored = document_->session().authoredHierarchy();
        const GuideGroup* g = authored ? authored->group(authored->findGroup(group)) : nullptr;
        const QSignalBlocker block(byLength_);
        byLength_->setChecked(g && g->segmentLength > 0.0f);
    }
    box_->setVisible(tree_->topLevelItemCount() > 0);
    tree_->setVisible(tree_->topLevelItemCount() > 0);
    // No group of its own: say where the guides shown come from, and how to brush them.
    const bool none = document_ && tree_->topLevelItemCount() == 0;
    const std::string source = none ? graphGuidesNode() : std::string();
    const auto& viewed = none ? document_->lastView().hierarchy : nullptr;
    const auto& held = none ? document_->file().data.guides : nullptr;
    const std::size_t made = viewed ? viewed->guides().size() : held ? held->guides().size() : 0;
    const QString body = !source.empty() && made > 0
                             ? tr("The %1 guides shown can't be brushed yet. Bake them into a group, or Place or Draw a new one.").arg(made)
                         : made > 0 ? tr("The %1 guides shown can't be brushed here. Place or Draw starts a group you can brush.").arg(made)
                                    : tr("Place or Draw starts the first group.");
    // The shared empty state: a title, then the line.
    empty_->setText(theme::glassy() ? body
                                    : tr("<p style='margin:0 0 4px 0; font-weight:600; font-size:13px; color:%1'>No guide groups yet</p><p style='margin:0'>%2</p>")
                                          .arg(theme::text().name(), body.toHtmlEscaped()));
    empty_->setVisible(none);
    emptyIcon_->setVisible(none && !theme::glassy());
    bake_->setVisible(!source.empty() && made > 0);
    ui::refreshActions(editsMenu_);
}

std::string GuidesPanel::graphGuidesNode() const {
    if (!document_) return {};
    std::string found;
    for (const GroomDocument::NodeInfo& n : document_->nodes()) {
        const nlohmann::json* spec = document_->spec(n.type);
        if (!spec || !spec->contains("outputs")) continue;
        for (const nlohmann::json& port : (*spec)["outputs"]) {
            if (port.is_object() && QString::fromStdString(port.value("type", std::string())).compare(QStringLiteral("guides"), Qt::CaseInsensitive) == 0) {
                found = n.name;
            }
        }
    }
    return found;
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

bool GuidesPanel::guideOp(const char* what, const GuideOp& op, GuideOpResult* done) {
    // Every host's Edit Guides op (the editor's applyGuideOp, guide_edits.h): its checks and edit.
    GuideOpResult result;
    const bool ok = edit(what, guideOpLabel(op), [&](GuideHierarchy& h) { result = applyGuideOp(h, op); });
    if (ok && done) *done = std::move(result);
    return ok;
}

bool GuidesPanel::addGroup(const std::string& parent, const std::string& name) {
    GuideOp op;
    op.op = "add_group";
    op.parent = parent;
    op.name = name;
    GuideOpResult done;
    const bool ok = guideOp("add a group", op, &done);
    if (ok) {
        refresh();
        setCurrentGroup(done.group);
    }
    return ok;
}

bool GuidesPanel::removeGroup(const std::string& name) {
    if (name.empty()) return false;
    GuideOp op;
    op.op = "remove_group";
    op.group = name;
    return guideOp("remove the group", op);
}

bool GuidesPanel::renameGroup(const std::string& name, const std::string& typed) {
    const std::string to = guideGroupName(typed);  // one word: spaces as '_'
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
    // Every host's Generate Children (the editor's, guide_edits.h): the selected guides, all of one
    // group, else every guide of `parent` (the last group when none is given).
    ChildrenRequest request;
    request.parents = document_->selection();
    if (request.parents.empty()) request.group = parent;
    request.params.count = std::clamp<std::size_t>(static_cast<std::size_t>(std::max(count, 1)), 1, kMaxChildrenPerGuide);
    return edit("generate children", "generate children", [&](GuideHierarchy& h) { groomlab::generateChildren(h, request); });
}

bool GuidesPanel::deleteSelected() {
    if (!document_ || document_->selection().empty()) return false;
    GuideOp op;
    op.op = "delete_guides";
    op.guides = document_->selection();
    const bool ok = guideOp("delete the guides", op);
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

bool GuidesPanel::mirrorToOtherSide(const BrushSettings& brush) {
    if (!document_) return false;
    if (document_->selection().empty()) {
        emit message(tr("No guides selected: select the guides to mirror to the other side"));
        return false;
    }
    BrushSettings b = brush;
    if (!b.mirrorTopology && (b.mirrorAxis < 0 || b.mirrorAxis > 2)) b.mirrorAxis = 0, b.mirrorCenter = 0.0f;  // off: World X
    const MeshPtr scalp = document_->session().scalp();
    GuideOp op;
    op.op = "mirror_shapes";
    op.guides = document_->selection();
    op.symmetry = StrokeSymmetry::of(b, scalp.get());
    GuideOpResult done;
    const bool ok = guideOp("mirror to the other side", op, &done);
    if (ok) {
        emit message(tr("Mirrored to the other side: %1 matched, %2 added").arg(done.mirrored.reshaped.size()).arg(done.mirrored.created.size()));
    }
    return ok;
}

// ---- Shape edits ------------------------------------------------------------------------------------

bool GuidesPanel::mirrorGroup(const std::string& name, bool fromPositive) {
    if (name.empty()) return false;
    GuideOp op;
    op.op = "mirror_group";
    op.group = name;
    op.fromPositive = fromPositive;
    return guideOp("mirror the group", op);
}

bool GuidesPanel::symmetrySync(bool fromPositive) {
    GuideOp op;
    op.op = "mirror_all";
    op.fromPositive = fromPositive;
    return guideOp("sync the symmetry", op);
}

bool GuidesPanel::promoteGroup(const std::string& name) {
    if (name.empty()) return false;
    GuideOp op;
    op.op = "promote_group";
    op.group = name;
    return guideOp("promote the group", op);
}

bool GuidesPanel::demoteGroup(const std::string& name, const std::string& under) {
    if (name.empty() || under.empty()) return false;
    GuideOp op;
    op.op = "demote_group";
    op.group = name;
    op.parent = under;
    return guideOp("demote the group", op);
}

bool GuidesPanel::reparentSelected(GuideId parent) {
    if (!document_) return false;
    if (document_->selection().empty()) {
        emit message(tr("No guides selected: select the children to move first"));
        return false;
    }
    GuideOp op;
    op.op = "reparent";
    op.guides = document_->selection();
    op.parentGuide = parent;
    return guideOp("reparent the guides", op);
}

std::vector<std::string> GuidesPanel::groupsBesideSelection() const {
    std::vector<std::string> names;
    const HierarchyPtr h = document_ ? document_->session().authoredHierarchy() : nullptr;
    if (!h) return names;
    for (const GroupId g : h->groupsBeside(document_->selection())) names.push_back(h->group(g)->name);
    return names;
}

bool GuidesPanel::moveSelectedToGroup(const std::string& group, bool duplicate) {
    if (!document_) return false;
    if (document_->selection().empty()) {
        emit message(tr("No guides selected"));
        return false;
    }
    GuideOp op;
    op.op = duplicate ? "duplicate_to_group" : "move_to_group";
    op.guides = document_->selection();
    op.group = group;
    return guideOp(duplicate ? "duplicate the guides" : "move the guides", op);
}

bool GuidesPanel::newGroupFromSelection() {
    if (!document_) return false;
    if (document_->selection().empty()) {
        emit message(tr("No guides selected"));
        return false;
    }
    GuideOp op;
    op.op = "new_group_from";
    op.guides = document_->selection();
    return guideOp("make a group of the guides", op);
}

std::string GuidesPanel::copySelectedGuides() {
    const HierarchyPtr h = document_ ? document_->session().authoredHierarchy() : nullptr;
    const std::vector<GuideId> ids = document_ ? document_->selection() : std::vector<GuideId>{};
    if (!h || ids.empty()) {
        emit message(tr("No guides selected"));
        return {};
    }
    try {
        return h->copyGuidesJson(ids);
    } catch (const std::exception& e) {
        emit message(QString::fromStdString(e.what()));
        return {};
    }
}

bool GuidesPanel::pasteGuides(const std::string& text) {
    if (!document_) return false;
    const std::string group = currentGroup();
    if (group.empty()) {
        emit message(tr("Pick a group to paste into"));
        return false;
    }
    GuideOp op;
    op.op = "paste_guides";
    op.text = text;
    op.group = group;
    return guideOp("paste the guides", op);
}

bool GuidesPanel::resampleGuides(int segments, int byLength) {
    if (!document_) return false;
    // Maya's -resample: along the smooth curve through their points, frozen guides kept. Selected
    // guides get segments of their own; a whole group (with none picked: every group) keeps the
    // count for its new guides and leaves guides with their own as they are.
    GuideOp op;
    op.op = "resample";
    op.byLength = byLength < 0 ? byLength_ && byLength_->isChecked() : byLength > 0;
    op.guides = document_->selection();
    if (op.guides.empty()) op.group = currentGroup();
    op.segments = std::clamp(segments, 1, kMaxGuideSegments);
    return guideOp("resample the guides", op);
}

bool GuidesPanel::resetGroupSegments(const std::string& name) {
    GuideOp op;
    op.op = "reset_group_segments";
    op.group = name;
    GuideOpResult done;
    const bool ok = guideOp("reset the group's segments", op, &done);
    if (ok) emit message(tr("Every guide of %1 at %2 segments (frozen ones kept)").arg(qs(name)).arg(done.segments));
    return ok;
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
