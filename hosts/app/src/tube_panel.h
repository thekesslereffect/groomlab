// SPDX-License-Identifier: Apache-2.0
#pragma once

// The tube blocks of the Groom Tools' tool card (brush_panel.h), Maya's palette.py TubePanel in C++:
// the targets' shape, width, flatness and twist (each change one undo step), the groom's shape
// library with its outline editor (tube_shape_editor.h) and the shape new guides take. Follows the
// groom through refresh(). Two layouts (setMode):
//
//   New    Place and Draw: "Tube", a small preview of the profile picked, the picker, Width,
//          Flatness and Twist (what new guides get; profiles are edited with the Shape brush)
//   Edit   Shape and Select: "Profile" (the picker with Apply, the outline editor, Edit as custom,
//          Mirror, Smooth; New / Rename / Duplicate / Delete glyphs in its header) and "Selected
//          guides" (Width, Flatness, Twist; Smooth in its header)
//
// Fill Tubes and Tube Blend are the Interpolate nodes' settings (fillTubes, tubeBlend: the Graph
// Editor's node settings).
//
// The targets: the selected guides, else the soloed ones (a brush's Shift+click on a guide:
// GroomDocument::tubeTargets). The shape picker lists the built-ins (choices.tubeShape) and the
// library; its choice (and Width, Flatness and Twist as shown) is also what Place and Draw give
// new guides (the brush session's placement tube: PlaceSettings::tube). The editor shows the
// chosen shape: a built-in read-only (Edit as Custom copies it into the library), a library shape
// editable: dragging previews live on the groom (no undo step), the release commits (one undo
// step, every guide taking it following). Rename is a name field in place, Delete arms on the
// first click and deletes on a second: no dialog.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/root_tube_tool.h"

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace groomlab::app {

class BrushSession;
class CardBlock;
class GroomDocument;
class TubeShapeEditor;
class ValueSlider;

class TubePanel : public QWidget {
    Q_OBJECT
public:
    explicit TubePanel(QWidget* parent = nullptr);

    enum class Mode { New, Edit };
    void setMode(Mode mode);  // Edit to start with
    Mode mode() const { return mode_; }

    void setSession(BrushSession* session);  // its document is the groom (null: none)
    // The targets' tubes, the library and Fill Tubes read again. Left alone while the artist is
    // busy in it (busy()); `force` (after the panel's own edit): only not mid-drag.
    void refresh(bool force = false);
    bool busy() const;

    // ---- State (tests) -----------------------------------------------------------------------
    const std::string& choice() const { return choice_; }  // the root shape picked: the targets', else the last picked
    // The mid's and the tip's profiles picked ("" unset: Between, As root), and the profile the
    // editor and the library's glyphs work on (0 root, 1 mid, 2 tip).
    const std::string& midChoice() const { return midChoice_; }
    const std::string& tipChoice() const { return tipChoice_; }
    int activeProfile() const { return active_; }
    void setActiveProfile(int profile);
    // The Tube brush's tubes in the viewport (Maya's Tube card): shaded surfaces, wires, each in
    // its root tube's color (Colors), drawn whatever the tool (Always visible).
    bool shaded() const;
    bool wireframe() const;
    bool colors() const;
    bool levelColors() const;  // Colors (Tube levels)
    bool alwaysVisible() const;
    const std::vector<GuideId>& targets() const { return targets_; }
    const std::string& source() const { return source_; }  // "selection", "solo" or "none"
    bool libraryShape(const std::string& name) const;
    // The placement tube (what Place and Draw give new guides) as JSON: {"shape", "width", "aspect", "twist"}.
    std::string placement() const;

    // ---- The edits, as the controls do them --------------------------------------------------
    void pick(const std::string& name);     // the picker: the targets take it, and new guides
    void pickProfile(int profile, const std::string& name);  // the mid's or the tip's picker ("" unset)
    void applyProfile(int profile);         // a row's Apply: its shape and its Length to every target
    void setHold(int profile, double percent);  // a row's Length (percent of the tube)
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
    void setShapeSmooth(bool on);           // the library shape's Smooth
    void setMirror(bool on);

    // ---- Tube roots and the tube hierarchy (Maya's root_tube_card.py; the core's root tool) ------
    // Draw root, Edit roots (Finish edit while editing), Select root: each starts the root mode
    // (the brush session's), again leaves it; Undo CV, Delete, Duplicate; the new roots' settings.
    void drawRoot();
    void editRoots();
    void selectRoot();
    void undoCv();
    void deleteRoots();
    void duplicateRoots();
    // Subdivide (issue 85): the selected roots cut into Voronoi cells (root_subdivision.h) in the
    // level below, by the density map (made at 0.5 the first time) and the multiplier, one undo
    // step. The paint brush: the density map painted (paintDensityRequested); again, painting
    // stops (paintDensityFinished), nothing cut. Apply: every cut again by the map, one undo step.
    void subdivideRoots();
    void paintDensity();
    void applyDensity();  // Apply: every cut by the density map and the multiplier again, all over
    void setMultiplier(double) {}  // (read when Subdivide or Apply runs)
    bool paintingDensity() const { return paintingDensity_; }
    void setRootSnap(bool on);
    void setRootDrawPath(bool on);
    void setRootDensity(double density);
    void setRootLength(double length);
    void setRootSegments(double segments);
    void setRootTaper(double taper);
    // The buttons as the root mode stands, the hierarchy's levels (after an undo: the tool reads
    // the groom again, its outlines following).
    void refreshRoots();
    // Add splits the selected tubes (none: the base group's) a level deeper, Remove undoes a split;
    // showLevel shows a level alone (the others' groups hidden) and picks its group.
    void addLevel();
    void removeLevel();
    void showLevel(int level);
    const std::string& levelBase() const { return levelBase_; }
    const std::vector<std::string>& levels() const { return levels_; }
    int shownLevel() const { return shownLevel_; }

    // ---- The pieces (tests, screenshots) ------------------------------------------------------
    CardBlock* rootsBlock() const { return rootsBlock_; }
    CardBlock* levelsBlock() const { return levelsBlock_; }
    QAbstractButton* drawRootButton() const { return drawRoot_; }
    QAbstractButton* editRootsButton() const { return editRoots_; }
    QAbstractButton* selectRootButton() const { return selectRoot_; }
    QAbstractButton* deleteRootsButton() const { return deleteRoots_; }
    QAbstractButton* duplicateRootsButton() const { return duplicateRoots_; }
    QComboBox* shapeBox() const { return shape_; }
    QComboBox* profileBox(int profile) const { return profile == 0 ? shape_ : profile == 1 ? mid_ : tip_; }
    ValueSlider* holdSlider(int profile) const { return holds_[static_cast<std::size_t>(profile)]; }
    QAbstractButton* shadedBox() const;
    TubeShapeEditor* editor() const { return editor_; }
    QLabel* targetsLabel() const { return targetsLabel_; }
    TubeShapeEditor* preview() const { return preview_; }  // New's small read-only profile
    QWidget* shapeOptions() const { return shapeOptions_; }
    QToolButton* newButton() const { return new_; }
    QToolButton* renameButton() const { return rename_; }
    QToolButton* duplicateButton() const { return duplicate_; }
    QToolButton* deleteButton() const { return delete_; }
    QPushButton* applyButton() const { return apply_; }
    QToolButton* smoothButton() const { return smooth_; }  // Selected guides' Smooth (its menu: which channels)
    QLineEdit* renameField() const { return renameField_; }
    ValueSlider* widthSlider() const { return width_; }
    ValueSlider* flatnessSlider() const { return flatness_; }
    CardBlock* tubeBlock() const { return tubeBlock_; }          // New
    CardBlock* profileBlock() const { return profileBlock_; }    // Edit
    CardBlock* selectedBlock() const { return selectedBlock_; }  // Edit

signals:
    void guidesEdited();     // the tubes or the library changed (one undo step): the host evaluates
    void previewed();        // a drag's live preview (no step): the host evaluates and redraws
    void settingsChanged();  // the placement tube changed (the brush session's settings)
    void message(const QString& text);  // what went wrong, or what was made ("" clears)
    void displayChanged();   // Shaded or Wireframe toggled: the host draws the brush's tubes again
    // The root mode started, left or changed its outlines (the host: the viewport's tool and
    // overlay, the guides hidden while editing).
    void rootModeChanged();
    // Paint subdivision density: the host picks the density map in the Maps panel and paints it (the
    // Paint tab shown); on Finish painting (or the host leaving the Paint brush any other way: it
    // calls paintDensity again), the Tools tab and the Tube brush again, then Edit roots.
    void paintDensityRequested(const std::string& map);
    void paintDensityFinished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    GroomDocument* document() const;
    // A slider whose value is committed when it settles (a release, a typed value, a wheel step).
    ValueSlider* committingSlider(double min, double max, double limitMin, double limitMax, int decimals,
                                  void (TubePanel::*commit)(double));
    void fillShapes();           // the pickers: Mixed (while the targets differ), the built-ins, the library
    std::string& activeChoice();  // the active profile's choice
    const std::string& activeChoice() const;
    void showChoice(bool mixed = false);
    void refreshTips();          // the fields' tips: what they change
    void pushPlacement();        // the placement tube to the brush session when it changed
    void edited(const std::string& error);  // after a guides edit: said, read again, the host told
    std::string uniqueName(const std::string& base) const;
    void disarmDelete();
    void sendPreview();
    void shapeCommitted(const std::vector<SectionPoint>& points);
    void startRoots(RootTubeTool::Mode mode);
    bool rootsDone(bool ok);  // after a root button: said, the buttons, the host told
    void fillLevels();
    std::string levelPicked() const;  // the brush's group, else the base kept

    BrushSession* session_ = nullptr;
    bool updating_ = false;
    Mode mode_ = Mode::Edit;
    // The targets and the library as last read.
    std::vector<GuideId> targets_;
    std::string source_ = "none";
    std::vector<std::string> shapes_;   // the targets' shapes
    std::vector<std::string> mids_, tips_;  // ... their mid and tip profiles ("" unset)
    std::string midChoice_, tipChoice_;     // the mid's and the tip's picked ("" unset)
    int active_ = 0;                        // the profile the editor shows: 0 root, 1 mid, 2 tip
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

    CardBlock* rootsBlock_ = nullptr;
    CardBlock* levelsBlock_ = nullptr;
    // (The modes and Duplicate: chips, checked while on; Undo CV and Delete: buttons.)
    QAbstractButton *drawRoot_ = nullptr, *editRoots_ = nullptr, *selectRoot_ = nullptr;
    QAbstractButton *undoCv_ = nullptr, *deleteRoots_ = nullptr, *duplicateRoots_ = nullptr;
    QAbstractButton *subdivideRoots_ = nullptr, *paintDensity_ = nullptr, *applyDensity_ = nullptr;
    ValueSlider* multiplier_ = nullptr;  // Subdivision multiplier (RootSubdivisionOptions::multiplier)
    bool paintingDensity_ = false;
    QCheckBox *lockView_ = nullptr, *mixedView_ = nullptr, *levelColors_ = nullptr;
    int lockedLevel_ = 0;  // Lock view: the level whose tubes stay drawn (0: off)
    void applyTubeViews();  // the document's Lock and Mixed views as the switches and the level say
    QCheckBox *rootSnap_ = nullptr, *rootDrawPath_ = nullptr, *colors_ = nullptr, *always_ = nullptr;
    ValueSlider *rootDensity_ = nullptr, *rootLength_ = nullptr, *rootSegments_ = nullptr, *rootTaper_ = nullptr;
    QPushButton *addLevel_ = nullptr, *removeLevel_ = nullptr;
    QLabel* levelBaseLabel_ = nullptr;
    QHBoxLayout* levelRow_ = nullptr;
    QWidget* levelLine_ = nullptr;  // the levels on the left, + and - on the right (LeadTrailLayout)
    std::vector<QAbstractButton*> levelButtons_;
    std::string levelBase_;
    std::vector<std::string> levels_;
    int shownLevel_ = 1;
    std::uint64_t rootsHeard_ = 0;  // the root tool's last message said (its messageCount)
    QLabel* targetsLabel_ = nullptr;  // what the fields change (their tips; not shown)
    CardBlock* tubeBlock_ = nullptr;
    CardBlock* profileBlock_ = nullptr;
    CardBlock* selectedBlock_ = nullptr;
    QVBoxLayout* newPicker_ = nullptr;     // where the picker sits in New
    QHBoxLayout* editPicker_ = nullptr;    // ... and in Edit (before Apply)
    QVBoxLayout* newFields_ = nullptr;     // where Width, Flatness and Twist sit in New
    QVBoxLayout* editFields_ = nullptr;    // ... and in Edit
    QWidget* fields_ = nullptr;
    QComboBox* shape_ = nullptr;
    QComboBox* mid_ = nullptr;
    QComboBox* tip_ = nullptr;
    std::array<QToolButton*, 3> profileButtons_{};  // Root, Mid, Tip: which one the editor shows
    std::array<QPushButton*, 3> profileApply_{};
    std::array<ValueSlider*, 3> holds_{};
    QCheckBox* shaded_ = nullptr;
    QCheckBox* wireframe_ = nullptr;
    TubeShapeEditor* preview_ = nullptr;
    TubeShapeEditor* editor_ = nullptr;
    QLabel* builtinNote_ = nullptr;  // under the editor: a built-in shape is duplicated to edit
    QWidget* shapeOptions_ = nullptr;  // Mirror and Smooth: a library shape's
    QToolButton* mirror_ = nullptr;
    QToolButton* shapeSmooth_ = nullptr;
    QToolButton* new_ = nullptr;
    QToolButton* rename_ = nullptr;
    QToolButton* duplicate_ = nullptr;
    QToolButton* delete_ = nullptr;
    QLineEdit* renameField_ = nullptr;
    QPushButton* apply_ = nullptr;
    ValueSlider* width_ = nullptr;
    double lengthFactor_ = 1.0;  // Width shown per scene unit (refreshUnits)
    ValueSlider* flatness_ = nullptr;
    ValueSlider* twist_ = nullptr;
    QToolButton* smooth_ = nullptr;
    std::string smoothChannels_ = "current";  // Smooth's: the brush's channel ("current") or "all"
    QTimer* disarm_ = nullptr;
    QTimer* previewTimer_ = nullptr;
    bool placed_ = false;  // the picker and fields not yet in a block
    std::vector<ValueSlider*> sliders_;
    std::vector<ValueSlider*> held_;  // the sliders being dragged
};

}  // namespace groomlab::app
