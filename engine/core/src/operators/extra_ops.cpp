// SPDX-License-Identifier: Apache-2.0
// Shaping operators that round out the look set: Twist (strands turned around
// an axis: their guide, or their clump), Smooth (bumps up to a share of the
// length taken out, at 1 straightened; lengths kept) and Cull (thin strands out
// by a map); and ToStrands (guides as strands).

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#if defined(_M_X64) || defined(__SSE2__)
#define GROOMLAB_SMOOTH_SSE2 1
#include <emmintrin.h>
#else
#define GROOMLAB_SMOOTH_SSE2 0  // (WebAssembly: the scalar loop, the same bits)
#endif

#include "groomlab/curve.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

// Twist: each strand turns around an axis curve, `turns` times per unit of the
// axis' length, building up along `profile`. The axis is the nearest curve of
// the `axes` input (guides: a lock's guide, braid paths, ToGuides output), or,
// with nothing connected, the middle of the strand's clump (the mean of the
// strands sharing `channel`, written by Clump): Clump -> Twist makes ropes.
class TwistOp : public Operator {
public:
    std::string typeName() const override { return op::kTwist; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Curves}, {"axes", PortType::Guides, true}};
    }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"turns", 1.0, "Turns around the axis per unit of its length; negative turns the other way (mappable)",
                      "perLength"}
                .soft(-2, 2),
            {"profile", Ramp::constant(1.0f), "How much of the twist along the length, root to tip"},
            ParamSpec{"clumpChannel", std::string("clumpId"),
                      "With nothing on the axes input: strands sharing this channel (a Clump's clumpChannel) twist around "
                      "their core strands' mean (those every preview holds). Unused while axes are connected"}
                .adv()
                .readsChannel(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Around axes, each strand turns around the axis nearest its root: with the same axes in, only
    // the strands the input changed in are turned again. Around its clump's middle, a strand's
    // axis is the mean of its clump, which any member moves: in full.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kTwist);
        // The input handed on (nothing to twist): the graph passes its changes on.
        auto handOn = [&] {
            if (incremental) return false;
            *result = in;
            return true;
        };
        if (in->empty()) return handOn();
        const GuidesPtr axesIn = valueAs<GuidesPtr>(inputs.at(1));
        const ScalarField turns(params(), "turns", scalpOf(ctx));
        turns.validate(*in);
        const bool aroundAxes = axesIn && !axesIn->empty();

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !aroundAxes || !inputUnchanged(*incremental, 1) || !turns.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }

        // look.slang's TwistCS (around clumps: TwistAxesCS making their middles first), the GPU
        // preview's own (look_cpu.h): in full, or the strands the input changed in (their axes).
        const std::string channel = params().getString("clumpChannel");
        if (!aroundAxes && !in->strandChannel(channel)) {
            throw GraphError("Twist: connect axes (guides), or put a Clump before it (no '" + channel + "' channel to find the clumps)");
        }
        std::string whyNot;
        gpu::StageInputs axesOf;
        axesOf.twist = gpu::twistInputs(*in, axesIn.get(), channel, false, &whyNot, last ? &changes->strands : nullptr);
        if (!axesOf.twist) throw GraphError("Twist: " + whyNot);
        if (aroundAxes && axesOf.twist->count == 0) return handOn();  // (no axis of two points)
        if (!last) {
            *result = gpu::runStage(gpu::StageKind::Twist, params(), in, scalpOf(ctx), std::move(axesOf));
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        gpu::runStageOnStrands(gpu::StageKind::Twist, params(), in, changes->strands, scalpOf(ctx), *out, std::move(axesOf));
        *changed = *changes;
        *result = StrandsPtr(out);
        return true;
    }
};

// Smooth: each strand toward its shape with the bumps up to `scale` of its length taken out (its
// direction along its length, by arc length, averaged by three box filters 2 x scale of its length
// wide, as a Gaussian of deviation scale x its length would, over the direction reflected at both
// ends: look.slang SmoothCS, look_stage_params.h smoothEnds): small scales take out kinks from noise, interpolation or sculpting, 1 straightens
// it along the line from its root to its tip. `strength` (with `profile` along the length) blends
// each point toward that shape, then the segment lengths are put back from the root out; the root
// never moves.
class SmoothOp : public Operator {
public:
    std::string typeName() const override { return op::kSmooth; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How far toward the smoothed shape, 0-1, lengths kept (mappable)", "fraction"}.range(0, 1),
            ParamSpec{"scale", 0.09, "The size of the bumps it takes out, as a share of the strand's length: small takes out "
                                    "kinks, 1 straightens the strand along the line from its root to its tip",
                      "fraction"}
                .range(0, 1),
            {"profile", Ramp::constant(1.0f), "Smoothing along the length, root to tip"},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kSmooth);
        const ScalarField strength(params(), "strength", scalpOf(ctx));
        strength.validate(*in);
        const auto scale = std::clamp(static_cast<float>(params().getDouble("scale")), 0.0f, 1.0f);
        if (!(scale > 0.0f) || in->empty()) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            *result = in;
            return true;
        }
        // look.slang's SmoothCS, the GPU preview's own (look_cpu.h): in full, or the strands the input
        // changed in, into the last result's copy.
        if (!incremental) {
            *result = gpu::runStage(gpu::StageKind::Smooth, params(), in, scalpOf(ctx));
            return true;
        }
        const StrandChanges* changes = someStrandsChanged(*incremental, 0);
        const StrandsPtr last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
        if (!last || !strength.strandLocal()) return false;
        if (changes->none()) {  // the same strands in: the same result
            *result = last;
            *changed = *changes;
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        gpu::runStageOnStrands(gpu::StageKind::Smooth, params(), in, changes->strands, scalpOf(ctx), *out);
        *changed = *changes;
        *result = StrandsPtr(out);
        return true;
    }
};

// Cull: keeps each strand with probability `keep` (mappable), decided by its
// ID, so thinning is stable and painting keep up only brings strands back.
class CullOp : public Operator {
public:
    std::string typeName() const override { return op::kCull; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"keep", 1.0, "Share of strands kept, 0-1 (mappable: paint thin or bald patches)", "fraction"})
                .soft(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kCull);
        const ScalarField keep(params(), "keep", scalpOf(ctx));
        keep.validate(*in);
        if (keep.isConstant() && keep.maximum() >= 1.0f) return in;
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        // Each strand's verdict in parallel, then the kept ones in strand order.
        std::vector<std::uint8_t> keeps(in->strandCount());
        parallelFor(0, in->strandCount(), [&](std::size_t s) { keeps[s] = isKept(*in, s, keep, seed) ? 1 : 0; });
        std::vector<std::size_t> kept;
        kept.reserve(in->strandCount());
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            if (keeps[s]) kept.push_back(s);
        }
        if (kept.size() == in->strandCount()) return in;
        auto out = StrandsPtr(std::make_shared<StrandSet>(in->subset(kept)));
        auto list = std::make_shared<std::vector<std::uint32_t>>(kept.begin(), kept.end());
        const std::lock_guard<std::mutex> lock(madeMutex_);
        made_ = {out, std::move(list)};
        return out;
    }
    // A dab changes some strands' points, not their IDs: the kept strands stay the same while the
    // changed strands' verdicts do (a keep that reads each strand alone), and each kept strand
    // changed is cut again over its place in the last result.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* result, StrandChanges* changed) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kCull);
        const StrandChanges* changes = someStrandsChanged(incremental, 0);
        const StrandsPtr last = valueAs<StrandsPtr>(incremental.previous);
        if (!changes || !last) return false;
        Made made;
        {
            const std::lock_guard<std::mutex> lock(madeMutex_);
            made = made_;
        }
        if (made.output.lock() != last || !made.kept || made.kept->size() != last->strandCount() ||
            (!made.kept->empty() && made.kept->back() >= in->strandCount())) {
            return false;
        }
        const ScalarField keep(params(), "keep", scalpOf(ctx));
        keep.validate(*in);
        if (!keep.strandLocal()) return false;
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        std::vector<std::size_t> pick;
        std::vector<std::uint32_t> at;
        for (const std::uint32_t s : changes->strands) {
            const auto it = std::lower_bound(made.kept->begin(), made.kept->end(), s);
            const bool was = it != made.kept->end() && *it == s;
            if (isKept(*in, s, keep, seed) != was) return false;  // the kept strands change: in full
            if (!was) continue;
            pick.push_back(s);
            at.push_back(static_cast<std::uint32_t>(it - made.kept->begin()));
        }
        if (at.empty()) {
            *result = last;
            *changed = StrandChanges::listed({});
            return true;
        }
        auto out = copyOfLast(last, incremental);
        writeStrands(*out, in->subset(pick), at);
        const StrandsPtr made2 = out;
        {
            const std::lock_guard<std::mutex> lock(madeMutex_);
            made_ = {made2, made.kept};
        }
        *result = made2;
        *changed = StrandChanges::listed(std::move(at));
        return true;
    }
    void adoptKeptWork(const Operator& previous) override {
        const auto* from = dynamic_cast<const CullOp*>(&previous);
        if (!from || from == this) return;
        Made made;
        {
            const std::lock_guard<std::mutex> lock(from->madeMutex_);
            made = from->made_;
        }
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (!made_.kept) made_ = std::move(made);
    }

private:
    static bool isKept(const StrandSet& in, std::size_t s, const ScalarField& keep, std::uint64_t seed) {
        return unitFloat(hashCombine(in.ids()[s], seed ^ 0xC011ull)) < keep.at(in, s);
    }
    // The last result cut (by identity) and the strands of its input it kept, in order.
    struct Made {
        std::weak_ptr<const StrandSet> output;
        std::shared_ptr<const std::vector<std::uint32_t>> kept;
    };
    mutable std::mutex madeMutex_;
    mutable Made made_;
};

// ToStrands: each guide becomes a strand (the reverse of ToGuides): render
// accent guides as hair, run look nodes on guides, or Merge them with
// scattered strands. Painted guide maps become strand channels, and layers
// painted along the guides point channels (resampled with the points).
class ToStrandsOp : public Operator {
public:
    std::string typeName() const override { return op::kToStrands; }
    // The guides' painted maps become channels when `channels` is on.
    bool carriesChannels(std::size_t slot) const override { return slot == 0 && params().getBool("channels"); }
    std::string category() const override { return "grow"; }
    std::vector<PortSpec> inputs() const override { return {{"guides", PortType::Guides}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"segments", std::int64_t{0}, "Segments per strand (0: each guide's own points)"})
                .range(0, 256)
                .soft(0, 32),
            ParamSpec{"width", 0.005, "Strand width until a Width node sets it", "width"}.atLeast(0).soft(0, 0.02),
            {"channels", true, "Carry the guides' painted maps over as strand channels"},
        };
    }

    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        const GuidesPtr guides = valueAs<GuidesPtr>(inputs.at(0));
        if (!guides) throw GraphError("ToStrands: guides input not connected");
        const auto segments = static_cast<std::size_t>(params().getInt("segments"));
        const float width = static_cast<float>(params().getDouble("width"));
        const bool channels = params().getBool("channels");
        auto out = std::make_shared<StrandSet>();
        std::vector<std::string> names, layers;
        if (channels) {
            for (const Guide& g : guides->guides()) {
                for (const auto& [name, value] : g.attributes) {
                    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
                }
                for (const auto& [name, values] : g.pointAttributes) {
                    if (!values.empty() && std::find(layers.begin(), layers.end(), name) == layers.end()) {
                        layers.push_back(name);
                    }
                }
            }
        }
        std::vector<std::vector<float>*> columns;
        for (const std::string& name : names) columns.push_back(&out->addStrandChannel(name, 0.0f));
        std::vector<std::vector<float>*> layerColumns;
        for (const std::string& name : layers) layerColumns.push_back(&out->addPointChannel(name, 0.0f));
        for (const Guide& g : guides->guides()) {
            if (g.points.size() < 2) continue;
            const std::vector<Vec3f> pts =
                segments > 0 ? resampleByArcLength(g.points.data(), g.points.size(), segments + 1) : g.points;
            // IDs apart from scattered strands' (Merge keeps both), stable per guide.
            const StrandId id = hashCombine(g.id, 0x7057A2D5ull);
            const std::size_t s = out->addStrand(id, pts, std::vector<float>(pts.size(), width), g.root);
            for (std::size_t c = 0; c < names.size(); ++c) {
                auto it = g.attributes.find(names[c]);
                (*columns[c])[s] = it == g.attributes.end() ? 0.0f : it->second;
            }
            // Each layer at the strand's points (resampled by arc length along the guide, as the
            // points are); a guide without it carries its attribute of that name, else 0 (as
            // Interpolate does).
            const std::size_t begin = out->strandBegin(s);
            for (std::size_t c = 0; c < layers.size(); ++c) {
                float* to = layerColumns[c]->data() + begin;
                auto it = g.pointAttributes.find(layers[c]);
                if (it != g.pointAttributes.end() && !it->second.empty() && segments == 0) {
                    const std::vector<float>& values = it->second;  // the guide's own points
                    for (std::size_t j = 0; j < pts.size(); ++j) to[j] = values[std::min(j, values.size() - 1)];
                } else if (it != g.pointAttributes.end() && !it->second.empty()) {
                    resampleValuesByArcLength(g.points.data(), g.points.size(), it->second.data(), it->second.size(), to,
                                              pts.size());
                } else {
                    auto scalar = g.attributes.find(layers[c]);
                    std::fill(to, to + pts.size(), scalar == g.attributes.end() ? 0.0f : scalar->second);
                }
            }
        }
        return StrandsPtr(out);
    }
};

}  // namespace

void registerToStrandsOperator(OperatorRegistry& registry) {
    registry.registerType(op::kToStrands,
                          {"convert", "Guides become strands: render them, or merge them in.", "guides to strands"},
                          [] { return std::make_unique<ToStrandsOp>(); });
}

void registerTwistOperator(OperatorRegistry& registry) {
    registry.registerType(op::kTwist,
                          {"look", "Twists strands around a lock or clump: ropes, twisted locks.", "rope twisted locks"},
                          [] { return std::make_unique<TwistOp>(); });
}

void registerSmoothOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSmooth,
                          {"shape",
                           "Takes out bumps up to a size (a share of the length; 1 straightens), keeping lengths and roots.",
                           "relax straighten bumps"},
                          [] { return std::make_unique<SmoothOp>(); });
}

void registerCullOperator(OperatorRegistry& registry) {
    registry.registerType(op::kCull,
                          {"grow", "Thins strands out: bald or thin patches.", "thin bald density remove patches"},
                          [] { return std::make_unique<CullOp>(); });
}

}  // namespace groomlab
