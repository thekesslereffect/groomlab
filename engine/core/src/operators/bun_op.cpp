// SPDX-License-Identifier: Apache-2.0
// Bun: the paths a Braid winds hair along to make buns. Each guide fed in places one bun: its root
// where the bun sits on the scalp, its way out (root to tip, or the scalp's normal there: `axis`)
// the bun's axis. Its path rises from the root up the bun's middle (`lift`: the stem the hair is
// gathered into, as a ponytail's base), then winds round that middle `turns` times:
//
//   coil: round the stem at `radius`, each turn `rise` higher and the last `shrink` as wide as the
//         first (a wrapped bun, a top knot; a rope Braid: a twisted bun, bantu knots);
//   flat: outward in a flat spiral from the stem to `radius`, lying low (a snail or cinnamon bun);
//
// and, with `tuck`, its end turns in under the coil toward the middle, hidden as a pinned end is.
// A Braid reading these paths (its braids input) gathers the hair rooted within its `region` of
// each bun into the stem and plaits or twists it along the coil, its length fitted to the path:
// Add > Bun makes the two (graph_inserts.h insertBun). Each path keeps its guide's root and an ID
// of its own from the guide's, so a bun's braid stays the same braid when others are added.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kPerTurn = 48;  // path points a turn (the Braid resamples it evenly)

float smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

class BunOp : public Operator {
public:
    std::string typeName() const override { return op::kBun; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"guides", PortType::Guides}}; }
    PortSpec output() const override { return {"paths", PortType::Guides}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }  // (axis normal; maps)

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"style", std::string("coil"),
             "coil: wound round its stem, turn on turn (a wrapped bun, a top knot); flat: a flat spiral out from the "
             "stem, lying low (a snail bun)",
             "",
             {"coil", "flat"}},
            ParamSpec{"radius", 2.0, "The bun's size: coil, how far its turns lie from the middle; flat, the outer turn's "
                                     "(mappable, per bun: at its root)", "length"}
                .atLeast(0)
                .soft(0, 6),
            ParamSpec{"turns", 2.5, "How many times the hair winds round (mappable, per bun)"}.atLeast(0.25).soft(0.5, 6),
            ParamSpec{"rise", 1.0, "How much higher each turn of a coil lies than the one before: about the braid's "
                                   "width, so the turns sit on each other (mappable, per bun)", "length"}
                .atLeast(0)
                .soft(0, 3)
                .when("style", {"coil"}),
            ParamSpec{"shrink", 0.7, "A coil's last turn's size against its first: below 1 a dome, 1 a drum", "fraction"}
                .range(0.1, 1.5)
                .when("style", {"coil"}),
            ParamSpec{"lift", 0.5, "How far up the middle the hair rises before it winds round: the stem it is gathered "
                                   "into (mappable, per bun)", "length"}
                .atLeast(0)
                .soft(0, 3),
            {"tuck", true, "The end turns in under the coil toward the middle, hidden"},
            {"clockwise", false, "Winds clockwise looking down the axis at the bun (else counterclockwise)"},
            ParamSpec{"startAngle", 0.0, "Where round the middle the hair starts winding, degrees (mappable, per bun)", "angle"}
                .soft(-180, 180)
                .adv(),
            {"axis", std::string("guide"),
             "The bun's axis: guide, along each guide (root to tip); normal, out of the scalp at its root", "",
             {"guide", "normal"}},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const GuidesPtr in = inputs.empty() ? nullptr : valueAs<GuidesPtr>(inputs[0]);
        const ScalpMesh* scalp = scalpOf(ctx);
        const bool flat = params().getString("style") == "flat";
        const bool tuck = params().getBool("tuck");
        const float sign = params().getBool("clockwise") ? -1.0f : 1.0f;
        const bool alongNormal = params().getString("axis") == "normal";
        const float shrink = static_cast<float>(params().getDouble("shrink"));
        auto out = std::make_shared<GuideSet>();
        if (!in) return GuidesPtr(out);
        // The mappable params, read per bun at its guide's root.
        const ScalarField radiusField(params(), "radius", scalp, in.get());
        const ScalarField turnsField(params(), "turns", scalp, in.get());
        const ScalarField riseField(params(), "rise", scalp, in.get());
        const ScalarField liftField(params(), "lift", scalp, in.get());
        const ScalarField angleField(params(), "startAngle", scalp, in.get());
        for (std::size_t gi = 0; gi < in->guides().size(); ++gi) {
            const Guide& g = in->guides()[gi];
            if (g.points.empty()) continue;
            const Vec3f root = g.points.front();
            Vec3f axis = g.points.size() > 1 ? g.points.back() - root : Vec3f{};
            if ((alongNormal || axis.length() < 1e-6f) && scalp && scalp->contains(g.root)) axis = scalp->normal(g.root);
            if (axis.length() < 1e-6f) axis = {0.0f, 1.0f, 0.0f};
            axis = axis.normalized();
            const float radius = std::max(radiusField.atRoot(g.root), 0.0f);
            const float turns = std::max(turnsField.atRoot(g.root), 0.25f);
            const float rise = std::max(riseField.atRoot(g.root), 0.0f);
            const float lift = std::max(liftField.atRoot(g.root), 0.0f);
            const float start = angleField.atRoot(g.root) * kPi / 180.0f;
            // The bun's own frame: u, v across the axis (u turned to the start angle), w the axis.
            const Vec3f p0 = anyPerpendicular(axis);
            const Vec3f q0 = axis.cross(p0);
            const Vec3f u = p0 * std::cos(start) + q0 * std::sin(start);
            const Vec3f v = axis.cross(u) * sign;
            auto at = [&](float r, float a, float h) { return root + (u * std::cos(a) + v * std::sin(a)) * r + axis * h; };

            std::vector<Vec3f> pts;
            // The stem: straight up the middle (a few points; the Braid resamples).
            const int stemPoints = 6;
            for (int i = 0; i < stemPoints; ++i) pts.push_back(root + axis * (lift * static_cast<float>(i) / stemPoints));
            // The winding: out from the middle over the first quarter turn into the turns.
            const float total = 2.0f * kPi * turns;
            const int count = std::max(8, static_cast<int>(std::ceil(turns * kPerTurn)));
            float lastR = 0.0f, lastA = 0.0f, lastH = lift;
            for (int i = 0; i <= count; ++i) {
                const float s = static_cast<float>(i) / static_cast<float>(count);  // 0..1 along the winding
                const float a = total * s;
                const float out1 = smooth01(a / (0.5f * kPi));  // out from the middle over a quarter turn
                float r, h;
                if (flat) {
                    // Turns evenly spaced out to the radius, the first a spacing out from the middle.
                    const float spacing = radius / (turns + 1.0f);
                    r = (spacing + (radius - spacing) * s) * out1;
                    h = lift;
                } else {
                    r = radius * (1.0f + (shrink - 1.0f) * s) * out1;
                    h = lift + rise * (a / (2.0f * kPi));
                }
                if (i == 0 && stemPoints > 0) continue;  // (the stem's top is the winding's start)
                pts.push_back(at(r, a, h));
                lastR = r, lastA = a, lastH = h;
            }
            // The tuck: on round a third of a turn, in toward the middle and down to halfway up.
            if (tuck && radius > 0.0f) {
                const int tuckPoints = kPerTurn / 3;
                const float toH = flat ? lift : lift + 0.5f * (lastH - lift);
                for (int i = 1; i <= tuckPoints; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(tuckPoints);
                    const float e = smooth01(t);
                    pts.push_back(at(lastR * (1.0f - 0.85f * e), lastA + (2.0f * kPi / 3.0f) * t, lastH + (toH - lastH) * e));
                }
            }
            Guide& made = out->add(hashCombine(g.id, 0xB0B5ull), std::move(pts));
            made.root = g.root;
            made.attributes = g.attributes;
        }
        return GuidesPtr(out);
    }
};

}  // namespace

void registerBunOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBun,
                          {"look",
                           "Buns: each guide (where a bun sits, its axis) becomes a path winding round its stem, for a "
                           "Braid to gather the hair into and coil (Add > Bun).",
                           "bun top knot bantu knots chignon coil twist bun updo"},
                          [] { return std::make_unique<BunOp>(); });
}

}  // namespace groomlab
