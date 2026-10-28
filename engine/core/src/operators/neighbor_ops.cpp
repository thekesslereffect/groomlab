// SPDX-License-Identifier: Apache-2.0
// Shaping operators that read other strands: BlendNeighbors (each strand's
// shape pulled toward its nearest neighbours' mean shape, for a coherent
// flow after noisy interpolation or sculpting) and RestoreLength (segment
// lengths or the whole length brought back from a reference set of the same
// strands, after deformers that stretch them).

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "groomlab/curve.h"
#include "groomlab/field.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/parts.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr std::size_t kMaxNeighbors = 16;
// With parts, some of the nearest roots may be across one: look at more.
constexpr std::size_t kMaxCandidates = kMaxNeighbors * 2 + 1;

// BlendNeighbors: each strand's shape (its points as offsets from its root)
// moves toward the inverse-distance-weighted mean of its `neighbors` nearest core
// strands' shapes (preview_core.h: those every preview holds, so its neighbours are the same at
// every preview density) by `strength` times `profile` along the length. Neighbours
// are found by root (on the rest scalp when it has one, so a deforming scalp
// keeps the same neighbours), their offsets turned from their root normal to
// this strand's, and resampled to this strand's point count when theirs
// differs. Roots never move; with preserveLength each strand keeps its own
// arc length. Every neighbour is read from the input set, so the result does
// not depend on the order strands are processed in. Neighbours are never across a part
// (respectParts) nor, with regions (the `regionChannel` an Interpolate's regionMap writes), in
// another region: the nearest of the strand's own region.
class BlendNeighborsOp : public Operator {
public:
    std::string typeName() const override { return op::kBlendNeighbors; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override {
        return params().getBool("respectParts") ? std::vector<std::string>{"scalp", "parts"}
                                                      : std::vector<std::string>{"scalp"};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"neighbors", std::int64_t{4}, "Nearest strands (by root, of those every preview holds) each strand blends toward, 1-16"}
                .range(1, static_cast<double>(kMaxNeighbors)),
            ParamSpec{"strength", 1.0, "How far each strand moves toward its neighbours' mean shape, 0-1 (mappable)",
                      "fraction"}
                .range(0, 1),
            {"profile", Ramp::constant(1.0f), "Amount along the length, root to tip"},
            ParamSpec{"preserveLength", true, "Scale the blended strand back to its own length"}.adv(),
            {"respectParts", true, "Never blend with a strand across a part"},
            ParamSpec{"regionChannel", std::string("region"),
                      "Channel numbering each strand's region (an Interpolate's regionMap writes region): strands blend "
                      "only with strands of their own region; empty, or a channel the strands lack: across regions"}
                .adv()
                .readsChannel(true),
        };
    }

    // operator_state.h blendNeighborsState.
    bool keptState(const StrandsPtr& output, std::shared_ptr<const BlendNeighborsState>* state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (!output || !kept_.state || !sameOrShown(kept_.output.lock(), output)) return false;
        *state = kept_.state;
        return true;
    }
    // Masked: its state answers for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (made && kept_.output.lock() == made) kept_.output = mixed;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kBlendNeighbors);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField amount(params(), "strength", scalp);
        amount.validate(*in);
        const std::vector<float>* regions = regionsOf(*in, params().getString("regionChannel"));
        remember(nullptr, nullptr);
        if (in->strandCount() < 2) {
            // Nothing to blend with, whatever the strength.
            auto none = std::make_shared<BlendNeighborsState>();
            none->offsets.assign(in->strandCount() + 1, 0u);
            none->normals.assign(in->strandCount(), Vec3f{});
            remember(in, std::move(none));
            return in;
        }
        if (amount.isConstant() && amount.maximum() <= 0.0f) return in;
        const std::size_t requested = static_cast<std::size_t>(params().getInt("neighbors"));
        const PartsPtr parts = params().getBool("respectParts") ? valueAs<PartsPtr>(ctx.external("parts")) : nullptr;

        // Roots to search (rest pose where there is one), root normals (zero when unrooted), sides of parts.
        const std::size_t count = in->strandCount();
        std::vector<Vec3f> roots(count), normals(count);
        std::vector<PartLines::Side> sides(parts ? count : 0);
        parallelFor(0, count, [&](std::size_t s) {
            const StrandRoot& root = in->roots()[s];
            const Vec3f p = in->strandPointCount(s) > 0 ? in->strandPoints(s)[0] : Vec3f{};
            roots[s] = restRootOr(scalp, root, p);
            normals[s] = scalp && scalp->contains(root) ? scalp->normal(root) : Vec3f{};
            if (parts) sides[s] = parts->sideOf(roots[s]);
        });
        // The strands each strand may blend with: the core ones (preview_core.h: those every preview
        // holds, so a strand's neighbours are the same at every preview density), all of them or with
        // regions those of its own region (a group each, searched on its own).
        const PreviewCore core(*in);
        struct Group {
            std::vector<std::uint32_t> strands;  // its core strands
            std::optional<PointIndex> index;
            std::size_t k = 0, candidates = 0;
        };
        std::vector<Group> groups;
        std::vector<std::uint32_t> groupOf(count, 0);
        std::map<float, std::uint32_t> ids;  // (a NaN region: a group of its own per strand)
        if (!regions) groups.emplace_back();
        for (std::size_t s = 0; s < count; ++s) {
            std::uint32_t id = 0;
            if (regions) {
                const float r = (*regions)[s];
                id = r != r ? static_cast<std::uint32_t>(groups.size())
                            : ids.emplace(r, static_cast<std::uint32_t>(groups.size())).first->second;
                if (id == groups.size()) groups.emplace_back();
            }
            if (core.core(s)) groups[id].strands.push_back(static_cast<std::uint32_t>(s));
            groupOf[s] = id;
        }
        for (Group& g : groups) {
            if (g.strands.empty()) continue;
            std::vector<Vec3f> at;
            at.reserve(g.strands.size());
            for (const std::uint32_t s : g.strands) at.push_back(roots[s]);
            g.index.emplace(std::move(at));
            // The strand itself may be among the nearest; with parts, some may be across one.
            g.k = std::min(g.strands.size(), requested);
            g.candidates = std::min(g.strands.size(), parts ? 2 * g.k + 1 : g.k + 1);
        }

        // Each strand's neighbours and their shares (kept for a host blending elsewhere, the GPU
        // preview: operator_state.h blendNeighborsState), whatever its strength.
        std::vector<std::array<std::uint32_t, kMaxNeighbors>> nearOf(count);
        std::vector<std::array<float, kMaxNeighbors>> shareOf(count);
        std::vector<std::uint32_t> keptOf(count, 0u);
        parallelFor(0, count, [&](std::size_t s) {
            if (in->strandPointCount(s) < 2) return;
            const Group& group = groups[groupOf[s]];
            if (!group.index) return;
            const std::size_t k = group.k;
            std::array<std::size_t, kMaxCandidates> idx{};
            std::array<float, kMaxCandidates> d2{};
            const std::size_t found = group.index->kNearest(roots[s], group.candidates, idx.data(), d2.data());
            // Keep the k nearest others, on this strand's side of any part.
            std::array<float, kMaxNeighbors> weight{};
            std::size_t kept = 0;
            float wsum = 0.0f;
            for (std::size_t i = 0; i < found && kept < k; ++i) {
                const std::size_t g = group.strands[idx[i]];
                if (g == s || in->strandPointCount(g) < 2) continue;
                if (parts && PartLines::separated(sides[s], sides[g])) continue;
                nearOf[s][kept] = static_cast<std::uint32_t>(g);
                weight[kept] = 1.0f / std::max(std::sqrt(d2[i]), 1e-6f);
                wsum += weight[kept];
                ++kept;
            }
            if (kept == 0 || wsum <= 0.0f) return;
            for (std::size_t i = 0; i < kept; ++i) shareOf[s][i] = weight[i] / wsum;
            keptOf[s] = static_cast<std::uint32_t>(kept);
        });
        auto state = std::make_shared<BlendNeighborsState>();
        state->offsets.assign(count + 1, 0u);
        for (std::size_t s = 0; s < count; ++s) state->offsets[s + 1] = state->offsets[s] + keptOf[s];
        state->neighbors.resize(state->offsets[count]);
        state->weights.resize(state->offsets[count]);
        for (std::size_t s = 0; s < count; ++s) {
            std::copy(nearOf[s].begin(), nearOf[s].begin() + keptOf[s], state->neighbors.begin() + state->offsets[s]);
            std::copy(shareOf[s].begin(), shareOf[s].begin() + keptOf[s], state->weights.begin() + state->offsets[s]);
        }
        state->normals = normals;

        // look.slang's BlendNeighborsCS, the GPU preview's own (look_cpu.h), on the neighbours found here.
        gpu::StageInputs neighbours;
        neighbours.neighbors = state;
        const StrandsPtr made = gpu::runStage(gpu::StageKind::BlendNeighbors, params(), in, scalp, std::move(neighbours));
        remember(made, std::move(state));
        return made;
    }

private:
    // The last result (by identity), and the neighbours it was
    // blended with.
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        std::shared_ptr<const BlendNeighborsState> state;
    };
    void remember(const StrandsPtr& output, std::shared_ptr<const BlendNeighborsState> state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        kept_.output = output;
        kept_.state = std::move(state);
    }
    mutable std::mutex keptMutex_;
    mutable Kept kept_;
};

// RestoreLength: brings each strand's length back from the `reference`
// strand with the same ID (from before the deformers that stretched it).
// `segments` mode restores every segment's length walking from the root
// along the deformed directions (follow the leader); `scale` scales the
// whole strand about its root to the reference's arc length. Strands with
// no reference are left alone. Without a reference, `even` respaces each
// strand's own points evenly by arc length, keeping its length.
class RestoreLengthOp : public Operator {
public:
    std::string typeName() const override { return op::kRestoreLength; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Curves}, {"reference", PortType::Curves, true}};
    }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"mode", std::string("segments"),
             "segments: each segment's length from the reference, along the deformed directions; scale: the whole "
             "strand scaled about its root to the reference's length; even: the strand's own points respaced "
             "evenly (no reference needed)",
             "", {"segments", "scale", "even"}},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand is restored from itself and the reference curve of its ID: only the strands the
    // input changed in are restored again, with the same reference (unchanged), or with reference
    // strands that changed in some strands (their own strands restored again too) when both hold
    // the same IDs in the same order, each once.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the inputs changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& /*ctx*/, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kRestoreLength);
        // The input handed on (nothing to restore): the graph passes its changes on.
        auto handOn = [&] {
            if (incremental) return false;
            *result = in;
            return true;
        };
        const std::string mode = params().getString("mode");
        if (mode != "segments" && mode != "scale" && mode != "even") {
            throw GraphError("RestoreLength: mode must be segments, scale or even");
        }
        if (in->empty()) return handOn();
        const bool even = mode == "even";

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the inputs changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last) return false;
            // Evenly, no reference is read; else one that changed in some strands is followed below.
            if (!even && !inputUnchanged(*incremental, 1) && !someStrandsChanged(*incremental, 1)) return false;
            if (changes->none() && (even || inputUnchanged(*incremental, 1))) {  // the same in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }

        // look.slang's RestoreLengthCS, the GPU preview's own (look_cpu.h), on its reference (strands,
        // or guides fed to the Curves input; by ID): in full, or the strands to restore again.
        const StrandsPtr refStrands = inputs.size() > 1 ? valueAs<StrandsPtr>(inputs[1]) : nullptr;
        const GuidesPtr refGuides = inputs.size() > 1 ? valueAs<GuidesPtr>(inputs[1]) : nullptr;
        gpu::StageInputs ref;
        if (!last) {
            if (!even) {
                bool found = false;
                ref.restore = gpu::restoreInputs(*in, refStrands.get(), refGuides.get(), nullptr, &found);
                if (!found) return handOn();  // nothing to restore from
            }
            *result = gpu::runStage(gpu::StageKind::RestoreLength, params(), in, nullptr, std::move(ref));
            return true;
        }
        // Incrementally: the strands the input changed in, and those whose reference strand
        // changed. A reference strand stands for the input's strand of its index when both hold
        // the same IDs in the same order and every reference strand is one (two points or more,
        // its ID its own).
        std::vector<std::uint32_t> remade;
        const StrandChanges* refChanges = even ? nullptr : someStrandsChanged(*incremental, 1);
        if (refChanges && !refChanges->none()) {
            if (!refStrands || refStrands->ids() != in->ids()) return false;
            std::unordered_set<StrandId> ids;
            for (std::size_t s = 0; s < refStrands->strandCount(); ++s) {
                if (refStrands->strandPointCount(s) < 2 || !ids.insert(refStrands->ids()[s]).second) return false;
            }
            std::set_union(changes->strands.begin(), changes->strands.end(), refChanges->strands.begin(), refChanges->strands.end(),
                           std::back_inserter(remade));
        } else {
            remade = changes->strands;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        if (!even) ref.restore = gpu::restoreInputs(*in, refStrands.get(), refGuides.get(), nullptr, nullptr, &remade);
        gpu::runStageOnStrands(gpu::StageKind::RestoreLength, params(), in, remade, nullptr, *out, std::move(ref));
        *changed = StrandChanges::listed(std::move(remade));
        *result = StrandsPtr(out);
        return true;
    }
};

}  // namespace

bool blendNeighborsState(const Operator& op, const StrandsPtr& output,
                         std::shared_ptr<const BlendNeighborsState>* state) {
    const auto* blend = dynamic_cast<const BlendNeighborsOp*>(&op);
    return blend && blend->keptState(output, state);
}

void registerBlendNeighborsOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBlendNeighbors,
                          {"deform",
                           "Pulls each strand toward its neighbours' shape for a coherent flow.",
                           "coherent flow neighbours"},
                          [] { return std::make_unique<BlendNeighborsOp>(); });
}

void registerRestoreLengthOperator(OperatorRegistry& registry) {
    registry.registerType(op::kRestoreLength,
                          {"shape", "Gives strands their length back after deformers.", "preserve length stretch"},
                          [] { return std::make_unique<RestoreLengthOp>(); });
}

}  // namespace groomlab
