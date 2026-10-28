// SPDX-License-Identifier: Apache-2.0
// RenderData: per-strand render controls for the hair shader beside the look (HairColor's color and
// highlight, which it leaves to it): opacity along each strand (mappable per strand, times a root
// to tip profile, with a random share per strand hashed from its stable ID). Written as a channel
// (export_view.h kOpacityChannel, per point) that Arnold reads as user data (create_hair_shader
// wires it) and Blender as an attribute. A later RenderData's values replace an earlier one's.

#include <algorithm>
#include <memory>
#include <vector>

#include "groomlab/export_view.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

class RenderDataOp : public Operator {
public:
    std::string typeName() const override { return op::kRenderData; }
    std::vector<ChannelWrite> channelWrites() const override { return {{kOpacityChannel, ""}}; }
    std::string category() const override { return "color"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"opacity", 1.0, "How opaque each strand is, 0-1 (mappable), times opacityProfile", "fraction"}
                .range(0, 1)
                .in("Opacity"),
            ParamSpec{"opacityProfile", Ramp::constant(1.0f), "Opacity along the strand, root to tip, times opacity"}.in("Opacity"),
            ParamSpec{"opacityRandom", 0.0, "Each strand up to this share less opaque, at random", "fraction"}.range(0, 1).in("Opacity"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand's values come from its ID and the map at it: with a map that reads no points, a
    // dab changes no value (the last result's channel on the new points); with one that reads each
    // strand alone, only the strands it moved are written again.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kRenderData);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField opacity(params(), "opacity", scalp);
        opacity.validate(*in);
        const bool local = opacity.strandLocal(), readsPoints = opacity.readsPositions();
        const Ramp profile = params().getRamp("opacityProfile");
        const auto opacityRandom = static_cast<float>(params().getDouble("opacityRandom"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result: its channel, for the strands no change reaches
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !local) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        std::vector<float>* opacities = nullptr;
        if (last) {
            opacities = out->copyPointChannelFrom(kOpacityChannel, *last);
            if (!opacities) return false;
        } else {
            opacities = &out->addPointChannel(kOpacityChannel, 1.0f);
        }
        const RampAlong along(profile, *out);

        const bool perPoint = opacity.perPoint();  // (a field per point read at each point, as Bands reads it)
        auto write = [&](std::size_t s) {
            float strand = perPoint ? 1.0f : std::clamp(opacity.at(*out, s), 0.0f, 1.0f);
            if (opacityRandom > 0.0f) strand *= 1.0f - opacityRandom * unitFloat(hashCombine(out->ids()[s], seed ^ 0x0BAC17ull));
            const std::size_t begin = out->strandBegin(s), n = out->strandPointCount(s);
            for (std::size_t i = 0; i < n; ++i) {
                const float here = perPoint ? std::clamp(opacity.atPoint(*out, s, i), 0.0f, 1.0f) : 1.0f;
                (*opacities)[begin + i] = std::clamp(strand * here * along(i, n), 0.0f, 1.0f);
            }
        };
        if (last) {
            if (readsPoints) forEachListed(changes->strands, write);  // else every value stays
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), write);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

}  // namespace

void registerRenderDataOperator(OperatorRegistry& registry) {
    registry.registerType(op::kRenderData,
                          {"color", "Render controls per strand for the shader: opacity along the strands.", "opacity shader"},
                          [] { return std::make_unique<RenderDataOp>(); });
}

}  // namespace groomlab
