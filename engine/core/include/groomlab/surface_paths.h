// SPDX-License-Identifier: Apache-2.0
#pragma once

// Paths along a scalp's skin, from points on it: how far each vertex lies along the surface, and
// how a direction at the point arrives there carried along it. Interpolate weighs its guides by
// these distances (followSurface) and carries their shapes along these paths (its frame
// transfer).
//
// A front from a point visits the rest pose's vertices nearest first (Dijkstra), out to a limit.
// Its distances are straight segments between the places where the path turns: a vertex takes
// the straight line from the anchor of the vertex it is reached from (the source, or a vertex
// where the path bent) while that line stays on the skin, else the path bends at that vertex.
// A line stays on the skin when it leans out of the tangent planes at both its ends by less than
// 30 degrees (one through a fold, a thin flap or across a shell leaves the surface steeply) and
// the edges the front took to get there are not much longer (it does not jump a gap or a notch
// of the mesh: an ear cut out of the scalp). So wherever the straight line hugs the skin, a
// vertex's distance is the straight line from the source, and where the skin folds away, the
// path goes round. On a convex patch the line is a chord: at the lean allowed, within 1.5% of the
// distance along the curved skin.
//
// A direction is carried from anchor to anchor by the shortest turn between their normals (along
// a straight piece on the skin, the turn parallel transport along it makes), so an end reached
// without a bend gets exactly the turn from the source's normal to its own, and one reached round
// a fold gets the turns through the bends: its twist about its normal beyond that plain turn.
//
// The mesh is welded first (vertices at one place are one: a scan split along its UV seams is
// one surface), so a seam is no edge to a path. Everything is on the rest pose, so a deforming
// scalp's paths are the same on every frame.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

struct SurfaceFront;

// A scalp's rest pose as a graph: its vertices welded, their neighbours, their normals, the
// triangles round each, and the vertices in a grid by where they are (which lie near a point).
class SurfaceGraph {
public:
    explicit SurfaceGraph(const ScalpMesh& scalp);

    std::size_t vertexCount() const { return positions_.size(); }
    std::size_t triangleCount() const { return corners_.size() / 3; }
    const Vec3f& position(std::uint32_t v) const { return positions_[v]; }
    const Vec3f& normal(std::uint32_t v) const { return normals_[v]; }  // over every face around it
    // Triangle t's welded corner k.
    std::uint32_t corner(std::size_t t, int k) const { return corners_[3 * t + static_cast<std::size_t>(k)]; }
    // v's neighbours (welded), *count of them.
    const std::uint32_t* neighbors(std::uint32_t v, std::size_t* count) const {
        *count = adjacencyBegin_[v + 1] - adjacencyBegin_[v];
        return adjacency_.data() + adjacencyBegin_[v];
    }
    // The lengths of v's edges to its neighbours, in their order.
    const float* edgeLengths(std::uint32_t v) const { return edgeLengths_.data() + adjacencyBegin_[v]; }
    float longestEdge(std::uint32_t v) const { return longest_[v]; }
    float longestEdge() const { return maxEdge_; }
    float meanEdge() const { return meanEdge_; }  // (rounded to an eighth of an octave)
    // The triangles with corner v, *count of them.
    const std::uint32_t* trianglesAround(std::uint32_t v, std::size_t* count) const {
        *count = trianglesBegin_[v + 1] - trianglesBegin_[v];
        return trianglesAround_.data() + trianglesBegin_[v];
    }

    bool contains(const StrandRoot& root) const {
        return root.face >= 0 && static_cast<std::size_t>(root.face) < triangleCount();
    }
    // A root's position and normal on the rest pose: ScalpMesh::position and normal on
    // scalp.restPose(), to the bit.
    Vec3f rootPosition(const StrandRoot& root) const;
    Vec3f rootNormal(const StrandRoot& root) const;

    // Calls f(v) for every vertex v within `distance` of p (each once, in no particular order; a
    // few farther ones too).
    template <class F>
    void forEachVertexNear(const Vec3f& p, float distance, const F& f) const;

    // The fronts made on this graph, by root and limit (SurfaceFronts looks there before making
    // one: another binding of the same guides, the stroke's and the view's, a density changed).
    // Kept up to a number of them, then let go of all at once.
    std::shared_ptr<const SurfaceFront> keptFront(const StrandRoot& root, float limit) const;
    void keepFront(const std::shared_ptr<const SurfaceFront>& front) const;

private:
    std::vector<ScalpMesh::Triangle> triangles_;
    std::vector<Vec3f> restPoints_;   // per point (not welded)
    std::vector<Vec3f> restNormals_;  // per point, as the scalp's own (not welded)
    std::vector<Vec3f> positions_, normals_;  // welded
    std::vector<std::uint32_t> corners_;
    std::vector<std::uint32_t> adjacencyBegin_, adjacency_;
    std::vector<float> edgeLengths_, longest_;
    float meanEdge_ = 0.0f, maxEdge_ = 0.0f;
    std::vector<std::uint32_t> trianglesBegin_, trianglesAround_;
    // The grid: cells of `cell_` from `origin_`, `dims_` of them; cell c's vertices at
    // [cellBegin_[c], cellBegin_[c + 1]).
    Vec3f origin_;
    float cell_ = 1.0f;
    std::int64_t dims_[3] = {0, 0, 0};
    std::vector<std::uint32_t> cellBegin_, cellVertices_;
    struct FrontKey {
        std::uint64_t root, limit;
        bool operator==(const FrontKey& o) const { return root == o.root && limit == o.limit; }
    };
    struct FrontKeyHash {
        std::size_t operator()(const FrontKey& k) const { return static_cast<std::size_t>(k.root * 0x9E3779B97F4A7C15ull ^ (k.limit + (k.root >> 7))); }
    };
    static FrontKey frontKey(const StrandRoot& root, float limit);
    mutable std::mutex keptMutex_;
    mutable std::unordered_map<FrontKey, std::shared_ptr<const SurfaceFront>, FrontKeyHash> kept_;
};

template <class F>
void SurfaceGraph::forEachVertexNear(const Vec3f& p, float distance, const F& f) const {
    if (cellBegin_.empty()) return;
    const float r = distance;
    std::int64_t lo[3], hi[3];
    const float c[3] = {p.x - origin_.x, p.y - origin_.y, p.z - origin_.z};
    for (int k = 0; k < 3; ++k) {
        const float a = (c[k] - r) / cell_, b = (c[k] + r) / cell_;
        if (!(b >= 0.0f) || !(a < static_cast<float>(dims_[k]))) return;  // (NaN: nothing)
        lo[k] = a > 0.0f ? static_cast<std::int64_t>(a) : 0;
        hi[k] = std::min<std::int64_t>(dims_[k] - 1, b < 9e18f ? static_cast<std::int64_t>(b) : dims_[k] - 1);
    }
    for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
        for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
                const std::size_t cell = static_cast<std::size_t>((z * dims_[1] + y) * dims_[0] + x);
                for (std::uint32_t i = cellBegin_[cell]; i < cellBegin_[cell + 1]; ++i) f(cellVertices_[i]);
            }
        }
    }
}

// The graph of `scalp`'s rest pose, shared by everything asking for the same one (keyed by its
// restSurfaceHash: one per groom, the same on every frame of a deforming scalp).
std::shared_ptr<const SurfaceGraph> sharedSurfaceGraph(const ScalpMesh& scalp);

// The front from one point: every vertex it reached with its distance along the skin, the length
// of the edges it took and its anchor; and the triangles within its limit in a straight line that
// it does not reach straight at all three corners (round a fold, across a gap, or not at all).
struct SurfaceFront {
    // Where the path last turned: [0] is the source. `tangent`: the source's tangent (any
    // perpendicular to its normal) carried there.
    struct Anchor {
        Vec3f position, normal, tangent;
        float distance = 0.0f, path = 0.0f;
    };
    StrandRoot root;
    Vec3f position, normal;
    float limit = 0.0f;
    std::vector<Anchor> anchors;
    // A vertex reached: its distance along the skin, the length of the edges walked to it and the
    // anchor its straight line comes from.
    struct Reached {
        std::uint32_t vertex;
        float distance, path;
        std::uint32_t anchor;
    };
    std::vector<Reached> reached;     // in the order reached
    std::vector<std::uint32_t> bent;  // triangles (ascending)
    const Reached* find(std::uint32_t v) const;  // null: not reached
    // reached by vertex: an open-addressed table of indices + 1 (0: empty), a power of two long.
    std::vector<std::uint32_t> table;
};

// The front from `root` (on the rest pose), out to `limit` along the skin: every vertex whose
// distance is within the limit plus its longest edge is expanded (so every corner of a triangle
// some point of which lies within the limit is reached).
std::shared_ptr<const SurfaceFront> surfaceFront(const SurfaceGraph& graph, const StrandRoot& root, float limit);
// Fronts made so far in the process (tests: what a change made again).
std::size_t surfaceFrontCount();

// Where a path from a source ends at a point.
struct SurfacePath {
    // Unknown: no front (the source is not on the scalp, nor the point). Beyond: the front never
    // reached the point (farther along the skin than the source's limit, or on another piece).
    // Straight: reached by the straight line (its distance is the straight line, its frame the
    // plain turn: the caller's own). Bent: round a fold, `distance` along the skin and `twist`.
    enum class Kind { Unknown, Beyond, Straight, Bent };
    Kind kind = Kind::Unknown;
    float distance = 0.0f;
    // Radians about the point's normal: the source's directions arrive turned by Rotation(source
    // normal, point normal), then by this about the point's normal (both rest normals).
    float twist = 0.0f;
};

// The end of paths: a root and its rest position (its rest normal worked out the first time a
// path may bend there).
struct SurfacePathEnd {
    SurfacePathEnd(const StrandRoot& r, const Vec3f& p) : root(r), position(p) {}
    StrandRoot root;
    Vec3f position;
    mutable Vec3f normal;
    mutable bool hasNormal = false;
};

// Fronts from many sources, and per triangle how many of them do not reach it straight where
// they might (none: every source within its limit in a straight line comes straight, so a path
// to a point there is the straight line without looking it up: almost the whole of a smooth scalp).
class SurfaceFronts {
public:
    // A front from each source with roots[i] on the scalp (else none) to limits[i]; those of
    // `previous` (on the same graph) whose root and limit are the same to the bit are shared,
    // not made again (and the counts follow the few made). Made side by side.
    SurfaceFronts(std::shared_ptr<const SurfaceGraph> graph, const std::vector<StrandRoot>& roots, const std::vector<float>& limits,
                  const SurfaceFronts* previous);

    const SurfaceGraph& graph() const { return *graph_; }
    const std::shared_ptr<const SurfaceGraph>& graphPointer() const { return graph_; }
    const SurfaceFront* front(std::size_t source) const { return fronts_[source].get(); }
    std::size_t made() const { return made_; }  // fronts made (not shared from `previous`)

    // Whether every front reaches `to`'s triangle straight wherever it is within the front's limit
    // in a straight line; then a point there within straightWithin2(source) of a source (squared,
    // as the caller measures it) is reached by the straight line: path() says Straight (or, from a
    // source with no front, Unknown: the straight line too) without looking.
    bool straightAt(const StrandRoot& to) const { return graph_->contains(to) && (*bentCounts_)[static_cast<std::size_t>(to.face)] == 0; }
    float straightWithin2(std::size_t source) const { return within2_[source]; }

    // The path from `source` to `end`; `straight2`: the squared straight line between them as the
    // caller measured it (from `end` to the source's root).
    SurfacePath path(std::size_t source, const SurfacePathEnd& end, float straight2) const;

private:
    std::shared_ptr<const SurfaceGraph> graph_;
    std::vector<std::shared_ptr<const SurfaceFront>> fronts_;
    std::shared_ptr<const std::vector<std::uint32_t>> bentCounts_;  // per triangle
    std::vector<float> within2_;  // per source: its limit squared, less a rounding allowance (no front: infinite)
    std::size_t made_ = 0;
};

}  // namespace groomlab
