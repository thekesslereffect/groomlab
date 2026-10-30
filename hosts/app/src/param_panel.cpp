// SPDX-License-Identifier: Apache-2.0
#include "param_panel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLayoutItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "glass.h"
#include "groom_document.h"
#include "param_editors.h"
#include "theme.h"
#include "groomlab/graph_io.h"
#include "groomlab/node_catalog.h"
#include "groomlab/simulation.h"

namespace groomlab::app {

using nlohmann::json;

namespace {

// A number param's slider spans 0 to a range that fits its default (twice it, at least 1); a
// negative default spans both sides. A value typed past it widens the span, so the slider is
// a convenience, not a limit.
std::pair<double, double> sliderRange(double defaultValue) {
    const double magnitude = std::max(1.0, std::abs(defaultValue) * 2.0);
    return defaultValue < 0.0 ? std::make_pair(-magnitude, magnitude) : std::make_pair(0.0, magnitude);
}

QString shortJson(const json& value) {
    QString text = QString::fromStdString(value.dump());
    if (text.size() > 60) text = text.left(57) + QStringLiteral("...");
    return text;
}

QString prettyName(const std::string& name) {
    // melaninRedness -> Melanin redness: the spec's camel case read as words.
    QString out;
    for (const char c : name) {
        if (std::isupper(static_cast<unsigned char>(c)) && !out.isEmpty()) out += QLatin1Char(' ');
        out += out.isEmpty() ? QChar(std::toupper(static_cast<unsigned char>(c))) : QChar(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

// How a number shows: the spec's unit (graph.h ParamSpec::unit) as a suffix, a fraction as a
// percentage, with the spec's decimals (its "decimals": the same in every host; as a percentage
// two fewer, 0 to 1; degrees 1) and the trailing zeros dropped when shown. Scene units (length,
// width, density, perLength) show bare unless Edit > Units picks real ones (unitChoice); an
// acceleration in them (Simulate's gravity) shows "/s²" after the number.
std::vector<UnitChoice> unitChoices(const std::string& kind) {
    if (kind == "length") return {{"scene", "units", 1.0}, {"cm", "cm", 1.0}, {"mm", "mm", 10.0}, {"m", "m", 0.01}, {"in", "in", 1.0 / 2.54}};
    if (kind == "width") return {{"scene", "units", 1.0}, {"um", "µm", 1e4}, {"mm", "mm", 10.0}, {"cm", "cm", 1.0}};
    if (kind == "density") return {{"scene", "/unit²", 1.0}, {"cm2", "/cm²", 1.0}, {"mm2", "/mm²", 0.01}, {"in2", "/in²", 2.54 * 2.54}};
    return {};
}

std::string unitChoice(const std::string& kind) {
    const std::string key = QSettings().value(QStringLiteral("units/") + QString::fromStdString(kind), QStringLiteral("scene")).toString().toStdString();
    for (const UnitChoice& c : unitChoices(kind)) {
        if (c.key == key) return key;
    }
    return "scene";
}

void setUnitChoice(const std::string& kind, const std::string& key) {
    QSettings().setValue(QStringLiteral("units/") + QString::fromStdString(kind), QString::fromStdString(key));
}

namespace {

// The picked unit of a kind ({1, ""}: scene units, bare).
UnitChoice pickedUnit(const std::string& kind) {
    const std::string key = unitChoice(kind);
    if (key != "scene") {
        for (const UnitChoice& c : unitChoices(kind)) {
            if (c.key == key) return c;
        }
    }
    return {"scene", "", 1.0};
}

}  // namespace

DisplayUnit displayUnit(const std::string& unit, bool integer, int specDecimals) {
    const int decimals = specDecimals >= 0 ? specDecimals : 3;
    if (unit == "length" || unit == "width" || unit == "density" || unit == "perLength") {
        // Real units when picked: the decimals follow the factor (0.006 cm reads 60 µm).
        UnitChoice c = pickedUnit(unit == "perLength" ? "length" : unit);
        if (unit == "perLength" && c.key != "scene") c = {c.key, "/" + c.label, 1.0 / c.factor};
        if (c.key == "scene" || integer) return {1.0, QString(), decimals};
        const int shift = static_cast<int>(std::lround(std::log10(c.factor)));
        return {c.factor, QStringLiteral(" ") + QString::fromStdString(c.label), std::clamp(decimals - shift, 0, 6)};
    }
    if (unit == "fraction" && !integer) return {100.0, QStringLiteral("%"), std::clamp(decimals - 2, 0, 1)};
    if (unit == "angle") return {1.0, QStringLiteral("°"), 1};
    if (unit == "turns") return {1.0, QStringLiteral(" turns"), decimals};
    if (unit == "seconds") return {1.0, QStringLiteral(" s"), decimals};
    if (unit == "pixels") return {1.0, QStringLiteral(" px"), integer ? 0 : decimals};
    if (unit == "acceleration") return {1.0, QStringLiteral(" /s²"), decimals};
    return {1.0, QString(), decimals};
}

namespace {

const std::string kAdvanced = "Advanced";

// The section a param's row goes in: Advanced for an advanced param (or one of that group), its
// group when the node has two groups or more (`headed`), else the main params ("").
std::string sectionFor(const json& p, bool headed) {
    const std::string group = p.value("group", std::string());
    if (p.value("advanced", false) || group == kAdvanced) return kAdvanced;
    return headed ? group : std::string();
}

// Whether a showWhen's own part holds (graph.h ParamCondition): its {"param", "in"} for the param's
// value (a bool as "true" or "false", a number as JSON writes it). No param: it holds.
bool conditionHolds(const json& condition, const json& value) {
    if (!condition.is_object()) return true;
    if (condition.value("param", std::string()).empty()) return true;
    const std::string text = value.is_string() ? value.get<std::string>() : value.is_boolean() ? (value.get<bool>() ? "true" : "false") : value.dump();
    for (const json& v : condition.value("in", json::array())) {
        if (v.is_string() && v.get<std::string>() == text) return true;
    }
    return false;
}

// Folded sections by operator type and section, for the session (Advanced folded unless opened).
std::map<std::pair<std::string, std::string>, bool>& foldedSections() {
    static std::map<std::pair<std::string, std::string>, bool> folded;
    return folded;
}

}  // namespace

ParamPanel::ParamPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    // Its own spacing, not the platform style's (Windows' varies with the widgets' kinds), so
    // sizeHint's sum is the height the rows take: the form never scrolls at the panel's own height.
    layout->setSpacing(6);
    title_ = new QLabel(tr("No node"), this);
    title_->setStyleSheet(QStringLiteral("font-weight: bold"));
    title_->hide();  // shown with a node
    // The title, and Upgrade beside it for a node saved with an older behaviour of its operator.
    auto* titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleRow->addWidget(title_, 1);
    upgrade_ = new RoundButton(this);
    upgrade_->setObjectName(QStringLiteral("upgradeNode"));
    upgrade_->setText(tr("Upgrade"));
    upgrade_->setDiameter(22);
    upgrade_->setRadius(8.0);
    upgrade_->setPadding(10);
    upgrade_->setFontSize(11);
    upgrade_->setFontWeight(700);
    upgrade_->setFill(theme::actionFill(theme::yellow()), theme::actionFill(theme::yellow()));
    upgrade_->setTextColor(theme::actionText(theme::ink()), theme::actionText(theme::ink()));
    upgrade_->hide();
    connect(upgrade_, &QAbstractButton::clicked, this, [this] { upgrade(); });
    titleRow->addWidget(upgrade_);
    // Presets beside it for a Simulate node: one click to the core's settings (simulatePresets).
    presets_ = new RoundButton(this);
    presets_->setObjectName(QStringLiteral("simulatePresets"));
    presets_->setText(tr("Presets"));
    presets_->setDiameter(22);
    presets_->setRadius(8.0);
    presets_->setPadding(10);
    presets_->setFontSize(11);
    presets_->setToolTip(tr("Set this node's motion settings to a preset (bend, twist, damping, drag, root stiffness and "
                            "its profile, inertia, iterations); gravity, wind, collisions and water stay as they are"));
    presets_->hide();
    connect(presets_, &QAbstractButton::clicked, this, [this] { showPresets(); });
    titleRow->addWidget(presets_);
    layout->addLayout(titleRow);
    // The node's actions (buildActions): right under its title, outside the rows' scroll area.
    actions_ = new QWidget(this);
    actions_->setObjectName(QStringLiteral("nodeActions"));
    actionsGrid_ = new QGridLayout(actions_);
    actionsGrid_->setContentsMargins(0, 0, 0, 0);
    actionsGrid_->setHorizontalSpacing(6);
    actionsGrid_->setVerticalSpacing(4);
    actions_->hide();
    layout->addWidget(actions_);
    // The node's error from the last evaluation, red under its title (the graph's badge says it too).
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("nodeError"));
    error_->setWordWrap(true);
    error_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    error_->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(theme::errorText().name()));
    error_->hide();
    layout->addWidget(error_);
    // The operator's description (what Maya's Attribute Editor shows under the node's name).
    description_ = new QLabel(this);
    description_->setWordWrap(true);
    description_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    description_->hide();
    layout->addWidget(description_);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("nodeNote"));
    note_->setWordWrap(true);
    note_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::graphNote().name()));
    note_->hide();
    layout->addWidget(note_);
    bypass_ = new QCheckBox(tr("Bypass"), this);
    bypass_->setEnabled(false);
    bypass_->hide();
    layout->addWidget(bypass_);
    // When the node runs: always, in the viewport only (exports skip it), in renders only.
    evaluation_ = new QComboBox(this);
    evaluation_->setObjectName(QStringLiteral("nodeEvaluation"));
    evaluation_->addItem(tr("Evaluate: Always"), QStringLiteral("always"));
    evaluation_->addItem(tr("Evaluate: Viewport Only"), QStringLiteral("viewportOnly"));
    evaluation_->addItem(tr("Evaluate: Render Only"), QStringLiteral("renderOnly"));
    evaluation_->setToolTip(tr("Viewport Only: exports and renders skip the node (a fast stand-in). Render Only: the "
                               "viewport skips it (a heavy node kept for the final strands)."));
    evaluation_->hide();
    layout->addWidget(evaluation_);
    empty_ = new QLabel(tr("No node selected: click one in the list or the graph"), this);
    empty_->setWordWrap(true);
    empty_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    layout->addWidget(empty_);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // the rows take the panel's width
    scroll_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    formHost_ = new QWidget(scroll_);
    // The rows never widen the panel: a wide editor takes the width the panel has.
    formHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    list_ = new QVBoxLayout(formHost_);
    list_->setContentsMargins(0, 0, 8, 0);  // room before the scroll bar for the sliders' numbers
    list_->setSpacing(8);
    scroll_->setWidget(formHost_);
    layout->addWidget(scroll_, 1);
    connect(bypass_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_ || !document_ || node_.empty()) return;
        const std::string error = document_->setBypass(node_, on);
        if (!error.empty()) {
            emit message(QString::fromStdString(error));
            refresh();
            return;
        }
        emit edited();
    });
    connect(evaluation_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (updating_ || !document_ || node_.empty() || index < 0) return;
        const std::string error = document_->setEvaluation(node_, evaluation_->itemData(index).toString().toStdString());
        if (!error.empty()) {
            emit message(QString::fromStdString(error));
            refresh();
            return;
        }
        emit edited();
    });
}

void ParamPanel::setDocument(GroomDocument* document) {
    document_ = document;
    node_.clear();
    build();
}

void ParamPanel::showNode(const std::string& node) {
    node_ = node;
    build();
}

void ParamPanel::refreshNote() {
    std::string text, error;
    if (document_ && !node_.empty()) {
        for (const auto& [name, why] : document_->nodeErrors()) {
            if (name == node_) error = why;
        }
        for (const auto& [name, note] : document_->nodeNotes()) {  // an attribute read that does not reach it too
            if (name == node_) text = note;
        }
        // A Scatter's or Resample's strand resolution: which Resample sets it (each branch its own).
        const nlohmann::json graph = nlohmann::json::parse(document_->graphJson(), nullptr, false);
        bool resamples = false;
        if (graph.is_object() && graph.contains("nodes") && graph["nodes"].is_array()) {
            for (const nlohmann::json& n : graph["nodes"]) {
                const std::string type = n.value("type", std::string());
                resamples = resamples || (n.value("name", std::string()) == node_ && (type == "Scatter" || type == "Resample"));
            }
        }
        if (resamples) {
            try {
                const GraphDocument doc = loadGraphJson(document_->graphJson());
                const std::string resolution = doc.graph.strandResolutionNote(doc.node(node_));
                if (!resolution.empty()) text += (text.empty() ? "" : "\nⓘ ") + resolution;
            } catch (const std::exception&) {
            }
        }
    }
    note_->setText(text.empty() ? QString() : QStringLiteral("\u24d8 ") + QString::fromStdString(text));
    note_->setVisible(!text.empty());
    error_->setText(error.empty() ? QString() : QStringLiteral("⚠ ") + QString::fromStdString(error));
    error_->setToolTip(QString::fromStdString(error));
    error_->setVisible(!error.empty());
    fitWrappedLabels();
}

void ParamPanel::fitWrappedLabels() {
    // The wrapped lines under the title keep every line: the rows' scroll area gives way, not them.
    const int w = width() >= 212 && width() <= 432 ? width() - 12 : 300;  // (not laid out yet: the Node panel's)
    for (QLabel* label : {description_, note_, error_}) {
        label->setMinimumHeight(label->isHidden() || label->text().isEmpty() ? 0 : label->heightForWidth(w));
    }
}

void ParamPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fitWrappedLabels();
}

QString ParamPanel::note() const { return note_->isVisible() ? note_->text() : QString(); }

QString ParamPanel::error() const { return error_->isHidden() ? QString() : error_->text(); }

void ParamPanel::clearForm() {
    while (QLayoutItem* item = list_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    rows_.clear();
    sections_.clear();
}

QWidget* ParamPanel::attributeEditor(Row& row, const json& p, const json& value, const json& channels) {
    const bool writes = p.value("channel", std::string()) == "writes";
    const std::string name = row.name;
    const std::string current = value.is_string() ? value.get<std::string>() : std::string();
    auto* host = new QWidget(formHost_);
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* h = new QHBoxLayout();
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto* chip = new QLabel(writes ? tr("Writes") : tr("Reads"), host);
    chip->setObjectName(QStringLiteral("attributeUse"));
    const QColor chipFill = writes ? theme::graphWrites() : theme::graphReads();
    chip->setStyleSheet(QStringLiteral("QLabel { background: %1; color: %2; border-radius: 6px; padding: 1px 6px; font-weight: 700; }")
                            .arg(chipFill.name(), theme::readableOn(chipFill).name()));
    chip->setToolTip(writes ? tr("Writes the attribute named here: a value per strand that nodes after it read by name. A name a node "
                                 "upstream writes is overwritten from here on.")
                            : tr("Reads the attribute named here, written by a node upstream (the list shows those reaching this node "
                                 "and which node writes each). A name that does not reach this node is ignored."));
    h->addWidget(chip);
    if (writes) {
        auto* line = new QLineEdit(host);
        line->setObjectName(QStringLiteral("attributeName"));
        line->setText(QString::fromStdString(current));
        line->setPlaceholderText(tr("none (no attribute)"));
        connect(line, &QLineEdit::editingFinished, this, [this, name, line, current] {
            const std::string text = line->text().trimmed().toStdString();
            if (text == current) return;
            edit(name, text, true);
            refresh();  // the warning as it is now
        });
        row.editors = {line};
        h->addWidget(line, 1);
    } else {
        auto* combo = new QComboBox(host);
        combo->setObjectName(QStringLiteral("attributeName"));
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        for (const json& c : channels.value("incoming", json::array())) {
            const std::string channel = c.value("name", std::string()), node = c.value("node", std::string());
            combo->addItem(QString::fromStdString(node.empty() ? channel : channel + " (from " + node + ")"), QString::fromStdString(channel));
        }
        combo->setCurrentIndex(-1);
        combo->setEditText(QString::fromStdString(current));
        combo->lineEdit()->setPlaceholderText(tr("none"));
        combo->setToolTip(tr("Attributes reaching this node, each with the node that writes it last; type any other name"));
        auto commit = [this, name, current](const std::string& text) {
            if (text == current) return;
            edit(name, text, true);
            refresh();
        };
        connect(combo, QOverload<int>::of(&QComboBox::activated), this, [combo, commit](int index) {
            const QString channel = combo->itemData(index).toString();
            combo->setEditText(channel);
            commit(channel.toStdString());
        });
        connect(combo->lineEdit(), &QLineEdit::editingFinished, this, [combo, commit] {
            QString text = combo->currentText().trimmed();
            const int at = combo->findText(text);
            if (at >= 0) text = combo->itemData(at).toString();
            commit(text.toStdString());
        });
        row.editors = {combo};
        h->addWidget(combo, 1);
    }
    v->addLayout(h);
    QString note;
    QString color = theme::graphNote().name();
    if (writes) {
        QString over;
        for (const json& o : channels.value("overwrites", json::array())) {
            if (o.value("param", std::string()) != name) continue;
            over += QString(over.isEmpty() ? "" : ", ") + QString::fromStdString(o.value("name", std::string()) + " from " + o.value("node", std::string()));
        }
        if (!over.isEmpty()) {
            note = QStringLiteral("\u26a0 overwrites ") + over;
            color = theme::graphWarning().name();
        }
    } else {
        for (const json& r : channels.value("reads", json::array())) {
            if (r.value("param", std::string()) != name || r.value("reaches", false)) continue;
            note = QStringLiteral("\u24d8 no node upstream writes ") + QString::fromStdString(r.value("name", std::string())) +
                   (r.value("optional", false) ? tr(": none applies") : tr(": this node ignores it"));
        }
    }
    if (!note.isEmpty()) {
        auto* label = new QLabel(note, host);
        label->setObjectName(QStringLiteral("attributeNote"));
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: %1;").arg(color));
        if (writes) {
            label->setToolTip(tr("A node upstream already writes this attribute: from here on the strands carry this node's, so nodes "
                                 "after it read this one. Give each node its own name (chained Clumps: lockId, then clumpId) unless "
                                 "replacing it is what you want."));
        }
        v->addWidget(label);
        row.attributeNote = label;
    }
    row.editor = writes ? "attributeWrites" : "attributeReads";
    return host;
}

QWidget* ParamPanel::numberEditor(Row& row, const json& p, const json& value, const QString& caption) {
    // One slider with the caption and the number on its line (the number edited inline, a value
    // typed past the span widening it): the span fits the default, the value may go anywhere.
    const std::string name = row.name;
    const bool integer = row.kind == "int";
    auto* slider = new ValueSlider(formHost_);
    slider->setStacked(true);
    slider->setCaption(caption);
    slider->setFill(integer ? theme::purple() : theme::blue());
    // Shown in the spec's unit: a fraction as a percentage (the slider holds the value x 100, the
    // document the value), the others with their suffix.
    // A unit that holds only while another param has some value (unitWhen: Noise's frequency in
    // world space) shows only then.
    const json unitWhen = p.value("unitWhen", json());
    const bool unitHolds = unitWhen.is_null() || conditionHolds(unitWhen, document_->paramOrDefault(node_, unitWhen.value("param", std::string())));
    row.unit = unitHolds ? p.value("unit", std::string()) : std::string();
    row.unitWhen = unitWhen;
    // Simulate's gravity is a length per second squared: its number carries "/s²" (scene units).
    const std::string shownUnit = (row.unit == "length" && name == "gravity") ? std::string("acceleration") : row.unit;
    const DisplayUnit unit = displayUnit(shownUnit, integer, p.value("decimals", -1));
    const double f = unit.factor;
    row.factor = f;
    const double defaultValue = (p.value("default", json(0.0)).is_number() ? p["default"].get<double>() : 0.0) * f;
    // The spec's slider range (softMin, softMax) when it has one, else one that fits the default; the
    // spec's hard limits (min, max: the core clamps there) bound what may be typed.
    auto range = sliderRange(defaultValue);
    if (p.contains("softMin") && p.contains("softMax")) range = {p["softMin"].get<double>() * f, p["softMax"].get<double>() * f};
    const double lowest = p.contains("min") ? p["min"].get<double>() * f : (integer ? -1000000000.0 : -1e9);
    const double highest = p.contains("max") ? p["max"].get<double>() * f : (integer ? 1000000000.0 : 1e9);
    slider->setRange(range.first, range.second);
    if (integer) {
        slider->setInteger(true);
        slider->setLimits(lowest, highest);
    } else {
        slider->setDecimals(unit.decimals);
        slider->setStep(std::max(0.001, (range.second - range.first) / 200.0));
        slider->setLimits(lowest, highest);
    }
    // The number with the spec's decimals, trailing zeros dropped (0.35, not 0.350), then the unit.
    const QString suffix = unit.suffix;
    const int decimals = unit.decimals;
    slider->setFormat([suffix, decimals, integer](double v) {
        return (integer ? QString::number(std::lround(v)) : trimmedNumber(v, decimals)) + suffix;
    });
    slider->setValue(value.is_number() ? value.get<double>() * f : defaultValue);
    // A drag previews every step and records one edit from where it started when the mouse
    // goes up; a typed value or a wheel step comes as a press, a change and a release at once.
    connect(slider, &ValueSlider::pressed, this, [this, name] {
        sliderDown_ = true;
        if (document_ && !node_.empty()) dragStart_ = document_->paramOrDefault(node_, name);
        emit dragging(true);
    });
    connect(slider, &ValueSlider::valueChanged, this, [this, name, integer, f](double v) {
        if (updating_) return;
        if (integer) edit(name, static_cast<std::int64_t>(std::lround(v)), false);
        else edit(name, v / f, false);
    });
    connect(slider, &ValueSlider::released, this, [this, name] {
        sliderDown_ = false;
        emit dragging(false);
        if (!document_ || node_.empty()) return;
        const json now = document_->paramOrDefault(node_, name);
        if (now != dragStart_) document_->recordEdit(node_, name, dragStart_, now);
        emit edited();
    });
    row.editors = {slider};
    return slider;
}

void ParamPanel::build() {
    updating_ = true;
    clearForm();
    note_->hide();  // shown again for a node with one
    error_->hide();
    const json* spec = document_ && !node_.empty() ? document_->nodeSpec(node_) : nullptr;
    title_->setVisible(spec != nullptr);
    description_->setVisible(spec != nullptr);
    bypass_->setVisible(spec != nullptr);
    evaluation_->setVisible(spec != nullptr);
    empty_->setVisible(spec == nullptr);
    scroll_->setVisible(spec != nullptr);
    upgrade_->hide();
    presets_->hide();
    type_.clear();
    buildActions();
    if (!spec) {
        title_->setText(node_.empty() ? tr("No node") : QString::fromStdString(node_));
        bypass_->setEnabled(false);
        bypass_->setChecked(false);
        updating_ = false;
        updateGeometry();
        return;
    }
    // The type by its label ("Linked Groom" for a Reference node), as every host shows it.
    // A node from the node library by its own label ("Soft Frizz").
    const std::string kind = spec->contains("library") ? spec->value("label", std::string()) : nodeLabel(spec->value("type", std::string()));
    title_->setText(QString::fromStdString(node_ + "  (" + kind + ")"));
    const auto [saved, newest] = document_->nodeVersion(node_);
    if (saved < newest) {
        // Saved with an older behaviour of its operator: Upgrade gives it the newest.
        const QString opType = QString::fromStdString(nodeLabel(spec->value("type", std::string())));
        upgrade_->setToolTip(tr("This %1 keeps the behaviour of version %2, as it was saved; %1 nodes are at version %3 now.\n"
                                "Upgrade: it evaluates as new %1 nodes do (its result may change). One undo step.")
                                 .arg(opType)
                                 .arg(saved)
                                 .arg(newest));
        upgrade_->show();
    }
    presets_->setVisible(spec->value("type", std::string()) == "Simulate");
    description_->setText(QString::fromStdString(spec->value("description", std::string())));
    description_->setVisible(!description_->text().isEmpty());
    refreshNote();
    bypass_->setEnabled(true);
    for (const GroomDocument::NodeInfo& info : document_->nodes()) {
        if (info.name != node_) continue;
        bypass_->setChecked(info.bypass);
        const int at = evaluation_->findData(QString::fromStdString(info.evaluation));
        evaluation_->setCurrentIndex(at < 0 ? 0 : at);
    }
    // What the map sources offer: the groom's painted maps and the scalp's other color sets.
    MapSourceEditor::Choices choices;
    for (const PaintedMap& m : document_->maps()) choices.maps.push_back({m.name, m.kind, m.texture, m.channels});
    choices.colorSets = document_->colorSetNames();

    const std::string type = spec->value("type", std::string());
    type_ = type;
    // The groups, in the order the spec lists them: headers only with two or more (one group reads
    // as the main params, as before the groups were named); every advanced param in Advanced.
    std::vector<std::string> groups;
    for (const json& p : spec->value("params", json::array())) {
        const std::string section = sectionFor(p, true);
        if (!section.empty() && section != kAdvanced && std::find(groups.begin(), groups.end(), section) == groups.end()) {
            groups.push_back(section);
        }
    }
    const bool headed = groups.size() >= 2;
    // The attributes reaching the node, written and read (graphChannels), for the attribute params.
    const json allChannels = document_->channels();
    const json nodeChannels = allChannels.value(node_, json::object());
    for (const json& p : spec->value("params", json::array())) {
        Row row;
        row.name = p.value("name", std::string());
        row.kind = p.value("kind", std::string());
        // A Compound's definition is its inner graph, not a value to edit: its exposed params are the rows.
        if ((type == "Compound" || type == "Script") && row.name == "definition") continue;
        row.section = sectionFor(p, headed);
        row.showWhen = p.value("showWhen", json());
        const json value = document_->paramOrDefault(node_, row.name);
        const QString description = QString::fromStdString(p.value("description", std::string()));
        const bool mappable = p.value("mappable", false);
        const bool exposed = document_->paramExposed(node_, row.name);
        const std::string fedBy = document_->paramInputSource(node_, row.name);
        QWidget* field = nullptr;
        const std::string name = row.name;
        if (row.kind == "bool") {
            auto* box = new QCheckBox(formHost_);
            box->setChecked(value.is_boolean() && value.get<bool>());
            connect(box, &QCheckBox::toggled, this, [this, name](bool on) { edit(name, on, true); });
            row.editors = {box};
            row.editor = "bool";
            field = box;
        } else if ((row.kind == "int" || row.kind == "float") && mappable) {
            // The value's editor inside the source editor: shown while the source is a value
            // (the row's label names the param, so the slider carries the number alone).
            QWidget* constant = numberEditor(row, p, value.is_number() ? value : p.value("default", json(0.0)), QString());
            auto* map = new MapSourceEditor(constant, formHost_);
            map->setChoices(choices);
            map->setValue(value, exposed, fedBy);
            connect(map, &MapSourceEditor::mapEdited, this, [this, name, map] {
                if (updating_) return;
                edit(name, map->value(), true);
            });
            connect(map, &MapSourceEditor::sourceChanged, this, [this, name, map](MapSourceEditor::Source source) {
                if (updating_ || source != MapSourceEditor::Source::Constant) return;
                // Back to a number: the value's editor shows; what it holds is the value now.
                for (const Row& r : rows_) {
                    if (r.name == name) edit(name, valueOf(r), true);
                }
            });
            connect(map, &MapSourceEditor::portRequested, this, [this, name](bool on) {
                if (updating_ || !document_ || node_.empty()) return;
                const std::string error = document_->exposeParam(node_, name, on);
                if (!error.empty()) {
                    emit message(QString::fromStdString(error));
                    refresh();
                    return;
                }
                emit edited();
            });
            row.map = map;
            row.editor = "map";
            field = map;
        } else if (row.kind == "int" || row.kind == "float") {
            field = numberEditor(row, p, value, prettyName(row.name));
            row.editor = row.kind;
        } else if (row.kind == "string" && (p.value("channel", std::string()) == "writes" || p.value("channel", std::string()) == "reads")) {
            field = attributeEditor(row, p, value, nodeChannels);
        } else if (row.kind == "string") {
            const json choicesList = p.value("choices", json::array());
            if (choicesList.is_array() && !choicesList.empty()) {
                auto* combo = new QComboBox(formHost_);
                for (const json& c : choicesList) combo->addItem(QString::fromStdString(c.get<std::string>()));
                combo->setCurrentText(QString::fromStdString(value.is_string() ? value.get<std::string>() : std::string()));
                connect(combo, &QComboBox::currentTextChanged, this,
                        [this, name](const QString& text) { edit(name, text.toStdString(), true); });
                row.editors = {combo};
                row.editor = "enum";
                field = combo;
            } else {
                auto* line = new QLineEdit(formHost_);
                line->setText(QString::fromStdString(value.is_string() ? value.get<std::string>() : std::string()));
                connect(line, &QLineEdit::editingFinished, this, [this, name, line] { edit(name, line->text().toStdString(), true); });
                row.editors = {line};
                row.editor = "string";
                field = line;
            }
        } else if (row.kind == "vec3" && p.value("unit", std::string()) == "color") {
            auto* color = new ColorEditor(formHost_);
            if (value.is_array() && value.size() == 3 && value[0].is_number() && value[1].is_number() && value[2].is_number()) {
                color->setLinear({value[0].get<float>(), value[1].get<float>(), value[2].get<float>()});
            }
            connect(color, &ColorEditor::edited, this, [this, name, color](bool live) {
                if (updating_) return;
                const auto rgb = color->linear();
                const json v = json::array({rgb[0], rgb[1], rgb[2]});
                // The picker's live colors are one edit, from the color before the first.
                if (live && !dragLive_) {
                    dragStart_ = document_ ? document_->paramOrDefault(node_, name) : json();
                    dragLive_ = true;
                }
                edit(name, v, !live && !dragLive_);
                if (!live && dragLive_) {
                    dragLive_ = false;
                    if (document_ && !node_.empty()) document_->recordEdit(node_, name, dragStart_, document_->paramOrDefault(node_, name));
                }
            });
            row.color = color;
            row.editor = "color";
            field = color;
            if (p.value("colorField", false)) {
                // A color a Map node's rgb output can drive: a port for it on the node (the color
                // here then multiplies the map's).
                auto* host = new QWidget(formHost_);
                auto* column = new QVBoxLayout(host);
                column->setContentsMargins(0, 0, 0, 0);
                column->setSpacing(4);
                color->setParent(host);
                column->addWidget(color);
                auto* portRow = new QHBoxLayout();
                portRow->setContentsMargins(0, 0, 0, 0);
                auto* port = new QCheckBox(tr("Map port"), host);
                port->setObjectName(QStringLiteral("colorPort_") + QString::fromStdString(name));
                port->setChecked(exposed);
                port->setToolTip(tr("Expose the color as an input on the node, for a Map node's rgb output to color "
                                    "each strand (times this color)"));
                portRow->addWidget(port);
                auto* fed = new QLabel(fedBy.empty() ? QString() : tr("× %1").arg(QString::fromStdString(fedBy)), host);
                fed->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
                portRow->addWidget(fed, 1);
                column->addLayout(portRow);
                connect(port, &QCheckBox::toggled, this, [this, name](bool on) {
                    if (updating_ || !document_ || node_.empty()) return;
                    const std::string error = document_->exposeParam(node_, name, on);
                    if (!error.empty()) {
                        emit message(QString::fromStdString(error));
                        refresh();
                        return;
                    }
                    emit edited();
                });
                field = host;
            }
        } else if (row.kind == "vec3") {
            auto* host = new QWidget(formHost_);
            auto* h = new QHBoxLayout(host);
            h->setContentsMargins(0, 0, 0, 0);
            // The spec's decimals (the catalog's, as every host shows them), trailing zeros dropped.
            const int decimals = p.value("decimals", 3);
            for (int k = 0; k < 3; ++k) {
                auto* spin = new TrimmedSpinBox(host);
                spin->setRange(-1e9, 1e9);
                spin->setDecimals(std::max(0, decimals));
                spin->setSingleStep(0.01);
                spin->setKeyboardTracking(false);
                spin->setMinimumWidth(56);
                spin->setValue(value.is_array() && value.size() == 3 && value[k].is_number() ? value[k].get<double>() : 0.0);
                h->addWidget(spin);
                row.editors.push_back(spin);
            }
            for (QWidget* w : row.editors) {
                connect(static_cast<QDoubleSpinBox*>(w), &QDoubleSpinBox::valueChanged, this, [this, name] {
                    if (updating_) return;
                    for (const Row& r : rows_) {
                        if (r.name == name) edit(name, valueOf(r), true);
                    }
                });
            }
            row.editor = "vec3";
            field = host;
        } else if (row.kind == "ramp") {
            auto* host = new QWidget(formHost_);
            auto* v = new QVBoxLayout(host);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(4);
            auto* ramp = new RampEditor(host);
            ramp->setValue(value);
            v->addWidget(ramp);
            // A Curve port the graph wires (a Curve node's ramp replaces this one while fed).
            auto* portRow = new QHBoxLayout();
            portRow->setContentsMargins(0, 0, 0, 0);
            auto* port = new QCheckBox(tr("Curve port"), host);
            port->setChecked(exposed);
            port->setToolTip(tr("Expose the ramp as a Curve input on the node, for a Curve node to feed"));
            portRow->addWidget(port);
            auto* fed = new QLabel(fedBy.empty() ? QString() : tr("fed by %1").arg(QString::fromStdString(fedBy)), host);
            fed->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
            portRow->addWidget(fed, 1);
            v->addLayout(portRow);
            connect(port, &QCheckBox::toggled, this, [this, name](bool on) {
                if (updating_ || !document_ || node_.empty()) return;
                const std::string error = document_->exposeParam(node_, name, on);
                if (!error.empty()) {
                    emit message(QString::fromStdString(error));
                    refresh();
                    return;
                }
                emit edited();
            });
            connect(ramp, &RampEditor::edited, this, [this, name, ramp](bool live) {
                if (updating_) return;
                if (live && !dragLive_) {
                    dragStart_ = document_ ? document_->paramOrDefault(node_, name) : json();
                    dragLive_ = true;
                }
                edit(name, ramp->value(), !live && !dragLive_);
                if (!live && dragLive_) {
                    dragLive_ = false;
                    if (document_ && !node_.empty()) document_->recordEdit(node_, name, dragStart_, document_->paramOrDefault(node_, name));
                }
            });
            row.ramp = ramp;
            row.editor = "ramp";
            field = host;
        } else {
            // A map param (a Map node's own): the JSON as it is.
            auto* label = new QLabel(shortJson(value), formHost_);
            label->setToolTip(QString::fromStdString(value.dump(2)));
            row.editors = {label};
            row.editor = "label";
            field = label;
        }
        const std::string channelUse = p.value("channel", std::string());
        field->setToolTip(channelUse == "writes" ? tr("Writes an attribute (a value per strand, read by name downstream).\n") + description
                          : channelUse == "reads" ? tr("Reads an attribute written upstream.\n") + description
                                                  : description);
        auto* rowHost = new QWidget(formHost_);
        auto* label = new QLabel(prettyName(row.name), rowHost);
        label->setToolTip(description);
        label->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(theme::textDim().name()));
        field->setParent(rowHost);
        if (row.editor == "int" || row.editor == "float") {
            // The slider carries its own caption: the whole width, no label beside it.
            delete label;
            auto* v = new QVBoxLayout(rowHost);
            v->setContentsMargins(0, 0, 0, 0);
            v->addWidget(field);
        } else if (row.editor == "ramp" || row.editor == "color" || row.editor == "map") {
            // A wide editor under its name, the panel's whole width.
            auto* v = new QVBoxLayout(rowHost);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(4);
            if (row.editor == "map") {
                // Paint this param (Maya's right-click > Paint, in one click): a vertex map named
                // after the node and param, wired here, the Paint brush on it.
                auto* top = new QHBoxLayout();
                top->setContentsMargins(0, 0, 0, 0);
                top->addWidget(label, 1);
                auto* paint = new RoundButton(rowHost);
                paint->setObjectName(QStringLiteral("paintParam_") + QString::fromStdString(name));
                paint->setText(tr("Paint"));
                paint->setDiameter(22);
                paint->setRadius(8.0);
                paint->setPadding(10);
                paint->setFontSize(11);
                paint->setFontWeight(700);
                paint->setFill(theme::actionFill(theme::yellow()), theme::actionFill(theme::yellow()));
                paint->setTextColor(theme::actionText(theme::ink()), theme::actionText(theme::ink()));
                paint->setToolTip(tr("Paint this param: a vertex map named after the node and param (white = its value now, black = 0) "
                                     "feeds it, and the Paint brush paints it: strokes lower it, Ctrl raises it (one undo step)"));
                connect(paint, &QAbstractButton::clicked, this, [this, name] {
                    if (!document_ || node_.empty()) return;
                    emit paintParamRequested(node_, name);
                });
                top->addWidget(paint);
                v->addLayout(top);
            } else {
                v->addWidget(label);
            }
            v->addWidget(field);
        } else {
            auto* h = new QHBoxLayout(rowHost);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(8);
            label->setFixedWidth(108);
            h->addWidget(label);
            h->addWidget(field, 1);
        }
        // Any other param takes a Value port (one value for the whole node: a Value node, or an
        // Expression set to give a value): right-click the row to expose it; while one feeds it,
        // the row says so and its editor rests.
        // A param that takes an input port (Maya's linkable): its right-click menu, the host's
        // (graph_ui.json's paramMenu: Expose as Input, Paint, Drive with Slider), else Expose alone.
        if (p.contains("port") && p["port"].is_object()) {
            rowHost->setProperty("glParam", QString::fromStdString(name));
            rowHost->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(rowHost, &QWidget::customContextMenuRequested, this, [this, name, rowHost](const QPoint& at) {
                if (!document_ || node_.empty()) return;
                QMenu menu(this);
                fillParamMenu(&menu, name);
                if (!menu.isEmpty()) menu.exec(rowHost->mapToGlobal(at));
            });
        }
        if (p.contains("port") && p["port"].value("type", std::string()) == "Value") {
            rowHost->setObjectName(QStringLiteral("paramRow_") + QString::fromStdString(name));
            if (exposed && !fedBy.empty()) {
                field->setEnabled(false);
                auto* fed = new QLabel(tr("← set by %1").arg(QString::fromStdString(fedBy)), rowHost);
                fed->setObjectName(QStringLiteral("paramFed_") + QString::fromStdString(name));
                fed->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
                rowHost->layout()->addWidget(fed);
            } else if (exposed) {
                field->setToolTip(field->toolTip() + tr("\nExposed as an input: a Value node linked to it sets it"));
            }
        }
        row.host = rowHost;
        rows_.push_back(std::move(row));
    }
    // The rows by section: the main params, each group under its header, Advanced last.
    std::vector<std::string> order{std::string()};
    if (headed) order.insert(order.end(), groups.begin(), groups.end());
    order.push_back(kAdvanced);
    for (const std::string& section : order) {
        if (std::none_of(rows_.begin(), rows_.end(), [&](const Row& r) { return r.section == section; })) continue;
        Section s;
        s.name = section;
        if (!section.empty()) {
            auto* header = new QPushButton(formHost_);
            header->setObjectName(QStringLiteral("paramSection_") + QString::fromStdString(section));
            header->setCheckable(true);
            header->setFlat(true);
            header->setCursor(Qt::PointingHandCursor);
            // The design's caps header: small muted capitals after a chevron, a hairline above.
            header->setStyleSheet(QStringLiteral(
                "QPushButton { background: transparent; border: none; border-top: 1px solid %3; border-radius: 0; color: %1; "
                "font-size: 11px; font-weight: 600; text-align: left; padding: 6px 0 2px 0; min-height: 0; } "
                "QPushButton:hover { color: %2; }")
                                      .arg(theme::textMuted().name(), theme::textDim().name(), theme::divider().name()));
            header->setChecked(!folded(section));
            titleSection(header, section);
            connect(header, &QAbstractButton::toggled, this, [this, section](bool open) { setFolded(section, !open); });
            list_->addWidget(header);
            s.header = header;
        }
        for (const Row& r : rows_) {
            if (r.section == section) list_->addWidget(r.host);
        }
        sections_.push_back(s);
    }
    list_->addStretch(1);
    updateVisibility();
    updating_ = false;
    updateGeometry();
}

void ParamPanel::titleSection(QAbstractButton* header, const std::string& name) const {
    setSectionHeader(header, QString::fromStdString(name), header->isChecked());
}

bool ParamPanel::folded(const std::string& section) const {
    if (section.empty()) return false;
    const auto it = foldedSections().find({type_, section});
    return it != foldedSections().end() ? it->second : section == kAdvanced;
}

void ParamPanel::setFolded(const std::string& section, bool on) {
    if (section.empty() || type_.empty()) return;
    foldedSections()[{type_, section}] = on;
    for (const Section& s : sections_) {
        if (s.name != section || !s.header) continue;
        const QSignalBlocker block(s.header);
        s.header->setChecked(!on);
        titleSection(s.header, section);
    }
    updateVisibility();
}

void ParamPanel::updateVisibility() {
    if (!document_ || node_.empty()) return;
    // A unit that follows another param (Noise's frequency by its space) changed: built again.
    for (const Row& r : rows_) {
        if (r.unitWhen.is_null()) continue;
        if (conditionHolds(r.unitWhen, document_->paramOrDefault(node_, r.unitWhen.value("param", std::string()))) == r.unit.empty()) {
            refresh();
            return;
        }
    }
    // A row's showWhen holds, and the row of the param it names shows too, and so on up (Clump's
    // count hides with clumpBy).
    auto shownByCondition = [&](const Row& row) {
        const Row* r = &row;
        for (std::size_t hops = 0; r && hops <= rows_.size(); ++hops) {
            const std::string on = r->showWhen.is_null() ? std::string() : r->showWhen.value("param", std::string());
            if (on.empty()) return true;
            if (!conditionHolds(r->showWhen, document_->paramOrDefault(node_, on))) return false;
            const auto next = std::find_if(rows_.begin(), rows_.end(), [&](const Row& o) { return o.name == on; });
            r = next == rows_.end() ? nullptr : &*next;
        }
        return true;
    };
    for (const Section& s : sections_) {
        const bool open = !folded(s.name);
        bool anyShown = false;
        for (const Row& r : rows_) {
            if (r.section != s.name) continue;
            const bool shown = shownByCondition(r);
            anyShown = anyShown || shown;
            r.host->setVisible(shown && open);
        }
        if (s.header) s.header->setVisible(anyShown);
    }
    updateGeometry();
}

bool ParamPanel::rowShown(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name) return r.host && r.host->isVisibleTo(const_cast<ParamPanel*>(this));
    }
    return false;
}

std::string ParamPanel::sectionOf(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name) return r.section;
    }
    return {};
}

ValueSlider* ParamPanel::numberSlider(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name && (r.kind == "float" || r.kind == "int") && !r.editors.empty()) return qobject_cast<ValueSlider*>(r.editors[0]);
    }
    return nullptr;
}

void ParamPanel::fillParamMenu(QMenu* menu, const std::string& name) {
    if (!document_ || node_.empty()) return;
    if (paramMenuFiller_) {
        paramMenuFiller_(menu, node_, name);
        return;
    }
    QAction* expose = menu->addAction(tr("Expose as Input"));
    expose->setCheckable(true);
    expose->setChecked(document_->paramExposed(node_, name));
    expose->setToolTip(tr("A port on the node for a Value node (or an Expression set to give a value) to set this param for the whole node"));
    connect(expose, &QAction::triggered, this, [this, name](bool on) {
        const std::string error = document_->exposeParam(node_, name, on);
        if (!error.empty()) emit message(QString::fromStdString(error));
        // Not under the menu's own click: the panel is built again on the next turn of the loop.
        QTimer::singleShot(0, this, [this] {
            refresh();
            emit edited();
        });
    });
}

void ParamPanel::upgrade() {
    if (!document_ || node_.empty()) return;
    const std::string error = document_->upgradeNode(node_);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    // Not under the button's own click: the panel is built again on the next turn of the loop.
    refresh();
    emit edited();
}

void ParamPanel::showPresets() {
    if (!document_ || node_.empty()) return;
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    for (const SimPreset& preset : simulatePresets()) {
        QAction* action = menu.addAction(QString::fromStdString(preset.title));
        action->setToolTip(QString::fromStdString(preset.description));
        action->setData(QString::fromStdString(preset.name));
    }
    QAction* picked = menu.exec(presets_->mapToGlobal(QPoint(0, presets_->height())));
    if (!picked) return;
    const std::string error = document_->applySimulatePreset(node_, picked->data().toString().toStdString());
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit message(tr("%1: %2 settings").arg(QString::fromStdString(node_), picked->text()));
    // Not under the menu's own click: the panel is built again on the next turn of the loop.
    refresh();
    emit edited();
}

void ParamPanel::buildActions() {
    for (auto& [id, b] : actionButtons_) {
        actionsGrid_->removeWidget(b);
        b->hide();
        b->deleteLater();  // (one may be the button whose click got here)
    }
    actionButtons_.clear();
    const bool shown = document_ && !node_.empty() && document_->nodeSpec(node_) && nodeActionsFiller_;
    if (shown) {
        for (const NodeAction& a : nodeActionsFiller_(node_)) {
            auto* b = new QPushButton(a.label, actions_);
            b->setObjectName(QStringLiteral("nodeAction_") + QString::fromStdString(a.id));
            b->setToolTip(a.tip);
            b->setEnabled(a.enabled);
            // After the click is over: the action may rebuild this panel (and this button).
            connect(b, &QPushButton::clicked, this, [run = a.run] {
                if (run) QTimer::singleShot(0, run);
            });
            const int at = static_cast<int>(actionButtons_.size());
            actionsGrid_->addWidget(b, at / 2, at % 2);
            actionButtons_.emplace_back(a.id, b);
        }
    }
    actions_->setVisible(!actionButtons_.empty());
}

std::vector<std::string> ParamPanel::actionIds() const {
    std::vector<std::string> ids;
    for (const auto& [id, b] : actionButtons_) ids.push_back(id);
    return ids;
}

QPushButton* ParamPanel::actionButton(const std::string& id) const {
    for (const auto& [i, b] : actionButtons_) {
        if (i == id) return b;
    }
    return nullptr;
}

QSize ParamPanel::sizeHint() const {
    // As tall as the rows (the host sizes the floating panel to this, within the view; the rows
    // scroll past that): the title, the description and the bypass box over the form.
    int height = 12;
    for (QWidget* w : {static_cast<QWidget*>(title_), static_cast<QWidget*>(actions_), static_cast<QWidget*>(bypass_), static_cast<QWidget*>(evaluation_),
                       static_cast<QWidget*>(empty_)}) {
        if (w->isVisibleTo(const_cast<ParamPanel*>(this))) height += w->sizeHint().height() + 6;
    }
    // The wrapped lines (the error, the description, the note) at the panel's width, every line.
    const int w = width() >= 212 && width() <= 432 ? width() - 12 : 300;  // (not laid out yet: the Node panel's)
    for (QLabel* label : {error_, description_, note_}) {
        if (label->isVisibleTo(const_cast<ParamPanel*>(this)) && !label->text().isEmpty()) height += label->heightForWidth(w) + 6;
    }
    if (scroll_->isVisibleTo(const_cast<ParamPanel*>(this))) height += formHost_->sizeHint().height() + 6;
    return QSize(300, height);
}

json ParamPanel::valueOf(const Row& row) const {
    if (row.ramp) return row.ramp->value();
    if (row.color) {
        const auto rgb = row.color->linear();
        return json::array({rgb[0], rgb[1], rgb[2]});
    }
    if (row.map && row.map->source() != MapSourceEditor::Source::Constant) return row.map->value();
    if (row.kind == "bool") return static_cast<QCheckBox*>(row.editors[0])->isChecked();
    if (row.kind == "int") return static_cast<std::int64_t>(std::lround(static_cast<ValueSlider*>(row.editors[0])->value()));
    if (row.kind == "float") return static_cast<ValueSlider*>(row.editors[0])->value() / row.factor;
    if (row.kind == "vec3") {
        json v = json::array();
        for (QWidget* w : row.editors) v.push_back(static_cast<QDoubleSpinBox*>(w)->value());
        return v;
    }
    if (row.kind == "string") {
        if (auto* combo = qobject_cast<QComboBox*>(row.editors[0])) return combo->currentText().toStdString();
        return static_cast<QLineEdit*>(row.editors[0])->text().toStdString();
    }
    return nullptr;
}

json ParamPanel::shownValue(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name) return valueOf(r);
    }
    return nullptr;
}

QString ParamPanel::description() const { return description_->isVisibleTo(const_cast<ParamPanel*>(this)) ? description_->text() : QString(); }

std::string ParamPanel::editorKind(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name) return r.editor;
    }
    return {};
}

QString ParamPanel::attributeNote(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name && r.attributeNote) return r.attributeNote->text();
    }
    return {};
}

QStringList ParamPanel::attributeChoices(const std::string& name) const {
    QStringList out;
    for (const Row& r : rows_) {
        if (r.name != name || r.editor != "attributeReads") continue;
        auto* combo = static_cast<QComboBox*>(r.editors[0]);
        for (int i = 0; i < combo->count(); ++i) out << combo->itemText(i);
    }
    return out;
}

RampEditor* ParamPanel::rampEditor(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name) return r.ramp;
    }
    return nullptr;
}

ColorEditor* ParamPanel::colorEditor(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name) return r.color;
    }
    return nullptr;
}

MapSourceEditor* ParamPanel::mapEditor(const std::string& name) const {
    for (const Row& r : rows_) {
        if (r.name == name) return r.map;
    }
    return nullptr;
}

void ParamPanel::refresh() {
    if (!document_ || node_.empty()) return;
    // Rebuilding keeps the sliders' ranges right and the map choices current (the form is
    // small), but not under the editor that asked: a refresh comes through the host from an
    // editor's own signal (a spin box's valueChanged, a key's drag), and deleting that editor
    // inside its handler is a use after free. So the rebuild waits for the event loop, and
    // while a drag is live (its editor must stay to finish it) the drag's end brings it.
    if (dragLive_ || sliderDown_ || rebuildPending_) return;
    rebuildPending_ = true;
    QTimer::singleShot(0, this, [this] {
        rebuildPending_ = false;
        if (!dragLive_ && !sliderDown_) build();
    });
}

void ParamPanel::rebuildNow() {
    if (!document_ || node_.empty()) return;
    build();
}

void ParamPanel::edit(const std::string& name, const json& value, bool record) {
    if (updating_ || !document_ || node_.empty()) return;
    const std::string error = document_->setParam(node_, name, value, record);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        refresh();
        return;
    }
    updateVisibility();  // rows shown only in some mode follow it at once
    emit edited();
}

}  // namespace groomlab::app
