// SPDX-License-Identifier: Apache-2.0
#pragma once

// The active tool's card in the Groom Tools (groom_tools.h), as the Simple layout lays it out
// (research/ui-redesign/SIMPLE.md, Maya's palette.py; the core's brush catalog, brush_catalog.h,
// says which settings each tool uses). Always the same blocks in the same order, a block only
// when it has rows:
//
//   header       the tool's icon and name; on the right what the next stroke edits ("● primary on
//                Base groom", the layer in the accent; Paint: "on density"): setEditing. The
//                tool's hint is the title's tooltip
//   Paint        the maps panel's Mode, Value + Fill and color (setPaintControls)
//   Brush        Radius (px), Strength (± for the tools that work both ways)
//   toggles      one chip row: ⇆ Mirror first, Pressure, Keep off scalp, Stretch (Comb),
//                Straighten (Smooth), Whole faces (Paint)
//   the tool's own block, named for what it sets:
//     New guides   Length, Segments, Match nearby (Place, Draw)
//     Paints       Width, Flatness, Twist (Tube: over the brush, untitled)
//     Erases       Guides, Parts, Flow
//     Keys         Key scale, Key length, Guides Scale / Trim, Delete key, Clear, Cancel, Apply (Length Keys)
//     Flow         Place (Arrows, Whorl...), the count, Clear all
//     Clone        Pick Source, Reset, Replace
//     Parts        Name, the parts drawn, Remove / Clear
//   the tubes    tube_panel.h's blocks (setTubes): the levels' row right under the header (Tube and
//                Roots), the Tube brush's Profile and Selected tubes, the Roots brush's root block;
//                Place's and Draw's new-guide tube is in More
//   More settings  folded: Falloff, Smooth, Mirror across (and the world plane's center), Applies
//                to (Guides, Parts, Flow), the curves (a chip that is on shows its ramp), the stamp
//                (Paint on a scalp map), Spacing, Solo new, the app's Propagate and Pointwise, the
//                new guides' tube
//
// Selected only and Front only are not in the card: the brushes keep their defaults (off and on).
// Every edit goes straight into the session's settings; the viewport's own edits (a radius drag)
// come back through refresh.

#include <functional>
#include <string>
#include <vector>

#include <QFrame>
#include <QString>

#include "definitions.h"
#include "tube_panel.h"

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace groomlab {
class LengthKeyTool;
class StrokeHost;
struct BrushToolSettings;
}  // namespace groomlab

namespace groomlab::app {

class AlphaPicker;
class BrushSession;
class CardBlock;
class FlowLayout;
class Section;
class TubePanel;
class ValueSlider;

class BrushPanel : public QFrame {
    Q_OBJECT
public:
    explicit BrushPanel(QWidget* parent = nullptr);

    void setSession(BrushSession* session);  // null: nothing to edit
    void setSubtitle(const QString& text);   // a second header line (none in the Simple layout: "" hides it)
    // The header's right: what the next stroke edits (rich text); its plain words (tests).
    void setEditing(const QString& richText);
    QString editing() const;
    void setTitleTip(const QString& tip);    // the title's tooltip: what the tool does (the catalog's hint)
    // The maps panel's Whole Faces switch (it carries the setting): shown as the toggles row's chip
    // for Paint, its own row hidden.
    void setWholeFaces(QCheckBox* box);
    // The tool whose card shows (a catalog id: comb, place, freeze, paint, select...; "" none).
    void setTool(const std::string& id);
    const std::string& tool() const { return tool_; }
    bool hasSettings() const { return hasSettings_; }  // a tool is set
    // The editors take the settings' values again (after a radius drag, a scripted edit).
    void refresh();
    AlphaPicker* alphaPicker() const { return alpha_; }
    // The parts the Part brush drew (the Parts block's list) and the one picked ("" none).
    void setParts(const std::vector<std::string>& names);
    std::string selectedPart() const;
    // The Flow block's count of the arrows and sinks the groom keeps ("34 arrows · 2 sinks"; Clear
    // all off with none), and its words (every host's).
    void setFlowCount(std::size_t arrows, std::size_t sinks);
    static QString flowCountText(std::size_t arrows, std::size_t sinks);
    // The Parts block's buttons from lists.json's parts bar and the window's bindings (parts.remove,
    // parts.clear; an id without a binding is left out).
    void setBindings(const ui::Bindings& bindings);
    // The tubes' blocks (tube_panel.h), placed after the tool's own block: Tube for Place and
    // Draw, Profile and Selected guides for Shape and Select.
    void setTubes(TubePanel* tubes);
    // The Paint block's first rows: the maps panel's Mode, Value + Fill and color (maps_panel.h).
    void setPaintControls(QWidget* controls);

    // ---- The pieces (tests, screenshots) -------------------------------------------------------
    QAbstractButton* symmetryBox() const;  // the toggles row's ⇆ Mirror chip
    QComboBox* symmetryMode() const { return symmetryMode_; }
    // The titles of the blocks shown, top to bottom (Brush, the tool's own, the tubes', Applies to).
    std::vector<std::string> blockTitles() const;

signals:
    void settingsChanged();
    void guidesEdited();        // Unfreeze All changed the guides: the host evaluates and redraws
    void clearFlowRequested();  // the Flow block's Clear all
    void lengthsPreviewed();    // the Keys block changed the Length Keys preview: evaluate and redraw

private:
    // A widget's writer of setting `id` (the editor's setToolSetting; `whole`: a whole number).
    std::function<void(double)> numberSetter(const char* id, bool whole = false);
    std::function<void(bool)> switchSetter(const char* id);
    QLabel* toolIcon_ = nullptr;   // the card's header: the tool's icon and name
    QLabel* toolTitle_ = nullptr;
    QLabel* toolHint_ = nullptr;   // under the title (setSubtitle: none in the Simple layout)
    QLabel* editing_ = nullptr;    // the header's right: what the next stroke edits
    // A fill field (the name inside) bound to a value of the catalog's (BrushSettingSpec `id`): its
    // span, what a typed value may reach and its decimals are the spec's, in what the field shows
    // (`shown` per unit of the spec's: 100 for a fraction shown in percent). A length's follow the
    // groom's size (lengthScale).
    ValueSlider* slider(QVBoxLayout* into, const char* id, const QString& label, std::function<double()> get,
                        std::function<void(double)> set, const QString& tip = {}, double shown = 1.0);
    // What a length of the catalog's is times on this groom: its scalp's size over the size the
    // specs are written for (kDefaultsScalpSize), to the nearest power of ten; 1 without a scalp.
    double lengthScale() const;
    // A chip bound to a bool.
    QToolButton* chip(FlowLayout* into, const QString& label, std::function<bool()> get, std::function<void(bool)> set,
                      const QString& tip = {});
    // A toggle as a switch row (a field row, its switch at the end), the catalog's setting `id`'s hint.
    QCheckBox* switchSetting(QVBoxLayout* into, const QString& label, std::function<bool()> get, std::function<void(bool)> set,
                             const QString& tip = QString());
    QCheckBox* settingSwitch(QVBoxLayout* into, const char* id, const QString& label, std::function<bool()> get, std::function<void(bool)> set);
    static void setRowVisible(QWidget* control, bool visible);
    static QWidget* rowOf(QWidget* control);  // a switch row's frame, else the control
    // A brush curve of the catalog's (falloffProfile, strengthProfile): a chip, and a ramp editor
    // shown under the chips while it is on (starting at the keys of `start`, a ramp's JSON); `get`
    // and `set` are the settings' JSON ("" for none).
    void curve(FlowLayout* chips, QVBoxLayout* editors, const std::string& setting, const QString& label, const std::string& start,
               std::function<std::string()> get, std::function<void(const std::string&)> set);
    void refreshSymmetry();
    void symmetryEdited();
    void edited();  // a setting changed by hand: the host follows

    BrushSession* session_ = nullptr;
    std::string tool_;
    bool hasSettings_ = false;
    bool updating_ = false;
    std::vector<std::function<void()>> refreshers_;
    QVBoxLayout* card_ = nullptr;
    // Brush.
    CardBlock* brush_ = nullptr;
    ValueSlider* radius_ = nullptr;
    ValueSlider* strength_ = nullptr;
    ValueSlider* falloff_ = nullptr;
    ValueSlider* smooth_ = nullptr;
    // The toggles row and More settings.
    QWidget* toggles_ = nullptr;
    FlowLayout* togglesFlow_ = nullptr;
    QWidget* moreBody_ = nullptr;
    QVBoxLayout* moreRows_ = nullptr;
    Section* more_ = nullptr;
    QWidget* curveChips_ = nullptr;
    QToolButton* pressure_ = nullptr;
    QToolButton* collide_ = nullptr;
    QToolButton* stretch_ = nullptr;
    QToolButton* straighten_ = nullptr;
    QToolButton* wholeFaces_ = nullptr;
    QCheckBox* wholeFacesBox_ = nullptr;
    QCheckBox* soloNew_ = nullptr;
    QToolButton* falloffCurve_ = nullptr;
    QToolButton* strengthCurve_ = nullptr;
    QCheckBox* propagate_ = nullptr;
    QCheckBox* pointwise_ = nullptr;
    QWidget* falloffRamp_ = nullptr;
    QWidget* strengthRamp_ = nullptr;
    // Paint.
    CardBlock* paint_ = nullptr;
    QVBoxLayout* paintSlot_ = nullptr;
    QWidget* alphaBox_ = nullptr;  // Paint on a scalp map: the stamp and its rows
    AlphaPicker* alpha_ = nullptr;
    QWidget* alphaOptions_ = nullptr;
    QComboBox* alphaRotation_ = nullptr;
    ValueSlider* alphaAngle_ = nullptr;
    ValueSlider* alphaSpacing_ = nullptr;
    // New guides.
    CardBlock* newGuides_ = nullptr;
    ValueSlider* spacing_ = nullptr;
    QCheckBox* placeCollide_ = nullptr;
    // The others.
    CardBlock* paints_ = nullptr;
    std::vector<QToolButton*> tubeChannels_;
    CardBlock* evens_ = nullptr;  // Smooth: what it evens out (choices.smoothMode)
    std::vector<QToolButton*> smoothModes_;
    CardBlock* erases_ = nullptr;
    CardBlock* parts_ = nullptr;
    QLineEdit* partName_ = nullptr;
    QListWidget* partList_ = nullptr;
    QWidget* partButtons_ = nullptr;
    QWidget* flowButtons_ = nullptr;  // the Flow block's Mirror buttons (lists/flow/mirror)
    // The Length Keys tool's block (lengthKeys); `lengthsEdit` runs an edit through the session
    // and tells the host (Apply: the guides changed, else the preview did).
    void lengthsEdit(const char* what, const std::function<void(LengthKeyTool&, StrokeHost&, const BrushToolSettings&)>& fn,
                     bool applies = false);
    CardBlock* keys_ = nullptr;
    ValueSlider* lengthScale_ = nullptr;
    ValueSlider* lengthKey_ = nullptr;
    std::vector<QToolButton*> lengthChanges_;
    QPushButton* lengthDeleteKey_ = nullptr;
    QPushButton* lengthClear_ = nullptr;
    QPushButton* lengthApply_ = nullptr;
    QPushButton* lengthCancel_ = nullptr;
    QLabel* lengthStatus_ = nullptr;
    CardBlock* flow_ = nullptr;
    QComboBox* flowSink_ = nullptr;
    QLabel* flowHint_ = nullptr;
    QLabel* flowCount_ = nullptr;
    QPushButton* clearFlow_ = nullptr;
    // Clone (issue 91): Pick Source (the next click captures), Reset, what the source holds.
    CardBlock* clone_ = nullptr;
    QPushButton* clonePick_ = nullptr;
    QPushButton* cloneReset_ = nullptr;
    QLabel* cloneCount_ = nullptr;
    // The tubes' blocks (another panel's).
    QVBoxLayout* tubeSlot_ = nullptr;
    TubePanel* tubes_ = nullptr;
    // Applies to.
    CardBlock* applies_ = nullptr;
    QWidget* actsOn_ = nullptr;
    QWidget* mirrorRow_ = nullptr;
    QToolButton* symmetry_ = nullptr;
    QComboBox* symmetryMode_ = nullptr;
    ValueSlider* symmetryCenter_ = nullptr;
};

}  // namespace groomlab::app
