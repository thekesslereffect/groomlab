// SPDX-License-Identifier: Apache-2.0
#pragma once

// The viewport's camera: an orbit camera around a target (Maya-like: Alt+left orbits, Alt+middle
// pans, Alt+right or the wheel dollies), y up, in the scene's units. Pure math over Qt's vector
// types, so the tests drive it without a window.

#include <algorithm>

#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QVector3D>

#include "groomlab/groom_session.h"

namespace groomlab::app {

class OrbitCamera {
public:
    OrbitCamera();

    // Spherical placement around the target: yaw turns around y (degrees), pitch tilts
    // (degrees, clamped short of the poles), distance along the view axis.
    void orbit(float yawDegrees, float pitchDegrees);
    // Moves the target in the view's plane by pixel deltas: what the point at the target's depth
    // moves on a viewport `heightPixels` tall.
    void pan(float dxPixels, float dyPixels, int heightPixels);
    // Multiplies the distance (a wheel tick: 0.9 in, 1.1 out).
    void dolly(float factor);
    // Fits the bounds: the target at their center, the distance so the bounding sphere fills
    // the view. Invalid bounds: a default view of the origin.
    void frame(const Bounds& bounds);

    QVector3D eye() const;
    const QVector3D& target() const { return target_; }
    // The target and the distance set outright (a close-up on a point: the CLI's --look-at).
    void setTarget(const QVector3D& target) { target_ = target; }
    void setDistance(float distance) { distance_ = std::max(distance, 1e-4f); }
    float distance() const { return distance_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    // The vertical field of view as shown (looking through a photo: for the last projection's
    // aspect, narrowed by the 2D view's zoom).
    float fovDegrees() const;
    void setFovDegrees(float fov) { fov_ = fov; }

    // ---- Looking through a reference photo's camera (the Photos tab's Look Through) ---------------
    // The view becomes the photo's camera: at `eye`, turned by `rotation` (camera axes to world:
    // it looks down its -Z, +Y up, roll and all), its field of view the photo's (`focalPx` over a
    // `width` x `height` image) fitted inside the frame at the photo's aspect (as the renderer
    // fits the photo: GroomRenderer::photoRect). `pivot`: how far ahead the camera orbits about.
    // Locked: orbit, pan, dolly and frame do nothing; unlocked they move the photo's camera (its
    // focal length kept), which lookThroughPose() reads back. The orbit camera before it comes
    // back with leaveLookThrough().
    void setLookThrough(const QVector3D& eye, const QQuaternion& rotation, double focalPx, double width, double height, float pivot,
                        bool locked);
    void setLookThroughLocked(bool locked);
    void leaveLookThrough();
    bool lookingThrough() const { return look_.on; }
    bool lookThroughLocked() const { return look_.on && look_.locked; }
    // The photo's camera as it is now (moved by an unlocked orbit, pan or dolly).
    QVector3D lookThroughEye() const { return look_.eye; }
    QQuaternion lookThroughRotation() const { return look_.rotation; }

    // ---- The 2D view: zooming into a locked photo's view without moving its camera -----------------
    // The picture (the photo and the scene through its camera alike) magnified `zoom2D` times about
    // `center2D`, a point of the unzoomed frame in normalized device coordinates (y up), which then
    // shows at the frame's middle: a frame point p of the unzoomed view shows at (p - center) * zoom.
    // The projection (and so every pick and brush through it) and fovDegrees carry it; the camera
    // does not move. Only while looking through a locked photo; leaving, unlocking or another photo
    // takes it back to 1x.
    // Zooms by `factor` keeping the frame point under (ndcX, ndcY) (as shown, y up) where it is.
    void zoom2D(float factor, float ndcX, float ndcY);
    // Moves the picture by pixel deltas (right, down) on a `width` x `height` frame.
    void pan2D(float dxPixels, float dyPixels, int widthPixels, int heightPixels);
    void reset2D();
    bool zoomed2D() const { return look_.on && (look_.zoom != 1.0f || look_.center[0] != 0.0f || look_.center[1] != 0.0f); }
    float zoom2D() const { return look_.on ? look_.zoom : 1.0f; }
    QPointF center2D() const { return look_.on ? QPointF(look_.center[0], look_.center[1]) : QPointF(); }
    // A point of the unzoomed frame (normalized device coordinates, y up) where it shows now.
    QPointF shown2D(const QPointF& ndc) const;
    // The vertical field of view as the camera has it, without the 2D view's zoom (a render of the
    // view, a placed camera).
    float cameraFovDegrees() const { return look_.on ? lookFov(lastAspect_) : fov_; }

    QMatrix4x4 view() const;
    // A perspective projection for the aspect; the near and far planes follow the distance so a
    // groom in centimeters and one in meters both resolve. `clipCorrection` is the RHI's
    // clipSpaceCorrMatrix (QRhi: OpenGL's clip space to the backend's).
    QMatrix4x4 projection(float aspect, const QMatrix4x4& clipCorrection = QMatrix4x4()) const;
    QMatrix4x4 viewProjection(float aspect, const QMatrix4x4& clipCorrection = QMatrix4x4()) const {
        return projection(aspect, clipCorrection) * view();
    }

private:
    QVector3D forward() const;  // the look-through camera's view direction
    float lookFov(float aspect) const;  // the look-through camera's vertical field of view for a frame of that aspect
    QVector3D target_{0.0f, 0.0f, 0.0f};
    float distance_ = 10.0f;
    float yaw_ = 30.0f;
    float pitch_ = 20.0f;
    float fov_ = 40.0f;
    struct LookThrough {
        bool on = false, locked = false;
        QVector3D eye;
        QQuaternion rotation;
        double focalPx = 1.0, width = 1.0, height = 1.0;
        float zoom = 1.0f, center[2] = {0.0f, 0.0f};  // the 2D view
        // The orbit camera to come back to.
        QVector3D target;
        float distance = 10.0f, yaw = 30.0f, pitch = 20.0f, fov = 40.0f;
    } look_;
    mutable float lastAspect_ = 1.0f;  // the last projection's (fovDegrees while looking through)
};

}  // namespace groomlab::app
