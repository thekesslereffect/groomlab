// SPDX-License-Identifier: Apache-2.0
#pragma once

// Groom Tools' Tubes section (groom_tools.h), Maya's palette.py TubePanel in C++: the tubes as a
// whole (opened by the Shape brush, Place and Draw). The targets' shape, width, flatness and twist
// (each change one undo step), the groom's shape library with its outline editor
// (tube_shape_editor.h), the shape new guides take, and the groom's Fill Tubes and Tube Blend (its
// Interpolate nodes' fillTubes and tubeBlend). Follows the groom through refresh().
//
// The targets: the selected guides, else the soloed ones (a brush's Shift+click on a guide:
// GroomDocument::tubeTargets). The shape picker lists the built-ins (choices.tubeShape) and the
// library; its choice (and Width, Flatness and Twist as shown) is also what Place and Draw give
// new guides (the brush session's placement tube: PlaceSettings::tube). The editor shows the
// chosen shape: a built-in read-only (Edit as Custom copies it into the library), a library shape
// editable: dragging previews live on the groom (no undo step), the release commits (one undo
// step, every guide taking it following). Rename is a name field in place, Delete arms on the
// first click and deletes on a second: no dialog.

#include <optional>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "groomlab/guide_hierarchy.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace groomlab::app {

class BrushSession;
class GroomDocument;
class TubeShapeEditor;
class ValueSlider;

class TubePanel : public QWidget {
    Q_OBJECT
public:
    explicit TubePanel(QWidget* parent = nullptr);

    void setSession(BrushSession* session);  // its document is the groom (null: none)
    // The targets' tubes, the library and Fill Tubes read again. Left alone while the artist is
    // busy in it (busy()); `force` (after the panel's own edit): only not mid-drag.
    void refresh(bool force = false);
    bool busy() const;

    // ---- State (tests) -----------------------------------------------------------------------
    const std::string& choice() const { return choice_; }  // the shape picked: the targets', else the last picked
    const std::vector<GuideId>& targets() const { return targets_; }
    const std::string& source() const { return source_; }  // "selection", "solo" or "none"
    bool libraryShape(const std::string& name) const;
    // The placement tube (what Place and Draw give new guides) as JSON: {"shape", "width", "aspect", "twist"}.
    std::string placement() const;

    // ---- The edits, as the controls do them --------------------------------------------------
    void pick(const std::string& name);     // the picker: the targets take it, and new guides
    void applyShape();                      // Apply to Selected
    void setWidth(double shown);            // the targets scaled so the first one's widest section is this
    // Width's unit again (Edit > Units: the lengths' unit; its label "Width (mm)", the slider in it).
    void refreshUnits();
    QString widthCaption() const;  // "Width (mm)": the field's caption, in the length unit chosen
    void setFlatness(double flatness);      // 0 as drawn .. 0.99 a flat ribbon
    void setTwist(double degrees);
    void smoothTargets();                   // Smooth: the current channel (the brush's) or all
    void editAsCustom();
    void newShape();
    void duplicateShape();
    void startRename();
    void finishRename();                    // the name field's Enter
    void cancelRename();                    // Esc
    void deleteShape();                     // the first click arms, a second one deletes
    void setFill(bool on);
    void setBlend(double percent);
    void setShapeSmooth(bool on);           // the library shape's Smooth
    void setMirror(bool on);

    // ---- The pieces (tests, screenshots) ------------------------------------------------------
    QComboBox* shapeBox() const { return shape_; }
    TubeShapeEditor* editor() const { return editor_; }
    QLabel* targetsLabel() const { return targetsLabel_; }
    QPushButton* editAsCustomButton() const { return editCustom_; }
    QPushButton* newButton() const { return new_; }
    QPushButton* renameButton() const { return rename_; }
    QPushButton* duplicateButton() const { return duplicate_; }
    QPushButton* deleteButton() const { return delete_; }
    QPushButton* applyButton() const { return apply_; }
    QPushButton* smoothButton() const { return smooth_; }
    QComboBox* smoothChannels() const { return smoothWhat_; }
    QCheckBox* mirrorBox() const { return mirror_; }
    QCheckBox* shapeSmoothBox() const { return shapeSmooth_; }
    QLineEdit* renameField() const { return renameField_; }
    ValueSlider* widthSlider() const { return width_; }
    ValueSlider* flatnessSlider() const { return flatness_; }
    ValueSlider* twistSlider() const { return twist_; }
    QCheckBox* fillBox() const { return fill_; }
    ValueSlider* blendSlider() const { return blend_; }

signals:
    void guidesEdited();     // the tubes or the library changed (one undo step): the host evaluates
    void graphEdited();      // Fill Tubes or Tube Blend changed the graph (one undo step)
    void previewed();        // a drag's live preview (no step): the host evaluates and redraws
    void settingsChanged();  // the placement tube changed (the brush session's settings)
    void message(const QString& text);  // what went wrong, or what was made ("" clears)

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    GroomDocument* document() const;
    // A slider whose value is committed when it settles (a release, a typed value, a wheel step).
    ValueSlider* committingSlider(double min, double max, double limitMin, double limitMax, int decimals,
                                  void (TubePanel::*commit)(double));
    void fillShapes();           // the picker: Mixed (while the targets differ), the built-ins, the library
    void showChoice(bool mixed = false);
    void pushPlacement();        // the placement tube to the brush session when it changed
    void edited(const std::string& error);  // after a guides edit: said, read again, the host told
    std::string uniqueName(const std::string& base) const;
    void disarmDelete();
    void sendPreview();
    void shapeCommitted(const std::vector<SectionPoint>& points);

    BrushSession* session_ = nullptr;
    bool updating_ = false;
    // The targets and the library as last read.
    std::vector<GuideId> targets_;
    std::string source_ = "none";
    std::vector<std::string> shapes_;   // the targets' shapes
    float reference_ = 0.0f;            // the first target's widest section (scene units)
    std::string choice_ = "round";
    struct LibraryShape {
        std::string name;
        std::vector<SectionPoint> points;
        bool smooth = true;
        std::size_t users = 0;
    };
    std::vector<LibraryShape> library_;
    const LibraryShape* library(const std::string& name) const;
    std::string targetsKey_;     // the targets and their shapes as last read
    std::string shapesKey_;      // what the picker lists
    std::string pushed_;         // the placement tube last pushed
    bool previewing_ = false;
    bool armed_ = false;

    QLabel* targetsLabel_ = nullptr;
    QComboBox* shape_ = nullptr;
    TubeShapeEditor* editor_ = nullptr;
    QPushButton* editCustom_ = nullptr;
    QCheckBox* mirror_ = nullptr;
    QCheckBox* shapeSmooth_ = nullptr;
    QPushButton* new_ = nullptr;
    QPushButton* rename_ = nullptr;
    QPushButton* duplicate_ = nullptr;
    QPushButton* delete_ = nullptr;
    QLineEdit* renameField_ = nullptr;
    QPushButton* apply_ = nullptr;
    ValueSlider* width_ = nullptr;
    double lengthFactor_ = 1.0;  // Width shown per scene unit (refreshUnits)
    ValueSlider* flatness_ = nullptr;
    ValueSlider* twist_ = nullptr;
    QPushButton* smooth_ = nullptr;
    QComboBox* smoothWhat_ = nullptr;
    QCheckBox* fill_ = nullptr;
    ValueSlider* blend_ = nullptr;
    QTimer* disarm_ = nullptr;
    QTimer* preview_ = nullptr;
    std::vector<ValueSlider*> sliders_;
    std::vector<ValueSlider*> held_;  // the sliders being dragged
};

}  // namespace groomlab::app
