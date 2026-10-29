// SPDX-License-Identifier: Apache-2.0
#pragma once

// Screen-space brushing without a host: a view's projection (world -> port pixels), what the
// scalp hides from that view's camera, the interactive brush's weight under a screen circle, a
// conservative test of whether that circle can reach a box, and guide points binned on screen
// for hover queries. A host (Maya's brush tool) only reads its camera into these.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "groomlab/brush_alpha.h"
#include "groomlab/brush.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

// A view's projection: world points to port pixels (origin bottom left), from the world-to-clip
// matrix in Maya's convention (row vectors: clip = [x y z 1] * M, M = modelView * projection,
// the matrix's rows first) and the port's size. Const and thread-safe.
class ViewProjection {
public:
    ViewProjection() = default;
    ViewProjection(const std::array<double, 16>& worldToClip, double width, double height);

    // A matrix and a port of positive size were given.
    bool valid() const { return valid_; }
    double width() const { return width_; }
    double height() const { return height_; }
    const std::array<double, 16>& matrix() const { return m_; }
    // The clip-to-world matrix (rows first); meaningful when invertible().
    bool invertible() const { return invertible_; }
    const std::array<double, 16>& inverse() const { return inverse_; }

    // The point in port pixels; false behind the camera (clip w <= 1e-9).
    bool project(const Vec3f& p, float& x, float& y) const;
    // The point in whole port pixels, as Maya's M3dView::worldToView gives them: false unless it
    // lies inside the view volume (|x|, |y| and |z| at most clip w, in front of the camera), the
    // pixels truncated toward zero.
    bool projectPixel(const Vec3f& p, int& x, int& y) const;
    // The world points under port pixel (x, y) at the near and far clip planes; false when the
    // matrix has no inverse.
    bool unproject(float x, float y, Vec3f& nearPoint, Vec3f& farPoint) const;
    // The pixel bounds of the box's projection: false when some of the box is not in front of the
    // camera (no bound then). The box's corners in front, its projection lies in their hull.
    bool projectBox(const Vec3f& lo, const Vec3f& hi, float& x0, float& y0, float& x1, float& y1) const;
    // The matrix and port size: equal views, equal hashes.
    std::uint64_t hash() const;

private:
    std::array<double, 16> m_{};
    std::array<double, 16> inverse_{};
    double width_ = 0.0, height_ = 0.0;
    bool valid_ = false, invertible_ = false;
};

// What the scalp hides from a camera: a point is hidden when the ray from the eye (or, for an
// orthographic camera, from far in front along the view) meets the scalp before it. Points are
// answered by cell (1/150 of the scalp's size): the ray goes to the cell's center, and the scalp
// must be met a little more than a cell before the center (so a cell holding the visible surface
// is visible whichever way the surface crosses it). The answer depends only on the cell, never
// on which points were asked first, and is kept: a stroke's dabs, or a hover's mouse moves, cast
// one ray per cell. Near the silhouette a cell's points take one answer. Thread-safe.
class ScalpVisibility {
public:
    ScalpVisibility(std::shared_ptr<const ScalpMesh> scalp, const Vec3f& eye, const Vec3f& viewDirection, bool orthographic);

    bool hidden(const Vec3f& p) const;
    // What the answers depend on: the scalp's surface and the camera. Equal keys, equal answers.
    std::uint64_t key() const { return key_; }
    static std::uint64_t keyOf(const ScalpMesh& scalp, const Vec3f& eye, const Vec3f& viewDirection, bool orthographic);
    float cellSize() const { return cell_; }
    std::size_t cellsCast() const;

private:
    bool cast(std::int64_t ix, std::int64_t iy, std::int64_t iz) const;

    std::shared_ptr<const ScalpMesh> scalp_;
    Vec3f eye_, along_;
    bool ortho_ = false;
    float cell_ = 1.0f;
    std::uint64_t key_ = 0;
    // The answers kept, in shards by cell (each with its own lock: dabs weighing guides in
    // parallel do not queue on one).
    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::uint64_t, bool> cells;
    };
    static constexpr std::size_t kShards = 64;
    mutable std::array<Shard, kShards> shards_;
};

// The interactive brush's weight: the falloff of the screen distance from the cursor (x, y) to
// a point's projection within `radius` pixels (as brushFalloff), 0 behind the camera and, with
// `visibility`, where the scalp hides the point; times `alpha` where the point lands in the
// circle (brush_alpha.h) when one is given.
BrushWeightFn screenBrushWeight(const ViewProjection& view, float x, float y, float radius, const BrushFalloff& falloff,
                                std::shared_ptr<const ScalpVisibility> visibility = nullptr, AlphaStamp alpha = {});
// Whether screenBrushWeight's circle can reach a box: false only when every point of the box
// projects farther than `radius` from the cursor (a guide whose bounds it is skips the dab).
BrushReachFn screenBrushReach(const ViewProjection& view, float x, float y, float radius);

// A view as the interactive tools read it (stroke.h, guide picking): the projection to port
// pixels (origin bottom left), the ray under a pixel, and the camera for Mask by scalp. Built
// from a matrix alone (fromMatrix: Python, other hosts), or by a host that also knows its own
// projection and rays (Maya's M3dView): the matrix is then used where the host checked it agrees
// (its weights may run in parallel), the host's functions otherwise.
struct ToolView {
    ViewProjection projection;  // valid: the view's matrix, the projection used
    // The host's projection, used when `projection` is not valid (screen weights then call it
    // from one thread only).
    std::function<bool(const Vec3f& p, float& x, float& y)> hostProject;
    // The host's ray under a pixel (unit direction); without it, the matrix's (unproject).
    std::function<bool(float x, float y, Vec3f& origin, Vec3f& direction)> hostRay;
    // The camera (Mask by scalp): the host's, read when asked, or the fields below.
    std::function<bool(Vec3f& eye, Vec3f& viewDirection, bool& orthographic)> hostCamera;
    bool hasCamera = false;
    Vec3f eye, viewDirection;
    bool orthographic = false;

    // A view of a world-to-clip matrix (ViewProjection's convention) and a port, the camera worked
    // out from the matrix (its eye, or for an orthographic view the direction it looks along).
    static ToolView fromMatrix(const std::array<double, 16>& worldToClip, double width, double height);

    bool project(const Vec3f& p, float& x, float& y) const;
    bool ray(float x, float y, Vec3f& origin, Vec3f& direction) const;
    bool camera(Vec3f& eye, Vec3f& viewDirection, bool& orthographic) const;
    // Screen weights read the matrix (thread-safe: a stamp may weigh in parallel).
    bool parallelWeights() const { return projection.valid(); }
    // screenBrushWeight through this view (the host's projection when the matrix is not valid).
    BrushWeightFn brushWeight(float x, float y, float radius, const BrushFalloff& falloff,
                              std::shared_ptr<const ScalpVisibility> visibility = nullptr, AlphaStamp alpha = {}) const;
    // screenBrushReach, or empty (nothing skipped) without a valid matrix.
    BrushReachFn brushReach(float x, float y, float radius) const;
};

// The world radius a circle of `screenRadius` pixels around (x, y) has at `center`: the ray one
// radius to the right met with the plane through the center facing the camera (0 where it meets
// none). One function for what a brush touches (BrushTool's map dabs) and the ring every host
// draws, so the ring is the brush's edge.
float worldRadiusAt(const ToolView& view, const Vec3f& center, float x, float y, float screenRadius);

// Polylines' points projected once (a view, a set of guides) and binned in screen cells: which
// polylines have a point near a circle, without projecting every point again (hover).
class ScreenPointGrid {
public:
    ScreenPointGrid(const ViewProjection& view, const std::vector<const std::vector<Vec3f>*>& polylines, float cell = 24.0f);
    // The polylines (their indices, ascending) with a point projected within `radius` (+1 pixel)
    // of (x, y); every polyline with a point within `radius` is among them.
    std::vector<std::uint32_t> near(float x, float y, float radius) const;
    std::size_t size() const { return count_; }

private:
    float cell_ = 24.0f;
    float x0_ = 0.0f, y0_ = 0.0f;
    int nx_ = 0, ny_ = 0;
    std::size_t count_ = 0;
    std::vector<std::uint32_t> cellStart_, cellLines_;
};

}  // namespace groomlab
