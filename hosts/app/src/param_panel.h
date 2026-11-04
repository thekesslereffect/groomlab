// SPDX-License-Identifier: Apache-2.0
#pragma once

// The parameter panel: one node's params as editors made from the operator's spec (a check box,
// a spin box, a slider with a spin box, a dropdown of the choices, three numbers for a vector,
// a swatch and picker for a color, a curve editor for a ramp, and for a mappable param the
// source it reads: a value, a painted map, a texture, a color set, an expression or a field
// port; param_editors.h), each edit going to the document (and so to the session, live) with a
// slider's or a key's drag as one undo step. A map set on a param is never lost by editing:
// the value's editor shows only while the source is a value.
//
// The rows follow the spec's layout hints (graph.h ParamSpec): with two groups or more, each group
// under a header that folds it (the main params first, without one); every advanced param in an
// Advanced section at the bottom, folded; a row whose showWhen (a param's value, that param's row
// shown too) does not hold hidden (and shown
// again as soon as an edit makes it hold, without a rebuild). Folds are kept per operator type for
// the session. A number shows its unit (a fraction as a percentage, degrees, turns, seconds,
// pixels); the stored value stays the spec's. A node saved with an older behaviour of its operator
// shows Upgrade beside its title.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <QStringList>
#include <QWidget>

#include <nlohmann/json.hpp>

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QMenu;
class QScrollArea;
class QVBoxLayout;

namespace groomlab::app {

class GroomDocument;
class RampEditor;
class ColorEditor;
class MapSourceEditor;
class RoundButton;
class ValueSlider;

// How a number param shows (param_panel.cpp): the value times `factor` (a fraction as a
// percentage), `decimals` at most (trailing zeros dropped), `suffix` after it ("%", "°", " px").
// `specDecimals`: the spec's "decimals" (-1: none given).
struct DisplayUnit {
    double factor = 1.0;
    QString suffix;
    int decimals = 3;
};
DisplayUnit displayUnit(const std::string& unit, bool integer, int specDecimals = -1);
// Real units for the scene's measures (Maya's groomlab.units): lengths, widths and densities (and a
// rate per length) shown in the units picked in the Graph Editor's Edit > Units, saved per user
// (QSettings units/<kind>), the groom's scene units being centimeters as Maya's ("scene": bare, as
// stored: the default). `kind` "length", "width" or "density".
struct UnitChoice {
    std::string key, label;  // "mm", "mm"
    double factor;           // shown per scene unit
};
std::vector<UnitChoice> unitChoices(const std::string& kind);
std::string unitChoice(const std::string& kind);
void setUnitChoice(const std::string& kind, const std::string& key);

class ParamPanel : public QWidget {
    Q_OBJECT
public:
    explicit ParamPanel(QWidget* parent = nullptr);

    void setDocument(GroomDocument* document);  // null: nothing to edit
    void showNode(const std::string& node);     // builds the editors for it ("": none)
    // The node's note from the last evaluation (it does nothing as set) as a grey line under the
    // description; hidden without one.
    // The node's error from the last evaluation (GroomDocument::nodeErrors) as a red line under
    // its title too; hidden without one.
    void refreshNote();
    QString note() const;
    QString error() const;
    const std::string& node() const { return node_; }
    // The editors take the document's values again (after undo and redo): rebuilt on the next
    // turn of the event loop (never under an editor's own signal), once no drag is live.
    void refresh();
    void rebuildNow();  // the same at once (tests; never from an editor's handler)
    // A param's right-click menu (a param that takes an input port): the host fills it (the
    // shared definitions' paramMenu, AppActions::fillParamMenu); without a filler, Expose as
    // Input alone. fillParamMenu fills `menu` for the shown node's param without showing it.
    using ParamMenuFiller = std::function<void(QMenu* menu, const std::string& node, const std::string& param)>;
    void setParamMenuFiller(ParamMenuFiller filler) { paramMenuFiller_ = std::move(filler); }
    void fillParamMenu(QMenu* menu, const std::string& param);
    // The node's actions by its type (graph_ui.json's nodeActions: the Cards exports, Simulate's
    // range, bake and clear, Bake Node to Guides...), two to a row under its title, over the rows'
    // scroll area: in view however far the params scroll. The host gives them (AppActions::nodeActions);
    // without a filler, none.
    struct NodeAction {
        std::string id;
        QString label, tip;
        std::function<void()> run;
        bool enabled = true;
    };
    using NodeActionsFiller = std::function<std::vector<NodeAction>(const std::string& node)>;
    void setNodeActionsFiller(NodeActionsFiller filler) { nodeActionsFiller_ = std::move(filler); }
    std::vector<std::string> actionIds() const;           // the shown node's, in order
    QPushButton* actionButton(const std::string& id) const;  // null: none
    QWidget* actionsBar() const { return actions_; }
    // As tall as the node's rows (the floating panel takes this height within the view).
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return QSize(200, 120); }
    // The description wraps, which would make the host's layout ask a height for a width (the
    // scroll area's small hint) instead of sizeHint: the hint is the answer.
    bool hasHeightForWidth() const override { return false; }

protected:
    void resizeEvent(QResizeEvent* event) override;  // the wrapped lines keep their height

public:
    // What the panel says of the node: its description (the operator's, as Maya's Attribute
    // Editor shows it), "" with no node.
    QString description() const;

    // ---- What the panel shows (tests, scripted checks) -------------------------------------
    // The value the row's editors hold for a param, as the document stores it (null: no row).
    nlohmann::json shownValue(const std::string& name) const;
    // The editors of a param: "ramp", "color", "map" (with the value's editor inside), "float",
    // "int", "bool", "enum", "string", "vec3", or "" without a row.
    std::string editorKind(const std::string& name) const;
    RampEditor* rampEditor(const std::string& name) const;
    ColorEditor* colorEditor(const std::string& name) const;
    MapSourceEditor* mapEditor(const std::string& name) const;
    ValueSlider* numberSlider(const std::string& name) const;  // a float's or int's slider (null: none)
    // Whether a param's row shows (its section open, its showWhen holding); false without a row.
    bool rowShown(const std::string& name) const;
    // The section a param's row is in: "" (the main params, no header), a group or "Advanced".
    std::string sectionOf(const std::string& name) const;
    // Folds (or opens) a section of every node of the shown node's type, for the session (the
    // section header's click).
    void setFolded(const std::string& section, bool folded);
    bool folded(const std::string& section) const;
    // An attribute param's line under its field (a write overwriting an upstream node's, a read that
    // does not reach the node), "" without one; and a reading param's choices as listed
    // ("clumpId (from clump1)"), the attributes reaching the node.
    QString attributeNote(const std::string& name) const;
    QStringList attributeChoices(const std::string& name) const;

signals:
    // The document's graph changed through this panel: the host evaluates and redraws.
    void edited();
    // The row's Paint action on a mappable param: the host makes a vertex map named after the
    // node and param, wires it here and picks the Paint brush on it (GroomDocument::paintParam).
    void paintParamRequested(const std::string& node, const std::string& name);
    // A slider's drag started or ended (the viewport draws its frames reduced meanwhile).
    void dragging(bool on);
    void message(const QString& text);

private:
    struct Row {
        std::string name, kind;         // the spec's kind
        std::string editor;             // editorKind
        std::vector<QWidget*> editors;  // the widgets holding the value (three for a vec3)
        RampEditor* ramp = nullptr;
        ColorEditor* color = nullptr;
        MapSourceEditor* map = nullptr;
        QWidget* host = nullptr;        // the row (its label and editor)
        std::string section;            // sectionOf
        nlohmann::json showWhen;        // the spec's {"param", "in"} (null: always)
        double factor = 1.0;            // what the slider shows per stored unit (a percentage: 100)
        std::string unit;               // the unit it shows (the spec's while its unitWhen holds)
        nlohmann::json unitWhen;        // the spec's unitWhen (null: none)
        QLabel* attributeNote = nullptr;  // an attribute param's warning or note line
    };
    struct Section {
        std::string name;
        QAbstractButton* header = nullptr;  // null for the main params
    };
    void build();
    void clearForm();
    void fitWrappedLabels();
    // Shows the rows whose showWhen holds in open sections, and the headers of sections with a row
    // shown: after a build and after each edit (no rebuild).
    void updateVisibility();
    void titleSection(QAbstractButton* header, const std::string& name) const;
    void upgrade();  // the Upgrade button: one undo step
    void showPresets();  // the Presets button (a Simulate node): its menu of the core's presets
    // A float or int: one slider (glass.h's ValueSlider) captioned `caption`, its number edited inline.
    QWidget* numberEditor(Row& row, const nlohmann::json& spec, const nlohmann::json& value, const QString& caption);
    // A param naming an attribute: "Writes" with a plain field (and a warning when it overwrites an
    // upstream node's), or "Reads" with the attributes reaching the node to pick from (any name
    // typed too; a note when the one named does not reach). `channels`: the node's graphChannels entry.
    QWidget* attributeEditor(Row& row, const nlohmann::json& spec, const nlohmann::json& value, const nlohmann::json& channels);
    nlohmann::json valueOf(const Row& row) const;  // what the editors show
    void edit(const std::string& name, const nlohmann::json& value, bool record);

    GroomDocument* document_ = nullptr;
    std::string node_;
    QLabel* title_ = nullptr;
    RoundButton* upgrade_ = nullptr;  // beside the title for an outdated node
    RoundButton* presets_ = nullptr;  // beside the title for a Simulate node (simulatePresets)
    QLabel* description_ = nullptr;
    QLabel* note_ = nullptr;  // the node's note (refreshNote)
    QLabel* error_ = nullptr;  // the node's error (refreshNote)
    QLabel* empty_ = nullptr;  // "No node selected", in place of the rows
    QLabel* emptyIcon_ = nullptr;  // its icon over it
    QCheckBox* bypass_ = nullptr;
    QComboBox* evaluation_ = nullptr;  // when the node runs
    QWidget* actions_ = nullptr;        // the node's actions (NodeAction), over the scroll area
    QGridLayout* actionsGrid_ = nullptr;
    std::vector<std::pair<std::string, QPushButton*>> actionButtons_;
    QScrollArea* scroll_ = nullptr;
    // The rows: a name beside a small editor, or a name over a wide one (a ramp, a color, a map source).
    QVBoxLayout* list_ = nullptr;
    QWidget* formHost_ = nullptr;
    std::vector<Row> rows_;
    std::vector<Section> sections_;
    std::string type_;  // the shown node's operator type (the folds are kept by it)
    bool updating_ = false;
    nlohmann::json dragStart_;  // a slider's or key's drag: the value before it started
    bool dragLive_ = false;     // ... and whether one is on (the first live edit keeps the start)
    bool sliderDown_ = false;   // a slider's drag is on
    bool rebuildPending_ = false;
    ParamMenuFiller paramMenuFiller_;
    NodeActionsFiller nodeActionsFiller_;
    void buildActions();  // the shown node's actions (none: the bar hidden)
};

}  // namespace groomlab::app
