// SPDX-License-Identifier: Apache-2.0
#pragma once

// The active tool's card in the Groom Tools (groom_tools.h), as Maya's Groom Tools show it
// (palette.py show_tool_settings): only the settings the tool uses (the core's brush catalog,
// brush_catalog.h, names them for every host), in Maya's order:
//
//   Radius (px), Strength (± for the tools that work both ways), Falloff, Smooth
//   Falloff curve, Root to tip                    (each a check box over its curve editor)
//   Alpha        the picker; Rotation, Angle, Spacing, Invert while one is picked  (Paint on a scalp map)
//   Selected only, Pen pressure, Keep off scalp, Front facing only   (two to a row)
//   NEW GUIDES   Match nearby guides, Length, Segments, Spacing, Solo new guides   (Place, Draw)
//   PARTS        Name, the parts drawn, Remove / Clear All                         (Part)
//   FLOW         Place (Arrows, Whorl...), Clear All, what a click or drag does     (Flow)
//   KEYS         Key scale, Key length, Scale or Trim, Delete Key, Clear, Cancel,
//                Apply                                                             (Length Keys)
//   Stretch                                                                         (Comb)
//   Paints       Width, Flatness, Twist (chips, choices.tubeChannel)                (Shape)
//   ERASES       Guides, Parts, Flow, and what Erase does not take                 (Erase)
//   ACTS ON      Guides, Parts, Flow                                               (the shaping brushes)
//   FREEZE MASK  Unfreeze All                                                      (Freeze)
//   the gestures (middle-drag: radius...)
//
// and the app's own: Propagate (the locked descendants edited too) with the brushes, Pointwise
// with Paint. Every edit goes straight into the session's settings; the viewport's own edits (a
// radius drag) come back through refresh. The tool's name and hint, the group, the symmetry and
// the strands while sculpting live around the card (groom_tools.h).

#include <functional>
#include <string>
#include <vector>

#include <QFrame>
#include <QString>

#include "definitions.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace groomlab {
class LengthKeyTool;
class StrokeHost;
struct BrushToolSettings;
}  // namespace groomlab

namespace groomlab::app {

class AlphaPicker;
class ValueSlider;
class BrushSession;

class BrushPanel : public QFrame {
    Q_OBJECT
public:
    explicit BrushPanel(QWidget* parent = nullptr);

    void setSession(BrushSession* session);  // null: nothing to edit
    // The tool whose settings show (a catalog id: comb, place, freeze, paint...; "select" for the
    // Select tool; "" none: nothing shows).
    void setTool(const std::string& id);
    const std::string& tool() const { return tool_; }
    // Whether the tool shows any setting (the Select tool's card is empty in the app).
    bool hasSettings() const { return hasSettings_; }
    // The editors take the settings' values again (after a radius drag, a scripted edit).
    void refresh();
    AlphaPicker* alphaPicker() const { return alpha_; }
    // The parts the Part brush drew (the Parts box's list) and the one picked ("" none).
    void setParts(const std::vector<std::string>& names);
    std::string selectedPart() const;
    // The Flow box's count of the arrows and sinks the groom keeps ("34 arrows · 2 sinks"; Clear All
    // off with none), and its words (every host's).
    void setFlowCount(std::size_t arrows, std::size_t sinks);
    static QString flowCountText(std::size_t arrows, std::size_t sinks);
    // The Parts box's buttons from lists.json's parts bar and the window's bindings (parts.remove,
    // parts.clear; an id without a binding is left out).
    void setBindings(const ui::Bindings& bindings);

signals:
    void settingsChanged();
    void guidesEdited();       // Unfreeze All changed the guides: the host evaluates and redraws
    void clearFlowRequested();  // the Flow box's Clear All
    void lengthsPreviewed();    // the Keys box changed the Length Keys preview: evaluate and redraw

private:
    // A slider row (a label, then the slider) bound to a float: its span `min`..`max`, a typed
    // value reaching `limitMin`..`limitMax`.
    ValueSlider* slider(QFormLayout* form, const QString& label, double min, double max, double limitMin, double limitMax,
                        int decimals, std::function<double()> get, std::function<void(double)> set, const QString& tip = {});
    QCheckBox* check(const QString& label, std::function<bool()> get, std::function<void(bool)> set, const QString& tip = {});
    QCheckBox* setting(const char* id, std::function<bool()> get, std::function<void(bool)> set);
    // A box of the card: a small heading (if any) over a column.
    QWidget* box(const QString& title, QVBoxLayout** column);
    // A brush curve of the catalog's (falloffProfile, strengthProfile): a check box over a curve
    // editor shown while it is ticked (starting at the keys of `start`, a ramp's JSON); `get` and
    // `set` are the settings' JSON ("" for none).
    QWidget* curve(const std::string& setting, const std::string& start, std::function<std::string()> get,
                   std::function<void(const std::string&)> set);
    void edited();  // a setting changed by hand: the host follows

    BrushSession* session_ = nullptr;
    std::string tool_;
    bool hasSettings_ = false;
    bool updating_ = false;
    std::vector<std::function<void()>> refreshers_;
    // The rows that come and go with the tool.
    QWidget* brushForm_ = nullptr;
    QLabel* radiusLabel_ = nullptr;
    ValueSlider* radius_ = nullptr;
    QLabel* strengthLabel_ = nullptr;
    ValueSlider* strength_ = nullptr;
    QLabel* falloffLabel_ = nullptr;
    ValueSlider* falloff_ = nullptr;
    QLabel* smoothLabel_ = nullptr;
    ValueSlider* smooth_ = nullptr;
    QWidget* falloffCurve_ = nullptr;
    QWidget* strengthCurve_ = nullptr;
    QWidget* alphaBox_ = nullptr;  // Paint on a scalp map: the alpha and its rows
    AlphaPicker* alpha_ = nullptr;
    QWidget* alphaOptions_ = nullptr;
    QComboBox* alphaRotation_ = nullptr;
    ValueSlider* alphaAngle_ = nullptr;
    ValueSlider* alphaSpacing_ = nullptr;
    QCheckBox* alphaInvert_ = nullptr;
    QGridLayout* checks_ = nullptr;
    QCheckBox* selectedOnly_ = nullptr;
    QCheckBox* pressure_ = nullptr;
    QCheckBox* collide_ = nullptr;
    QCheckBox* frontFacing_ = nullptr;
    QCheckBox* propagate_ = nullptr;
    QCheckBox* pointwise_ = nullptr;
    std::vector<QCheckBox*> checkOrder_;
    QWidget* createBox_ = nullptr;
    QLabel* spacingLabel_ = nullptr;
    ValueSlider* spacing_ = nullptr;
    QWidget* partBox_ = nullptr;
    QLineEdit* partName_ = nullptr;
    QListWidget* parts_ = nullptr;
    QWidget* partButtons_ = nullptr;
    // The Length Keys tool's box (lengthKeys); `lengthsEdit` runs an edit through the session
    // and tells the host (Apply: the guides changed, else the preview did).
    void lengthsEdit(const char* what, const std::function<void(LengthKeyTool&, StrokeHost&, const BrushToolSettings&)>& fn,
                     bool applies = false);
    QWidget* lengthsBox_ = nullptr;
    QDoubleSpinBox* lengthScale_ = nullptr;
    QDoubleSpinBox* lengthKey_ = nullptr;
    QComboBox* lengthChange_ = nullptr;
    QPushButton* lengthDeleteKey_ = nullptr;
    QPushButton* lengthClear_ = nullptr;
    QPushButton* lengthApply_ = nullptr;
    QPushButton* lengthCancel_ = nullptr;
    QLabel* lengthStatus_ = nullptr;
    QWidget* flowBox_ = nullptr;
    QComboBox* flowSink_ = nullptr;
    QLabel* flowHint_ = nullptr;
    QLabel* flowCount_ = nullptr;
    QPushButton* clearFlow_ = nullptr;
    QWidget* stretchBox_ = nullptr;
    QWidget* tubeChannelBox_ = nullptr;          // Shape: Paints Width / Flatness / Twist
    std::vector<QToolButton*> tubeChannels_;
    QWidget* eraseBox_ = nullptr;
    QWidget* targetBox_ = nullptr;
    QWidget* freezeBox_ = nullptr;
    QLabel* gestures_ = nullptr;
};

}  // namespace groomlab::app
