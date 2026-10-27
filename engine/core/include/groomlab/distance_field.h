// SPDX-License-Identifier: Apache-2.0
#pragma once

// A sparse signed distance field of a mesh: what Collide asks of its surfaces at every point of
// every strand (the height above the surface along the closest point's normal, and that normal),
// made once per mesh and answered by a lookup, where the mesh's closest-point tree would be asked
// again for each point.
//
// The field samples exactly what ScalpMesh::closestPoint gives at the nodes of a grid: the
// height h = (p - closest).normal (positive on the normal's side: on an open scalp, the side the
// hair grows on; negative under it, however deep), the closest point's smooth normal and its
// distance. A query interpolates them trilinearly. It is sparse and lazy. Space is cut into
// regions of 4 x 4 x 4 bricks of 4 x 4 x 4 cells. A region is made the first time a query falls
// in it, by one closest-point query at its middle: far from the surface, that answer bounds the
// height and distance of every point in it (the distance changes no faster than the point moves,
// the height at most about twice as fast), which settles most points clear of the surface
// without another query. Near the surface (or where its bounds do not settle a point) its bricks
// are made the same way, each by a query at its middle: far ones keep their own bounds, one within
// a few cells of the surface holds its 5 x 5 x 5 nodes (made with the first one a query needs:
// most such bricks a strand meets are settled by their bounds), each sampled the first time a
// query needs it (hair touches few of them). A query the field cannot answer as the mesh would
// (bounds that do not settle it, more than a few cells under the surface, a cell across a crease or
// the medial axis where the closest point jumps, the edge of the search radius, outside the grid) says so,
// and the caller asks the mesh: those points get the exact answer. Everything is made by whichever
// thread first needs it (others wait), from exact queries alone, so the answers never depend on
// the order threads meet them.
//
// The grid is the world's: its nodes sit at whole multiples of the cell size (a power of two for
// every field made here) plus half a cell, its regions start at whole multiples of theirs (from
// there), and a point finds its cell from its own coordinates alone. Two meshes alike in one place have the same nodes there,
// with the same samples: an edit of one side of a scalp leaves the field's answers on the other
// side as they were, to the bit.
//
// A caller may ask for the field's answer alone (Probe::fieldOnly: Interpolate's keepClearance,
// Collide's `method` field), never the mesh's: where the field would ask, the answer is the closest
// point among the triangles around the closest points it holds nearby (the cell's corners', or a
// far brick's middle's and those of the bricks beside it on the point's side: each of their
// triangles' corners' triangles), then down the mesh from the nearest of those while a triangle
// around its corners is nearer, which is the mesh's own almost everywhere (but for near ties) and
// is what a copy of the field on a GPU can answer too (tables()).
//
// Fields are shared across evaluations (sharedDistanceField): keyed by the mesh's surface hash
// (a deforming scalp's every frame is a mesh of its own: a field per frame, the rest pose's its
// own), the cell size and the reach. Collide's `method` field uses them; Gravity's surface and the
// guide simulation (simulation.h) push against the same ones. A mesh deformed away from its rest
// pose (an animated scalp; a collider given a rest pose) is not given a field of its own on every
// frame: each of them asks its rest pose's field through RestPoseCollision, below.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

// A field's grid where its caller wants other than Collide's: the cell size (0: cellSizeFor the
// offset) and how many cells deep its fine band reaches either side of the surface. Collide's
// band is 4 cells: its offsets are within it. A caller pushing points to heights well above that
// (Interpolate's keepClearance) takes coarser cells and a wider band, so those points are blended
// from nodes, not asked of the mesh one by one.
struct FieldGrid {
    float cell = 0.0f;
    int band = 4;
};

class MeshDistanceField {
public:
    // `cellSize`: the fine grid's spacing; `reach`: how far from the surface the field samples
    // (beyond it a point is a miss, as a closest-point query bounded by it); `bandCells`: how far
    // from the surface (in cells) its bricks hold nodes.
    MeshDistanceField(std::shared_ptr<const ScalpMesh> mesh, float cellSize, float reach, int bandCells = 4);
    ~MeshDistanceField();
    MeshDistanceField(const MeshDistanceField&) = delete;
    MeshDistanceField& operator=(const MeshDistanceField&) = delete;

    float cellSize() const { return cell_; }
    float reach() const { return reach_; }
    const ScalpMesh& mesh() const { return *mesh_; }

    // Collide's push at p (CollideOp): a point less than `offset` above the surface, and not
    // deeper under it than `maxDepth`, moves along the closest point's normal to `offset` above
    // it; points farther from the surface than `searchRadius` (at most the reach) are left alone.
    // Kept: nothing to do. Moved: *delta is the move. Ask: the field cannot tell; ask the mesh.
    enum class Push { Kept, Moved, Ask };
    // A caller's extras for a push. `beside` (asked; 0: none): farther than that from the surface, a
    // point whose height is less than half its distance either way is beside the surface (past an
    // open edge of it), not over or under it, and is left alone. What the push learnt of p on the
    // way (lower bounds of its height and of its distance from the surface: a far brick's or
    // region's bounds, the blend's height, the mesh's own answer; -inf and 0 where it learnt
    // nothing). The height changes at most about twice as fast as a point moves (the bounds above
    // assume so), the distance no faster: a caller walking a strand skips the points those say are
    // left alone at the offset it asks.
    // `fieldOnly`: never Ask (the field's answer where it would: see above; a point outside the
    // grid is beyond the search, which the grid reaches past).
    // `relaxed`: a soft target (Interpolate's keepClearance): a cell whose corners' normals are
    // within about 26 degrees of each other (kRelaxedCreaseCosine) blends them, where a push asks
    // them within about 2.6; a cell across a sharper crease, or where the closest point jumps, is
    // answered as without it. The blend rounds the crease's corner off by up to about an eighth
    // of the cell times the angle: a strand pushed to a clearance it keeps only on average.
    struct Probe {
        float beside = 0.0f;
        bool fieldOnly = false;
        bool relaxed = false;
        float heightLow = -std::numeric_limits<float>::infinity();
        float distanceLow = 0.0f;
        void learnt(float height, float distance) {
            heightLow = height;
            distanceLow = distance;
        }
        void forget() { learnt(-std::numeric_limits<float>::infinity(), 0.0f); }
        // Beside the surface at that height and distance.
        bool besideAt(float height, float distance) const {
            return beside > 0.0f && distance > beside && 2.0f * std::fabs(height) < distance;
        }
    };
    Push push(const Vec3f& p, float offset, float maxDepth, float searchRadius, Vec3f* delta, Probe* probe = nullptr) const;
    // Whether push can ever move a point: not when maxDepth is within two cell diagonals of the
    // offset (every cell a push would be made in is that near the depth limit: the mesh's).
    bool canMove(float offset, float maxDepth) const;
    // Lower bounds of the height and of the distance from the surface of every point within
    // `radius` of p: the bounds of the region (far from the surface) or brick p falls in (made from
    // exact queries at their middles), less twice the radius and the radius. False outside the
    // grid. A caller with a whole strand within the radius of its root skips the surface for it
    // where those settle every push it could ask (Collide's per-strand test).
    bool bounds(const Vec3f& p, float radius, float* heightLow, float* distanceLow) const;

    // The interpolated height and normal at p (normalized; tests and tools), false where push
    // would ask the mesh or the point is not within a few cells of the surface.
    bool sample(const Vec3f& p, float* height, Vec3f* normal) const;

    // What has been made so far (stats, tests): regions, bricks bounding the points in them, bricks
    // holding nodes, nodes sampled, the exact closest-point queries all that took, and the memory.
    struct Stats {
        std::size_t regions = 0, farBricks = 0, fineBricks = 0, nodes = 0, queries = 0, bytes = 0;
    };
    Stats stats() const;
    // How many fields have been made in this process (tests: a field kept across frames is not
    // made again).
    static std::size_t madeCount();

    // The cell size Collide's field takes for a mesh and an offset: about the offset (a power of
    // two, shared by nearby offsets), within 1/1024 and 1/128 of the mesh's bounding box diagonal.
    // Over or under a faceted surface the height kinks where the closest triangle changes, which a
    // blend of corners rounds off by about an eighth of the cell times the angle between
    // neighbouring triangles: a point pushed to the offset lands within a few percent of it of
    // where the mesh puts it (micrometres on a head in centimetres). Finer cells cost memory and
    // time (nodes on the scale of the hair's spacing are each sampled for few points); coarser
    // ones blur the surface.
    static float cellSizeFor(const ScalpMesh& mesh, float offset);
    // The reach for a search radius: the next power of two at or above it (fields are shared
    // between nearby radii; queries still stop at their own).
    static float reachFor(float searchRadius);

    // The field as one array of 32-bit words, for a copy of it that answers pushes with
    // Probe::fieldOnly as push does (look.slang's keepClearance walk), for offsets up to `maxOffset`
    // at `searchRadius`: every region; the bricks of each region whose bounds do not settle such a
    // push; each fine brick's nodes and their closest faces, where its bounds do not; and the mesh
    // the answers from closest faces read (points, vertex normals, triangles, each vertex's
    // triangles). Everything it holds is made here (in parallel; the field keeps it, as if pushes
    // had asked for it), the words kept for the next call with the same offset and radius. Null
    // when the field has no grid (a mesh too big for its cells) or `searchRadius` is beyond its
    // reach. Layout (word offsets, kTable*): a header, then regions (kTableRegionWords each: far,
    // heightLow, distanceLow, its first brick or kTableNone), bricks (64 a region, kTableBrickWords
    // each: its middle's closest face or kTableNone, heightLow, distanceLow, its first node or
    // kTableNone: a far brick, or a fine one its bounds settle), nodes (125 a brick, x fastest: the
    // Node's 8 bytes), node faces (one word each), points, vertex normals (xyz each), triangles
    // (kTableTriangleWords each: a TriangleBound, its corners last), ring starts (a vertex's
    // triangles from ringStart[v] to ringStart[v + 1]) and rings.
    using Tables = std::shared_ptr<const std::vector<std::uint32_t>>;
    Tables tables(float maxOffset, float searchRadius) const;
    enum : std::uint32_t {
        kTableDims = 0,          // regions along x, y, z
        kTableOriginRegion = 3,  // the first region's place in the world, in regions (signed; the grid is half a cell off the world's)
        kTableCell = 6,          // floats: the cell, the band, heightScale (a node's height steps), the reach
        kTableBand = 7,
        kTableHeightScale = 8,
        kTableReach = 9,
        kTableRegions = 10,  // where each part starts (words)
        kTableBricks = 11,
        kTableNodes = 12,
        kTableNodeFaces = 13,
        kTablePoints = 14,
        kTableNormals = 15,
        kTableTriangles = 16,
        kTableRingStart = 17,
        kTableRings = 18,
        kTableInverseCell = 19,  // a float: 1 / the cell (the point's cell is its coordinates times it)
        kTableHeaderWords = 20,
        kTableRegionWords = 4,
        kTableBrickWords = 4,
        kTableTriangleWords = 8,
        kTableTriangleCorners = 5,  // where a triangle's corners start among its words
        kTableNone = 0xFFFFFFFFu,
    };
    // How many steps down the mesh Probe::fieldOnly's answer takes from the closest faces the field
    // holds near p at most (look.slang's ANSWER_STEPS).
    static constexpr int kAnswerSteps = 32;

    // A triangle as Probe::fieldOnly's answer weighs it: a sphere about it (`center`, `reach`) that
    // the point closestPointOnFace makes (rounded) is inside of for every p within sqrt(`within`)
    // of its center (-1: none, a sliver or a degenerate triangle), and its corners. Where p is
    // farther from the center than the reach plus the nearest so far, the triangle cannot be
    // nearer: it is skipped (the answer the same). The tables hold one a triangle, as laid out here.
    struct TriangleBound {
        Vec3f center;
        float reach = 0.0f;
        float within = -1.0f;
        std::int32_t corner[3] = {0, 0, 0};
    };
    // Triangle abc's (its corners left 0).
    static TriangleBound triangleBound(const Vec3f& a, const Vec3f& b, const Vec3f& c);

private:
    struct Node {  // one grid node's exact sample, in 8 bytes
        std::int16_t height;     // in heightScale_ steps, held at +-32767 (far beyond the band either way)
        std::uint16_t distance;  // a half float's bits, rounded up (only compared against the search radius)
        std::int16_t normal[2];  // the unit normal, octahedral, in 1/32767ths
    };
    struct Nodes;
    struct Brick;
    struct Region;
    struct Located;
    struct Rings;
    // (Those marked inline are steps of every push, inlined into it in distance_field.cpp.)
    inline const Region& regionAt(int bx, int by, int bz) const;
    // The region at `index` (regionAt's), made by its closest-point query (another thread's, when
    // one makes it first).
    const Region& makeRegion(std::size_t index, int rx, int ry, int rz) const;
    const Brick& brickAt(const Region& region, int bx, int by, int bz) const;
    // The brick p falls in (made when first asked), through the thread's memo of the bricks it met
    // last.
    inline const Brick& brickOf(const Located& at) const;
    Brick* makeBrick(int bx, int by, int bz) const;
    // A fine brick's nodes, made on first use (none sampled yet).
    Nodes& nodesOf(const Brick& brick) const;
    // Probe::fieldOnly's answer where push would ask: the closest point to p among the triangles
    // around the faces `seeds` (each one's corners' triangles; kTableNone and repeats skipped), then
    // down the mesh from the nearest (while a triangle around its corners is nearer, kAnswerSteps
    // at most), as pushOutOf answers from the mesh's.
    Push answerNear(const Vec3f& p, const std::uint32_t* seeds, int count, float offset, float maxDepth, float searchRadius,
                    Vec3f* delta, Probe& probe) const;
    // The seeds of a far brick's answer at `at` (its brick): the brick's middle's closest face, and
    // those of the 7 bricks beside it on p's side of its middle (the 8 middles around p; kTableNone
    // beyond the grid or in another region far from the surface).
    void farSeeds(const Located& at, const Brick& brick, std::uint32_t* seeds) const;
    // push past the bounds: a far brick's answer (Probe::fieldOnly), and a fine brick's blend (or
    // the answer where it asks). Apart from push, which most points leave by a bound.
    Push pushFar(const Vec3f& p, const Located& at, const Brick& brick, float offset, float maxDepth, float searchRadius,
                 Vec3f* delta, Probe& probe) const;
    Push pushFine(const Vec3f& p, const Located& at, const Brick& brick, float offset, float maxDepth, float searchRadius,
                  Vec3f* delta, Probe* probe) const;
    const Rings& rings() const;
    // Node `index` of a fine brick, sampled on first use. `within`: a distance the surface is known
    // to be within from it (a neighbour's, plus the way between them), which bounds the query (the
    // same answer, found sooner); infinity when none is known.
    // `seedFace`: a face known to be near (a neighbouring node's; kTableNone: none), besides the brick's.
    Node nodeAt(const Brick& brick, int index, float within, std::uint32_t seedFace = kTableNone) const;
    // Where p falls: its region, brick and cell (bricks made only when asked: `bricks`); false
    // outside the grid.
    inline bool locate(const Vec3f& p, Located* at) const;
    // The trilinear blend of a fine cell's corners at the cell's fraction f; false where the cell
    // asks the mesh; *clear where every point there is left alone.
    bool blend(const Brick& brick, int cell, const Vec3f& f, float maxDepth, float searchRadius, float offset,
               float* height, Vec3f* normal, bool* clear, Probe* probe = nullptr) const;

    std::shared_ptr<const ScalpMesh> mesh_;
    std::uint64_t serial_ = 0;  // unique in the process (threads' memos of bricks name fields by it)
    float cell_ = 0.0f, reach_ = 0.0f, brickSize_ = 0.0f, regionSize_ = 0.0f, band_ = 0.0f, heightScale_ = 0.0f;
    float inverseCell_ = 0.0f;
    Vec3f origin_;               // the first region's corner: originRegion_ regions from the world's origin
    int originRegion_[3] = {0, 0, 0};
    float cellLo_[3] = {0, 0, 0}, cellHi_[3] = {0, 0, 0};  // the grid, in cells from the world's origin
    int regionDims_[3] = {0, 0, 0};
    std::unique_ptr<Region[]> regions_;
    mutable std::once_flag ringsMade_;
    mutable std::unique_ptr<Rings> rings_;
    mutable std::mutex tablesMutex_;
    mutable Tables tables_;
    mutable float tablesOffset_ = 0.0f, tablesRadius_ = 0.0f;
    // The counts (stats; queries_ the regions' and bricks', a node's counted in nodes_), a cache line
    // away from everything above: while a field is being made, every thread's queries write them,
    // and every push reads the grid above.
    [[maybe_unused]] char apart_[64] = {};
    mutable std::atomic<std::size_t> regionsMade_{0}, far_{0}, fine_{0}, nodes_{0}, queries_{0}, nodeBlocks_{0};
};

using DistanceFieldPtr = std::shared_ptr<const MeshDistanceField>;

// The field of `mesh` at that cell size, reach and band: one already made for a mesh of the same
// surface (ScalpMesh::surfaceHash) when the last few kept hold it, else a new one (kept, the
// oldest let go). Thread-safe.
DistanceFieldPtr sharedDistanceField(const std::shared_ptr<const ScalpMesh>& mesh, float cellSize, float reach,
                                     int bandCells = 4);

// The field a collision with `mesh` asks (Collide's, Gravity's surface): sharedDistanceField at
// cellSizeFor(offset) (or the grid's) and reachFor(searchRadius). Null for a mesh deformed away
// from its rest pose (an animated scalp is a new mesh every frame, whose field would serve one
// evaluation): Collide asks its rest pose's field (RestPoseCollision), as Gravity's surface and the
// guide simulation do (CollisionSurface).
DistanceFieldPtr collisionField(const std::shared_ptr<const ScalpMesh>& mesh, float offset, float searchRadius,
                                const FieldGrid& grid = {});

// Collide's push of p out of `mesh`: less than `offset` above its surface (along the closest
// point's normal) and not deeper than `maxDepth`, p moves to `offset` above it; farther from it
// than `searchRadius`, it stays. Asks `field` (null: none) first, the mesh where the field cannot
// tell. True when p moved.
bool pushOutOf(const ScalpMesh& mesh, const MeshDistanceField* field, Vec3f& p, float offset, float maxDepth, float searchRadius,
               MeshDistanceField::Probe* probe = nullptr);
// Its mesh's part: the push the mesh's closest point asks for (*delta, added to p), what it learnt
// into `probe`; true when it moves p.
bool meshPushOf(const ScalpMesh& mesh, const Vec3f& p, float offset, float maxDepth, float searchRadius, Vec3f* delta,
                MeshDistanceField::Probe* probe = nullptr);

// Collide on a mesh deformed away from its rest pose, through the rest pose's field: made once
// (kept across frames, while the rest surface, the cell size and the reach stay), where a field of
// the deformed mesh would be made for every frame and serve one evaluation. A point is taken to
// rest space by the rigid motion of the frame of the deformed mesh's vertex nearest it (follow-
// the-scalp's frames, follow_scalp.h: the vertex's normal and its longest rest edge; no scale),
// pushed there, and its push turned back by the same frame. Exact for a rigid motion of the
// whole mesh (to rounding: the field's answer at the moved point, a few percent of the offset
// from the mesh's, as on a still mesh). Where the mesh bends or stretches, the surface near a
// point moves with that frame only up to how the skin between the vertex and the point's closest
// point turns and stretches, which the push misses by: a few percent of the offset where a head
// bends a few degrees, up to about a tenth of it at ten (RestFieldKeepsStrandsOutOfABendingScalp;
// a field made for that frame misses by the field's own few percent). Points the field cannot
// tell about (Ask) are the deformed mesh's, as with a still mesh's field. Per frame (shared by the
// evaluations of one deformed surface, the incremental ones of a brush stroke on a held frame):
// the vertices' frames, linear in the vertices, no closest-point query. A deforming mesh without
// a rest pose has none of this: collisionField makes a field for each frame (Maya passes a deformed
// collider's original shape as its rest pose).
//
// The nearest vertex is found by a walk over the mesh's edges from the last point's (a strand's
// points are near one another) to a vertex none of whose neighbours is nearer: a Cursor per
// strand, started at its root's vertex on the scalp, or by a tree of the vertices for a point
// without one (a collider's first), which also says when a point is too far from every vertex
// for the surface to be within the search radius (and how far it can move and still be: the next
// points there cost nothing).
class RestPoseCollision {
public:
    // Null unless `mesh` has triangles and a rest pose its points are away from (collisionField
    // answers the rest), and the rest field can move points at that offset and depth
    // (MeshDistanceField::canMove: else it would only ask, and the mesh's tree skips far points
    // as cheaply as the walk would).
    static std::shared_ptr<const RestPoseCollision> of(const std::shared_ptr<const ScalpMesh>& mesh, float offset,
                                                       float maxDepth, float searchRadius, const FieldGrid& grid = {});

    // Where a strand's walk is: the vertex nearest its last point (none: ask the tree), and a
    // point known clear of the search radius with how far from it points still are.
    struct Cursor {
        static constexpr std::size_t kNone = ~std::size_t{0};
        std::size_t vertex = kNone;
        Vec3f anchor;
        float clear = -1.0f;
    };
    // A strand's cursor: at the vertex of its root's face nearest `rootPoint` when the root is on
    // this mesh there (the scalp's strands), else none.
    Cursor cursor(const StrandRoot& root, const Vec3f& rootPoint) const;
    // MeshDistanceField::push at p on the deformed mesh (its delta in the deformed mesh's space),
    // the cursor moved to p.
    MeshDistanceField::Push push(const Vec3f& p, float offset, float maxDepth, float searchRadius, Vec3f* delta,
                                 Cursor& cursor, MeshDistanceField::Probe* probe = nullptr) const;
    const MeshDistanceField& restField() const { return *field_; }

    // The walk as a GPU walks it (look.slang restPush: push and cursor, word for word), one block
    // of words: kWalk* header words, then the deformed mesh's vertices (xyz), each vertex's ring
    // (ring starts, then the rings), each vertex's motion to rest (kWalkMotionWords: flags, bit 0
    // still, kWalkNoMotion: none (a degenerate face's vertex, where push asks the mesh, which the
    // GPU has not: a host keeps its walks away from those); then the deformed frame's origin, x, y,
    // z and the rest frame's), and the vertices on a triangle (what the tree holds, for a point
    // without a cursor).
    enum : std::uint32_t {
        kWalkVertices = 0, kWalkPoints = 1, kWalkRingStart = 2, kWalkRings = 3, kWalkMotions = 4, kWalkTree = 5,
        kWalkTreeCount = 6, kWalkLongestEdge = 7, kWalkHeader = 8, kWalkMotionWords = 25, kWalkNoMotion = 2
    };
    std::shared_ptr<const std::vector<std::uint32_t>> walkWords() const;

    struct Rest;   // per rest surface: the rest pose, each vertex's frame edge, the vertices' rings
    struct Frame;  // per deformed surface: the vertices' points and motions to rest

private:
    std::shared_ptr<const Frame> frame_;
    DistanceFieldPtr field_;
};

// What a push out of a mesh asks (Gravity's surface, the guide simulation's scalp and colliders):
// its field (collisionField), or for a mesh deformed away from its rest pose its rest pose's
// through RestPoseCollision (made once for every frame), and the mesh itself where neither can
// tell (or neither applies: no field for that offset and depth). A still mesh, or one without a
// rest pose, asks what it asked before RestPoseCollision: its field, else the mesh.
struct CollisionSurface {
    std::shared_ptr<const ScalpMesh> mesh;
    DistanceFieldPtr field;
    std::shared_ptr<const RestPoseCollision> rest;

    // Null mesh for none (or no triangles).
    static CollisionSurface of(const std::shared_ptr<const ScalpMesh>& mesh, float offset, float maxDepth, float searchRadius,
                               const FieldGrid& grid = {});
    explicit operator bool() const { return mesh != nullptr; }
    // A strand's walk over the deformed mesh (RestPoseCollision::cursor; none without a rest pose).
    RestPoseCollision::Cursor cursor(const StrandRoot& root, const Vec3f& rootPoint) const;
    // pushOutOf: p moved to `offset` above the surface when less than that above it and not deeper
    // than `maxDepth`; true when it moved. `cursor`: the strand's walk, moved to p.
    // `probe`: MeshDistanceField::Probe (through a rest pose, what was learnt is of the point taken
    // there).
    bool pushOut(Vec3f& p, float offset, float maxDepth, float searchRadius, RestPoseCollision::Cursor& cursor,
                 MeshDistanceField::Probe* probe = nullptr) const;
    // The same, its push into *delta (what pushOut adds to p) and p left as it is.
    bool push(const Vec3f& p, float offset, float maxDepth, float searchRadius, Vec3f* delta, RestPoseCollision::Cursor& cursor,
              MeshDistanceField::Probe* probe = nullptr) const;
};

}  // namespace groomlab
