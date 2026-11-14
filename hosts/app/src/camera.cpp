// SPDX-License-Identifier: Apache-2.0
#include "camera.h"

#include <algorithm>
#include <cmath>

namespace groomlab::app {

namespace {
constexpr float kPi = 3.14159265358979f;
float radians(float degrees) { return degrees * kPi / 180.0f; }
float degrees(float radians) { return radians * 180.0f / kPi; }
}  // namespace

OrbitCamera::OrbitCamera() = default;

void OrbitCamera::orbit(float yawDegrees, float pitchDegrees) {
    if (look_.on) {
        if (look_.locked) return;
        // The photo's camera about the point ahead of it: the yaw about world Y, the pitch about
        // its own right axis (raising the eye for a positive pitch, as the orbit camera does).
        const QVector3D right = look_.rotation.rotatedVector(QVector3D(1.0f, 0.0f, 0.0f));
        const QQuaternion turn = QQuaternion::fromAxisAndAngle(QVector3D(0.0f, 1.0f, 0.0f), yawDegrees) *
                                 QQuaternion::fromAxisAndAngle(right, -pitchDegrees);
        look_.eye = target_ + turn.rotatedVector(look_.eye - target_);
        look_.rotation = (turn * look_.rotation).normalized();
        return;
    }
    yaw_ = std::fmod(yaw_ + yawDegrees, 360.0f);
    pitch_ = std::clamp(pitch_ + pitchDegrees, -89.0f, 89.0f);
}

void OrbitCamera::pan(float dxPixels, float dyPixels, int heightPixels) {
    if (heightPixels <= 0 || (look_.on && look_.locked)) return;
    // The view's height at the target's depth, in scene units per pixel.
    const float unitsPerPixel = 2.0f * distance_ * std::tan(radians(fovDegrees()) * 0.5f) / static_cast<float>(heightPixels);
    const QMatrix4x4 v = view();
    const QVector3D right(v(0, 0), v(0, 1), v(0, 2));
    const QVector3D up(v(1, 0), v(1, 1), v(1, 2));
    const QVector3D move = (-dxPixels * right + dyPixels * up) * unitsPerPixel;
    target_ += move;
    if (look_.on) look_.eye += move;
}

void OrbitCamera::dolly(float factor) {
    if (factor <= 0.0f || (look_.on && look_.locked)) return;
    distance_ = std::max(distance_ * factor, 1e-4f);
    if (look_.on) look_.eye = target_ - forward() * distance_;
}

void OrbitCamera::frame(const Bounds& bounds) {
    if (look_.on && look_.locked) return;
    if (!bounds.valid) {
        target_ = QVector3D(0.0f, 0.0f, 0.0f);
        distance_ = 10.0f;
        if (look_.on) look_.eye = target_ - forward() * distance_;
        return;
    }
    const QVector3D lo(bounds.lo.x, bounds.lo.y, bounds.lo.z), hi(bounds.hi.x, bounds.hi.y, bounds.hi.z);
    target_ = (lo + hi) * 0.5f;
    const float radius = std::max((hi - lo).length() * 0.5f, 1e-3f);
    // The sphere fills the view: tangent of the half field of view at the distance meets the radius.
    distance_ = radius / std::sin(radians(fovDegrees()) * 0.5f) * 1.05f;
    if (look_.on) look_.eye = target_ - forward() * distance_;  // (a photo's camera: its direction kept)
}

QVector3D OrbitCamera::eye() const {
    if (look_.on) return look_.eye;
    const float y = radians(yaw_), p = radians(pitch_);
    const QVector3D dir(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y));
    return target_ + dir * distance_;
}

QVector3D OrbitCamera::forward() const { return look_.rotation.rotatedVector(QVector3D(0.0f, 0.0f, -1.0f)); }

float OrbitCamera::lookFov(float aspect) const {
    // Fitted inside the frame at the photo's aspect: its height spans the frame's when the frame is
    // wider than the photo, else its width spans the frame's width.
    const double photoAspect = look_.width / std::max(look_.height, 1.0);
    double tanHalf = 0.5 * look_.height / std::max(look_.focalPx, 1e-6);
    if (aspect > 0.0f && aspect < photoAspect) tanHalf = 0.5 * look_.width / std::max(look_.focalPx, 1e-6) / aspect;
    return degrees(2.0f * static_cast<float>(std::atan(tanHalf)));
}

void OrbitCamera::setLookThrough(const QVector3D& eye, const QQuaternion& rotation, double focalPx, double width, double height, float pivot,
                                 bool locked) {
    if (!look_.on) {
        reset2D();  // (a new look: the whole view)
        look_.target = target_;
        look_.distance = distance_;
        look_.yaw = yaw_;
        look_.pitch = pitch_;
        look_.fov = fov_;
    }
    look_.on = true;
    look_.locked = locked;
    look_.eye = eye;
    look_.rotation = rotation.normalized();
    look_.focalPx = focalPx > 0.0 ? focalPx : std::max(width, 1.0) * 50.0 / 36.0;
    look_.width = std::max(width, 1.0);
    look_.height = std::max(height, 1.0);
    distance_ = std::max(pivot, 1e-3f);
    target_ = eye + forward() * distance_;
}

void OrbitCamera::setLookThroughLocked(bool locked) {
    if (!look_.on) return;
    if (!locked) reset2D();  // unlocked, the navigation moves the camera again: from the whole view
    look_.locked = locked;
}

float OrbitCamera::fovDegrees() const {
    if (!look_.on) return fov_;
    const float base = lookFov(lastAspect_);
    if (look_.zoom == 1.0f) return base;
    return degrees(2.0f * std::atan(std::tan(radians(base) * 0.5f) / look_.zoom));
}

// ---- The 2D view ------------------------------------------------------------------------------------

namespace {
constexpr float kMinZoom2D = 0.25f, kMaxZoom2D = 64.0f;
}

void OrbitCamera::zoom2D(float factor, float ndcX, float ndcY) {
    if (!look_.on || !look_.locked || !(factor > 0.0f)) return;
    const float zoom = std::clamp(look_.zoom * factor, kMinZoom2D, kMaxZoom2D);
    // The frame point under the cursor (unzoomed), kept under it at the new zoom.
    const float bx = ndcX / look_.zoom + look_.center[0], by = ndcY / look_.zoom + look_.center[1];
    look_.zoom = zoom;
    look_.center[0] = bx - ndcX / zoom;
    look_.center[1] = by - ndcY / zoom;
    if (std::abs(look_.zoom - 1.0f) < 1e-3f && std::abs(look_.center[0]) < 1e-3f && std::abs(look_.center[1]) < 1e-3f) reset2D();
}

void OrbitCamera::pan2D(float dxPixels, float dyPixels, int widthPixels, int heightPixels) {
    if (!look_.on || !look_.locked || widthPixels <= 0 || heightPixels <= 0) return;
    // The picture follows the cursor: a pixel right is 2 / width of the shown frame, y up.
    look_.center[0] -= 2.0f * dxPixels / static_cast<float>(widthPixels) / look_.zoom;
    look_.center[1] += 2.0f * dyPixels / static_cast<float>(heightPixels) / look_.zoom;
}

void OrbitCamera::reset2D() {
    look_.zoom = 1.0f;
    look_.center[0] = look_.center[1] = 0.0f;
}

QPointF OrbitCamera::shown2D(const QPointF& ndc) const {
    if (!look_.on) return ndc;
    return QPointF((ndc.x() - look_.center[0]) * look_.zoom, (ndc.y() - look_.center[1]) * look_.zoom);
}

void OrbitCamera::leaveLookThrough() {
    if (!look_.on) return;
    reset2D();
    look_.on = false;
    target_ = look_.target;
    distance_ = look_.distance;
    yaw_ = look_.yaw;
    pitch_ = look_.pitch;
    fov_ = look_.fov;
}

QMatrix4x4 OrbitCamera::view() const {
    QMatrix4x4 m;
    if (look_.on) {
        m.lookAt(look_.eye, look_.eye + forward(), look_.rotation.rotatedVector(QVector3D(0.0f, 1.0f, 0.0f)));
        return m;
    }
    m.lookAt(eye(), target_, QVector3D(0.0f, 1.0f, 0.0f));
    return m;
}

QMatrix4x4 OrbitCamera::projection(float aspect, const QMatrix4x4& clipCorrection) const {
    QMatrix4x4 m = clipCorrection;
    const float nearPlane = std::max(distance_ * 0.01f, 1e-4f);
    const float farPlane = distance_ * 100.0f;
    lastAspect_ = aspect > 0.0f ? aspect : 1.0f;
    if (look_.on && (look_.zoom != 1.0f || look_.center[0] != 0.0f || look_.center[1] != 0.0f)) {
        // The 2D view: the part of the camera's frame it shows, as an off-center frustum (the
        // camera's frame [-1, 1] in its normalized coordinates spans tanHalf * near either side).
        const float tanY = std::tan(radians(lookFov(lastAspect_)) * 0.5f), tanX = tanY * lastAspect_;
        const float h = 1.0f / look_.zoom;
        m.frustum(nearPlane * tanX * (look_.center[0] - h), nearPlane * tanX * (look_.center[0] + h),
                  nearPlane * tanY * (look_.center[1] - h), nearPlane * tanY * (look_.center[1] + h), nearPlane, farPlane);
        return m;
    }
    m.perspective(fovDegrees(), lastAspect_, nearPlane, farPlane);
    return m;
}

}  // namespace groomlab::app
