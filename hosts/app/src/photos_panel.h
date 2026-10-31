// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Groom Tools' Photos tab, from engine/ui/lists.json's photos (the bar, the right-click menus)
// as the redesign lays it out: Photos (the header: how many, Import Photo...; the list), the picked
// photo's box (Rename, Delete and the rows below), then Guides from photos, folded. Reference photos
// to groom to, each a camera.
//
//   list          the photos (a thumbnail, the name, the size, locked); double-click: Look Through
//   Look Through  the viewport shows the photo's camera with the photo fitted in the frame (again:
//                 back to the camera it showed before); Lock keeps the camera where it is (tumbling
//                 through it does nothing; unlocked, tumbling lines it up by eye)
//   Opacity, Focal Length, Photo (In front / Behind) and the eye: how the photo shows while looking
//                 through it (a focal length set by hand turns Keep focal length on)
//   Align Photo...  the Photos window (reference_window.h) on the picked photo, its Align stage: the
//                 photo and the mesh side by side, landmark pairs clicked on each; below it, where
//                 the alignment stands
//   Guides from Photos  the same window's Guides stage
//
// The photos are the groom's (GroomDocument::referencePhotos, its file's "referencePhotos": the
// same records the Photos window and Blender's Photos edit), each with its "display" (opacity,
// depth, locked, visible, keepFocal). The window (MainWindow) owns the viewport: the panel asks
// (lookThroughRequested, displayChanged) and the window tells it back (setLookingThrough).

#include <optional>
#include <set>
#include <string>

#include <QImage>
#include <QMap>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

#include "definitions.h"
#include "groomlab/reference_guides.h"

class QFrame;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QToolButton;

namespace groomlab::app {

class GroomDocument;
class ListBar;
class ListHeader;
class Section;
class ValueSlider;

// A camera for a photo never lined up (Import Photo's, Maya's reference._place): in front (+Z) of
// `document`'s groom, far enough for it to fill most of the photo's short side, a 50 mm lens.
RefCamera placedReferenceCamera(const GroomDocument* document, int width, int height);

class PhotosPanel : public QWidget {
    Q_OBJECT
public:
    explicit PhotosPanel(QWidget* parent = nullptr);

    void setDocument(GroomDocument* document);  // null: nothing to list
    // The window's bindings: the bars (photos, guides) and the right-click menu.
    void setActions(const ui::Bindings& bindings, const ui::FillOptions& options);
    void refresh();  // the list and the picked photo's controls from the document

    // ---- The photos ------------------------------------------------------------------------------
    int count() const;
    int current() const;  // the picked photo, or -1
    void select(int index);
    QString name(int index) const;
    QString imagePath(int index) const;
    QImage picture(int index) const;  // read once per file (and when it changes)
    QSize imageSize(int index) const;
    // The photo's camera (stored, else none: never lined up).
    std::optional<RefCamera> camera(int index) const;
    void setCamera(int index, const RefCamera& camera);  // stored with the groom
    bool placeCamera(int index);  // placedReferenceCamera stored for photo `index`
    // How it shows while looking through it.
    struct Display {
        double opacity = 0.6;  // Maya's reference.DEFAULT_OPACITY
        bool front = true;     // choices.json photoDepth: front (over the mesh) or behind
        bool locked = false;
        bool visible = true;
        bool keepFocal = false;  // a solve keeps the focal length (set by hand, or Keep focal length)
    };
    Display display(int index) const;
    void setDisplay(int index, const Display& display);

    // ---- Edits (the bindings and the tests) -------------------------------------------------------
    // A photo from a file: its size read, named after it (made unique), its camera placed in front
    // of the groom (+Z) far enough for it to fill the photo's short side (Maya's _place), picked.
    // Its index, or -1 (why: message()).
    int importPhoto(const QString& path);
    bool rename(int index, const QString& name);  // false: empty, taken or no such photo
    bool remove(int index);
    bool keepFocal() const;     // the picked photo's Keep focal length
    // The focal length back to 50 mm and its slider to its starting range (a solve gone wrong; the
    // slider's right-click menu): Keep focal length turns on, as with any focal length set by hand.
    void resetFocal();
    QString alignText() const;  // where the picked photo's alignment stands (below Align Photo...)

    // ---- Looking through ---------------------------------------------------------------------------
    // The photo the viewport looks through ("" none): the buttons follow (the window says).
    void setLookingThrough(const QString& name);
    const QString& lookingThrough() const { return looking_; }
    int lookingIndex() const;

    // The right-click menu (not shown: the caller pops it up; it deletes itself).
    QMenu* contextMenu(const QPoint& pos);
    std::set<std::string> menuState() const;  // "photo" with one picked

    // ---- The pieces (tests) ------------------------------------------------------------------------
    QListWidget* list() const { return list_; }
    QPushButton* lookButton() const { return look_; }
    QToolButton* lockButton() const { return lock_; }
    ValueSlider* opacitySlider() const { return opacity_; }
    ValueSlider* focalSlider() const { return focal_; }
    QToolButton* depthButton(bool front) const { return front ? front_ : behind_; }
    QToolButton* visibleButton() const { return visible_; }
    QPushButton* alignButton() const { return align_; }
    ListBar* bar() const { return bar_; }          // the header's: Import Photo...
    ListBar* itemBar() const { return itemBar_; }  // the picked photo's: Rename, Delete
    Section* guidesSection() const { return guidesSection_; }  // Guides from photos (folded)

signals:
    void lookThroughRequested(int index, bool on);
    void displayChanged();  // the opacity, the side, the visibility or the lock of a photo
    void edited();          // the photos changed (stored with the groom)
    void alignRequested(int index);  // Align Photo...: the Photos window's Align stage on it
    void message(const QString& text);

private:
    nlohmann::json photos() const;  // the document's records (an array)
    void store(const nlohmann::json& photos);
    void refreshControls();
    QString uniqueName(const QString& wanted, int except = -1) const;

    GroomDocument* document_ = nullptr;
    ui::Bindings bindings_;
    ui::FillOptions options_;
    QString looking_;
    bool updating_ = false;
    mutable QMap<QString, QImage> pictures_;  // by file
    ListHeader* header_ = nullptr;
    ListBar* bar_ = nullptr;
    ListBar* itemBar_ = nullptr;
    QFrame* box_ = nullptr;
    QLabel* empty_ = nullptr;
    Section* guidesSection_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* look_ = nullptr;
    QToolButton* lock_ = nullptr;
    ValueSlider* opacity_ = nullptr;
    ValueSlider* focal_ = nullptr;  // the photo camera's focal length (mm, a 36 mm film back)
    QToolButton* front_ = nullptr;
    QToolButton* behind_ = nullptr;
    QToolButton* visible_ = nullptr;
    QPushButton* align_ = nullptr;
    QLabel* alignAbout_ = nullptr;
    QLabel* guidesHint_ = nullptr;
    ListBar* guidesBar_ = nullptr;
};

}  // namespace groomlab::app
