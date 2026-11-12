// SPDX-License-Identifier: Apache-2.0
#include "properties_panel.h"
#include "theme.h"

#include <algorithm>

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "glass.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

bool forApp(const ui::json& entry) {
    if (!entry.is_object() || !entry.contains("hosts")) return true;
    for (const ui::json& h : entry.at("hosts")) {
        if (h == ui::kHost) return true;
    }
    return false;
}

std::string keyOf(const ui::json& item) { return item.value("app", item.value("id", std::string())); }

PropertiesPanel::Item makeItem(const ui::json& item, const std::string& section) {
    PropertiesPanel::Item i;
    i.id = item.value("id", std::string());
    i.key = keyOf(item);
    i.label = ui::hostText("label", ui::kHost, {&item});
    i.tip = ui::hostText("tip", ui::kHost, {&item});
    i.section = section;
    i.custom = item.contains("custom");
    i.parts = item.value("parts", ui::json::array());
    return i;
}

void collect(const ui::json& sections, std::vector<PropertiesPanel::Item>& out) {
    for (const ui::json& section : sections) {
        if (!forApp(section)) continue;
        for (const ui::json& item : section.at("items")) {
            if (item.is_object() && forApp(item)) out.push_back(makeItem(item, section.value("id", std::string())));
        }
        collect(section.value("sections", ui::json::array()), out);
    }
}

const ui::json* partOf(const std::string& key, const std::string& part) {
    static const std::vector<PropertiesPanel::Item> items = PropertiesPanel::appItems();
    for (const PropertiesPanel::Item& i : items) {
        if (i.key != key) continue;
        for (const ui::json& p : i.parts) {
            if (p.value("id", std::string()) == part) return &p;
        }
    }
    return nullptr;
}

}  // namespace

// ---- PropertySection ------------------------------------------------------------------------------

PropertySection::PropertySection(const QString& title, bool nested, QWidget* parent) : QWidget(parent), title_(title) {
    setProperty("nested", nested);
    if (!nested) {
        // A section is a card (the style sheet's propCard): its title and its rows on one block.
        setProperty("role", "propCard");
        setAttribute(Qt::WA_StyledBackground, true);
    }
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(nested ? 0 : 12, 0, nested ? 0 : 12, nested ? 2 : 6);
    layout->setSpacing(0);
    header_ = new QPushButton(this);
    header_->setObjectName(QStringLiteral("propSection"));
    header_->setProperty("role", nested ? "propSubsection" : "propSection");
    header_->setCheckable(true);
    header_->setFlat(true);
    header_->setCursor(Qt::PointingHandCursor);
    connect(header_, &QPushButton::toggled, this, [this](bool on) {
        setExpanded(on);
        emit toggled(on);
    });
    layout->addWidget(header_);
    body_ = new QWidget(this);
    body_->setObjectName(QStringLiteral("propBody"));
    grid_ = new QGridLayout(body_);
    grid_->setContentsMargins(0, 0, 0, 6);
    grid_->setHorizontalSpacing(12);
    grid_->setVerticalSpacing(6);
    // One label column for every section (Maya's AE): the controls line up down the whole dock.
    grid_->setColumnMinimumWidth(0, 92);  // (nested ones too: no indent, so the values line up)
    grid_->setColumnStretch(0, 0);
    grid_->setColumnStretch(1, 1);
    layout->addWidget(body_);
    setExpanded(false);
}

void PropertySection::setExpanded(bool expanded) {
    expanded_ = expanded;
    {
        const QSignalBlocker block(header_);
        header_->setChecked(expanded);
    }
    if (property("nested").toBool()) {
        // A section inside a section: its title as it is (a level under the caps), the same
        // chevron as the sections over it.
        header_->setText(QString(title_).replace(QLatin1Char('&'), QStringLiteral("&&")));
        const QString chevron = expanded ? QStringLiteral(":/groomlab/icons/ui_chevron_down.svg") : QStringLiteral(":/groomlab/icons/ui_chevron_right.svg");
        header_->setIcon(QIcon(tintedIcon(chevron, theme::textMuted(), 10, header_->devicePixelRatioF())));
        header_->setIconSize(QSize(10, 10));
    } else {
        // Folded: the title with its first value after it ("Controls · Frizz 1"), what is inside at a glance.
        QString title = title_;
        if (!expanded) {
            for (ValueSlider* s : body_->findChildren<ValueSlider*>()) {
                // (one hidden on purpose skipped; one not shown yet, under the folded body, counts)
                if ((s->isHidden() && s->testAttribute(Qt::WA_WState_ExplicitShowHide)) || s->caption().isEmpty()) continue;
                title += QStringLiteral("  \u00B7  ") + s->caption() + QStringLiteral(" ") + s->shownText();
                if (!s->property("summaryHooked").toBool()) {
                    s->setProperty("summaryHooked", true);
                    connect(s, &ValueSlider::valueChanged, this, [this] {
                        if (!expanded_) setExpanded(false);
                    });
                }
                break;
            }
            // No value to say: what the section says it holds when empty ("none").
            if (title == title_ && !property("emptySummary").toString().isEmpty())
                title += QStringLiteral("  \u00B7  ") + property("emptySummary").toString();
        }
        setSectionHeader(header_, title, expanded);
    }
    body_->setVisible(expanded);
}

// ---- PropertiesPanel --------------------------------------------------------------------------------

std::vector<PropertiesPanel::Item> PropertiesPanel::appItems(const ui::Definitions& definitions) {
    std::vector<Item> out;
    collect(definitions.get("properties/sections"), out);
    return out;
}

QString PropertiesPanel::partLabel(const std::string& key, const std::string& part) {
    const ui::json* p = partOf(key, part);
    return p ? qs(ui::hostText("label", ui::kHost, {p})) : QString();
}

QString PropertiesPanel::partTip(const std::string& key, const std::string& part) {
    const ui::json* p = partOf(key, part);
    return p ? qs(ui::hostText("tip", ui::kHost, {p})) : QString();
}

PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("propertiesPanel"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("propertiesScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* column = new QWidget(scroll_);
    column->setObjectName(QStringLiteral("propertiesColumn"));
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(12, 9, 10, 12);  // the cards 12 px in from the dock's edge, 24 from the rail (the scroll bar beside takes its own 8, the dock 6)
    layout->setSpacing(8);
    build(defs().get("properties/sections"), layout, nullptr);
    layout->addStretch(1);
    scroll_->setWidget(column);
    outer->addWidget(scroll_);
}

void PropertiesPanel::build(const ui::json& sections, QVBoxLayout* into, PropertySection* parent) {
    for (const ui::json& s : sections) {
        if (!forApp(s)) continue;
        const std::string id = s.value("id", std::string());
        auto* section = new PropertySection(qs(ui::hostText("label", ui::kHost, {&s})), parent != nullptr);
        section->setObjectName(QStringLiteral("propSection_") + qs(id));
        sections_[id] = section;
        sectionOrder_.push_back(id);
        if (parent) {
            QGridLayout* grid = parent->grid();
            grid->addWidget(section, grid->rowCount(), 0, 1, 2);
        } else {
            // The first section sits right under the dock's title: no rule over it (no doubled edge).
            if (into->count() == 0) section->header()->setProperty("first", true);
            into->addWidget(section);
        }
        QGridLayout* grid = section->grid();
        for (const ui::json& item : s.at("items")) {
            const int row = grid->rowCount();
            if (item == "-") {
                auto* rule = new QFrame(section->body());
                rule->setProperty("role", "propRule");
                rule->setFrameShape(QFrame::HLine);
                rule->setFixedHeight(1);
                grid->addWidget(rule, row, 0, 1, 2);
                Row r;
                r.section = section;
                r.row = row;
                r.rule = rule;
                rows_.push_back(r);
                continue;
            }
            if (!forApp(item)) continue;
            Row r;
            r.section = section;
            r.row = row;
            r.item = makeItem(item, id);
            r.label = new QLabel(qs(r.item.label), section->body());
            r.label->setProperty("role", "propLabel");
            r.label->setWordWrap(true);
            r.label->setToolTip(qs(r.item.tip));
            r.label->setObjectName(QStringLiteral("propLabel_") + qs(r.item.key));
            grid->addWidget(r.label, row, 0, Qt::AlignVCenter);
            byKey_[r.item.key] = rows_.size();
            rows_.push_back(std::move(r));
        }
        build(s.value("sections", ui::json::array()), nullptr, section);
        section->setExpanded(s.value("expanded", false));
    }
}

bool PropertiesPanel::setControl(const std::string& key, QWidget* control) {
    const auto it = byKey_.find(key);
    if (it == byKey_.end() || !control) return false;
    Row& r = rows_[it->second];
    control->setParent(r.section->body());
    control->setObjectName(control->objectName().isEmpty() ? QStringLiteral("prop_") + qs(key) : control->objectName());
    control->setToolTip(qs(r.item.tip));
    if (auto* field = qobject_cast<ValueSlider*>(control)) {
        // A number field carries its label inside: the whole row, the label column's text in it.
        if (field->caption().isEmpty()) field->setCaption(qs(r.item.label));
        r.label->hide();
        r.inField = true;
        r.section->grid()->addWidget(control, r.row, 0, 1, 2);
    } else {
        // Every other control in a field of its own, as a number field: one row type down the card,
        // the label inside at its left, the control at its right (a check box at the row's end).
        auto* box = new QFrame(r.section->body());
        box->setObjectName(QStringLiteral("propField_") + qs(key));
        box->setProperty("role", "propField");
        box->setMinimumHeight(theme::rowHeight());
        auto* line = new QHBoxLayout(box);
        line->setContentsMargins(8, 0, 2, 0);
        line->setSpacing(8);
        r.label->setParent(box);
        r.label->setWordWrap(false);
        r.label->setMinimumWidth(104);  // one value edge: the controls start past the longest label
        line->addWidget(r.label, 0, Qt::AlignVCenter);
        if (qobject_cast<QCheckBox*>(control)) {
            line->addStretch(1);
            control->setProperty("role", "switch");  // (no label gap: it ends where the rows' numbers end)
            line->addWidget(control, 0, Qt::AlignVCenter);
            line->addSpacing(6);
        } else {
            line->addWidget(control, 1);  // (at the value column, as every field's value)
        }
        r.label->show();
        r.field = box;
        r.section->grid()->addWidget(box, r.row, 0, 1, 2);
    }
    r.control = control;
    return true;
}

bool PropertiesPanel::setBlock(const std::string& key, QWidget* block) {
    const auto it = byKey_.find(key);
    if (it == byKey_.end() || !block) return false;
    Row& r = rows_[it->second];
    block->setParent(r.section->body());
    if (block->objectName().isEmpty()) block->setObjectName(QStringLiteral("prop_") + qs(key));
    r.label->hide();
    r.section->grid()->addWidget(block, r.row, 0, 1, 2);
    r.control = block;
    return true;
}

void PropertiesPanel::finish() {
    for (Row& r : rows_) {
        // One pitch: a shown row (a check box, a value) as tall as a slider's; a hidden one none.
        if (!r.rule) r.section->grid()->setRowMinimumHeight(r.row, r.control && r.wanted ? theme::rowHeight() : 0);
        if (r.rule || r.control) continue;
        r.label->hide();  // nothing the app does for it yet
    }
    // A divider shows between shown rows of its section alone.
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i].rule) continue;
        bool before = false, after = false;
        for (std::size_t k = 0; k < rows_.size(); ++k) {
            if (rows_[k].section != rows_[i].section || rows_[k].rule || !rows_[k].control) continue;
            (k < i ? before : after) = true;
        }
        rows_[i].rule->setVisible(before && after);
    }
    // A section with nothing shown goes (nested ones first: they count for their parent).
    for (auto it = sectionOrder_.rbegin(); it != sectionOrder_.rend(); ++it) {
        PropertySection* s = sections_[*it];
        bool any = std::any_of(rows_.begin(), rows_.end(), [&](const Row& r) { return r.section == s && r.control && r.wanted; });
        for (const auto& [id, child] : sections_) {
            if (child != s && child->parentWidget() == s->body() && !child->isHidden()) any = true;
        }
        s->setVisible(any);
    }
    // The folded sections' summaries, now their controls are there.
    for (const auto& [id, s] : sections_) {
        if (!s->expanded() && !s->property("nested").toBool()) s->setExpanded(false);
    }
}

PropertySection* PropertiesPanel::section(const std::string& id) const {
    const auto it = sections_.find(id);
    return it == sections_.end() ? nullptr : it->second;
}

std::vector<PropertySection*> PropertiesPanel::sections() const {
    std::vector<PropertySection*> out;
    for (const std::string& id : sectionOrder_) out.push_back(sections_.at(id));
    return out;
}

QWidget* PropertiesPanel::control(const std::string& key) const {
    const auto it = byKey_.find(key);
    return it == byKey_.end() ? nullptr : rows_[it->second].control;
}

QLabel* PropertiesPanel::label(const std::string& key) const {
    const auto it = byKey_.find(key);
    return it == byKey_.end() || rows_[it->second].item.custom ? nullptr : rows_[it->second].label;
}

bool PropertiesPanel::shown(const std::string& key) const {
    const auto it = byKey_.find(key);
    return it != byKey_.end() && rows_[it->second].control && rows_[it->second].wanted;
}

std::vector<std::string> PropertiesPanel::boundKeys() const {
    std::vector<std::string> out;
    for (const Row& r : rows_) {
        if (r.control && !r.rule) out.push_back(r.item.key);
    }
    return out;
}

void PropertiesPanel::setRowVisible(const std::string& key, bool visible) {
    const auto it = byKey_.find(key);
    if (it == byKey_.end()) return;
    Row& r = rows_[it->second];
    r.wanted = visible;
    if (!r.rule) r.section->grid()->setRowMinimumHeight(r.row, r.control && visible ? theme::rowHeight() : 0);
    if (r.control) r.control->setVisible(visible);
    if (r.field) r.field->setVisible(visible);
    if (!r.item.custom && !r.inField) r.label->setVisible(visible && r.control);
}

void PropertiesPanel::expandAll(bool expanded) {
    for (const auto& [id, s] : sections_) s->setExpanded(expanded);
}

}  // namespace groomlab::app
