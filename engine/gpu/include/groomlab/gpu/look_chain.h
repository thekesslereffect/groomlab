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
// strands it takes), the shapers working on each strand alone: Bend, Direction, Transform and
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
// the scalp's mesh; not over a scalp deformed away from its rest pose), in
// any order. Cull, Split (any output: LookChain::baseOutput) and Mirror, which keep or add strands: a run through one
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
// mapped params are. (A Curl adding points still ends the run.) A graph
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
#include "groomlab/look/look_run.h"

namespace groomlab {
struct SculptEdits;
}

namespace groomlab::gpu {

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
        float cursorsLongest = -1.0f;  // the longest guide they were found for
        std::string cursorsWhy;        // why there are none (interpolateRestCursors)
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
    // `output`: its output the run takes (a Split's b, c or d).
    bool readGather(const GraphDocument& doc, NodeId id, std::size_t output, const ScalpMesh* groomScalp, StageInputs* out,
                    std::string* why);
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
