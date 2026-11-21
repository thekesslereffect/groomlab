// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers for operators that evaluate incrementally (Operator::evaluateIncremental, graph.h):
// the node's last result is copied array by array and only the strands its inputs changed in
// are made again, each exactly as a full evaluation makes it.

#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/parallel.h"

namespace groomlab {

// The strands input `slot` changed in, when an operator can follow them: known and not all of
// them (the same strands, IDs, roots, point counts and channels as the value the last result was
// made from). Null otherwise.
inline const StrandChanges* someStrandsChanged(const IncrementalInputs& in, std::size_t slot) {
    if (slot >= in.inputs.size() || !in.inputs[slot] || in.inputs[slot]->all) return nullptr;
    return in.inputs[slot].get();
}

// The same, and also when the strands listed may hold another number of points than they did
// (StrandChanges::recounted: a Resample by curvature or a Curl adding points before it, after a
// comb dab): for an operator that then lays its last result out anew (pointsOfLast below), each
// strand not listed keeping what it held at its new place. Null otherwise.
inline const StrandChanges* someStrandsChangedOrRecounted(const IncrementalInputs& in, std::size_t slot) {
    if (slot >= in.inputs.size() || !in.inputs[slot] || (in.inputs[slot]->all && !in.inputs[slot]->recounted)) return nullptr;
    return in.inputs[slot].get();
}

// True when input `slot` is the value the last result was made from, or one equal to it.
inline bool inputUnchanged(const IncrementalInputs& in, std::size_t slot) {
    return slot < in.inputs.size() && in.inputs[slot] && in.inputs[slot]->none();
}

// True when input `slot` holds the very points the last result was made from (unchanged, or only
// its widths and channels changed: StrandChanges::pointsKept), so an operator whose points depend
// on its input's points alone keeps its last ones.
inline bool inputPointsKept(const IncrementalInputs& in, std::size_t slot) {
    return slot < in.inputs.size() && in.inputs[slot] && (in.inputs[slot]->none() || in.inputs[slot]->pointsKept);
}

// The input's widths and channels on the last result's points: a copy of `input` sharing every
// array of it but the positions, which are `last`'s very array (the two hold the same strands:
// inputPointsKept). For an operator that only moves points, by its input's points alone.
inline StrandsPtr withPointsOf(const StrandSet& input, const StrandSet& last) {
    auto out = std::make_shared<StrandSet>(input);
    out->sharePositionsOf(last);
    return out;
}

// The node's last result as strands, when it holds as many strands and points as `input` (the
// input's changes keep its layout, so the result made from it before had this one's).
inline StrandsPtr previousStrandsFor(const IncrementalInputs& in, const StrandSet& input) {
    StrandsPtr previous = valueAs<StrandsPtr>(in.previous);
    if (!previous || previous->strandCount() != input.strandCount() || previous->pointCount() != input.pointCount()) {
        return nullptr;
    }
    return previous;
}
// The same for changes that may have recounted strands (someStrandsChangedOrRecounted): as many
// strands as `input`, and unless recounted as many points.
inline StrandsPtr previousStrandsFor(const IncrementalInputs& in, const StrandSet& input, const StrandChanges& changes) {
    StrandsPtr previous = valueAs<StrandsPtr>(in.previous);
    if (!previous || previous->strandCount() != input.strandCount() ||
        (!changes.recounted && previous->pointCount() != input.pointCount())) {
        return nullptr;
    }
    return previous;
}

// The last result's points (or widths) into `out`, a set holding the same strands, for the
// strands an incremental evaluation leaves alone: the last result's very array when the graph let
// go of it (IncrementalInputs::sole) and `last` is its only other holder (nobody else can read it),
// else a copy. The last result's points (widths) are not read after this. Its points shared (the
// nodes after it hold them still): the points of the result before it, when the graph kept them
// and nobody else holds them (IncrementalInputs::spare), with the strands the last result changed
// in written over them from it; so only those strands are copied, not every point.
inline std::vector<Vec3f>& positionsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in) {
    if (in.sole && last.use_count() == 2 && last->positionsSole()) return out.takePositionsFrom(const_cast<StrandSet&>(*last));
    if (in.spare && in.spareChanges && !in.spareChanges->all && in.spare->positionsSole() &&
        in.spare->positions().size() == last->positions().size() && last->positions().size() == last->offsets().back()) {
        std::vector<Vec3f>& points = out.takePositionsFrom(*in.spare);
        const std::vector<std::uint32_t>& strands = in.spareChanges->strands;
        parallelFor(0, strands.size(), [&](std::size_t k) {
            const std::size_t begin = last->strandBegin(strands[k]), n = last->strandPointCount(strands[k]);
            if (n) std::memcpy(&points[begin], &last->positions()[begin], n * sizeof(Vec3f));
        }, 256);
        return points;
    }
    return out.copyPositionsFrom(*last);
}
inline std::vector<float>& widthsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in) {
    if (in.sole && last.use_count() == 2) return out.takeWidthsFrom(const_cast<StrandSet&>(*last));
    return out.copyWidthsFrom(*last);
}

// Whether `last` holds every strand of `out` not listed (`listed` sorted) at the number of points
// `out` holds it at, the two holding as many strands: what a result made before some strands of its
// input were recounted (StrandChanges::recounted) holds against that input now.
bool sameCountsButListed(const StrandSet& out, const StrandSet& last, const std::vector<std::uint32_t>& listed);
// A per-point array of `last` (`from`) into `out`'s (`to`), for the strands not listed (sorted):
// each one's elements where `out` holds that strand (sameCountsButListed), runs of strands copied
// whole, in parallel blocks. The listed strands' elements are left as they come.
void relayUnlisted(Vec3f* to, const StrandSet& out, const Vec3f* from, const StrandSet& last, const std::vector<std::uint32_t>& listed);
void relayUnlisted(float* to, const StrandSet& out, const float* from, const StrandSet& last, const std::vector<std::uint32_t>& listed);

// positionsOfLast for the strands `changes` leaves alone, into `out` (a copy of the input); with
// strands recounted (StrandChanges::recounted: the last result is laid out as the input was) each
// one's points where the input holds that strand now, into an array of `out`'s own. False when the
// last result does not hold those strands at those counts (the operator then evaluates in full).
// The listed strands' points are for the caller to write.
bool pointsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in, const StrandChanges& changes);
// The same of the widths (widthsOfLast).
bool widthsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in, const StrandChanges& changes);
// A point channel of the last result as `out`'s own (StrandSet::copyPointChannelFrom; added when
// `out` has none), with strands recounted each one's values where `out` holds that strand, the
// listed strands' left at the channel's default. Null when the last result has no such channel or
// one of another length.
std::vector<float>* pointChannelOfLast(StrandSet& out, const std::string& name, const StrandSet& last, const StrandChanges& changes);

// The last result as a set of its own, for an operator whose result is laid out as its last one
// but not as its input (a subset, a merge): every array shared with it, except its points and
// widths, which are its very arrays when the graph let go of it (positionsOfLast), else copies.
inline std::shared_ptr<StrandSet> copyOfLast(const StrandsPtr& last, const IncrementalInputs& in) {
    StrandSet arrays;
    positionsOfLast(arrays, last, in);
    widthsOfLast(arrays, last, in);
    auto out = std::make_shared<StrandSet>(*last);
    out->takePositionsFrom(arrays);
    out->takeWidthsFrom(arrays);
    return out;
}

// Strand k of `from` written over strand at[k] of `out` (each the same number of points; `out`
// holding every channel `from` has, with what it lacks left as it is): its points, widths and
// channel values. A channel is taken for writing only where a value differs, so the arrays no
// strand changed in stay shared.
void writeStrands(StrandSet& out, const StrandSet& from, const std::vector<std::uint32_t>& at);

// body(s) for each strand listed, side by side.
template <class Body>
void forEachListed(const std::vector<std::uint32_t>& strands, const Body& body) {
    parallelFor(0, strands.size(), [&](std::size_t k) { body(static_cast<std::size_t>(strands[k])); }, 128);
}

// Strand s's points of `from` into `to` (sets of the same layout): a strand made again starts
// from its incoming points, as in a full evaluation's copy of them.
inline void copyStrandPoints(StrandSet& to, const StrandSet& from, std::size_t s) {
    const Vec3f* src = from.strandPoints(s);
    Vec3f* dst = to.strandPoints(s);
    for (std::size_t i = 0, n = from.strandPointCount(s); i < n; ++i) dst[i] = src[i];
}

// An operator's own stage on the CPU engine (look_cpu.h; look.slang's, the GPU preview's own).
struct EngineStage {
    gpu::StageKind kind;
    const ParamSet& params;
    const ScalpMesh* scalp;
    // Each strand's direction (StageInputs::directions) of strand s, where the node takes one per
    // strand (Bend's and Gravity's vector field; Direction's three mapped params); null: the params'.
    std::function<Vec3f(std::size_t)> direction = nullptr;
    // Its stage for the strands `which` of `in`, into `out` (all of them: null, the result made).
    StrandsPtr run(const StrandsPtr& in, const std::vector<std::uint32_t>* which, StrandSet* out) const;
};
// The run the operators moving each strand by its stage share: in full, or (with `incremental`)
// only the strands the input changed in, into the last result's copy; false when that cannot be
// done. `local`: every mapped value reads each strand alone.
bool rerunChanged(const StrandsPtr& in, bool local, const IncrementalInputs* incremental, Value* result, StrandChanges* changed,
                  const EngineStage& engine);
// Whether the params `names` of `params` (their maps validated on `in`, as the node reads them)
// each read a strand alone (ScalarField::strandLocal): rerunChanged's `local` for a node's mapped
// numbers.
bool paramsStrandLocal(const ParamSet& params, std::initializer_list<const char*> names, const ScalpMesh* scalp, const StrandSet& in);
// The same of a node that may have nothing to do. `identity`: the strands pass through as they
// came (so does an empty set); `otherInputsSame`: its inputs besides the strands are as they were.
bool reshapeEach(const StrandsPtr& in, bool identity, bool local, const IncrementalInputs* incremental, bool otherInputsSame,
                 Value* result, StrandChanges* changed, const EngineStage& engine);

}  // namespace groomlab
