// SPDX-License-Identifier: Apache-2.0
// Continuity: a small change of a randomness param gives a small change of the groom. Swept from
// 0 to past 1 (the overdrive) in steps of 0.01, Scatter keeps the same strands (IDs and count, the
// full set and a thinned preview's alike) and moves each root along its own path from its even
// spot to its random one, a step's worth at a time; a Clump picking its own centers keeps the same
// centers (count and identity: the clump numbers) and moves each a little, so a step changes the
// clump of only the few strands at the borders the moved centers shift. Nothing swaps or
// reshuffles at 0, at 1 or anywhere between.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr int kSteps = 100;  // 0 to 1 by 0.01
double stepValue(int k) { return static_cast<double>(k) / 100.0; }

struct ScatterRig {
    Graph graph;
    ExternalInputs externals;
    NodeId scatter = 0;
    explicit ScatterRig(MeshPtr scalp, double density) {
        externals.set("scalp", scalp, scalp->surfaceHash());
        const NodeId s = graph.addNode(op::kScalp);
        scatter = graph.addNode(op::kScatter);
        graph.node(scatter).params().set("density", density);
        graph.connect(s, scatter, 0);
    }
    StrandsPtr eval(double randomness, float previewDensity) {
        graph.node(scatter).params().set("randomness", randomness);
        EvalContext ctx;
        ctx.previewDensity = previewDensity;
        ctx.externals = &externals;
        return graph.evaluateStrands(scatter, ctx);
    }
};

// Scatter swept over randomness: per step, whether the strands stayed (IDs and count) and the
// worst root movement over what the step allows: the step times the root's own way from its even
// spot (randomness 0) to its random one (1), and `slack` for a curved scalp's snap onto it.
void sweepScatter(MeshPtr scalp, double density, float previewDensity, float slack, const char* what) {
    ScatterRig rig(scalp, density);
    const StrandsPtr even = rig.eval(0.0, previewDensity);
    const StrandsPtr random = rig.eval(1.0, previewDensity);
    ASSERT_TRUE(even && random);
    ASSERT_GT(even->strandCount(), 100u) << what;
    EXPECT_EQ(even->ids(), random->ids()) << what;
    std::vector<float> way(even->strandCount());
    float widest = 0.0f;
    for (std::size_t i = 0; i < way.size(); ++i) {
        way[i] = (random->strandPoints(i)[0] - even->strandPoints(i)[0]).length();
        widest = std::max(widest, way[i]);
    }
    const float spacing = 1.0746f / std::sqrt(static_cast<float>(density) * previewDensity);
    StrandsPtr last = rig.eval(0.0, previewDensity);
    std::size_t idChanges = 0;
    double worstSwap = 0.0;  // the share of the strands a step swapped for others, at worst
    float worst = 0.0f, worstMove = 0.0f;
    double worstAt = 0.0;
    for (int k = 1; k <= kSteps; ++k) {
        const StrandsPtr now = rig.eval(stepValue(k), previewDensity);
        ASSERT_TRUE(now);
        if (now->ids() != last->ids()) {
            ++idChanges;
            const std::set<StrandId> before(last->ids().begin(), last->ids().end());
            std::size_t fresh = 0;
            for (const StrandId id : now->ids()) fresh += before.count(id) ? 0 : 1;
            worstSwap = std::max(worstSwap, static_cast<double>(fresh) / static_cast<double>(now->strandCount()));
            last = now;
            continue;
        }
        for (std::size_t i = 0; i < now->strandCount(); ++i) {
            const float moved = (now->strandPoints(i)[0] - last->strandPoints(i)[0]).length();
            const float allowed = 0.01f * way[i] * (1.0f + slack) + 1e-5f * spacing;
            if (moved / allowed > worst) {
                worst = moved / allowed;
                worstAt = stepValue(k);
            }
            worstMove = std::max(worstMove, moved);
        }
        last = now;
    }
    std::printf("[ continuity ] Scatter %s (%zu strands): %zu of %d steps changed the strands (%.1f%% swapped at worst); worst step moved a root "
                "%.3f of its allowance (at %.2f), %.4f spacings at most (widest way %.2f spacings)\n",
                what, even->strandCount(), idChanges, kSteps, 100.0 * worstSwap, worst, worstAt, worstMove / spacing, widest / spacing);
    EXPECT_EQ(idChanges, 0u) << what;
    EXPECT_LE(worst, 1.0f) << what << " at " << worstAt;
    // Every root moved in steps of a hundredth of its way at most: far below a spacing.
    EXPECT_LT(worstMove, 0.05f * spacing) << what;
    // Past 1 (its most): as at 1.
    const StrandsPtr over = rig.eval(1.2, previewDensity);
    for (std::size_t i = 0; i < over->strandCount(); ++i) EXPECT_EQ(over->strandPoints(i)[0], random->strandPoints(i)[0]) << what;
}

TEST(Continuity, ScatterRandomnessMovesTheSameRootsSmoothlyFromEvenToRandom) {
    sweepScatter(test::makePlane(8, 1.0f), 2000.0, 1.0f, 0.0f, "plane, full");
    sweepScatter(test::makePlane(8, 1.0f), 2000.0, 0.2f, 0.0f, "plane, preview 0.2");
    sweepScatter(test::makeSphere(24, 48, 0.5f), 2000.0, 1.0f, 0.5f, "sphere, full");
    sweepScatter(test::makeSphere(24, 48, 0.5f), 2000.0, 0.2f, 0.5f, "sphere, preview 0.2");
}

// A Clump picking its own centers swept over randomness: the centers' count and identity (each
// strand's clump number, from its center's ID) stay, and each step moves a strand to another clump
// only where a moved center shifts a border.
void sweepClump(MeshPtr scalp, const char* clumpBy, const char* what) {
    Graph graph;
    ExternalInputs externals;
    externals.set("scalp", scalp, scalp->surfaceHash());
    const NodeId s = graph.addNode(op::kScalp);
    const NodeId scatter = graph.addNode(op::kScatter);
    graph.node(scatter).params().set("density", 20000.0 / scalp->restTotalArea());
    graph.connect(s, scatter, 0);
    const NodeId clump = graph.addNode(op::kClump);
    graph.connect(scatter, clump, 0);
    graph.node(clump).params().set("clumpBy", std::string(clumpBy));
    graph.node(clump).params().set("count", std::int64_t{60});
    graph.node(clump).params().set("size", std::sqrt(static_cast<double>(scalp->restTotalArea()) / 60.0));
    auto eval = [&](double randomness, std::size_t* centers) {
        graph.node(clump).params().set("randomness", randomness);
        EvalContext ctx;
        ctx.externals = &externals;
        const StrandsPtr out = graph.evaluateStrands(clump, ctx);
        ClumpState state;
        *centers = clumpState(graph.node(clump), out, &state) ? state.chosen.size() : 0;
        const std::vector<float>* numbers = out->strandChannel("clumpId");
        return numbers ? *numbers : std::vector<float>();
    };
    std::size_t firstCount = 0;
    std::vector<float> last = eval(0.0, &firstCount);
    ASSERT_FALSE(last.empty());
    ASSERT_GT(firstCount, 40u);
    std::size_t countChanges = 0;
    double worstShare = 0.0, worstAt = 0.0, total = 0.0;
    for (int k = 1; k <= kSteps; ++k) {
        std::size_t count = 0;
        const std::vector<float> now = eval(stepValue(k), &count);
        ASSERT_EQ(now.size(), last.size());
        countChanges += count != firstCount ? 1 : 0;
        std::size_t changed = 0;
        for (std::size_t i = 0; i < now.size(); ++i) changed += now[i] != last[i] ? 1 : 0;
        const double share = static_cast<double>(changed) / static_cast<double>(now.size());
        total += share;
        if (share > worstShare) {
            worstShare = share;
            worstAt = stepValue(k);
        }
        last = now;
    }
    std::printf("[ continuity ] Clump %s (%zu centers): %zu of %d steps changed the count; a step moved %.2f%% of the "
                "strands to another clump at worst (at %.2f), %.2f%% on average\n",
                what, firstCount, countChanges, kSteps, 100.0 * worstShare, worstAt, 100.0 * total / kSteps);
    EXPECT_EQ(countChanges, 0u) << what;
    // Each center's spot moves a hundredth of its way per step (its way up to a spacing): the
    // borders between clumps shift as little, a strand at a time; none swaps its clump wholesale.
    EXPECT_LT(worstShare, 0.025) << what << " at " << worstAt;
    EXPECT_LT(total / kSteps, 0.012) << what;
}

TEST(Continuity, ClumpRandomnessMovesTheSameCentersALittleAtATime) {
    sweepClump(test::makePlane(8, 1.0f), "count", "plane, by count");
    sweepClump(test::makeSphere(24, 48, 0.5f), "size", "sphere, by size");
}

// Tangle's partners: a small change of the radius adds or drops a few candidates; a strand changes
// partner only when the one it picks comes or goes (or the newcomer draws lower), not wherever its
// list of candidates changed.
TEST(Continuity, TangleChangesFewPartnersForASmallChangeOfItsRadius) {
    Graph graph;
    ExternalInputs externals;
    const MeshPtr plane = test::makePlane(8, 1.0f);
    externals.set("scalp", plane, plane->surfaceHash());
    const NodeId s = graph.addNode(op::kScalp);
    const NodeId scatter = graph.addNode(op::kScatter);
    graph.node(scatter).params().set("density", 4000.0);
    graph.node(scatter).params().set("length", 0.3);
    graph.connect(s, scatter, 0);
    const NodeId tangle = graph.addNode(op::kTangle);
    graph.connect(scatter, tangle, 0);
    graph.node(tangle).params().set("fraction", 1.0);
    graph.node(tangle).params().set("radius", 0.1);
    graph.node(tangle).params().set("reach", 0.0);
    auto eval = [&](const char* param, double value) {
        graph.node(tangle).params().set(param, value);
        EvalContext ctx;
        ctx.externals = &externals;
        return graph.evaluateStrands(tangle, ctx);
    };
    auto changedShare = [](const StrandsPtr& a, const StrandsPtr& b) {
        std::size_t changed = 0;
        for (std::size_t i = 0; i < a->strandCount(); ++i) {
            const std::size_t n = a->strandPointCount(i);
            changed += (a->strandPoints(i)[n - 1] - b->strandPoints(i)[n - 1]).length() > 1e-6f ? 1 : 0;
        }
        return static_cast<double>(changed) / static_cast<double>(a->strandCount());
    };
    const StrandsPtr base = eval("radius", 0.1);
    const StrandsPtr wider = eval("radius", 0.101);
    const double radiusShare = changedShare(base, wider);
    std::printf("[ continuity ] Tangle: radius 0.100 -> 0.101 changed %.1f%% of the strands\n", 100.0 * radiusShare);
    EXPECT_LT(radiusShare, 0.02);
}

}  // namespace
}  // namespace groomlab
