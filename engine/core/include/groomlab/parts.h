// SPDX-License-Identifier: Apache-2.0
#pragma once

// Parting (like the parts in an animal's fur): lines on the scalp that hair
// does not mix across. Interpolate and Clump skip guides and clump centers
// on the other side of a part, and the Part operator opens the gap.
//
// Parts are stored bound to the scalp (face + barycentric coordinates per
// point, like guide roots), so they follow a deforming scalp, travel in
// groom files and transfer with the groom to a new scalp. The scalp they are
// bound to is the cage (the scalp before the Scalp node subdivides it), so the
// subdivision level can change under them (PartSet::onRefined).
//
// Sides are measured on the rest pose (stable while the scalp deforms): a
// point's side is the sign of its offset across the part at the nearest
// point on the line. Points whose nearest point is a line's end have no
// side, so hair flows around the end of a part.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/transfer.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class SubdivisionPlan;  // subdivision.h

struct Part {
    std::string name;
    std::vector<StrandRoot> points;  // along the line, on the scalp
};

class PartSet {
public:
    std::vector<Part> parts;

    // A part through points near the scalp (e.g. a drawn curve), bound to their closest scalp points.
    static Part fromPoints(const std::string& name, const std::vector<Vec3f>& points, const ScalpMesh& scalp);

    bool empty() const { return parts.empty(); }

    // Adds a part through `points` (a stroke drawn on or near the scalp):
    // resampled evenly to at most `samples` points, bound to the scalp, and
    // named `base`, or base1, base2... when taken. Returns the name. Throws
    // std::invalid_argument when the stroke is too short.
    std::string add(const std::string& base, const std::vector<Vec3f>& points, const ScalpMesh& scalp,
                    std::size_t samples = 64);
    // The part named `name` taken out; false when there is none (every host's Remove Part).
    bool remove(const std::string& name);
    // The same parts bound to another scalp: each point to the closest point of `to` (GH-8).
    PartSet transferred(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit = {}) const;
    // The same by any mapping (transfer.h): each point where the mapping takes it.
    PartSet transferred(const ScalpMapping& mapping) const;
    // Parts are stored bound to the cage: the scalp as the host gives it, before any subdivision
    // (subdivision.h), whatever the Scalp node's level. On a subdivided scalp they are resolved on
    // the refined mesh: onRefined carries every point there (SubdivisionPlan::refinedRoot),
    // onCage back (cageRoot: parts drawn on the refined scalp, to store). Points off the mesh
    // stay off it.
    PartSet onRefined(const SubdivisionPlan& plan) const;
    PartSet onCage(const SubdivisionPlan& plan) const;
    const Part* find(const std::string& name) const;
    std::uint64_t contentHash() const;
    std::string toJson() const;                       // {"version": 1, "parts": [{"name", "points": [[f, u, v], ...]}]}
    static PartSet fromJson(const std::string& text);  // empty text: no parts; throws std::runtime_error
};

// Parts resolved on a scalp, for side tests and drawing.
class PartLines {
public:
    PartLines(const PartSet& set, const ScalpMesh& scalp);

    struct Side {
        int side = 0;         // -1, +1, or 0 when not beside a part (or past its ends)
        int part = -1;        // which part
        std::size_t segment = 0;
        float distance = 0.0f;  // to the line, rest pose
    };
    // Side of a rest-pose point (e.g. scalp.restPositionOr(root, ...)). The nearest segment is
    // found through a box tree per part; between equally near segments the first part's, then
    // its first segment, wins (as a scan of every segment in order would pick).
    Side sideOf(const Vec3f& restPoint, const std::string& only = {}) const;
    static bool separated(const Side& a, const Side& b) {
        return a.part >= 0 && a.part == b.part && a.side != 0 && b.side != 0 && a.side != b.side;
    }
    // The direction away from the part on a point's side, tangent to the
    // scalp, in the current pose.
    Vec3f awayFrom(const Side& side, const Vec3f& currentNormal) const;

    bool empty() const { return rest_.empty(); }
    const std::vector<std::vector<Vec3f>>& lines() const { return current_; }  // current pose, for drawing
    const std::vector<std::string>& names() const { return names_; }
    std::uint64_t contentHash() const { return hash_; }
    // What sideOf answers depend on: the parts on the rest pose (PartSet::contentHash and
    // ScalpMesh::restSurfaceHash). The same on every frame of a deforming scalp with a rest pose,
    // so work keyed on sides (Interpolate's binding) carries over.
    std::uint64_t sideHash() const { return sideHash_; }

private:
    // A box over the rest segments [first, last) of a part's line; leaves hold a few segments,
    // inner nodes two children (indices into the part's tree, root first).
    struct SegmentNode {
        Vec3f lo, hi;
        std::uint32_t first = 0, last = 0;
        std::uint32_t left = 0, right = 0;  // 0: a leaf (the root is never a child)
    };
    std::uint32_t buildTree(std::vector<SegmentNode>& tree, const std::vector<Vec3f>& line, std::uint32_t first,
                            std::uint32_t last);

    std::vector<std::string> names_;
    std::vector<std::vector<Vec3f>> rest_, restNormals_, current_;
    std::vector<std::vector<SegmentNode>> trees_;  // per part
    float extent_ = 0.0f;  // the largest coordinate of the rest lines (rounding slack scales with it)
    std::uint64_t hash_ = 0;
    std::uint64_t sideHash_ = 0;
};

using PartsPtr = std::shared_ptr<const PartLines>;

// ---- Auto parts, found where the hair splits (the Part node's partsFrom auto) ----------------------------
//
// The lines where `strands` part on `scalp`: where the hair on either side of a line flows away
// from it, as a parting does. Named auto1, auto2... in the order found, bound to `scalp` as drawn
// parts are, so PartLines and the Part node open them exactly as painted ones.
//
// The flow is the strands' own: each strand's first segment laid on the scalp's tangent plane at
// its root (a strand standing straight up counts for little), so it is the hair as the node takes
// it, however it was made (guides interpolated, a Flow node along painted arrows, brushes).
//  1. Each strand's direction is spread to its root triangle's corners (barycentric weights), and
//     the vertices' sums are gathered into small cells of the scalp (an eighth of the radius R).
//  2. The field at every vertex is the cells' sums within R, weighted (1 - d^2/R^2)^2 and by how
//     much their normals face the vertex's, laid on its tangent plane: smooth, so a small edit of
//     the hair moves it a little. R is 5% of the scalp's size (the square root of its area), at
//     least 3 root spacings of the strands (few strands: guides taken directly).
//  3. An edge (i, j) of the scalp parts where the field along it turns from pointing back to i to
//     pointing on to j (d_i . (p_i - p_j) > 0 and d_j . (p_j - p_i) > 0: the discrete divergence
//     ridge, one edge wide however smooth the field; the part crosses the edge where the field's
//     component along it is zero, linearly between its ends) and the split there is sharp: the
//     hair within R on either side of that crossing (cells beyond the plane through it, across the
//     edge, leaving out a band of R/4 each side of it, too near to tell) flows away from it on
//     both sides, coherently (each side's mean direction at least 0.2 long), with at least
//     `splitAngleDegrees` between the two sides' mean directions, and some hair lies within R/2
//     of it (a part stops near the hair's edge).
//  4. Split edges are joined where their crossings are within R/2 (or 1.5 edge lengths, on a
//     coarse scalp); in each joined set the longest path (two sweeps of shortest paths) is a
//     part, the split edges joined to it dropped, until what is left is shorter than `minLength`
//     (a share of the scalp's size). A whorl's center splits over a few edges only.
//  5. Each part is resampled every R/2 and smoothed (four passes of 1/4, 1/2, 1/4, the ends kept),
//     then bound to the scalp's closest points.
// The radius is the whole scalp's: a small patch of hair on a big scalp (a head's on a body) is
// smoothed at the big scalp's scale. Parts closer together than about R merge.
// Pure and deterministic: the same strands and scalp give the same parts, at any thread count,
// and k times as big a groom gives the same parts k times as big. Empty without strands rooted on
// the scalp or without a split.
PartSet findAutoParts(const StrandSet& strands, const ScalpMesh& scalp, float splitAngleDegrees, float minLength);

}  // namespace groomlab
