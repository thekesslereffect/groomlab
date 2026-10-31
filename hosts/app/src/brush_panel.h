// SPDX-License-Identifier: Apache-2.0
#pragma once

// The active tool's card in the Groom Tools (groom_tools.h), as the redesign lays it out
// (research/ui-redesign/QT.md, "Groom Tools layout"; the core's brush catalog, brush_catalog.h,
// says which settings each tool uses). Always the same blocks in the same order, a block only
// when it has rows:
//
//   Brush        Radius (px), Strength (± for the tools that work both ways), Falloff, Smooth (Paint);
//                chips Pressure, Off scalp, Falloff curve, Root to tip (a curve chip that is on shows
//                its ramp under the chips), and the app's own Propagate (the shaping brushes) and
//                Pointwise (Paint)
//   the tool's own block, named for what it sets:
//     Paint        the maps panel's Mode, Value + Fill and color (setPaintControls), the alpha stamp
//     New guides   Length, Segments, Spacing (Place); Match nearby, Solo new, Off scalp
//     Comb         Stretch
//     Paints       Width, Flatness, Twist (Shape)
//     Erases       Guides, Parts, Flow, a short note
//     Keys         Key scale, Key length, Guides Scale / Trim, Delete key, Clear, Cancel, Apply (Length Keys)
//     Flow         Place (Arrows, Whorl...), the count, Clear all
//     Parts        Name, the parts drawn, Parts from Curves / Remove / Clear
//   the tubes    tube_panel.h's blocks (setTubes): Tube (Place, Draw), Profile and Selected guides
//                (Shape, Select)
//   Applies to   Acts on (Guides, Parts, Flow) where the tool has targets; Mirror (Symmetry and
//                across what, the world plane's center) for the tools that mirror
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
class TubePanel;
class ValueSlider;

class BrushPanel : public QFrame {
    Q_OBJECT
public:
    explicit BrushPanel(QWidget* parent = nullptr);

    void setSession(BrushSession* session);  // null: nothing to edit
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
    QCheckBox* symmetryBox() const { return symmetry_; }
    QComboBox* symmetryMode() const { return symmetryMode_; }
    // The titles of the blocks shown, top to bottom (Brush, the tool's own, the tubes', Applies to).
    std::vector<std::string> blockTitles() const;

signals:
    void settingsChanged();
    void guidesEdited();        // Unfreeze All changed the guides: the host evaluates and redraws
    void clearFlowRequested();  // the Flow block's Clear all
    void lengthsPreviewed();    // the Keys block changed the Length Keys preview: evaluate and redraw

private:
    // A fill field (the name inside) bound to a float: its span `min`..`max`, a typed value
    // reaching `limitMin`..`limitMax`.
    ValueSlider* slider(QVBoxLayout* into, const QString& label, double min, double max, double limitMin, double limitMax, int decimals,
                        std::function<double()> get, std::function<void(double)> set, const QString& tip = {});
    // A chip bound to a bool.
    QToolButton* chip(FlowLayout* into, const QString& label, std::function<bool()> get, std::function<void(bool)> set,
                      const QString& tip = {});
    QToolButton* settingChip(FlowLayout* into, const char* id, const QString& label, std::function<bool()> get, std::function<void(bool)> set);
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
    QToolButton* pressure_ = nullptr;
    QToolButton* collide_ = nullptr;
    QToolButton* falloffCurve_ = nullptr;
    QToolButton* strengthCurve_ = nullptr;
    QToolButton* propagate_ = nullptr;
    QToolButton* pointwise_ = nullptr;
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
    QToolButton* placeCollide_ = nullptr;
    // The others.
    CardBlock* comb_ = nullptr;
    CardBlock* paints_ = nullptr;
    std::vector<QToolButton*> tubeChannels_;
    CardBlock* erases_ = nullptr;
    CardBlock* parts_ = nullptr;
    QLineEdit* partName_ = nullptr;
    QListWidget* partList_ = nullptr;
    QWidget* partButtons_ = nullptr;
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
    // The tubes' blocks (another panel's).
    QVBoxLayout* tubeSlot_ = nullptr;
    TubePanel* tubes_ = nullptr;
    // Applies to.
    CardBlock* applies_ = nullptr;
    QWidget* actsOn_ = nullptr;
    QWidget* mirrorRow_ = nullptr;
    QCheckBox* symmetry_ = nullptr;
    QComboBox* symmetryMode_ = nullptr;
    ValueSlider* symmetryCenter_ = nullptr;
};

}  // namespace groomlab::app
