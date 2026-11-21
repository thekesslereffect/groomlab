// SPDX-License-Identifier: Apache-2.0
// SmoothChannel: a point channel smoothed along each strand. Each point's value becomes the average
// of the strand's values about `length` either side of it by arc length (three box averages, which
// come to a Gaussian), the window cut where the strand ends, so a value that rises and falls over a
// short stretch (a Braid's braidPress at each contact) fades in and out over a longer one. `strength`
// mixes the smoothed value with the one it had. A mask read by a later node (a Clump's strength
// mapped by 1 - $braidPress) then changes gently along the hair instead of kinking it. A strand
// channel, or none of the name, is handed on as it is.

#include <algorithm>
#include <memory>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "internal.h"

namespace groomlab {

namespace {

// One box average of `in` (n values on points at arc lengths `at`) into `out`: each point the mean
// of the values within `half` of it along the strand, each weighted by the length its point
// stands for (half of each segment beside it).
void boxAlong(const float* in, const float* at, const float* weight, std::size_t n, float half, float* out) {
    std::size_t first = 0, last = 0;  // the window [first, last)
    double sum = 0.0, total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        while (last < n && at[last] <= at[i] + half) {
            sum += static_cast<double>(in[last]) * weight[last];
            total += weight[last];
            ++last;
        }
        while (first < last && at[first] < at[i] - half) {
            sum -= static_cast<double>(in[first]) * weight[first];
            total -= weight[first];
            ++first;
        }
        out[i] = total > 0.0 ? static_cast<float>(sum / total) : in[i];
    }
}

class SmoothChannelOp : public Operator {
public:
    std::string typeName() const override { return op::kSmoothChannel; }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    PortSpec output() const override { return {"out", PortType::Strands}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }  // maps on length and strength

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"channel", std::string("braidPress"),
                      "The point channel smoothed along each strand (a Braid's braidPress, braidTail, a painted or "
                      "written one); a strand channel or none of the name: the strands handed on as they are"}
                .readsChannel(),
            ParamSpec{"length", 1.0, "How far along the strand the values are averaged, about this much either side of each "
                                     "point, scene units: larger, the softer the changes (mappable)",
                      "length"}
                .range(0, 1e9)
                .soft(0, 5),
            ParamSpec{"strength", 1.0, "How far toward the smoothed values, 0-1 (mappable)", "fraction"}.range(0, 1),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kSmoothChannel);
        const std::string name = params().getString("channel");
        // The length and strength, read at each strand (mappable).
        const ScalarField lengthField(params(), "length", scalpOf(ctx)), strengthField(params(), "strength", scalpOf(ctx));
        const std::vector<float>* values = name.empty() ? nullptr : in->pointChannel(name);
        if (!values || values->size() != in->pointCount()) return in;
        lengthField.validate(*in);
        strengthField.validate(*in);
        const bool mapped = !lengthField.isConstant() || !strengthField.isConstant();
        if (!mapped && (!(lengthField.at(*in, 0) > 0.0f) || !(std::clamp(strengthField.at(*in, 0), 0.0f, 1.0f) > 0.0f))) return in;

        auto out = std::make_shared<StrandSet>(*in);  // shares the input's arrays until written
        std::vector<float>& smoothed = *out->mutablePointChannel(name);
        parallelFor(0, out->strandCount(), [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s), begin = in->strandBegin(s);
            if (n < 2) return;
            const float length = lengthField.at(*in, s);
            const float strength = std::clamp(strengthField.at(*in, s), 0.0f, 1.0f);
            if (!(length > 0.0f) || !(strength > 0.0f)) return;
            // (three boxes, each about the length wide in all: a Gaussian of about half the length's deviation)
            const float half = 0.5f * length;
            thread_local std::vector<float> at, weight, a, b;
            at.resize(n);
            weight.resize(n);
            a.assign(values->begin() + static_cast<std::ptrdiff_t>(begin), values->begin() + static_cast<std::ptrdiff_t>(begin + n));
            b.resize(n);
            const Vec3f* p = in->strandPoints(s);
            at[0] = 0.0f;
            for (std::size_t i = 1; i < n; ++i) at[i] = at[i - 1] + (p[i] - p[i - 1]).length();
            for (std::size_t i = 0; i < n; ++i) {
                const float before = i > 0 ? at[i] - at[i - 1] : 0.0f, after = i + 1 < n ? at[i + 1] - at[i] : 0.0f;
                weight[i] = 0.5f * (before + after);
            }
            if (!(at[n - 1] > 0.0f)) return;  // (a strand of one place: nothing to smooth along)
            boxAlong(a.data(), at.data(), weight.data(), n, half, b.data());
            boxAlong(b.data(), at.data(), weight.data(), n, half, a.data());
            boxAlong(a.data(), at.data(), weight.data(), n, half, b.data());
            const float* was = values->data() + begin;
            float* to = smoothed.data() + begin;
            for (std::size_t i = 0; i < n; ++i) to[i] = was[i] + (b[i] - was[i]) * strength;
        });
        return StrandsPtr(out);
    }
};

}  // namespace

void registerSmoothChannelOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSmoothChannel,
                          {"utility",
                           "Smooths a point channel along each strand (a Braid's braidPress before a Clump's strength reads it), "
                           "so a mask changes gently along the hair.",
                           "blur soften channel attribute mask along braidPress"},
                          [] { return std::make_unique<SmoothChannelOp>(); });
}

}  // namespace groomlab
