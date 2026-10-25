// SPDX-License-Identifier: Apache-2.0
// Subdivide: smooth strands from few points. Each level doubles every strand's segments with a
// smooth curve through its points (centripetal Catmull-Rom: curve.h's smoothPolyline, no loops or
// overshoot), so the strand keeps its shape and its corners round off; widths and every per-point
// channel follow linearly. Unlike Resample it asks nothing of the Scatters before it: the nodes
// between grow, clump and curl the few points, and only what comes after it (best: nothing, it goes
// last) pays for the many. Its levels for renders and exports, and fewer for the viewport (as Maya's
// smooth mesh preview's display and render levels).

#include <memory>
#include <string>
#include <vector>

#include "groomlab/curve.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr std::int64_t kMaxLevels = 4;  // 16 pieces a segment

class SubdivideOp : public Operator {
public:
    std::string typeName() const override { return op::kSubdivide; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"levels", std::int64_t{2}, "Renders and exports: each level doubles the segments, a smooth curve "
                                                      "through the points (0: as they come)"})
                .range(0, kMaxLevels),
            notAnimatable({"viewportLevels", std::int64_t{1}, "The viewport's levels (fewer draws faster; while a param is "
                                                              "dragged none, so the GPU previews the nodes before it)"})
                .range(0, kMaxLevels),
        };
    }

    // The levels this evaluation subdivides by (the viewport's or the render's; none while a host
    // drags a param in the viewport: EvalContext::liveDrag).
    int levelsFor(const EvalContext& ctx) const {
        if (ctx.viewport && ctx.liveDrag) return 0;
        const std::int64_t levels = params().getInt(ctx.viewport ? "viewportLevels" : "levels");
        return static_cast<int>(std::clamp<std::int64_t>(levels, 0, kMaxLevels));
    }
    std::uint64_t contextSalt(const EvalContext& ctx) const override { return hash64(0x5DB0u + static_cast<std::uint64_t>(levelsFor(ctx))); }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kSubdivide);
        const int levels = levelsFor(ctx);
        if (levels == 0 || in->strandCount() == 0) return in;
        const std::size_t pieces = std::size_t{1} << levels;
        // The new layout: a strand of n points (3 or more) gets (n - 1) * pieces + 1; shorter ones stay.
        const std::size_t count = in->strandCount();
        std::vector<std::size_t> points(count);
        for (std::size_t s = 0; s < count; ++s) {
            const std::size_t n = in->strandPointCount(s);
            points[s] = n < 3 ? n : (n - 1) * pieces + 1;
        }
        auto out = std::make_shared<StrandSet>();
        out->resize(points, StrandSet::kLeavePositions);
        out->mutableIds() = in->ids();
        out->mutableRoots() = in->roots();
        std::vector<std::vector<float>*> outPoint;
        std::vector<const std::vector<float>*> inPoint;  // looked up once, not per strand
        for (const std::string& name : in->pointChannelNames()) {
            outPoint.push_back(&out->addPointChannel(name, in->pointChannelDefault(name)));
            inPoint.push_back(in->pointChannel(name));
        }
        for (const std::string& name : in->strandChannelNames()) {
            out->addStrandChannel(name, in->strandChannelDefault(name)) = *in->strandChannel(name);
        }
        const bool widths = in->widths().size() == in->pointCount();
        std::vector<float>& outWidths = out->mutableWidths();
        parallelFor(0, count, [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s), m = points[s];
            const std::size_t from = in->strandBegin(s), to = out->strandBegin(s);
            Vec3f* dst = out->strandPoints(s);
            // Linear between the input's points: point i of the input is point i * pieces of the result.
            auto follow = [&](const float* values, float* into) {
                if (m == n) {
                    std::copy(values, values + n, into);
                    return;
                }
                for (std::size_t j = 0; j < m; ++j) {
                    const std::size_t i = std::min(j / pieces, n - 2);
                    const float t = static_cast<float>(j - i * pieces) / static_cast<float>(pieces);
                    into[j] = values[i] * (1.0f - t) + values[i + 1] * t;
                }
            };
            if (m == n) {
                std::copy(in->strandPoints(s), in->strandPoints(s) + n, dst);
            } else {
                const std::vector<Vec3f> smooth = smoothPolyline(in->strandPoints(s), n, static_cast<int>(pieces));
                std::copy(smooth.begin(), smooth.end(), dst);
            }
            if (widths) follow(in->widths().data() + from, outWidths.data() + to);
            for (std::size_t c = 0; c < outPoint.size(); ++c) follow(inPoint[c]->data() + from, outPoint[c]->data() + to);
        });
        out->setInstances(in->instances());
        for (const auto& layer : in->cardLayers()) out->addCardLayer(layer);
        for (const auto& layer : in->featherLayers()) out->addFeatherLayer(layer);
        return StrandsPtr(out);
    }
};

}  // namespace

void registerSubdivideOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSubdivide,
                          {"shape", "Smooth strands from few points: each level doubles the segments along a smooth curve "
                                    "through them. Put it last.",
                           "smooth curves catmull subdivision render resolution points"},
                          [] { return std::make_unique<SubdivideOp>(); });
}

}  // namespace groomlab
