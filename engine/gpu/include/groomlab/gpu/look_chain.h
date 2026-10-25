// SPDX-License-Identifier: Apache-2.0
#pragma once

// GPU look preview: which part of a graph the GPU can run, and with what.
//
// While an artist drags a param of a node near the end of the graph, or brushes guides, a viewport
// can show the result without a CPU evaluation of what follows: the strands as they enter the
// first node that changes (the last evaluation's cached result) are uploaded once, and each new
// value (or brush dab) runs that node and every one after it, then the lit lines' frames, on the
// GPU (look_engine.h), written into the viewport's own vertex buffers. The CPU operators stay the
// reference: commits, renders, exports and tests evaluate on the CPU, and the release of a drag
// or a stroke evaluates there and replaces the preview.
//
// The nodes the GPU runs (stages): Interpolate (from the binding and guide shapes the CPU's
// Interpolate keeps, operator_state.h; the guides it takes as the CPU last made them, a brush
// dab's made anew on the CPU up to it, guides alone being few: the hand guides, or a Sculpt
// node's output, a sculpt stroke's edits or a dragged strength laid on its cached input,
// sculptedGuides; keeping its strands clear of a scalp, from the scalp's
// distance field as tables, walked as the CPU walks it; over a scalp
// deformed away from its rest pose, through the rest pose's field and each vertex's frame, the
// strand's nearest vertex walked over the mesh's edges as RestPoseCollision walks it), Length, Clump (the CPU's center choice; centers made by a branch of their own the run
// moves, centersFrom inputCurves through another Interpolate, made by that branch's own run first:
// LookCentersRun; centers made from strands the GPU moved are
// resampled, shared (copyAmount) and given frames on the GPU; its per-clump variation, volumize and
// flatness), Wet (its levels as Clump stages back to back, the largest first: each level's params,
// profile and center choice the CPU's level's, operator_state.h WetState, its profile made again
// for a dragged wetness; a smaller level's centers resampled on the GPU from the strands the level
// before it moved; its wetness channel, unread by the drawing, as the CPU last wrote it; at a
// wetness of 0 handed on as it came), Noise, Curl (curling each clump as one too: the clumps' middles made on the GPU from
// their core strands; a coil by the strands' widths; not one that added points to its strands,
// too few a turn: a run keeps the layout), Width, Part (the strands' sides of the parts
// from the CPU: the groom's painted ones, or partsFrom auto, an auto Part's lines as its last
// evaluation found them, operator_state.h autoPartState: found in the strands it takes, which a
// drag or a dab upstream moves, they stay as they were until the release finds them again; its
// splitAngle and minLength, which pick the lines, the CPU's), Gravity, Flyaway and Collide (out of the scalp and a collider through their
// distance fields as tables, as Interpolate's clearance walk asks them), Wind (at the time its
// last evaluation was made at: windTime), Blend (its looks a and b each a run of these from
// where they part, a node both come from: no Width in either, their widths the same; its look
// channels and mix channel as the CPU last wrote them), SelfCollide and Static (the hair grid in
// compute, look.slang: the core strands the node splats and its cell as it lays them on the
// strands it took, operator_state.h HairGridLayout; the strands' Catmull-Rom samples splatted with
// 64-bit fixed-point integer atomics into blocks laid on the GPU where the samples are, as the CPU
// lays its sparse grid, within room for half as wide again as the CPU's last grid, its blocks twice
// over (wider: its far side unsplatted); the scalp near the grid's nodes from the CPU,
// hairGridWalls; the red-black SOR solve a dispatch a colour; SelfCollide's slab spreads; Static's
// cantilever bend; the walk keeping the strands off the scalp through the floor's field as tables;
// not over a scalp deformed away from its rest pose, nor without a scalp where the run moves the
// strands it takes), Tie (three kernels, look.slang: its frame and the ring's 64 bins of the
// largest radial measure, from the core strands the run hands it, in one group; each strand's
// ring point and route, the scalp's closest points under its arc found down the mesh of the
// scalp's field's tables from the face its start was nearest on the CPU, their outer hull; then
// its path pushed out of the scalp through the field's tables and the strand laid along it at
// its segments' lengths; its affect per strand, read as a mapped param, mixing it as the graph
// does; its tail channel written for a Tie after it in the run, a stage reading it the CPU's;
// not over a scalp deformed away from its rest pose), the shapers working on each strand alone: Bend, Direction, Transform and
// Displace (each strand's root frame from the scalp's normal and UVs at its root, RootInputs; a
// direction one world vector, not one per strand; Direction two of its three params mapped at most)
// and Twist (around its axes input's curves as cached, each strand's nearest found again on the GPU
// for a root the run moves; or around each clump's middle, made on the GPU from its core strands:
// TwistInputs), and the shapers reading their neighbours or a reference: Smooth (its three box filters over each strand's reflected increments, a
// group a strand), RestoreLength (its reference as cached, resampled to each strand's count on
// the CPU, or the result of a stage of the run that makes it: LookStage::from2; a reference made
// from what the run moves through other nodes: the CPU's), BlendNeighbors (each strand's
// neighbours and their shares as its last evaluation found them, operator_state.h
// BlendNeighborsState: by the roots, the core, the regions and the parts; their shapes the run's),
// Tangle (each strand's partner and its span's draw as its last evaluation picked them, TangleState;
// a fraction reading what the run moves, or clumps or regions the run writes anew: the CPU's) and
// Flow (the field and scalp its last evaluation traced along, FlowState: the field baked at the
// vertices, its pins and sinks asked where it asks the field itself; each step's closest point down
// the scalp's mesh as Tie's routes find them; not over a scalp deformed away from its rest pose), in
// any order. Cull, Split (its output a) and Mirror, which keep or add strands: a run through one
// starts with it (its result the base, its node the chain's base too), the nodes before it a run of
// their own up its input (LookBranch, made on a sibling engine first, as a Merge's input b's), whose
// strands it copies into its result's layout as its last evaluation laid it out (GatherInputs: each
// strand from the one it kept, or mirrored across the plane and moved onto the scalp as the CPU
// moved it); its own params, which pick the strands, the CPU's. A Merge without a
// seam to blend: the run goes on up its input a, its stages on a's strands alone (the first of the
// base's: the Merge's cached result with a's points as the first stage up there takes them), b's
// strands handed on as the Merge last made them; one Merge a run. A drag up its input b (a node
// making b's strands): b's own run, a chain of its own up b from the first node the drag sets
// (LookBranch), made on a sibling engine before the Merge, which takes b's points (and widths) from
// it; a's strands as cached when the drag is on b's side alone.
// Jiggle with motion to lag behind (its springs over the scalp and history its last evaluation read,
// operator_state.h jiggleState: each strand's root lag and face's turn made on the CPU as JiggleOp
// makes them, again for a dragged frequency, damping or history; the strands turned about their
// roots on the GPU; a history past the frames sampled for it the CPU's), and MeshCut (its point
// counts kept: each segment a ray down its mesh's own tree, ScalpMesh::rayTree, the root's side by
// its closest point, the kept part resampled along the strand). Any other stage restricted by its
// affect (a mask: a map, a plugged field, a number between 0 and 1, or a dragged affect) runs too,
// its result mixed back toward what it took after it (look.slang AffectCS, as applyAffect mixes it:
// per strand or per point, a Width's widths, a Wet after its last level), the affect read as the
// mapped params are, or where a Tie of the run writes its tail (Tie mixes by its own: its affect
// picks the strands it ties). (A Curl adding points still ends the run.) A graph
// takes the GPU from the first of them after its last node the GPU cannot run (or does not run
// with these params) to the output, through nodes that hand their input on as it came (Output, a
// bypass, a Switch (the input it picks), an even Resample with nothing to do, a Jiggle with no
// motion to lag behind, a MeshCut with no mesh, a node of no affect) or its points
// (HairColor, Attribute, RenderData: channels alone; Cards: its cards, made again by the commit).
// Anything else ends the run: the CPU's. A preview holding hidden core strands (preview_core.h)
// runs on the GPU too: every strand runs, the shown ones are written (LookEngine).
//
// This module (groomlab_gpu) has no host in it: a host hands it a graph document evaluated by the
// core and its device (Maya: Viewport 2.0's; the tests: one of their own).

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/distance_field.h"
#include "groomlab/graph_io.h"
#include "groomlab/operator_state.h"
#include "groomlab/ramp.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {
struct SculptEdits;
}

namespace groomlab::gpu {

// A node the GPU runs: one kernel over the strands.
// One a GROOMLAB_STAGE row of look_stages.inc (its node type, name, mapped fields and the params a
// drag may set), in its order.
enum class StageKind {
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) Kind,
#include "groomlab/gpu/look_stages.inc"
#undef GROOMLAB_STAGE
};
constexpr int kStageKinds = 0
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) +1
#include "groomlab/gpu/look_stages.inc"
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
    std::vector<LookStage> stages;  // graph order: the first takes the base's strands
    std::vector<NodeId> passOn;     // nodes that must hand their input (or its points) on as it came (checked on the cache)
    std::size_t indexOf(NodeId node) const;
    NodeId node(StageKind kind) const;  // the first stage of that kind's node (0: none)
};
// The longest run that ends the graph; false with *why when it ends with a node the GPU does not
// run.
bool findLookChain(const GraphDocument& doc, LookChain* chain, std::string* why);
// The same ending at node `end` (its `output`) instead of the graph's output: a branch feeding
// another node, as a Clump's centers.
bool findLookChainTo(const GraphDocument& doc, NodeId end, LookChain* chain, std::string* why);
// The stages from `first` on (those before it keep their cached results: the new base is the
// node feeding stage `first`). Within a Blend's looks, from the first stage of look a: the run
// makes both from where they part.
LookChain chainFrom(const GraphDocument& doc, const LookChain& chain, std::size_t first);

// A drag's values: node name -> param -> value, over the nodes' own. A number (a slider's step, a
// seed), a ramp (a profile's curve edited live: its keys as the graph would load them,
// rampFromJson), or a vector (Curl's weightDirection; Transform's translate, rotate, pivot and their
// randoms; Bend's, Direction's and Displace's direction).
using OverrideValue = std::variant<double, Ramp, Vec3f>;
using Overrides = std::map<std::string, std::map<std::string, OverrideValue>>;
// The first stage whose node takes a value of `overrides`, or a Merge whose input b a node of them
// makes (b's own run: StageParams::branch) (kNoStage: none).
std::size_t firstOverridden(const GraphDocument& doc, const LookChain& chain, const Overrides& overrides);

// One stage's params as its operator reads them. A mapped param's number is the multiplier of its
// values (StageFields): a plugged field's own number (the CPU multiplies the field by it), 1 for a
// map that is the param's own value.
//   Interpolate: value[0] limitDistance; flag[0] samples, [1] preserveLength; ramp: the falloff.
//   Length: value[0] keep (mapped, per strand), [1] fraction (mapped, per strand), [2] trimMin,
//           [3] trimMax (low to high); seed.
//   Clump: value[0] strength (mapped; negative repels), [1] volumize (mapped, per strand), [2]
//          flatness, [3] lockRadius; flag[0] repelRadius's float bits, [1] pastTip extend, [2]
//          flatten (0 surface, 1 random, 2 direction); value2[0] strengthRandom, [1]
//          lockRadiusRandom, [2] copyAmount; flag2[0-2] flattenDirection's float bits; seed;
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
//   Tie: value[0] radius, [1] tightness (0-1), [2] spread (radians, 0-80 degrees), [3] twist;
//        value2[0] ease, [1-3] direction; flag[0] 1 when it writes a tail channel, [1] its affect's
//        number's float bits (the mapped affect's multiplier; 1 unmasked), [2] 1 when masked (its
//        affect a mask: the result mixed by it per strand, field 0); flag2[0-2] center's float bits.
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
// Any stage but Tie restricted by its affect: masked, affect, mixWidths below (one way for all).
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
    // Affect masks (not Tie's: its affect picks the strands it ties, flag[1-2]). Masked (its affect a map, a field plugged
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
// A Cull's, Split's or Mirror's run up its input (StageParams::branch, its stage a run's first): the
// chain ending at the node feeding it (findLookChainTo), and its branch: that chain from stage
// `first` (chainFrom), its params read with `overrides`. False with *why.
bool gatherInputChain(const GraphDocument& doc, NodeId gather, LookChain* chain, std::string* why);
bool gatherBranch(const GraphDocument& doc, const LookChain& inputChain, std::size_t first, const Overrides& overrides,
                  std::shared_ptr<const LookBranch>* out, std::string* why);
// The chain's params (the nodes' own, with `overrides` on top; a plugged curve is the ramp the
// node's last evaluation took). False with *why for anything the kernels do not do.
bool readLookParams(const GraphDocument& doc, const LookChain& chain, const Overrides& overrides, LookParams* params,
                    std::string* why);
// Whether `param` of the node of type `type` is one a drag may preview on the GPU: the numbers
// the kernels read, the seeds (the strands' draws are made again for each: look_engine.h), and the
// profiles (Interpolate's falloff), each a ramp. (A Clump's seed picks its own centers when it
// has no centers input: readLookParams refuses that one. Params that pick other strands or change
// their counts, Scatter's, Resample's, Interpolate's binding, are the CPU's: Wet's wetness alone,
// its clumpSize, levels, randomness and seed picking its levels' centers; Part's strength and width
// alone, an auto Part's splitAngle and minLength picking its lines; BlendNeighbors' neighbors,
// Tangle's fraction, radius and seed picking whom a strand reads.) A Sculpt node's strength: the
// guides it hands on made on the CPU (sculptedGuides), the strands after them on the GPU.
bool previewableParam(const std::string& type, const std::string& param);
// Whether a drag of these values can be previewed: each of a previewable param (a number for a
// number, a ramp for a ramp), on a node that is a stage (or not in the chain). False with *why.
bool previewableOverrides(const GraphDocument& doc, const Overrides& overrides, std::string* why);

// A drag of a Sculpt node's strength (sculpt.h; previewableParam): the guides the node hands on at
// `strength`, made on the CPU as the operator makes them, its edits (`edits`, the node's stored
// ones; null: none) laid on the guides it is fed (its cached input, unchanged by the drag); the GPU
// runs from the first stage they make (an Interpolate taking them: LookInputs::read's `standIn`).
// False with *why: its input not cached (not evaluated yet), its strength plugged.
bool sculptedGuides(const GraphDocument& doc, NodeId sculpt, const SculptEdits* edits, double strength, GuidesPtr* out,
                    std::string* why);
// Whether any of `nodes` makes node `id`'s result (is it, or is upstream of it).
bool madeFrom(const Graph& graph, NodeId id, const std::set<NodeId>& nodes);
// The first stage of `chain` whose result a node of `nodes` makes (kNoStage: none).
std::size_t firstMadeFrom(const GraphDocument& doc, const LookChain& chain, const std::set<NodeId>& nodes);

// The cached strands entering the chain (as the nodes made them: a preview's hidden core strands in
// them, preview_core.h) and those at its output as the graph handed them on (the shown ones, as
// the viewport's buffers hold them); false with *why when the cache does not hold them (not
// evaluated yet, a Resample that did change the strands, another layout).
bool lookStrands(const GraphDocument& doc, const LookChain& chain, StrandsPtr* base, StrandsPtr* output, std::string* why);

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
    // A point channel a Tie of the run writes (its tail; an affect mapped to it alone): that stage's
    // index, the values read on the GPU where it wrote them, remapped low + (high - low) v as the
    // map remaps them (`values` the CPU's, as read). kNoStage: none.
    std::size_t channelFrom = kNoStage;
    float low = 0.0f, high = 1.0f;
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
// Reads the mapped params' values on the strands each stage takes (its cached input): texture,
// color set, channel, guide and expression maps and plugged fields, as their ScalarField reads
// them on scalp (the graph's "scalp" external, which the operators read). False with *why when
// the cache does not hold the strands or a map cannot be read.
bool readLookFields(const GraphDocument& doc, const LookChain& chain, const ScalpMesh* scalp, LookFields* fields,
                    std::string* why);
// Whether `fields` are the ones readLookFields would read from the document's cache now.
bool sameLookFields(const GraphDocument& doc, const LookChain& chain, const LookFields& fields);
// Whether they hold for a run of `chain` (its first stage changes, everything after it moves):
// values read from the points' positions hold only on the first stage, and (stages moving each
// point after the ones before it: Clump, Noise, Curl) not per point; after an Interpolate that
// runs, only values read at the roots from the scalp (its channels move with the guides). False
// with *why.
bool lookFieldsHold(const GraphDocument& doc, const LookChain& chain, const LookFields& fields, std::string* why);

// What Interpolate and Clump take beyond their params, read from what the CPU's operators keep
// (operator_state.h) and the document's cache. Each part is shared and immutable: an engine
// uploads what it does not hold (by pointer).
// The guides a strand blends at most on the GPU (its records' slots); more: the CPU's.
constexpr std::size_t kInterpolateSlots = 16;
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
struct CollideInputs {
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
// Tie: the scalp (the "scalp" external) as the CPU's Tie takes it: its box's center and size, the
// clearance, depth and search radius of its push, and its collision field as tables (their mesh
// part also what the routes' closest points are found down); per strand the face its start (its
// root, or an earlier Tie's tail start) was nearest on the strands the node took, where a strand's
// walk down the mesh to the closest points under its route starts; the core strands (the ring's
// bins read them). Its tail channel's values as it takes them: written by an earlier Tie of the
// run (`channelFrom`, that stage), else as the strands it took carry them (null: none).
struct TieInputs {
    bool hasScalp = false;
    Vec3f scalpCenter;
    float scalpSize = 0.0f, clearance = 0.0f, depth = 0.0f, searchRadius = 0.0f;
    MeshDistanceField::Tables tables;
    std::shared_ptr<const std::vector<std::uint32_t>> seeds;
    std::shared_ptr<const std::vector<std::uint32_t>> core;
    std::size_t channelFrom = static_cast<std::size_t>(-1);
    std::shared_ptr<const std::vector<float>> channel;
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
// (xyz; the first row's w 1 where it turned).
struct JiggleInputs {
    std::shared_ptr<const std::vector<float>> strands, faces;
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
// its mesh, as Tie's routes find them); null without a scalp.
struct FlowInputs {
    bool arrows = false;
    std::shared_ptr<const std::vector<float>> strands, field;
    std::uint32_t vertices = 0, pins = 0, sinks = 0;
    MeshDistanceField::Tables tables;
};
struct StageInputs {
    std::shared_ptr<const InterpolateInputs> interpolate;
    std::shared_ptr<const ClumpInputs> clump;
    std::shared_ptr<const CurlInputs> curl;
    std::shared_ptr<const PartInputs> part;
    std::shared_ptr<const CollideInputs> collide;
    std::shared_ptr<const HairGridInputs> grid;
    std::shared_ptr<const TieInputs> tie;
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
    // Gravity, Flyaway, Collide, Wind, SelfCollide, Static, Tie, Bend, Direction, Transform, Jiggle and
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

// Reads what a run of `chain` takes (per stage), keeping what was read for the next run (a new
// value of a drag, a brush dab): the guides' shapes once per guides (checked against the binding:
// the same guides, by ID, else false), the strands bound again where guide roots moved (a dab on
// guides whose children follow them), the center choice once per Clump result.
class LookInputs {
public:
    // `params`: the run's (Interpolate's samples); `parts`: the groom's (Interpolate binds again
    // respecting them; Part leans away from them, on `scalp`, the groom's). `changed`: nodes besides
    // the stages whose results the run stands for anew (a brush stroke's guides): Clump centers
    // made from them are the CPU's. `fields`: the run's mapped params (those reading what the
    // binding makes hold while it is not bound again; a Clump's centerWeight holds the choice by its
    // values). `scalpMesh`: the groom's scalp (`scalp`) as the operators take it (the "scalp"
    // external: SelfCollide's and Static's floor and walls; null: they have none). `standIn`: guides
    // standing in for a node's cached result (a Sculpt node's at a dragged strength: sculptedGuides;
    // the node is in `changed` too); an Interpolate taking guides made from one of them through other
    // nodes is the CPU's. False with *why.
    bool read(const GraphDocument& doc, const LookChain& chain, const LookParams& params, const PartLines* parts,
              std::vector<StageInputs>* inputs, std::string* why, const std::set<NodeId>& changed = {},
              const ScalpMesh* scalp = nullptr, const LookFields* fields = nullptr, const MeshPtr& scalpMesh = nullptr,
              const std::map<NodeId, GuidesPtr>& standIn = {});
    // Milliseconds the last read took (CPU), how many guide shapes it made, and how many times
    // the strands were bound again for moved guide roots.
    double lastReadMs() const { return readMs_; }
    std::size_t lastGuidesMade() const { return guidesMade_; }
    std::size_t rebinds() const { return rebinds_; }

private:
    struct InterpolateMemo {
        std::shared_ptr<const void> made;              // the operator's binding when read
        std::shared_ptr<const InterpolateState> current;  // it, or bound again for the guides now
        std::set<const void*> fitting;                 // chunks whose strands fit the slots
        GuidesPtr guides;  // the guides the rows were made from
        std::size_t samples = 0;
        bool smooth = true;
        const void* scalp = nullptr;
        std::shared_ptr<const InterpolateGuideRows> rows;
        StrandsPtr strands;  // whose root normals
        std::shared_ptr<const std::vector<Vec3f>> normals;
        MeshPtr clearanceScalp;  // keepClearance: the scalp the field's tables were read for
        std::shared_ptr<const InterpolateClearance> clearance;
        // Over a deformed scalp: each strand's cursor, for this clearance and these strands.
        std::shared_ptr<const std::vector<std::uint32_t>> cursors;
        std::shared_ptr<const InterpolateClearance> cursorsFor;
        StrandsPtr cursorStrands;
        float degenerateApart = 0.0f;  // the nearest a degenerate face's vertex is to a root
        bool rebound = false;  // `current` bound again since the operator's binding (moved guide roots)
    };
    struct ClumpMemo {
        StrandsPtr output;
        std::shared_ptr<const ClumpInputs> fixed;  // the CPU's centers
        std::shared_ptr<const ClumpInputs> made;   // the GPU makes them
        bool tried = false;
        bool weighed = false;  // `made` picked by centerWeight's values (not by the roots alone)
        bool branch = false;   // `made` names the strands of its centers' branch (centersFrom inputCurves)
        std::shared_ptr<const std::vector<StrandId>> ids;  // per center: its ID
        std::uint64_t seed = 0;                            // what `variation` was drawn for
        std::shared_ptr<const std::vector<float>> variation;
    };
    struct CurlMemo {
        StrandsPtr in;
        std::uint64_t seed = 0;
        std::shared_ptr<const CurlInputs> inputs;
    };
    struct PartMemo {
        StrandsPtr in;
        const PartLines* parts = nullptr;  // the lines opened: the groom's, or `found`
        PartsPtr found;                    // partsFrom auto: the lines its last evaluation found (held)
        const ScalpMesh* scalp = nullptr;
        std::shared_ptr<const PartInputs> inputs;
    };
    // Collide: its surfaces' fields and their tables, each made for offsets (plus the softness) up
    // to its `most` at its field's reach: every offset of its field's cell size and reach.
    struct CollideMemo {
        MeshPtr meshes[2];
        DistanceFieldPtr fields[2];
        float most[2] = {0.0f, 0.0f};
        float softness = 0.0f;
        std::shared_ptr<const CollideInputs> inputs;
    };
    struct ReachMemo {
        StrandsPtr in;
        float reach = 0.0f;
    };
    // SelfCollide and Static: the grid's layout on the strands it took, and the walls of the scalp
    // for its cell (kept while the scalp and the cell are).
    struct GridMemo {
        StrandsPtr in;
        MeshPtr scalp;
        std::shared_ptr<const HairGridInputs> inputs;
    };
    std::map<NodeId, GridMemo> grid_;
    // Tie: what it reads of the strands it took and the scalp (kept while they are).
    struct TieMemo {
        StrandsPtr in;
        MeshPtr scalp;
        std::string channel;
        std::shared_ptr<const TieInputs> inputs;
        MeshPtr tablesScalp;  // the scalp the tables (their rings welded) were made for
        MeshDistanceField::Tables tables;
    };
    std::map<NodeId, TieMemo> tie_;
    // RestoreLength, BlendNeighbors and Tangle: what they read of the strands they took, their
    // reference and the CPU's last evaluation (kept while those are the same).
    struct ShaperMemo {
        StrandsPtr in;
        const void* made = nullptr;   // the reference (RestoreLength), the operator's state
        std::shared_ptr<const void> keep;  // (it, held while compared by address)
        std::shared_ptr<const RestoreInputs> restore;
        std::shared_ptr<const TangleInputs> tangle;
        std::shared_ptr<const FlowInputs> flow;
        MeshPtr tablesScalp;  // Flow: the scalp its tables (their rings welded) were made for
        MeshDistanceField::Tables tables;
        float reach = 0.0f;
    };
    std::map<NodeId, ShaperMemo> shaper_;
    // RestoreLength, BlendNeighbors, Tangle (stage k of `chain`; `before`: the nodes the run moves
    // before it; `rebound`: a running Interpolate before it bound again): what they take, into *out.
    // False with *why.
    bool readShaper(const GraphDocument& doc, const LookChain& chain, std::size_t k, const std::set<NodeId>& before, bool rebound,
                    const ScalpMesh* groomScalp, const MeshPtr& scalpMesh, StageInputs* out, std::string* why);
    // Tie, stage k of `chain`: what its kernels take (out->tie); false with *why.
    bool readTie(const GraphDocument& doc, const LookChain& chain, std::size_t k, const ScalpMesh* groomScalp, const MeshPtr& scalpMesh,
                 StageInputs* out, std::string* why);
    // Bend, Direction, Transform, Displace: the roots on the scalp, for the strands they took.
    struct RootMemo {
        StrandsPtr in;
        const ScalpMesh* scalp = nullptr;
        std::shared_ptr<const RootInputs> inputs;
    };
    std::map<NodeId, RootMemo> roots_;
    // Twist: its axes and each strand's, for the strands it took and its axes input.
    struct TwistMemo {
        StrandsPtr in;
        GuidesPtr axes;
        std::string channel;
        std::shared_ptr<const TwistInputs> inputs;
    };
    std::map<NodeId, TwistMemo> twist_;
    // What those take (into `out`), node `id`'s; `standIn`: the nodes whose results the run takes guides made for it in place of.
    bool shaperRoots(const GraphDocument& doc, NodeId id, const ScalpMesh* scalp, StageInputs* out, std::string* why);
    bool twistAxes(const GraphDocument& doc, NodeId id, const std::set<NodeId>& standIn, StageInputs* out, std::string* why);
    // Jiggle: the lags for the strands it took, the scalp and history it read and the params (its
    // frequency, damping and history) they were made at.
    struct JiggleMemo {
        StrandsPtr in;
        MeshPtr scalp;
        ScalpHistoryPtr history;
        double nowFrame = 0.0, frequency = 0.0, damping = 0.0;
        int frames = 0;  // the history param
        std::shared_ptr<const JiggleInputs> inputs;
    };
    std::map<NodeId, JiggleMemo> jiggle_;
    bool readJiggle(const GraphDocument& doc, NodeId id, const StageParams& params, StageInputs* out, std::string* why);
    // MeshCut: its mesh's tree, once per mesh.
    struct MeshCutMemo {
        MeshPtr mesh;
        std::shared_ptr<const MeshCutInputs> inputs;
    };
    std::map<NodeId, MeshCutMemo> meshCut_;
    bool readMeshCut(const GraphDocument& doc, NodeId id, StageInputs* out, std::string* why);
    // Cull, Split, Mirror: their layout, once per input and result.
    struct GatherMemo {
        StrandsPtr in, out;
        const ScalpMesh* scalp = nullptr;
        std::shared_ptr<const GatherInputs> inputs;
    };
    std::map<NodeId, GatherMemo> gather_;
    bool readGather(const GraphDocument& doc, NodeId id, const ScalpMesh* groomScalp, StageInputs* out, std::string* why);
    MeshPtr scalpMesh_;  // the last read's (a Clump's centers' branch reads with it)
    // A Clump's centers made by a branch the run moves (LookCentersRun): what that branch's run
    // takes, kept as a chain's is (its own reader; its mapped values, read once per evaluation).
    struct CentersMemo {
        std::shared_ptr<LookInputs> inputs;
        LookChain fieldsChain;
        LookFields fields;
        std::size_t rebindsSeen = 0;  // its reader's rebinds() counted already
    };
    // The run of the branch ending at `end` (feeding Clump `clump`'s centers) from its first node
    // that `moved` (the nodes the run stands for anew) makes; false with *why.
    bool centersRun(const GraphDocument& doc, NodeId clump, NodeId end, const std::set<NodeId>& moved, const PartLines* parts,
                    const ScalpMesh* scalp, std::shared_ptr<const LookCentersRun>* run, std::string* why,
                    const std::map<NodeId, GuidesPtr>& standIn);
    std::map<NodeId, CentersMemo> centers_;
    // A Merge's input b moved by a drag (`branch`: b's chain and params), or the run up a Cull's,
    // Split's or Mirror's input (node `node`'s): its run, its strands, mapped values and inputs read
    // as centersRun reads a branch's; false with *why.
    bool branchRun(const GraphDocument& doc, NodeId node, const LookBranch& branch, const PartLines* parts, const ScalpMesh* scalp,
                   const std::set<NodeId>& changed, const std::map<NodeId, GuidesPtr>& standIn,
                   std::shared_ptr<const LookCentersRun>* run, std::string* why);
    std::map<NodeId, CentersMemo> branches_;  // by Merge (or Cull, Split, Mirror)
    std::map<NodeId, CollideMemo> collide_;
    std::map<NodeId, ReachMemo> reach_;
    std::map<NodeId, InterpolateMemo> interpolate_;
    std::map<std::pair<NodeId, int>, ClumpMemo> clump_;  // by node and level (a Wet's)
    std::map<NodeId, CurlMemo> curl_;
    std::map<NodeId, PartMemo> part_;
    const GraphDocument* doc_ = nullptr;
    double readMs_ = 0;
    std::size_t guidesMade_ = 0, rebinds_ = 0;
};

// A box around every point the chain can make from `base` with these params, values and inputs
// (a viewport culls by its items' bounds: a drag that pushes hair past the CPU's last bounds must
// not lose it), grown stage by stage: Interpolate's strands lie within twice its guides' longest
// offset of their roots (blended offsets are no longer than the longest, preserveLength at most
// doubles them; keeping clearance, twice the longest guide: lifted points keep their segments'
// lengths); Clump's between where they were and their centers' (grown by the lock radius,
// and with pastTip extend by the longest strand it took);
// Length's scale stretches them about their roots; Noise's sum stays under kNoiseReach per axis
// (times the largest amplitude and profile) and Curl's within its largest radius, varied, times its
// profile; volumize pushes a point out by its share of the box's diagonal at most; Part turns
// segments about their roots (within the longest strand of them), as do Gravity and Wind; Flyaway's strays
// wander by their stray under the noise's reach and carry on past their tips by lengthen; Collide
// keeps segment lengths from points it pushed to its offset above its surfaces (within their boxes
// grown by the offset). Bend and Direction turn strands about their roots (within their lengths);
// Transform scales by its largest scale and turns about the root (or the pivot), then moves by its
// translate and its random part; Displace moves points by its largest distance times the profile;
// Twist turns a point about its axis, keeping its distance there (within the diagonal of the box so
// far and its axes'). Width moves nothing.
struct LookBounds {
    Vec3f lo, hi;
    bool valid = false;
};
constexpr float kNoiseReach = 1.5f;  // > the largest |component| of VectorNoise (about 1.0, checked by a test)
// `slack` multiplies the reach (a host binding bounds for a drag that may go further: 2).
LookBounds lookBounds(const LookParams& params, const LookFields& fields, const std::vector<StageInputs>& inputs,
                      const StrandSet& base, float slack = 1.0f);

// Max and mean distance between two point arrays (sizes must match; else max is infinity).
struct PointError {
    double max = 0, mean = 0;
    std::size_t count = 0;
};
PointError compareVectors(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b);

}  // namespace groomlab::gpu
