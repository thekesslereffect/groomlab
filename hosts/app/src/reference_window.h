// SPDX-License-Identifier: Apache-2.0
#pragma once

// The app's Photos window (Groom Tools > Photos > Align Photo..., Tools > Guides from Photos
// (experimental)), as Maya's (groomlab.ui.reference_window) and built like the Transfer window
// (marker_view.h): never modal, the reference photo on the left (2D: the wheel zooms at the
// cursor, the middle button or Alt+left pans, F or Fit fits) and the groom's scalp on the right (a
// MarkerView: its own orbit camera), settings beside them. Two stages (the chips at the top left):
//   - Align: landmarks only; the panel holds the pairs and their errors, the camera (Focal Length,
//     Keep focal length, Lock) and the mesh drawn over the photo.
//   - Guides (experimental): Guides from Photos besides: flow arrows, mask touch-ups, Generate.
// Photos: a tab each (Import Photo..., Remove), each with its own landmarks, flow arrows, mask
// touch-ups and solved camera (the core's ReferencePhotoEditor, reference_edit.h, the same rules
// as Maya's window):
//   - Landmarks: a click on the photo and one on the mesh (either order) make a pair; a drag moves
//     a point, Ctrl+click deletes it and its partner. 4 pairs solve the camera live (the focal
//     length kept), 6 or more the focal length too, unless Keep focal length (the photo's, kept in
//     its "display" with the Photos tab's settings; a focal length set by hand turns it on); a
//     locked photo's camera stays where it is. Each pair's error shows as a line to where its mesh
//     point lands, bad pairs red; the mesh's outline or wireframe draws over the photo.
//   - Flow: a drag from root to tip draws an arrow; Ctrl+drag erases.
//   - Hair / Backdrop: mask touch-ups (Ctrl swaps them); Mask tint and Flow field show the mask
//     and the strand directions the core reads (previewReferenceView).
//   - Ctrl+Z / Ctrl+Y undo and redo the current photo's edits (its own stack); everything is stored
//     with the groom on every edit (GroomDocument::setReferencePhotos: its file's "referencePhotos").
//   - Generate: guidesFromReference over the photos with a solved camera, into the fromReference
//     guide group (replaced), one undo step of the document's.

#include <memory>
#include <optional>
#include <vector>

#include <QImage>
#include <QPointF>
#include <QString>
#include <QWidget>

#include "marker_view.h"
#include "groomlab/reference_edit.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabBar;
class QToolButton;
class QTreeWidget;

namespace groomlab::app {

class MainWindow;
class ReferenceWindow;
class ValueSlider;

// One photo of the window: its file, its edits and its camera.
struct ReferencePhoto {
    QString name;
    QString image;     // the photo's file
    int width = 0, height = 0;
    QString mask;      // a mask image ("" automatic)
    ReferencePhotoEditor editor;
    std::optional<RefCamera> camera;  // solved (or as stored)
    ReferenceLandmarkSolve solve;     // the last solve
    QImage picture;                   // the photo as read
    std::string display;              // the Photos tab's "display" (opacity, depth, locked, visible, keepFocal) as JSON text
};

// The photo, 2D.
class PhotoView : public QWidget {
    Q_OBJECT
public:
    explicit PhotoView(ReferenceWindow* window, QWidget* parent = nullptr);
    void photoChanged();  // another photo (or none): fitted again
    void fit();
    QPointF toPhoto(const QPointF& widget) const;
    QPointF toWidget(const QPointF& photo) const;
    double scale() const { return scale_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class Drag { None, Pan, Point, Stroke, Erase, Mask, Owned };
    ReferenceWindow* window_;
    double scale_ = 1.0;
    QPointF offset_;
    bool userMoved_ = false;
    Drag drag_ = Drag::None;
    QPointF last_;
    std::optional<QPointF> hoverPos_;
    std::size_t dragIndex_ = 0;
    bool firstMove_ = false;
    std::vector<std::array<float, 2>> path_;
    bool include_ = true;
    float radius_ = 8.0f;
};

class ReferenceWindow : public QWidget, public MarkerViewHost {
    Q_OBJECT
public:
    enum class Stage { Align, Guides };
    enum class Mode { Landmarks, Flow, Hair, Backdrop };
    enum class Overlay { Outline, Wireframe, Off };

    explicit ReferenceWindow(MainWindow* main, QWidget* parent = nullptr);
    ~ReferenceWindow() override;

    // The groom's scalp into the mesh view and its stored photos into the tabs (opening, another
    // groom opened, after a transfer).
    void reloadGroom();
    // The groom's scalp shown again at its subdivision level now (Scalp Subdivision changed), the
    // camera and the landmarks kept; nothing when it shows that already.
    void refreshScalp();
    // The groom's photos read again (the Photos tab changed them), the current one kept by name.
    void photosChanged();

    // ---- Photos ----
    // A photo from a file (its size read; stored with the groom); its index, or -1 (why in the
    // message line).
    int importPhoto(const QString& path);
    bool removePhoto(int index);
    void selectPhoto(int index);
    int currentIndex() const { return current_; }
    int photoCount() const { return static_cast<int>(photos_.size()); }
    ReferencePhoto* photo();  // the current one, or null
    const ReferencePhoto* photo() const;

    // ---- Edits (the clicks call these; scripts and tests too) ----
    // Align (landmarks only) or Guides (the flow, the mask and Generate too).
    void setStage(Stage stage);
    Stage stage() const { return stage_; }
    // The photo view's tool; one of Guides' moves to the Guides stage.
    void setMode(Mode mode);
    Mode mode() const { return mode_; }
    std::size_t addLandmark(MarkerSide side, const Vec3f& point);  // photo: (u, v, 0); mesh: snapped
    std::vector<std::size_t> deleteLandmark(MarkerSide side, std::size_t index);
    void moveLandmark(MarkerSide side, std::size_t index, const Vec3f& point, bool newStep = true);
    void clearLandmarks();
    std::size_t addFlowStroke(const std::vector<std::array<float, 2>>& points, bool directed = true);
    std::size_t eraseFlow(const std::vector<std::array<float, 2>>& path, float radius = 0.0f);
    void clearFlow();
    std::size_t addMaskStroke(const std::vector<std::array<float, 2>>& points, float radius, bool include);
    void clearMask();
    void setKeepFocal(bool on);  // the current photo's Keep focal length (stored with it)
    bool keepFocal() const;
    // The current photo's focal length set by hand (mm on a 36 mm film back): Keep focal length turns
    // on and the camera lines up again keeping it. False: no photo, or locked.
    bool setFocal(double mm);
    // The focal length back to 50 mm and its slider to its starting range (a solve gone wrong; the
    // slider's right-click menu).
    bool resetFocal();
    void setLocked(bool on);  // the current photo's camera kept where it is (the landmarks don't move it)
    bool locked() const;
    bool undo();
    bool redo();
    // The photo landmark within `pixels` (photo pixels) of `uv`, or -1.
    int photoPointNear(const QPointF& uv, double pixels) const;
    // Lights a point and its partner (either side), or none.
    void setHover(MarkerHover hover) override;
    const MarkerHover& hover() const override { return hover_; }

    // ---- What the photo view draws ----
    void setOverlay(Overlay overlay);
    Overlay overlay() const;
    double overlayOpacity() const;
    void setMaskTint(bool on);
    void setFieldLines(bool on);
    bool maskTint() const;
    bool fieldLines() const;
    double brushSize() const;
    const std::vector<std::array<float, 4>>& overlayLines();  // photo pixels
    const std::vector<ReferenceArrow>& arrows();
    // The current photo's mask and field as the core works on them (null: not wanted or none).
    const ReferenceViewPreview* preview();
    QImage previewTint();

    // ---- Generate ----
    QString requirement() const;  // why Generate cannot run yet ("" when it can)
    // Grows the guides into the fromReference group; false with why in the message line.
    bool generate();
    void setGuideCount(int count);
    void setMaxLength(double length);
    void setSymmetry(const QString& name);  // auto, none, x, topology

    PhotoView* photoView() const { return photoView_; }
    MarkerView* meshView() const { return meshView_; }
    QPushButton* generateButton() const { return generateButton_; }
    QWidget* guidesPanel() const { return guidesPanel_; }
    QToolButton* modeButton(Mode mode) const { return modeButtons_[static_cast<int>(mode)]; }
    ValueSlider* focalSlider() const { return focal_; }
    QToolButton* lockButton() const { return lock_; }
    QString statusText() const;
    QString messageText() const;
    QString solveText() const;

    // ---- MarkerViewHost (the mesh view: the mesh side of the landmarks) ----
    const MarkerLists& markerLists() const override;
    bool markerEditing() const override { return mode_ == Mode::Landmarks && photo() != nullptr; }
    std::size_t addPicked(MarkerSide side, const MarkerPoint& picked) override;
    std::vector<std::size_t> deletePoint(MarkerSide side, std::size_t index) override { return deleteLandmark(side, index); }
    void movePicked(MarkerSide side, std::size_t index, const MarkerPoint& picked, bool newStep) override;
    void edited(bool store = true) override;
    QRhi* rhi() override;
    QString emptyViewText(MarkerSide side) const override;

signals:
    void guidesChanged();  // Generate made or replaced the fromReference group
    void photosStored();   // the photos were stored with the groom (an edit here)

private:
    friend class PhotoView;
    void buildPanel(QWidget* panel);
    void refreshTabs();
    void refreshPanel();
    void solve(bool findBad = true);
    void store();
    void loadPhotos();
    double focalGuess(const ReferencePhoto& p) const;
    void setDisplayFlag(const char* key, bool on);  // in the current photo's "display", stored

    MainWindow* main_;
    std::vector<std::unique_ptr<ReferencePhoto>> photos_;
    int current_ = -1;
    Stage stage_ = Stage::Align;
    bool updatingCamera_ = false;  // the camera controls set from the photo (not an edit)
    Mode mode_ = Mode::Landmarks;
    MarkerHover hover_;
    std::unique_ptr<QRhi> rhi_;
    bool rhiTried_ = false;
    MarkerLists empty_;
    std::vector<std::array<float, 4>> overlayLines_;
    std::uint64_t overlayKey_ = ~0ull;
    std::vector<ReferenceArrow> arrows_;
    std::uint64_t arrowsKey_ = ~0ull;
    std::optional<ReferenceViewPreview> preview_;
    QImage previewTint_;
    std::uint64_t previewKey_ = ~0ull;
    std::uint64_t solveCount_ = 0;  // moves the overlay's and the preview's keys

    QTabBar* tabs_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QToolButton* stageButtons_[2] = {};
    QToolButton* modeButtons_[4] = {};
    PhotoView* photoView_ = nullptr;
    MarkerView* meshView_ = nullptr;
    QLabel* meshTitle_ = nullptr;
    QLabel* solveLabel_ = nullptr;
    QTreeWidget* pairs_ = nullptr;
    QCheckBox* keepFocal_ = nullptr;
    ValueSlider* focal_ = nullptr;
    QToolButton* lock_ = nullptr;
    QComboBox* overlayBox_ = nullptr;
    ValueSlider* opacity_ = nullptr;
    QWidget* guidesPanel_ = nullptr;
    QLabel* flowLabel_ = nullptr;
    QCheckBox* maskTint_ = nullptr;
    QCheckBox* fieldLines_ = nullptr;
    ValueSlider* brush_ = nullptr;
    QComboBox* symmetry_ = nullptr;
    QSpinBox* guideCount_ = nullptr;
    QDoubleSpinBox* maxLength_ = nullptr;
    QPushButton* generateButton_ = nullptr;
    QPushButton* undoButton_ = nullptr;
    QPushButton* redoButton_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* message_ = nullptr;
};

}  // namespace groomlab::app
