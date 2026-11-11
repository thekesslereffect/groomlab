// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/strand_set.h"

namespace groomlab {

// A point on the scalp: triangle + barycentric (u, v) and the evaluated
// position and smooth normal there. Barycentrics follow StrandRoot:
// position = a * (1 - u - v) + b * u + c * v.
struct SurfacePoint {
    StrandRoot root;
    Vec3f position;
    Vec3f normal;
};

// Closest point on triangle abc to p, as barycentric (u, v) with
// point = a + u * (b - a) + v * (c - a). Ericson, Real-Time Collision Detection 5.1.5. (Inline:
// the closest-point walks weigh it for every triangle they meet.) A degenerate triangle (two
// corners at one point, or all three on a line) gives a point on it, never NaN: its divisions
// by zero are guarded, as scalp_paint.cpp's distanceSquaredToTriangle guards them.
inline void closestBarycentric(const Vec3f& p, const Vec3f& a, const Vec3f& b, const Vec3f& c, float& u, float& v) {
    const Vec3f ab = b - a, ac = c - a, ap = p - a;
    const float d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { u = 0; v = 0; return; }

    const Vec3f bp = p - b;
    const float d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0f && d4 <= d3) { u = 1; v = 0; return; }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { u = d1 - d3 != 0.0f ? d1 / (d1 - d3) : 0.0f; v = 0; return; }

    const Vec3f cp = p - c;
    const float d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0f && d5 <= d6) { u = 0; v = 1; return; }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { u = 0; v = d2 - d6 != 0.0f ? d2 / (d2 - d6) : 0.0f; return; }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float den = (d4 - d3) + (d5 - d6);
        const float w = den != 0.0f ? (d4 - d3) / den : 0.0f;
        u = 1.0f - w;
        v = w;
        return;
    }

    const float sum = va + vb + vc;
    if (!(std::abs(sum) > 0.0f)) {  // all three on a line: the nearest corner
        const float da = ap.dot(ap), db = bp.dot(bp), dc = cp.dot(cp);
        u = db < da && db <= dc ? 1.0f : 0.0f;
        v = dc < da && dc < db ? 1.0f : 0.0f;
        return;
    }
    const float denom = 1.0f / sum;
    u = vb * denom;
    v = vc * denom;
}

// One of a groom's scalps in the mesh that holds them all (several scalps on one groom:
// combineScalps, scalp_pieces.h): its name ("scalp" for the groom's own, the host's name for each
// other one), its triangles and points there (contiguous: each scalp's after the one before) and
// the render subdivision of its own mesh (what a Scalp node set to match render reading it takes).
// `subdivision`: the Catmull-Clark levels this scalp is refined by when the whole mesh is
// (subdivision.h), each scalp at its own (GroomSession::scalpSubdivisions); -1 (the default): the
// levels the whole mesh is refined by.
struct ScalpPiece {
    std::string name;
    std::size_t firstTriangle = 0, triangleCount = 0;
    std::size_t firstPoint = 0, pointCount = 0;
    int renderSubdivision = 0;
    std::string renderSubdivisionFrom;
    int subdivision = -1;
};

// Triangulated scalp surface that strands and guides are rooted on (GH-2).
class ScalpMesh {
public:
    using Triangle = std::array<std::int32_t, 3>;

    ScalpMesh() = default;
    ScalpMesh(std::vector<Vec3f> points, std::vector<Triangle> triangles);

    std::size_t pointCount() const { return points_.size(); }
    std::size_t triangleCount() const { return triangles_.size(); }
    const std::vector<Vec3f>& points() const { return points_; }
    const std::vector<Triangle>& triangles() const { return triangles_; }

    float totalArea() const { return totalArea_; }

    // Whether a root is on this mesh (a valid triangle).
    bool contains(const StrandRoot& root) const {
        return root.face >= 0 && static_cast<std::size_t>(root.face) < triangles_.size();
    }

    Vec3f position(const StrandRoot& root) const {
        assert(root.face >= 0 && static_cast<std::size_t>(root.face) < triangles_.size());
        const Triangle& tri = triangles_[static_cast<std::size_t>(root.face)];
        const float w = 1.0f - root.u - root.v;
        return points_[tri[0]] * w + points_[tri[1]] * root.u + points_[tri[2]] * root.v;
    }
    Vec3f normal(const StrandRoot& root) const;  // smooth (area-weighted vertex normals)

    // Closest point on the surface, through an AABB tree built on the first
    // query (thread-safe). Returns face -1 on an empty mesh, or when nothing
    // is within maxDistance (a bound prunes the search, so far points are cheap).
    // A mesh made by withPoints searches its source's tree refitted to its points, and answers
    // as a tree built on its own points does (when the order the tree meets triangles in could
    // matter, ties within rounding, it builds its own tree for that query).
    SurfacePoint closestPoint(const Vec3f& p, float maxDistance = std::numeric_limits<float>::infinity()) const;
    // The same, also giving in `*lowerBound` a distance nothing on the mesh is closer than (a hit's
    // own distance; on a miss, at least maxDistance). Distance to the mesh changes no faster than
    // the point moves, so a caller moving a point by less than *lowerBound - maxDistance knows the
    // next query misses too (Collide walking a strand root to tip).
    SurfacePoint closestPointBounded(const Vec3f& p, float maxDistance, float* lowerBound) const;

    // Same result by testing every triangle. Reference for tests and benchmarks.
    SurfacePoint closestPointBruteForce(const Vec3f& p) const;
    // The closest point of triangle `face` to p (its root and position, no normal): what the
    // queries above weigh each triangle by (its squared distance, (position - p).dot(position - p)).
    SurfacePoint closestPointOnFace(const Vec3f& p, std::size_t face) const {
        const Triangle& tri = triangles_[face];
        float u = 0, v = 0;
        closestBarycentric(p, points_[tri[0]], points_[tri[1]], points_[tri[2]], u, v);
        SurfacePoint sp;
        sp.root = StrandRoot{static_cast<std::int32_t>(face), u, v};
        sp.position = position(sp.root);
        return sp;
    }
    // The vertex normals normal() blends (area-weighted, each normalized).
    const std::vector<Vec3f>& vertexNormals() const { return vertexNormals_; }

    // Nearest hit of the ray origin + t * direction (t >= 0) with either side
    // of the surface. Returns false on a miss; otherwise fills *hit and *t.
    bool intersectRay(const Vec3f& origin, const Vec3f& direction, SurfacePoint* hit, float* t = nullptr) const;

    // The mesh as words for a host searching it elsewhere as intersectRay and closestPoint do (the
    // GPU preview's MeshCut): the tree built on its own points (the answers a freshly built mesh
    // gives), its points, vertex normals and triangles. The header (kRayTree*): the nodes' count, then
    // where (in words) the nodes (2 words each: first, count; a leaf's triangles from `first` in the
    // order, an inner node's children at first and first + 1), their boxes (6 floats each: lo, hi),
    // the triangles' order, the points (3 floats each), the vertex normals (3 floats each) and the
    // triangles (3 words each) start. Null without triangles.
    enum : std::uint32_t {
        kRayTreeNodeCount = 0, kRayTreeNodes, kRayTreeBoxes, kRayTreeOrder, kRayTreePoints, kRayTreeNormals, kRayTreeTriangles,
        kRayTreeHeader
    };
    std::shared_ptr<const std::vector<std::uint32_t>> rayTree() const;

    // Everything: points, triangles, UVs, color sets and rest pose.
    std::uint64_t contentHash() const { return hash_; }
    // The surface alone: points, triangles, UVs and rest pose, not the color sets. The "scalp"
    // external's key (setGroomExternals), so painting a vertex map (a color set) re-evaluates only
    // the nodes that read that set, which salt their keys with colorSetHash.
    std::uint64_t surfaceHash() const { return surfaceHash_; }
    // The rest pose's surface: triangles, UVs and rest points (surfaceHash() without a rest
    // pose); not the current points of a mesh that has one, nor the color sets. The same on every
    // frame of a deforming scalp with a rest pose (what is worked out on the rest pose keys on it).
    std::uint64_t restSurfaceHash() const;
    // One color set's name and values; for a set the mesh lacks, a value of its own (so whether
    // the set exists, which decides how a map by that name is read, keys the cache too).
    std::uint64_t colorSetHash(const std::string& name) const;
    // Hash of the triangle list alone: the same for every pose of one mesh,
    // different once it is re-meshed (guides bound to faces then re-root).
    std::uint64_t topologyHash() const { return topologyHash_; }
    // The fingerprint a groom file keeps of the scalp it was made on (scalp.topology): the point
    // count and the triangles' vertex indices, what strands and guides are rooted by. The same for
    // every pose, UV layout, polygon grouping and color set of one mesh; another vertex order,
    // triangulation or point count differs (GroomSession says so when a scalp set differs from the
    // groom's: GroomData::scalpFingerprint). Never 0.
    std::uint64_t topologyFingerprint() const;

    // ---- Per-corner data for maps (PG-2) --------------------------------
    // Values are given per triangle corner (3 per triangle, in triangle
    // order) and interpolated with the root's barycentrics.
    using UV = std::array<float, 2>;
    using Color = std::array<float, 4>;

    void setCornerUVs(std::vector<UV> uvs);  // throws std::invalid_argument on a size mismatch
    bool hasUVs() const { return !cornerUVs_.empty(); }
    const std::vector<UV>& cornerUVs() const { return cornerUVs_; }
    UV uvAt(const StrandRoot& root) const;  // (0, 0) without UVs
    // The direction U grows in across triangle `face` (dP/du from its corners' UVs, not normalized):
    // what a root frame takes as its x (Bend, Direction, Transform, Displace, Reference); zero where
    // the UVs give none (no UVs, or none across the face).
    Vec3f uvTangent(std::int32_t face) const;

    // Named color sets (e.g. painted with Maya's Paint Vertex Color tool).
    void setCornerColors(const std::string& name, std::vector<Color> colors);
    void removeColorSet(const std::string& name);  // nothing for a set it lacks
    bool hasColorSet(const std::string& name) const { return colorSets_.count(name) != 0; }
    Color colorAt(const std::string& name, const StrandRoot& root) const;  // zeros if absent
    std::vector<std::string> colorSetNames() const;
    const std::vector<Color>* cornerColors(const std::string& name) const;  // null if absent

    // Rest pose: the same vertices in an undeformed position. Strand counts
    // and neighbor searches use it, so a deforming scalp keeps the same
    // strands on every frame (stable topology for motion blur and caches).
    // Throws std::invalid_argument on a point count mismatch; empty clears it.
    void setRestPoints(std::vector<Vec3f> points);
    bool hasRest() const { return !restPoints_.empty(); }
    const std::vector<Vec3f>& restPoints() const { return restPoints_; }
    float restTriangleArea(std::size_t tri) const { return restPoints_.empty() ? areas_[tri] : restAreas_[tri]; }
    float restTotalArea() const { return restPoints_.empty() ? totalArea_ : restTotalArea_; }
    // Where a root sits on the rest pose; `fallback` without a rest pose or for an unrooted root.
    Vec3f restPositionOr(const StrandRoot& root, const Vec3f& fallback) const;
    // The content hash of restPose() without making it: the same on every frame of
    // a deforming scalp with a rest pose (contentHash() without one).
    std::uint64_t restPoseHash() const;
    // The scalp in its rest pose: the rest points as its points (keeping the rest
    // pose, UVs and color sets); a copy of itself without a rest pose.
    ScalpMesh restPose() const { return restPoints_.empty() ? *this : withPoints(restPoints_); }

    // The same topology, UVs, color sets and rest pose with new point
    // positions, e.g. a frame of an animated scalp cache. Equal to building the mesh from these
    // points and setting the same UVs, color sets and rest pose (the same hashes and answers),
    // without the work that does not depend on the points: the triangles are not checked
    // again, the UV, color and rest hashes and rest areas are kept, and once this mesh's
    // closest-point tree is built the new mesh refits it (boxes recomputed bottom-up on the same
    // tree) on its first query instead of building its own.
    ScalpMesh withPoints(std::vector<Vec3f> points) const;
    // Everything but the points is the same as `other`'s: triangles, UVs, color sets and rest
    // pose (by their hashes). Then other.withPoints(points()) equals this mesh.
    bool samePointsLayout(const ScalpMesh& other) const;
    // Whether the closest-point tree came from a refit of another mesh's (tests, stats); builds it.
    bool treeRefitted() const;

    // ---- Polygons and subdivision (subdivision.h) ------------------------
    // The host's polygons the triangles were made from: polygon i made the next polygons()[i]
    // triangles, in triangle order (Maya's getTriangles counts). Empty: every triangle is a face
    // of its own. Catmull-Clark refines these polygons (quads, n-gons) as a renderer does, not the
    // triangles. Throws std::invalid_argument when the counts do not add up to the triangles.
    // Not in any of the hashes above (polygonHash), so a mesh with or without them roots,
    // scatters and caches the same.
    void setPolygons(std::vector<std::int32_t> trianglesPerPolygon);
    const std::vector<std::int32_t>& polygons() const { return polygons_; }
    std::uint64_t polygonHash() const { return polygonHash_; }
    std::uint64_t uvHash() const { return uvHash_; }
    // The Catmull-Clark levels a renderer refines this mesh by (Maya: its Arnold subdivision, else
    // its smooth mesh preview; a groom file: the level written with it): what a Scalp or
    // ColliderMesh node set to "match render" refines it by. `from` says where it came from, for
    // the node's note. In contentHash() when above 0.
    void setRenderSubdivision(int levels, std::string from = {});
    int renderSubdivision() const { return renderSubdivision_; }
    const std::string& renderSubdivisionFrom() const { return renderSubdivisionFrom_; }
    // The levels this mesh was refined by from its cage (subdivideScalp); 0 for a cage.
    int subdivisionLevel() const { return subdivisionLevel_; }
    std::size_t cageFaceCount() const { return cageFaces_; }  // the cage's faces (a refined mesh)
    // Marks a refined mesh: its levels, its cage's face count and where the cage's render level
    // came from (renderSubdivisionFrom, for the note; its renderSubdivision stays 0).
    void setSubdivided(int levels, std::size_t cageFaces, std::string from = {}) {
        subdivisionLevel_ = levels;
        cageFaces_ = cageFaces;
        renderSubdivisionFrom_ = std::move(from);
    }
    // A refined mesh's cage: the mesh subdivideScalp refined it from (null for a cage, or for a
    // mesh refined elsewhere). Roots are placed and choices keyed on the cage (CageBinding,
    // subdivision.h), so a new subdivision level moves the strands onto the surface it draws
    // without changing which strands there are. Kept by withPoints; in none of the hashes.
    const std::shared_ptr<const ScalpMesh>& cage() const { return cage_; }
    void setCage(std::shared_ptr<const ScalpMesh> cage) { cage_ = std::move(cage); }

    // ---- Several scalps on one groom (scalp_pieces.h) ---------------------
    // The scalps this mesh is made of, in order (combineScalps); empty for a mesh of one scalp
    // (every groom with a single scalp). In surfaceHash() and contentHash() when set. Throws
    // std::invalid_argument for pieces that do not tile the triangles and points in order.
    void setPieces(std::vector<ScalpPiece> pieces);
    const std::vector<ScalpPiece>& pieces() const;
    std::uint64_t piecesHash() const { return piecesHash_; }  // 0 without pieces
    // The piece of that name; null when there is none.
    const ScalpPiece* piece(const std::string& name) const;
    // A scalp's own mesh cut out of the groom's scalps together (scalpPieces): isPiece(), the whole
    // mesh it was cut from (what the "scalp" external holds and strands' roots index; null once
    // nothing else holds it) and where its triangles start there. A root placed on the piece is at
    // face + wholeFaceOffset() on the whole mesh (toWholeRoot). False, null and 0 for any other
    // mesh. In none of the hashes (the piece's external hashes the offset).
    bool isPiece() const { return isPiece_; }
    std::shared_ptr<const ScalpMesh> whole() const { return whole_.lock(); }
    std::int32_t wholeFaceOffset() const { return wholeFaceOffset_; }
    void setWhole(const std::shared_ptr<const ScalpMesh>& whole, std::int32_t faceOffset) {
        whole_ = whole;
        isPiece_ = whole != nullptr;
        wholeFaceOffset_ = whole ? faceOffset : 0;
    }

private:
    struct BvhTree;
    struct Bvh;
    struct LazyBvh;
    const Bvh& bvh() const;     // built on these points, or a refit of the source's (withPoints)
    const Bvh& ownBvh() const;  // built on these points (made when a refitted tree cannot decide)
    void buildTree(Bvh& b) const;
    SurfacePoint closestOnTree(const Bvh& b, const Vec3f& p, float maxDistance, float* lowerBound) const;
    bool rayOnTree(const Bvh& b, const Vec3f& origin, const Vec3f& direction, SurfacePoint* hit, float* t) const;
    void buildGeometry(bool checkIndices);  // normals, areas, geometry (and topology) hashes from points_ and triangles_
    void updateHash();  // combines the parts below, whatever order they were set in

    std::vector<Vec3f> points_;
    std::vector<Triangle> triangles_;
    std::vector<Vec3f> vertexNormals_;
    std::vector<float> areas_;
    float totalArea_ = 0.0f;
    std::uint64_t hash_ = 0;
    std::uint64_t surfaceHash_ = 0;
    std::uint64_t geometryHash_ = 0;
    std::uint64_t topologyHash_ = 0;
    std::uint64_t uvHash_ = 0;
    std::uint64_t restHash_ = 0;
    std::map<std::string, std::uint64_t> colorHashes_;
    std::vector<UV> cornerUVs_;
    std::map<std::string, std::vector<Color>> colorSets_;
    std::vector<Vec3f> restPoints_;
    std::vector<float> restAreas_;
    float restTotalArea_ = 0.0f;
    std::vector<std::int32_t> polygons_;
    std::uint64_t polygonHash_ = 0;
    int renderSubdivision_ = 0;
    std::string renderSubdivisionFrom_;
    int subdivisionLevel_ = 0;
    std::size_t cageFaces_ = 0;
    std::shared_ptr<const ScalpMesh> cage_;
    std::shared_ptr<const std::vector<ScalpPiece>> pieces_;  // null: one scalp
    std::uint64_t piecesHash_ = 0;
    std::weak_ptr<const ScalpMesh> whole_;  // weak: the whole mesh's piece cache holds its pieces
    bool isPiece_ = false;
    std::int32_t wholeFaceOffset_ = 0;
    std::shared_ptr<LazyBvh> bvh_;  // shared so ScalpMesh stays copyable
};

// Where a root sits on the rest pose of `scalp` (stable while the scalp
// deforms); `fallback` without a scalp, a rest pose, or a root on it.
inline Vec3f restRootOr(const ScalpMesh* scalp, const StrandRoot& root, const Vec3f& fallback) {
    return scalp ? scalp->restPositionOr(root, fallback) : fallback;
}

// The surface area of the rest pose (the mesh as it is without one), summed in double: what a groom
// scaled with its scalp measures (GroomSession::groomScale).
double restSurfaceArea(const ScalpMesh& scalp);
// `scalp` with its rest pose scaled about the rest points' center to `area` (scene units squared):
// a host whose rest mesh does not take the scalp's scale (Maya's rest is the mesh's own points
// placed once; a rig scaled at its root leaves the bound mesh's original shape as it was) gives
// the area it measured on the scalp, so the groom scales with the scalp and the strands stay as
// many. The same pointer without a rest pose, for an area that is not positive, and within 1e-6.
std::shared_ptr<const ScalpMesh> withRestArea(const std::shared_ptr<const ScalpMesh>& scalp, double area);
// The area of `triangles` on `points` (double), and the points scaled about their center to
// `area` (false: left as they are: no area, or within 1e-6): withRestArea's, for a host handing a
// rest pose's points alone (GroomSession::updateScalpPoints).
double surfaceArea(const std::vector<Vec3f>& points, const std::vector<ScalpMesh::Triangle>& triangles);
// The area `points` (the scalp's triangles, posed) give the rest pose without the pose's stretch:
// the rest area times the median of the triangles' area ratios, posed over rest. A uniform scale
// (a rig scaled at its root) scales every triangle alike and comes through; a pose stretches some
// (a shoulder, a neck) and leaves the median. surfaceArea(points) without a rest pose or when the
// counts differ.
double poseFreeArea(const ScalpMesh& scalp, const std::vector<Vec3f>& points);
bool scaleToArea(std::vector<Vec3f>& points, const std::vector<ScalpMesh::Triangle>& triangles, double area);

}  // namespace groomlab
