// SPDX-License-Identifier: Apache-2.0
#pragma once

// What some operators keep between evaluations (their memos), read by a host that runs part of
// their work elsewhere: Maya's GPU preview (groomlab/gpu/look_chain.h) blends Interpolate's strands and
// clumps them on the GPU while a param is dragged or a brush stroke is live, from the binding
// and the center choice the CPU's last evaluation made. Each answer is for one result of the
// node (`output`, by identity: its cached value): another result, or a memo the operator does
// not keep for it, answers false and the host runs nothing of it. Nothing here evaluates or
// changes the operators; the CPU stays the reference for everything (the host's results are
// checked against it).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/distance_field.h"
#include "groomlab/graph.h"
#include "groomlab/guide_set.h"
#include "groomlab/parts.h"
#include "groomlab/portable_math.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class FlowField;  // flow.h

// ---- Clump --------------------------------------------------------------------------------------

// Each center as Clump follows it: resampled evenly by arc length to this many points.
constexpr std::size_t kClumpCenterSamples = 32;
constexpr std::uint32_t kNoClumpCenter = 0xffffffffu;

// What Clump made `output` from: its centers (the ones with 2 points or more, with centersFrom
// inputPoints 1 or more, in the centers' order), their curves resampled to kClumpCenterSamples
// points each (the center's own, or its axis strand's: `axes`), and each strand's center (kNoClumpCenter:
// not clumped, a shaft or a vane without centers). The assignment is by root (and parts, vanes),
// so it holds while the strands' and centers' roots stay.
struct ClumpState {
    std::vector<Vec3f> curves;  // center c at [c * kClumpCenterSamples, (c + 1) * kClumpCenterSamples)
    // Per center: the scalp's normal at its root (zero: none), what its lock and ribbon frames
    // start from (so they turn with the scalp).
    std::vector<Vec3f> centerNormals;
    std::shared_ptr<const std::vector<std::uint32_t>> assigned;
    std::vector<std::size_t> centerGuides;  // per center: its index among the centers input's guides
    // A Clump of centersFrom pick picks its centers among the strands it takes: the strand each
    // of those guides is (guide g: strand chosen[g]), when the choice is by roots alone (every
    // strand rooted on the scalp, no centerWeight left one out: the same while no root moves).
    // Empty otherwise.
    std::vector<std::size_t> chosen;
    // With centersFrom inputPoints: per center (in `curves` order), the strand of the
    // strands it takes whose points (resampled) are that center's curve, the one it gathers nearest
    // the center's root (kNoClumpCenter: it gathers none, its curve is its first point). The same
    // while no root moves. Empty otherwise (the curves are the centers' own).
    std::vector<std::uint32_t> axes;
    // Per center (in `curves` order): its ID (what its per-clump variation is drawn from) and the
    // point its strands were assigned by (its root on the rest scalp: what copyAmount's nearest
    // centers are found from).
    std::vector<StrandId> centerIds;
    std::vector<Vec3f> centerRoots;
    // A Clump of centersFrom pick: the strand each of its guides is, whatever the choice was
    // made from (`chosen` only when by roots alone; a centerWeight varying per strand picks by its
    // values too, which hold while what they read does).
    std::vector<std::size_t> picked;
};
// False when `op` is not a Clump or kept nothing for `output`.
// (`output`: the Clump's last result, or what the graph handed on of it with no strand hidden:
// preview_core.h sameOrShown; the same for interpolateState.)
bool clumpState(const Operator& op, const StrandsPtr& output, ClumpState* state);

// ---- Wet ----------------------------------------------------------------------------------------

// Wet (wet_op.cpp) clumps with a Clump per level, run largest first: level j (j halvings of its
// clumpSize: 0 the largest) of its levels, each taking the one before's result. What made
// `output`: each level's Clump params as Wet set them (its profile for the wetness of then) and
// what that Clump made from the strands it took (as clumpState answers for its result).
struct WetState {
    float wetness = 0.0f;
    std::vector<ParamSet> params;    // per level, the largest first
    std::vector<ClumpState> levels;  // as `params`
};
// False when `op` is not a Wet or kept nothing for `output` (its last result, or what the graph
// handed on of it with no strand hidden; a wetness of 0 runs no level and keeps nothing).
bool wetState(const Operator& op, const StrandsPtr& output, std::shared_ptr<const WetState>* state);
// How many levels a Wet with these params runs (its `levels`, as it clamps them).
std::int64_t wetLevelCount(const ParamSet& wet);
// Level j's (0 the largest) Clump profile of `levels` at `wetness`: all a change of the wetness
// changes of the levels (their centers stay).
Ramp wetLevelProfile(std::int64_t j, std::int64_t levels, float wetness);

// ---- Interpolate --------------------------------------------------------------------------------

// Interpolate keeps each weight a strand blends with in 16 bits: its share of the strand's
// weights in 65536ths, the shares summing to 65536 exactly (each rounded down, the rest handed out
// one each to the largest remainders), kept less one (a guide whose share rounds to 0 is dropped).
// The blend reads (code + 1) / 65536, exact in a float, so a host blends the same weights as the
// CPU. (Against the weights unrounded a strand moves by at most a few micrometres on a head.)
constexpr float kInterpolateWeightUnit = 1.0f / 65536.0f;
inline float interpolateWeight(std::uint16_t code) { return static_cast<float>(code + 1u) * kInterpolateWeightUnit; }

// Each strand's guides and weights as Interpolate blends them (its binding), flat: strand s's
// guides (indices into the kept guides: those with 2 points or more, in order), weights (codes:
// interpolateWeight) and twists at [offsets[s], offsets[s + 1]), the squared distance to its
// nearest guide (the influence with a radius) and whether it is bound at all (0: left as it came;
// a bound strand has a guide at least). A twist (radians; 0 almost everywhere) is what the path
// from the guide's root to the strand's adds where it bends round a fold: the guide's offsets turn
// from its root's normal to the strand's (Rotation), then by the twist about the strand's normal
// (TwistedTurn, frames.h).
struct InterpolateRecords {
    std::vector<std::uint32_t> offsets;  // strandCount + 1
    std::vector<std::uint32_t> guides;
    std::vector<std::uint16_t> weights;
    std::vector<float> twists;
    std::vector<float> nearest2;
    std::vector<std::uint8_t> bound;
};

// What Interpolate made `output` from: the binding (opaque; the same pointer: the same records)
// and the guide roots it was bound for (on the rest scalp), the guides, whether guide attributes
// or layers were blended into channels.
struct InterpolateState {
    std::shared_ptr<const void> binding;
    std::uint64_t baseKey = 0;  // what it binds from besides guide roots (the strands, params, parts): equal keys, comparable bindings
    std::size_t strandCount = 0;
    std::vector<Vec3f> roots;  // per kept guide
    GuidesPtr guides;
    StrandsPtr strands;        // the strands it took (input 0), when still held
    bool channels = false;     // guide attributes or layers made strand or point channels
    std::vector<float> weights;  // per kept guide, its guideWeight (empty: every guide alike)
};
bool interpolateState(const Operator& op, const StrandsPtr& output, InterpolateState* state);
// The binding's records, flat (a pass over every strand: hosts keep them per binding), and those
// of strands [first, first + count) alone (offsets from 0).
void interpolateRecords(const InterpolateState& state, InterpolateRecords* records);
void interpolateRecords(const InterpolateState& state, std::size_t first, std::size_t count, InterpolateRecords* records);
// Interpolate binds strands in chunks of interpolateChunkStrands(), and binding again after some
// guide roots moved shares the chunks no moved guide reaches: the binding's chunks, equal pointers
// holding equal records (a host updates only the others).
std::size_t interpolateChunkStrands();
std::vector<const void*> interpolateChunks(const InterpolateState& state);
// `state` for its strands `which` alone, in that order (each one's records as they are): what the
// CPU engine binds a view of them by (LookEngine::setBaseView).
std::shared_ptr<const InterpolateState> interpolateStateOf(const InterpolateState& state, const std::vector<std::uint32_t>& which);
// The bytes the binding's records hold (guides, weights, twists, per strand its offset, nearest
// distance and whether it is bound; not what binding again reads).
std::size_t interpolateRecordBytes(const InterpolateState& state);
// Binds `state`'s strands again to the kept guides rooted at `roots` (as many as before; where
// strands search for them: interpolateGuideRows' roots), as Interpolate binds them when only guide
// roots moved (a brush dab on guides whose children follow them): the strands no moved guide
// reaches keep their records, the result is a full binding's. `guideRoots`: the kept guides' roots
// on the scalp (the paths along it start there; a guide whose root and reach stayed keeps its
// paths). `parts`: the groom's parts (respected as the node's params say; null: none). The
// operator keeps the binding as one of its own, so its next evaluation with these roots finds it.
// `guideWeights`: the kept guides' weights (interpolateGuideWeights). False when `op` is not an
// Interpolate or the guides are not as many.
bool interpolateRebind(const Operator& op, const PartLines* parts, const std::vector<Vec3f>& roots,
                       const std::vector<StrandRoot>& guideRoots, const std::vector<float>& guideWeights,
                       InterpolateState* state);
// The kept guides' guideWeight (`params`: the Interpolate node's, as evaluated: a field plugged in)
// read at their roots on `scalp` (a guide off it: 1); empty when every guide weighs alike (a number,
// no scalp), as the binding holds them (InterpolateState::weights). Throws GraphError as the map does.
std::vector<float> interpolateGuideWeights(const ParamSet& params, const std::vector<const Guide*>& kept, const GuideSet* guides,
                                           const ScalpMesh* scalp);

// The kept guides' shapes as Interpolate blends them (its first stage): each guide (the smooth
// curve through its points with smoothGuides) resampled by arc length to `samples` points, as
// offsets from its first; its root where strands search for it (on the rest scalp when `scalp` has
// one), its root's normal (zero when `scalp` is null or the guide is not rooted on it) and its
// length. `samples`: max(32, the core strands' most points: preview_core.h), interpolateSamples.
struct InterpolateGuideRows {
    std::size_t samples = 0;
    std::vector<Vec3f> offsets;  // guide g's at [g * samples, (g + 1) * samples)
    std::vector<Vec3f> roots, normals;
    std::vector<float> lengths;
};
std::size_t interpolateSamples(const StrandSet& strands);
// `params`: the Interpolate node's (smoothGuides); `kept`: the guides with 2 points or more.
void interpolateGuideRows(const ParamSet& params, const std::vector<const Guide*>& kept, const MeshPtr& scalp,
                          std::size_t samples, InterpolateGuideRows* rows);

// keepClearance: whether an Interpolate with these params keeps its strands clear of `scalp`
// (keepClearance, taking the guides' shape, a scalp with triangles).
bool interpolateKeepsClearance(const ParamSet& params, const ScalpMesh* scalp);
// What its walk asks of the scalp, for a host walking the strands as the node does (look.slang):
// the clearance a strand keeps (times its influence), how far its offset eases back over, how far
// apart two asked points walk the points between one by one where they touch the skin, how deep
// under the skin a point is still in it, the search radius and how far past an open edge a point
// is beside the scalp; the scalp's field as tables (MeshDistanceField::tables: the field's answers
// alone, as the node's walk asks them). Over a scalp deformed away from its rest pose the walk asks the rest pose's field through each vertex's frame
// (RestPoseCollision): `tables` are the rest field's, `rest` the walk (each strand's cursor from
// it) and `walk` its words (RestPoseCollision::walkWords). Null with *why when the walk is the
// CPU's alone: a deformed scalp whose rest field cannot move points at those distances, or a
// scalp too big for the field's grid. (A deformed scalp's degenerate faces' vertices have no motion:
// a host keeps the walks it runs away from them, walkWords.)
struct InterpolateClearance {
    float floor = 0.0f, ease = 0.0f, span = 0.0f, depth = 0.0f, searchRadius = 0.0f, beside = 0.0f;
    MeshDistanceField::Tables tables;
    std::shared_ptr<const RestPoseCollision> rest;
    std::shared_ptr<const std::vector<std::uint32_t>> walk;
    // The scalp: over a deformed one, what the walk asks where it reaches a degenerate face's vertex
    // (meshPushOf; the CPU engine's alone, through the host: look.slang hostClearanceMeshPush).
    MeshPtr mesh;
    // The CPU engine's: every push asked of `surface` through the host (`host`: look.slang
    // hostClearanceSurfacePush; its field makes only the bricks the strands come near, where tables
    // are made whole), the tables a placeholder.
    CollisionSurface surface;
    bool host = false;
};
// `host` (the CPU engine's caller): every push asked of the scalp's surface through the host
// (InterpolateClearance::host), no tables made.
std::shared_ptr<const InterpolateClearance> interpolateClearance(const ParamSet& params, const MeshPtr& scalp, std::string* why,
                                                                 bool host = false);
// Over a deformed scalp (clearance.rest): each strand's walk's first vertex, its root face's vertex
// nearest its first point (RestPoseCollision::cursor; 0xffffffff: none), for the kernel's walk. Null
// with *why where a walk could reach a degenerate face's vertex (no motion: the mesh asked, the CPU
// engine's alone: `meshAsked` true for it) within the strands' reach (`longestGuide` the longest
// guide's length).
std::shared_ptr<const std::vector<std::uint32_t>> interpolateRestCursors(const InterpolateClearance& clearance, const StrandSet& strands,
                                                                         float longestGuide, std::string* why, bool meshAsked = false);
// Which points of a strand of `points` points the walk asks of the field: every step-th and the
// tip, at most 16 asked (a strand of up to 17 points: every point); the points between move by the
// moves of the two asked around them, blended by index, or are walked one by one where either of
// the two was lifted (look.slang CLEAR_ASKED).
constexpr std::size_t kClearanceAsked = 16;
inline std::size_t interpolateClearanceStep(std::size_t points) {
    return points > kClearanceAsked + 1 ? (points - 1 + kClearanceAsked - 1) / kClearanceAsked : 1;
}

// ---- Collide ------------------------------------------------------------------------------------

// What a strand's walk knows of a mesh deformed away from its rest pose (CollideOp asks it through
// the rest pose's field, RestPoseCollision): where its walk is, and a point the mesh's own closest
// point query left with how near the mesh can be to it (bound below 0: none yet).
struct CollideRestState {
    RestPoseCollision::Cursor cursor;
    Vec3f anchor;
    float bound = -1.0f;
};
// CollideOp's push of point p out of `mesh` to `to` above it (the offset, or with `soft` the offset
// plus the softness, the push then eased: softPush), within maxDepth below and the search radius:
// through `rest` for a mesh deformed away from its rest pose (its rest field, the mesh itself where
// that cannot tell), else through `field` (a still mesh's: its answers alone, the mesh's where it
// has no grid), else (method exact) the mesh itself. True when it moved p. The CPU engine's Collide
// asks the same (look.slang hostCollidePush; the GPU's reads the field as tables).
bool collideRestPush(const RestPoseCollision* rest, const MeshDistanceField* field, const ScalpMesh& mesh, Vec3f& p, float to, bool soft,
                     float softness, float maxDepth, float searchRadius, CollideRestState& state);
// Its last step: the mesh's own closest point (the bound of the last query skipping what it settles).
bool collideMeshPush(const ScalpMesh& mesh, Vec3f& p, float to, bool soft, float softness, float maxDepth, float searchRadius,
                     CollideRestState& state);

// ---- Part ---------------------------------------------------------------------------------------

// The part lines a Part node with params `part` opens on `strands` (the strands it takes) on
// `scalp`: with partsFrom auto the ones findAutoParts finds there (parts.h), else `painted` (the
// groom's "parts" external). What PartOp opens, for a host drawing a selected Part's parts. Null
// without any.
PartsPtr partLinesFor(const ParamSet& part, const StrandSet& strands, const ScalpMesh& scalp, const PartsPtr& painted);
// The part lines a Part of partsFrom auto found in the strands it took for `output` (its last
// result, or what the graph handed on of it with no strand hidden), into *lines: null when it
// found none (or had no scalp) and handed its strands on as they came. A host opening the parts
// elsewhere (the GPU preview) takes them from here while what Part takes moves. False when `op`
// is not a Part, or its last evaluation was not one of partsFrom auto making `output`.
bool autoPartState(const Operator& op, const StrandsPtr& output, PartsPtr* lines);

// ---- Wind ---------------------------------------------------------------------------------------

// The time Wind made `output` at (its evaluation's: the gusts and the flutter travel with it) and
// the frame rate its frames were counted at (its loop's). False when `op` is not a Wind or
// `output` is not its last result.
bool windTime(const Operator& op, const StrandsPtr& output, double* time, double* framesPerSecond = nullptr);

// Where Wind's gusts and flutter stand at a time (WindOp; the GPU preview's Wind stage works them
// out alike). Unlooped, the gusts' noise field has travelled `shift` (noise units) along the
// wind. Looped (loop on, loopEnd after loopStart, in frames), the time repeats every loopEnd -
// loopStart frames and the field walks a closed path instead: a root's spot q in noise space (its
// position times frequency) reads the four-dimensional noise at windLoopPoint, which has come
// all the way round, to the same gusts, at the loop's end, the field having travelled speed x
// the loop's length along the wind meanwhile (so the gusts repeat that far apart along it).
// The flutter's phase turns a whole number of times a loop.
struct WindPhase {
    Vec3f shift;
    float flutterPhase = 0.0f;  // radians, 0 to 2 pi
    bool looped = false;
    Vec3f across1, across2;  // looped: unit directions across the wind
    float radius = 0.0f;     // looped: the circle's radius in noise units
    float turnsPer = 0.0f;   // looped: radians of the circle per noise unit along the wind
    float angle = 0.0f;      // looped: radians of the circle the time has turned it
};
WindPhase windPhase(const Vec3f& direction, float frequency, double speed, double seconds, double framesPerSecond, bool loop,
                    double loopStart, double loopEnd);
// The looped noise's point for spot q (noise units; `direction` the wind's, unit): q across the
// wind as its first two, the circle (by q along the wind) as its last two (the fourth into *w).
inline Vec3f windLoopPoint(const WindPhase& phase, const Vec3f& direction, const Vec3f& q, float* w) {
    const float a = q.dot(direction) * phase.turnsPer - phase.angle;
    *w = phase.radius * pm::sin(a);
    return {q.dot(phase.across1), q.dot(phase.across2), phase.radius * pm::cos(a)};
}

// ---- SelfCollide and Static: the hair grid --------------------------------------------------------

// What a SelfCollide or Static node lays its hair grid by on the strands it takes (hair_grid.cpp,
// the same as the node's own evaluation lays it), for a host that splats and solves the grid
// elsewhere (the GPU preview): the strands splatted (the core, thinned by ID: the same on every
// evaluation of a set of these strands), each of their segments, the cell and what a sample
// carries; the size of the grid the CPU lays on these very strands (what a host's grid makes room
// for); and the scalp's floor its walk keeps points above (Interpolate keepClearance's field, as
// tables: MeshDistanceField::tables at the floor and the search radius).
struct HairGridLayout {
    std::vector<std::uint32_t> segments;  // 2 words a segment of a splatted strand: its first point's index, its strand
    std::size_t strands = 0;              // how many strands are splatted
    double h = 0.0;                       // the cell the strands are sampled by
    double cell = 0.0;                    // the grid's cell: h, or coarser where hair spreads far wider than the scalp
    double wallReach = 0.0;               // how far a node looks for the scalp, in cells (hairGridWalls)
    std::int64_t margin = 0;              // nodes laid past the samples each way
    int dilate = 0;                       // rings of blocks laid around those holding samples
    double weight = 1.0;                 // the strands of a render each splatted one stands for
    double nominal = 0.0;                 // SelfCollide: a strand's cross-section without widths
    double section = 0.0;                 // Static: a strand's charge cross-section
    bool widths = false;                  // the splatted strands have widths (a Width before the node)
    // The CPU's grid on these strands: its nodes along each axis, its block table's cells along each,
    // the blocks it lays, and whether it took cells coarser than h (hair spread far wider than the scalp).
    std::int64_t nodes[3] = {}, blockCells[3] = {};
    std::size_t blocks = 0;
    bool coarsened = false;
    // The floor (none without a scalp: no tables).
    float floor = 0.0f, depth = 0.0f, searchRadius = 0.0f, beside = 0.0f;
    MeshDistanceField::Tables tables;
    // Over a scalp deformed away from its rest pose: the floor asked through its rest pose (no
    // tables; the CPU engine's alone, its walk asking the host: hairGridFloorPush).
    CollisionSurface floorSurface;
};
// The layout of node type `type` (op::kSelfCollide or op::kStatic) on `in`, the strands it takes,
// over `scalp` (its "scalp" external; null: none). False with *why when it lays none (nothing to
// splat) or its scalp's floor cannot be read as tables (too big for the field's grid). Over a scalp
// deformed away from its rest pose: floorSurface, no tables (the walk asks the rest pose's field
// vertex by vertex: hairGridFloorCursor, hairGridFloorPush).
bool hairGridLayout(const std::string& type, const StrandSet& in, const std::shared_ptr<const ScalpMesh>& scalp, HairGridLayout* out,
                    std::string* why, double scale = 1.0);  // scale: EvalContext::scale, the lattices' anchor
// The scalp near the world's grid nodes (the whole multiples of h): what wallsOf finds for each
// node of a grid of that cell (its closest point within `reachCells` cells: the node's height over
// it and its normal), for every node of the box that can be that near. Words: the box's first node
// (3, signed), its nodes along each axis (3), how many walls, 0; then a word a node of the box (x
// fastest: its wall, 0xffffffff for none near); then 4 words a wall (height, normal xyz, as
// floats). Null when the box would be too large.
// `within` (lo, hi; null: none): only the nodes inside that box (the rest none near).
std::shared_ptr<const std::vector<std::uint32_t>> hairGridWalls(const ScalpMesh& scalp, double h, double reachCells,
                                                                const Vec3f* within = nullptr);
// The walk's floor over a deformed scalp (layout.floorSurface), as the nodes' own walk asks it
// (hair_grid.h ScalpFloor): a strand's cursor from its root, and p pushed above the floor (true
// when it moved).
RestPoseCollision::Cursor hairGridFloorCursor(const HairGridLayout& layout, const StrandRoot& root, const Vec3f& rootPoint);
bool hairGridFloorPush(const HairGridLayout& layout, Vec3f& p, RestPoseCollision::Cursor& cursor);

// ---- BlendNeighbors, Tangle and Flow: what each strand reads beside its own points ----------------

// The neighbours BlendNeighbors blended each strand of `output` toward (its last evaluation), as it
// found them on the strands it took: strand s's at [offsets[s], offsets[s + 1]) of `neighbors`
// (indices of those strands, nearest first) with `weights` (each its share of the strand's sum: what
// the blend multiplies its shape by), whatever the strand's strength (found for every strand of two
// points or more); `normals` per strand the scalp's normal at its root (zero: none), a neighbour's
// shape turned from its root's to the strand's. They go by the roots (on the rest scalp), the core
// strands, the regions and the parts alone. False when `op` is not a BlendNeighbors or kept nothing
// for `output` (its last result, what the graph handed on of it with no strand hidden, or its
// result mixed by its affect: Operator::resultMixed).
struct BlendNeighborsState {
    std::vector<std::uint32_t> offsets;  // strands + 1
    std::vector<std::uint32_t> neighbors;
    std::vector<float> weights;
    std::vector<Vec3f> normals;
};
bool blendNeighborsState(const Operator& op, const StrandsPtr& output, std::shared_ptr<const BlendNeighborsState>* state);

// The partners Tangle picked for the strands of `output` (its last evaluation): per strand the strand
// it sticks to (kNoTanglePartner: none: under its fraction's draw, or no core strand near) and its
// span's draw (the middle of its span lo + (hi - lo) x it, lo and hi by `start` and `span`). They go
// by the roots, the core strands, the clump and region channels, the fraction, the radius and the
// seed. False when `op` is not a Tangle or kept nothing for `output` (as blendNeighborsState).
constexpr std::uint32_t kNoTanglePartner = 0xFFFFFFFFu;
struct TangleState {
    std::vector<std::uint32_t> partners;
    std::vector<float> draws;
};
bool tangleState(const Operator& op, const StrandsPtr& output, std::shared_ptr<const TangleState>* state);

// The flow field Flow laid the strands of `output` along (its last evaluation's "flow" external:
// null without arrows) and the scalp it traced them over (its "scalp" external, by address alone:
// null without one). False when `op` is not a Flow or kept nothing for `output` (as
// blendNeighborsState).
struct FlowState {
    std::shared_ptr<const FlowField> flow;
    const ScalpMesh* scalp = nullptr;
};
bool flowState(const Operator& op, const StrandsPtr& output, FlowState* state);

// ---- Jiggle -------------------------------------------------------------------------------------

// The frame a looping Jiggle (loop from loopStart to loopEnd, frames) reads the scalp at for
// `frame` before `now`: once now is at or past the loop's start, a frame before it is read that
// many frames before the loop's end (wrapped into the range), so a scalp animation that loops
// over the range jiggles seamlessly across its seam. Others as they are; a range of no length:
// no wrap.
double jiggleLoopFrame(double frame, double now, double loopStart, double loopEnd);

// What a Jiggle made `output` from (its last result it jiggled, or the graph's mix of it): the
// scalp and the scalp's history it read (its "scalp" and "scalpHistory" externals) and the frame it
// was evaluated at, at that frame rate. False when `op` is not a Jiggle or `output` is not such a
// result (one it handed its input on for: no history, a scalp that did not move).
struct JiggleState {
    MeshPtr scalp;
    ScalpHistoryPtr history;
    double nowFrame = 0.0, framesPerSecond = 24.0;
};
bool jiggleState(const Operator& op, const StrandsPtr& output, JiggleState* state);
// How a Jiggle with `params` (its frequency, damping, history and loop; a drag's in place of its
// own) hangs strands over `state`'s scalp (JiggleOp's arithmetic, for a host running its springs
// elsewhere): the frames of the history it reads; per vertex of the scalp the lag of a point riding
// it (zero where none moved); per face its turn's drag (sum of weight x (R_k - I), row major) and
// whether it turned. Only the faces `used` flags (and their vertices) are worked out, the rest left
// zero (empty: every face).
struct JiggleLags {
    std::size_t frames = 0;
    std::vector<Vec3f> vertexLag;
    std::vector<std::array<double, 9>> faceTurn;
    std::vector<char> faceTurned;
};
void jiggleLags(const ParamSet& params, const JiggleState& state, const std::vector<char>& used, JiggleLags* out);
// How many frames of `state`'s history a Jiggle with `params` reads.
std::size_t jiggleFramesRead(const ParamSet& params, const JiggleState& state);

}  // namespace groomlab
