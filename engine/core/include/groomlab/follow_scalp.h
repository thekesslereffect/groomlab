// SPDX-License-Identifier: Apache-2.0
#pragma once

// Following the scalp (GroomSession::setFollowScalp, a groom's followScalp): the groom is
// evaluated on the scalp's rest pose and each strand is then carried to the scalp as it is now
// by the rigid motion of its root's frame.
//
// A root's frame on a scalp: its origin is the root's position, its axes one edge of the root's
// face (the longest on the rest pose, so a sliver's short edge never steers it) made
// perpendicular to the normal (x), the smooth normal at the root's barycentrics (y) and their
// cross product (z). A point p carried from scalp `from` to scalp `to` (the same topology, other
// points; the same edge on both) is
//     to.origin + to.axes * (from.axes^T * (p - from.origin)),
// a rotation and a translation: no scale, so a strand keeps its rest shape and length however
// the skin under it stretches (only its root's place and turn follow). A root whose frame is
// the same on both scalps (that part did not move) keeps its points to the bit.

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class GuideHierarchy;

struct RootFrame {
    Vec3f origin, x, y, z;
    bool valid = false;  // false: the root is not on the mesh, or its face is degenerate
    bool operator==(const RootFrame& o) const {
        return valid == o.valid && origin == o.origin && x == o.x && y == o.y && z == o.z;
    }
};

// The frame of `root` on `scalp`, its x along edge `edge` of the root's face (from corner
// `edge` to the next); -1: the face's longest edge on this scalp.
RootFrame rootFrameOn(const ScalpMesh& scalp, const StrandRoot& root, int edge = -1);
// The index of the longest edge of the root's face (0 when it is not on the scalp).
int longestEdge(const ScalpMesh& scalp, const StrandRoot& root);

// The rigid motion of a root from one scalp to another.
struct RootMotion {
    RootFrame from, to;
    // No motion: the same frame on both (or a root on neither): points stay as they are.
    bool still() const { return !from.valid || !to.valid || from == to; }
    Vec3f point(const Vec3f& p) const;      // a point carried
    Vec3f direction(const Vec3f& v) const;  // a direction turned
    RootMotion inverse() const { return {to, from}; }
};
RootMotion rootMotion(const ScalpMesh& from, const ScalpMesh& to, const StrandRoot& root);

// Strands evaluated on `from` carried to `to`: each strand's points by its root's motion (a
// strand without a root on the scalp stays), Instance nodes' fixed directions turned with their
// strand's root, the Cards nodes' cards (each by its first strand's root; its strands, for
// baking, carried too) and Propagate nodes' rachis rings (each shaft by the scalp point nearest
// its base on `from`). `strands` itself when nothing moves. `boxes`: strandBoxes(*strands, from),
// when the caller keeps them (strands carried frame after frame): each root's frame on `from` is
// read from them, not found again; the same result.
struct StrandBoxes;
StrandsPtr carryStrands(const StrandsPtr& strands, const ScalpMesh& from, const ScalpMesh& to, const StrandBoxes* boxes = nullptr);

// Whether carryStrands moves nothing of `strands` but each strand's points (no card layers or
// rachis rings ride on them), or instances ride on them too, each placed by a matrix on its strand
// alone (no scale or instance bent onto the skin or along its strand: InstanceSet::hasMeshes): placing them on
// their strands carried (carryStrands of those strands, then InstanceSet::resolve) gives what
// placing them on all the strands carried does.
bool carriesPointsAndInstances(const StrandSet& strands);

// Each strand's box at rest (its finite points' center and half extents) and its root's frame on
// the rest pose rom (rootMotion's: its face's longest edge there), made once for strands carried
// frame after frame (carriedBounds).
struct StrandBoxes {
    std::vector<Vec3f> centers, halves;  // a strand without a finite point: a half extent below 0
    std::vector<RootFrame> frames;       // on `from`
    std::vector<std::int8_t> edges;      // the edge each frame's x runs along
    bool finite = true;                  // every point finite (StrandSet::Extent::finite)
    float longestStrand = 0.0f;
};
std::shared_ptr<const StrandBoxes> strandBoxes(const StrandSet& strands, const ScalpMesh& from);
// A box holding every point of carryStrands(strands, from, to) (the strands and rest pose `boxes` were made from):
// each strand's box at rest turned and moved by its root's motion, the boxes' union (a little more
// than the carried points' extent, never less; a pass over the strands, not their points).
StrandSet::Extent carriedBounds(const StrandSet& strands, const StrandBoxes& boxes, const ScalpMesh& to);

// Strands evaluated at rest, handed to a host that carries them to the scalp now itself
// (ViewRequest::hostCarries: Maya's viewport, on the GPU) in place of carryStrands' result, which
// carried() makes on demand for whoever needs the strands where they are (once, thread-safe).
class FollowCarry {
public:
    FollowCarry(StrandsPtr rest, MeshPtr from, MeshPtr to);
    const StrandsPtr& rest() const { return rest_; }
    const MeshPtr& from() const { return from_; }  // the scalp at rest
    const MeshPtr& to() const { return to_; }      // the scalp now
    StrandsPtr carried() const;

private:
    StrandsPtr rest_;
    MeshPtr from_, to_;
    mutable std::mutex mutex_;
    mutable StrandsPtr carried_;
};
using FollowCarryPtr = std::shared_ptr<const FollowCarry>;

// What a GPU carries a strand by (its root on `from`; 32 bytes, the kernel's layout,
// groomlab/gpu/line_frames.h): the corners of its root's face, the edge its frame's x runs along
// (rootMotion's: the face's longest on `from`; kFollowUnrooted: a root not on the scalp, whose
// strand stays) and its barycentrics.
struct FollowRoot {
    std::uint32_t corners[3] = {0, 0, 0};
    std::uint32_t edge = 0;
    float u = 0.0f, v = 0.0f;
    std::uint32_t pad[2] = {0, 0};
};
constexpr std::uint32_t kFollowUnrooted = 3;
// `strands`' roots on `from` into `out` (strandCount of them); returns a hash of what it wrote
// (never 0).
std::uint64_t writeFollowRoots(const StrandSet& strands, const ScalpMesh& from, FollowRoot* out);

// Flat guides given on `to` (rooted on it) taken to `from` by the inverse motion of their
// roots: where they are on `to` is where the strands they shape are carried back to. `guides`
// itself when nothing moves.
GuidesPtr carryGuidesBack(const GuidesPtr& guides, const ScalpMesh& from, const ScalpMesh& to);

// Guide poses (hierarchy guides by ID, in world space on `to`: simulated, cached) taken to
// `from` by the inverse motion of their guide's root in `rooted`. Poses of guides `rooted`
// does not have stay as they are.
GuidesPtr carryPosesBack(const GuidesPtr& poses, const GuideHierarchy& rooted, const ScalpMesh& from, const ScalpMesh& to);

// A hierarchy on `to` (a brush stroke's working copy) taken to `from`: every guide's points and
// frame reference by the inverse motion of its root.
std::shared_ptr<GuideHierarchy> carryHierarchyBack(const GuideHierarchy& hierarchy, const ScalpMesh& from, const ScalpMesh& to);

// The scalp's earlier frames (world space, `now` being the scalp at the evaluated time) as the
// rest pose `rest` would have moved: each vertex's displacement from now turned into rest space
// by the vertex's own frame (its normal and its first face's edge from it), so Jiggle in rest
// space lags as the head moved, relative to it. Exact for a rigid motion of the whole head.
ScalpHistoryPtr historyAtRest(const ScalpHistory& history, const ScalpMesh& rest, const ScalpMesh& now);

}  // namespace groomlab
