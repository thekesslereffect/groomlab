// SPDX-License-Identifier: Apache-2.0
// A scalp mirrored across its own surface (scalp_mirror.h).

#include "groomlab/scalp_mirror.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

#include "groomlab/point_index.h"
#include "groomlab/random.h"
#include "groomlab/process_caches.h"

namespace groomlab {

// Where a point's nearest surface spot goes, and the linear map carrying offsets from it: the
// triangle's edges and normal to its partner triangle's edges and normal.
struct ScalpMirror::Frame {
    Vec3f from, to;
    Vec3f r0, r1, r2;  // the rows of [e1 e2 n]^-1
    Vec3f c0, c1, c2;  // the partner's e1', e2', n'
    Vec3f apply(const Vec3f& v) const { return c0 * r0.dot(v) + c1 * r1.dot(v) + c2 * r2.dot(v); }
};

ScalpMirror::ScalpMirror(const ScalpMesh& scalp)
    : points_(scalp.pointCount()), triangles_(scalp.triangleCount()), key_(scalp.restSurfaceHash()) {
    partners_.assign(points_, StrandRoot{});
    pairs_.assign(points_, -1);
    if (points_ == 0 || triangles_ == 0) return;
    const ScalpMesh rest = scalp.restPose();
    const std::vector<Vec3f>& pts = rest.points();
    Vec3f lo = pts.front(), hi = pts.front();
    for (const Vec3f& p : pts) {
        lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    const float middle = 0.5f * (lo.x + hi.x);
    // A vertex this close to the mirrored spot is its partner (a symmetric mesh's twin).
    const float snap = 1e-4f * std::max((hi - lo).length(), 1e-6f);
    middle_ = middle;
    seam_ = snap;
    // Each vertex at one corner of a triangle using it: a partner vertex as a surface point.
    std::vector<StrandRoot> corner(points_);
    const std::vector<ScalpMesh::Triangle>& tris = rest.triangles();
    for (std::size_t f = tris.size(); f-- > 0;) {
        for (int k = 0; k < 3; ++k) {
            const std::size_t v = static_cast<std::size_t>(tris[f][static_cast<std::size_t>(k)]);
            if (v >= points_) continue;
            corner[v] = StrandRoot{static_cast<std::int32_t>(f), k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f};
        }
    }
    const PointIndex index(pts);
    for (std::size_t i = 0; i < points_; ++i) {
        const Vec3f m(2.0f * middle - pts[i].x, pts[i].y, pts[i].z);
        float d2 = 0.0f;
        const std::size_t j = index.nearest(m, &d2);
        if (j < points_ && d2 <= snap * snap && corner[j].face >= 0) {
            pairs_[i] = static_cast<int>(j);
            partners_[i] = corner[j];
            continue;
        }
        const SurfacePoint sp = rest.closestPoint(m);
        partners_[i] = sp.root.face >= 0 ? sp.root : corner[i];
    }
}

std::shared_ptr<const ScalpMirror> ScalpMirror::of(const ScalpMesh& scalp) {
    // The last few scalps' mirrors (a stroke asks per stamp; a host may brush two scalps).
    static std::mutex mutex;
    static std::vector<std::shared_ptr<const ScalpMirror>> kept;
    static const bool registered = (registerProcessCache("scalp mirrors", [] {
                                        std::vector<std::shared_ptr<const ScalpMirror>> drop;
                                        const std::lock_guard<std::mutex> lock(mutex);
                                        drop.swap(kept);
                                    }),
                                    true);
    (void)registered;
    const std::uint64_t key = scalp.restSurfaceHash();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const auto& m : kept) {
            if (m->key_ == key && m->fits(scalp)) return m;
        }
    }
    auto made = std::make_shared<const ScalpMirror>(scalp);
    const std::lock_guard<std::mutex> lock(mutex);
    kept.insert(kept.begin(), made);
    if (kept.size() > 4) kept.resize(4);
    return made;
}

int ScalpMirror::side(const ScalpMesh& scalp, const Vec3f& p, const StrandRoot* root) const {
    if (!fits(scalp)) return 0;
    StrandRoot at = root && scalp.contains(*root) ? *root : scalp.closestPoint(p).root;
    if (!scalp.contains(at)) return 0;
    // Where it is on the rest pose: the side does not change as the head turns.
    const std::vector<Vec3f>& rest = scalp.hasRest() ? scalp.restPoints() : scalp.points();
    const ScalpMesh::Triangle& tri = scalp.triangles()[static_cast<std::size_t>(at.face)];
    const float w = 1.0f - at.u - at.v;
    const float x = rest[static_cast<std::size_t>(tri[0])].x * w + rest[static_cast<std::size_t>(tri[1])].x * at.u +
                    rest[static_cast<std::size_t>(tri[2])].x * at.v;
    const float d = x - middle_;
    return std::abs(d) <= seam_ ? 0 : (d > 0.0f ? 1 : -1);
}

bool ScalpMirror::fits(const ScalpMesh& scalp) const {
    return scalp.pointCount() == points_ && scalp.triangleCount() == triangles_ && points_ > 0;
}

std::size_t ScalpMirror::pairedCount() const {
    return static_cast<std::size_t>(std::count_if(pairs_.begin(), pairs_.end(), [](int j) { return j >= 0; }));
}

bool ScalpMirror::frameAt(const ScalpMesh& scalp, const Vec3f& p, Frame& out) const {
    if (!fits(scalp)) return false;
    const SurfacePoint sp = scalp.closestPoint(p);
    if (sp.root.face < 0) return false;
    const ScalpMesh::Triangle& tri = scalp.triangles()[static_cast<std::size_t>(sp.root.face)];
    const float w[3] = {1.0f - sp.root.u - sp.root.v, sp.root.u, sp.root.v};
    Vec3f to[3], toNormal;
    out.to = Vec3f{};
    for (int k = 0; k < 3; ++k) {
        const StrandRoot& partner = partners_[static_cast<std::size_t>(tri[static_cast<std::size_t>(k)])];
        to[k] = scalp.position(partner);
        out.to += to[k] * w[k];
        toNormal += scalp.normal(partner) * w[k];
    }
    out.from = sp.position;
    const std::vector<Vec3f>& pts = scalp.points();
    const Vec3f a = pts[static_cast<std::size_t>(tri[0])], b = pts[static_cast<std::size_t>(tri[1])], c = pts[static_cast<std::size_t>(tri[2])];
    const Vec3f e1 = b - a, e2 = c - a, n = scalp.normal(sp.root);
    const Vec3f n2 = toNormal.normalized();
    const float det = e1.dot(e2.cross(n));
    const Vec3f f1 = to[1] - to[0], f2 = to[2] - to[0];
    const float scale = e1.length() * e2.length();
    if (std::fabs(det) > 1e-8f * scale && f1.cross(f2).length() > 1e-6f * scale && n2.length() > 0.5f) {
        out.r0 = e2.cross(n) / det;
        out.r1 = n.cross(e1) / det;
        out.r2 = e1.cross(e2) / det;
        out.c0 = f1;
        out.c1 = f2;
        out.c2 = n2;
        return true;
    }
    // A sliver, or a partner triangle squashed flat: the reflection taking the normal to the
    // partner's (world x when they agree).
    Vec3f k = (n - n2).normalized();
    if (k.length() < 0.5f) k = Vec3f(1.0f, 0.0f, 0.0f);
    out.r0 = Vec3f(1.0f, 0.0f, 0.0f) - k * (2.0f * k.x);
    out.r1 = Vec3f(0.0f, 1.0f, 0.0f) - k * (2.0f * k.y);
    out.r2 = Vec3f(0.0f, 0.0f, 1.0f) - k * (2.0f * k.z);
    out.c0 = Vec3f(1.0f, 0.0f, 0.0f);
    out.c1 = Vec3f(0.0f, 1.0f, 0.0f);
    out.c2 = Vec3f(0.0f, 0.0f, 1.0f);
    return true;
}

Vec3f ScalpMirror::point(const ScalpMesh& scalp, const Vec3f& p) const {
    Frame f;
    if (!frameAt(scalp, p, f)) return p;
    return f.to + f.apply(p - f.from);
}

Vec3f ScalpMirror::vector(const ScalpMesh& scalp, const Vec3f& at, const Vec3f& v) const {
    Frame f;
    if (!frameAt(scalp, at, f)) return v;
    return f.apply(v);
}

}  // namespace groomlab
