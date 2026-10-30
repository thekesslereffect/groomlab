// SPDX-License-Identifier: Apache-2.0
#pragma once

// The maps panel (the Groom Tools' Paint tab: Maps, under the Paint card), from the shared
// definitions (engine/ui/lists.json's maps), as the redesign lays it out: the header (Maps, how
// many, + New Map and Import... glyphs), the groom's painted maps as plain rows (vertex maps and texture maps on the scalp, guide layers on
// the guides; a gray ramp or r g b swatch each, ⚠ and the warning color for a texture whose image
// is missing; a double-click renames in place, a right-click opens lists.maps.context), the new
// map's form (+ New Map shows it), and the picked map's box: Rename, Convert to ▾, Delete, Show on
// (the scalp, the channel shown) and what reads it ("Read by"). What Paint paints toward (the mode,
// the value or a color map's color and channels, Fill) is paintControls(): the Paint card holds it. Every edit goes to the document as one undo step. Painting: the Paint brush set on the
// picked map (its kind, its file, the value or color and channels) and the viewport put in brush
// mode. A new map may be for a setting (Density, Width... or any mappable param): made, linked and
// painted at once (paintParamRequested).
//
// The list is rebuilt only when what it shows changed, and while the panel is hidden not at all
// (invalidate: a stroke marks it stale; it catches up when it shows or is asked for its map).

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QWidget>

#include "definitions.h"
#include "groomlab/scalp_paint.h"

class QCheckBox;
class QComboBox;
class QFrame;
class QGroupBox;
class QLineEdit;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace groomlab::app {

class ValueSlider;

class BrushSession;
class ColorEditor;
class GroomDocument;
class ListBar;
class ListHeader;
class RoundButton;

class MapsPanel : public QWidget {
    Q_OBJECT
public:
    explicit MapsPanel(QWidget* parent = nullptr);

    void setDocument(GroomDocument* document);  // null: nothing to list
    void setSession(BrushSession* session);     // the brush the Paint button sets up
    // The window's bindings: the bar and the right-click menu are built with them.
    void setActions(const ui::Bindings& bindings, const ui::FillOptions& options);
    void refresh();     // the list from the document, now
    void invalidate();  // stale: refreshed on the next turn while shown, else when shown or asked
    bool stale() const { return stale_; }
    int rebuilds() const { return rebuilds_; }  // the list's rebuilds so far (tests)
    std::string selectedMap() const;
    void selectMap(const std::string& name);
    std::string selectedKind() const;   // "vertices", "texture", "guides" ("" none)
    int selectedChannels() const;       // 1 gray (or none), 4 color
    std::string selectedFile() const;   // a texture map's image, resolved ("" otherwise)
    std::string selectedMissing() const;  // a texture map's expected image when it is missing, else ""
    QString usersText() const;          // the "Read by" line
    // The conditions the right-click menu's entries need for the picked map: "map", "map.scalp"
    // (vertices or texture), "map.color" (a color scalp map), "map.texture".
    std::set<std::string> menuState() const;
    // The right-click menu for the row at `pos` (the list's viewport coordinates; that row picked
    // first), not shown: the caller pops it up (it deletes itself when it closes).
    QMenu* contextMenu(const QPoint& pos);
    void editSelectedName();  // the picked map's name edited in place (Rename)
    // Deleting a texture map deletes its file too (Delete Its File Too; on by default).
    bool deleteFiles() const { return deleteFiles_; }
    void setDeleteFiles(bool on) { deleteFiles_ = on; }
    // The new map's form (+ New Map shows it, Create or another New Map hides it).
    void setNewMapFormShown(bool shown);
    bool newMapFormShown() const;
    // What the panel would make: the new-map form's fields (tests and scripted runs set them).
    void setNewMapFields(const QString& name, const QString& kind, bool color, double value, int resolution);
    // The form's For: a setting the new map is for (a common one, or one under Other: any mappable
    // param of the graph), or none ("" ""): a blank map. False when node.param is not offered.
    bool setNewMapTarget(const QString& node, const QString& param);
    std::pair<std::string, std::string> newMapTarget() const;
    // The value a gray map is painted toward (the Paint box's slider).
    void setPaintValue(double value);
    double newMapFill() const;  // the form's fill (1.0 until set: a mask that lets everything through)
    bool hintShown() const;     // the one-line hint while the groom has no maps
    // The edits as functions: "" or the error shown.
    std::string createFromFields();
    std::string renameSelected(const QString& to);
    std::string removeSelected();  // its file too with deleteFiles()
    std::string importImage(const QString& path);  // a new texture map from an image, picked
    std::string relinkSelected(const QString& path);
    std::string reloadSelected();
    std::string convertSelected(const std::string& kind, int resolution = 1024);
    std::string setSelectedChannels(int channels);
    // The picked map set to the value (a guide layer: on the selected guides when some are), a
    // color map to the color in the channels ticked.
    std::string fillSelected();
    // Sets the brush session up to paint the selected map and asks the host for brush mode.
    bool paintSelected();
    // Shows the selected map on the scalp (or none).
    void showSelected(bool show);

    // The pieces (tests, screenshots).
    QListWidget* list() const { return list_; }
    ListBar* bar() const { return bar_; }          // the header's: + New Map, Import...
    ListBar* itemBar() const { return itemBar_; }  // the picked map's: Rename, Convert to, Delete
    QFrame* mapBox() const { return box_; }
    // Mode, Value + Fill and the color (its channels, Fill): the Paint card's (brush_panel.h).
    QWidget* paintControls() const { return paintControls_; }

signals:
    void edited();          // the document changed (maps made, renamed, removed): evaluate, undo, redisplay
    void paintRequested();  // the brush is set on a map: the host enters brush mode
    void displayChanged();  // the map shown on the scalp changed: the host redisplays
    void brushChanged();    // the brush's paint settings changed: the host refreshes the brush panels
    void message(const QString& text);
    // Create with For set to a setting: the host makes (or finds) the map node.param reads, links
    // it and paints it (MainWindow::paintParam).
    void paintParamRequested(const std::string& node, const std::string& param, const std::string& kind);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void fresh() const;         // refreshed first when stale
    void refreshTargets();      // the For choices from the document's graph (when its nodes changed)
    void targetChanged();       // the form follows For (named for the setting, gray, on the scalp)
    void refreshRows();         // the paint, show and Read by rows for the picked map
    void applyPaintSettings();  // the value, color and channels to the session
    void itemRenamed(QListWidgetItem* item);
    std::string done(const std::string& error, const std::string& select = {});  // after an edit: refreshed, picked, edited()
    GroomDocument* document_ = nullptr;
    BrushSession* session_ = nullptr;
    ui::Bindings bindings_;
    ui::FillOptions options_;
    bool stale_ = false, pending_ = false;
    int rebuilds_ = 0;
    std::string shownKey_, targetsKey_;
    bool deleteFiles_ = true;
    ListHeader* header_ = nullptr;
    ListBar* bar_ = nullptr;
    ListBar* itemBar_ = nullptr;
    QFrame* box_ = nullptr;
    QWidget* paintControls_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* hint_ = nullptr;  // shown while the list is empty
    QLabel* users_ = nullptr;  // Read by
    QGroupBox* newBox_ = nullptr;
    QComboBox* newFor_ = nullptr;    // nothing, a common setting, or Other (newOther_)
    QComboBox* newOther_ = nullptr;  // every mappable param of the graph
    QLineEdit* newName_ = nullptr;
    QComboBox* newKind_ = nullptr;
    QCheckBox* newColor_ = nullptr;
    ValueSlider* newValue_ = nullptr;
    QSpinBox* newResolution_ = nullptr;
    QToolButton* show_ = nullptr;
    QComboBox* channel_ = nullptr;
    ValueSlider* paintValue_ = nullptr;
    QWidget* valueRow_ = nullptr;
    ColorEditor* paintColor_ = nullptr;
    ValueSlider* paintAlpha_ = nullptr;
    std::vector<QToolButton*> paintChannels_;
    std::vector<QToolButton*> paintModes_;
    QWidget* colorRow_ = nullptr;
    RoundButton* paint_ = nullptr;
    bool updating_ = false;
};

}  // namespace groomlab::app
