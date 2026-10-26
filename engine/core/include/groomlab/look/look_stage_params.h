// SPDX-License-Identifier: Apache-2.0
#pragma once

// A stage's params as its kernel takes them (StageParams), read from its node's: one reading for an
// operator running its own stage on the CPU engine (look_cpu.h runStage) and the GPU preview
// (engine/gpu look_chain.cpp readLookParams: a drag's values in place of the node's own).

#include <array>
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

private:
    const ParamSet& params_;
    std::string name_;
};

// Whether readStageParams reads a stage of `kind`'s params.
bool readsStageParams(StageKind kind);
// A stage of `kind`'s params (Length, Width, Part, Gravity, Flyaway, the shapers working on each
// strand alone, Smooth, BlendNeighbors, Tangle, Flow) into *s: its numbers, flags and ramps, not its stage, seed or mask (the reader's).
// False with *why where the kernel does not run it so (the CPU's). (Width's feathers' shafts with a
// rachis, an auto Part's lines: the caller's to refuse.)
bool readStageParams(StageKind kind, const StageParamReader& r, StageParams* s, std::string* why);

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
void writeRootFrames(const StrandSet& in, const ScalpMesh* scalp, float* to);
std::shared_ptr<const RootInputs> rootInputs(const StrandSet& in, const ScalpMesh* scalp);
// Twist's axes (TwistInputs, as TwistOp makes them): around the curves of `axes` (two points or more,
// resampled by arc length) each strand's nearest by its root (`nearestByKernel`: left to TwistCS, its
// root words other than any root); else around each clump's middle (by `channel`: its core
// strands, preview_core.h). Null with *why where the strands carry no such channel.
std::shared_ptr<TwistInputs> twistInputs(const StrandSet& in, const GuideSet* axes, const std::string& channel, bool nearestByKernel,
                                         std::string* why);
// Tangle's partners as the kernel takes them (TangleInputs) from its operator's (operator_state.h
// TangleState, on `in`).
std::shared_ptr<TangleInputs> tangleInputs(const StrandSet& in, const TangleState& state);
// Part's sides (PartInputs, its reach 0: the caller's): each strand's side of `lines` (only the part
// `only` where named) from its root on the rest scalp, the way away from it and how far; none `within`
// or farther (an operator's own width: the strands it leaves; the preview's every one, a width dragged).
std::shared_ptr<PartInputs> partInputs(const StrandSet& in, const PartLines* lines, const ScalpMesh* scalp, const std::string& only,
                                       float within = std::numeric_limits<float>::infinity());

}  // namespace groomlab::gpu
