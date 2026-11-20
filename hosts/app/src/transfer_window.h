// SPDX-License-Identifier: Apache-2.0
#pragma once

// Tools > Transfer Groom to Mesh: a Wrap-style window, never modal. Two views side by side, the
// groom's current scalp on the left and the new scalp (a mesh file picked with Browse) on the
// right, each with its own orbit camera (Alt+left orbits, Alt+middle or middle pans, Alt+right,
// right or the wheel dollies; Frame fits the mesh), and the transfer's choices below: the method,
// keep guide lengths, project painted maps, symmetry, Import Markers..., the pair count and why
// Transfer cannot run yet, and Transfer.
//
// Marker points are clicked on the meshes (the marker methods; with the others the views show the
// meshes only and a left drag orbits). The rules are the core's TransferMarkerEditor
// (transfer_markers_edit.h), the same as Maya's window:
//   - each side keeps its own ordered list; pair N is the Nth point on the left and the Nth on
//     the right, placed in any order. Unmatched points draw hollow, matched ones filled; the left
//     side yellow, the right cyan; numbers are the pair index from 1.
//   - a click adds a point at the end of that side's list (a ray against the mesh: the core's
//     ScalpMesh::intersectRay, as the brushes pick); a drag on a point slides it over the surface;
//     Ctrl+click on a point deletes it and its partner (both lists renumber); hovering a point
//     lights it and its partner in the other view.
//   - Symmetry X: a click also adds the mirror across x = the mesh's bounding-box center, snapped
//     onto the mesh, as the next point on that side; deleting either deletes both and partners.
//   - Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) undo and redo marker edits (the window's own stack).
//   - the complete pairs are stored with the groom on every edit (GroomDocument::
//     setTransferMarkers: saved in its file, a marker transfer's markers); unmatched points live
//     in the window while it is open.
//
// The views render the mesh with the app's GroomRenderer (no lookdev, no strands) into an
// OffscreenView on a QRhi the window shares between them, read back into an image the widget
// paints with the dots over it: one path on screen, in the offscreen tests and in a grab().

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>

#include "camera.h"
#include "marker_view.h"
#include "groomlab/transfer_markers_edit.h"
#include "groomlab/view_projection.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QRhi;

namespace groomlab::app {

class MainWindow;

class TransferWindow : public QWidget, public MarkerViewHost {
    Q_OBJECT
public:
    explicit TransferWindow(MainWindow* main, QWidget* parent = nullptr);
    ~TransferWindow() override;

    // The groom's scalp into the left view and its stored markers into the lists (opening, after a
    // transfer, another groom opened). Unmatched points are dropped.
    void reloadGroom();
    // The groom's scalp shown again at its subdivision level now (Scalp Subdivision changed), the
    // camera and the markers kept; nothing when it shows that already.
    void refreshScalp();
    // The new scalp from a mesh file (.obj, .abc): the right view reloads, the left is kept. False
    // (and why in the status line) when it cannot be read.
    bool setNewScalpPath(const QString& path);
    std::shared_ptr<const ScalpMesh> meshOf(MarkerSide side) const;

    void setMethod(TransferMethod method);
    TransferMethod method() const;
    bool markerEditing() const override { return usesMarkers(method()); }
    void setSymmetry(bool on);
    bool symmetry() const;

    // ---- Marker edits (the clicks call these after picking; scripts and tests call them) ----
    // A point snapped onto the side's mesh at the end of its list (with Symmetry X, its mirror
    // after it); returns its index.
    std::size_t addPoint(MarkerSide side, const Vec3f& world);
    // The same for a picked point (its root kept).
    std::size_t addPicked(MarkerSide side, const MarkerPoint& picked) override;
    std::vector<std::size_t> deletePoint(MarkerSide side, std::size_t index) override;
    void movePoint(MarkerSide side, std::size_t index, const Vec3f& world, bool newStep = true);
    void movePicked(MarkerSide side, std::size_t index, const MarkerPoint& picked, bool newStep) override;
    bool undo();
    bool redo();
    void clearPoints();
    // Pairs from a JSON file (transfer.h's format) replacing both lists: one undo step.
    bool importMarkers(const QString& path);
    const TransferMarkerEditor& editor() const { return editor_; }
    const MarkerLists& markerLists() const override { return editor_.lists(); }
    // The unmatched points (the longer list's tail).
    std::vector<std::pair<MarkerSide, std::size_t>> pending() const;

    // Hover: a point and its partner lit in both views.
    void setHover(MarkerHover hover) override;
    const MarkerHover& hover() const override { return hover_; }

    MarkerView* view(MarkerSide side) const { return side == MarkerSide::Old ? left_ : right_; }
    // Why Transfer cannot run yet ("" when it can).
    QString requirement() const;
    QString statusText() const;
    QPushButton* transferButton() const { return transferButton_; }
    // Transfers with the window's choices (MainWindow::transferToMesh); false with why in the
    // status line.
    bool transfer();
    // The shared GPU device (made on first use; null when none can be made).
    QRhi* rhi() override;
    // A finished drag or a hover change: the views repaint; `store`: the pairs to the groom.
    void edited(bool store = true) override;

private:
    MarkerPoint snapped(MarkerSide side, const Vec3f& world) const;
    void undoStep();
    void redoStep();
    void store();
    void refreshStatus();
    void chooseNewScalp();
    void chooseMarkersFile();

    MainWindow* main_;
    TransferMarkerEditor editor_;
    std::shared_ptr<const ScalpMesh> newScalp_;
    QString newScalpPath_;
    std::optional<std::pair<MarkerSide, std::size_t>> hover_;
    std::unique_ptr<QRhi> rhi_;
    bool rhiTried_ = false;
    MarkerView* left_ = nullptr;
    MarkerView* right_ = nullptr;
    QLabel* leftTitle_ = nullptr;
    QLabel* rightTitle_ = nullptr;
    QComboBox* methodBox_ = nullptr;
    QCheckBox* keepLengths_ = nullptr;
    QCheckBox* fitSize_ = nullptr;  // fit the groom to the new scalp's size (TransferOptions::fitSize)
    QCheckBox* maps_ = nullptr;
    QCheckBox* symmetry_ = nullptr;
    QPushButton* undoButton_ = nullptr;
    QPushButton* redoButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QPushButton* transferButton_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* message_ = nullptr;
};

}  // namespace groomlab::app
