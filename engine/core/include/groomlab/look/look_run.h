// SPDX-License-Identifier: Apache-2.0
#pragma once

// A look run: the stages of a graph's last nodes that look.slang's kernels run, their params, mapped
// values and inputs (what LookEngine takes, look_engine.h). The kernels run on the GPU for a drag's
// preview (engine/gpu: which nodes, read from the graph and its cache, look_chain.h) and, compiled to
// C++, on the CPU (makeCpuLookEngine): the core's operators' arithmetic, one source.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/distance_field.h"
#include "groomlab/graph.h"
#include "groomlab/operator_state.h"
#include "groomlab/ramp.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab::gpu {

// A node the GPU runs: one kernel over the strands.
// One a GROOMLAB_STAGE row of look_stages.inc (its node type, name, mapped fields and the params a
// drag may set), in its order.
enum class StageKind {
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) Kind,
#include "groomlab/look/look_stages.inc"
#undef GROOMLAB_STAGE
};
constexpr int kStageKinds = 0
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) +1
#include "groomlab/look/look_stages.inc"
#undef GROOMLAB_STAGE
    ;
const char* stageName(StageKind kind);
constexpr std::size_t kNoStage = static_cast<std::size_t>(-1);
// Cull, Split and Mirror: stages that copy the strands a run of their own hands them into their
// result's layout (a run's first stage alone).
inline bool gathers(StageKind kind) { return kind == StageKind::Cull || kind == StageKind::Split || kind == StageKind::Mirror; }

// `from`: the stage whose result it takes (kNoStage: the base's strands); a Blend's look b `from2`,
// a RestoreLength's reference `from2` where a stage of the chain makes it (kNoStage: the reference
// as cached, which nothing the run moves may make).
// The one before it but for a Blend's looks: look a's stages, then look b's, each from where they
// part (a stage, or the base), then the Blend. A Wet node is a Clump stage a level (`level`: its
// halvings, 0 the largest, run first), back to back: a node's stages are the ones of its ID, its
// first the one a run of it starts from (LookChain::indexOf).
struct LookStage {
    StageKind kind = StageKind::Noise;
    NodeId node = 0;
    std::size_t from = kNoStage, from2 = kNoStage;
    int level = -1;  // a Wet's level (a Clump stage); -1: the node's own stage
    // Up a Merge's input a (a stage before the chain's Merge): it runs on a's strands alone, the
    // first of the base's (StageParams::strands); b's follow them as cached.
    bool prefix = false;
};

// The run of GPU nodes ending the graph. `base` feeds the first of them: its cached strands are
// what the GPU starts from.
struct LookChain {
    NodeId base = 0, output = 0;
    std::size_t baseOutput = 0;  // the base's output the run takes (a Split's b, c or d)
    std::vector<LookStage> stages;  // graph order: the first takes the base's strands
    std::vector<NodeId> passOn;     // nodes that must hand their input (or its points) on as it came (checked on the cache)
    std::size_t indexOf(NodeId node) const;
    NodeId node(StageKind kind) const;  // the first stage of that kind's node (0: none)
};

// One stage's params as its operator reads them. A mapped param's number is the multiplier of its
// values (StageFields): a plugged field's own number (the CPU multiplies the field by it), 1 for a
// map that is the param's own value.
//   Interpolate: value[0] limitDistance; flag[0] samples, [1] preserveLength; ramp: the falloff.
//   Length: value[0] keep (mapped, per strand), [1] fraction (mapped, per strand), [2] trimMin,
//           [3] trimMax (low to high); seed.
//   Clump: value[0] strength (mapped; negative repels), [1] volumize (mapped, per strand), [2]
//          flatness, [3] lockRadius; flag[0] repelRadius's float bits, [1] pastTip extend, [2]
//          flatten (0 surface, 1 random, 2 direction); value2[0] strengthRandom, [1]
//          lockRadiusRandom, [2] copyAmount, [3] strandStrengthRandom; flag2[0-2] flattenDirection's float bits; seed;
//          ramp: the profile; ramp2: volumizeProfile. A Wet's level the same, from its level's
//          Clump params (WetState), its profile wetLevelProfile at the (dragged) wetness.
//   Noise: value[0] amplitude (mapped), [1] frequency, [2] fraction (mapped, at the root); flag[0]
//          octaves, [1] world space, [2] kinky; seed; ramp: the profile.
//   Curl: value[0] radius (mapped; a coil's: coilWidths, its values the strands' widths), [1]
//         frequency, [2] randomness, [3] phase; flag[0] shape (0 helix, 1 wave, 2 zigzag, 3 coil),
//         [1] phaseRandom's float bits, [2] coherence clump; value2[0] spacingRandom, [1] weight
//         over kWeightTurns (0: none); flag2[0] a frequencyProfile other than 1 everywhere, [1-3]
//         the unit weightDirection's float bits; seed; ramp: the profile; ramp2: frequencyProfile.
//   Width: value[0] width (mapped), [1] randomness; seed; ramp: the profile (by arc length).
//   Part: value[0] strength (mapped, per strand), [1] width; ramp: the profile.
//   Gravity: value[0] strength (mapped, per strand), [1] stiffness; value2[0-2] the direction
//            (normalized; zero: none); ramp: stiffnessProfile (by the arc's share, its keys).
//   Flyaway: value[0] stray (mapped, per strand), [1] fraction (mapped, per strand), [2]
//            frequency, [3] start; value2[0] standOut, [1] lengthen; flag[0] 1 (the noise's
//            octaves); seed.
//   Collide: value[0] offset, [1] maxDepth, [2] the search radius; flag[0] iterations.
//   Wind: value[0] strength (mapped, per strand), [1] gust, [2] frequency, [3] turbulence; value2[0-2]
//         the direction (normalized; zero: none), [3] the flutter's phase at the evaluation's time;
//         flag[0-2] the float bits of how far the gusts travelled by then (along the direction, in
//         noise space: WindOp's `shift`); looped (flag[3] 1, operator_state.h WindPhase): flag[0]
//         the circle's angle, [1] its turns per noise unit, flag2[0-2] the first direction across
//         the wind, [3] the circle's radius, as float bits; seed; ramp: the profile.
//   Blend: value[0] mix (mapped); ramp: the profile.
//   SelfCollide: value[0] strength (mapped, per strand), [1] packing (at least 1e-3); flag2[0-1] the
//                limit past which hair is overpacked (ceil(packing 2^32), low and high words); ramp:
//                the profile.
//   Static: value[0] strength (mapped, per strand), [1] randomness (0-1); ramp: the profile.
//   Bend: value[0] angle (mapped, per strand), [1] start, [2] end, [3] randomness; flag[0] along (0
//         normal, 1 tangent, 2 direction); value2[0-2] the direction (as given: a world vector); seed.
//   Direction: value[0] inclination, [1] roll, [2] strength (each mapped, per strand: flag[1-3] which
//              field holds its values, 0 or 1, 2 none: the first two mapped of the three, a third the
//              CPU's), [3] inclinationRandom; value2[0-2] direction, [3] rollRandom; flag[0] along
//              tangent (by the scalp's UVs); seed.
//   Transform: value[0] scale (mapped, per strand), [1] scaleRandom; flag[0] world (about the pivot),
//              [1] nothing to do (hands its input on), [2] translateRandom not zero, [3] rotateRandom not
//              zero; value2[0-2] translate; more[0-2] rotate, [4-6] pivot, [8-10] translateRandom,
//              [12-14] rotateRandom; seed.
//   Displace: value[0] distance (mapped, per strand or per point); flag[0] along the normal; value2[0-2]
//             the direction (unit; zero: none); ramp: the profile.
//   Twist: value[0] turns (mapped, per strand); ramp: the profile (around axes or its clumps' middles:
//          TwistInputs).
//   Jiggle: value[0] strength (mapped, per strand), [1] frequency, [2] damping (and as doubles' bits in
//           more[0-1] and [2-3]); flag[0] history (the frames it reads, the lags made of them:
//           JiggleInputs); ramp: the profile (RampAlong).
//   MeshCut: value[0] offset; flag[0] 1 keep inside (the mesh a hull), 0 outside (an obstacle).
//   Merge: flag[0] the strands of its input a (the base's first; b's after them, handed on as cached,
//          or as b's own run makes them: branch).
//   Cull, Split, Mirror: the run of the nodes before it (branch; null: its result as cached);
//          Mirror: flag[0] the axis (0-2), value[0] the plane's offset.
// The shapers reading their neighbours or a reference:
//   Smooth: value[0] strength (mapped, per strand), [1] scale (0-1); flag[0] its cells (smoothCells);
//           ramp: the profile.
//   RestoreLength: flag[0] mode (0 segments, 1 scale, 2 even), [1] 1 when a stage of the run makes
//                  its reference (LookStage::from2).
//   BlendNeighbors: value[0] strength (mapped, per strand); flag[0] preserveLength; ramp: the
//                   profile.
//   Tangle: value[0] strength (mapped, per strand), [1] start, [2] span, [3] reach.
//   Flow: value[0] lift (degrees; mapped, per strand), [1] bend, [2] rollRandom (radians); seed.
// Any stage restricted by its affect: masked, affect, mixWidths below (one way for all).
struct LookBranch;
struct StageParams {
    LookStage stage;
    float value[4] = {};
    int flag[4] = {};
    float value2[4] = {};
    int flag2[4] = {};
    float more[16] = {};  // more numbers (Transform's vectors), in the constants' octave seeds' rows
    // The strands it runs on: the base's first strands (a stage up a Merge's input a: LookStage::
    // prefix; its inputs and mapped values are a's), 0 every one.
    std::size_t strands = 0;
    std::uint64_t seed = 0;
    Ramp ramp;
    bool tabled = false;  // the ramp by point index (RampAlong; Width's and Interpolate's are not)
    Ramp ramp2;           // a second profile (Clump's volumizeProfile), by point index
    bool hasRamp2 = false;
    // Affect masks. Masked (its affect a map, a field plugged
    // in, a number between 0 and 1, or dragged): the node's result mixed back toward what its first
    // stage took by its affect (StageFields::affect at that stage, times `affect`: the mapped
    // multiplier, or the number), as the graph mixes it (applyAffect), after the node's last stage
    // (a Wet's smallest level); `mixWidths`: its widths too (a Width, on strands that carry widths).
    bool masked = false;
    float affect = 1.0f;
    bool mixWidths = false;
    // A Merge whose input b the drag moves: b's own run (null: b's strands as the Merge last made them).
    // A Cull, Split or Mirror: the run up its input (null: its result as cached).
    std::shared_ptr<const LookBranch> branch;
};
struct LookParams {
    std::vector<StageParams> stages;  // as the chain's
    const StageParams* find(StageKind kind) const;
    StageParams* find(StageKind kind);
};
// A Merge's input b moved by a drag (a node making b's strands dragged): b's own chain (findLookChainTo
// the node feeding b, from the first of its stages the drag sets) and its params with the drag's values.
// Its strands, mapped values and inputs are read with the run's (LookInputs::read: StageInputs::merged).
struct LookBranch {
    LookChain chain;
    LookParams params;
};
// Whether a run of `params` makes widths: a Width runs, or one of a Merge's input b's run.
bool runsWidth(const LookParams& params);

// A field the GPU evaluates per strand from the points a run hands the node (an expression of the
// strands' shapes or `longest`, a channel the run's Interpolate writes: its guides' influence): the
// expression's program (expression.h, reverse Polish) as the kernels read it, 4 words an op (kind,
// code, value's bits, slot), with what does not move per strand (`slotCount` values a strand: the roots'
// variables, rand, index, channels nothing in the run writes), then remapped as the field remaps
// it (min + (max - min) v).
struct FieldProgram {
    std::vector<std::uint32_t> ops;
    std::vector<float> statics;  // strands x slots
    std::uint32_t slotCount = 0;  // (not `slots`: a Qt macro in the app)
    std::uint32_t depth = 0;     // the stack's depth
    float low = 0.0f, high = 1.0f;  // the remap: low + (high - low) v
    bool shape = false;          // reads the strand's shape (length, reach, straightness, lift, tip)
    bool influence = false;      // reads the guides' influence the run's Interpolate makes
    // `longest`: the longest of the core strands (preview_core.h), these.
    std::vector<std::uint32_t> core;
    bool longest = false;
};
// A mapped param's values as its operator's ScalarField reads them (field.h), on the strands that
// operator takes, without the multiplier (StageParams). kNumber: the param is a plain number.
struct FieldValues {
    enum Mode { kNumber = 0, kPerStrand = 1, kPerPoint = 2 };
    Mode mode = kNumber;
    bool readsPositions = false;  // read from the points: they hold while nothing before the node moves them
    // Holds whatever the run moves: read at the roots from the scalp, IDs and channels nothing in
    // the run writes (a texture, a color set, an expression of those).
    bool fixed = false;
    // Reads channels the run's Interpolate writes from its binding (guide attributes, regions):
    // they hold while the binding does (not bound again for moved guide roots).
    bool binding = false;
    // The strands' widths (a coil's radius): they hold while no Width before the node runs.
    bool widths = false;
    std::vector<float> values;
    // Evaluated on the GPU per strand (`values` the CPU's, as read): it holds whatever moves.
    std::shared_ptr<const FieldProgram> program;
};
// Each stage's mapped params (StageParams says which are mapped: [0] and, Noise's fraction,
// Clump's volumize and Flyaway's fraction, [1]); a masked node's affect at its first stage
// (StageParams::masked; mode kNumber where it is a number), read on the strands that stage takes.
struct StageFields {
    FieldValues field[2];
    FieldValues affect;
};
struct LookFields {
    std::vector<StageFields> stages;  // as the chain's
    // The strands they were read from (the base and the stages' results, held): the same pointers
    // in the document's cache mean the same values (sameLookFields).
    std::vector<StrandsPtr> sources;
    std::uint64_t serial = 0;  // new for every read: an engine uploads the values it does not hold
    bool any() const;
};

// What Interpolate and Clump take beyond their params, read from what the CPU's operators keep
// (operator_state.h) and the document's cache. Each part is shared and immutable: an engine
// uploads what it does not hold (by pointer).
// The guides a strand blends at most on the GPU (its records' slots); more: the CPU's.
constexpr std::size_t kInterpolateSlots = 16;
// The guide samples Interpolate's kernel takes on the CPU engine at most (look.slang CPU_MAX_SAMPLES;
// the GPU's: 64).
constexpr std::size_t kInterpolateCpuSamples = 512;
struct LookCentersRun;
struct InterpolateInputs {
    // The binding (Interpolate's, or bound again for guide roots a stroke moved): its chunks say
    // which strands' records changed (interpolateChunks).
    std::shared_ptr<const InterpolateState> binding;
    std::shared_ptr<const InterpolateGuideRows> rows;       // the guides' shapes (new for new guides)
    std::shared_ptr<const std::vector<Vec3f>> strandNormals;  // per strand: the scalp's normal at its root (zero: none)
    // keepClearance: the scalp's field as tables and the walk's distances (new for a new scalp);
    // null when the node does not keep clearance.
    std::shared_ptr<const InterpolateClearance> clearance;
    // Keeping clearance over a deformed scalp (clearance->walk): per strand the vertex its walk
    // starts at (0xffffffff: none, the nearest found from every vertex).
    std::shared_ptr<const std::vector<std::uint32_t>> cursors;
    // fillTubes (the CPU engine's): the tubes' samples and count, each tube (its root join, then
    // each sample's center, U and V axes: 1 + 9 x samples words), then 8 words a strand (its tube,
    // -1 none; its section u, v; the deeper tube whose soft edge it lies in, -1 none; its
    // coordinates there; how far it leans; 0): look.slang tubeOf. Null: none.
    std::shared_ptr<const std::vector<std::uint32_t>> tubes;
};
struct ClumpInputs {
    std::shared_ptr<const std::vector<std::uint32_t>> assigned;  // per strand: its center (kNoClumpCenter: none)
    std::size_t centers = 0;
    // The centers' curves (kClumpCenterSamples points each) as the CPU made them, when the run
    // leaves them as they are; else null and `chosen` (per center: the strand of the stage's input
    // its curve is, the Clump's own choice or, with centersFrom inputPoints, its axis strand) has the GPU
    // make them from the strands it moved.
    std::shared_ptr<const std::vector<Vec3f>> curves;
    std::shared_ptr<const std::vector<std::uint32_t>> chosen;
    // centersFrom inputCurves, the centers made by a branch the run moves (a Scatter's strands
    // through an Interpolate of the stroke's guides, a Noise): that branch's own run, made on the
    // GPU before the Clump; `chosen` then names each center's strand of its result.
    std::shared_ptr<const LookCentersRun> centersRun;
    // Per center: the scalp's normal at its root (zero: none), what its frames start from
    // (ClumpState::centerNormals, look.slang frameSeed).
    std::shared_ptr<const std::vector<Vec3f>> normals;
    // Per center 4 floats (its per-clump variation, drawn from its ID and the seed as ClumpOp draws
    // it): the strength's draw, the lock radius's, a random flatten's turn (a share of one), 0.
    std::shared_ptr<const std::vector<float>> variation;
    // copyAmount, centers made on the GPU: per center the nearest centers it takes the shape of, 9
    // words (the count, then up to 8; a center without a shape neither gives nor takes).
    std::shared_ptr<const std::vector<std::uint32_t>> neighbors;
    // pastTip extend: the longest strand it took (the CPU's last run), how far past an axis's tip a
    // point can go (lookBounds).
    float reach = 0.0f;
};
// Curl coherent by clump: the clumps as the CPU finds them on the strands it takes (by the clump
// channel; each one's middle from its core strands), fixed while the run goes.
struct CurlInputs {
    std::shared_ptr<const std::vector<std::uint32_t>> clumpOf;  // per strand: its clump (0xffffffff: curls alone)
    // Per clump its core strands (in strand order): `count + 1` offsets, then the strands.
    std::shared_ptr<const std::vector<std::uint32_t>> members;
    // Per clump 8 floats: its members' root normals summed, normalized (what its frames start
    // from), 0, then its draw's four numbers (CurlOp::drawFor, from its key and the seed).
    std::shared_ptr<const std::vector<float>> clumps;
    std::size_t count = 0;
};
// Part: each strand's side of the parts (from its root on the rest scalp: fixed; an auto Part's
// lines as its last evaluation found them), 4 floats a strand: the way away from its part xyz, the
// distance to it (-1: no part, or not on the scalp).
struct PartInputs {
    std::shared_ptr<const std::vector<float>> sides;
    float reach = 0.0f;  // the longest strand it took (the CPU's last run): how far a turned point goes
};
// Collide: the surfaces' distance fields as tables (MeshDistanceField::tables, the fields the CPU
// asks at its offset, made for every offset those fields serve: a dragged offset is the kernel's
// number; read as Interpolate's clearance walk reads them: the field's answers alone, never the
// mesh's tree), the scalp's then the collider's, and their boxes (lookBounds). Fixed per field.
// A mesh deformed away from its rest pose (`rests`, a bit each; the CPU engine's alone: its tables
// a placeholder) is asked through the host (CollideHost, look.slang hostCollidePush): the
// operator's own push, collideRestPush, on its rest pose's walk.
struct CollideHost {
    std::vector<std::shared_ptr<const RestPoseCollision>> rests;  // per mesh (null: a still one)
    std::vector<MeshPtr> meshes;
    float maxDepth = 0.0f, searchRadius = 0.0f, softness = 0.0f;
    std::shared_ptr<const std::vector<StrandRoot>> roots;  // per strand of the run (its walk starts at its root)
};
struct CollideInputs {
    std::uint32_t rests = 0;
    std::shared_ptr<const CollideHost> host;  // (with rests)
    std::vector<MeshDistanceField::Tables> tables;
    std::vector<Vec3f> boxes;  // per mesh: lo, hi
    std::uint32_t colliders = 0;  // which are the collider input's, a bit each (softness and smoothing act on it)
};
// SelfCollide and Static: the hair grid as the node lays it on the strands it takes
// (operator_state.h HairGridLayout: the strands splatted, their segments, the cell, what a sample
// carries, the floor's field as tables), read on its cached input (the strands splatted are the
// core by ID, the cell by the scalp: the same for the strands a run hands it), and the scalp near
// the world's grid nodes (hairGridWalls), the same while the scalp and the cell are.
struct HairGridInputs {
    std::shared_ptr<const HairGridLayout> layout;
    std::shared_ptr<const std::vector<std::uint32_t>> walls;  // null: no scalp
};
// Bend, Direction, Transform and Displace: each strand's root on the scalp (the groom's, as the
// operators take it), read on the strands the node took (fixed while the scalp and the roots are),
// 8 floats a strand: the scalp's normal there xyz, 1 when rooted on the scalp (0: not, its first
// segment the normal); the scalp's U direction across the root's face from its UVs xyz (zero: none,
// transform_ops.cpp uvTangent), 0.
struct RootInputs {
    std::shared_ptr<const std::vector<float>> frames;
};
// Twist: the axes each strand turns around (TwistOp), kTwistAxisSamples points each. Around the
// curves of its axes input (guides the run leaves as they are): the axes as the CPU makes them, then
// each one's arc length (`axes`), each strand's the one nearest its root as the CPU found it on the
// strands the node took. Else around each clump's middle (by clumpChannel): the clumps' core
// strands (`members`: count + 1 offsets, then the strands), the middles made on the GPU from the
// strands it moves. Per strand 4 words (`strands`): its axis or clump (0xffffffff: it does not turn),
// then the root's bits its axis was found for (around axes: a root the run moves finds its own).
constexpr std::size_t kTwistAxisSamples = 32;  // extra_ops.cpp kAxisSamples
struct TwistInputs {
    std::shared_ptr<const std::vector<std::uint32_t>> strands;
    std::shared_ptr<const std::vector<float>> axes;               // around axes (else null)
    std::shared_ptr<const std::vector<std::uint32_t>> members;    // by clump (else null)
    std::size_t count = 0;                                        // axes or clumps
};
// Jiggle: how each strand hangs from its root's face (operator_state.h jiggleLags at the params'
// frequency, damping and history, over the scalp and its history Jiggle's last evaluation read:
// jiggleState), fixed while the roots are: per strand 4 floats, its root's lag (its face's
// vertices' lags blended as JiggleOp blends them) xyz and its face (a word's bits; 0xffffffff: it
// stays, rooted off the scalp or of fewer than 2 points); per face 12 floats, its turn's drag by rows
// (xyz; the first row's w 1 where it turned). `faceTurns`: the same turns in double, 9 a face (the
// CPU engine's: JiggleOp drags a strand by them).
struct JiggleInputs {
    std::shared_ptr<const std::vector<float>> strands, faces;
    std::shared_ptr<const std::vector<double>> faceTurns;
};
// MeshCut: its mesh as ScalpMesh::rayTree lays it out (its points, normals, triangles and the tree
// intersectRay and closestPoint search), each strand cut against it alone. Null: no mesh (the
// strands pass through).
struct MeshCutInputs {
    std::shared_ptr<const std::vector<std::uint32_t>> tree;
};
// Cull, Split, Mirror: their result's layout as their last evaluation laid it out, from what it took
// (its input as made, laid out as the strands its branch hands it): per strand of the result 8 words,
// the first point of the strand it copies there, its flags (1: mirrored across the plane where
// coordinate `axis` is `offset`; 2: then moved by `onto`, onto the scalp as MirrorOp moved it), 0, 0,
// onto's float bits xyz, 0.
struct GatherInputs {
    std::shared_ptr<const std::vector<std::uint32_t>> strands;
    int axis = 0;
    float offset = 0.0f;
    float reach = 0.0f;  // the longest onto (lookBounds)
};
// RestoreLength (segments, scale) with a reference the run does not make (LookStage::from2
// kNoStage): per point the reference's segment lengths at the strand's point count, resampled as the
// node resamples it (at the strand's first point: the reference's whole length, -1 for none: the
// strand left as it came), and per strand 4 floats: the reference's first segment xyz (a collapsed
// first segment carries on along it), 0.
struct RestoreInputs {
    std::shared_ptr<const std::vector<float>> lengths, firsts;
};
// Tangle: per strand 4 words from its partner choice (operator_state.h TangleState): the partner
// (kNoTanglePartner: none), the float bits of the draw its span's middle is made from (the lower
// ID's of a pair that picked each other), the float bits of the share it moves (0.5 a pair's, else
// 1), 0.
struct TangleInputs {
    std::shared_ptr<const std::vector<std::uint32_t>> strands;
};
// Flow: the field its last evaluation laid the strands along and the scalp it traced them over
// (operator_state.h FlowState). `strands`: 12 floats a strand: its root's face (bits; 0xffffffff:
// not on the scalp), u, v, 0; the scalp's normal there xyz, 0; the field there (atRoot) xyz, 0.
// `field`: the field baked at the scalp's vertices (4 floats each, `vertices` of them: none where
// the field has none), then each pin (position xyz, 0, direction xyz, 0), then each sink (center
// xyz, radius, normal xyz, angle in radians). `tables`: the scalp's (its closest points found down
// its mesh); null without a scalp. `host`: the field and the scalp
// themselves, the CPU engine's alone (look.slang FlowCS asks the core's queries there: their ties
// the operator's), no tables needed.
struct FlowHost {
    std::shared_ptr<const FlowField> flow;  // null: no arrows
    const ScalpMesh* scalp = nullptr;
};
struct FlowInputs {
    bool arrows = false;
    std::shared_ptr<const std::vector<float>> strands, field;
    std::uint32_t vertices = 0, pins = 0, sinks = 0;
    MeshDistanceField::Tables tables;
    FlowHost host;
};
struct StageInputs {
    std::shared_ptr<const InterpolateInputs> interpolate;
    std::shared_ptr<const ClumpInputs> clump;
    std::shared_ptr<const CurlInputs> curl;
    std::shared_ptr<const PartInputs> part;
    std::shared_ptr<const CollideInputs> collide;
    std::shared_ptr<const HairGridInputs> grid;
    std::shared_ptr<const RootInputs> roots;   // Bend, Direction, Transform, Displace
    std::shared_ptr<const TwistInputs> twist;
    std::shared_ptr<const RestoreInputs> restore;
    std::shared_ptr<const BlendNeighborsState> neighbors;  // BlendNeighbors: its neighbours (operator_state.h)
    std::shared_ptr<const TangleInputs> tangle;
    std::shared_ptr<const FlowInputs> flow;
    std::shared_ptr<const JiggleInputs> jiggle;
    std::shared_ptr<const MeshCutInputs> meshCut;
    std::shared_ptr<const GatherInputs> gather;  // Cull, Split, Mirror
    // Merge: its input b's own run (StageParams::branch), made on a sibling engine before the Merge.
    // Cull, Split, Mirror: the run up its input, so.
    std::shared_ptr<const LookCentersRun> merged;
    // Gravity, Flyaway, Collide, Wind, SelfCollide, Static, Bend, Direction, Transform, Jiggle and
    // the shapers reading their neighbours or a reference: the longest strand the node took (the CPU's
    // last run; a RestoreLength's reference's too), how far from its root a point it moves can go
    // (lookBounds).
    float reach = 0.0f;
};
// A Clump's centers made by a branch of their own that the run moves (ClumpInputs::centersRun): the
// branch's chain (findLookChainTo the node feeding the centers, from its first node the run
// changes: Interpolate, for a stroke's guides), the strands it starts from (cached: nothing the run
// changes makes them) and its run's params, mapped values and inputs, as a chain's. A Merge's input b
// moved by a drag runs as one too (LookBranch, StageInputs::merged).
struct LookCentersRun {
    LookChain chain;
    StrandsPtr base;
    const ScalpMesh* scalp = nullptr;  // where its strands' roots are (their root normals; null: none)
    LookParams params;
    LookFields fields;
    std::vector<StageInputs> inputs;
};

}  // namespace groomlab::gpu
