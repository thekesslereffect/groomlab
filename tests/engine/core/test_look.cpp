// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>

#include <nlohmann/json.hpp>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/strand_geometry.h"
#include "groomlab/brush.h"
#include "groomlab/cards.h"
#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/expression.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/hair_color.h"
#include "groomlab/map_ref.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/noise.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/point_index.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A graph fed by a plane scalp; helpers add nodes and evaluate.
struct LookGraph {
    Graph g;
    ExternalInputs ext;
    NodeId scalp = 0, scatter = 0, output = 0;

    explicit LookGraph(double density = 400.0, int cells = 8) {
        ext.set("scalp", test::makePlane(cells), 1);
        scalp = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", density);
        output = g.addNode(op::kResample);  // unconnected: it only holds the strands' segments
        segments(15);
        g.connect(scalp, scatter, 0);
    }
    void segments(std::int64_t n) { g.node(output).params().set("segments", n); }
    NodeId add(const char* type, NodeId input) {
        const NodeId id = g.addNode(type);
        g.connect(input, id, 0);
        return id;
    }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
    Value evalValue(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluate(id, ctx);
    }
};

// The centers a Clump without a centers input picked (its kept state, operator_state.h): a guide
// for each, the strand it is (its ID, root and points as they came in), in the order picked; none
// when it kept no choice by roots.
GuidesPtr ownCenters(LookGraph& lg, NodeId clump) {
    const StrandsPtr out = lg.eval(clump);
    NodeId source = 0;
    std::size_t output = 0;
    auto centers = std::make_shared<GuideSet>();
    if (!lg.g.inputSource(clump, 0, &source, &output)) return centers;
    const StrandsPtr in = lg.eval(source);
    ClumpState state;
    if (!clumpState(lg.g.node(clump), out, &state)) return centers;
    for (const std::size_t s : state.chosen) {
        const Vec3f* pts = in->strandPoints(s);
        centers->add(in->ids()[s], std::vector<Vec3f>(pts, pts + in->strandPointCount(s))).root = in->roots()[s];
    }
    return centers;
}

// Each strand's center as the Clump `clump` made `out`: its index among the centers input's guides (its
// kept state; -1: none). The clumpId channel numbers the clumps from their centers' IDs instead.
std::vector<int> centerIndices(LookGraph& lg, NodeId clump, const StrandsPtr& out) {
    std::vector<int> c(out->strandCount(), -1);
    ClumpState state;
    if (!clumpState(lg.g.node(clump), out, &state)) return c;
    for (std::size_t s = 0; s < c.size(); ++s) {
        const std::uint32_t at = (*state.assigned)[s];
        if (at != kNoClumpCenter) c[s] = static_cast<int>(state.centerGuides[at]);
    }
    return c;
}

float maxDisplacement(const StrandSet& a, const StrandSet& b) {
    float m = 0.0f;
    for (std::size_t i = 0; i < a.pointCount(); ++i) m = std::max(m, (a.positions()[i] - b.positions()[i]).length());
    return m;
}

// ---------------------------------------------------------------------------

TEST(Ramp, EvaluatesAndRoundTripsThroughJson) {
    Ramp r{{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.5f}};
    EXPECT_FLOAT_EQ(r(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(r(0.25f), 0.625f);   // smooth (monotone cubic) through the keys
    EXPECT_FLOAT_EQ(r(0.75f), 0.8125f);
    EXPECT_FLOAT_EQ(r(0.5f), 1.0f);
    EXPECT_FLOAT_EQ(r(2.0f), 0.5f);
    // Flat at the peak, no overshoot anywhere, no kink: slopes match across the key.
    for (float t = 0.0f; t <= 1.0f; t += 0.01f) EXPECT_LE(r(t), 1.0f + 1e-6f);
    EXPECT_NEAR((r(0.5f) - r(0.49f)) / 0.01f, (r(0.51f) - r(0.5f)) / 0.01f, 0.2f);
    EXPECT_FLOAT_EQ(Ramp::linear(0.0f, 1.0f)(0.3f), 0.3f);  // two keys: a straight line
    r.straight = true;
    EXPECT_FLOAT_EQ(r(0.25f), 0.5f);
    EXPECT_FLOAT_EQ(r(0.75f), 0.75f);
    EXPECT_FLOAT_EQ(Ramp{}(0.3f), 1.0f);

    GraphDocument doc = loadGraphJson(R"({
        "nodes": [{"name": "c", "type": "Clump", "params": {"profile": [[1, 1], [0, 0.2]], "strength": 0.5}},
                  {"name": "o", "type": "Output"}],
        "connections": [{"from": "c", "to": "o", "input": "strands"}], "output": "o"})");
    const Ramp loaded = doc.graph.node(doc.node("c")).params().getRamp("profile");
    EXPECT_FLOAT_EQ(loaded(0.0f), 0.2f);  // keys sorted on load
    EXPECT_FLOAT_EQ(loaded(0.5f), 0.6f);
    EXPECT_FLOAT_EQ(doc.graph.node(doc.node("c")).params().getRamp("strength")(0.3f), 0.5f);  // number -> constant

    const GraphDocument again = loadGraphJson(saveGraphJson(doc));
    EXPECT_EQ(again.graph.node(again.node("c")).params().getRamp("profile"), loaded);

    // Straight segments survive a save and load.
    GraphDocument lin = loadGraphJson(R"({
        "nodes": [{"name": "c", "type": "Clump", "params": {"profile": {"keys": [[0, 0], [0.5, 1], [1, 0.5]], "linear": true}}},
                  {"name": "o", "type": "Output"}],
        "connections": [{"from": "c", "to": "o", "input": "strands"}], "output": "o"})");
    const Ramp straight = lin.graph.node(lin.node("c")).params().getRamp("profile");
    EXPECT_TRUE(straight.straight);
    EXPECT_FLOAT_EQ(straight(0.25f), 0.5f);
    const GraphDocument linAgain = loadGraphJson(saveGraphJson(lin));
    EXPECT_EQ(linAgain.graph.node(linAgain.node("c")).params().getRamp("profile"), straight);
}

TEST(PointIndex, MatchesBruteForce) {
    Rng rng(5);
    std::vector<Vec3f> pts;
    for (int i = 0; i < 2000; ++i) pts.emplace_back(rng.uniform(-1, 1), 0.0f, rng.uniform(-1, 1));  // flat set
    const PointIndex index(pts);
    for (int q = 0; q < 500; ++q) {
        const Vec3f p(rng.uniform(-1.2f, 1.2f), rng.uniform(-0.2f, 0.2f), rng.uniform(-1.2f, 1.2f));
        std::vector<std::pair<float, std::size_t>> all;
        for (std::size_t i = 0; i < pts.size(); ++i) all.emplace_back((pts[i] - p).dot(pts[i] - p), i);
        std::sort(all.begin(), all.end());

        std::size_t idx[5];
        float d2[5];
        ASSERT_EQ(index.kNearest(p, 5, idx, d2), 5u);
        for (int k = 0; k < 5; ++k) EXPECT_FLOAT_EQ(d2[k], all[k].first);
        EXPECT_EQ(index.nearest(p), all[0].second);
    }
    EXPECT_EQ(PointIndex().nearest({0, 0, 0}), SIZE_MAX);
}

// The kd-tree as it was built before the implicit layout (nodes with child links), to check the
// implicit one gives the same answers, equally distant points in the same order.
class LinkedTree {
public:
    explicit LinkedTree(std::vector<Vec3f> points) : points_(std::move(points)) {
        std::vector<std::uint32_t> order(points_.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::uint32_t>(i);
        root_ = build(order, 0, order.size());
    }
    std::size_t kNearest(const Vec3f& p, std::size_t k, std::size_t* indices, float* distances2) const {
        if (root_ == kNone || k == 0) return 0;
        std::size_t found = 0;
        auto consider = [&](std::uint32_t i) {
            const Vec3f d = points_[i] - p;
            const float d2 = d.dot(d);
            if (found == k && d2 >= distances2[k - 1]) return;
            std::size_t pos = std::min(found, k - 1);
            while (pos > 0 && distances2[pos - 1] > d2) {
                distances2[pos] = distances2[pos - 1];
                indices[pos] = indices[pos - 1];
                --pos;
            }
            distances2[pos] = d2;
            indices[pos] = i;
            if (found < k) ++found;
        };
        std::vector<std::pair<std::uint32_t, float>> stack{{root_, 0.0f}};
        while (!stack.empty()) {
            const auto [node, plane] = stack.back();
            stack.pop_back();
            if (found == k && plane >= distances2[k - 1]) continue;
            const Node& n = nodes_[node];
            consider(n.point);
            const float diff = coord(p, n.axis) - coord(points_[n.point], n.axis);
            const std::uint32_t nearChild = diff < 0 ? n.left : n.right, farChild = diff < 0 ? n.right : n.left;
            if (farChild != kNone) stack.emplace_back(farChild, diff * diff);
            if (nearChild != kNone) stack.emplace_back(nearChild, 0.0f);
        }
        return found;
    }

private:
    struct Node {
        std::uint32_t point, left, right;
        int axis;
    };
    static constexpr std::uint32_t kNone = 0xffffffffu;
    static float coord(const Vec3f& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }
    std::uint32_t build(std::vector<std::uint32_t>& order, std::size_t begin, std::size_t end) {
        if (begin >= end) return kNone;
        Vec3f lo = points_[order[begin]], hi = lo;
        for (std::size_t i = begin + 1; i < end; ++i) {
            const Vec3f& p = points_[order[i]];
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const Vec3f ext = hi - lo;
        const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
        const std::size_t mid = begin + (end - begin) / 2;
        std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end, [&](std::uint32_t a, std::uint32_t b) {
            const float ca = coord(points_[a], axis), cb = coord(points_[b], axis);
            return ca < cb || (ca == cb && a < b);  // ties by index: the same tree under any standard library
        });
        const auto index = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({order[mid], kNone, kNone, axis});
        const std::uint32_t left = build(order, begin, mid);
        const std::uint32_t right = build(order, mid + 1, end);
        nodes_[index].left = left;
        nodes_[index].right = right;
        return index;
    }
    std::vector<Vec3f> points_;
    std::vector<Node> nodes_;
    std::uint32_t root_ = kNone;
};

TEST(PointIndex, AnswersAsTheLinkedTreeDidTiesIncluded) {
    // A coarse grid (many equally distant points, duplicates too) and random points, large enough
    // for the parallel build.
    Rng rng(11);
    std::vector<Vec3f> pts;
    for (int i = 0; i < 30000; ++i) {
        pts.emplace_back(static_cast<float>(i % 40) * 0.05f, static_cast<float>((i / 40) % 30) * 0.05f,
                         static_cast<float>(i % 7) * 0.05f);
    }
    for (int i = 0; i < 20000; ++i) pts.emplace_back(rng.uniform(0, 2), rng.uniform(0, 1.5f), rng.uniform(0, 0.3f));
    const PointIndex index(pts);
    const LinkedTree reference(pts);
    for (int q = 0; q < 4000; ++q) {
        // On the grid (ties) for half the queries, anywhere for the rest.
        const Vec3f p = q % 2 ? Vec3f(rng.uniform(-0.1f, 2.1f), rng.uniform(-0.1f, 1.6f), rng.uniform(-0.1f, 0.4f))
                              : Vec3f(static_cast<float>(q % 40) * 0.05f + 0.025f, static_cast<float>(q % 30) * 0.05f,
                                      0.025f);
        for (const std::size_t k : {std::size_t{1}, std::size_t{8}, std::size_t{13}}) {
            std::size_t a[13], b[13];
            float da[13], db[13];
            const std::size_t na = index.kNearest(p, k, a, da), nb = reference.kNearest(p, k, b, db);
            ASSERT_EQ(na, nb);
            for (std::size_t i = 0; i < na; ++i) {
                ASSERT_EQ(a[i], b[i]) << "query " << q << " k " << k << " rank " << i;
                ASSERT_EQ(da[i], db[i]);
            }
        }
    }
}

TEST(Noise, IsDeterministicSmoothAndBounded) {
    EXPECT_FLOAT_EQ(gradientNoise({0.3f, 1.7f, -2.2f}, 9), gradientNoise({0.3f, 1.7f, -2.2f}, 9));
    EXPECT_NE(gradientNoise({0.3f, 1.7f, -2.2f}, 9), gradientNoise({0.3f, 1.7f, -2.2f}, 10));
    EXPECT_FLOAT_EQ(gradientNoise({2.0f, -3.0f, 5.0f}, 1), 0.0f);  // zero on the lattice
    Rng rng(2);
    for (int i = 0; i < 2000; ++i) {
        const Vec3f p(rng.uniform(-50, 50), rng.uniform(-50, 50), rng.uniform(-50, 50));
        const float v = fractalNoise(p, 3, 3);
        EXPECT_LE(std::fabs(v), 1.2f);
        EXPECT_NEAR(v, fractalNoise(p + Vec3f(1e-3f, 0, 0), 3, 3), 0.02f);  // continuous
    }
}

TEST(Noise, VectorNoiseGivesWhatVectorNoiseGivesBitForBit) {
    // Walks along z (strand space: x and y kept, cells reused), walks through every axis, and
    // random jumps (every cell new), over each octave count; negative coordinates and lattice
    // points included.
    Rng rng(8);
    auto same = [](const Vec3f& a, const Vec3f& b) { return std::memcmp(&a, &b, sizeof(Vec3f)) == 0; };
    for (int octaves = 1; octaves <= VectorNoise::kMaxOctaves; ++octaves) {
        for (int walk = 0; walk < 40; ++walk) {
            const std::uint64_t seed = hash64(static_cast<std::uint64_t>(octaves * 100 + walk));
            VectorNoise noise(seed, octaves);
            Vec3f p(rng.uniform(-500, 500), rng.uniform(-500, 500), rng.uniform(-500, 500));
            if (walk == 0) p = Vec3f(-2.0f, 3.0f, -1.0f);
            const Vec3f step = walk % 3 == 0 ? Vec3f(0, 0, 0.13f)
                                             : Vec3f(rng.uniform(-0.4f, 0.4f), rng.uniform(-0.4f, 0.4f), rng.uniform(-0.4f, 0.4f));
            for (int i = 0; i < 64; ++i) {
                const Vec3f q = walk % 3 == 2 ? Vec3f(rng.uniform(-50, 50), rng.uniform(-50, 50), rng.uniform(-50, 50)) : p;
                ASSERT_TRUE(same(noise(q), vectorNoise(q, seed, octaves))) << "octaves " << octaves << " walk " << walk << " i " << i;
                p += step;
            }
        }
    }
}

TEST(Noise, OctaveWeightsFadeBetweenHalfACellAndACellOfTheSpacing) {
    // Octave o's cells are 2^-o across: a point spacing of 0.3 cells is 0.6 of octave 1's cells
    // (fading) and 1.2 of octave 2's (gone); no spacing keeps every octave.
    float w[8];
    octaveWeights(0.0f, 8, w);
    for (const float v : w) EXPECT_EQ(v, 1.0f);
    octaveWeights(0.3f, 8, w);
    EXPECT_EQ(w[0], 1.0f);                // 0.3 cells: below half a cell
    EXPECT_GT(w[1], 0.0f);                // 0.6 cells: fading
    EXPECT_LT(w[1], 1.0f);
    EXPECT_EQ(w[2], 0.0f);                // 1.2 cells: a point a bump, gone
    for (int o = 3; o < 8; ++o) EXPECT_EQ(w[o], 0.0f);
    octaveWeights(0.5f, 2, w);
    EXPECT_EQ(w[0], 1.0f);
    EXPECT_EQ(w[1], 0.0f);
    // The weights scale the octaves (the sum normalized as without them): weights of 1 give the
    // plain sum to the bit, a weight of 0 on every octave gives nothing.
    const float ones[3] = {1.0f, 1.0f, 1.0f}, none[3] = {0.0f, 0.0f, 0.0f};
    const Vec3f p(3.7f, -1.2f, 8.9f);
    const Vec3f plain = vectorNoise(p, 5, 3), weighted = vectorNoise(p, 5, 3, ones);
    EXPECT_EQ(std::memcmp(&weighted, &plain, sizeof(Vec3f)), 0);
    EXPECT_EQ(vectorNoise(p, 5, 3, none), Vec3f{});
    VectorNoise cached(5, 3, ones);
    const Vec3f fromCache = cached(p);
    EXPECT_EQ(std::memcmp(&fromCache, &plain, sizeof(Vec3f)), 0);
}

TEST(Noise, OctavesFinerThanTheStrandsPointsFadeOut) {
    // The same bent strands at 8, 9, 10 and 11 points under a Noise of many octaves at a high
    // frequency, in strand space and in world space. A strand cannot draw the octaves finer than
    // its points: read in full they alias, each point taking what amounts to a random offset, so
    // the look changes with Resample's segments and shimmers (the results at one point count and
    // the next, compared as curves at the same shares of the length, far apart). Faded out by
    // octaveWeights, the strands draw the octaves they can and the results agree from one count
    // to the next; and a strand with fewer points never carries more noise than one with more.
    // The unfaded sum, what the noise gave before, is read through vectorNoise to show the miss.
    constexpr std::size_t kStrands = 48, kFine = 64;
    constexpr float kAmplitude = 0.1f;
    constexpr int kOctaves = 8;
    constexpr std::uint64_t kSeed = 3;
    auto resampled = [&](std::size_t count) {
        auto set = std::make_shared<StrandSet>();
        for (std::size_t s = 0; s < kStrands; ++s) {
            std::vector<Vec3f> fine;
            const float x = 0.3f * static_cast<float>(s);
            for (int i = 0; i <= 200; ++i) {
                const float t = static_cast<float>(i) / 200.0f;
                fine.emplace_back(x + 0.4f * std::sin(2.0f * t), 1.2f * t, 0.3f * t * t);
            }
            set->addStrand(static_cast<StrandId>(s + 1), resampleByArcLength(fine.data(), fine.size(), count));
        }
        return set;
    };
    // A frequency the coarse points draw (2 features over 7 segments), then ones past their limit.
    for (const float frequency : {2.0f, 10.0f, 15.0f, 20.0f}) {
        const bool aliased = frequency > 7.0f;  // more features than segments: nothing to draw
        for (const bool world : {false, true}) {
            const char* space = world ? "world" : "strand";
            auto noised = [&](const std::shared_ptr<StrandSet>& in) {
                auto noise = OperatorRegistry::instance().create(op::kNoise);
                noise->params().set("amplitude", static_cast<double>(kAmplitude));
                noise->params().set("frequency", static_cast<double>(frequency));
                noise->params().set("octaves", std::int64_t{kOctaves});
                noise->params().set("profile", Ramp::constant(1.0f));
                noise->params().set("space", std::string(space));
                noise->params().set("seed", std::int64_t{kSeed});
                EvalContext ctx;
                return valueAs<StrandsPtr>(noise->evaluate(ctx, {Value(StrandsPtr(in))}));
            };
            // Each strand's displacement at point i of n: the operator's (faded), and every
            // octave in full as the noise read before (unfaded).
            struct Drawn {
                std::vector<Vec3f> faded, unfaded;  // per strand, per point
                std::size_t n = 0;
            };
            auto drawn = [&](std::size_t n) {
                const std::shared_ptr<StrandSet> in = resampled(n);
                const StrandsPtr out = noised(in);
                Drawn d;
                d.n = n;
                for (std::size_t s = 0; s < kStrands; ++s) {
                    Rng rng(hashCombine(static_cast<StrandId>(s + 1), kSeed));
                    const Vec3f offset = randomOffsetZYX(rng);
                    for (std::size_t i = 0; i < n; ++i) {
                        const Vec3f p = in->strandPoints(s)[i];
                        d.faded.push_back(out->strandPoints(s)[i] - p);
                        const Vec3f q = world ? p * frequency : offset + Vec3f(0, 0, static_cast<float>(i) / static_cast<float>(n - 1) * frequency);
                        d.unfaded.push_back(i == 0 ? Vec3f{} : vectorNoise(q, kSeed, kOctaves) * kAmplitude);
                    }
                }
                return d;
            };
            auto rms = [&](const std::vector<Vec3f>& v) {
                double sum = 0.0;
                for (const Vec3f& d : v) sum += d.dot(d);
                return std::sqrt(sum / static_cast<double>(v.size())) / kAmplitude;
            };
            // The displacement at share t of the length of a strand of n points, straight between
            // its points as the strand is drawn.
            auto at = [](const std::vector<Vec3f>& d, std::size_t s, std::size_t n, float t) {
                const float x = t * static_cast<float>(n - 1);
                const std::size_t k = std::min(static_cast<std::size_t>(x), n - 2);
                const float f = x - static_cast<float>(k);
                return d[s * n + k] * (1.0f - f) + d[s * n + k + 1] * f;
            };
            double shimmerFaded = 0.0, shimmerUnfaded = 0.0;
            std::size_t samples = 0;
            Drawn previous = drawn(8);
            for (std::size_t n = 9; n <= 11; ++n) {
                const Drawn next = drawn(n);
                for (std::size_t s = 0; s < kStrands; ++s) {
                    for (std::size_t i = 1; i < n; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
                        const Vec3f f = next.faded[s * n + i] - at(previous.faded, s, previous.n, t);
                        const Vec3f u = next.unfaded[s * n + i] - at(previous.unfaded, s, previous.n, t);
                        shimmerFaded += f.dot(f);
                        shimmerUnfaded += u.dot(u);
                        ++samples;
                    }
                }
                EXPECT_LE(rms(previous.faded), rms(next.faded) + 1e-6) << space << " " << frequency << ": " << previous.n << " vs " << n
                                                                      << " points: fewer points never carry more noise";
                previous = next;
            }
            shimmerFaded = std::sqrt(shimmerFaded / static_cast<double>(samples)) / kAmplitude;
            shimmerUnfaded = std::sqrt(shimmerUnfaded / static_cast<double>(samples)) / kAmplitude;
            const double fine = rms(drawn(kFine).faded);
            std::printf("[ noise  ] %s space, frequency %.0f: from one point count to the next the strands change by %.3f of the "
                        "amplitude (every octave in full: %.3f); 8 points carry %.3f, 64 points %.3f\n",
                        space, frequency, shimmerFaded, shimmerUnfaded, rms(drawn(8).faded), fine);
            EXPECT_GT(fine, 0.15) << "the 64-point strands carry the noise";
            if (!aliased) {
                EXPECT_GT(rms(drawn(8).faded), 0.1) << "8 points draw 2 features";
                continue;
            }
            EXPECT_LT(shimmerFaded, 0.05) << space << " " << frequency;
            EXPECT_GT(shimmerUnfaded, 0.2) << space << " " << frequency;
            EXPECT_LT(shimmerFaded * 3.0, shimmerUnfaded) << space << " " << frequency;
        }
    }
}

// ---------------------------------------------------------------------------

TEST(Clump, PullsTipsOntoTheirCenter) {
    LookGraph lg;
    auto centers = std::make_shared<GuideSet>();
    centers->add(1, test::straightCurve({0.25f, 0, 0.5f}, {0, 1, 0}, 8));
    centers->add(2, test::straightCurve({0.75f, 0, 0.5f}, {0.3f, 1, 0}, 8));
    lg.ext.set("centers", GuidesPtr(centers), 7);
    const NodeId src = lg.g.addNode(op::kGuides);
    lg.g.node(src).params().set("input", std::string("centers"));
    const NodeId clump = lg.add(op::kClump, lg.scatter);
    lg.g.connect(src, clump, 1);
    lg.g.node(clump).params().set("centersFrom", std::string("inputCurves"));  // onto the center curves themselves
    lg.g.node(clump).params().set("strength", 1.0);  // all the way onto the center at the tip

    const StrandsPtr before = lg.eval(lg.scatter);
    const StrandsPtr out = lg.eval(clump);
    const auto* ids = out->strandChannel("clumpId");
    ASSERT_NE(ids, nullptr);
    const std::vector<int> centerOf = centerIndices(lg, clump, out);
    std::map<int, float> numbers;  // each center's clump number: one per center, none shared
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* p = out->strandPoints(s);
        const std::size_t n = out->strandPointCount(s);
        const int c = centerOf[s];
        EXPECT_EQ(c, p[0].x < 0.5f ? 0 : 1);
        EXPECT_EQ(numbers.emplace(c, (*ids)[s]).first->second, (*ids)[s]);
        EXPECT_EQ(p[0], before->strandPoints(s)[0]);  // roots stay
        // Full strength at the tip: on the center's point as far along it as the strand is long
        // (the centers are straight; the second is longer than the strands).
        const std::vector<Vec3f>& center = centers->guides()[c].points;
        const float along = std::min(arcLength(before->strandPoints(s), n), arcLength(center));
        const Vec3f centerAt = center.front() + (center.back() - center.front()).normalized() * along;
        EXPECT_NEAR((p[n - 1] - centerAt).length(), 0.0f, 1e-4f);
    }
    ASSERT_EQ(numbers.size(), 2u);
    EXPECT_NE(numbers[0], numbers[1]);

    // Strands that escape their clump (Yeti's and XGen's flyaways): a strength varying per strand.
    // Every strand free: nothing moves.
    lg.g.node(clump).params().set("strength", ParamValue(MapRef{MapRef::Source::Expression, "rand(1) < 2 ? 0 : 1"}));
    EXPECT_NEAR(maxDisplacement(*lg.eval(clump), *before), 0.0f, 1e-6f);
}

TEST(Clump, StrandStrengthRandomLetsEachStrandHangLooseItsOwnWay) {
    // One center; at strength 1 every tip lands on it. With strandStrengthRandom each strand's pull
    // is scaled down by its own random share: tips spread off the center, each strand the same
    // every evaluation, and the strands of one clump differ (unlike strengthRandom, per clump).
    LookGraph lg;
    auto centers = std::make_shared<GuideSet>();
    centers->add(1, test::straightCurve({0.5f, 0, 0.5f}, {0, 1, 0}, 8));
    lg.ext.set("centers", GuidesPtr(centers), 7);
    const NodeId src = lg.g.addNode(op::kGuides);
    lg.g.node(src).params().set("input", std::string("centers"));
    const NodeId clump = lg.add(op::kClump, lg.scatter);
    lg.g.connect(src, clump, 1);
    lg.g.node(clump).params().set("centersFrom", std::string("inputCurves"));
    lg.g.node(clump).params().set("strength", 1.0);
    const StrandsPtr before = lg.eval(lg.scatter);
    auto tipOff = [&](const StrandSet& s, std::size_t k) {
        const Vec3f tip = s.strandPoints(k)[s.strandPointCount(k) - 1];
        return std::hypot(tip.x - 0.5f, tip.z - 0.5f);  // across the center (straight up y)
    };
    const StrandsPtr tight = lg.eval(clump);
    for (std::size_t k = 0; k < tight->strandCount(); ++k) EXPECT_LT(tipOff(*tight, k), 1e-4f);

    lg.g.node(clump).params().set("strandStrengthRandom", 0.6);
    const StrandsPtr loose = lg.eval(clump);
    std::vector<float> share;  // how much of its root's distance each tip keeps
    for (std::size_t k = 0; k < loose->strandCount(); ++k) {
        const float from = tipOff(*before, k);
        if (from < 0.05f) continue;
        share.push_back(tipOff(*loose, k) / from);
    }
    ASSERT_GT(share.size(), 10u);
    const auto [lo, hi] = std::minmax_element(share.begin(), share.end());
    EXPECT_LE(*hi, 0.6f + 1e-3f);   // at most 60% left loose
    EXPECT_GT(*hi - *lo, 0.2f);     // and each strand its own way
    lg.g.node(clump).params().set("strandStrengthRandom", 0.0);
    EXPECT_NEAR(maxDisplacement(*lg.eval(clump), *tight), 0.0f, 1e-6f);
    lg.g.node(clump).params().set("strandStrengthRandom", 0.6);
    EXPECT_NEAR(maxDisplacement(*lg.eval(clump), *loose), 0.0f, 1e-6f);  // the same every time (by each strand's ID)
}

TEST(Clump, PullsEachPointToTheCentersPointAtItsOwnArcLength) {
    // A long, a short and a longer strand beside a straight center up y, at strength 1 all along:
    // each point lands on the center's point as far from the root as the point is along its
    // strand, so the short strand's tip stops at the center's point at its own length (short
    // hairs stay by the clump's root, as Ornatrix's and Yeti's), not at the center's tip, and the
    // longer strand's points past the center's length gather at its tip.
    auto centers = std::make_shared<GuideSet>();
    centers->add(1, test::straightCurve({0, 0, 0}, {0, 1, 0}, 8));
    auto strands = std::make_shared<StrandSet>();
    strands->addStrand(1, test::straightCurve({0.1f, 0, 0}, {0, 1, 0}, 8));
    strands->addStrand(2, test::straightCurve({-0.1f, 0, 0.05f}, {0, 0.4f, 0}, 8));
    strands->addStrand(3, test::straightCurve({0, 0, -0.1f}, {0.2f, 1.5f, 0}, 8));
    auto clump = OperatorRegistry::instance().create(op::kClump);
    clump->params().set("strength", 1.0);
    clump->params().set("profile", Ramp::constant(1.0f));
    clump->params().set("centersFrom", std::string("inputCurves"));
    EvalContext ctx;
    const StrandsPtr out = valueAs<StrandsPtr>(clump->evaluate(ctx, {Value(StrandsPtr(strands)), Value(GuidesPtr(centers))}));
    ASSERT_TRUE(out);
    for (std::size_t s = 0; s < 3; ++s) {
        const Vec3f* src = strands->strandPoints(s);
        const Vec3f* p = out->strandPoints(s);
        EXPECT_EQ(p[0], src[0]) << "roots stay";
        float arc = 0.0f;
        for (std::size_t i = 1; i < 9; ++i) {
            arc += (src[i] - src[i - 1]).length();
            const Vec3f expected(0.0f, std::min(arc, 1.0f), 0.0f);
            EXPECT_NEAR((p[i] - expected).length(), 0.0f, 1e-5f) << "strand " << s << " point " << i;
        }
    }
    EXPECT_NEAR((out->strandPoints(1)[8] - Vec3f(0.0f, 0.4f, 0.0f)).length(), 0.0f, 1e-5f) << "the short strand's tip";
    EXPECT_NEAR((out->strandPoints(2)[8] - Vec3f(0.0f, 1.0f, 0.0f)).length(), 0.0f, 1e-5f) << "past the center's tip: the tip";
}

TEST(Clump, OwnCentersAreKeptAcrossAStrengthTweakAndTheSameOnOneThread) {
    // A Clump picking its own centers (no centers input), by count and by size on a scalp (the
    // darts thrown on it): a strength tweak keeps the same centers (its pick is kept, keyed on
    // what the pick reads), the result is what a fresh node makes, and one thread and many agree
    // to the bit.
    for (const bool bySize : {false, true}) {
        LookGraph lg(1500.0);
        const NodeId clump = lg.add(op::kClump, lg.scatter);
        lg.g.node(clump).params().set("clumpBy", std::string(bySize ? "size" : "count"));
        lg.g.node(clump).params().set("count", std::int64_t{40});
        lg.g.node(clump).params().set("size", 0.15);
        lg.g.node(clump).params().set("strength", 0.7);
        const StrandsPtr first = lg.eval(clump);
        const std::vector<float> ids = *first->strandChannel("clumpId");
        std::size_t clumps = 0;
        for (const float id : ids) clumps = std::max(clumps, static_cast<std::size_t>(id) + 1);
        EXPECT_GT(clumps, 20u) << (bySize ? "size" : "count");
        lg.g.node(clump).params().set("strength", 0.75);
        const StrandsPtr tweaked = lg.eval(clump);
        EXPECT_EQ(*tweaked->strandChannel("clumpId"), ids) << "the same centers after a strength tweak";
        EXPECT_NE(tweaked->positions(), first->positions());

        LookGraph fresh(1500.0);
        const NodeId again = fresh.add(op::kClump, fresh.scatter);
        fresh.g.node(again).params() = lg.g.node(clump).params();
        const StrandsPtr made = fresh.eval(again);
        EXPECT_EQ(made->positions(), tweaked->positions()) << "a fresh node makes the same";
#if GROOMLAB_HAS_TBB
        StrandsPtr serial;
        tbb::task_arena single(1);
        single.execute([&] {
            LookGraph one(1500.0);
            const NodeId node = one.add(op::kClump, one.scatter);
            one.g.node(node).params() = lg.g.node(clump).params();
            serial = one.eval(node);
        });
        EXPECT_EQ(serial->positions(), tweaked->positions()) << "one thread and many agree";
        EXPECT_EQ(*serial->strandChannel("clumpId"), ids);
#endif
    }
}

TEST(Clump, NegativeStrengthPartsTheStrandsAroundTheirCenters) {
    // Fur standing up from the plane; a long whisker standing at (0.5, 0.5), one lying low along
    // the skin, and one exactly on a strand: a negative strength pushes the fur within the repel
    // radius away from them, roots and lengths kept, never into the skin; the rest stays bit for bit.
    LookGraph lg(3000.0);
    const StrandsPtr before = lg.eval(lg.scatter);
    ASSERT_GT(before->strandCount(), 1000u);
    const Vec3f onStrand = before->strandPoints(0)[0];
    auto centers = std::make_shared<GuideSet>();
    centers->add(1, test::straightCurve({0.5f, 0, 0.5f}, {0, 2, 0}, 8));
    centers->add(2, test::straightCurve({0.1f, 0, 0.15f}, {0.8f, 0.15f, 0}, 8));
    centers->add(3, test::straightCurve(onStrand, {0, 1, 0}, 8));
    lg.ext.set("centers", GuidesPtr(centers), 7);
    const NodeId src = lg.g.addNode(op::kGuides);
    lg.g.node(src).params().set("input", std::string("centers"));
    const NodeId clump = lg.add(op::kClump, lg.scatter);
    lg.g.connect(src, clump, 1);
    lg.g.node(clump).params().set("centersFrom", std::string("inputCurves"));  // part around the whiskers themselves
    lg.g.node(clump).params().set("strength", -0.8);
    lg.g.node(clump).params().set("profile", Ramp::linear(1.0f, 1.0f));
    const float radius = 0.12f;
    lg.g.node(clump).params().set("repelRadius", static_cast<double>(radius));
    const StrandsPtr out = lg.eval(clump);
    ASSERT_EQ(out->pointCount(), before->pointCount());
    const std::vector<int> centerOf = centerIndices(lg, clump, out);

    // Distance across the upright whisker (it is straight up: in the plane).
    auto across = [](const Vec3f& p) { return std::hypot(p.x - 0.5f, p.z - 0.5f); };
    std::size_t parted = 0, untouched = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* a = before->strandPoints(s);
        const Vec3f* b = out->strandPoints(s);
        const std::size_t n = out->strandPointCount(s);
        EXPECT_EQ(b[0], a[0]);  // roots stay
        for (std::size_t i = 1; i < n; ++i) {
            EXPECT_NEAR((b[i] - b[i - 1]).length(), (a[i] - a[i - 1]).length(), 1e-5f) << "lengths kept";
            EXPECT_GE(b[i].y, -1e-6f) << "never into the skin";
        }
        const int clumpId = centerOf[s];
        if (clumpId == 0 && across(a[0]) < 0.8f * radius && across(a[0]) > 1e-3f) {
            ++parted;
            for (std::size_t i = 1; i < n; ++i) EXPECT_GT(across(b[i]), across(a[i]) + 1e-3f) << "pushed away";
        }
        // Beyond the radius of its center (straight strands: every point as far as the root).
        const Vec3f* c = centers->guides()[static_cast<std::size_t>(clumpId)].points.data();
        const float rootDistance = clumpId == 1 ? 1.0f : std::hypot(a[0].x - c[0].x, a[0].z - c[0].z);
        if (clumpId != 1 && rootDistance > 2.0f * radius * 1.01f) {
            ++untouched;
            EXPECT_EQ(std::memcmp(a, b, n * sizeof(Vec3f)), 0) << "beyond twice the radius: as it came";
        }
    }
    EXPECT_GT(parted, 10u);
    EXPECT_GT(untouched, before->strandCount() / 2);
    // The strand the third center sits on goes some way of its own (its root's offset is none).
    EXPECT_GT(maxDisplacement(*out, *before), 0.05f);
    const Vec3f* onIt = out->strandPoints(0);
    EXPECT_GT(std::hypot(onIt[out->strandPointCount(0) - 1].x - onStrand.x, onIt[out->strandPointCount(0) - 1].z - onStrand.z),
              0.05f);

    // A weaker push moves less; strength 0 moves nothing.
    lg.g.node(clump).params().set("strength", -0.3);
    const StrandsPtr weaker = lg.eval(clump);
    EXPECT_LT(maxDisplacement(*weaker, *before), maxDisplacement(*out, *before));
    lg.g.node(clump).params().set("strength", 0.0);
    EXPECT_EQ(maxDisplacement(*lg.eval(clump), *before), 0.0f);
}

TEST(Clump, ScatteredPointsAsCentersPlaceClumpsTheStrandsShape) {
    // Long strands bending over (Interpolate from guides 3 long), clumped toward a sparse Scatter's
    // straight spikes 1 long, fed straight in (strands as centers, no ToGuides). centersFrom
    // inputPoints: the spikes only place the clumps, each clump's axis is the strand it gathers
    // nearest its spike, so the strands keep their length and their tips stay where they grew.
    // inputCurves pulls them onto the spikes themselves: past a spike's tip every point
    // gathers at it, the strands' lengths lost.
    LookGraph lg(1500.0);
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const Vec3f root(0.15f + 0.35f * static_cast<float>(i), 0.0f, 0.15f + 0.35f * static_cast<float>(j));
            std::vector<Vec3f> pts;  // an arc of radius 2 over 1.5 radians: 3 long, up then over along x
            for (int k = 0; k < 16; ++k) {
                const float a = 1.5f * static_cast<float>(k) / 15.0f;
                pts.push_back(root + Vec3f(2.0f * (1.0f - std::cos(a)), 2.0f * std::sin(a), 0.0f));
            }
            guides->add(id++, pts);
        }
    }
    lg.ext.set("guides", GuidesPtr(guides), 17);
    const NodeId guideNode = lg.g.addNode(op::kGuides);
    const NodeId interp = lg.add(op::kInterpolate, lg.scatter);
    lg.g.connect(guideNode, interp, 1);
    const NodeId spots = lg.add(op::kScatter, lg.scalp);
    lg.g.node(spots).params().set("density", 25.0);
    lg.g.node(spots).params().set("seed", std::int64_t{4});
    const NodeId clump = lg.add(op::kClump, interp);
    lg.g.connect(spots, clump, 1);
    lg.g.node(clump).params().set("centersFrom", std::string("inputPoints"));
    lg.g.node(clump).params().set("strength", 0.8);

    const StrandsPtr before = lg.eval(interp);
    const StrandsPtr centers = lg.eval(spots);
    ASSERT_GT(centers->strandCount(), 12u);
    ASSERT_LT(centers->strandCount(), 50u);
    struct Look {
        float worstLength = 0.0f;  // the largest share of its length a strand gained or lost
        float meanTipMove = 0.0f;  // how far the tips moved, on average
        std::size_t clumps = 0;    // distinct clump IDs
        bool allClumped = true;
    };
    auto look = [&](const StrandsPtr& out) {
        Look l;
        const std::vector<float>* ids = out->strandChannel("clumpId");
        EXPECT_TRUE(ids);
        std::set<int> seen;
        double moved = 0.0;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            const std::size_t n = out->strandPointCount(s);
            const float was = arcLength(before->strandPoints(s), n), now = arcLength(out->strandPoints(s), n);
            l.worstLength = std::max(l.worstLength, std::abs(now / was - 1.0f));
            moved += (out->strandPoints(s)[n - 1] - before->strandPoints(s)[n - 1]).length();
            const int c = ids ? static_cast<int>((*ids)[s]) : -1;
            l.allClumped = l.allClumped && c >= 0;
            seen.insert(c);
        }
        l.meanTipMove = static_cast<float>(moved / static_cast<double>(out->strandCount()));
        l.clumps = seen.size();
        return l;
    };

    const StrandsPtr fromStrands = lg.eval(clump);
    const Look strands = look(fromStrands);
    EXPECT_TRUE(strands.allClumped);
    EXPECT_GT(strands.clumps, centers->strandCount() * 3 / 4) << "about a clump a spike";
    EXPECT_LT(strands.worstLength, 0.1f) << "the strands keep their length";
    EXPECT_LT(strands.meanTipMove, 0.2f) << "the tips stay by where they grew, gathered into clumps";
    EXPECT_GT(strands.meanTipMove, 0.01f) << "they clump";
    ClumpState state;
    ASSERT_TRUE(clumpState(lg.g.node(clump), fromStrands, &state));
    ASSERT_EQ(state.axes.size(), state.centerGuides.size());
    for (std::size_t c = 0; c < state.axes.size(); ++c) {
        if (state.axes[c] == kNoClumpCenter) continue;
        const std::uint32_t a = state.axes[c];
        EXPECT_EQ((*state.assigned)[a], c) << "an axis is a strand of its own clump";
        const Vec3f tip = before->strandPoints(a)[before->strandPointCount(a) - 1];
        EXPECT_NEAR((state.curves[(c + 1) * kClumpCenterSamples - 1] - tip).length(), 0.0f, 1e-4f) << "its curve is the axis strand";
    }
    // The spikes' own shape (inputCurves): onto 1-long spikes, the strands lose their
    // length and their tips move to the spikes' tips, every point to the bit as before.
    lg.g.node(clump).params().set("centersFrom", std::string("inputCurves"));
    const StrandsPtr fromCenters = lg.eval(clump);
    const Look spikes = look(fromCenters);
    EXPECT_TRUE(spikes.allClumped);
    EXPECT_GT(spikes.worstLength, 0.3f);
    EXPECT_GT(spikes.meanTipMove, 1.0f);
    std::printf("[ clump  ] scattered centers: from strands, lengths within %.3f, tips moved %.3f; from the spikes, %.3f and "
                "%.3f\n",
                strands.worstLength, strands.meanTipMove, spikes.worstLength, spikes.meanTipMove);
    // The spikes as guides (ToGuides, the only way in before strands were taken): the same to the
    // bit.
    const NodeId asGuides = lg.add(op::kToGuides, spots);
    lg.g.connect(asGuides, clump, 1);
    EXPECT_EQ(lg.eval(clump)->contentHash(), fromCenters->contentHash());
    // pastTip extend: past a spike's tip the strands go on along it, keeping their length.
    lg.g.node(clump).params().set("pastTip", std::string("extend"));
    const Look extended = look(lg.eval(clump));
    EXPECT_LT(extended.worstLength, 0.5f * spikes.worstLength);
    lg.g.node(clump).params().set("pastTip", std::string("gather"));

    // Centers that are strands of its own (ToGuides of them): either way, the same to the bit.
    const NodeId own = lg.add(op::kToGuides, interp);
    lg.g.node(own).params().set("fraction", 0.03);
    lg.g.connect(own, clump, 1);
    const StrandsPtr ownCurves = lg.eval(clump);
    lg.g.node(clump).params().set("centersFrom", std::string("inputPoints"));
    EXPECT_EQ(lg.eval(clump)->contentHash(), ownCurves->contentHash());

    // Centers of a single point (a root, no curve): placed clumps from the strands; nothing from
    // the centers' own shape (none).
    auto points = std::make_shared<GuideSet>();
    for (std::size_t s = 0; s < centers->strandCount(); ++s) points->add(100 + s, {centers->strandPoints(s)[0]});
    lg.ext.set("points", GuidesPtr(points), 23);
    const NodeId pointNode = lg.g.addNode(op::kGuides);
    lg.g.node(pointNode).params().set("input", std::string("points"));
    lg.g.connect(pointNode, clump, 1);
    const Look single = look(lg.eval(clump));
    EXPECT_TRUE(single.allClumped);
    EXPECT_GT(single.clumps, centers->strandCount() * 3 / 4);
    EXPECT_LT(single.worstLength, 0.1f);
    lg.g.node(clump).params().set("centersFrom", std::string("inputCurves"));
    EXPECT_FALSE(look(lg.eval(clump)).allClumped) << "no curves to follow";
}

TEST(ClumpCenters, PicksSpreadStableCenters) {
    LookGraph lg(2000.0);
    const NodeId auto1 = lg.add(op::kClump, lg.scatter);
    lg.g.node(auto1).params().set("count", std::int64_t{40});
    lg.g.node(auto1).params().set("randomness", 0.0);  // the even centers
    const GuidesPtr a = ownCenters(lg, auto1);
    ASSERT_TRUE(a);
    EXPECT_GE(a->size(), 34u);  // about the count
    EXPECT_LE(a->size(), 50u);

    const float spacing = 0.7f * std::sqrt(1.0f / 40.0f);  // kEvenShare
    for (std::size_t i = 0; i < a->size(); ++i) {
        for (std::size_t j = i + 1; j < a->size(); ++j) {
            EXPECT_GE((a->guides()[i].points[0] - a->guides()[j].points[0]).length(), spacing * 0.99f * 0.99f);  // (1% steps)
        }
    }
    lg.g.clearCache();
    const GuidesPtr b = ownCenters(lg, auto1);
    ASSERT_EQ(a->size(), b->size());
    for (std::size_t i = 0; i < a->size(); ++i) EXPECT_EQ(a->guides()[i].id, b->guides()[i].id);
}

TEST(ClumpCenters, RandomnessSpreadsOrScattersTheCentersAsScatterDoes) {
    // randomness 0: the centers are kept apart (every one about the average spacing from its
    // nearest, none closer than kEvenShare of it), about the count; 1: each moved to a random spot
    // up to a spacing from its even one, so some sit side by side; in between, in between; past 1
    // farther still. The same centers (count and IDs) at every randomness. On a sphere the spacing
    // comes from the scalp's own area, so 0 spreads as evenly there as on a plane.
    auto nearest = [](const GuideSet& g) {
        std::vector<float> d(g.size(), 1e30f);
        for (std::size_t i = 0; i < g.size(); ++i) {
            for (std::size_t j = 0; j < g.size(); ++j) {
                if (i != j) d[i] = std::min(d[i], (g.guides()[i].points[0] - g.guides()[j].points[0]).length());
            }
        }
        return d;
    };
    auto centers = [](double randomness, MeshPtr scalp) {
        LookGraph lg(20000.0);
        lg.ext.set("scalp", scalp, scalp->surfaceHash());
        const NodeId auto1 = lg.add(op::kClump, lg.scatter);
        lg.g.node(auto1).params().set("count", std::int64_t{60});
        lg.g.node(auto1).params().set("randomness", randomness);
        return ownCenters(lg, auto1);
    };
    for (const MeshPtr& scalp : {test::makePlane(8, 1.0f), test::makeSphere(24, 48, 0.5f)}) {
        const float average = std::sqrt(scalp->restTotalArea() / 60.0f);
        const GuidesPtr even = centers(0.0, scalp), half = centers(0.5, scalp), random = centers(1.0, scalp),
                        over = centers(1.8, scalp);
        ASSERT_TRUE(even && half && random && over);
        std::printf("[ centers  ] %zu triangles: %zu of 60\n", scalp->triangleCount(), even->size());
        EXPECT_GE(even->size(), 51u);  // about the count
        EXPECT_LE(even->size(), 69u);
        // The same centers whatever the randomness (ownCenters names each by the strand it lands
        // on: the count stays).
        for (const GuidesPtr& c : {half, random, over}) EXPECT_EQ(c->size(), even->size());
        auto stats = [](const std::vector<float>& d, float* least, float* mean) {
            *least = 1e30f;
            *mean = 0.0f;
            for (const float x : d) *least = std::min(*least, x), *mean += x / static_cast<float>(d.size());
        };
        float minEven = 0, meanEven = 0, minHalf = 0, meanHalf = 0, minRandom = 0, meanRandom = 0, minOver = 0, meanOver = 0;
        stats(nearest(*even), &minEven, &meanEven);
        stats(nearest(*half), &minHalf, &meanHalf);
        stats(nearest(*random), &minRandom, &meanRandom);
        stats(nearest(*over), &minOver, &meanOver);
        std::printf("[ centers  ] nearest, of the average spacing: even %.2f (closest %.2f), half %.2f (%.2f), random "
                    "%.2f (%.2f), 1.8 %.2f (%.2f)\n", meanEven / average, minEven / average, meanHalf / average,
                    minHalf / average, meanRandom / average, minRandom / average, meanOver / average, minOver / average);
        EXPECT_GE(minEven, 0.7f * average * 0.98f);  // (the spacing in 1% steps)
        EXPECT_LT(minHalf, minEven);
        EXPECT_LT(meanHalf, meanEven);
        EXPECT_LT(meanRandom, meanHalf);
        EXPECT_LT(meanOver, meanHalf);  // past 1 random still (as white noise: about half the spacing)
        EXPECT_LT(minRandom, 0.3f * average);  // some side by side
    }
}

TEST(ClumpCenters, ASizeGivesClumpsThatFarApartHoweverBigTheScalp) {
    // size (a length): the count is the area strands cover over size squared, the spacing the
    // size's share: on the unit plane, size 0.1 asks for about 100 clumps, 0.2 for about 25.
    auto centers = [](double size, double randomness = 0.3) {
        LookGraph lg(4000.0);
        const NodeId auto1 = lg.add(op::kClump, lg.scatter);
        lg.g.node(auto1).params().set("clumpBy", std::string("size"));
        lg.g.node(auto1).params().set("count", std::int64_t{3});  // not read: clumpBy size
        lg.g.node(auto1).params().set("size", size);
        lg.g.node(auto1).params().set("randomness", randomness);
        return ownCenters(lg, auto1);
    };
    const GuidesPtr small = centers(0.1), big = centers(0.2);
    ASSERT_TRUE(small && big);
    EXPECT_GE(small->size(), 60u);
    EXPECT_LE(small->size(), 115u);  // (the plane's edges hold a few more)
    const double ratio = static_cast<double>(small->size()) / static_cast<double>(big->size());
    EXPECT_GT(ratio, 2.5);  // (4 but for the plane's edges, which the bigger clumps reach more of)
    EXPECT_LT(ratio, 5.0);
    // At randomness 0 kept kEvenShare of the size apart.
    const GuidesPtr even = centers(0.1, 0.0);
    for (std::size_t i = 0; i < even->size(); ++i) {
        for (std::size_t j = i + 1; j < even->size(); ++j) {
            EXPECT_GE((even->guides()[i].points[0] - even->guides()[j].points[0]).length(), 0.7f * 0.1f * 0.99f);
        }
    }
    // The clumps the strands are in: as many.
    LookGraph lg(4000.0);
    const NodeId clump = lg.add(op::kClump, lg.scatter);
    lg.g.node(clump).params().set("clumpBy", std::string("size"));
    lg.g.node(clump).params().set("size", 0.1);
    const StrandsPtr out = lg.eval(clump);
    const std::vector<float>* ids = out->strandChannel("clumpId");
    ASSERT_NE(ids, nullptr);
    std::set<long> clumps;
    for (const float c : *ids) clumps.insert(std::lround(c));
    EXPECT_GE(clumps.size(), 60u);
    EXPECT_LE(clumps.size(), 115u);
}

TEST(ClumpCenters, ClumpByReadsOnlyTheOneItPicks) {
    // clumpBy count: the size does nothing; clumpBy size: the count does nothing (the catalog shows
    // each only with its clumpBy, and clumpBy only with centersFrom pick).
    auto centers = [](const std::string& by, std::int64_t count, double size) {
        LookGraph lg(4000.0);
        const NodeId auto1 = lg.add(op::kClump, lg.scatter);
        lg.g.node(auto1).params().set("clumpBy", by);
        lg.g.node(auto1).params().set("count", count);
        lg.g.node(auto1).params().set("size", size);
        const GuidesPtr out = ownCenters(lg, auto1);
        std::vector<std::uint64_t> ids;
        for (const Guide& g : out->guides()) ids.push_back(g.id);
        return ids;
    };
    EXPECT_EQ(centers("count", 40, 0.05), centers("count", 40, 0.3));
    EXPECT_EQ(centers("size", 10, 0.2), centers("size", 90, 0.2));
    EXPECT_NE(centers("count", 40, 0.2), centers("size", 40, 0.2));
    EXPECT_TRUE(centers("size", 40, 0.0).empty());  // size 0: no clumps
    for (const nlohmann::json& op : nlohmann::json::parse(operatorSpecsJson())) {
        if (op["type"] != "Clump") continue;
        for (const nlohmann::json& p : op["params"]) {
            if (p["name"] == "count") EXPECT_EQ(p["showWhen"], (nlohmann::json{{"param", "clumpBy"}, {"in", {"count"}}}));
            if (p["name"] == "clumpBy") EXPECT_EQ(p["showWhen"], (nlohmann::json{{"param", "centersFrom"}, {"in", {"pick"}}}));
        }
    }
}

TEST(ClumpCenters, CentersStayPutWhenTheScatterDensityChanges) {
    // More strands: every ID changes, but the centers are placed on the scalp first and take the
    // nearest root, so each clump stays about where it was instead of rerolling.
    auto centersAt = [](double density) {
        LookGraph lg(density);
        const NodeId centers = lg.add(op::kClump, lg.scatter);
        lg.g.node(centers).params().set("count", std::int64_t{40});
        std::vector<Vec3f> roots;
        const GuidesPtr picked = ownCenters(lg, centers);
        for (const Guide& g : picked->guides()) roots.push_back(g.points[0]);
        return roots;
    };
    const std::vector<Vec3f> a = centersAt(2000.0), b = centersAt(3000.0);
    ASSERT_GE(a.size(), 25u);
    EXPECT_NEAR(static_cast<double>(a.size()), static_cast<double>(b.size()), 6.0);
    const float spacing = 0.7f * std::sqrt(1.0f / 40.0f);
    std::size_t stayed = 0;
    for (const Vec3f& r : a) {
        float nearest = 1e9f;
        for (const Vec3f& q : b) nearest = std::min(nearest, (q - r).length());
        if (nearest < spacing * 0.5f) ++stayed;
    }
    EXPECT_GE(stayed, a.size() * 7 / 10);  // most centers within half a spacing of where they were
}

TEST(NoiseOp, WorldIsCoherentStrandIsIndependent) {
    LookGraph lg;
    const NodeId noise = lg.add(op::kNoise, lg.scatter);
    lg.g.node(noise).params().set("amplitude", 0.1);
    lg.g.node(noise).params().set("space", std::string("world"));
    const StrandsPtr before = lg.eval(lg.scatter);
    const StrandsPtr world = lg.eval(noise);
    EXPECT_GT(maxDisplacement(*world, *before), 0.01f);
    EXPECT_LE(maxDisplacement(*world, *before), 0.1f * 1.2f * std::sqrt(3.0f));
    for (std::size_t s = 0; s < world->strandCount(); ++s) EXPECT_EQ(world->strandPoints(s)[0], before->strandPoints(s)[0]);

    // Neighbouring strands move alike in world space, not in strand space.
    auto neighbourSpread = [&](const StrandSet& out) {
        float spread = 0.0f;
        int pairs = 0;
        for (std::size_t s = 0; s + 1 < out.strandCount() && pairs < 200; ++s) {
            const Vec3f& r0 = before->strandPoints(s)[0];
            for (std::size_t t = s + 1; t < out.strandCount(); ++t) {
                if ((before->strandPoints(t)[0] - r0).length() > 0.02f) continue;
                const std::size_t n = out.strandPointCount(s);
                const Vec3f d0 = out.strandPoints(s)[n - 1] - before->strandPoints(s)[n - 1];
                const Vec3f d1 = out.strandPoints(t)[n - 1] - before->strandPoints(t)[n - 1];
                spread += (d0 - d1).length();
                ++pairs;
                break;
            }
        }
        return pairs ? spread / pairs : 0.0f;
    };
    const float worldSpread = neighbourSpread(*world);
    lg.g.node(noise).params().set("space", std::string("strand"));
    const float strandSpread = neighbourSpread(*lg.eval(noise));
    EXPECT_LT(worldSpread * 3.0f, strandSpread);
}

TEST(Curl, WrapsAtTheGivenRadius) {
    LookGraph lg;
    const NodeId curl = lg.add(op::kCurl, lg.scatter);
    lg.g.node(curl).params().set("radius", 0.05);
    lg.g.node(curl).params().set("randomness", 0.0);
    lg.g.node(curl).params().set("profile", Ramp::constant(1.0f));
    const StrandsPtr before = lg.eval(lg.scatter);
    const StrandsPtr out = lg.eval(curl);
    // The radius everywhere past the first quarter turn (the root stays: the curl eases in from it).
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        ASSERT_EQ(out->strandPointCount(s), before->strandPointCount(s));
        EXPECT_EQ(out->strandPoints(s)[0], before->strandPoints(s)[0]);
        float arc = 0.0f;
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
            arc += (before->strandPoints(s)[i] - before->strandPoints(s)[i - 1]).length();
            EXPECT_NEAR((out->strandPoints(s)[i] - before->strandPoints(s)[i]).length(), 0.05f * test::curlEase(arc, static_cast<float>(i) / static_cast<float>(out->strandPointCount(s) - 1), 0.4f), 1e-4f);
        }
    }
}

TEST(Curl, CurlsTurnWithTheScalp) {
    // Strands rooted on a sphere, leaning off their normals, their first segments about the
    // diagonal where two world axes are equally aligned with them (where anyPerpendicular's
    // choice of axis, the frames' old seed, flips): the scene turned rigidly by a few degrees
    // across it, the curls turn with it (their offsets, turned back, the same), as a head
    // turning keeps its curls, rather than some strands curling in another plane.
    const std::shared_ptr<const ScalpMesh> scalp = test::makeSphere(24, 48, 1.0f);
    const float angle = 0.0873f;  // 5 degrees about x
    auto turned = [](const Vec3f& p, float a) { return rotateAxisAngle(p, Vec3f(1, 0, 0), a); };
    struct Scene {
        MeshPtr mesh;
        std::shared_ptr<StrandSet> strands;
    };
    auto scene = [&](float a) {
        std::vector<Vec3f> points = scalp->points();
        for (Vec3f& p : points) p = turned(p, a);
        Scene s{std::make_shared<const ScalpMesh>(std::move(points), scalp->triangles()), std::make_shared<StrandSet>()};
        for (int k = 0; k < 8; ++k) {
            const Vec3f d = Vec3f(0.75f, 0.435f + 0.01f * static_cast<float>(k), 0.46f).normalized();  // y about z
            const Vec3f n = (d + Vec3f(-0.3f, 0.1f, 0.2f)).normalized();  // the root's way: 20 degrees off d
            const StrandRoot root = scalp->closestPoint(n).root;         // the same root in both scenes
            std::vector<Vec3f> pts;
            for (int i = 0; i <= 8; ++i) pts.push_back(s.mesh->position(root) + turned(d, a) * (0.6f * static_cast<float>(i) / 8.0f));
            s.strands->addStrand(static_cast<StrandId>(k + 1), pts, {}, root);
        }
        return s;
    };
    auto curled = [&](const Scene& s) {
        auto curl = OperatorRegistry::instance().create(op::kCurl);
        curl->params().set("radius", 0.05);
        curl->params().set("randomness", 0.0);
        curl->params().set("phaseRandom", 0.0);
        curl->params().set("profile", Ramp::constant(1.0f));
        ExternalInputs ext;
        ext.set("scalp", s.mesh, s.mesh->contentHash());
        EvalContext ctx;
        ctx.externals = &ext;
        return valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(StrandsPtr(s.strands))}));
    };
    const Scene a = scene(0.0f), b = scene(angle);
    const StrandsPtr ca = curled(a), cb = curled(b);
    ASSERT_TRUE(ca && cb);
    for (std::size_t s = 0; s < 8; ++s) {
        for (std::size_t i = 1; i < 9; ++i) {
            const Vec3f offsetA = ca->strandPoints(s)[i] - a.strands->strandPoints(s)[i];
            const Vec3f offsetB = turned(cb->strandPoints(s)[i] - b.strands->strandPoints(s)[i], -angle);
            // (Eased in over the first quarter of the strand.)
            EXPECT_NEAR(offsetA.length(), 0.05f * test::curlEase(0.6f * static_cast<float>(i) / 8.0f, static_cast<float>(i) / 8.0f, 0.4f), 1e-3f)
                << "strand " << s << " point " << i;
            EXPECT_NEAR(offsetA.x, offsetB.x, 1e-4f) << "strand " << s << " point " << i;
            EXPECT_NEAR(offsetA.y, offsetB.y, 1e-4f) << "strand " << s << " point " << i;
            EXPECT_NEAR(offsetA.z, offsetB.z, 1e-4f) << "strand " << s << " point " << i;
        }
    }
}

TEST(Noise, AFractionOfTheStrandsFrizz) {
    LookGraph lg;
    const NodeId frizz = lg.add(op::kNoise, lg.scatter);
    lg.g.node(frizz).params().set("frequency", 8.0);
    lg.g.node(frizz).params().set("octaves", std::int64_t{1});
    const StrandsPtr before = lg.eval(lg.scatter);
    lg.g.node(frizz).params().set("fraction", 0.0);
    EXPECT_NEAR(maxDisplacement(*lg.eval(frizz), *before), 0.0f, 1e-7f);

    lg.g.node(frizz).params().set("fraction", 0.3);
    const StrandsPtr out = lg.eval(frizz);
    std::size_t moved = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        if ((out->strandPoints(s)[n - 1] - before->strandPoints(s)[n - 1]).length() > 1e-6f) ++moved;
    }
    EXPECT_NEAR(static_cast<double>(moved) / out->strandCount(), 0.3, 0.07);
}

TEST(Attribute, RandomAndConstantChannels) {
    LookGraph lg;
    const NodeId attr = lg.add(op::kAttribute, lg.scatter);
    lg.g.node(attr).params().set("name", std::string("melanin"));
    // A random value per strand: an expression of rand, in [0.2, 0.4].
    MapRef random;
    random.source = MapRef::Source::Expression;
    random.name = "rand";
    random.min = 0.2f;
    random.max = 0.4f;
    lg.g.node(attr).params().set("value", random);
    const auto* ch = lg.eval(attr)->strandChannel("melanin");
    ASSERT_NE(ch, nullptr);
    std::set<float> distinct;
    for (float v : *ch) {
        EXPECT_GE(v, 0.2f);
        EXPECT_LE(v, 0.4f);
        distinct.insert(v);
    }
    EXPECT_GT(distinct.size(), ch->size() / 2);

    const std::vector<float> first = *ch;
    // So does mix(0.2, 0.4, rand), alike.
    random.name = "mix(0.2, 0.4, rand)";
    random.min = 0.0f;
    random.max = 1.0f;
    lg.g.node(attr).params().set("value", random);
    const std::vector<float> mixed = *lg.eval(attr)->strandChannel("melanin");
    ASSERT_EQ(mixed.size(), first.size());
    for (std::size_t s = 0; s < mixed.size(); ++s) EXPECT_NEAR(mixed[s], first[s], 1e-5f);
    // A number: the same on every strand.
    lg.g.node(attr).params().set("value", 0.7);
    for (float v : *lg.eval(attr)->strandChannel("melanin")) EXPECT_FLOAT_EQ(v, 0.7f);
}

TEST(MergeSwitch, CombineAndChoose) {
    LookGraph lg;
    const NodeId curl = lg.add(op::kCurl, lg.scatter);
    const NodeId merge = lg.add(op::kMerge, lg.scatter);
    lg.g.connect(curl, merge, 1);
    const std::size_t n = lg.eval(lg.scatter)->strandCount();
    EXPECT_EQ(lg.eval(merge)->strandCount(), 2 * n);
    EXPECT_TRUE(lg.eval(merge)->validate());

    const NodeId sw = lg.add(op::kSwitch, lg.scatter);
    lg.g.connect(curl, sw, 1);
    EXPECT_EQ(lg.eval(sw)->contentHash(), lg.eval(lg.scatter)->contentHash());
    lg.g.node(sw).params().set("index", std::int64_t{1});
    EXPECT_EQ(lg.eval(sw)->contentHash(), lg.eval(curl)->contentHash());
}

TEST(Scatter, APreviewTooBigForMemoryStopsWithAMessage) {
    // 100 units² x 400k/unit² x 0.5 preview x 257 points = 5G points: an error, not an allocation.
    LookGraph lg(400000.0);
    lg.ext.set("scalp", test::makePlane(8, 10.0f), 2);
    lg.segments(256);
    EvalContext ctx;
    ctx.externals = &lg.ext;
    ctx.previewDensity = 0.5f;
    try {
        lg.g.evaluateStrands(lg.scatter, ctx);
        FAIL() << "expected a GraphError";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("lower the preview density"), std::string::npos) << e.what();
    }
}

TEST(Scatter, AViewportPointBudgetThinsThePreview) {
    // 100 units² x 4000/unit² x 51 points = 20M points at full preview; a 2M budget keeps a tenth.
    LookGraph lg(4000.0);
    lg.ext.set("scalp", test::makePlane(8, 10.0f), 2);
    lg.segments(50);
    EvalContext ctx;
    ctx.externals = &lg.ext;
    ctx.previewDensity = 1.0f;
    ctx.maxPoints = 2.0e6;
    const StrandsPtr thin = lg.g.evaluateStrands(lg.scatter, ctx);
    EXPECT_LE(thin->pointCount(), 2.1e6);
    EXPECT_GE(thin->pointCount(), 1.8e6);
    // The same strands as a plain 10% preview: stable IDs, no pop when the budget kicks in.
    EvalContext tenth;
    tenth.externals = &lg.ext;
    tenth.previewDensity = static_cast<float>(2.0e6 / (100.0 * 4000.0 * 51.0));
    EXPECT_EQ(lg.g.evaluateStrands(lg.scatter, tenth)->strandCount(), thin->strandCount());
}

TEST(Blend, MixesTwoLooksOfTheSameStrands) {
    LookGraph lg;
    const NodeId curl = lg.add(op::kCurl, lg.scatter);
    const NodeId blend = lg.add(op::kBlend, lg.scatter);
    lg.g.connect(curl, blend, 1);
    const StrandsPtr dry = lg.eval(lg.scatter), wet = lg.eval(curl);

    // keepLength (the default): midway between straight and curled, each strand as long as both
    // (the points blended pull it in); a mix of 0 or 1 gives back that look to the bit.
    auto lengthOf = [](const StrandsPtr& s, std::size_t i) { return arcLength(s->strandPoints(i), s->strandPointCount(i)); };
    lg.g.node(blend).params().set("mix", 0.5);
    const StrandsPtr kept = lg.eval(blend);
    for (std::size_t i = 0; i < kept->strandCount(); i += 9) {
        EXPECT_NEAR(lengthOf(kept, i), 0.5f * (lengthOf(dry, i) + lengthOf(wet, i)), 1e-3f * lengthOf(dry, i)) << i;
    }
    for (const double mix : {0.0, 1.0}) {
        lg.g.node(blend).params().set("mix", mix);
        EXPECT_EQ(lg.eval(blend)->positions(), (mix == 0.0 ? dry : wet)->positions()) << mix;
    }
    // Off: the points blended.
    lg.g.node(blend).params().set("keepLength", false);
    lg.g.node(blend).params().set("mix", 0.0);
    EXPECT_EQ(lg.eval(blend)->positions(), dry->positions());
    lg.g.node(blend).params().set("mix", 1.0);
    EXPECT_EQ(lg.eval(blend)->positions(), wet->positions());
    lg.g.node(blend).params().set("mix", 0.5);
    const StrandsPtr half = lg.eval(blend);
    for (std::size_t i = 0; i < half->pointCount(); i += 7) {
        const Vec3f mid = (dry->positions()[i] + wet->positions()[i]) * 0.5f;
        EXPECT_LT((half->positions()[i] - mid).length(), 1e-5f);
    }
    ASSERT_NE(half->strandChannel("blend"), nullptr);
    EXPECT_FLOAT_EQ(half->strandChannel("blend")->front(), 0.5f);

    // A profile of 0 at the root keeps roots on a.
    Ramp tips = Ramp::linear(0.0f, 1.0f);
    lg.g.node(blend).params().set("profile", tips);
    const StrandsPtr ends = lg.eval(blend);
    EXPECT_EQ(ends->strandPoints(3)[0], dry->strandPoints(3)[0]);

    // Different strands: a clear error.
    const NodeId merge = lg.add(op::kMerge, lg.scatter);
    lg.g.connect(lg.scatter, merge, 1);
    lg.g.connect(merge, blend, 1);
    EXPECT_THROW(lg.eval(blend), GraphError);
}

TEST(Blend, MixesASideWhoseCurlAddedPoints) {
    // A tight Curl adds points to its strands (blend_two_looks on a small creature): the same
    // strands with points of their own, each resampled to the more points either side has.
    LookGraph lg;
    const NodeId curl = lg.add(op::kCurl, lg.scatter);
    lg.g.node(curl).params().set("frequency", 12.0);
    lg.g.node(curl).params().set("radius", 0.02);
    const NodeId blend = lg.add(op::kBlend, lg.scatter);
    lg.g.connect(curl, blend, 1);
    const StrandsPtr plain = lg.eval(lg.scatter), curled = lg.eval(curl);
    ASSERT_GT(curled->pointCount(), plain->pointCount()) << "the curl adds points";

    lg.g.node(blend).params().set("mix", 1.0);
    const StrandsPtr all = lg.eval(blend);
    ASSERT_EQ(all->strandCount(), plain->strandCount());
    for (std::size_t s = 0; s < all->strandCount(); s += 5) {
        ASSERT_EQ(all->strandPointCount(s), std::max(plain->strandPointCount(s), curled->strandPointCount(s)));
        // The curled side's shape: its root and tip, and as long.
        const std::size_t n = all->strandPointCount(s), m = curled->strandPointCount(s);
        EXPECT_LT((all->strandPoints(s)[0] - curled->strandPoints(s)[0]).length(), 1e-5f);
        EXPECT_LT((all->strandPoints(s)[n - 1] - curled->strandPoints(s)[m - 1]).length(), 1e-4f);
        EXPECT_NEAR(arcLength(all->strandPoints(s), n), arcLength(curled->strandPoints(s), m),
                    1e-3f * arcLength(curled->strandPoints(s), m));
    }
    lg.g.node(blend).params().set("mix", 0.0);
    const StrandsPtr none = lg.eval(blend);
    for (std::size_t s = 0; s < none->strandCount(); s += 5) {
        const std::size_t n = none->strandPointCount(s), m = plain->strandPointCount(s);
        EXPECT_LT((none->strandPoints(s)[n - 1] - plain->strandPoints(s)[m - 1]).length(), 1e-4f);
    }
}

// ---------------------------------------------------------------------------

TEST(Performance, FullLookGraphAt100kStrands) {
    // Spec budget: 100k strands x 16 points through 6 operators in < 100 ms
    // (16-core reference machine). Asserted loosely here; the number is printed.
    auto scalp = test::makeSphere(60, 120, 1.0f);
    auto guides = std::make_shared<GuideSet>();
    Rng rng(4);
    for (int i = 0; i < 300; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        guides->add(static_cast<std::uint64_t>(i + 1), test::straightCurve(n, n * 0.8f + Vec3f(0.2f, -0.3f, 0), 12));
    }
    guides->rootOnMesh(*scalp);

    GraphDocument doc = loadGraphJson(R"({
      "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 8000}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "clump", "type": "Clump", "params": {"strength": 0.7, "count": 400}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.02}},
        {"name": "curl", "type": "Curl"},
        {"name": "width", "type": "Width"},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 15}}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interp", "input": "strands"},
        {"from": "guides", "to": "interp", "input": "guides"},
        {"from": "scalp", "to": "interp", "input": "scalp"},
        {"from": "interp", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "noise", "input": "strands"},
        {"from": "noise", "to": "curl", "input": "strands"},
        {"from": "curl", "to": "width", "input": "strands"},
        {"from": "width", "to": "out", "input": "strands"}],
      "output": "out"})");
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(scalp), 1);
    ext.set("guides", GuidesPtr(guides), 2);
    EvalContext ctx;
    ctx.externals = &ext;

    const auto t0 = std::chrono::steady_clock::now();
    const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[ perf ] %zu strands x 16 points, 7 operators: %.1f ms\n", out->strandCount(), ms);

    // Per-operator breakdown: evaluate the chain node by node from a cold cache,
    // so each step times only the newly evaluated node.
    doc.graph.clearCache();
    for (const char* name : {"scatter", "interp", "clump", "noise", "curl", "width", "out"}) {
        const auto s0 = std::chrono::steady_clock::now();
        doc.graph.evaluate(doc.node(name), ctx);
        std::printf("[ perf ]   %-8s %6.1f ms\n", name,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
    }
    RecordProperty("full_graph_ms", std::to_string(ms));
    EXPECT_GT(out->strandCount(), 90000u);
    EXPECT_LT(ms, 400.0);  // loose (measured ~90 ms here, 24 cores): machines and build types vary
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(ToGuides, StrandsBecomeGuidesForClumping) {
    LookGraph lg(400.0);
    const NodeId attr = lg.add(op::kAttribute, lg.scatter);
    lg.g.node(attr).params().set("name", std::string("tag"));
    const NodeId toGuides = lg.add(op::kToGuides, attr);
    lg.g.node(toGuides).params().set("fraction", 0.1);
    const StrandsPtr strands = lg.eval(attr);
    const GuidesPtr guides = std::get<GuidesPtr>(lg.evalValue(toGuides));
    ASSERT_TRUE(guides);
    EXPECT_GT(guides->size(), strands->strandCount() / 20);  // about a tenth, by ID
    EXPECT_LT(guides->size(), strands->strandCount() / 5);
    std::set<std::uint64_t> ids(strands->ids().begin(), strands->ids().end());
    for (const Guide& g : guides->guides()) {
        EXPECT_TRUE(ids.count(g.id));
        EXPECT_EQ(g.points.size(), 16u);
        EXPECT_EQ(g.attributes.count("tag"), 1u);  // channels come along
    }

    // Procedural guides as clump centers: strands pull toward them.
    const NodeId clump = lg.g.addNode(op::kClump);
    lg.g.connect(lg.scatter, clump, 0);
    lg.g.connect(toGuides, clump, 1);
    lg.g.node(clump).params().set("centersFrom", std::string("inputPoints"));
    const StrandsPtr clumped = lg.eval(clump);
    EXPECT_EQ(clumped->strandCount(), strands->strandCount());
    EXPECT_GT(maxDisplacement(*lg.eval(lg.scatter), *clumped), 0.0f);
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(Instances, ToMeshMakesEveryInstancesMeshWhereItSits) {
    // An Instance node's instances: auto makes their prototypes' meshes, each moved to where it
    // sits (the matrix's rows); a host's prototype (a square of two triangles) comes out a quad,
    // one without a mesh is left out.
    LookGraph lg(200.0);
    const NodeId inst = lg.add(op::kInstance, lg.scatter);
    lg.g.node(inst).params().set("probability", 1.0);
    lg.g.node(inst).params().set("orient", std::string("up"));
    lg.g.node(inst).params().set("prototype", std::string("square"));
    const StrandsPtr s = lg.eval(inst);
    ASSERT_TRUE(s->instances() && !s->instances()->empty());
    StrandMeshOptions o;
    EXPECT_EQ(resolveMeshMake(*s, o), StrandMeshOptions::Make::Instances);
    EXPECT_EQ(buildStrandMesh(*s, nullptr, o).mesh.faceCount(), 0u) << "no mesh for 'square': none made";
    InstancePrototype square;
    square.found = true;
    square.points = {{-0.5f, 0, 0}, {0.5f, 0, 0}, {0.5f, 1, 0}, {-0.5f, 1, 0}};
    square.triangles = {0, 1, 2, 0, 2, 3};
    square.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const PrototypeLookup lookup = [&square](const std::string& name) { return name == "square" ? &square : nullptr; };
    const StrandMesh mesh = buildStrandMesh(*s, nullptr, o, lookup);
    const auto placed = s->instances()->resolve(*s);
    ASSERT_EQ(mesh.mesh.faceCount(), placed.size());
    EXPECT_TRUE(std::all_of(mesh.mesh.faceCounts.begin(), mesh.mesh.faceCounts.end(), [](int n) { return n == 4; }));
    EXPECT_EQ(mesh.faceParts.front(), static_cast<int>(MeshPart::Instances));
    ASSERT_EQ(mesh.mesh.points.size(), 4 * placed.size());
    const InstanceSet::Placed& p = placed[3];
    const Vec3f corner = p.origin + p.x * 0.5f + p.y * 1.0f;  // the square's (0.5, 1, 0)
    EXPECT_LT((mesh.mesh.points[4 * 3 + 2] - corner).length(), 1e-5f);
    EXPECT_FLOAT_EQ(mesh.mesh.u[4 * 3 + 2], 1.0f);
}

TEST(Instances, PlacedOnTheStrandsAndCarriedDownstream) {
    LookGraph lg(200.0);
    const NodeId inst = lg.add(op::kInstance, lg.scatter);
    lg.g.node(inst).params().set("probability", 1.0);
    lg.g.node(inst).params().set("count", std::int64_t{2});
    lg.g.node(inst).params().set("tMin", 0.8);
    lg.g.node(inst).params().set("orient", std::string("up"));
    lg.g.node(inst).params().set("prototype", std::string("snowflake"));
    const StrandsPtr s = lg.eval(inst);
    ASSERT_TRUE(s->instances());
    EXPECT_EQ(s->instances()->size(), 2 * s->strandCount());
    EXPECT_EQ(s->instances()->prototypes, std::vector<std::string>{"snowflake"});
    const auto placed = s->instances()->resolve(*s);
    ASSERT_EQ(placed.size(), 2 * s->strandCount());
    for (const auto& p : placed) {
        EXPECT_GE(p.origin.y, 0.8f - 1e-4f);  // scatter strands stand up 1 unit: t in [0.8, 1]
        EXPECT_LE(p.origin.y, 1.0f + 1e-4f);
        EXPECT_NEAR(p.y.normalized().y, 1.0f, 1e-4f);  // orient up
    }

    // A node after Instance (Curl) moves the strands: the instances follow.
    const NodeId curl = lg.add(op::kCurl, inst);
    lg.g.node(curl).params().set("radius", 0.2);
    const StrandsPtr curled = lg.eval(curl);
    ASSERT_TRUE(curled->instances());
    const auto moved = curled->instances()->resolve(*curled);
    ASSERT_EQ(moved.size(), placed.size());
    float shift = 0.0f;
    for (std::size_t i = 0; i < moved.size(); ++i) shift = std::max(shift, (moved[i].origin - placed[i].origin).length());
    EXPECT_GT(shift, 0.05f);

    // Preview density drops the instances of the dropped strands.
    const StrandSet preview = curled->withDensity(0.25f);
    EXPECT_LT(curled->instances()->resolve(preview).size(), moved.size() / 2);

    // Merge keeps both sides' instances.
    const NodeId other = lg.add(op::kInstance, lg.scatter);
    lg.g.node(other).params().set("probability", 1.0);
    lg.g.node(other).params().set("prototype", std::string("bead"));
    const NodeId merge = lg.g.addNode(op::kMerge);
    lg.g.connect(inst, merge, 0);
    lg.g.connect(other, merge, 1);
    const StrandsPtr merged = lg.eval(merge);
    EXPECT_EQ(merged->instances()->prototypes, (std::vector<std::string>{"snowflake", "bead"}));
    EXPECT_EQ(merged->instances()->size(), 3 * s->strandCount());
}

// One strand up the y axis, 2 long, widths 1 -> 0.5 -> 0.25, with an instance at t.
StrandSet taperedStrand(float t, bool scaleByWidth, std::vector<float> widths = {1.0f, 0.5f, 0.25f}) {
    StrandSet s;
    s.addStrand(5, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}}, widths);
    auto instances = std::make_shared<InstanceSet>();
    InstanceSet::Instance i;
    i.strand = 5;
    i.id = 9;
    i.t = t;
    i.scale = 2.0f;
    i.orient = InstanceOrient::Up;
    i.scaleByWidth = scaleByWidth;
    instances->instances.push_back(i);
    s.setInstances(instances);
    return s;
}

TEST(Instances, ScaleByWidthFollowsTheWidthProfile) {
    // On: the width where it sits over the root width.
    auto placed = taperedStrand(0.5f, true).instances()->resolve(taperedStrand(0.5f, true));
    ASSERT_EQ(placed.size(), 1u);
    EXPECT_NEAR(placed[0].origin.y, 1.0f, 1e-5f);
    EXPECT_NEAR(placed[0].x.length(), 2.0f * 0.5f, 1e-5f);
    EXPECT_NEAR(placed[0].y.length(), 2.0f * 0.5f, 1e-5f);
    const StrandSet at75 = taperedStrand(0.75f, true);
    EXPECT_NEAR(at75.instances()->resolve(at75)[0].x.length(), 2.0f * 0.375f, 1e-5f);  // between points: interpolated
    const StrandSet atRoot = taperedStrand(0.0f, true);
    EXPECT_NEAR(atRoot.instances()->resolve(atRoot)[0].x.length(), 2.0f, 1e-5f);
    // Off: the scale as it is, wherever it sits.
    const StrandSet off = taperedStrand(0.75f, false);
    EXPECT_NEAR(off.instances()->resolve(off)[0].x.length(), 2.0f, 1e-5f);
    // A zero root width: no factor.
    const StrandSet zeroRoot = taperedStrand(0.75f, true, {0.0f, 0.5f, 0.25f});
    EXPECT_NEAR(zeroRoot.instances()->resolve(zeroRoot)[0].x.length(), 2.0f, 1e-5f);
}

TEST(Instances, PinnedToRestSitAtTheirRestDistance) {
    StrandSet s = taperedStrand(0.5f, false);
    auto instances = std::make_shared<InstanceSet>(*s.instances());
    instances->instances[0].pinToRest = true;
    s.setInstances(instances);
    // No rest distance yet: at t, as unpinned.
    EXPECT_NEAR(s.instances()->resolve(s)[0].origin.y, 1.0f, 1e-5f);
    instances->instances[0].restDistance = 0.4f;
    EXPECT_NEAR(s.instances()->resolve(s)[0].origin.y, 0.4f, 1e-5f);
    instances->instances[0].restDistance = 5.0f;  // longer than the strand is now: at the tip
    EXPECT_NEAR(s.instances()->resolve(s)[0].origin.y, 2.0f, 1e-5f);
    EXPECT_TRUE(s.instances()->needsRestPose());

    // pinRestDistances: t times the rest strand's length, matched by strand ID.
    StrandSet rest;
    rest.addStrand(5, {{0, 0, 0}, {0, 0.6f, 0}});
    instances->instances[0].restDistance = -1.0f;
    const StrandsPtr pinned = pinRestDistances(std::make_shared<StrandSet>(s), rest);
    EXPECT_NEAR(pinned->instances()->instances[0].restDistance, 0.3f, 1e-6f);
    EXPECT_NEAR(pinned->instances()->resolve(*pinned)[0].origin.y, 0.3f, 1e-5f);
    StrandSet other;
    other.addStrand(6, {{0, 0, 0}, {0, 0.6f, 0}});  // not the strand: stays at its fraction
    EXPECT_LT(pinRestDistances(std::make_shared<StrandSet>(s), other)->instances()->instances[0].restDistance, 0.0f);
}

TEST(Instances, FittedToTheStrandSpanItsRestLength) {
    // The tapered strand is 2 long up +Y. Fitted, scale 0.5, aspect 0.25, at the root.
    auto fitted = [](float t, bool scaleByWidth, float restLength) {
        StrandSet s = taperedStrand(t, scaleByWidth);
        auto instances = std::make_shared<InstanceSet>(*s.instances());
        InstanceSet::Instance& i = instances->instances[0];
        i.orient = InstanceOrient::Strand;
        i.scale = 0.5f;
        i.fitStrand = true;
        i.aspect = 0.25f;
        i.restLength = restLength;
        s.setInstances(instances);
        return s;
    };
    // No rest length yet: the length now (2), half of it along +Y (the strand), a quarter of that across.
    StrandSet s = fitted(0.0f, true, -1.0f);
    auto p = s.instances()->resolve(s)[0];
    EXPECT_TRUE(s.instances()->needsRestPose());  // the host fills the rest length
    EXPECT_NEAR(p.y.length(), 1.0f, 1e-5f);
    EXPECT_NEAR(p.y.normalized().y, 1.0f, 1e-5f);  // +Y along the strand
    EXPECT_NEAR(p.x.length(), 0.25f, 1e-5f);
    EXPECT_NEAR(p.z.length(), 0.25f, 1e-5f);
    EXPECT_NEAR(p.x.dot(p.y), 0.0f, 1e-6f);  // at right angles, however stretched
    EXPECT_NEAR(p.y.dot(p.z), 0.0f, 1e-6f);
    EXPECT_NEAR(p.z.dot(p.x), 0.0f, 1e-6f);
    // A rest length: from it, whatever the strand's length now.
    s = fitted(0.0f, true, 3.0f);
    p = s.instances()->resolve(s)[0];
    EXPECT_NEAR(p.y.length(), 1.5f, 1e-5f);
    EXPECT_NEAR(p.x.length(), 0.375f, 1e-5f);
    // The width factor narrows it across only (half the root width at t 0.5).
    s = fitted(0.5f, true, 3.0f);
    p = s.instances()->resolve(s)[0];
    EXPECT_NEAR(p.y.length(), 1.5f, 1e-5f);
    EXPECT_NEAR(p.x.length(), 0.375f * 0.5f, 1e-5f);
    EXPECT_NEAR(p.z.length(), 0.375f * 0.5f, 1e-5f);
    s = fitted(0.5f, false, 3.0f);
    EXPECT_NEAR(s.instances()->resolve(s)[0].x.length(), 0.375f, 1e-5f);
    // Fixed (not fitted): the aspect and rest length do nothing.
    StrandSet fixed = fitted(0.5f, true, 3.0f);
    auto instances = std::make_shared<InstanceSet>(*fixed.instances());
    instances->instances[0].fitStrand = false;
    fixed.setInstances(instances);
    EXPECT_FALSE(fixed.instances()->needsRestPose());
    p = fixed.instances()->resolve(fixed)[0];
    EXPECT_NEAR(p.x.length(), 0.25f, 1e-5f);  // 0.5 times the width factor 0.5
    EXPECT_NEAR(p.y.length(), 0.25f, 1e-5f);

    // pinRestDistances fills the fitted instance's rest length (and no rest distance: not pinned).
    StrandSet rest;
    rest.addStrand(5, {{0, 0, 0}, {0, 0.6f, 0}, {0, 1.2f, 0}});
    const StrandsPtr filled = pinRestDistances(std::make_shared<StrandSet>(fitted(0.0f, true, -1.0f)), rest);
    EXPECT_NEAR(filled->instances()->instances[0].restLength, 1.2f, 1e-6f);
    EXPECT_LT(filled->instances()->instances[0].restDistance, 0.0f);
    EXPECT_NEAR(filled->instances()->resolve(*filled)[0].y.length(), 0.6f, 1e-5f);
}

TEST(Instances, NodeSizeModeFitStrand) {
    LookGraph lg(200.0);
    const NodeId inst = lg.add(op::kInstance, lg.scatter);
    lg.g.node(inst).params().set("probability", 1.0);
    lg.g.node(inst).params().set("scaleRandom", 0.0);
    lg.g.node(inst).params().set("aspect", 0.3);
    lg.g.node(inst).params().set("fitScale", 0.5);
    const StrandsPtr fixed = lg.eval(inst);
    for (const auto& i : fixed->instances()->instances) {
        EXPECT_FALSE(i.fitStrand);  // fixed by default: aspect and fitScale unused
        EXPECT_FLOAT_EQ(i.scale, 0.02f);
    }
    EXPECT_FALSE(fixed->instances()->needsRestPose());
    lg.g.node(inst).params().set("sizeMode", std::string("fitStrand"));
    const StrandsPtr fitted = lg.eval(inst);
    EXPECT_NE(fitted->instances()->contentHash(), fixed->instances()->contentHash());
    EXPECT_TRUE(fitted->instances()->needsRestPose());
    for (const auto& i : fitted->instances()->instances) {
        EXPECT_TRUE(i.fitStrand);
        EXPECT_FLOAT_EQ(i.scale, 0.5f);  // fitScale, a fraction
        EXPECT_FLOAT_EQ(i.aspect, 0.3f);
    }
    // Scatter strands stand up 1 unit: resolved without a rest pose, the length now.
    const auto placed = fitted->instances()->resolve(*fitted);
    ASSERT_FALSE(placed.empty());
    for (const auto& p : placed) {
        EXPECT_NEAR(p.y.length(), 0.5f, 1e-4f);
        EXPECT_NEAR(p.x.length(), 0.5f * 0.3f, 1e-4f);  // scatter strands have no width profile
    }
    lg.g.node(inst).params().set("sizeMode", std::string("stretchy"));
    EXPECT_THROW(lg.eval(inst), GraphError);
}

TEST(Instances, NodeSetsScaleByWidthAndPinToRest) {
    LookGraph lg(200.0);
    const NodeId inst = lg.add(op::kInstance, lg.scatter);
    lg.g.node(inst).params().set("probability", 1.0);
    const StrandsPtr defaults = lg.eval(inst);
    ASSERT_TRUE(defaults->instances() && !defaults->instances()->empty());
    for (const auto& i : defaults->instances()->instances) {
        EXPECT_TRUE(i.scaleByWidth);  // on by default
        EXPECT_FALSE(i.pinToRest);    // off by default
    }
    lg.g.node(inst).params().set("scaleByWidth", false);
    lg.g.node(inst).params().set("pinToRest", true);
    const StrandsPtr set = lg.eval(inst);
    EXPECT_NE(set->instances()->contentHash(), defaults->instances()->contentHash());
    for (const auto& i : set->instances()->instances) {
        EXPECT_FALSE(i.scaleByWidth);
        EXPECT_TRUE(i.pinToRest);
    }
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

// Strands on a plane with one braid path standing up from (0.5, 0, 0.5).
struct BraidScene {
    LookGraph lg{3000.0, 16};
    NodeId braid = 0, guides = 0;
    std::shared_ptr<GuideSet> paths = std::make_shared<GuideSet>();
    std::uint64_t version = 5;
    explicit BraidScene(const std::string& style, std::vector<Vec3f> path = {{0.5f, 0, 0.5f}, {0.5f, 1.0f, 0.5f}}) {
        addPath(std::move(path));
        lg.segments(24);
        guides = lg.g.addNode(op::kGuides);
        braid = lg.add(op::kBraid, lg.scatter);
        lg.g.connect(guides, braid, 1);
        lg.g.node(braid).params().set("style", style);
        lg.g.node(braid).params().set("strays", 0.0);
    }
    void addPath(std::vector<Vec3f> path) {
        paths->add(paths->size() + 1, std::move(path));
        lg.ext.set("guides", GuidesPtr(std::make_shared<GuideSet>(*paths)), ++version);
    }
    void setParam(const char* name, ParamValue value) { lg.g.node(braid).params().set(name, std::move(value)); }
    // A unit sphere head instead of the plane, with one path standing up from its top.
    void sphere() {
        lg.ext.set("scalp", test::makeSphere(24, 48, 1.0f), 2);
        lg.g.node(lg.scatter).params().set("density", 400.0);
        paths = std::make_shared<GuideSet>();
        addPath({{0.0f, 1.0f, 0.0f}, {0.0f, 2.0f, 0.0f}});
    }
};

bool strandMoved(const StrandSet& before, const StrandSet& after, std::size_t s) {
    if (after.strandPointCount(s) != before.strandPointCount(s)) return true;  // points added
    for (std::size_t i = 0; i < after.strandPointCount(s); ++i) {
        if (!(after.strandPoints(s)[i] == before.strandPoints(s)[i])) return true;
    }
    return false;
}

TEST(Braid, GathersTheRegionIntoLobesAroundThePath) {
    BraidScene scene("three");
    const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
    const StrandsPtr after = scene.lg.eval(scene.braid);
    ASSERT_EQ(after->strandCount(), before->strandCount());
    std::size_t braided = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        const Vec3f r = before->strandPoints(s)[0];
        const float d = std::hypot(r.x - 0.5f, r.z - 0.5f);
        const Vec3f mid = after->strandPoints(s)[12];  // half way up
        if (d > 0.26f) {
            EXPECT_EQ(mid, before->strandPoints(s)[12]);  // outside the region: untouched
            continue;
        }
        if (d > 0.24f) continue;  // at the region's edge
        ++braided;
        EXPECT_EQ(after->strandPoints(s)[0], r);  // roots stay
        EXPECT_LT(std::hypot(mid.x - 0.5f, mid.z - 0.5f), 0.16f);  // within the braid: lobes reach 0.55 + lobeFill widths
    }
    EXPECT_GT(braided, 200u);
}

TEST(Braid, EveryStyleAndCornrowsStayAboveTheScalp) {
    for (const char* style : {"three", "fishtail", "rope"}) {
        BraidScene scene(style);
        EXPECT_NO_THROW(scene.lg.eval(scene.braid)) << style;
    }
    // A cornrow along the scalp: braided strands stay above it.
    BraidScene row("cornrow", {{0.2f, 0.01f, 0.5f}, {0.8f, 0.01f, 0.5f}});
    row.lg.g.node(row.braid).params().set("region", 0.12);
    row.lg.g.node(row.braid).params().set("tail", 0.0);
    const StrandsPtr before = row.lg.eval(row.lg.scatter);
    const StrandsPtr after = row.lg.eval(row.braid);
    std::size_t checked = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        if (after->strandPoints(s)[12] == before->strandPoints(s)[12]) continue;  // not in the row
        for (std::size_t i = 6; i < after->strandPointCount(s); ++i) EXPECT_GT(after->strandPoints(s)[i].y, -1e-3f);
        ++checked;
    }
    EXPECT_GT(checked, 20u);
    BraidScene bad("zigzag");
    EXPECT_THROW(bad.lg.eval(bad.braid), GraphError);
}

TEST(Braid, SectionsDepthPhaseTightnessAndTwistShapeTheWeave) {
    // The braid's cross-section half way up: its extent across x and z (one is depth, one side to side).
    auto spread = [](BraidScene& scene) {
        const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
        const StrandsPtr after = scene.lg.eval(scene.braid);
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (std::size_t s = 0; s < after->strandCount(); ++s) {
            if (after->strandPoints(s)[12] == before->strandPoints(s)[12]) continue;
            const Vec3f m = after->strandPoints(s)[12];
            lo[0] = std::min(lo[0], m.x), hi[0] = std::max(hi[0], m.x);
            lo[1] = std::min(lo[1], m.z), hi[1] = std::max(hi[1], m.z);
        }
        const float a = hi[0] - lo[0], b = hi[1] - lo[1];
        return std::min(a, b) / std::max(a, b);
    };
    auto hashWith = [](const char* name, ParamValue value) {
        BraidScene scene("three");
        scene.lg.g.node(scene.braid).params().set(name, value);
        return scene.lg.eval(scene.braid)->contentHash();
    };
    BraidScene round("three"), flat("three");
    round.lg.g.node(round.braid).params().set("fuzz", 0.0);
    flat.lg.g.node(flat.braid).params().set("fuzz", 0.0);
    flat.lg.g.node(flat.braid).params().set("depth", 0.2);
    EXPECT_LT(spread(flat), 0.8f * spread(round));  // flatter across

    const std::uint64_t plain = hashWith("seed", std::int64_t{0});
    EXPECT_NE(hashWith("sections", std::int64_t{5}), plain);      // a five-strand braid
    EXPECT_EQ(hashWith("sections", std::int64_t{3}), plain);      // three is the default
    EXPECT_NE(hashWith("phase", 0.5), plain);
    EXPECT_NE(hashWith("sectionTwist", 1.0), plain);
    EXPECT_EQ(hashWith("crossingsProfile", Ramp::constant(1.0f)), plain);
    EXPECT_NE(hashWith("crossingsProfile", Ramp::linear(0.5f, 2.0f)), plain);
    for (const char* style : {"three", "rope", "cornrow", "fishtail"}) {  // every style takes more sections
        BraidScene scene(style);
        scene.lg.g.node(scene.braid).params().set("sections", std::int64_t{6});
        const StrandsPtr out = scene.lg.eval(scene.braid);
        EXPECT_TRUE(out->validate()) << style;
    }
}

TEST(Braid, FedStrandsEndWhereThePathEnds) {
    // A 0.6 long row picking up strands 1.0 long: fitted, none runs past the row's end.
    auto farthest = [](bool endAtPath) {
        BraidScene row("cornrow", {{0.2f, 0.01f, 0.5f}, {0.8f, 0.01f, 0.5f}});
        row.lg.g.node(row.braid).params().set("feed", true);
        row.lg.g.node(row.braid).params().set("region", 0.1);
        row.lg.g.node(row.braid).params().set("endAtPath", endAtPath);
        const StrandsPtr before = row.lg.eval(row.lg.scatter);
        const StrandsPtr after = row.lg.eval(row.braid);
        float tip = 0.0f;
        for (std::size_t s = 0; s < after->strandCount(); ++s) {
            const std::size_t n = after->strandPointCount(s);
            if (!strandMoved(*before, *after, s)) continue;  // not in the row
            tip = std::max(tip, after->strandPoints(s)[n - 1].x);
        }
        return tip;
    };
    EXPECT_LT(farthest(true), 0.8f + 0.2f);   // within the braid's width of its end
    EXPECT_GT(farthest(false), 1.2f);         // hanging on past it, as before
}

// A cornrow row along z = 0.5 from x = 0.2 to 0.8, fed.
BraidScene fedRow() {
    BraidScene row("cornrow", {{0.2f, 0.01f, 0.5f}, {0.8f, 0.01f, 0.5f}});
    row.setParam("feed", true);
    return row;
}

TEST(Braid, FeedDistanceIsThePickUpReachAndRegionTheRoots) {
    {
        BraidScene row = fedRow();
        row.setParam("feedDistance", 0.1);
        row.setParam("region", 0.05);
        const StrandsPtr before = row.lg.eval(row.lg.scatter);
        const StrandsPtr after = row.lg.eval(row.braid);
        std::size_t joined = 0, left = 0;
        for (std::size_t s = 0; s < after->strandCount(); ++s) {
            const Vec3f r = before->strandPoints(s)[0];
            const float dz = std::abs(r.z - 0.5f), dRoot = std::hypot(r.x - 0.2f, r.z - 0.5f);
            if (dz > 0.12f && dRoot > 0.07f) {  // beyond feedDistance of the row and region of its root
                EXPECT_FALSE(strandMoved(*before, *after, s)) << s;
                ++left;
            } else if (dz < 0.08f && r.x > 0.25f && r.x < 0.75f) {
                EXPECT_TRUE(strandMoved(*before, *after, s)) << s;
                ++joined;
            }
        }
        EXPECT_GT(joined, 100u);
        EXPECT_GT(left, 1000u);
    }
    {
        // The root region joins regardless of the (shorter) feed distance.
        BraidScene row = fedRow();
        row.setParam("feedDistance", 0.05);
        row.setParam("region", 0.15);
        const StrandsPtr before = row.lg.eval(row.lg.scatter);
        const StrandsPtr after = row.lg.eval(row.braid);
        std::size_t fromRoot = 0;
        for (std::size_t s = 0; s < after->strandCount(); ++s) {
            const Vec3f r = before->strandPoints(s)[0];
            const float dz = std::abs(r.z - 0.5f), dRoot = std::hypot(r.x - 0.2f, r.z - 0.5f);
            if (dz > 0.07f && dRoot < 0.14f) {  // too far from the row to be picked up, near its root
                EXPECT_TRUE(strandMoved(*before, *after, s)) << s;
                ++fromRoot;
            } else if (dz > 0.07f && dRoot > 0.16f) {
                EXPECT_FALSE(strandMoved(*before, *after, s)) << s;
            }
        }
        EXPECT_GT(fromRoot, 30u);
    }
}

TEST(Braid, FeedUntilStopsThePickUpPartWayAlongThePath) {
    BraidScene row = fedRow();
    row.setParam("feedDistance", 0.1);
    row.setParam("region", 0.05);
    row.setParam("feedUntil", 0.5);  // picked up along x <= 0.5 only
    const StrandsPtr before = row.lg.eval(row.lg.scatter);
    const StrandsPtr after = row.lg.eval(row.braid);
    std::size_t firstHalf = 0, secondHalf = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        const Vec3f r = before->strandPoints(s)[0];
        const float dz = std::abs(r.z - 0.5f);
        if (dz < 0.08f && r.x > 0.25f && r.x < 0.48f) {
            EXPECT_TRUE(strandMoved(*before, *after, s)) << s;
            ++firstHalf;
        } else if (r.x > 0.62f && r.x < 0.9f) {  // beyond feedDistance of the pick-up stretch's end
            EXPECT_FALSE(strandMoved(*before, *after, s)) << s;
            ++secondHalf;
        }
    }
    EXPECT_GT(firstHalf, 50u);
    EXPECT_GT(secondHalf, 300u);

    // feedUntil 0: only the root region joins (a ponytail braid along the path).
    BraidScene tail = fedRow();
    tail.setParam("feedUntil", 0.0);
    tail.setParam("region", 0.05);
    const StrandsPtr tailAfter = tail.lg.eval(tail.braid);
    std::size_t moved = 0;
    for (std::size_t s = 0; s < tailAfter->strandCount(); ++s) {
        const Vec3f r = before->strandPoints(s)[0];
        const float dRoot = std::hypot(r.x - 0.2f, r.z - 0.5f);
        if (dRoot > 0.07f) EXPECT_FALSE(strandMoved(*before, *tailAfter, s)) << s;
        moved += strandMoved(*before, *tailAfter, s) ? 1 : 0;
    }
    EXPECT_GT(moved, 5u);
}

TEST(Braid, PonytailSweepsOverTheScalpIntoTheBraid) {
    // A standing path in the middle of the plane gathering hair from far around it.
    {
        BraidScene scene("three");
        scene.setParam("region", 0.45);
        const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
        const StrandsPtr after = scene.lg.eval(scene.braid);
        std::size_t gathered = 0;
        for (std::size_t s = 0; s < after->strandCount(); ++s) {
            const Vec3f r = before->strandPoints(s)[0];
            const float d = std::hypot(r.x - 0.5f, r.z - 0.5f);
            if (d < 0.3f || d > 0.44f) continue;
            ++gathered;
            const std::size_t n = after->strandPointCount(s);
            for (std::size_t i = 1; i < n; ++i) EXPECT_GT(after->strandPoints(s)[i].y, -1e-3f) << s << " " << i;
            // Pressed onto the scalp (0.03 clearance: a quarter of the width) on the way in...
            EXPECT_GT(after->strandPoints(s)[1].y, 0.03f - 1e-3f) << s;
            EXPECT_LT(after->strandPoints(s)[1].y, 0.04f) << s;
            // ...and in the braid half way up all the same.
            const Vec3f mid = after->strandPoints(s)[12];
            EXPECT_LT(std::hypot(mid.x - 0.5f, mid.z - 0.5f), 0.16f) << s;
        }
        EXPECT_GT(gathered, 300u);
    }
    // On a head: hair from far down the sphere would cut through it on its way to the top.
    auto inside = [](bool alongScalp) {
        BraidScene scene("three");
        scene.sphere();
        scene.setParam("region", 1.2);
        scene.setParam("alongScalp", alongScalp);
        const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
        const StrandsPtr after = scene.lg.eval(scene.braid);
        std::size_t cut = 0, gathered = 0;
        for (std::size_t s = 0; s < after->strandCount(); ++s) {
            const Vec3f r = before->strandPoints(s)[0];
            const float d = (r - Vec3f{0.0f, 1.0f, 0.0f}).length();
            if (d < 0.7f || d > 1.1f) continue;
            ++gathered;
            const std::size_t n = after->strandPointCount(s);
            bool dips = false;
            for (std::size_t i = 1; i < n; ++i) dips = dips || after->strandPoints(s)[i].length() < 1.0f - 1e-3f;
            cut += dips ? 1 : 0;
            const Vec3f mid = after->strandPoints(s)[12];
            EXPECT_LT(std::hypot(mid.x, mid.z), 0.16f) << s;  // gathered into the braid either way
        }
        EXPECT_GT(gathered, 100u);
        return cut;
    };
    EXPECT_EQ(inside(true), 0u);
    EXPECT_GT(inside(false), 50u);
}

TEST(Braid, TwoCornrowsEachTakeTheirOwnStrandsAndWriteTheChannels) {
    BraidScene rows("cornrow", {{0.2f, 0.01f, 0.35f}, {0.8f, 0.01f, 0.35f}});
    rows.addPath({{0.2f, 0.01f, 0.65f}, {0.8f, 0.01f, 0.65f}});
    rows.setParam("feed", true);
    rows.setParam("feedDistance", 0.1);
    rows.setParam("region", 0.1);
    const StrandsPtr before = rows.lg.eval(rows.lg.scatter);
    const StrandsPtr after = rows.lg.eval(rows.braid);
    const std::vector<float>* ids = after->strandChannel("braidId");
    const std::vector<float>* flat = after->strandChannel("braidFlat");
    ASSERT_TRUE(ids && flat);
    ASSERT_EQ(ids->size(), after->strandCount());
    std::size_t braided = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        const bool moved = strandMoved(*before, *after, s);
        EXPECT_EQ(moved, (*ids)[s] >= 0.0f) << s;
        if (!moved) {
            EXPECT_FLOAT_EQ((*ids)[s], -1.0f) << s;
            continue;
        }
        ++braided;
        const Vec3f r = before->strandPoints(s)[0];
        const float own = r.z < 0.5f ? 0.35f : 0.65f, other = 1.0f - own;
        EXPECT_FLOAT_EQ((*ids)[s], r.z < 0.5f ? 0.0f : 1.0f) << s;  // the nearer row, in the guides' order
        EXPECT_FLOAT_EQ((*flat)[s], 1.0f) << s;                     // a cornrow lies on the scalp
        const Vec3f mid = after->strandPoints(s)[12];
        EXPECT_LT(std::abs(mid.z - own), std::abs(mid.z - other)) << s;
    }
    EXPECT_GT(braided, 200u);

    BraidScene round("three");
    const StrandsPtr braid = round.lg.eval(round.braid);
    const std::vector<float>* roundIds = braid->strandChannel("braidId");
    const std::vector<float>* roundFlat = braid->strandChannel("braidFlat");
    ASSERT_TRUE(roundIds && roundFlat);
    std::size_t inBraid = 0;
    for (std::size_t s = 0; s < braid->strandCount(); ++s) {
        if ((*roundIds)[s] < 0.0f) continue;
        ++inBraid;
        EXPECT_FLOAT_EQ((*roundFlat)[s], 0.0f) << s;
    }
    EXPECT_GT(inBraid, 200u);
}

TEST(Braid, BoxBraidsPickTheirOwnPaths) {
    // No braids input: paths picked among the strands about `region` apart, each taking the strands
    // around it.
    LookGraph lg{3000.0, 16};
    lg.segments(24);
    const NodeId braid = lg.add(op::kBraid, lg.scatter);
    lg.g.node(braid).params().set("region", 0.25);  // about 16 braids over the unit plane
    lg.g.node(braid).params().set("strays", 0.0);
    const StrandsPtr before = lg.eval(lg.scatter);
    const StrandsPtr after = lg.eval(braid);
    const std::vector<float>* ids = after->strandChannel("braidId");
    ASSERT_TRUE(ids);
    std::set<float> distinct;
    std::size_t moved = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        if ((*ids)[s] < 0.0f) continue;
        distinct.insert((*ids)[s]);
        moved += strandMoved(*before, *after, s) ? 1 : 0;
    }
    EXPECT_GE(distinct.size(), 6u);
    EXPECT_GT(moved, 300u);
}

TEST(Braid, BraidIdIsMinusOneForLooseStrandsAndStrays) {
    BraidScene scene("three");
    scene.setParam("strays", 0.5);
    const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
    const StrandsPtr after = scene.lg.eval(scene.braid);
    const std::vector<float>* ids = after->strandChannel("braidId");
    ASSERT_TRUE(ids);
    std::size_t inRegion = 0, strays = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        const Vec3f r = before->strandPoints(s)[0];
        const float d = std::hypot(r.x - 0.5f, r.z - 0.5f);
        const bool moved = strandMoved(*before, *after, s);
        EXPECT_EQ(moved, (*ids)[s] >= 0.0f) << s;  // loose, stray or out of the region: -1
        if (d > 0.25f) {
            EXPECT_FALSE(moved) << s;
            continue;
        }
        ++inRegion;
        strays += moved ? 0 : 1;
    }
    EXPECT_GT(inRegion, 400u);
    EXPECT_GT(strays, inRegion * 3 / 10);
    EXPECT_LT(strays, inRegion * 7 / 10);
}

TEST(Braid, AddsPointsSoEveryWeaveRepeatIsDrawn) {
    // A path 1 long at 2.5 crossings: 2.5 repeats of 12 points each, on strands of 24 segments.
    BraidScene scene("three");
    const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
    const StrandsPtr after = scene.lg.eval(scene.braid);
    ASSERT_TRUE(after->validate());
    const std::vector<float>* ids = after->strandChannel("braidId");
    ASSERT_TRUE(ids);
    std::size_t braided = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        const std::size_t n = after->strandPointCount(s);
        if ((*ids)[s] < 0.0f) {
            EXPECT_EQ(n, before->strandPointCount(s)) << s;  // loose: as it came
            continue;
        }
        ++braided;
        EXPECT_GE(n, 31u) << s;
        EXPECT_LE(n, 32u) << s;
        EXPECT_EQ(after->strandPoints(s)[0], before->strandPoints(s)[0]) << s;  // the root stays
    }
    EXPECT_GT(braided, 200u);
    // resolution 0: the strands' own points; maxSegments caps what is added.
    scene.setParam("resolution", 0.0);
    EXPECT_EQ(scene.lg.eval(scene.braid)->pointCount(), before->pointCount());
    scene.setParam("resolution", 100.0);
    scene.setParam("maxSegments", std::int64_t{40});
    const StrandsPtr capped = scene.lg.eval(scene.braid);
    for (std::size_t s = 0; s < capped->strandCount(); ++s) {
        if ((*capped->strandChannel("braidId"))[s] >= 0.0f) EXPECT_EQ(capped->strandPointCount(s), 41u) << s;
    }
}

TEST(Braid, AResampleAfterItKeepsTheWeavesPoints) {
    // The default graph's Resample (16 segments) after a Braid: the loose strands take its count,
    // the braided keep theirs.
    BraidScene scene("three");
    const NodeId resample = scene.lg.add(op::kResample, scene.braid);
    scene.lg.g.node(resample).params().set("segments", std::int64_t{8});
    const StrandsPtr out = scene.lg.eval(resample);
    const std::vector<float>* ids = out->strandChannel("braidId");
    ASSERT_TRUE(ids);
    std::size_t braided = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if ((*ids)[s] >= 0.0f) {
            ++braided;
            EXPECT_GE(out->strandPointCount(s), 31u) << s;
        } else {
            EXPECT_EQ(out->strandPointCount(s), 9u) << s;
        }
    }
    EXPECT_GT(braided, 200u);
}

TEST(Braid, SectionsArePartedByTheSideTheHairLiesOn) {
    // A path along x over the plane: hair rooted to one side of it (z) goes into the section on that
    // side, so the sections leave the scalp without crossing each other.
    BraidScene scene("three", {{0.5f, 0.01f, 0.5f}, {1.5f, 0.01f, 0.5f}});
    scene.setParam("crossings", 0.5);
    scene.setParam("lobeFill", 0.2);
    scene.setParam("fuzz", 0.0);
    const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
    const StrandsPtr after = scene.lg.eval(scene.braid);
    const std::vector<float>* ids = after->strandChannel("braidId");
    ASSERT_TRUE(ids);
    std::size_t sided = 0, agree = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        if ((*ids)[s] < 0.0f) continue;
        const Vec3f d = before->strandPoints(s)[0] - Vec3f{0.5f, 0.01f, 0.5f};
        if (std::abs(d.z) < 0.6f * d.length()) continue;  // in the middle section's wedges
        const std::size_t n = after->strandPointCount(s);
        const Vec3f joined = after->strandPoints(s)[(n - 1) / 6];  // just past the gather
        ++sided;
        agree += (joined.z - 0.5f) * d.z > 0.0f ? 1 : 0;
    }
    EXPECT_GT(sided, 50u);
    EXPECT_GT(agree, sided * 95 / 100);
}

TEST(Braid, FeedThickensTheBraidWithTheHairItPicksUp) {
    // The sections' center curves along a fed row: narrow where it has picked up little, wide
    // where it holds most of its hair; without feedGrowth, as wide all along.
    auto spread = [](double growth, float from, float to) {
        BraidScene row = fedRow();
        row.setParam("region", 0.05);
        row.setParam("widthProfile", Ramp::constant(1.0f));
        row.setParam("tail", 0.0);
        row.setParam("feedGrowth", growth);
        row.setParam("display", std::string("paths"));
        const StrandsPtr curves = row.lg.eval(row.braid);
        const std::vector<float>* section = curves->strandChannel("braidSection");
        EXPECT_TRUE(section);
        float most = 0.0f;
        for (std::size_t s = 0; s < curves->strandCount(); ++s) {
            if (!section || (*section)[s] < 0.0f) continue;
            for (std::size_t i = 0; i < curves->strandPointCount(s); ++i) {
                const Vec3f p = curves->strandPoints(s)[i];
                if (p.x >= from && p.x <= to) most = std::max(most, std::abs(p.z - 0.5f));
            }
        }
        return most;
    };
    EXPECT_LT(spread(1.0, 0.22f, 0.42f), 0.75f * spread(1.0, 0.58f, 0.78f));
    EXPECT_NEAR(spread(0.0, 0.22f, 0.42f), spread(0.0, 0.58f, 0.78f), 0.1f * spread(0.0, 0.58f, 0.78f));
}

TEST(Braid, ThePathsOutputHandsOnThePathsItBraidsAlong) {
    // Its braids input's paths, evenly resampled, in their order with their IDs.
    BraidScene scene("three");
    scene.addPath({{0.2f, 0, 0.2f}, {0.2f, 1.0f, 0.2f}});
    EvalContext ctx;
    ctx.externals = &scene.lg.ext;
    const GuidesPtr paths = valueAs<GuidesPtr>(scene.lg.g.evaluateOutput(scene.braid, 1, ctx));
    ASSERT_TRUE(paths);
    ASSERT_EQ(paths->guides().size(), 2u);
    for (std::size_t b = 0; b < 2; ++b) {
        const Guide& g = paths->guides()[b];
        EXPECT_EQ(g.id, b + 1);
        ASSERT_GE(g.points.size(), 64u);
        const Vec3f axis = b == 0 ? Vec3f{0.5f, 0.0f, 0.5f} : Vec3f{0.2f, 0.0f, 0.2f};
        for (const Vec3f& p : g.points) EXPECT_LT(std::hypot(p.x - axis.x, p.z - axis.z), 1e-5f) << b;
        EXPECT_NEAR(g.points.back().y, 1.0f, 1e-5f);
    }
    // Without a braids input: the paths it picked among the strands, each a strand's (its ID).
    LookGraph lg{3000.0, 16};
    const NodeId braid = lg.add(op::kBraid, lg.scatter);
    lg.g.node(braid).params().set("region", 0.3);
    EvalContext own;
    own.externals = &lg.ext;
    const GuidesPtr picked = valueAs<GuidesPtr>(lg.g.evaluateOutput(braid, 1, own));
    const StrandsPtr strands = lg.eval(lg.scatter);
    ASSERT_TRUE(picked);
    EXPECT_GE(picked->guides().size(), 4u);
    const std::set<StrandId> ids(strands->ids().begin(), strands->ids().end());
    for (const Guide& g : picked->guides()) EXPECT_TRUE(ids.count(g.id)) << g.id;
}

TEST(Braid, DisplayPathsShowsEachPathAndItsSections) {
    BraidScene scene("three");
    scene.addPath({{0.2f, 0, 0.2f}, {0.2f, 1.0f, 0.2f}});
    scene.setParam("display", std::string("paths"));
    const StrandsPtr curves = scene.lg.eval(scene.braid);
    ASSERT_EQ(curves->strandCount(), 8u);  // two paths, each with its three sections
    ASSERT_TRUE(curves->validate());
    const std::vector<float>* ids = curves->strandChannel("braidId");
    const std::vector<float>* section = curves->strandChannel("braidSection");
    ASSERT_TRUE(ids && section);
    std::set<StrandId> distinct(curves->ids().begin(), curves->ids().end());
    EXPECT_EQ(distinct.size(), 8u);
    for (std::size_t s = 0; s < 8; ++s) {
        const float braid = static_cast<float>(s / 4);
        EXPECT_FLOAT_EQ((*ids)[s], braid) << s;
        EXPECT_FLOAT_EQ((*section)[s], static_cast<float>(s % 4) - 1.0f) << s;
        const Vec3f axis = s < 4 ? Vec3f{0.5f, 0.0f, 0.5f} : Vec3f{0.2f, 0.0f, 0.2f};
        for (std::size_t i = 0; i < curves->strandPointCount(s); ++i) {
            const Vec3f p = curves->strandPoints(s)[i];
            const float off = std::hypot(p.x - axis.x, p.z - axis.z);
            if (s % 4 == 0) EXPECT_LT(off, 1e-5f) << s;    // the path itself
            else EXPECT_LT(off, 0.12f * 0.6f) << s;         // a section's center, within the braid
        }
    }
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(Twist, TurnsStrandsAroundTheirAxis) {
    // Half a turn around an upright axis over its unit length: tips end up across it.
    BraidScene scene("three");
    const NodeId twist = scene.lg.add(op::kTwist, scene.lg.scatter);
    scene.lg.g.connect(scene.guides, twist, 1);
    scene.lg.g.node(twist).params().set("turns", 0.5);
    const StrandsPtr before = scene.lg.eval(scene.lg.scatter);
    const StrandsPtr after = scene.lg.eval(twist);
    ASSERT_EQ(after->strandCount(), before->strandCount());
    for (std::size_t s = 0; s < after->strandCount(); s += 37) {
        const std::size_t n = after->strandPointCount(s);
        const Vec3f b = before->strandPoints(s)[n - 1], a = after->strandPoints(s)[n - 1];
        EXPECT_EQ(after->strandPoints(s)[0], before->strandPoints(s)[0]);  // roots stay
        EXPECT_NEAR(a.x - 0.5f, -(b.x - 0.5f), 1e-3f);
        EXPECT_NEAR(a.z - 0.5f, -(b.z - 0.5f), 1e-3f);
    }
    // No axes and no clumps to turn around: a clear error.
    const NodeId alone = scene.lg.add(op::kTwist, scene.lg.scatter);
    EXPECT_THROW(scene.lg.eval(alone), GraphError);
}

TEST(Twist, WithoutAxesTurnsEachClumpAroundItsMiddle) {
    LookGraph lg(2000.0);
    const NodeId clump = lg.add(op::kClump, lg.scatter);
    const NodeId centers = lg.add(op::kToGuides, lg.scatter);
    lg.g.node(centers).params().set("fraction", 0.02);
    lg.g.connect(centers, clump, 1);
    lg.g.node(clump).params().set("centersFrom", std::string("inputPoints"));
    lg.g.node(clump).params().set("strength", 0.5);
    const NodeId twist = lg.add(op::kTwist, clump);
    lg.g.node(twist).params().set("turns", 1.0);
    const StrandsPtr clumped = lg.eval(clump), twisted = lg.eval(twist);
    EXPECT_GT(maxDisplacement(*clumped, *twisted), 0.01f);
    for (std::size_t s = 0; s < twisted->strandCount(); ++s) EXPECT_EQ(twisted->strandPoints(s)[0], clumped->strandPoints(s)[0]);
}

TEST(Smooth, TakesOutKinksAndKeepsLengths) {
    LookGraph lg;
    const NodeId frizz = lg.add(op::kNoise, lg.scatter);  // frizz: fine noise
    lg.g.node(frizz).params().set("amplitude", 0.05);
    lg.g.node(frizz).params().set("frequency", 8.0);
    lg.g.node(frizz).params().set("octaves", std::int64_t{1});
    const NodeId smooth = lg.add(op::kSmooth, frizz);
    lg.g.node(smooth).params().set("scale", 0.2);
    const StrandsPtr frizzy = lg.eval(frizz), smoothed = lg.eval(smooth);
    auto bending = [](const StrandSet& set) {  // total turning along every strand
        double total = 0.0;
        for (std::size_t s = 0; s < set.strandCount(); ++s) {
            const Vec3f* p = set.strandPoints(s);
            for (std::size_t i = 1; i + 1 < set.strandPointCount(s); ++i) {
                total += std::acos(std::clamp((p[i] - p[i - 1]).normalized().dot((p[i + 1] - p[i]).normalized()), -1.0f, 1.0f));
            }
        }
        return total;
    };
    EXPECT_LT(bending(*smoothed), 0.5 * bending(*frizzy));
    for (std::size_t s = 0; s < smoothed->strandCount(); s += 17) {
        const std::size_t n = smoothed->strandPointCount(s);
        EXPECT_NEAR(arcLength(smoothed->strandPoints(s), n), arcLength(frizzy->strandPoints(s), n), 1e-3f);
        EXPECT_EQ(smoothed->strandPoints(s)[0], frizzy->strandPoints(s)[0]);
    }
}

// A wavy strand about 1.2 long, n points evenly in its parameter, scaled by k and moved by `at`.
std::vector<Vec3f> wavyStrand(std::size_t n, float k = 1.0f, Vec3f at = {}) {
    std::vector<Vec3f> pts;
    for (std::size_t i = 0; i < n; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(n - 1);
        const Vec3f p{0.06f * std::sin(15.0f * u) + 0.2f * std::sin(4.1f * u), u, 0.05f * std::cos(11.0f * u) + 0.1f * u * u};
        pts.push_back(at + p * k);
    }
    return pts;
}

StrandsPtr strandOf(const std::vector<Vec3f>& pts) {
    auto set = std::make_shared<StrandSet>();
    set->addStrand(1, pts, {}, StrandRoot{});
    return set;
}

StrandsPtr smoothed(const std::vector<Vec3f>& pts, double scale, double strength = 1.0, const Ramp& profile = Ramp::constant(1.0f)) {
    auto smooth = OperatorRegistry::instance().create(op::kSmooth);
    smooth->params().set("scale", scale);
    smooth->params().set("strength", strength);
    smooth->params().set("profile", profile);
    EvalContext ctx;
    return valueAs<StrandsPtr>(smooth->evaluate(ctx, {strandOf(pts)}));
}

// Roots where they were and every segment its old length.
void expectRootAndLengthsKept(const StrandSet& out, const std::vector<Vec3f>& in, float tolerance) {
    ASSERT_EQ(out.strandPointCount(0), in.size());
    const Vec3f* p = out.strandPoints(0);
    EXPECT_EQ(p[0], in[0]);
    for (std::size_t i = 1; i < in.size(); ++i) EXPECT_NEAR((p[i] - p[i - 1]).length(), (in[i] - in[i - 1]).length(), tolerance) << i;
}

// The Straighten node Smooth replaced (its line toward the tip): each point blended toward the
// line from the root toward the tip, at its own distance along the strand, by strength x the
// profile, then the segment lengths put back from the root.
std::vector<Vec3f> oldStraighten(std::vector<Vec3f> p, float k, const Ramp& profile) {
    const std::size_t n = p.size();
    const Vec3f dir = (p[n - 1] - p[0]).normalized();
    std::vector<float> lengths(n, 0.0f);
    for (std::size_t i = 1; i < n; ++i) lengths[i] = (p[i] - p[i - 1]).length();
    float arc = 0.0f;
    for (std::size_t i = 1; i < n; ++i) {
        arc += lengths[i];
        const float w = k * std::clamp(profile(static_cast<float>(i) / static_cast<float>(n - 1)), 0.0f, 1.0f);
        p[i] = p[i] + (p[0] + dir * arc - p[i]) * w;
    }
    Vec3f previous = p[0];
    for (std::size_t i = 1; i < n; ++i) {
        const Vec3f d = p[i] - previous;
        previous = previous + d * (lengths[i] / d.length());
        p[i] = previous;
    }
    return p;
}

// The Smooth it had before `scale`: `iterations` passes of each point toward its neighbors'
// midpoint by `strength`, then the lengths put back from the root.
std::vector<Vec3f> oldSmooth(std::vector<Vec3f> p, float strength, int iterations) {
    const std::size_t n = p.size();
    std::vector<float> lengths(n, 0.0f);
    for (std::size_t i = 1; i < n; ++i) lengths[i] = (p[i] - p[i - 1]).length();
    for (int pass = 0; pass < iterations; ++pass) {
        std::vector<Vec3f> next = p;
        for (std::size_t i = 1; i + 1 < n; ++i) next[i] = p[i] + ((p[i - 1] + p[i + 1]) * 0.5f - p[i]) * strength;
        p = next;
    }
    Vec3f previous = p[0];
    for (std::size_t i = 1; i < n; ++i) {
        const Vec3f d = p[i] - previous;
        previous = previous + d * (lengths[i] / d.length());
        p[i] = previous;
    }
    return p;
}

float largestGap(const Vec3f* a, const std::vector<Vec3f>& b) {
    float gap = 0.0f;
    for (std::size_t i = 0; i < b.size(); ++i) gap = std::max(gap, (a[i] - b[i]).length());
    return gap;
}

TEST(Smooth, ScaleOneStraightensAsStraightenDid) {
    const std::vector<Vec3f> in = wavyStrand(25);
    const float length = arcLength(in);
    // Fully: every point on the line from the root toward the old tip, lengths kept.
    const StrandsPtr straight = smoothed(in, 1.0);
    expectRootAndLengthsKept(*straight, in, 1e-5f * length);
    const Vec3f dir = (in.back() - in.front()).normalized();
    for (std::size_t i = 1; i < in.size(); ++i) {
        const Vec3f d = straight->strandPoints(0)[i] - in[0];
        EXPECT_NEAR((d - dir * d.dot(dir)).length(), 0.0f, 1e-5f * length) << i;
    }
    EXPECT_LT(largestGap(straight->strandPoints(0), oldStraighten(in, 1.0f, Ramp::constant(1.0f))), 1e-5f * length);
    // Partly, growing toward the tip: Straighten's strength and profile, the same.
    const Ramp toTheTip = Ramp::linear(0.0f, 1.0f);
    const StrandsPtr half = smoothed(in, 1.0, 0.5, toTheTip);
    expectRootAndLengthsKept(*half, in, 1e-5f * length);
    EXPECT_LT(largestGap(half->strandPoints(0), oldStraighten(in, 0.5f, toTheTip)), 1e-5f * length);
    EXPECT_GT(largestGap(half->strandPoints(0), in), 0.02f * length);
}

TEST(Smooth, SameShapeAtAnyResolution) {
    for (const double scale : {0.05, 0.1, 0.3, 0.7}) {
        // The same polyline with a point added in the middle of every segment: the same shape.
        const std::vector<Vec3f> coarse = wavyStrand(33);
        std::vector<Vec3f> fine;
        for (std::size_t i = 0; i < coarse.size(); ++i) {
            if (i > 0) fine.push_back((coarse[i - 1] + coarse[i]) * 0.5f);
            fine.push_back(coarse[i]);
        }
        const float length = arcLength(coarse);
        const StrandsPtr a = smoothed(coarse, scale), b = smoothed(fine, scale);
        expectRootAndLengthsKept(*a, coarse, 1e-5f * length);
        expectRootAndLengthsKept(*b, fine, 1e-5f * length);
        float gap = 0.0f;
        for (std::size_t i = 0; i < coarse.size(); ++i) gap = std::max(gap, (a->strandPoints(0)[i] - b->strandPoints(0)[2 * i]).length());
        EXPECT_LT(gap, 2e-3f * length) << scale;
        EXPECT_GT(largestGap(a->strandPoints(0), coarse), 0.01f * length) << scale;  // it did smooth
        // The curve itself sampled twice as finely: the same within the sampling's own difference.
        const std::vector<Vec3f> dense = wavyStrand(65);
        const StrandsPtr c = smoothed(dense, scale);
        float apart = 0.0f;
        for (std::size_t i = 0; i < coarse.size(); ++i) apart = std::max(apart, (a->strandPoints(0)[i] - c->strandPoints(0)[2 * i]).length());
        EXPECT_LT(apart, 1e-2f * length) << scale;
    }
}

TEST(Smooth, ScaleInvariantAndDeterministic) {
    const std::vector<Vec3f> in = wavyStrand(40);
    const Vec3f at{3.0f, -2.0f, 7.0f};
    const std::vector<Vec3f> big = wavyStrand(40, 100.0f, at * 100.0f);
    for (const double scale : {0.08, 0.4}) {
        const StrandsPtr a = smoothed(in, scale, 0.8), b = smoothed(big, scale, 0.8);
        const float length = arcLength(in);
        for (std::size_t i = 0; i < in.size(); ++i) {
            const Vec3f shrunk = b->strandPoints(0)[i] * 0.01f - at;
            EXPECT_NEAR((shrunk - a->strandPoints(0)[i]).length(), 0.0f, 1e-4f * length) << i;
        }
        EXPECT_EQ(smoothed(in, scale, 0.8)->positions(), a->positions());  // the same every time
    }
}

// The turning along a strand: the angles between its segments, summed.
double turning(const Vec3f* p, std::size_t n) {
    double total = 0.0;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        total += std::acos(std::clamp((p[i] - p[i - 1]).normalized().dot((p[i + 1] - p[i]).normalized()), -1.0f, 1.0f));
    }
    return total;
}

TEST(Smooth, DefaultLooksAsTheOldDefault) {
    // The old default (strength 0.5, 4 passes) took out as much of a frizz's turning, on strands
    // of 16 points (the usual count), as the default scale does: within 15%, fine or coarse.
    for (const double frequency : {3.0, 5.0, 8.0, 12.0}) {
        LookGraph lg;
        const NodeId frizz = lg.add(op::kNoise, lg.scatter);
        lg.g.node(frizz).params().set("amplitude", 0.05);
        lg.g.node(frizz).params().set("frequency", frequency);
        const StrandsPtr in = lg.eval(frizz), out = lg.eval(lg.add(op::kSmooth, frizz));
        double before = 0.0, old = 0.0, now = 0.0;
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            const std::size_t n = in->strandPointCount(s);
            ASSERT_EQ(n, 16u);
            before += turning(in->strandPoints(s), n);
            old += turning(oldSmooth(std::vector<Vec3f>(in->strandPoints(s), in->strandPoints(s) + n), 0.5f, 4).data(), n);
            now += turning(out->strandPoints(s), n);
        }
        EXPECT_NEAR(now / before, old / before, 0.15 * old / before) << frequency;
    }
}

TEST(Bend, AtTheRootRaisesCombedStrandsAboutTheirRoots) {
    // Along the normal with start and end 0: each strand turned whole about its root.
    LookGraph lg;
    const NodeId comb = lg.add(op::kBend, lg.scatter);
    lg.g.node(comb).params().set("along", std::string("direction"));
    lg.g.node(comb).params().set("direction", Vec3f{1, 0, 0});
    lg.g.node(comb).params().set("angle", 90.0);
    const NodeId lift = lg.add(op::kBend, comb);
    lg.g.node(lift).params().set("end", 0.0);
    lg.g.node(lift).params().set("angle", 30.0);
    const StrandsPtr flat = lg.eval(comb), lifted = lg.eval(lift);
    for (std::size_t s = 0; s < lifted->strandCount(); s += 23) {
        const std::size_t n = lifted->strandPointCount(s);
        const Vec3f* a = lifted->strandPoints(s);
        const Vec3f* b = flat->strandPoints(s);
        EXPECT_EQ(a[0], b[0]);
        EXPECT_GT(a[n - 1].y, b[n - 1].y);                                              // higher off the scalp
        EXPECT_NEAR(arcLength(a, n), arcLength(b, n), 1e-3f);                           // same shape, turned
        EXPECT_NEAR((a[n - 1] - a[0]).length(), (b[n - 1] - b[0]).length(), 1e-3f);
    }
    lg.g.node(lift).params().set("angle", 170.0);  // never past standing up
    const StrandsPtr up = lg.eval(lift);
    for (std::size_t s = 0; s < up->strandCount(); s += 23) {
        const Vec3f* a = up->strandPoints(s);
        EXPECT_GT((a[up->strandPointCount(s) - 1] - a[0]).normalized().y, 0.0f);
    }
}

TEST(Cull, KeepsAStableShare) {
    LookGraph lg(4000.0);
    const NodeId cull = lg.add(op::kCull, lg.scatter);
    lg.g.node(cull).params().set("keep", 0.25);
    const StrandsPtr all = lg.eval(lg.scatter), quarter = lg.eval(cull);
    EXPECT_NEAR(static_cast<double>(quarter->strandCount()) / all->strandCount(), 0.25, 0.03);
    lg.g.node(cull).params().set("keep", 0.5);
    const StrandsPtr half = lg.eval(cull);
    std::set<std::uint64_t> halfIds(half->ids().begin(), half->ids().end());
    for (const std::uint64_t id : quarter->ids()) EXPECT_TRUE(halfIds.count(id));  // more keep: only additions
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

// Strands combed flat along +x (a stand-in for Flow), and one upright guide 1.5 long at (0.5, 0, 0.5).
struct GuidedFlow {
    LookGraph lg{3000.0, 16};
    NodeId comb = 0, interp = 0;
    GuidedFlow() {
        auto set = std::make_shared<GuideSet>();
        set->add(1, std::vector<Vec3f>{{0.5f, 0, 0.5f}, {0.5f, 0.75f, 0.5f}, {0.5f, 1.5f, 0.5f}});
        lg.ext.set("guides", GuidesPtr(set), 7);
        comb = lg.add(op::kBend, lg.scatter);
        lg.g.node(comb).params().set("along", std::string("direction"));
        lg.g.node(comb).params().set("direction", Vec3f{1, 0, 0});
        lg.g.node(comb).params().set("angle", 90.0);
        const NodeId guides = lg.g.addNode(op::kGuides);
        interp = lg.add(op::kInterpolate, comb);
        lg.g.connect(guides, interp, 1);
        lg.g.node(interp).params().set("limitDistance", 0.2);
    }
    static float distance(const StrandSet& s, std::size_t i) {
        const Vec3f r = s.strandPoints(i)[0];
        return std::hypot(r.x - 0.5f, r.z - 0.5f);
    }
};

TEST(Interpolate, ARadiusKeepsTheFlowAwayFromTheGuides) {
    GuidedFlow f;
    const StrandsPtr flow = f.lg.eval(f.comb), guided = f.lg.eval(f.interp);
    const std::vector<float>* influence = guided->strandChannel("guideInfluence");
    ASSERT_NE(influence, nullptr);
    std::size_t near = 0, far = 0;
    for (std::size_t s = 0; s < guided->strandCount(); ++s) {
        const std::size_t n = guided->strandPointCount(s);
        const float d = GuidedFlow::distance(*guided, s);
        if (d > 0.21f) {  // out of reach: the flow, untouched
            for (std::size_t i = 0; i < n; ++i) EXPECT_EQ(guided->strandPoints(s)[i], flow->strandPoints(s)[i]);
            EXPECT_EQ((*influence)[s], 0.0f);
            ++far;
        } else if (d < 0.02f) {  // at the guide: its shape
            EXPECT_GT((*influence)[s], 0.85f);
            EXPECT_GT(guided->strandPoints(s)[n - 1].y, 1.2f);
            ++near;
        }
    }
    EXPECT_GT(far, 100u);
    EXPECT_GT(near, 0u);
}

TEST(Interpolate, TakingLengthKeepsTheFlowShape) {
    GuidedFlow f;
    f.lg.g.node(f.interp).params().set("take", std::string("length"));
    const StrandsPtr flow = f.lg.eval(f.comb), guided = f.lg.eval(f.interp);
    for (std::size_t s = 0; s < guided->strandCount(); ++s) {
        if (GuidedFlow::distance(*guided, s) > 0.02f) continue;
        const std::size_t n = guided->strandPointCount(s);
        const Vec3f* a = guided->strandPoints(s);
        const Vec3f* b = flow->strandPoints(s);
        EXPECT_GT((a[n - 1] - a[0]).normalized().dot((b[n - 1] - b[0]).normalized()), 0.999f);  // still combed along x
        EXPECT_GT(arcLength(a, n), 1.3f);  // toward the guide's 1.5 (the flow was 1.0)
    }
    f.lg.g.node(f.interp).params().set("take", std::string("sideways"));
    EXPECT_THROW(f.lg.eval(f.interp), GraphError);
}

TEST(Interpolate, StrandsFollowTheSmoothCurveThroughAGuidesPoints) {
    // A hook drawn with few points: the hair follows the curve the viewport draws through them,
    // not the corners between them.
    LookGraph lg{3000.0, 48};
    lg.segments(100);  // strands grown with 101 points, as a Resample of 100 segments gives them
    auto set = std::make_shared<GuideSet>();
    set->add(1, std::vector<Vec3f>{{0.5f, 0, 0.5f}, {0.5f, 1.0f, 0.5f}, {0.7f, 1.35f, 0.5f}, {1.0f, 1.4f, 0.5f},
                                    {1.2f, 1.1f, 0.5f}, {1.2f, 0.7f, 0.5f}});
    lg.ext.set("guides", GuidesPtr(set), 11);
    const NodeId guides = lg.g.addNode(op::kGuides);
    const NodeId interp = lg.add(op::kInterpolate, lg.scatter);
    lg.g.connect(guides, interp, 1);
    auto sharpest = [&] {
        const StrandsPtr out = lg.eval(interp);
        float worst = 0.0f;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            const Vec3f* p = out->strandPoints(s);
            const std::size_t n = out->strandPointCount(s);
            for (std::size_t k = 1; k + 1 < n; ++k) {
                const Vec3f a = (p[k] - p[k - 1]).normalized(), b = (p[k + 1] - p[k]).normalized();
                worst = std::max(worst, std::acos(std::clamp(a.dot(b), -1.0f, 1.0f)) * 57.2957795f);
            }
        }
        return worst;
    };
    EXPECT_LT(sharpest(), 15.0f);  // a round bend, a few degrees a segment (straight spans: about 19 at the points)
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(Expression, StrandShapeVariables) {
    StrandSet s;
    s.resizeUniform(2, 3);
    Vec3f* a = s.strandPoints(0);  // standing: straight up, 2 long
    a[0] = {0, 0, 0}; a[1] = {0, 1, 0}; a[2] = {0, 2, 0};
    Vec3f* b = s.strandPoints(1);  // bent along the scalp, 2 long, reach sqrt(2)
    b[0] = {1, 0, 0}; b[1] = {2, 0, 0}; b[2] = {2, 0, 1};
    auto eval = [&](const char* text, std::size_t i) {
        const Expression e(text);
        const Expression::Bound bound = e.bind(s);
        return e.evaluate(s, i, nullptr, &bound);
    };
    EXPECT_FLOAT_EQ(eval("length", 0), 2.0f);
    EXPECT_FLOAT_EQ(eval("longest", 1), 2.0f);
    EXPECT_FLOAT_EQ(eval("straightness", 0), 1.0f);
    EXPECT_NEAR(eval("straightness", 1), std::sqrt(2.0f) / 2.0f, 1e-5f);
    EXPECT_FLOAT_EQ(eval("tipy", 0), 2.0f);
    EXPECT_FLOAT_EQ(eval("length / longest > 0.5 ? 1 : 0", 1), 1.0f);
    // Before strands exist (Scatter density): the shape reads 0.
    EXPECT_FLOAT_EQ(Expression("length + 1").evaluateAt(StrandRoot{}, nullptr), 1.0f);
}

TEST(Expression, ClumpsOnlyTheLongStrands) {
    // Lengths vary; an Expression node drives Clump's strength, and its centerWeight has it pick
    // its centers only among the long strands.
    LookGraph lg(3000.0);
    const NodeId length = lg.add(op::kLength, lg.scatter);
    lg.g.node(length).params().set("randomness", 1.0);
    MapRef longOnly;
    longOnly.source = MapRef::Source::Expression;
    longOnly.name = "length / longest > 0.7 ? 1 : 0";
    const NodeId clump = lg.add(op::kClump, length);
    lg.g.node(clump).params().set("count", std::int64_t{30});
    lg.g.node(clump).params().set("centerWeight", longOnly);
    const NodeId expr = lg.g.addNode(op::kExpression);
    lg.g.node(expr).params().set("expression", std::string("length / longest > 0.7 ? 1 : 0"));
    lg.g.setParamInputs(clump, {"strength"});
    lg.g.connect(expr, clump, 2);

    const StrandsPtr before = lg.eval(length);
    float longest = 0.0f;
    for (std::size_t s = 0; s < before->strandCount(); ++s) {
        longest = std::max(longest, arcLength(before->strandPoints(s), before->strandPointCount(s)));
    }
    const StrandsPtr after = lg.eval(clump);
    ClumpState state;
    ASSERT_TRUE(clumpState(lg.g.node(clump), after, &state));
    const std::size_t picked = state.curves.size() / kClumpCenterSamples;
    ASSERT_GT(picked, 5u);
    for (std::size_t c = 0; c < picked; ++c) {
        EXPECT_GT(arcLength(state.curves.data() + c * kClumpCenterSamples, kClumpCenterSamples), 0.7f * longest * 0.999f);
    }

    std::size_t moved = 0;
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        const std::size_t n = after->strandPointCount(s);
        const bool isLong = arcLength(before->strandPoints(s), n) > 0.7f * longest;
        const bool same = after->strandPoints(s)[n - 1] == before->strandPoints(s)[n - 1];
        if (!isLong) EXPECT_TRUE(same);  // short strands: strength 0, untouched
        if (isLong && !same) ++moved;
    }
    EXPECT_GT(moved, 20u);
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(ToStrands, EachGuideBecomesAStrand) {
    auto set = std::make_shared<GuideSet>();
    Guide& a = set->add(1, std::vector<Vec3f>{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}});
    a.attributes["density"] = 0.5f;
    set->add(2, std::vector<Vec3f>{{1, 0, 0}, {1, 1, 0}});
    set->add(3, std::vector<Vec3f>{{2, 0, 0}});  // one point: not a strand
    ExternalInputs ext;
    ext.set("guides", GuidesPtr(set), 1);
    Graph g;
    const NodeId guides = g.addNode(op::kGuides);
    const NodeId strands = g.addNode(op::kToStrands);
    g.connect(guides, strands, 0);
    EvalContext ctx;
    ctx.externals = &ext;
    StrandsPtr out = g.evaluateStrands(strands, ctx);
    ASSERT_EQ(out->strandCount(), 2u);
    EXPECT_EQ(out->strandPointCount(0), 3u);  // the guide's own points
    EXPECT_EQ(out->strandPoints(0)[2], Vec3f(0, 2, 0));
    EXPECT_FLOAT_EQ((*out->strandChannel("density"))[0], 0.5f);
    EXPECT_FLOAT_EQ((*out->strandChannel("density"))[1], 0.0f);
    EXPECT_FLOAT_EQ(out->widths()[0], 0.005f);
    EXPECT_NE(out->ids()[0], 1u);  // apart from scattered strands' IDs
    g.node(strands).params().set("segments", std::int64_t{8});
    out = g.evaluateStrands(strands, ctx);
    EXPECT_EQ(out->strandPointCount(1), 9u);
    EXPECT_TRUE(out->validate());
    // And back: ToGuides round-trips the shape.
    const NodeId back = g.addNode(op::kToGuides);
    g.connect(strands, back, 0);
    const GuidesPtr round = valueAs<GuidesPtr>(g.evaluate(back, ctx));
    ASSERT_EQ(round->size(), 2u);
    EXPECT_EQ(round->guides()[0].points.back(), Vec3f(0, 2, 0));
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(Curves, ShapingNodesWorkOnGuidesToo) {
    // ToGuides -> Noise -> Clump's centers: the noise reshapes the guides, which stay guides.
    LookGraph lg(2000.0);
    const NodeId centers = lg.add(op::kToGuides, lg.scatter);
    lg.g.node(centers).params().set("fraction", 0.01);
    const NodeId frizz = lg.g.addNode(op::kNoise);
    lg.g.connect(centers, frizz, 0);
    lg.g.node(frizz).params().set("fraction", 1.0);
    lg.g.node(frizz).params().set("amplitude", 0.05);
    EXPECT_EQ(lg.g.outputType(frizz), PortType::Guides);
    const NodeId clump = lg.add(op::kClump, lg.scatter);
    lg.g.connect(frizz, clump, 1);  // Guides into Clump's centers
    lg.g.node(clump).params().set("centersFrom", std::string("inputPoints"));

    const GuidesPtr plain = valueAs<GuidesPtr>(lg.evalValue(centers));
    const GuidesPtr frizzed = valueAs<GuidesPtr>(lg.evalValue(frizz));
    ASSERT_TRUE(frizzed);
    ASSERT_EQ(frizzed->size(), plain->size());
    bool moved = false;
    for (std::size_t i = 0; i < plain->size(); ++i) {
        const Guide& a = plain->guides()[i];
        const Guide& b = frizzed->guides()[i];
        EXPECT_EQ(a.id, b.id);
        EXPECT_EQ(a.root.face, b.root.face);
        EXPECT_EQ(a.points.front(), b.points.front());
        moved = moved || a.points.back() != b.points.back();
    }
    EXPECT_TRUE(moved);
    EXPECT_NO_THROW(lg.eval(clump));

    // Fed strands, the same node gives strands.
    const NodeId strandFrizz = lg.add(op::kNoise, lg.scatter);
    EXPECT_EQ(lg.g.outputType(strandFrizz), PortType::Strands);
    EXPECT_TRUE(lg.eval(strandFrizz));

    // A guides output can't feed a strands-only input.
    const NodeId width = lg.g.addNode(op::kWidth);
    EXPECT_THROW(lg.g.connect(frizz, width, 0), GraphError);

    // Disabled, it passes the guides straight through.
    lg.g.setBypass(frizz, true);
    const GuidesPtr through = valueAs<GuidesPtr>(lg.evalValue(frizz));
    ASSERT_TRUE(through);
    EXPECT_EQ(through->guides().front().points, plain->guides().front().points);
}

TEST(Curves, GuidesKeepTheirPaintedMapsThroughAShapingNode) {
    auto set = std::make_shared<GuideSet>();
    Guide& g = set->add(5, std::vector<Vec3f>{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}});
    g.attributes["density"] = 0.25f;
    ExternalInputs ext;
    ext.set("guides", GuidesPtr(set), 1);
    Graph graph;
    const NodeId guides = graph.addNode(op::kGuides);
    const NodeId length = graph.addNode(op::kLength);
    const NodeId smooth = graph.addNode(op::kSmooth);
    graph.connect(guides, length, 0);
    graph.connect(length, smooth, 0);  // a chain of Curves nodes resolves all the way
    EXPECT_EQ(graph.outputType(smooth), PortType::Guides);
    graph.node(length).params().set("keep", 0.5);
    EvalContext ctx;
    ctx.externals = &ext;
    const GuidesPtr out = valueAs<GuidesPtr>(graph.evaluate(smooth, ctx));
    ASSERT_TRUE(out);
    ASSERT_EQ(out->size(), 1u);
    EXPECT_EQ(out->guides()[0].id, 5u);
    EXPECT_FLOAT_EQ(out->guides()[0].attributes.at("density"), 0.25f);
    EXPECT_NEAR(out->guides()[0].points.back().y, 1.0f, 1e-4f);  // halved
    // Unconnected, a Curves node's output is either.
    EXPECT_EQ(graph.outputType(graph.addNode(op::kNoise)), PortType::Curves);
}

TEST(Curves, GuidesKeepTheirPerPointLayersThroughAShapingNode) {
    // A layer painted along a guide rides through Smooth as a point channel and comes
    // back a layer, so Interpolate after it still blends it into the strands.
    LookGraph lg(200.0);
    lg.segments(4);  // 5 points per strand
    auto guides = std::make_shared<GuideSet>();
    for (std::uint64_t id = 1; id <= 4; ++id) {
        const float x = 0.2f * static_cast<float>(id);
        guides->add(id, test::straightCurve({x, 0, 0.5f}, {0, 1, 0}, 4)).pointAttributes["band"] = {0, 0.25f, 0.5f, 0.75f, 1};
    }
    guides->guides()[1].pointAttributes["band"] = {1, 2};  // the wrong length: reads 0 along it
    lg.ext.set("guides", GuidesPtr(guides), guides->contentHash());
    const NodeId guideNode = lg.g.addNode(op::kGuides);
    const NodeId smooth = lg.g.addNode(op::kSmooth);
    lg.g.connect(guideNode, smooth, 0);
    const GuidesPtr smoothed = valueAs<GuidesPtr>(lg.evalValue(smooth));
    ASSERT_TRUE(smoothed);
    ASSERT_EQ(smoothed->size(), 4u);
    EXPECT_EQ(smoothed->guides()[0].pointAttributes.at("band"), (std::vector<float>{0, 0.25f, 0.5f, 0.75f, 1}));
    EXPECT_EQ(smoothed->guides()[1].pointAttributes.at("band"), (std::vector<float>(5, 0.0f)));

    const NodeId interp = lg.add(op::kInterpolate, lg.scatter);
    lg.g.node(interp).params().set("neighbors", std::int64_t{1});
    lg.g.connect(smooth, interp, 1);
    const StrandsPtr out = lg.eval(interp);
    const std::vector<float>* band = out->pointChannel("band");
    ASSERT_TRUE(band);
    bool ramped = false;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const float* v = band->data() + out->strandBegin(s);
        ramped = ramped || (v[0] < 0.1f && v[out->strandPointCount(s) - 1] > 0.9f);
    }
    EXPECT_TRUE(ramped);
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

// A row of strands along x in [x0, x1), each 3 points, all leaning by `tip`.
StrandsPtr strandRow(float x0, float x1, int count, const Vec3f& tip, std::uint64_t firstId) {
    auto s = std::make_shared<StrandSet>();
    s->resizeUniform(static_cast<std::size_t>(count), 3);
    for (int i = 0; i < count; ++i) {
        const float x = x0 + (x1 - x0) * (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        Vec3f* p = s->strandPoints(static_cast<std::size_t>(i));
        p[0] = {x, 0, 0};
        p[1] = p[0] + tip * 0.5f;
        p[2] = p[0] + tip;
        s->mutableIds()[static_cast<std::size_t>(i)] = firstId + static_cast<std::uint64_t>(i);
    }
    return s;
}

TEST(Merge, BlendMeetsTheShapesAtTheSeam) {
    // a stands up on the left, b leans over on the right; they meet at x = 0.5.
    const StrandsPtr a = strandRow(0.0f, 0.5f, 50, {0, 1, 0}, 0);
    const StrandsPtr b = strandRow(0.5f, 1.0f, 50, {1, 0, 0}, 1000);
    auto merge = OperatorRegistry::instance().create(op::kMerge);
    EvalContext ctx;
    const auto plain = valueAs<StrandsPtr>(merge->evaluate(ctx, {a, b}));
    ASSERT_EQ(plain->strandCount(), 100u);
    EXPECT_NEAR(plain->strandPoints(49)[2].y, 1.0f, 1e-6f);  // no blending by default

    merge->params().set("blend", 0.2);
    const auto blended = valueAs<StrandsPtr>(merge->evaluate(ctx, {a, b}));
    ASSERT_EQ(blended->strandCount(), 100u);
    const Vec3f seamA = blended->strandPoints(49)[2] - blended->strandPoints(49)[0];  // a's last, at the seam
    const Vec3f seamB = blended->strandPoints(50)[2] - blended->strandPoints(50)[0];  // b's first
    EXPECT_LT((seamA - seamB).length(), 0.15f);   // nearly the same shape across the seam
    EXPECT_GT(seamA.x, 0.3f);                     // a leans toward b ...
    EXPECT_GT(seamB.y, 0.3f);                     // ... and b stands toward a
    EXPECT_NEAR(blended->strandPoints(0)[2].y, 1.0f, 1e-6f);   // far from the seam: untouched
    EXPECT_NEAR(blended->strandPoints(99)[2].x, 1.995f, 1e-5f);
    for (std::size_t s = 0; s < 100; ++s) {  // roots never move
        EXPECT_EQ(blended->strandPoints(s)[0], plain->strandPoints(s)[0]);
    }
}

// ---------------------------------------------------------------------------

// The preview must show the render: a strand kept at a lower preview density
// has the same shape and clump as at full density, through every look node
// that could depend on which other strands exist (Clump's own centers,
// Tangle's partners), as long as the preview keeps their candidate shares.
TEST(Look, PreviewDensityKeepsEveryStrandsShapeAndClump) {
    LookGraph lg(3000.0);
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.2f, 0, 0.3f}, {-0.4f, 0.9f, 0.1f}, 8));
    guides->add(2, test::straightCurve({0.8f, 0, 0.4f}, {0.4f, 0.9f, -0.2f}, 8));
    guides->add(3, test::straightCurve({0.5f, 0, 0.8f}, {0.1f, 0.8f, 0.5f}, 8));
    lg.ext.set("guides", GuidesPtr(guides), guides->contentHash());
    const NodeId guideNode = lg.g.addNode(op::kGuides);
    const NodeId interp = lg.add(op::kInterpolate, lg.scatter);
    lg.g.connect(guideNode, interp, 1);
    const NodeId clump = lg.add(op::kClump, interp);
    lg.g.node(clump).params().set("count", std::int64_t{40});
    lg.g.node(clump).params().set("strength", 0.8);
    const NodeId noise = lg.add(op::kNoise, clump);
    const NodeId curl = lg.add(op::kCurl, noise);
    const NodeId frizz = lg.add(op::kNoise, curl);  // frizz: fine noise on some strands
    lg.g.node(frizz).params().set("fraction", 0.3);
    lg.g.node(frizz).params().set("frequency", 8.0);
    const NodeId tangle = lg.add(op::kTangle, frizz);
    lg.g.node(tangle).params().set("fraction", 0.8);
    lg.g.node(tangle).params().set("radius", 0.08);
    const NodeId flyaway = lg.add(op::kFlyaway, tangle);
    const NodeId width = lg.add(op::kWidth, flyaway);

    auto evalAt = [&](NodeId node, float density) {
        EvalContext ctx;
        ctx.externals = &lg.ext;
        ctx.previewDensity = density;
        return lg.g.evaluateStrands(node, ctx);
    };
    // Node by node, so a failure names the first one that depends on the density.
    const std::pair<const char*, NodeId> stages[] = {{"Scatter", lg.scatter}, {"Interpolate", interp}, {"Clump", clump},
                                                     {"Noise", noise},        {"Curl", curl},          {"Noise (frizz)", frizz},
                                                     {"Tangle", tangle},      {"Flyaway", flyaway},    {"Width", width}};
    for (const auto& [name, node] : stages) {
        const StrandsPtr full = evalAt(node, 1.0f);
        const StrandsPtr preview = evalAt(node, 0.2f);
        ASSERT_TRUE(full && preview) << name;
        EXPECT_GT(full->strandCount(), 2000u);
        EXPECT_LT(preview->strandCount(), full->strandCount() / 3);

        std::map<StrandId, std::size_t> byId;
        for (std::size_t s = 0; s < full->strandCount(); ++s) byId[full->ids()[s]] = s;
        const std::vector<float>* fullClump = full->strandChannel("clumpId");
        const std::vector<float>* previewClump = preview->strandChannel("clumpId");
        float worst = 0.0f;
        std::size_t clumpMismatches = 0;
        for (std::size_t s = 0; s < preview->strandCount(); ++s) {
            auto it = byId.find(preview->ids()[s]);
            ASSERT_TRUE(it != byId.end()) << name << ": a preview strand missing from the render";
            const std::size_t f = it->second;
            ASSERT_EQ(preview->strandPointCount(s), full->strandPointCount(f)) << name;
            for (std::size_t i = 0; i < preview->strandPointCount(s); ++i) {
                worst = std::max(worst, (preview->strandPoints(s)[i] - full->strandPoints(f)[i]).length());
            }
            if (fullClump && previewClump && (*previewClump)[s] != (*fullClump)[f]) ++clumpMismatches;
            EXPECT_FLOAT_EQ(preview->widths()[preview->strandBegin(s)], full->widths()[full->strandBegin(f)]) << name;
        }
        EXPECT_EQ(clumpMismatches, 0u) << name;
        EXPECT_LT(worst, 1e-5f) << name << ": a preview strand differs from its render shape by " << worst;
    }
}

// ---------------------------------------------------------------------------
// Per-point guide layers and per-point fields

TEST(Interpolate, CarriesPerPointGuideLayersIntoPointChannels) {
    LookGraph lg(200.0);
    lg.segments(8);  // 9 points per strand
    auto guides = std::make_shared<GuideSet>();
    // Two straight guides of 5 points: "band" peaks half way up the left one;
    // the right one carries only a scalar, which stands in as a flat layer.
    guides->add(1, test::straightCurve({0.25f, 0, 0.5f}, {0, 1, 0}, 4)).pointAttributes["band"] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    guides->add(2, test::straightCurve({0.75f, 0, 0.5f}, {0, 1, 0}, 4)).attributes["band"] = 0.2f;
    lg.ext.set("guides", GuidesPtr(guides), guides->contentHash());
    const NodeId guideNode = lg.g.addNode(op::kGuides);
    const NodeId interp = lg.add(op::kInterpolate, lg.scatter);
    lg.g.node(interp).params().set("neighbors", std::int64_t{1});
    lg.g.connect(guideNode, interp, 1);

    const StrandsPtr out = lg.eval(interp);
    const std::vector<float>* band = out->pointChannel("band");
    ASSERT_TRUE(band);
    ASSERT_EQ(band->size(), out->pointCount());
    std::size_t left = 0, right = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s), begin = out->strandBegin(s);
        ASSERT_EQ(n, 9u);
        const float x = out->strandPoints(s)[0].x;
        const float* v = band->data() + begin;
        if (x < 0.45f) {  // the left guide's band, read at each point's arc-length position
            ++left;
            EXPECT_NEAR(v[0], 0.0f, 1e-3f);
            EXPECT_NEAR(v[2], 0.0f, 0.05f);  // t 0.25: still at the guide's second value (grid-softened)
            EXPECT_NEAR(v[3], 0.5f, 0.05f);  // t 0.375: half way up the ramp to the peak
            EXPECT_GT(v[4], 0.9f);           // t 0.5: the peak (softened by the common resolution)
            EXPECT_NEAR(v[5], 0.5f, 0.05f);
            EXPECT_NEAR(v[8], 0.0f, 1e-3f);
        } else if (x > 0.55f) {  // the scalar, flat along the strand
            ++right;
            for (std::size_t k = 0; k < n; ++k) EXPECT_NEAR(v[k], 0.2f, 1e-4f);
        }
    }
    EXPECT_GT(left, 20u);
    EXPECT_GT(right, 20u);

    // Downstream, an attribute map of the name reads the point channel per point: Width follows the band.
    const NodeId width = lg.add(op::kWidth, interp);
    MapRef map;
    map.source = MapRef::Source::Attribute;
    map.name = "band";
    map.max = 0.1f;
    lg.g.node(width).params().set("width", map);
    lg.g.node(width).params().set("profile", Ramp::constant(1.0f));
    const StrandsPtr wide = lg.eval(width);
    for (std::size_t s = 0; s < wide->strandCount(); ++s) {
        const std::size_t begin = wide->strandBegin(s);
        for (std::size_t k = 0; k < 9; ++k) EXPECT_NEAR(wide->widths()[begin + k], 0.1f * (*band)[begin + k], 1e-5f);
    }
}

TEST(Layers, PerPointLayersFlowFromParentsToChildrenAndRoundTrip) {
    const auto plane = test::makePlane(8);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    const GuideId parent = h.addGuide(primary, test::straightCurve({0.5f, 0, 0.5f}, {0, 1, 0}, 4));  // 5 points
    const GuideId other = h.addGuide(primary, test::straightCurve({0.7f, 0, 0.5f}, {0, 1, 0}, 4));
    h.setPointAttribute(parent, "band", {0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    h.setPointAttribute(other, "band", {1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    h.evaluate(plane.get());
    EXPECT_TRUE(h.hasPointAttributes());
    EXPECT_FALSE(h.hasAttributes());
    EXPECT_EQ(h.attributeNames(), (std::vector<std::string>{"band"}));
    EXPECT_EQ(h.pointAttributeNames(), (std::vector<std::string>{"band"}));

    // Children inherit the parent's layer, resampled along them (they follow it point for point).
    ChildGenerationParams p;
    p.count = 3;
    const std::vector<GuideId> parents{parent};
    h.generateChildren(primary, "children", p, &parents);
    h.evaluate(plane.get());
    const std::vector<GuideId> children = h.childrenOf(parent);
    ASSERT_EQ(children.size(), 3u);
    for (const GuideId child : children) {
        const HierarchyGuide& g = *h.guide(child);
        EXPECT_TRUE(g.pointAttributes.empty());  // nothing of its own
        std::vector<float> layer;
        ASSERT_TRUE(h.effectivePointAttribute(g, "band", &layer));
        ASSERT_EQ(layer.size(), g.points.size());
        EXPECT_FLOAT_EQ(layer[2], 1.0f);
        EXPECT_FLOAT_EQ(layer[1], 0.0f);
        EXPECT_EQ(h.effectivePointAttributes(g).at("band"), layer);
    }
    // A child's own layer overrides, resampled to its points (3 values over 5 points).
    h.setPointAttribute(children[0], "band", {0.5f, 0.5f, 0.5f});
    EXPECT_EQ(h.guide(children[0])->pointAttributes.at("band"), (std::vector<float>{0.5f, 0.5f, 0.5f, 0.5f, 0.5f}));
    // Reshaping a guide to another point count resamples its layers.
    h.setPoints(parent, test::straightCurve({0.5f, 0, 0.5f}, {0, 1, 0}, 8));
    const std::vector<float> reshaped = h.guide(parent)->pointAttributes.at("band");
    ASSERT_EQ(reshaped.size(), 9u);
    EXPECT_FLOAT_EQ(reshaped[4], 1.0f);
    EXPECT_FLOAT_EQ(reshaped[0], 0.0f);
    h.evaluate(plane.get());

    // flatten carries the effective layers onto the guides; JSON keeps them.
    const GuideSet flat = h.flatten(-1);  // the children
    ASSERT_EQ(flat.size(), 3u);
    for (const Guide& g : flat.guides()) {
        ASSERT_EQ(g.pointAttributes.count("band"), 1u);
        EXPECT_EQ(g.pointAttributes.at("band").size(), g.points.size());
    }
    const GuideSet top = h.flatten(0);
    ASSERT_EQ(top.size(), 2u);
    for (const Guide& g : top.guides()) EXPECT_EQ(g.pointAttributes.at("band").size(), g.points.size());
    const GuideHierarchy again = GuideHierarchy::fromJson(h.toJson());
    EXPECT_EQ(again.guide(parent)->pointAttributes.at("band"), reshaped);
    EXPECT_EQ(again.contentHash(), h.contentHash());
    h.setPointAttribute(children[1], "band", {0.1f, 0.2f, 0.3f, 0.4f, 0.5f});
    EXPECT_NE(again.contentHash(), h.contentHash());  // a layer edit changes the hash (re-evaluates the groom)

    // Mirroring copies the layers; rename and remove cover both kinds.
    const std::vector<GuideId> mirrored = h.mirrorGroup(primary, 0, 0.5f, true);
    ASSERT_EQ(mirrored.size(), 1u);
    EXPECT_EQ(h.guide(mirrored[0])->pointAttributes.at("band"), (std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}));
    h.renameAttribute("band", "stripe");
    EXPECT_EQ(h.pointAttributeNames(), (std::vector<std::string>{"stripe"}));
    EXPECT_EQ(h.guide(parent)->pointAttributes.count("band"), 0u);
    h.setAttribute(parent, "other", 1.0f);
    EXPECT_THROW(h.renameAttribute("stripe", "other"), std::invalid_argument);
    EXPECT_EQ(h.attributeNames(), (std::vector<std::string>{"other", "stripe"}));
    h.removeAttribute("stripe");
    EXPECT_FALSE(h.hasPointAttributes());
    h.setPointAttribute(parent, "stripe", {0.5f});
    EXPECT_TRUE(h.hasPointAttributes());
    h.setPointAttribute(parent, "stripe", {});  // empty removes
    EXPECT_FALSE(h.hasPointAttributes());
}

TEST(Brush, PointwiseAttributePaintsABandAlongAGuide) {
    const auto scalp = test::makePlane(8);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    const GuideId id = h.addGuide(primary, test::straightCurve({0.5f, 0, 0.5f}, {0, 1, 0}, 8));  // 9 points, y 0 to 1
    h.setAttribute(id, "tint", 0.2f);
    h.evaluate(scalp.get());

    BrushSettings s;
    s.mode = BrushMode::Attribute;
    s.attribute = "tint";
    s.value = 1.0f;
    s.radius = 0.2f;
    s.pointwise = true;
    EXPECT_EQ(applyBrushStamp(h, scalp.get(), s, {0.5f, 0.5f, 0.5f}, Vec3f{}), 1u);
    const std::vector<float> layer = h.guide(id)->pointAttributes.at("tint");
    ASSERT_EQ(layer.size(), 9u);
    EXPECT_FLOAT_EQ(layer[4], 1.0f);  // under the brush center: all the way to the value
    for (const std::size_t k : {0u, 1u, 2u, 6u, 7u, 8u}) EXPECT_FLOAT_EQ(layer[k], 0.2f) << k;  // out of reach: the scalar it started from
    EXPECT_GT(layer[3], 0.3f);  // the falloff: part of the way
    EXPECT_LT(layer[3], 0.9f);
    EXPECT_FLOAT_EQ(layer[3], layer[5]);
    EXPECT_FLOAT_EQ(h.guide(id)->attributes.at("tint"), 0.2f);  // the scalar is left alone

    // Painting again builds up; a stamp away from the guide changes nothing.
    applyBrushStamp(h, scalp.get(), s, {0.5f, 0.5f, 0.5f}, Vec3f{});
    EXPECT_GT(h.guide(id)->pointAttributes.at("tint")[3], layer[3]);
    EXPECT_EQ(applyBrushStamp(h, scalp.get(), s, {0.5f, 3.0f, 0.5f}, Vec3f{}), 0u);

    // Not pointwise: one value per guide, as before.
    s.pointwise = false;
    applyBrushStamp(h, scalp.get(), s, {0.5f, 0.5f, 0.5f}, Vec3f{});
    EXPECT_FLOAT_EQ(h.guide(id)->attributes.at("tint"), 1.0f);

    // The layer reaches the strands: Interpolate writes it as a point channel.
    LookGraph lg(100.0);
    lg.segments(8);
    lg.ext.set("hierarchy", HierarchyPtr(std::make_shared<GuideHierarchy>(h)), h.contentHash());
    const NodeId guideNode = lg.g.addNode(op::kGuides);
    const NodeId interp = lg.add(op::kInterpolate, lg.scatter);
    lg.g.connect(guideNode, interp, 1);
    const StrandsPtr out = lg.eval(interp);
    const std::vector<float>* tint = out->pointChannel("tint");
    ASSERT_TRUE(tint);
    ASSERT_GT(out->strandCount(), 0u);
    EXPECT_NEAR((*tint)[4], 1.0f, 0.05f);
    EXPECT_NEAR((*tint)[0], 0.2f, 1e-3f);
}

TEST(HairColor, TipStrengthPaintsColorAlongTheStrands) {
    LookGraph lg(100.0);
    lg.segments(10);  // 11 points: t = 0, 0.1, ... 1
    const NodeId color = lg.add(op::kHairColor, lg.scatter);
    ParamSet& params = lg.g.node(color).params();
    params.set("melanin", 0.0);
    params.set("melaninRandom", 0.0);
    params.set("dye", Vec3f(0.0f, 0.0f, 0.0f));
    params.set("tipColor", Vec3f(1.0f, 1.0f, 1.0f));
    params.set("tipProfile", Ramp::constant(1.0f));
    MapRef band;
    band.source = MapRef::Source::Expression;
    band.name = "(t > 0.35) * (t < 0.65)";  // a band in the middle of every strand
    params.set("tipStrength", band);
    const StrandsPtr out = lg.eval(color);
    const std::vector<float>* r = out->pointChannel("colorR");
    ASSERT_TRUE(r);
    ASSERT_GT(out->strandCount(), 0u);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t begin = out->strandBegin(s);
        ASSERT_EQ(out->strandPointCount(s), 11u);
        for (std::size_t k = 0; k < 11; ++k) {
            const bool inBand = k >= 4 && k <= 6;
            EXPECT_FLOAT_EQ((*r)[begin + k], inBand ? 1.0f : 0.0f) << k;
        }
        EXPECT_FLOAT_EQ((*out->strandChannel("colorR"))[s], 0.0f);  // the per-strand color stays the base
    }

    // A number scales the blend the profile shapes.
    params.set("tipStrength", 0.5);
    params.set("tipProfile", Ramp::linear(0.0f, 1.0f));
    const StrandsPtr half = lg.eval(color);
    const std::vector<float>* hr = half->pointChannel("colorR");
    ASSERT_TRUE(hr);
    EXPECT_FLOAT_EQ((*hr)[10], 0.5f);
    EXPECT_NEAR((*hr)[5], 0.25f, 1e-5f);
}

// One operator evaluated on the given inputs, its params set by `set`; `scalp`, when
// given, is the scalp external.
Value applyOp(const char* type, const std::vector<Value>& inputs, const std::function<void(ParamSet&)>& set = {},
              std::shared_ptr<const ScalpMesh> scalp = nullptr) {
    auto node = OperatorRegistry::instance().create(type);
    if (set) set(node->params());
    ExternalInputs ext;
    if (scalp) ext.set("scalp", scalp, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    return node->evaluate(ctx, inputs);
}

MapRef attributeMap(const std::string& name, float lo = 0.0f, float hi = 1.0f) {
    MapRef map;
    map.source = MapRef::Source::Attribute;
    map.name = name;
    map.min = lo;
    map.max = hi;
    return map;
}

// Straight strands up from (x, 0, 0.5), `points` points each, length 1.
std::shared_ptr<StrandSet> uprights(const std::vector<float>& xs, int points = 5) {
    auto s = std::make_shared<StrandSet>();
    for (std::size_t i = 0; i < xs.size(); ++i) {
        s->addStrand(100 + i, test::straightCurve({xs[i], 0.0f, 0.5f}, {0, 1, 0}, points - 1));
    }
    return s;
}

TEST(Attribute, RemappingAChannelInPlaceReadsTheIncomingValues) {
    auto in = uprights({0.1f, 0.2f, 0.3f, 0.4f});
    in->addStrandChannel("attr") = {0.1f, 0.2f, 0.3f, 0.4f};
    const StrandsPtr out = valueAs<StrandsPtr>(applyOp(op::kAttribute, {StrandsPtr(in)}, [](ParamSet& p) {
        p.set("name", std::string("attr"));
        p.set("value", attributeMap("attr", 0.0f, 2.0f));  // doubled
    }));
    const std::vector<float>& attr = *out->strandChannel("attr");
    for (std::size_t s = 0; s < 4; ++s) EXPECT_NEAR(attr[s], 0.2f * static_cast<float>(s + 1), 1e-6f);
    EXPECT_NEAR((*in->strandChannel("attr"))[3], 0.4f, 1e-6f);  // the input is left alone
}

TEST(HairColor, MelaninMappedToTheMelaninUpstreamKeepsIt) {
    auto in = uprights({0.1f, 0.2f});
    in->addStrandChannel("melanin") = {0.8f, 0.6f};
    const StrandsPtr out = valueAs<StrandsPtr>(applyOp(op::kHairColor, {StrandsPtr(in)}, [](ParamSet& p) {
        p.set("melanin", attributeMap("melanin"));
        p.set("melaninRandom", 0.0);
        p.set("greys", 0.0);
    }));
    EXPECT_NEAR((*out->strandChannel("melanin"))[0], 0.8f, 1e-6f);
    EXPECT_NEAR((*out->strandChannel("melanin"))[1], 0.6f, 1e-6f);
}

TEST(Merge, KeepsBothSidesChannels) {
    auto a = uprights({0.1f, 0.2f});
    a->addStrandChannel("colorR") = {0.2f, 0.3f};
    a->addPointChannel("band", 0.0f).assign(a->pointCount(), 0.5f);
    auto b = uprights({0.7f, 0.8f, 0.9f});
    b->addStrandChannel("colorR") = {0.7f, 0.8f, 0.9f};
    b->addStrandChannel("braidId", -1.0f) = {3.0f, 3.0f, 4.0f};
    const StrandsPtr out = valueAs<StrandsPtr>(applyOp(op::kMerge, {StrandsPtr(a), StrandsPtr(b)}));
    ASSERT_EQ(out->strandCount(), 5u);
    EXPECT_TRUE(out->validate());
    EXPECT_EQ(*out->strandChannel("colorR"), (std::vector<float>{0.2f, 0.3f, 0.7f, 0.8f, 0.9f}));
    // Only on b: a's strands take its default; only on a: b's points take its default.
    EXPECT_EQ(*out->strandChannel("braidId"), (std::vector<float>{-1.0f, -1.0f, 3.0f, 3.0f, 4.0f}));
    const std::vector<float>& band = *out->pointChannel("band");
    ASSERT_EQ(band.size(), out->pointCount());
    EXPECT_FLOAT_EQ(band[a->pointCount() - 1], 0.5f);
    EXPECT_FLOAT_EQ(band[a->pointCount()], 0.0f);
}

StrandsPtr merged(const StrandsPtr& a, const StrandsPtr& b) {
    return valueAs<StrandsPtr>(applyOp(op::kMerge, {a, b}));
}

TEST(Merge, NumbersBsClumpsAndBraidsAfterAs) {
    auto a = uprights({0.1f, 0.2f, 0.3f});
    a->addStrandChannel("clumpId", -1.0f) = {0.0f, 1.0f, -1.0f};
    a->addStrandChannel("braidId", -1.0f) = {-1.0f, 0.0f, -1.0f};
    auto b = uprights({0.6f, 0.7f, 0.8f});
    b->addStrandChannel("clumpId", -1.0f) = {0.0f, -1.0f, 2.0f};
    b->addStrandChannel("braidId", -1.0f) = {0.0f, 0.0f, 1.0f};
    b->addStrandChannel("groupId", 0.0f) = {1.0f, 1.0f, 1.0f};  // the user's groups: left alone
    const StrandsPtr out = merged(a, b);
    // b's from the next block of group numbers past a's highest (groupNumberBase): where they start
    // never moves with which groups a has.
    const auto base = static_cast<float>(kGroupNumbers);
    EXPECT_EQ(*out->strandChannel("clumpId"), (std::vector<float>{0, 1, -1, base, -1, base + 2}));
    EXPECT_EQ(*out->strandChannel("braidId"), (std::vector<float>{-1, 0, -1, base, base, base + 1}));
    EXPECT_EQ(*out->strandChannel("groupId"), (std::vector<float>{0, 0, 0, 1, 1, 1}));
    EXPECT_EQ(*b->strandChannel("clumpId"), (std::vector<float>{0.0f, -1.0f, 2.0f}));  // the input is left alone

    // Only one side clumped (or a with no clump): nothing renumbered.
    auto plain = uprights({0.5f});
    EXPECT_EQ(*merged(plain, b)->strandChannel("clumpId"), (std::vector<float>{-1, 0, -1, 2}));
    EXPECT_EQ(*merged(a, plain)->strandChannel("clumpId"), (std::vector<float>{0, 1, -1, -1}));
}

TEST(Merge, TwoClumpedLooksMakeCardsOfOneLookEach) {
    LookGraph lg(1500.0);
    auto clumped = [&](NodeId scatter) {
        const NodeId clump = lg.add(op::kClump, scatter);
        lg.g.node(clump).params().set("count", std::int64_t{12});
        return clump;
    };
    const NodeId other = lg.g.addNode(op::kScatter);
    lg.g.node(other).params().set("density", 1500.0);
    lg.g.node(other).params().set("seed", std::int64_t{3});
    lg.g.connect(lg.scalp, other, 0);
    const NodeId merge = lg.g.addNode(op::kMerge);
    lg.g.connect(clumped(lg.scatter), merge, 0);
    lg.g.connect(clumped(other), merge, 1);
    const StrandsPtr out = lg.eval(merge);
    const std::size_t na = lg.eval(lg.scatter)->strandCount();
    ASSERT_GT(na, 100u);
    ASSERT_GT(out->strandCount(), na + 100u);
    std::set<float> clumpsA, clumpsB;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const float c = (*out->strandChannel("clumpId"))[s];
        if (c >= 0.0f) (s < na ? clumpsA : clumpsB).insert(c);
    }
    EXPECT_GE(clumpsA.size(), 8u);
    EXPECT_GE(clumpsB.size(), 8u);
    for (const float c : clumpsB) EXPECT_EQ(clumpsA.count(c), 0u) << c;

    // One Cards pass over the merged groom: every clump card holds one look's strands.
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    const auto plane = test::makePlane(8);
    const CardMesh cards = buildCards(*out, plane.get(), settings);
    std::size_t checked = 0;
    for (const CardMesh::Card& card : cards.cards) {
        if (card.clump < 0 || card.strands.empty()) continue;
        const bool fromA = card.strands.front() < na;
        for (const std::size_t s : card.strands) EXPECT_EQ(s < na, fromA);
        ++checked;
    }
    EXPECT_GE(checked, 24u);
}

TEST(Merge, GivesBsStrandsNewIdsOnlyWhereAHasThem) {
    auto a = uprights({0.1f, 0.2f});  // IDs 100, 101
    auto b = std::make_shared<StrandSet>();
    b->addStrand(101, test::straightCurve({0.5f, 0, 0.5f}, {0, 1, 0}, 4));
    b->addStrand(7, test::straightCurve({0.6f, 0, 0.5f}, {0, 1, 0}, 4));
    b->addStrand(100, test::straightCurve({0.7f, 0, 0.5f}, {0, 1, 0}, 4));
    const StrandsPtr out = merged(a, b);
    const std::vector<StrandId>& ids = out->ids();
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], 100u);  // a's stay
    EXPECT_EQ(ids[1], 101u);
    EXPECT_EQ(ids[3], 7u);  // no collision: kept
    EXPECT_EQ(std::set<StrandId>(ids.begin(), ids.end()).size(), 5u);
    EXPECT_EQ(ids[2], hashCombine(101, 0x9E3779B97F4A7C15ull));  // deterministic
    EXPECT_EQ(merged(a, b)->ids(), ids);
    EXPECT_EQ(b->ids()[0], 101u);  // the input is left alone

    // No collision: plain concatenation.
    auto c = uprights({0.1f});
    c->mutableIds()[0] = 55;
    EXPECT_EQ(merged(a, c)->ids(), (std::vector<StrandId>{100, 101, 55}));
}

TEST(Merge, TwoScattersWithOneSeedGiveUniqueIds) {
    LookGraph lg(800.0);
    const NodeId other = lg.g.addNode(op::kScatter);
    lg.g.node(other).params().set("density", 800.0);
    lg.g.connect(lg.scalp, other, 0);
    const NodeId merge = lg.g.addNode(op::kMerge);
    lg.g.connect(lg.scatter, merge, 0);
    lg.g.connect(other, merge, 1);
    const StrandsPtr a = lg.eval(lg.scatter);
    ASSERT_EQ(lg.eval(other)->ids(), a->ids());  // the same seed: the same strands
    const StrandsPtr out = lg.eval(merge);
    ASSERT_EQ(out->strandCount(), 2 * a->strandCount());
    EXPECT_EQ(std::set<StrandId>(out->ids().begin(), out->ids().end()).size(), out->strandCount());
}

TEST(Resample, KeepsEveryChannelsDefault) {
    auto in = uprights({0.1f, 0.2f});
    in->addStrandChannel("clumpId", -1.0f) = {3.0f, -1.0f};
    in->addPointChannel("band", 0.5f);
    const StrandsPtr out = valueAs<StrandsPtr>(applyOp(op::kResample, {StrandsPtr(in)}, [](ParamSet& p) {
        p.set("segments", std::int64_t{9});
        p.set("mode", std::string("even"));
    }));
    ASSERT_EQ(out->strandPointCount(0), 10u);
    EXPECT_EQ(*out->strandChannel("clumpId"), (std::vector<float>{3.0f, -1.0f}));
    EXPECT_FLOAT_EQ(out->strandChannelDefault("clumpId"), -1.0f);
    EXPECT_FLOAT_EQ(out->pointChannelDefault("band"), 0.5f);
    // So unclumped strands merged in after it are in no clump (not clump 0).
    const StrandsPtr both = merged(uprights({0.8f}), out);
    EXPECT_EQ(*both->strandChannel("clumpId"), (std::vector<float>{-1.0f, 3.0f, -1.0f}));
}

TEST(Merge, ASideWithoutHairColorGetsItsDefaultLookNotBlack) {
    auto colored = valueAs<StrandsPtr>(applyOp(op::kHairColor, {StrandsPtr(uprights({0.1f, 0.2f}))}, [](ParamSet& p) {
        p.set("melanin", 0.9);
        p.set("tipStrength", 0.5);
    }));
    const std::array<float, 3> fallback = defaultHairColor();
    EXPECT_GT(fallback[0], 0.02f);
    for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ(colored->strandChannelDefault(kColorChannels[k]), fallback[k]);
    auto plain = uprights({0.5f, 0.6f, 0.7f});
    auto pigmented = uprights({0.8f});
    pigmented->addStrandChannel("melanin") = {0.8f};  // melanin but no RGB: its own color

    for (const bool coloredFirst : {true, false}) {
        const StrandsPtr out = coloredFirst ? merged(colored, plain) : merged(plain, colored);
        const std::size_t first = coloredFirst ? 2 : 0;  // plain's strands
        for (std::size_t s = first; s < first + 3; ++s) {
            for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ((*out->strandChannel(kColorChannels[k]))[s], fallback[k]);
            EXPECT_FLOAT_EQ((*out->strandChannel(kMelaninChannel))[s], kDefaultMelanin);
            EXPECT_FLOAT_EQ((*out->strandChannel(kMelaninRednessChannel))[s], kDefaultMelaninRedness);
            // Per point too (Unreal's writer prefers the per-point colors).
            for (std::size_t i = 0; i < out->strandPointCount(s); ++i) {
                for (int k = 0; k < 3; ++k) {
                    EXPECT_FLOAT_EQ((*out->pointChannel(kColorChannels[k]))[out->strandBegin(s) + i], fallback[k]);
                }
            }
        }
        for (const auto& c : strandColors(*out)) EXPECT_GT(c[0] + c[1] + c[2], 1e-5f);  // nothing black
        // The colored side is as it was.
        const std::size_t own = coloredFirst ? 0 : 3;
        EXPECT_FLOAT_EQ((*out->strandChannel("colorR"))[own], (*colored->strandChannel("colorR"))[0]);
        EXPECT_FLOAT_EQ((*out->pointChannel("colorR"))[out->strandBegin(own) + 4], (*colored->pointChannel("colorR"))[4]);
    }

    const StrandsPtr withMelanin = merged(colored, pigmented);
    const std::array<float, 3> dark = melaninColor(0.8f, kDefaultMelaninRedness);
    for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ((*withMelanin->strandChannel(kColorChannels[k]))[2], dark[k]);
    EXPECT_FLOAT_EQ((*withMelanin->strandChannel(kMelaninChannel))[2], 0.8f);
}

TEST(Merge, StrandsWithoutATipBlendKeepTheirOwnColorPerPoint) {
    // Two HairColor branches: one blends to a tip color (per-point RGB), the other is one color
    // per strand. The plain branch's points carry its own color, not HairColor's default.
    auto tipped = valueAs<StrandsPtr>(applyOp(op::kHairColor, {StrandsPtr(uprights({0.1f, 0.2f}))}, [](ParamSet& p) {
        p.set("melanin", 0.95);
        p.set("tipColor", Vec3f(0.9f, 0.9f, 0.9f));
        p.set("tipStrength", 1.0);
    }));
    auto plain = valueAs<StrandsPtr>(applyOp(op::kHairColor, {StrandsPtr(uprights({0.5f, 0.6f, 0.7f}))}, [](ParamSet& p) {
        p.set("melanin", 0.1);
        p.set("redness", 0.9);
    }));
    ASSERT_TRUE(tipped->pointChannel("colorR"));
    ASSERT_EQ(plain->pointChannel("colorR"), nullptr);
    const std::array<float, 3> fallback = defaultHairColor();
    for (const bool tippedFirst : {true, false}) {
        const StrandsPtr out = tippedFirst ? merged(tipped, plain) : merged(plain, tipped);
        const std::size_t first = tippedFirst ? 2 : 0;  // plain's strands
        const std::vector<std::array<float, 3>> points = pointColors(*out);
        ASSERT_EQ(points.size(), out->pointCount());
        for (std::size_t s = 0; s < 3; ++s) {
            const std::array<float, 3> own = strandColors(*plain)[s];
            EXPECT_GT(std::abs(own[0] - fallback[0]) + std::abs(own[2] - fallback[2]), 0.05f);  // tells them apart
            for (std::size_t i = 0; i < out->strandPointCount(first + s); ++i) {
                for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ(points[out->strandBegin(first + s) + i][k], own[k]);
            }
        }
        // The tipped side still blends toward its tip.
        const std::size_t t = tippedFirst ? 0 : 3;
        const std::size_t root = out->strandBegin(t), tip = root + out->strandPointCount(t) - 1;
        EXPECT_GT(points[tip][0], points[root][0] + 0.3f);
    }
}

TEST(HairColor, WithoutATipBlendRemovesEarlierPointColors) {
    const StrandsPtr tipped = valueAs<StrandsPtr>(applyOp(op::kHairColor, {StrandsPtr(uprights({0.1f, 0.2f}))},
                                                          [](ParamSet& p) { p.set("tipStrength", 0.7); }));
    ASSERT_TRUE(tipped->pointChannel("colorR"));
    const StrandsPtr again = valueAs<StrandsPtr>(applyOp(op::kHairColor, {tipped}, [](ParamSet& p) {
        p.set("melanin", 0.9);
    }));
    for (const char* name : kColorChannels) EXPECT_EQ(again->pointChannel(name), nullptr) << name;
    EXPECT_TRUE(again->strandChannel("colorR"));
    EXPECT_TRUE(tipped->pointChannel("colorR"));  // the input is left alone
}

// ---------------------------------------------------------------------------
// Blend's look: two looks of the same strands, a dark and straight, b light, bent and wider.

struct TwoLooks {
    StrandsPtr a, b;
};

TwoLooks twoLooks(const std::vector<float>& xs, bool aTipped = false, bool bTipped = false, bool aColored = true) {
    auto base = uprights(xs, 6);
    auto bent = std::make_shared<StrandSet>(*base);
    for (std::size_t i = 0; i < bent->pointCount(); ++i) {
        bent->mutablePositions()[i] += Vec3f(0.05f * static_cast<float>(i % 6), 0.0f, 0.02f);
        bent->mutableWidths()[i] = 3.0f;
    }
    auto color = [](StrandsPtr in, double melanin, double redness, bool tipped) {
        return valueAs<StrandsPtr>(applyOp(op::kHairColor, {std::move(in)}, [&](ParamSet& p) {
            p.set("melanin", melanin);
            p.set("redness", redness);
            p.set("melaninRandom", 0.0);
            p.set("tipColor", Vec3f(0.9f, 0.9f, 0.9f));
            if (tipped) p.set("tipStrength", 1.0);
        }));
    };
    auto a = std::make_shared<StrandSet>(*(aColored ? color(base, 0.9, 0.1, aTipped) : StrandsPtr(base)));
    auto b = std::make_shared<StrandSet>(*color(bent, 0.1, 0.8, bTipped));
    // IDs: never mixed. A roughness only b has.
    std::vector<float>& clumpA = a->addStrandChannel(kClumpIdChannel, -1.0f);
    std::vector<float>& clumpB = b->addStrandChannel(kClumpIdChannel, -1.0f);
    std::vector<float>& rough = b->addStrandChannel(kRoughnessChannel, 0.25f);
    for (std::size_t s = 0; s < xs.size(); ++s) {
        clumpA[s] = static_cast<float>(s);
        clumpB[s] = static_cast<float>(s + 10);
        rough[s] = 0.1f * static_cast<float>(s % 7);
    }
    return {a, b};
}

StrandsPtr blended(const TwoLooks& looks, const std::function<void(ParamSet&)>& set) {
    return valueAs<StrandsPtr>(applyOp(op::kBlend, {looks.a, looks.b}, set));
}

float channelAt(const StrandSet& s, const char* name, std::size_t i, bool perPoint = false) {
    const std::vector<float>* values = perPoint ? s.pointChannel(name) : s.strandChannel(name);
    return values ? (*values)[i] : -100.0f;
}

TEST(Blend, ShapeModeMovesOnlyPointsAndWidths) {
    const TwoLooks looks = twoLooks({0.1f, 0.2f, 0.3f}, false, true);
    const StrandsPtr out = blended(looks, [](ParamSet& p) {
        p.set("include", std::string("shape"));
        p.set("keepLength", false);  // (the points blended)
        p.set("mix", 0.3);
        p.set("profile", Ramp::linear(0.2f, 1.0f));
    });
    // Points and widths as the shape-only Blend always moved them, to the bit.
    const StrandSet& a = *looks.a;
    const StrandSet& b = *looks.b;
    const Ramp profile = Ramp::linear(0.2f, 1.0f);
    for (std::size_t s = 0; s < a.strandCount(); ++s) {
        const std::size_t n = a.strandPointCount(s), begin = a.strandBegin(s);
        for (std::size_t k = 0; k < n; ++k) {
            const float f = 0.3f * profile(static_cast<float>(k) / static_cast<float>(n - 1));
            const Vec3f p = a.positions()[begin + k] + (b.positions()[begin + k] - a.positions()[begin + k]) * f;
            EXPECT_EQ(out->positions()[begin + k], p) << s << " " << k;
            const float w = a.widths()[begin + k] + (b.widths()[begin + k] - a.widths()[begin + k]) * f;
            EXPECT_EQ(out->widths()[begin + k], w);
        }
    }
    // Every channel is a's, and a's only (b's tip colors and roughness do not come over).
    std::vector<std::string> names = a.strandChannelNames();
    names.push_back("blend");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(out->strandChannelNames(), names);
    EXPECT_TRUE(out->pointChannelNames().empty());
    for (const std::string& name : a.strandChannelNames()) EXPECT_EQ(*out->strandChannel(name), *a.strandChannel(name)) << name;
}

TEST(Blend, MixesStrandColorAndPigmentByTheAmount) {
    const TwoLooks looks = twoLooks({0.1f, 0.2f, 0.3f});
    const StrandSet& a = *looks.a;
    const StrandSet& b = *looks.b;
    ASSERT_GT(std::abs(channelAt(a, "colorR", 0) - channelAt(b, "colorR", 0)), 0.2f);
    for (const double amount : {0.0, 0.5, 1.0}) {
        const StrandsPtr out = blended(looks, [&](ParamSet& p) { p.set("mix", amount); });
        const auto t = static_cast<float>(amount);
        for (std::size_t s = 0; s < 3; ++s) {
            for (const char* name : {"colorR", "colorG", "colorB", kMelaninChannel, kMelaninRednessChannel}) {
                const float x = channelAt(a, name, s), y = channelAt(b, name, s);
                EXPECT_FLOAT_EQ(channelAt(*out, name, s), x + (y - x) * t) << name << " at " << amount;
            }
            // Only b has a roughness: a's strands count as its default.
            EXPECT_FLOAT_EQ(channelAt(*out, kRoughnessChannel, s), 0.25f + (channelAt(b, kRoughnessChannel, s) - 0.25f) * t);
            EXPECT_EQ(channelAt(*out, kClumpIdChannel, s), static_cast<float>(s));  // IDs: a's
        }
        // One color per strand on both sides, an even weight: no per-point colors.
        EXPECT_EQ(out->pointChannel("colorR"), nullptr);
        EXPECT_FLOAT_EQ(out->strandChannelDefault(kRoughnessChannel), 0.25f);
    }
    // The default mode is the look's.
    EXPECT_EQ(OperatorRegistry::instance().create(op::kBlend)->params().getString("include"), "shapeAndLook");
}

TEST(Blend, APerPointAmountMixesTheColorPerPoint) {
    TwoLooks looks = twoLooks({0.1f, 0.2f});
    auto a = std::make_shared<StrandSet>(*looks.a);
    std::vector<float>& wet = a->addPointChannel("wet");
    for (std::size_t s = 0; s < a->strandCount(); ++s) {
        const std::size_t n = a->strandPointCount(s);
        for (std::size_t k = 0; k < n; ++k) wet[a->strandBegin(s) + k] = static_cast<float>(k) / static_cast<float>(n - 1);
    }
    looks.a = a;
    const StrandsPtr out = blended(looks, [](ParamSet& p) { p.set("mix", attributeMap("wet")); });
    ASSERT_NE(out->pointChannel("colorG"), nullptr);
    const std::array<float, 3> from = strandColors(*looks.a)[1], to = strandColors(*looks.b)[1];
    const std::size_t n = out->strandPointCount(1), begin = out->strandBegin(1);
    float mean = 0.0f;
    for (std::size_t k = 0; k < n; ++k) {
        const float w = wet[begin + k];
        mean += w;
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(channelAt(*out, kColorChannels[c], begin + k, true), from[c] + (to[c] - from[c]) * w) << k;
        }
    }
    mean /= static_cast<float>(n);
    // The strand's own color: by its mean weight along it.
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(channelAt(*out, kColorChannels[c], 1), from[c] + (to[c] - from[c]) * mean, 1e-6f);
    EXPECT_NEAR(channelAt(*out, kMelaninChannel, 1),
                channelAt(*looks.a, kMelaninChannel, 1) + (channelAt(*looks.b, kMelaninChannel, 1) - channelAt(*looks.a, kMelaninChannel, 1)) * mean,
                1e-6f);
}

TEST(Blend, TheProfileShowsInTheColorAlongTheStrands) {
    const TwoLooks looks = twoLooks({0.1f, 0.2f});
    const StrandsPtr out = blended(looks, [](ParamSet& p) {
        p.set("mix", 1.0);
        p.set("profile", Ramp::linear(0.0f, 1.0f));
    });
    const std::vector<std::array<float, 3>> points = pointColors(*out);
    ASSERT_EQ(points.size(), out->pointCount());
    const std::size_t root = out->strandBegin(0), tip = root + out->strandPointCount(0) - 1;
    for (int c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ(points[root][c], strandColors(*looks.a)[0][c]);  // dry roots
        EXPECT_FLOAT_EQ(points[tip][c], strandColors(*looks.b)[0][c]);   // wet ends
        // Per strand: halfway (the profile's mean along it).
        const float x = strandColors(*looks.a)[0][c], y = strandColors(*looks.b)[0][c];
        EXPECT_NEAR(channelAt(*out, kColorChannels[c], 0), x + (y - x) * 0.5f, 1e-6f);
    }
    EXPECT_EQ(out->strandPoints(0)[0], looks.a->strandPoints(0)[0]);
}

TEST(Blend, ASideWithOnlyStrandColorsSpreadsThemAlongItsPoints) {
    for (const bool aTipped : {true, false}) {
        const TwoLooks looks = twoLooks({0.1f, 0.2f}, aTipped, !aTipped);
        const StrandSet& tipped = aTipped ? *looks.a : *looks.b;
        const StrandSet& plain = aTipped ? *looks.b : *looks.a;
        ASSERT_NE(tipped.pointChannel("colorR"), nullptr);
        ASSERT_EQ(plain.pointChannel("colorR"), nullptr);
        const StrandsPtr out = blended(looks, [](ParamSet& p) { p.set("mix", 0.5); });
        ASSERT_NE(out->pointChannel("colorR"), nullptr);
        for (std::size_t s = 0; s < 2; ++s) {
            const std::array<float, 3> own = strandColors(plain)[s];
            for (std::size_t k = 0; k < out->strandPointCount(s); ++k) {
                const std::size_t i = out->strandBegin(s) + k;
                for (int c = 0; c < 3; ++c) {
                    const float t = channelAt(tipped, kColorChannels[c], i, true);
                    EXPECT_FLOAT_EQ(channelAt(*out, kColorChannels[c], i, true), aTipped ? t + (own[c] - t) * 0.5f : own[c] + (t - own[c]) * 0.5f);
                }
            }
        }
    }
}

TEST(Blend, ASideWithoutHairColorCountsAsItsDefaultLook) {
    const TwoLooks looks = twoLooks({0.1f, 0.2f}, false, false, false);
    ASSERT_EQ(looks.a->strandChannel("colorR"), nullptr);
    const std::array<float, 3> fallback = defaultHairColor();
    const StrandsPtr dry = blended(looks, [](ParamSet& p) { p.set("mix", 0.0); });
    const StrandsPtr wet = blended(looks, [](ParamSet& p) { p.set("mix", 1.0); });
    for (std::size_t s = 0; s < 2; ++s) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(channelAt(*dry, kColorChannels[c], s), fallback[c]);
            EXPECT_FLOAT_EQ(channelAt(*wet, kColorChannels[c], s), channelAt(*looks.b, kColorChannels[c], s));
        }
        EXPECT_FLOAT_EQ(channelAt(*dry, kMelaninChannel, s), kDefaultMelanin);
        EXPECT_FLOAT_EQ(channelAt(*dry, kMelaninRednessChannel, s), kDefaultMelaninRedness);
        EXPECT_FLOAT_EQ(channelAt(*wet, kMelaninChannel, s), channelAt(*looks.b, kMelaninChannel, s));
    }
    // Neither side colored: no color appears.
    const TwoLooks plain{looks.a, looks.a};
    EXPECT_EQ(blended(plain, [](ParamSet& p) { p.set("mix", 0.5); })->strandChannel("colorR"), nullptr);
}

#if GROOMLAB_HAS_TBB
TEST(Blend, OneThreadAndManyMixTheSameLook) {
    std::vector<float> xs;
    for (int i = 0; i < 3000; ++i) xs.push_back(0.001f * static_cast<float>(i));
    const TwoLooks looks = twoLooks(xs, false, true);
    auto run = [&] {
        return blended(looks, [](ParamSet& p) {
            p.set("mix", 0.7);
            p.set("profile", Ramp::linear(0.0f, 1.0f));
        });
    };
    const StrandsPtr many = run();
    StrandsPtr one;
    tbb::task_arena single(1);
    single.execute([&] { one = run(); });
    EXPECT_EQ(one->positions(), many->positions());
    EXPECT_EQ(one->widths(), many->widths());
    for (const std::string& name : many->strandChannelNames()) EXPECT_EQ(*one->strandChannel(name), *many->strandChannel(name)) << name;
    for (const std::string& name : many->pointChannelNames()) EXPECT_EQ(*one->pointChannel(name), *many->pointChannel(name)) << name;
}
#endif

#if GROOMLAB_HAS_TBB
TEST(Look, OneThreadAndManyGiveTheSameKeptWork) {
    // What is kept between evaluations and what is built side by side (a relaxed Scatter's roots,
    // Interpolate's binding and its rebinding after a dab, the guide hierarchy level by level, the
    // kd-trees): one thread and many make the same strands, frame after frame.
    const auto sphere = test::makeSphere(30, 60, 1.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    rest->setRestPoints(sphere->points());
    PartSet parts;
    parts.parts.push_back(PartSet::fromPoints("p", {Vec3f(-0.6f, 0.8f, 0.0f).normalized(), Vec3f(0, 1, 0),
                                                    Vec3f(0.6f, 0.8f, 0.1f).normalized()}, *sphere));
    GuideHierarchy base;
    const GroupId primary = base.addGroup("primary");
    Rng rng(3);
    for (int i = 0; i < 150; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0, 1), rng.uniform(-1, 1)).normalized();
        base.addGuide(primary, test::straightCurve(n, n * 0.6f + Vec3f(0.1f, -0.2f, 0), 10));
    }
    base.evaluate(sphere.get());
    ChildGenerationParams children;
    children.count = 4;
    base.generateChildren(primary, "children", children);
    const std::string graph = R"({"version": 1, "output": "clump", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 1500.0, "randomness": 0.3}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "clump", "type": "Clump", "params": {"strength": 0.6, "count": 60}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "interp", "input": "strands"},
                      {"from": "guides", "to": "interp", "input": "guides"},
                      {"from": "scalp", "to": "interp", "input": "scalp"},
                      {"from": "interp", "to": "clump", "input": "strands"}]})";
    auto run = [&] {
        GraphDocument doc = loadGraphJson(graph);
        GuideHierarchy h = base;
        std::vector<StrandsPtr> frames;
        BrushSettings brush;
        brush.radius = 0.3f;
        brush.level = 0;
        brush.propagate = true;
        for (int frame = 0; frame < 3; ++frame) {
            std::vector<Vec3f> points = sphere->points();
            for (Vec3f& p : points) p = Vec3f(p.x * (1.0f + 0.03f * static_cast<float>(frame)), p.y, p.z);
            const auto scalp = std::make_shared<const ScalpMesh>(rest->withPoints(std::move(points)));
            if (frame == 2) applyBrushStroke(h, scalp.get(), brush, {{0.0f, 1.2f, 0.0f}, {0.1f, 1.2f, 0.0f}});
            auto evaluated = std::make_shared<GuideHierarchy>(h);
            evaluated->evaluate(scalp.get());
            ExternalInputs ext;
            setGroomExternals(ext, scalp, nullptr, evaluated, std::make_shared<const PartLines>(parts, *scalp));
            EvalContext ctx;
            ctx.previewDensity = 0.5f;
            ctx.externals = &ext;
            frames.push_back(doc.graph.evaluateStrands(doc.output, ctx));
        }
        return frames;
    };
    const std::vector<StrandsPtr> many = run();
    std::vector<StrandsPtr> one;
    tbb::task_arena single(1);
    single.execute([&] { one = run(); });
    ASSERT_EQ(one.size(), many.size());
    for (std::size_t f = 0; f < many.size(); ++f) {
        ASSERT_TRUE(one[f] && many[f]);
        ASSERT_GT(many[f]->strandCount(), 5000u);
        EXPECT_EQ(one[f]->ids(), many[f]->ids()) << "frame " << f;
        EXPECT_EQ(one[f]->positions(), many[f]->positions()) << "frame " << f;
        for (const std::string& name : many[f]->strandChannelNames()) {
            EXPECT_EQ(*one[f]->strandChannel(name), *many[f]->strandChannel(name)) << name;
        }
    }
}
#endif

TEST(Flyaway, ASecondKeepsTheFirstsStrays) {
    std::vector<float> xs;
    for (int i = 0; i < 300; ++i) xs.push_back(0.003f * static_cast<float>(i));
    const StrandsPtr once = valueAs<StrandsPtr>(applyOp(op::kFlyaway, {StrandsPtr(uprights(xs))}, [](ParamSet& p) {
        p.set("fraction", 0.2);
        p.set("seed", std::int64_t{1});
    }));
    const StrandsPtr twice = valueAs<StrandsPtr>(applyOp(op::kFlyaway, {once}, [](ParamSet& p) {
        p.set("fraction", 0.2);
        p.set("seed", std::int64_t{2});
    }));
    const std::vector<float>& first = *once->strandChannel("flyaway");
    const std::vector<float>& both = *twice->strandChannel("flyaway");
    std::size_t before = 0, after = 0;
    for (std::size_t s = 0; s < xs.size(); ++s) {
        if (first[s] > 0.0f) EXPECT_EQ(both[s], 1.0f) << s;
        before += first[s] > 0.0f;
        after += both[s] > 0.0f;
    }
    EXPECT_GT(before, 20u);
    EXPECT_GT(after, before + 20u);
}

TEST(Merge, CardsAndInstancesFromANodeUpstreamOfBothSidesAreKeptOnce) {
    auto base = std::make_shared<StrandSet>(*uprights({0.1f, 0.2f}));
    base->addCardLayer(std::make_shared<CardMesh>());
    auto upstream = std::make_shared<InstanceSet>();
    const std::uint32_t bead = upstream->prototypeIndex("bead");
    upstream->instances.push_back({bead, 100, 1});
    upstream->instances.push_back({bead, 101, 2});
    base->setInstances(upstream);
    // Two branches of it: b moved, and with an Instance node of its own after the shared one.
    auto b = std::make_shared<StrandSet>(*base);
    for (Vec3f& p : b->mutablePositions()) p.x += 1.0f;
    auto more = std::make_shared<InstanceSet>(*upstream);
    more->instances.push_back({more->prototypeIndex("burr"), 101, 3});
    b->setInstances(more);

    const StrandsPtr out = merged(base, b);
    ASSERT_EQ(out->strandCount(), 4u);
    EXPECT_EQ(out->cardLayers().size(), 1u);
    ASSERT_TRUE(out->instances());
    ASSERT_EQ(out->instances()->size(), 3u);  // the shared two once, and b's own
    EXPECT_EQ(out->instances()->prototypes, (std::vector<std::string>{"bead", "burr"}));
    // b's own sits on b's strand 101, which Merge renamed: it follows.
    const auto& own = out->instances()->instances.back();
    EXPECT_EQ(own.strand, out->ids()[3]);
    EXPECT_NE(own.strand, 101u);
    const auto placed = out->instances()->resolve(*out);
    ASSERT_EQ(placed.size(), 3u);
    EXPECT_GT(placed.back().origin.x, 1.0f);  // on b's (moved) strand

    // The very same set on both sides: once.
    auto same = std::make_shared<StrandSet>(*base);
    EXPECT_EQ(merged(base, same)->instances(), base->instances());
    // Separate cards and instances on each side: both kept.
    auto separate = std::make_shared<StrandSet>(*uprights({0.5f}));
    separate->addCardLayer(std::make_shared<CardMesh>());
    auto own2 = std::make_shared<InstanceSet>();
    own2->instances.push_back({own2->prototypeIndex("bead"), 100, 9});
    separate->setInstances(own2);
    const StrandsPtr both = merged(base, separate);
    EXPECT_EQ(both->cardLayers().size(), 2u);
    EXPECT_EQ(both->instances()->size(), 3u);
}

TEST(ToGuides, PointChannelsBecomeLayersAndToStrandsBringsThemBack) {
    auto in = uprights({0.1f, 0.3f}, 5);
    in->addPointChannel("band") = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    const GuidesPtr guides = valueAs<GuidesPtr>(applyOp(op::kToGuides, {StrandsPtr(in)}));
    ASSERT_EQ(guides->size(), 2u);
    EXPECT_EQ(guides->guides()[1].pointAttributes.at("band"), (std::vector<float>{1.0f, 0.0f, 1.0f, 0.0f, 1.0f}));

    // Back at the guides' own points: the same values.
    const StrandsPtr back = valueAs<StrandsPtr>(applyOp(op::kToStrands, {guides}));
    ASSERT_TRUE(back->pointChannel("band"));
    EXPECT_EQ(*back->pointChannel("band"), *in->pointChannel("band"));

    // Resampled to 8 segments: the layer follows by arc length (the first guide's ramp stays a ramp).
    const StrandsPtr fine = valueAs<StrandsPtr>(
        applyOp(op::kToStrands, {guides}, [](ParamSet& p) { p.set("segments", std::int64_t{8}); }));
    const std::vector<float>& band = *fine->pointChannel("band");
    ASSERT_EQ(band.size(), 18u);
    for (std::size_t k = 0; k < 9; ++k) EXPECT_NEAR(band[k], static_cast<float>(k) / 8.0f, 1e-5f);
}

TEST(Width, LeavesThePointsShared) {
    auto in = uprights({0.1f, 0.2f});
    const StrandsPtr out = valueAs<StrandsPtr>(applyOp(op::kWidth, {StrandsPtr(in)}));
    EXPECT_EQ(out->positions().data(), in->positions().data());  // widths written, points not cloned
    EXPECT_NEAR(out->widths()[0], 0.007f, 1e-6f);
}

TEST(Bend, AlongTheNormalNegativeAnglesStopAtTheScalp) {
    const auto plane = test::makePlane(4);
    auto in = std::make_shared<StrandSet>();
    const StrandRoot root{0, 0.3f, 0.3f};
    const Vec3f base = plane->position(root);
    in->addStrand(1, test::straightCurve(base, Vec3f(0.3f, 1.0f, 0.0f).normalized(), 4), {}, root);
    const StrandsPtr down = valueAs<StrandsPtr>(
        applyOp(op::kBend, {StrandsPtr(in)}, [](ParamSet& p) {
            p.set("angle", -120.0);
            p.set("end", 0.0);
        }, plane));
    const Vec3f* pts = down->strandPoints(0);
    for (std::size_t k = 0; k < 5; ++k) EXPECT_GE(pts[k].y, -1e-5f) << k;  // never through the plane
    EXPECT_NEAR(pts[4].y, 0.0f, 1e-4f);                                     // laid onto it
    EXPECT_NEAR((pts[4] - pts[0]).length(), 1.0f, 1e-4f);                   // shape kept
    // Up still stops at standing straight.
    const StrandsPtr up = valueAs<StrandsPtr>(
        applyOp(op::kBend, {StrandsPtr(in)}, [](ParamSet& p) {
            p.set("angle", 90.0);
            p.set("end", 0.0);
        }, plane));
    EXPECT_NEAR(up->strandPoints(0)[4].x - base.x, 0.0f, 1e-4f);
}

TEST(Interpolate, PowerZeroLeavesOutGuidesOutOfReach) {
    // Three guides close together pointing up, one far off pointing sideways; a strand
    // among the three is out of the far one's reach and follows the three alone.
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (float x : {0.1f, 0.15f, 0.2f}) guides->add(id++, test::straightCurve({x, 0, 0.5f}, {0, 1, 0}));
    guides->add(id++, test::straightCurve({0.9f, 0, 0.5f}, {1, 0, 0}));
    const StrandsPtr out = valueAs<StrandsPtr>(
        applyOp(op::kInterpolate, {StrandsPtr(uprights({0.12f})), GuidesPtr(guides), Value{}}, [](ParamSet& p) {
            p.set("power", 0.0);
            p.set("neighbors", std::int64_t{2});
            p.set("reach", 1.0);
        }));
    const Vec3f* pts = out->strandPoints(0);
    EXPECT_NEAR(pts[4].x, 0.12f, 1e-4f);
    EXPECT_NEAR(pts[4].y, 1.0f, 1e-4f);
}


TEST(Clump, WithoutCentersPicksItsOwn) {
    // No centers input: about `count` centers picked among the strands, so a Clump alone clumps;
    // clumpId written either way (-1 for every strand with count 0).
    LookGraph lg;
    const NodeId clump = lg.add(op::kClump, lg.scatter);
    lg.g.node(clump).params().set("count", std::int64_t{12});
    const StrandsPtr before = lg.eval(lg.scatter);
    const StrandsPtr out = lg.eval(clump);
    const auto* ids = out->strandChannel("clumpId");
    ASSERT_NE(ids, nullptr);
    std::set<int> clumps;
    for (const float id : *ids) clumps.insert(static_cast<int>(id));
    EXPECT_GE(clumps.size(), 6u);
    EXPECT_LE(clumps.size(), 17u);  // (a few more than the count on a small plane: its edges)
    EXPECT_EQ(clumps.count(-1), 0u);  // every strand in a clump
    EXPECT_GT(maxDisplacement(*before, *out), 0.01f);
    // The tips of one clump's strands drew together: closer than they were.
    float was = 0.0f, now = 0.0f;
    for (std::size_t s = 1; s < out->strandCount(); ++s) {
        if ((*ids)[s] != (*ids)[0]) continue;
        const std::size_t n = out->strandPointCount(s);
        was += (before->strandPoints(s)[n - 1] - before->strandPoints(0)[n - 1]).length();
        now += (out->strandPoints(s)[n - 1] - out->strandPoints(0)[n - 1]).length();
    }
    EXPECT_LT(now, 0.6f * was);
    // At randomness 0 (each center its even strand, by its ID, the strands clumping by its root) the
    // same strands as guides on its centers input: the same clumps.
    lg.g.node(clump).params().set("randomness", 0.0);
    const StrandsPtr evenOut = lg.eval(clump);
    ids = evenOut->strandChannel("clumpId");
    const GuidesPtr picked = ownCenters(lg, clump);
    ASSERT_GT(picked->size(), 0u);
    lg.ext.set("guides", picked, picked->contentHash());
    const NodeId guides = lg.g.addNode(op::kGuides);
    const NodeId fed = lg.add(op::kClump, lg.scatter);
    lg.g.connect(guides, fed, 1);
    lg.g.node(fed).params().set("centersFrom", std::string("inputPoints"));
    const StrandsPtr same = lg.eval(fed);
    EXPECT_EQ(maxDisplacement(*same, *evenOut), 0.0f);
    EXPECT_EQ(*same->strandChannel("clumpId"), *ids);
    // centersFrom decides, never the link: pick ignores linked centers (a note says how to use
    // them), and an input mode with nothing linked clumps nothing (a note says what to change).
    lg.g.node(clump).params().set("count", std::int64_t{5});
    lg.g.node(fed).params() = lg.g.node(clump).params();  // centersFrom pick, as the clump's
    EXPECT_EQ(lg.eval(fed)->contentHash(), lg.eval(clump)->contentHash());
    const auto noteOf = [&](NodeId node) {
        const auto it = lg.g.lastNotes().find(node);
        return it == lg.g.lastNotes().end() ? std::string() : it->second;
    };
    EXPECT_NE(noteOf(fed).find("centersFrom is pick"), std::string::npos) << noteOf(fed);
    lg.g.disconnect(fed, 1);
    lg.g.node(fed).params().set("centersFrom", std::string("inputCurves"));
    EXPECT_EQ(maxDisplacement(*before, *lg.eval(fed)), 0.0f);
    EXPECT_NE(noteOf(fed).find("connect centers"), std::string::npos) << noteOf(fed);
    lg.g.node(clump).params().set("count", std::int64_t{12});
    // None picked: the strands as they came, each in no clump.
    lg.g.node(clump).params().set("count", std::int64_t{0});
    const StrandsPtr none = lg.eval(clump);
    EXPECT_EQ(maxDisplacement(*before, *none), 0.0f);
    ASSERT_NE(none->strandChannel("clumpId"), nullptr);
    for (const float id : *none->strandChannel("clumpId")) EXPECT_EQ(id, -1.0f);
}

TEST(ClumpCenters, StrandsWithoutPointsAreNeverCentersNorRead) {
    // Strands of no points (an empty guide's, a cut to nothing) among the rest: no read past their
    // points, never a center; Merge's seam blend skips them the same way.
    auto in = uprights({0.1f, 0.3f, 0.5f, 0.7f, 0.9f});
    in->addStrand(900, {});
    in->addStrand(901, {});
    auto op = OperatorRegistry::instance().create(op::kClump);
    op->params().set("count", std::int64_t{10});
    const StrandsPtr clumped = valueAs<StrandsPtr>(op->evaluate(EvalContext{}, {StrandsPtr(in), Value{}}));
    ASSERT_TRUE(clumped);
    EXPECT_EQ(clumped->strandCount(), in->strandCount());
    ClumpState state;
    ASSERT_TRUE(clumpState(*op, clumped, &state));
    EXPECT_GT(state.curves.size(), 0u);
    const std::vector<float>& ids = *clumped->strandChannel("clumpId");
    EXPECT_EQ(ids[ids.size() - 1], -1.0f);  // the empty strands: in no clump
    EXPECT_EQ(ids[ids.size() - 2], -1.0f);
    auto other = uprights({0.15f, 0.35f});
    other->addStrand(902, {});
    const StrandsPtr merged = valueAs<StrandsPtr>(applyOp(op::kMerge, {StrandsPtr(in), StrandsPtr(other)}, [](ParamSet& p) {
        p.set("blend", 0.5);
    }));
    ASSERT_TRUE(merged);
    EXPECT_EQ(merged->strandCount(), in->strandCount() + other->strandCount());
}

TEST(Mirror, TheMirroredCopysGroupsAreItsOwn) {
    // Clumps, braids and feathers numbered on the kept side are numbered again on the mirror (as
    // Merge numbers its second input's: from the next block of group numbers), so the two sides
    // never share a clump.
    auto in = uprights({0.2f, 0.4f, 0.6f, 0.8f});
    for (const char* name : {"clumpId", "braidId", "featherId"}) in->addStrandChannel(name, -1.0f) = {0.0f, 0.0f, 1.0f, -1.0f};
    const StrandsPtr out = valueAs<StrandsPtr>(applyOp(op::kMirror, {StrandsPtr(in)}, [](ParamSet& p) {
        p.set("mode", std::string("add"));
    }));
    ASSERT_EQ(out->strandCount(), 8u);
    for (const char* name : {"clumpId", "braidId", "featherId"}) {
        const std::vector<float>& ids = *out->strandChannel(name);
        const auto base = static_cast<float>(kGroupNumbers);
        const std::vector<float> want = {0.0f, 0.0f, 1.0f, -1.0f, base, base, base + 1.0f, -1.0f};
        EXPECT_EQ(ids, want) << name;
    }
}

}  // namespace
}  // namespace groomlab
