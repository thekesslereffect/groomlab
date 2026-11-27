// SPDX-License-Identifier: Apache-2.0
// Guides rooted on the scalp: turned root first, joined to the scalp, kept out of the head. See
// guide_rooting.h.

#include "groomlab/guide_rooting.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "groomlab/curve.h"
#include "groomlab/hair_shell.h"
#include "groomlab/parallel.h"

namespace groomlab {

namespace {

// Which way a mesh's normals point: +1 out of its middle (most of them), -1 in.
float outwardSign(const ScalpMesh& mesh) {
    if (mesh.pointCount() == 0 || mesh.vertexNormals().size() != mesh.pointCount()) return 1.0f;
    Vec3f center(0, 0, 0);
    for (const Vec3f& q : mesh.points()) center += q;
    center = center / static_cast<float>(mesh.pointCount());
    double out = 0.0;
    for (std::size_t i = 0; i < mesh.pointCount(); ++i)
        out += static_cast<double>((mesh.points()[i] - center).dot(mesh.vertexNormals()[i]));
    return out >= 0.0 ? 1.0f : -1.0f;
}

float distanceTo(const ScalpMesh& mesh, const Vec3f& p, SurfacePoint* at = nullptr) {
    const SurfacePoint sp = mesh.closestPoint(p);
    if (at) *at = sp;
    return sp.root.face < 0 ? std::numeric_limits<float>::infinity() : (sp.position - p).length();
}

// The rule: the first point off the scalp, the last clearly nearer it (by half the first's gap), and
// the chain not falling from its start to its end (hair hangs root to tip: a line that ends lower
// than it starts by more than a twentieth of a head radius is hair lying down the head to the
// hairline, root first, not a tip-first one).
bool startsAtTip(const std::vector<Vec3f>& pts, float d0, float d1, const GuideRootingOptions& o, const GuideRootingCounts& s) {
    if (!(d0 > s.snapGap) || !(d1 + std::max(s.snapGap, 0.5f * d0) < d0)) return false;
    const Vec3f up = o.up.length() > 1e-9f ? o.up.normalized() : Vec3f(0, 1, 0);
    return (pts.back() - pts.front()).dot(up) > -0.05f * s.headRadius;
}

RootedGuide rootOne(const std::vector<Vec3f>& in, const ScalpMesh& scalp, const ScalpMesh& head, float sign, const GuideRootingOptions& o,
                    const GuideRootingCounts& s) {
    RootedGuide out;
    if (in.size() < 2) {
        out.droppedShort = true;
        return out;
    }
    std::vector<Vec3f> pts = in;
    // 1. The way: the end nearer the scalp first.
    SurfacePoint near0, near1;
    float d0 = distanceTo(scalp, pts.front(), &near0);
    const float d1 = distanceTo(scalp, pts.back(), &near1);
    if (o.flip && startsAtTip(pts, d0, d1, o, s)) {
        std::reverse(pts.begin(), pts.end());
        std::swap(near0, near1);
        d0 = d1;
        out.flipped = true;
    }
    // 2. The root on the scalp: the nearest point, or (radial) the scalp under the root end toward the
    // head's middle (where the head's surface meets that line; the nearest scalp point to it).
    if (o.radial && d0 > s.snapGap) {
        const Vec3f inward = o.center - pts.front();
        SurfacePoint hit;
        float t = 0.0f;
        if (inward.length() > 1e-9f && head.intersectRay(pts.front(), inward.normalized(), &hit, &t)) {
            const SurfacePoint under = scalp.closestPoint(hit.position);
            if (under.root.face >= 0) {
                near0 = under;
                d0 = (pts.front() - under.position).length();
            }
        }
    }
    out.rootGap = d0;
    if (near0.root.face < 0 || d0 > s.maxGap) {
        out.droppedGap = true;
        return out;
    }
    Vec3f n = near0.normal.length() > 1e-9f ? near0.normal.normalized() : Vec3f(0, 1, 0);
    if ((pts.front() - near0.position).dot(n) < 0.0f) n = -n;  // (toward the root's side)
    if (d0 <= s.snapGap) {
        pts.front() = near0.position;
        out.snapped = d0 > 0.0f;
    } else {
        // (leaving along the scalp's normal: a root off to the side of the nearest point, past the
        // scalp's edge, bends out first)
        const Vec3f off = pts.front() - near0.position;
        const float along = off.dot(n);
        if (along < 0.7f * off.length()) pts.insert(pts.begin(), near0.position + n * std::max(0.5f * s.snapGap, 0.5f * along));
        pts.insert(pts.begin(), near0.position);
        out.connected = true;
    }
    out.root = near0;
    // 3. The head: dips pushed out, passes cut.
    const float eps = 1e-4f * s.headRadius;
    const float keepOut = o.keepOut > 0.0f ? o.keepOut : 0.25f * s.snapGap;
    // The next crossing of the head along the path from point i (its segment's parameter, the hit,
    // whether it goes in), from `from` on segment i.
    struct Crossing {
        std::size_t segment = 0;
        Vec3f at;
        bool entering = false;
        bool found = false;
    };
    auto next = [&](std::size_t i, const Vec3f& from) {
        Crossing c;
        Vec3f start = from;
        for (std::size_t k = i; k + 1 < pts.size(); ++k) {
            const Vec3f seg = pts[k + 1] - start;
            const float len = seg.length();
            if (len > eps) {
                const Vec3f d = seg / len;
                SurfacePoint hit;
                float t = 0.0f;
                // (from just past the start: the root on the scalp is no crossing)
                if (head.intersectRay(start + d * eps, d, &hit, &t) && t + eps < len) {
                    c.segment = k;
                    c.at = start + d * (t + eps);
                    c.entering = d.dot(hit.normal * sign) < 0.0f;
                    c.found = true;
                    return c;
                }
            }
            start = pts[k + 1];
        }
        return c;
    };
    std::size_t i = 0;
    Vec3f from = pts.front();
    for (int guard = 0; guard < 64; ++guard) {
        const Crossing c = next(i, from);
        if (!c.found) break;
        // The stretch inside: from where it enters to where it leaves; a first crossing that leaves
        // says it went in unseen where the walk stood (at the root: it grew into the head).
        std::size_t inSegment = i;
        Vec3f inAt = from;
        Crossing exit = c;
        if (c.entering) {
            inSegment = c.segment;
            inAt = c.at;
            exit = next(c.segment, c.at + (pts[c.segment + 1] - c.at).normalized() * (2.0f * eps));
        }
        // (no way out: the rest of it is inside)
        const bool leaves = exit.found && !exit.entering;
        const std::size_t last = leaves ? exit.segment : pts.size() - 1;
        // (a dip that comes out again: pushed out up to pushDepth; a tip ending inside only when it
        // barely sank in, a hair's width under the surface)
        const float limit = leaves ? s.pushDepth : 2.0f * s.snapGap;
        bool shallow = true;
        for (std::size_t k = inSegment + 1; shallow && k <= last; ++k)
            if (distanceTo(head, pts[k]) > limit) shallow = false;
        Vec3f dip;
        if (shallow && leaves && exit.segment == inSegment) {
            // (a chord through the head between two points outside it: its middle)
            dip = (inAt + exit.at) * 0.5f;
            if (distanceTo(head, dip) > limit) shallow = false;
        }
        if (shallow) {
            if (leaves && exit.segment == inSegment) {
                SurfacePoint q;
                distanceTo(head, dip, &q);
                pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(inSegment + 1), q.position + q.normal.normalized() * (sign * keepOut));
                i = inSegment + 1;
            } else {
                for (std::size_t k = inSegment + 1; k <= last; ++k) {
                    SurfacePoint q;
                    distanceTo(head, pts[k], &q);
                    if (q.root.face >= 0) pts[k] = q.position + q.normal.normalized() * (sign * keepOut);
                }
                i = last;
            }
            out.pushed = true;
            if (i + 1 >= pts.size()) break;
            from = pts[i];
            continue;
        }
        // Cut where it went in (a little outside).
        const Vec3f back = pts[inSegment] - inAt;
        const float bl = back.length();
        Vec3f end = inAt;
        if (bl > 1e-9f) end = inAt + back * (std::min(keepOut, 0.5f * bl) / bl);
        pts.resize(inSegment + 1);
        if ((end - pts.back()).length() > eps) pts.push_back(end);
        out.cut = true;
        break;
    }
    if (pts.size() < 2 || (out.cut && arcLength(pts) < s.minLength)) {
        out.droppedShort = true;
        out.root = SurfacePoint{};
        return out;
    }
    out.points = std::move(pts);
    return out;
}

}  // namespace

GuideRootingCounts guideRootingScale(const ScalpMesh& scalp, const GuideRootingOptions& o) {
    GuideRootingCounts s;
    float r = o.headRadius;
    if (!(r > 0.0f) && scalp.triangleCount() > 0) r = headFrameOf(scalp, Vec3f(0, 1, 0), Vec3f(0, 0, 1)).radius;
    if (!(r > 0.0f)) r = 1.0f;
    s.headRadius = r;
    s.snapGap = o.snapShare * r;
    s.maxGap = std::max(s.snapGap, o.maxGapShare * r);
    s.pushDepth = o.pushShare * r;
    s.minLength = o.minLengthShare * r;
    return s;
}

RootedGuide rootGuide(const std::vector<Vec3f>& points, const ScalpMesh& scalp, const ScalpMesh* head, const GuideRootingOptions& options,
                      const GuideRootingCounts& scale) {
    const ScalpMesh& mesh = head ? *head : scalp;
    return rootOne(points, scalp, mesh, outwardSign(mesh), options, scale);
}

GuideRootingCounts rootGuides(std::vector<std::vector<Vec3f>>& guides, const ScalpMesh& scalp, const ScalpMesh* head,
                              const GuideRootingOptions& options, std::vector<std::size_t>* kept, std::vector<SurfacePoint>* roots) {
    GuideRootingCounts c = guideRootingScale(scalp, options);
    c.guides = static_cast<int>(guides.size());
    const ScalpMesh& mesh = head ? *head : scalp;
    const float sign = outwardSign(mesh);
    std::vector<RootedGuide> done(guides.size());
    parallelFor(0, guides.size(), [&](std::size_t i) { done[i] = rootOne(guides[i], scalp, mesh, sign, options, c); }, 4);
    std::vector<std::vector<Vec3f>> out;
    if (kept) kept->clear();
    if (roots) roots->clear();
    for (std::size_t i = 0; i < done.size(); ++i) {
        const RootedGuide& g = done[i];
        c.flipped += g.flipped;
        c.snapped += g.snapped;
        c.connected += g.connected;
        c.pushed += g.pushed;
        c.cut += g.cut;
        c.droppedGap += g.droppedGap;
        c.droppedShort += g.droppedShort;
        if (g.points.empty()) continue;
        out.push_back(g.points);
        if (kept) kept->push_back(i);
        if (roots) roots->push_back(g.root);
    }
    c.dropped = c.droppedGap + c.droppedShort;
    guides.swap(out);
    return c;
}

bool guideStartsAtTip(const std::vector<Vec3f>& points, const ScalpMesh& scalp, const GuideRootingCounts& s, const GuideRootingOptions& o) {
    if (points.size() < 2) return false;
    return startsAtTip(points, distanceTo(scalp, points.front()), distanceTo(scalp, points.back()), o, s);
}

}  // namespace groomlab
