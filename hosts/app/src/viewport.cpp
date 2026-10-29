// SPDX-License-Identifier: Apache-2.0
#include "viewport.h"

#include <algorithm>
#include <cstdlib>

#include <QByteArray>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTabletEvent>
#include <QTimer>
#include <QWheelEvent>

#include "brush_session.h"

namespace groomlab::app {

namespace {

// GROOMLAB_APP_RHI picks the API (d3d11, d3d12, vulkan, opengl, metal, null); else Qt's choice
// for the platform (Direct3D 11 on Windows, Metal on macOS, OpenGL elsewhere).
bool apiFromEnvironment(QRhiWidget::Api* api) {
    const QByteArray name = qgetenv("GROOMLAB_APP_RHI").toLower();
    if (name.isEmpty()) return false;
    if (name == "d3d11" || name == "direct3d11") *api = QRhiWidget::Api::Direct3D11;
    else if (name == "d3d12" || name == "direct3d12") *api = QRhiWidget::Api::Direct3D12;
    else if (name == "vulkan") *api = QRhiWidget::Api::Vulkan;
    else if (name == "opengl" || name == "gl") *api = QRhiWidget::Api::OpenGL;
    else if (name == "metal") *api = QRhiWidget::Api::Metal;
    else if (name == "null") *api = QRhiWidget::Api::Null;
    else return false;
    return true;
}

unsigned strokeModifiers(const QMouseEvent* event) {
    return (event->modifiers().testFlag(Qt::ControlModifier) ? kStrokeCtrl : 0u) |
           (event->modifiers().testFlag(Qt::ShiftModifier) ? kStrokeShift : 0u);
}

SelectMode selectModeOf(const QMouseEvent* event) {
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier), shift = event->modifiers().testFlag(Qt::ShiftModifier);
    if (ctrl && shift) return SelectMode::Add;
    if (ctrl) return SelectMode::Deselect;
    if (shift) return SelectMode::Toggle;
    return SelectMode::Replace;
}

}  // namespace

Viewport::Viewport(QWidget* parent) : QRhiWidget(parent) {
    QRhiWidget::Api api;
    if (apiFromEnvironment(&api)) setApi(api);
    setSampleCount(4);  // smoother lines and overlays; Qt falls back when the API refuses it
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
    // A reduced frame (the adaptive interactive quality) is followed by the full picture once
    // nothing moved for a while: each reduced frame starts the wait over.
    settle_ = new QTimer(this);
    settle_->setSingleShot(true);
    settle_->setInterval(kSettleMs);
    connect(settle_, &QTimer::timeout, this, [this] {
        LookdevRenderer& lookdev = renderer_.lookdevRenderer();
        if (!lookdev.lastFrameReduced()) return;
        lookdev.settle();
        update();
    });
}

void Viewport::setInteracting(bool on) {
    LookdevRenderer& lookdev = renderer_.lookdevRenderer();
    lookdev.setInteracting(on);
    // The drag's end: its last frame was reduced, the full one follows at once (the settle
    // wait is for streams without an end of their own: the wheel, a scripted change).
    if (!on && lookdev.lastFrameReduced()) {
        lookdev.settle();
        update();
    }
}

Viewport::~Viewport() = default;

void Viewport::setFrame(const DisplayFrame& frame) {
    renderer_.setFrame(frame);
    continuous_ = false;
    update();
}

void Viewport::setScalp(MeshPtr scalp) {
    renderer_.setScalp(std::move(scalp));
    continuous_ = false;
    update();
}

void Viewport::frameAll(const Bounds& bounds) {
    camera_.frame(bounds);
    cameraMoved();
}

void Viewport::cameraMoved() {
    renderer_.invalidateAccumulation();
    continuous_ = false;
    emit cameraChanged();
    update();
}

QString Viewport::apiName() const {
    switch (api()) {
        case Api::Direct3D11: return QStringLiteral("Direct3D 11");
        case Api::Direct3D12: return QStringLiteral("Direct3D 12");
        case Api::Vulkan: return QStringLiteral("Vulkan");
        case Api::Metal: return QStringLiteral("Metal");
        case Api::OpenGL: return QStringLiteral("OpenGL");
        case Api::Null: return QStringLiteral("Null");
    }
    return QStringLiteral("?");
}

void Viewport::initialize(QRhiCommandBuffer* /*cb*/) {
    // Pipelines are made on the first render for the render target given there.
    initialized_ = true;
}

void Viewport::render(QRhiCommandBuffer* cb) {
    if (!rhi() || !renderTarget()) return;
    if (continuous_ && frameTimer_.isValid()) lastFrameMs_ = frameTimer_.nsecsElapsed() / 1e6;
    frameTimer_.start();
    QElapsedTimer cpu;
    cpu.start();
    renderer_.render(rhi(), cb, renderTarget(), camera_);
    lastRenderCpuMs_ = cpu.nsecsElapsed() / 1e6;
    lastGpuMs_ = cb->lastCompletedGpuTime() * 1000.0;
    // Refining: the next frame right after this one (from the event loop, not from here).
    continuous_ = renderer_.wantsAnotherFrame();
    if (continuous_) QTimer::singleShot(0, this, [this] { update(); });
    if (renderer_.lookdevActive() && renderer_.lookdevRenderer().lastFrameReduced()) settle_->start();
    emit frameDrawn();
}

void Viewport::releaseResources() {
    renderer_.releaseResources();
    initialized_ = false;
}

void Viewport::resizeEvent(QResizeEvent* event) {
    renderer_.invalidateAccumulation();
    continuous_ = false;
    QRhiWidget::resizeEvent(event);
}

// ---- Tools -------------------------------------------------------------------------------------

void Viewport::setBrushSession(BrushSession* session) {
    session_ = session;
    syncView();
}

void Viewport::setToolMode(ToolMode mode) {
    if (mode != ToolMode::Camera && !session_) mode = ToolMode::Camera;
    if (mode == mode_) return;
    if (session_ && session_->stroking()) session_->cancel();
    mode_ = mode;
    drag_ = Drag::None;
    // The hover's ring and lines follow the cursor without a button in brush mode.
    setMouseTracking(mode_ == ToolMode::Brush);
    setCursor(mode_ == ToolMode::Camera ? Qt::ArrowCursor : Qt::CrossCursor);
    if (session_ && mode_ != ToolMode::Brush) session_->clearHover();
    refreshOverlay();
    emit toolModeChanged(mode_);
}

void Viewport::syncView() {
    if (session_) session_->setView(camera_, width(), height());
}

void Viewport::refreshOverlay() {
    static const DisplayGeometry none;
    if (session_ && mode_ != ToolMode::Camera) {
        renderer_.setToolOverlay(session_->overlay(), session_->overlayVersion());
    } else {
        renderer_.setToolOverlay(none, 0);
    }
    update();
}

float Viewport::pressure() const {
    if (!session_ || !session_->settings().pressure) return 1.0f;
    return tabletDown_ ? tabletPressure_ : 1.0f;
}

void Viewport::tabletEvent(QTabletEvent* event) {
    // The pen's pressure is kept for the mouse events Qt synthesizes from the pen (the event
    // is left unaccepted so they come), as the stroke's pressure while the pen is down.
    tabletPressure_ = std::clamp(static_cast<float>(event->pressure()), 0.0f, 1.0f);
    if (event->type() == QEvent::TabletPress) tabletDown_ = true;
    else if (event->type() == QEvent::TabletRelease) tabletDown_ = false;
    event->ignore();
}

bool Viewport::cameraPress(QMouseEvent* event, bool alt) {
    // Maya's camera: Alt+left orbits, Alt+middle pans, Alt+right dollies. Without Alt, in camera
    // mode, the plain buttons do the same; in the tools' modes the right button still orbits.
    if (event->button() == Qt::LeftButton && (alt || mode_ == ToolMode::Camera)) drag_ = Drag::Orbit;
    else if (event->button() == Qt::MiddleButton && (alt || mode_ == ToolMode::Camera)) drag_ = Drag::Pan;
    else if (event->button() == Qt::RightButton) drag_ = alt ? Drag::Dolly : Drag::Orbit;
    else return false;
    renderer_.lookdevRenderer().setInteracting(true);  // the drag's frames reduced from its first
    return true;
}

void Viewport::mousePressEvent(QMouseEvent* event) {
    last_ = event->position().toPoint();
    pressAt_ = event->position();
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);
    syncView();
    event->accept();
    if (yHeld_ && event->button() == Qt::LeftButton && !alt) {
        // Y + click: the marking menu (Maya's GroomLab hotkey set), in any tool.
        drag_ = Drag::Menu;
        emit markingMenuPressed(event->position());
        return;
    }
    if (alt || mode_ == ToolMode::Camera || !session_) {
        cameraPress(event, alt);
        return;
    }
    if (mode_ == ToolMode::Brush) {
        const bool slider = event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && (bHeld_ || mHeld_));
        if (slider) {
            // Middle-drag (or B held + left-drag) left/right resizes the brush; with Shift (or M
            // held + left-drag) it changes the strength, as GroomLab's Maya context does.
            const bool strength = event->modifiers().testFlag(Qt::ShiftModifier) || (mHeld_ && !bHeld_);
            drag_ = strength ? Drag::Strength : Drag::Radius;
            dragStart_ = strength ? session_->settings().brush.strength : session_->settings().screenRadius;
            bDragged_ = bHeld_;
            return;
        }
        if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier) && session_->settings().shiftSelects()) {
            // Shift on a guide brush: selecting guides without leaving the brush (a drag paints them
            // in with the ring, a click adds the guides in it).
            drag_ = Drag::Paint;
            paintMoved_ = false;
            return;
        }
        if (event->button() == Qt::LeftButton) {
            drag_ = Drag::Stroke;
            if (!session_->press(event->position(), pressure(), strokeModifiers(event))) {
                drag_ = Drag::None;
                if (!session_->lastError().empty()) emit message(QString::fromStdString(session_->lastError()));
                return;
            }
            renderer_.lookdevRenderer().setInteracting(true);
            refreshOverlay();
            if (session_->stroking()) {
                emit strokeStarted();
                emit strokeDabbed();
            }
            return;
        }
        cameraPress(event, false);
        return;
    }
    // Select mode: a click or a box with the left button.
    if (event->button() == Qt::LeftButton) {
        drag_ = Drag::Box;
        return;
    }
    cameraPress(event, false);
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
    const QPoint now = event->position().toPoint();
    const QPoint delta = now - last_;
    last_ = now;
    if (drag_ == Drag::Menu) {
        event->accept();
        emit markingMenuDragged(event->position());
        return;
    }
    if (drag_ == Drag::None) {
        // Hovering in brush mode: the ring and the guides under the brush follow the cursor.
        if (mode_ == ToolMode::Brush && session_ && !(event->buttons() & Qt::AllButtons)) {
            syncView();
            session_->hover(event->position());
            refreshOverlay();
        }
        return;
    }
    event->accept();
    switch (drag_) {
        case Drag::Orbit:
            camera_.orbit(-delta.x() * 0.4f, delta.y() * 0.4f);
            cameraMoved();
            break;
        case Drag::Pan:
            camera_.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()), height());
            cameraMoved();
            break;
        case Drag::Dolly:
            camera_.dolly(1.0f - (delta.x() + delta.y()) * 0.005f);  // as Maya: right or down zooms in
            cameraMoved();
            break;
        case Drag::Stroke:
            if (!session_) break;
            session_->move(event->position(), pressure());
            if (!session_->lastError().empty()) {
                drag_ = Drag::None;
                emit message(QString::fromStdString(session_->lastError()));
                break;
            }
            refreshOverlay();
            if (session_->stroking()) emit strokeDabbed();
            break;
        case Drag::Radius:
        case Drag::Strength: {
            if (!session_) break;
            const float dx = static_cast<float>(event->position().x() - pressAt_.x());
            BrushToolSettings& s = session_->settings();
            if (drag_ == Drag::Radius) s.screenRadius = std::clamp(dragStart_ + dx, 5.0f, 2000.0f);
            else s.brush.strength = std::clamp(dragStart_ + dx * 0.01f, -10.0f, 10.0f);
            // The ring where the drag started, at its new size.
            session_->hover(pressAt_);
            refreshOverlay();
            emit brushSettingsChanged();
            break;
        }
        case Drag::Box:
            if (!session_) break;
            syncView();
            session_->showBox(pressAt_, event->position());
            refreshOverlay();
            break;
        case Drag::Paint: {
            if (!session_) break;
            syncView();
            // Once it moves it paints (from the press on): the guides under the ring join the selection.
            std::size_t painted = 0;
            if (!paintMoved_ && (event->position() - pressAt_).manhattanLength() >= 4.0) {
                paintMoved_ = true;
                painted += session_->paintSelection(pressAt_);
            }
            if (paintMoved_) painted += session_->paintSelection(event->position());
            session_->hover(event->position());
            refreshOverlay();
            if (painted) emit selectionChanged();  // the guides drawn as selected now
            break;
        }
        case Drag::None:
        case Drag::Menu:
            break;
    }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
    const Drag drag = drag_;
    drag_ = Drag::None;
    event->accept();
    if (drag == Drag::Menu) {
        emit markingMenuReleased(event->position());
        return;
    }
    // A camera drag's end: the full picture (a stroke's comes with its commit, redrawn by the
    // host; the brush's hover after it draws it too).
    if (drag != Drag::None) setInteracting(false);
    if (!session_) return;
    switch (drag) {
        case Drag::Stroke: {
            emit strokeReleasing();
            const StrokeRelease released = session_->release();
            if (!session_->lastError().empty()) emit message(QString::fromStdString(session_->lastError()));
            for (const std::string& warning : session_->takeWarnings()) emit message(QString::fromStdString(warning));
            session_->hover(event->position());
            refreshOverlay();
            if (released == StrokeRelease::Stroke) emit strokeEnded();
            break;
        }
        case Drag::Box: {
            // A click when the box is tiny, else the box's corners.
            const QPointF at = event->position();
            const bool click = std::abs(at.x() - pressAt_.x()) < 3.0 && std::abs(at.y() - pressAt_.y()) < 3.0;
            syncView();
            session_->select(click ? std::vector<QPointF>{at} : std::vector<QPointF>{pressAt_, at}, selectModeOf(event), true);
            session_->clearHover();
            refreshOverlay();
            emit selectionChanged();
            break;
        }
        case Drag::Paint:
            syncView();
            if (paintMoved_) {
                session_->soloSelection();  // the rest frozen, one undo step (the Select tool's solo)
            } else if (session_->paintSelection(pressAt_, SelectMode::Add, true) == 0) {
                // A click: the guides in the ring added, the rest frozen; on none, nothing selected
                // and every guide unfrozen.
                session_->paintSelection(pressAt_, SelectMode::Replace, true);
            }
            session_->hover(event->position());
            refreshOverlay();
            emit selectionChanged();
            break;
        case Drag::Radius:
        case Drag::Strength:
            emit brushSettingsChanged();
            break;
        default:
            break;
    }
}

void Viewport::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) return;
    camera_.dolly(steps > 0 ? 0.9f : 1.1f);
    for (int i = 1; i < std::abs(steps); ++i) camera_.dolly(steps > 0 ? 0.9f : 1.1f);
    cameraMoved();
    event->accept();
}

void Viewport::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    switch (event->key()) {
        case Qt::Key_B:
            // B: brush mode; held, a left drag sizes the brush (the mode stays as it was then).
            bHeld_ = true;
            bDragged_ = false;
            if (mode_ != ToolMode::Brush) setToolMode(ToolMode::Brush);
            event->accept();
            return;
        case Qt::Key_M:
            // M held: a left drag sets the strength (in brush mode, entered as B does).
            mHeld_ = true;
            if (mode_ != ToolMode::Brush) setToolMode(ToolMode::Brush);
            event->accept();
            return;
        case Qt::Key_Y:
            // Y held: the next left press opens the marking menu.
            yHeld_ = true;
            event->accept();
            return;
        case Qt::Key_S:
            setToolMode(mode_ == ToolMode::Select ? ToolMode::Camera : ToolMode::Select);
            event->accept();
            return;
        case Qt::Key_Escape:
            if (session_ && session_->stroking()) session_->cancel();
            setToolMode(ToolMode::Camera);
            event->accept();
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            emit deleteRequested();
            event->accept();
            return;
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            // The brush level up (parents) or down (children), as Maya's hotkeys do; -1 is every level.
            if (session_) {
                int& level = session_->settings().brush.level;
                level = std::clamp(level + (event->key() == Qt::Key_PageUp ? -1 : 1), -1, 7);
                emit brushSettingsChanged();
                refreshOverlay();
            }
            event->accept();
            return;
        default:
            break;
    }
    QRhiWidget::keyPressEvent(event);
}

void Viewport::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_B && !event->isAutoRepeat()) {
        bHeld_ = false;
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_M && !event->isAutoRepeat()) {
        mHeld_ = false;
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Y && !event->isAutoRepeat()) {
        yHeld_ = false;
        event->accept();
        return;
    }
    QRhiWidget::keyReleaseEvent(event);
}

void Viewport::leaveEvent(QEvent* event) {
    if (session_ && drag_ == Drag::None) {
        session_->clearHover();
        refreshOverlay();
    }
    QRhiWidget::leaveEvent(event);
}

}  // namespace groomlab::app
