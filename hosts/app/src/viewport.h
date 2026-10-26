// SPDX-License-Identifier: Apache-2.0
#pragma once

// The viewport widget: a QRhiWidget (Qt renders it through QRhi on the platform's API) that
// hands its frames to the GroomRenderer and turns the mouse into the orbit camera, or, in brush
// mode, into strokes on the BrushSession (the core's brush tool over the open groom) and in
// select mode into guide picks. It draws when asked (update(): a camera move, a new display
// frame, a resize, a hover) and, in refine mode, again after each frame while the renderer says
// another would improve the image, so the picture keeps converging while nothing moves.
//
// Keys: B enters brush mode (held: a left drag sets the radius, Shift+drag the strength), M
// held: a left drag sets the strength (brush mode too), Y held: a left press opens the marking
// menu (markingMenuPressed; the drag and the release go to it), S select mode, Escape the camera. Alt with any button is always the camera (Maya's); without
// Alt the plain buttons orbit, pan and dolly in camera mode. In brush mode a left drag strokes
// (Shift: smooth; Ctrl on a guide brush: paint the selection, Ctrl+Shift a lasso, a click adding the guides in the ring;
// painting, Ctrl: the opposite), the middle button sets the
// radius (Shift: the strength), the right orbits; in select mode a left click or box picks
// (Shift: toggle, Ctrl: deselect, Ctrl+Shift: add). A pen's pressure (QTabletEvent) scales the
// stroke.

#include <QElapsedTimer>
#include <QPoint>
#include <QPointF>
#include <QRhiWidget>
#include <QTimer>

#include "camera.h"
#include "renderer.h"

namespace groomlab::app {

class BrushSession;

class Viewport : public QRhiWidget {
    Q_OBJECT
public:
    enum class ToolMode { Camera, Brush, Select };

    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    GroomRenderer& renderer() { return renderer_; }
    OrbitCamera& camera() { return camera_; }
    const OrbitCamera& camera() const { return camera_; }

    // What to draw (the builder's frame; the scalp), then a redraw.
    void setFrame(const DisplayFrame& frame);
    void setScalp(MeshPtr scalp);
    void frameAll(const Bounds& bounds);
    // The API in use, for the status bar ("Direct3D 11"), once the widget initialized.
    QString apiName() const;
    // The last frame's time from the one before, while frames follow each other (refining):
    // close to the GPU's time per frame.
    double lastFrameMs() const { return lastFrameMs_; }
    // The CPU time the last render() took (recording the frame, not the GPU's work).
    double lastRenderCpuMs() const { return lastRenderCpuMs_; }
    // The GPU's own time of the last frame whose timestamps came back (0: the device gives none).
    double lastGpuMs() const { return lastGpuMs_; }
    // A drag outside this widget (a slider's) is live: the lookdev frames draw reduced under
    // the adaptive interactive quality, the full picture once it ends (the mouse's own drags
    // say so themselves).
    void setInteracting(bool on);

    // ---- Tools -------------------------------------------------------------------------------
    // The brush session the mouse drives in brush and select modes (null: the camera alone).
    void setBrushSession(BrushSession* session);
    BrushSession* brushSession() const { return session_; }
    void setToolMode(ToolMode mode);
    ToolMode toolMode() const { return mode_; }
    // The session's view is the camera over this widget (called before every event; a host
    // that moved the camera itself calls it before asking the session anything).
    void syncView();
    // The session's overlay (the ring, the hovered guides) to the renderer, then a redraw.
    void refreshOverlay();

signals:
    void cameraChanged();
    // A frame was drawn (the first one: a screenshot may be taken).
    void frameDrawn();
    void toolModeChanged(ToolMode mode);
    // A stroke was pressed (before its first strokeDabbed); its release is about to commit it
    // (before the tool's release: the host takes in what still evaluates the stroke).
    void strokeStarted();
    void strokeReleasing();
    // A stroke's press or drag changed the session's preview: the host evaluates and redraws.
    void strokeDabbed();
    // A stroke ended (committed when it changed something, which the host shows); the brush's
    // settings changed (the radius or strength dragged); the selection changed.
    void strokeEnded();
    void brushSettingsChanged();
    void selectionChanged();
    // Delete was pressed over the viewport (the host removes the selected guides).
    void deleteRequested();
    // Something went wrong in a stroke (the host says so; the stroke was dropped).
    void message(const QString& text);
    // Y held and the left button pressed (in any tool): the host opens the marking menu at `at`
    // (this widget's coordinates); the drag and the release that follow are the menu's.
    void markingMenuPressed(QPointF at);
    void markingMenuDragged(QPointF at);
    void markingMenuReleased(QPointF at);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class Drag { None, Orbit, Pan, Dolly, Stroke, Radius, Strength, Box, Paint, Lasso, Menu };
    void cameraMoved();
    bool cameraPress(QMouseEvent* event, bool alt);  // the camera's buttons; false: not a camera drag
    float pressure() const;
    GroomRenderer renderer_;
    OrbitCamera camera_;
    BrushSession* session_ = nullptr;
    ToolMode mode_ = ToolMode::Camera;
    Drag drag_ = Drag::None;
    QPoint last_;
    QPointF pressAt_;
    bool paintMoved_ = false;  // a brush's Ctrl drag (Drag::Paint, Lasso) moved: it paints (else a click)
    std::vector<QPointF> lasso_;  // a brush's Ctrl+Shift drag so far
    float dragStart_ = 0.0f;  // the radius or strength when its drag started
    bool bHeld_ = false;      // B is down: a left drag sizes the brush
    bool bDragged_ = false;   // ... and did, so releasing B keeps the mode
    bool mHeld_ = false;      // M is down: a left drag sets the brush strength
    bool yHeld_ = false;      // Y is down: a left press opens the marking menu
    float tabletPressure_ = 1.0f;
    bool tabletDown_ = false;  // a pen is pressed: its pressure scales the stroke
    bool initialized_ = false;
    QElapsedTimer frameTimer_;
    bool continuous_ = false;  // the last frame was asked by the renderer, not a change
    double lastFrameMs_ = 0.0;
    double lastRenderCpuMs_ = 0.0;
    double lastGpuMs_ = 0.0;
    // After a reduced frame: the full picture once nothing moved for this long.
    QTimer* settle_ = nullptr;
    static constexpr int kSettleMs = 150;
};

}  // namespace groomlab::app
