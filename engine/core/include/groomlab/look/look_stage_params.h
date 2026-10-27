// SPDX-License-Identifier: Apache-2.0
#pragma once

// A stage's params as its kernel takes them (StageParams), read from its node's: one reading for an
// operator running its own stage on the CPU engine (look_cpu.h runStage) and the GPU preview
// (engine/gpu look_chain.cpp readLookParams: a drag's values in place of the node's own).

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/look/look_run.h"

namespace groomlab::gpu {

// How a node's params are read: as they are (an operator's own), or (the preview's) with a drag's
// values.
class StageParamReader {
public:
    StageParamReader(const ParamSet& params, std::string name) : params_(params), name_(std::move(name)) {}
    virtual ~StageParamReader() = default;
    // As the node's evaluation reads them (a plugged field: ParamSet::field).
    const ParamSet& params() const { return params_; }
    // The node's name, what a *why starts with ("" an operator's own).
    const std::string& name() const { return name_; }
    virtual double number(const std::string& param) const { return params_.getDouble(param); }
    virtual Ramp ramp(const std::string& param) const { return params_.getRamp(param); }
    virtual Vec3f vector(const std::string& param) const { return params_.getVec3(param); }
    // A vector param that is one world vector for every strand (a VectorField without an
    // expression); false for one per strand.
    virtual bool constantVector(const std::string& param, Vec3f* out) const;
    // A mappable param's number, the multiplier of its values (StageFields::field): an operator's
    // own reads its values whole (readFieldValues: 1 where mapped). False with *why where the
    // kernel cannot take it.
    virtual bool mappable(const std::string& param, float* value, std::string* why) const;
    // The time the stage runs at (seconds) and its frame rate: Wind's (an operator's evaluation's,
    // the preview's the one its cached result was made at).
    double time = 0.0;
    double framesPerSecond = 24.0;
    // Interpolate's guide samples (interpolateSamples of the strands it takes).
    std::size_t samples = 0;
    // Interpolate filling tubes: the caller hands their spines in (InterpolateInputs::tubes, the CPU
    // engine's); else a fillTubes Interpolate is the CPU's.
    bool tubes = false;
    // Interpolate taking the guides' length alone (take length): the caller runs it on the CPU
    // engine; else the CPU's.
    bool lengthAlone = false;

private:
    const ParamSet& params_;
    std::string name_;
};

// Whether readStageParams reads a stage of `kind`'s params.
bool readsStageParams(StageKind kind);
// A stage of `kind`'s params (Interpolate (its samples, flag[0], the caller's), Length, Clump (not a Wet's level), Noise, Curl, Width, Part, Gravity, Flyaway, Wind, Collide, the shapers working on
// each strand alone, Smooth, BlendNeighbors, Tangle, Flow, RestoreLength (its reference's place the
// preview's: flag[1]), MeshCut) into *s: its numbers, flags and ramps, not its stage, seed or mask (the reader's).
// False with *why where the kernel does not run it so (the CPU's). (Width's feathers' shafts with a
// rachis, an auto Part's lines: the caller's to refuse.)
bool readStageParams(StageKind kind, const StageParamReader& r, StageParams* s, std::string* why);

// A Clump's (or a Wet's level's) numbers and choices but its strength, volumize and profiles into
// *s, `get` reading a number of `clump` (the preview's: a drag's).
template <class Get>
void clumpShape(const ParamSet& clump, const Get& get, StageParams* s) {
    s->value[2] = static_cast<float>(get("flatness"));
    s->value[3] = static_cast<float>(get("lockRadius"));
    const float repelRadius = static_cast<float>(get("repelRadius"));
    std::memcpy(&s->flag[0], &repelRadius, sizeof(float));  // the kernel reads its bits as a float
    s->flag[1] = clump.getString("pastTip") == "extend" ? 1 : 0;
    const std::string flatten = clump.getString("flatten");
    s->flag[2] = flatten == "random" ? 1 : flatten == "direction" ? 2 : 0;
    // The per-clump variation (by each center's ID: ClumpInputs::variation) and copy.
    s->value2[0] = static_cast<float>(get("strengthRandom"));
    s->value2[1] = static_cast<float>(get("lockRadiusRandom"));
    s->value2[2] = static_cast<float>(get("copyAmount"));
    const Vec3f direction = clump.getVec3("flattenDirection");
    std::memcpy(&s->flag2[0], &direction.x, sizeof(float));
    std::memcpy(&s->flag2[1], &direction.y, sizeof(float));
    std::memcpy(&s->flag2[2], &direction.z, sizeof(float));
    s->tabled = true;
    s->hasRamp2 = true;
}
// Clump's centers as the kernel takes them (ClumpInputs) from its operator's: each strand's center,
// the centers' curves as made (their copy shared), their roots' normals, their variation by `ids`
// and `seed` (clumpVariation: the strength's, the lock radius's and a random flatten's draws).
std::shared_ptr<ClumpInputs> clumpInputs(std::shared_ptr<const std::vector<std::uint32_t>> assigned, std::vector<Vec3f> curves,
                                         std::vector<Vec3f> normals, const std::vector<StrandId>& ids, std::uint64_t seed);
std::shared_ptr<const std::vector<float>> clumpVariation(const std::vector<StrandId>& ids, std::uint64_t seed);

// A param holding a map of its own, or that or a field plugged in (as its node's evaluation reads it).
bool ownMap(const ParamSet& params, const std::string& name);
bool mappedParam(const ParamSet& params, const std::string& name);
// Direction's mapped params, the first two of inclination, roll and strength (its fields holding
// their values, in this order).
std::vector<std::string> directionMapped(const ParamSet& params);
// The params a stage's two mapped fields are read for (StageFields::field; look_stages.inc, a
// Direction's the ones it maps), "" none.
std::array<std::string, 2> mappedParams(StageKind kind, const ParamSet& params);
// Whether field `i`'s values may vary along the strands (read per point where they do).
bool mappedPerPoint(StageKind kind, int i);
// Whether the stage moves each point by its own mapped value (a per-point value read after the
// points before it moved is the CPU's).
bool movesPointByPoint(StageKind kind);

// The frames Bend, Direction, Transform and Displace take (RootInputs): each strand's scalp normal
// and U direction at its root (none off the scalp), 8 floats a strand into `to` (every one written).
// The CPU engine writes them into its own memory where a run gives none (look_engine.cpp).
void writeRootFrames(const StrandSet& in, const ScalpMesh* scalp, float* to, const std::vector<std::uint32_t>* which = nullptr);  // which: those strands alone, in that order
std::shared_ptr<const RootInputs> rootInputs(const StrandSet& in, const ScalpMesh* scalp);
// Twist's axes (TwistInputs, as TwistOp makes them): around the curves of `axes` (two points or more,
// resampled by arc length) each strand's nearest by its root (`nearestByKernel`: left to TwistCS, its
// root words other than any root); else around each clump's middle (by `channel`: its core
// strands, preview_core.h). Null with *why where the strands carry no such channel. `which` (around
// axes, not by kernel): those strands alone, a strand's words at its place in the list (an engine's
// view, LookEngine::setBaseView).
std::shared_ptr<TwistInputs> twistInputs(const StrandSet& in, const GuideSet* axes, const std::string& channel, bool nearestByKernel,
                                         std::string* why, const std::vector<std::uint32_t>* which = nullptr);
// Tangle's partners as the kernel takes them (TangleInputs) from its operator's (operator_state.h
// TangleState, on `in`).
std::shared_ptr<TangleInputs> tangleInputs(const StrandSet& in, const TangleState& state);
// Collide's surfaces' fields as tables (CollideInputs): `meshes` (the scalp, then the collider: two at
// most), their fields at the offset (collisionField), `colliders` a bit each for the collider input's;
// tables for offsets up to the most the fields' cells settle (a dragged offset the kernel's number).
// Null with *why where a surface has no field or tables the kernel reads (the CPU's).
std::shared_ptr<CollideInputs> collideInputs(const std::vector<MeshPtr>& meshes, std::uint32_t colliders,
                                             const std::vector<DistanceFieldPtr>& fields, float offset, float softness, std::string* why);
// RestoreLength's reference (RestoreInputs) for the strands `in`: each strand's reference curve by
// its ID (`refStrands`', else `refGuides`'), resampled to its point count, its segments' lengths and
// first segment (a strand without one: -1, left as it is); `longest` the longest reference, `found`
// whether there is any (null: not asked). `which`: those strands alone (an engine's view), their
// first segments at their places in the list, their lengths at their points (the rest 0).
std::shared_ptr<RestoreInputs> restoreInputs(const StrandSet& in, const StrandSet* refStrands, const GuideSet* refGuides, float* longest,
                                             bool* found = nullptr, const std::vector<std::uint32_t>* which = nullptr);
// Smooth's window ends (SmoothOp's, the same for every strand: its three boxes `smoothWidth` cells
// wide over the reflected period of 2 x `cells` cells), each cell's upper end, then each one's
// lower: whole periods before it, how far into its cell, and the cell (24 bytes each: the CPU
// engine hands them to look.slang SmoothCS as they are).
struct SmoothEnd {
    double turns = 0.0;
    double into = 0.0;
    std::uint32_t cell = 0, unused = 0;
};
double smoothWidth(float scale, std::size_t cells);
std::vector<SmoothEnd> smoothEnds(float scale, std::size_t cells);
// Flow's field and roots as the kernel takes them (FlowInputs, its tables the caller's) for the
// strands `in` traced over `scalp` (null: none) along `field` (null: no arrows), as FlowOp reads them.
std::shared_ptr<FlowInputs> flowInputs(const StrandSet& in, const ScalpMesh* scalp, std::shared_ptr<const FlowField> field,
                                       const std::vector<std::uint32_t>* which = nullptr);  // which: those strands alone, in that order
// Jiggle's lags as the kernel takes them (JiggleInputs) from its operator's (operator_state.h
// jiggleLags, over `scalp`) for the strands `in`.
std::shared_ptr<JiggleInputs> jiggleInputs(const StrandSet& in, const ScalpMesh& scalp, const JiggleLags& lags,
                                           const std::vector<std::uint32_t>* which = nullptr);  // which: those strands alone, in that order
// Curl's clumps as the kernel takes them (CurlInputs, coherence clump), found on the strands `in` by
// the channel `channel` as CurlOp finds them, their frames from `scalp`'s normals (null: none), their
// draws by `seed`; null without the channel.
// Tie's inputs (TieInputs but its tables and channelFrom: the caller's) for the strands `in` over
// `scalp` (null or no triangles: none) with its tail channel `channel`, as TieOp takes them: the
// scalp's box, its push's clearance, depth and search radius, each strand's seed face, the core
// strands, the channel as `in` carries it, and the host (the CPU engine's).
std::shared_ptr<TieInputs> tieInputs(const StrandsPtr& in, const MeshPtr& scalp, const std::string& channel);
std::shared_ptr<CurlInputs> curlInputs(const StrandSet& in, const ScalpMesh* scalp, const std::string& channel, std::uint64_t seed);
// MeshCut's mesh as the kernel takes it (its ray tree; none for no mesh).
std::shared_ptr<MeshCutInputs> meshCutInputs(const MeshPtr& mesh);
// Part's sides (PartInputs, its reach 0: the caller's): each strand's side of `lines` (only the part
// `only` where named) from its root on the rest scalp, the way away from it and how far; none `within`
// or farther (an operator's own width: the strands it leaves; the preview's every one, a width dragged).
std::shared_ptr<PartInputs> partInputs(const StrandSet& in, const PartLines* lines, const ScalpMesh* scalp, const std::string& only,
                                       float within = std::numeric_limits<float>::infinity(),
                                       const std::vector<std::uint32_t>* which = nullptr);  // which: those strands alone, in that order

}  // namespace groomlab::gpu
