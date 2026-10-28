// SPDX-License-Identifier: Apache-2.0
// The textured-hair and shaping nodes: Curl's shapes, phase and clump coherence; Clump's twist
// and flatness; Gravity, Displace and Mirror; Noise's kinky mode; Width's randomness.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "groomlab/curl_phase.h"
#include "groomlab/curve.h"
#include "groomlab/distance_field.h"
#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_set.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Straight strands grown up from a plane scalp (+Y), and nodes after them.
struct ShapeGraph {
    Graph g;
    ExternalInputs ext;
    NodeId scalp = 0, scatter = 0, segmentsNode = 0;

    explicit ShapeGraph(double density = 200.0) {
        ext.set("scalp", test::makePlane(8), 1);
        scalp = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", density);
        segmentsNode = g.addNode(op::kResample);  // unconnected: it only holds the strands' segments
        g.node(segmentsNode).params().set("segments", std::int64_t{31});
        g.connect(scalp, scatter, 0);
    }
    NodeId add(const char* type, NodeId input, const std::map<std::string, ParamValue>& params = {}) {
        const NodeId id = g.addNode(type);
        g.connect(input, id, 0);
        for (const auto& [k, v] : params) g.node(id).params().set(k, v);
        return id;
    }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
    std::string note(NodeId id) {
        eval(id);
        for (const auto& [node, text] : g.lastNotes()) {
            if (node == id) return text;
        }
        return {};
    }
};

// Point i of strand s moved by the node: after minus before.
Vec3f moved(const StrandSet& after, const StrandSet& before, std::size_t s, std::size_t i) {
    return after.strandPoints(s)[i] - before.strandPoints(s)[i];
}

// Eight strands along one curved shape (bent in two planes), sampled at `n` points evenly by
// parameter (not by arc length, as nodes upstream leave them), in two clumps of four, curled.
std::shared_ptr<StrandSet> bends(std::size_t n) {
    auto strands = std::make_shared<StrandSet>();
    std::vector<float>& ids = strands->addStrandChannel("clumpId", -1.0f);
    for (int k = 0; k < 8; ++k) {
        std::vector<Vec3f> pts(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float lean = 0.05f * static_cast<float>(k);
            pts[i] = Vec3f(0.4f * std::sin(1.5f * t) + 0.3f * static_cast<float>(k), 2.0f * t, (0.3f + lean) * (1.0f - std::cos(2.0f * t * t)));
        }
        strands->addStrand(static_cast<StrandId>(k + 1), pts);
        ids.push_back(static_cast<float>(k / 4));
    }
    return strands;
}
StrandsPtr curledBends(std::size_t n, const std::map<std::string, ParamValue>& params) {
    auto curl = OperatorRegistry::instance().create(op::kCurl);
    for (const auto& [k, v] : params) curl->params().set(k, v);
    EvalContext ctx;
    return valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(StrandsPtr(bends(n)))}));
}

TEST(Curl, ChangedStrandsCurlAgainWithThePointsTheyHad) {
    // Points added (1.8 points a turn coming in): strands moved a little curl again into the last
    // result, as a full evaluation makes them; a strand whose count would change: in full. Evenly
    // spaced, and tightening toward the tip at random spacing.
    for (const bool spaced : {false, true}) {
        SCOPED_TRACE(spaced ? "spaced" : "even");
        auto curl = OperatorRegistry::instance().create(op::kCurl);
        curl->params().set("radius", 0.1);
        curl->params().set("frequency", 4.0);
        if (spaced) {
            curl->params().set("frequencyProfile", Ramp{{0.0f, 0.3f}, {1.0f, 1.0f}});
            curl->params().set("spacingRandom", 0.6);
        }
        EvalContext ctx;
        const std::shared_ptr<StrandSet> before = bends(17);
        const StrandsPtr last = valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(StrandsPtr(before))}));
        ASSERT_GT(last->pointCount(), before->pointCount());
        for (const float push : {0.001f, 0.5f}) {
            auto moved = std::make_shared<StrandSet>(*before);
            for (const std::size_t s : {1u, 5u}) {
                Vec3f* p = moved->strandPoints(s);
                for (std::size_t i = 1; i < 17; ++i) p[i] = p[i] + Vec3f(push, 0.0f, 0.0f) * static_cast<float>(i) * (1.0f + p[i].y);
            }
            IncrementalInputs incremental;
            incremental.previous = Value(last);
            incremental.inputs.push_back(std::make_shared<const StrandChanges>(StrandChanges::listed({1, 5})));
            Value got;
            StrandChanges changed;
            const bool done = curl->evaluateIncremental(ctx, {Value(StrandsPtr(moved))}, incremental, &got, &changed);
            const StrandsPtr fresh = valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(StrandsPtr(moved))}));
            if (push < 0.01f) {
                ASSERT_TRUE(done);
                const StrandsPtr out = valueAs<StrandsPtr>(got);
                ASSERT_EQ(out->offsets(), fresh->offsets());
                EXPECT_EQ(out->positions(), fresh->positions());
                EXPECT_EQ(out->widths(), fresh->widths());
                EXPECT_EQ(changed.strands, (std::vector<std::uint32_t>{1, 5}));
            } else {
                // Stretched to many more turns: more points, another layout.
                EXPECT_NE(fresh->pointCount(), last->pointCount());
                EXPECT_FALSE(done);
            }
        }
    }
}

TEST(Curl, ConvergesToOneCurveAtAnyPointCount) {
    // The curl is a curve of the strand's own shape, not of its points: at 17 to 129 points each
    // point lies on the curl made at 257 (its point at the same share), off it by the smooth
    // curve's own approximation alone, far below the chords of a turn at that count, and from 33
    // points on (an axis of 32 samples on the strand's curve, as at 257) falling about threefold
    // or more with each doubling (as fast as a polyline's chords close in on the curve, O(1/n²),
    // give or take; the curl before halved, O(1/n), from a kink at the root and frames of the
    // points).
    for (const char* coherence : {"strand", "clump"}) {
        for (const char* shape : {"helix", "wave", "zigzag"}) {
            const std::map<std::string, ParamValue> params{{"radius", 0.1},           {"frequency", 2.0},
                                                           {"shape", std::string(shape)}, {"coherence", std::string(coherence)},
                                                           {"tolerance", 0.0},        {"profile", Ramp{{0.0f, 0.2f}, {0.3f, 1.0f}}}};
            const StrandsPtr dense = curledBends(257, params);
            double previous = 0.0;
            for (const std::size_t n : {17u, 33u, 65u, 129u}) {
                const StrandsPtr coarse = curledBends(n, params);
                const std::size_t step = 256 / (n - 1);
                double worst = 0.0;
                for (std::size_t s = 0; s < coarse->strandCount(); ++s) {
                    for (std::size_t i = 0; i < n; ++i) {
                        worst = std::max(worst, double((coarse->strandPoints(s)[i] - dense->strandPoints(s)[i * step]).length()));
                    }
                }
                // The chord of a turn sampled at this many points (about 4.4 turns along the strand).
                const double chord = 0.1 * (1.0 - std::cos(3.14159265 * 4.4 / double(n - 1)));
                std::printf("%s %s n %zu: off the dense curl %.2e (chord %.2e)\n", coherence, shape, n, worst, chord);
                EXPECT_LT(worst, 0.25 * chord) << coherence << " " << shape << " at " << n << " points";
                if (n > 33) EXPECT_LT(worst, 0.45 * previous) << coherence << " " << shape << " at " << n << " points";
                previous = worst;
            }
        }
    }
}

// The farthest any point of `dense` is from the polyline q.
float farthestFrom(const Vec3f* dense, std::size_t n, const Vec3f* q, std::size_t m) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        float best = 1e30f;
        for (std::size_t s = 0; s + 1 < m; ++s) {
            const Vec3f ab = q[s + 1] - q[s];
            const float len2 = ab.dot(ab);
            const float u = len2 > 0.0f ? std::clamp((dense[i] - q[s]).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
            best = std::min(best, (dense[i] - (q[s] + ab * u)).length());
        }
        worst = std::max(worst, best);
    }
    return worst;
}

TEST(Curl, AddsPointsWhereATurnHasTooFew) {
    // About 9 turns over 16 segments (1.8 points a turn: a polyline of them is no curl at all):
    // by default Curl adds points to keep 4 a turn (tolerance 0.3: no curl aliases into another),
    // with tolerance 0.1 to keep 7 (a segment strays at most a tenth of the radius from the
    // curl), each on the curl the strands make at 256 segments.
    for (const char* coherence : {"strand", "clump"}) {
        const std::map<std::string, ParamValue> params{{"radius", 0.1}, {"frequency", 4.0}, {"coherence", std::string(coherence)},
                                                       {"randomness", 0.0}, {"tolerance", 0.1}};
        auto with = [&](std::map<std::string, ParamValue> more) {
            for (const auto& [k, v] : params) more.emplace(k, v);
            return more;
        };
        const StrandsPtr dense = curledBends(257, with({{"tolerance", 0.0}}));
        const StrandsPtr own = curledBends(17, with({{"tolerance", 0.0}}));
        const StrandsPtr added = curledBends(17, with({}));
        const StrandsPtr fewer = curledBends(17, with({{"tolerance", 0.3}}));
        const StrandsPtr capped = curledBends(17, with({{"maxSegments", std::int64_t{40}}}));
        for (std::size_t s = 0; s < added->strandCount(); ++s) {
            ASSERT_EQ(own->strandPointCount(s), 17u);
            const std::size_t n = added->strandPointCount(s);
            EXPECT_GE(n, 17u * 3) << coherence;
            EXPECT_LT(fewer->strandPointCount(s), n) << coherence;
            EXPECT_GE(fewer->strandPointCount(s), 17u * 2) << coherence;
            // At 4 a turn, within the chord a square leaves (r (1 - cos(pi / 4)) = 0.029).
            EXPECT_LT(farthestFrom(dense->strandPoints(s), 257, fewer->strandPoints(s), fewer->strandPointCount(s)), 0.032f);
            EXPECT_LE(capped->strandPointCount(s), 41u) << coherence;
            EXPECT_GT(capped->strandPointCount(s), 17u) << coherence;
            EXPECT_EQ(added->strandPoints(s)[0], own->strandPoints(s)[0]);  // the root stays
            // The curl the dense strands make, within the chord a 7-gon leaves (r (1 - cos(pi / 7))
            // = 0.0099): the added points lie on it.
            const float off = farthestFrom(dense->strandPoints(s), 257, added->strandPoints(s), n);
            const float aliased = farthestFrom(dense->strandPoints(s), 257, own->strandPoints(s), 17);
            EXPECT_LT(off, 0.012f) << coherence << " strand " << s;
            EXPECT_GT(aliased, 0.05f) << coherence << " strand " << s;
            // Each point the dense curl's own, off it by its chords alone.
            EXPECT_LT(farthestFrom(added->strandPoints(s), n, dense->strandPoints(s), 257), 2e-3f) << coherence << " strand " << s;
        }
    }
}

TEST(Curl, AddedPointsCarryTheStrandsWidthsAndChannels) {
    auto strands = std::make_shared<StrandSet>();
    std::vector<Vec3f> pts;
    std::vector<float> widths;
    for (int i = 0; i <= 8; ++i) {
        pts.emplace_back(0.0f, 0.25f * static_cast<float>(i), 0.0f);
        widths.push_back(0.01f + 0.001f * static_cast<float>(i));
    }
    strands->addStrand(1, pts, widths);
    std::vector<float>& shade = strands->addPointChannel("shade");
    for (int i = 0; i <= 8; ++i) shade[static_cast<std::size_t>(i)] = static_cast<float>(i);
    strands->addStrandChannel("tag", 0.0f)[0] = 7.0f;
    auto curl = OperatorRegistry::instance().create(op::kCurl);
    curl->params().set("frequency", 6.0);
    curl->params().set("radius", 0.05);
    EvalContext ctx;
    const StrandsPtr out = valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(StrandsPtr(strands))}));
    const std::size_t n = out->strandPointCount(0);
    ASSERT_GT(n, 9u);
    ASSERT_TRUE(out->validate());
    EXPECT_NE(curl->note(ctx).find("added points to 1 strand"), std::string::npos) << curl->note(ctx);
    EXPECT_EQ((*out->strandChannel("tag"))[0], 7.0f);
    const std::vector<float>& outShade = *out->pointChannel("shade");
    for (std::size_t k = 1; k < n; ++k) {
        // Along the strand, as the channel and the widths are: each the same share between its two
        // incoming points.
        EXPECT_GE(outShade[k], outShade[k - 1]);
        EXPECT_NEAR(out->widths()[k], 0.01f + 0.001f * outShade[k], 1e-6f);
    }
    EXPECT_EQ(outShade.front(), 0.0f);
    EXPECT_EQ(outShade.back(), 8.0f);
}

TEST(Curl, AZigzagKeepsItsKinks) {
    // Straight strands, a zigzag of 3 turns a unit over 31 segments (about 10 points a turn, so
    // the segments cut its kinks, by up to a fifth of its radius): with the tolerance (0.1) Curl
    // adds a point at each kink cut by more, so every kink past the ease keeps 90% of its peak;
    // with the strands' own points alone some fall short.
    ShapeGraph sg;
    const StrandsPtr before = sg.eval(sg.scatter);
    const std::map<std::string, ParamValue> common{{"shape", std::string("zigzag")}, {"radius", 0.05}, {"randomness", 0.0},
                                                   {"phaseRandom", 0.0}, {"phase", 0.1}, {"frequency", 3.0},
                                                   {"profile", Ramp::constant(1.0f)}, {"tolerance", 0.1}};
    // The farthest strand s reaches from its straight base line near `arc` along it.
    auto reach = [&](const StrandSet& out, std::size_t s, float arc) {
        const Vec3f root = before->strandPoints(s)[0];
        const Vec3f axis = (before->strandPoints(s)[before->strandPointCount(s) - 1] - root).normalized();
        float most = 0.0f;
        for (std::size_t i = 0; i < out.strandPointCount(s); ++i) {
            const Vec3f d = out.strandPoints(s)[i] - root;
            if (std::fabs(d.dot(axis) - arc) < 0.04f) most = std::max(most, (d - axis * d.dot(axis)).length());
        }
        return most;
    };
    auto kinked = common;
    const StrandsPtr withKinks = sg.eval(sg.add(op::kCurl, sg.scatter, kinked));
    kinked["tolerance"] = 0.0;
    const StrandsPtr without = sg.eval(sg.add(op::kCurl, sg.scatter, kinked));
    std::size_t fallShort = 0;
    for (std::size_t s = 0; s < before->strandCount(); ++s) {
        const float total = arcLength(before->strandPoints(s), before->strandPointCount(s));
        // Kinks past the ease (a quarter turn), at 0.1 + k/2 turns.
        for (int k = 0; k < 64; ++k) {
            const float arc = (0.25f - 0.1f + 0.5f * static_cast<float>(k)) / 3.0f;
            if (!(arc > 0.25f / 3.0f + 1e-3f && arc < total - 0.04f)) continue;
            EXPECT_GE(reach(*withKinks, s, arc), 0.9f * 0.05f - 1e-4f) << "strand " << s << " kink " << k;
            fallShort += reach(*without, s, arc) < 0.9f * 0.05f - 1e-4f;
        }
        EXPECT_GT(withKinks->strandPointCount(s), before->strandPointCount(s)) << "strand " << s;
        EXPECT_EQ(without->strandPointCount(s), before->strandPointCount(s)) << "strand " << s;
    }
    EXPECT_GT(fallShort, 0u);
}

// ---- The spacing: frequencyProfile and spacingRandom (curl_phase.h) ----------------------------

TEST(Curl, SpacingConvergesToOneCurveAtAnyPointCount) {
    // Curls tightening toward the tip (frequencyProfile: smooth, and straight with a key between
    // the axis's samples) and spaced at random along each strand (spacingRandom): the angle is the
    // integral of the local frequency along the axis, so, as Curl.ConvergesToOneCurveAtAnyPointCount,
    // each point at 17 to 129 points lies on the curl made at 257 (its point at the same share), off
    // it by the smooth curve's own approximation alone, falling twofold or more with each doubling
    // from 33 on (the turns here up to 4 a unit). Weighted too (weight 1.5 along +Y, the way
    // these strands run: the stretch joins the profiled arc, so the same holds).
    Ramp straight{{0.0f, 0.3f}, {0.37f, 1.0f}, {1.0f, 0.6f}};
    straight.straight = true;
    const Ramp profiles[] = {Ramp{{0.0f, 0.2f}, {0.5f, 0.55f}, {1.0f, 1.0f}}, straight};
    double length = 0.0;  // the longest strand
    const std::shared_ptr<StrandSet> shape257 = bends(257);
    for (std::size_t s = 0; s < shape257->strandCount(); ++s) length = std::max(length, double(arcLength(shape257->strandPoints(s), 257)));
    for (const char* coherence : {"strand", "clump"}) {
        for (const char* shape : {"helix", "wave", "zigzag", "coil"}) {
            for (std::size_t p = 0; p < 3; ++p) {
                std::map<std::string, ParamValue> params{
                    {"radius", 0.1},           {"coilWidths", 0.1},       {"frequency", 2.0},
                    {"spacingRandom", 0.8},    {"shape", std::string(shape)}, {"coherence", std::string(coherence)},
                    {"tolerance", 0.0},        {"profile", Ramp{{0.0f, 0.2f}, {0.3f, 1.0f}}}, {"frequencyProfile", profiles[p % 2]}};
                if (p == 2) {  // weighted
                    params["weight"] = 1.5;
                    params["weightDirection"] = Vec3f{0.0f, 1.0f, 0.0f};
                }
                const StrandsPtr dense = curledBends(257, params);
                double previous = 0.0;
                for (const std::size_t n : {17u, 33u, 65u, 129u}) {
                    const StrandsPtr coarse = curledBends(n, params);
                    const std::size_t step = 256 / (n - 1);
                    double worst = 0.0;
                    for (std::size_t s = 0; s < coarse->strandCount(); ++s) {
                        for (std::size_t i = 0; i < n; ++i) {
                            worst = std::max(worst, double((coarse->strandPoints(s)[i] - dense->strandPoints(s)[i * step]).length()));
                        }
                    }
                    // The chord of a turn at the tightest (the frequency at most varied by
                    // randomness, times the profile's peak, 1, times 1 + spacingRandom).
                    const double perSegment = 2.0 * 1.15 * 1.8 * length / double(n - 1);
                    const double chord = 0.1 * (1.0 - std::cos(3.14159265 * std::min(perSegment, 1.0)));
                    std::printf("%s %s profile %zu n %zu: off the dense curl %.2e (chord %.2e)\n", coherence, shape, p, n, worst, chord);
                    EXPECT_LT(worst, 0.25 * chord) << coherence << " " << shape << " profile " << p << " at " << n << " points";
                    if (n > 33) EXPECT_LT(worst, 0.5 * previous) << coherence << " " << shape << " profile " << p << " at " << n << " points";
                    previous = worst;
                }
            }
        }
    }
}

TEST(Curl, PhaseNeverTurnsBack) {
    // The spacing's closed form (curl_phase.h): the turns are the integral of the local frequency
    // (their slope is it, to a central difference), keep the frequency's sign and never turn back
    // (|n| < 1 at spacingRandom 1).
    for (const std::uint32_t key : {1u, 77u, 0xDEADBEEFu, curlSpacingKey(0.1f, 0.7f, 0.3f, 0.9f)}) {
        for (const float frequency : {2.5f, -1.7f}) {
            for (const float spacing : {0.3f, 1.0f}) {
                const float sign = frequency < 0.0f ? -1.0f : 1.0f;
                const float step = 0.01f / std::fabs(frequency);  // a hundredth of a turn
                float last = 0.0f;
                for (int k = 1; k < 20000; ++k) {  // 200 turns
                    const float w = step * static_cast<float>(k);
                    const float turns = curlTurns(frequency, w, spacing, key);
                    ASSERT_GE(sign * (turns - last), -1e-5f) << "key " << key << " at " << w;
                    last = turns;
                    const float n = curlSpacingRate(key, std::fabs(frequency) * w);
                    ASSERT_LT(std::fabs(n), 1.0f);
                    if (k < 400 && k % 7 == 0) {
                        const double slope = (double(curlTurns(frequency, w + step, spacing, key)) - double(curlTurns(frequency, w - step, spacing, key))) /
                                             (2.0 * double(step));
                        EXPECT_NEAR(slope, curlLocalFrequency(frequency, 1.0f, w, spacing, key), 2e-3 * std::fabs(frequency)) << "key " << key << " at " << w;
                        EXPECT_NEAR(curlAngle(frequency, w, 0.3f, spacing, key), 0.3f + kTwoPi * turns, 1e-4f);
                    }
                }
            }
        }
    }
    // A helix about straight strands (constant frames: its angle read off each offset), tightening
    // past a stretch of no turns at all (frequencyProfile 0) at the most random spacing: each step
    // along the strand turns it on, or not at all, never back.
    ShapeGraph sg;
    const StrandsPtr before = sg.eval(sg.scatter);
    Ramp stops{{0.0f, 0.5f}, {0.4f, 0.0f}, {0.6f, 0.0f}, {1.0f, 1.0f}};
    stops.straight = true;
    const NodeId curl = sg.add(op::kCurl, sg.scatter,
                               {{"radius", 0.05}, {"randomness", 0.0}, {"phaseRandom", 0.0}, {"frequency", 4.0},
                                {"spacingRandom", 1.0}, {"frequencyProfile", stops}, {"tolerance", 0.0}});
    const StrandsPtr out = sg.eval(curl);
    std::size_t stopped = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        const Vec3f along = (before->strandPoints(s)[n - 1] - before->strandPoints(s)[0]).normalized();
        const Vec3f a = moved(*out, *before, s, n - 1).normalized(), b = along.cross(a);
        float previous = 0.0f;
        bool first = true;
        for (std::size_t i = 1; i < n; ++i) {
            const Vec3f d = moved(*out, *before, s, i);
            if (d.length() < 1e-4f) continue;  // (at the root: no radius yet)
            const float angle = std::atan2(d.dot(b), d.dot(a));
            if (!first) {
                float turn = angle - previous;
                while (turn <= -kTwoPi / 2.0f) turn += kTwoPi;
                while (turn > kTwoPi / 2.0f) turn -= kTwoPi;
                EXPECT_GE(turn, -1e-4f) << "strand " << s << " point " << i;
                const float t0 = static_cast<float>(i - 1) / static_cast<float>(n - 1), t1 = static_cast<float>(i) / static_cast<float>(n - 1);
                if (t0 >= 0.4f && t1 <= 0.6f) {
                    EXPECT_NEAR(turn, 0.0f, 1e-3f) << "strand " << s << " point " << i;  // no turns where the profile is 0
                    ++stopped;
                }
            }
            previous = angle;
            first = false;
        }
    }
    EXPECT_GT(stopped, 0u);
}

TEST(Curl, FrequencyProfileTightensTowardTheTip) {
    // About straight strands of evenly spaced points (length L), frequency 3f and frequencyProfile
    // rising from a third at the root to all of it at the tip: the angle at share t is
    // 2 pi f L (t + t²) + phase, the integral of the local frequency f (1 + 2t). A zigzag's kinks
    // there (where the angle is pi/2 + k pi) keep their peaks with the tolerance's points, however
    // tight they get.
    ShapeGraph sg;
    const StrandsPtr before = sg.eval(sg.scatter);
    const Ramp rising{{0.0f, 1.0f / 3.0f}, {1.0f, 1.0f}};
    const float f = 2.0f, phase = 0.1f;
    const NodeId helix = sg.add(op::kCurl, sg.scatter,
                                {{"radius", 0.05}, {"randomness", 0.0}, {"phaseRandom", 0.0}, {"phase", double(phase)},
                                 {"frequency", 3.0 * f}, {"frequencyProfile", rising}, {"tolerance", 0.0}});
    const StrandsPtr out = sg.eval(helix);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        const float length = arcLength(before->strandPoints(s), n);
        const Vec3f along = (before->strandPoints(s)[n - 1] - before->strandPoints(s)[0]).normalized();
        // From a point past the ease (the profile's radius full from a quarter of the strand on).
        const std::size_t from = (n - 1) / 3;
        const Vec3f a = moved(*out, *before, s, from).normalized(), b = along.cross(a);
        auto expected = [&](std::size_t i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            return kTwoPi * f * length * (t + t * t);
        };
        float unwrapped = 0.0f, previous = 0.0f;
        for (std::size_t i = from + 1; i < n; ++i) {
            const Vec3f d = moved(*out, *before, s, i);
            const float angle = std::atan2(d.dot(b), d.dot(a));
            float turn = angle - previous;
            while (turn <= -kTwoPi / 2.0f) turn += kTwoPi;
            while (turn > kTwoPi / 2.0f) turn -= kTwoPi;
            unwrapped += turn;
            previous = angle;
            EXPECT_NEAR(unwrapped, expected(i) - expected(from), 2e-3f) << "strand " << s << " point " << i;
        }
    }
    // The zigzag's kinks.
    auto reach = [&](const StrandSet& curled, std::size_t s, float arc) {
        const Vec3f root = before->strandPoints(s)[0];
        const Vec3f axis = (before->strandPoints(s)[before->strandPointCount(s) - 1] - root).normalized();
        float most = 0.0f;
        for (std::size_t i = 0; i < curled.strandPointCount(s); ++i) {
            const Vec3f d = curled.strandPoints(s)[i] - root;
            if (std::fabs(d.dot(axis) - arc) < 0.01f) most = std::max(most, (d - axis * d.dot(axis)).length());
        }
        return most;
    };
    std::map<std::string, ParamValue> zigzag{{"shape", std::string("zigzag")}, {"radius", 0.05}, {"randomness", 0.0},
                                             {"phaseRandom", 0.0}, {"phase", double(phase)}, {"frequency", 3.0 * f},
                                             {"profile", Ramp::constant(1.0f)}, {"frequencyProfile", rising}, {"tolerance", 0.1}};
    const StrandsPtr withKinks = sg.eval(sg.add(op::kCurl, sg.scatter, zigzag));
    zigzag["tolerance"] = 0.0;
    const StrandsPtr without = sg.eval(sg.add(op::kCurl, sg.scatter, zigzag));
    std::size_t kinks = 0, fallShort = 0;
    for (std::size_t s = 0; s < before->strandCount(); ++s) {
        const std::size_t n = before->strandPointCount(s);
        const float length = arcLength(before->strandPoints(s), n);
        // Kink k at angle pi/2 + k pi: the turns 2 f L (t + t²) + 2 phase = k + 1/2.
        for (int k = 0; k < 64; ++k) {
            const float c = (static_cast<float>(k) + 0.5f) / 2.0f - phase;  // in turns from the start
            const float t = 0.5f * (-1.0f + std::sqrt(1.0f + 4.0f * c / (f * length)));
            if (!(c > 0.0f) || !(t * f * length * (1.0f + t) > 0.25f + 1e-3f) || t > 1.0f - 0.04f) continue;  // past the ease, before the tip
            ++kinks;
            EXPECT_GE(reach(*withKinks, s, t * length), 0.9f * 0.05f - 1e-4f) << "strand " << s << " kink " << k;
            fallShort += reach(*without, s, t * length) < 0.9f * 0.05f - 1e-4f;
        }
    }
    EXPECT_GT(kinks, before->strandCount() * 4);
    EXPECT_GT(fallShort, 0u);
}

TEST(Curl, AddsPointsWhereTheTurnsAreTighter) {
    // Curls tightening toward the tip (and spaced at random): the points a turn the tolerance asks
    // for follow the local frequency, so a strand gets the points its tightest turns need (more
    // than at its root's frequency, no more than at its tip's everywhere), and each lies on the
    // curl the strands make at 256 segments.
    for (const char* coherence : {"strand", "clump"}) {
        for (const double spacing : {0.0, 0.7}) {
            SCOPED_TRACE(std::string(coherence) + " spacing " + std::to_string(spacing));
            const std::map<std::string, ParamValue> params{{"radius", 0.1},      {"frequency", 5.0},
                                                           {"coherence", std::string(coherence)},
                                                           {"randomness", 0.0},  {"tolerance", 0.1},
                                                           {"spacingRandom", spacing},
                                                           {"frequencyProfile", Ramp{{0.0f, 0.1f}, {1.0f, 1.0f}}}};
            auto with = [&](std::map<std::string, ParamValue> more) {
                for (const auto& [k, v] : params) more.emplace(k, v);
                return more;
            };
            const StrandsPtr dense = curledBends(257, with({{"tolerance", 0.0}}));
            const StrandsPtr tight = curledBends(17, with({}));
            const StrandsPtr loose = curledBends(17, with({{"frequencyProfile", Ramp::constant(0.1f)}, {"spacingRandom", 0.0}}));
            const StrandsPtr everywhere =
                curledBends(17, with({{"frequency", 5.0 * (1.0 + spacing)}, {"frequencyProfile", Ramp::constant(1.0f)}, {"spacingRandom", 0.0}}));
            for (std::size_t s = 0; s < tight->strandCount(); ++s) {
                const std::size_t n = tight->strandPointCount(s);
                EXPECT_GT(n, loose->strandPointCount(s)) << "strand " << s;
                EXPECT_LE(n, everywhere->strandPointCount(s)) << "strand " << s;
                // Within the chord a 7-gon leaves (r (1 - cos(pi / 7)) = 0.0099).
                EXPECT_LT(farthestFrom(dense->strandPoints(s), 257, tight->strandPoints(s), n), 0.012f) << "strand " << s;
                // Each point the dense curl's own, off it by the dense polyline's chords alone (at
                // its tightest, 8.5 turns a unit with the spacing's noise: r (1 - cos(pi 8.5 L / 256)),
                // up to 2.6e-3).
                EXPECT_LT(farthestFrom(tight->strandPoints(s), n, dense->strandPoints(s), 257), spacing > 0.0 ? 3.5e-3f : 2e-3f) << "strand " << s;
            }
        }
    }
}

TEST(Curl, ClumpCoherenceSharesTheSpacing) {
    // Curling coherently, every member of a clump takes the same offset at the same point with the
    // spacing tightening and varying at random too (one noise a clump); on their own they differ,
    // and the spacing's noise changes the curls.
    ShapeGraph sg;
    const NodeId clump = sg.g.addNode(op::kClump);
    sg.g.connect(sg.scatter, clump, 0);
    sg.g.node(clump).params().set("count", std::int64_t{6});
    sg.g.node(clump).params().set("strength", 0.0);
    const std::map<std::string, ParamValue> common{{"radius", 0.05},
                                                   {"profile", Ramp::constant(1.0f)},
                                                   {"frequency", 3.0},
                                                   {"spacingRandom", 1.0},
                                                   {"frequencyProfile", Ramp{{0.0f, 0.33f}, {1.0f, 1.0f}}}};
    auto coherent = common;
    coherent["coherence"] = std::string("clump");
    auto even = coherent;
    even["spacingRandom"] = 0.0;
    const NodeId together = sg.add(op::kCurl, clump, coherent), alone = sg.add(op::kCurl, clump, common),
                 evenly = sg.add(op::kCurl, clump, even);
    const StrandsPtr before = sg.eval(clump), out = sg.eval(together), own = sg.eval(alone), plain = sg.eval(evenly);
    const std::vector<float>& ids = *before->strandChannel("clumpId");
    std::map<long long, std::size_t> first;
    std::size_t members = 0, apartAlone = 0, respaced = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        float noise = 0.0f;
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) noise = std::max(noise, (out->strandPoints(s)[i] - plain->strandPoints(s)[i]).length());
        respaced += noise > 1e-2f;
        const auto [it, fresh] = first.emplace(std::llround(ids[s]), s);
        if (fresh) continue;
        ++members;
        const std::size_t m = it->second;
        float worst = 0.0f, worstAlone = 0.0f;
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
            worst = std::max(worst, (moved(*out, *before, s, i) - moved(*out, *before, m, i)).length());
            worstAlone = std::max(worstAlone, (moved(*own, *before, s, i) - moved(*own, *before, m, i)).length());
        }
        EXPECT_LT(worst, 1e-5f) << "strand " << s << " and " << m;
        apartAlone += worstAlone > 1e-2f;
    }
    EXPECT_GT(members, out->strandCount() / 2);
    EXPECT_GT(apartAlone, members / 2);
    EXPECT_GT(respaced, out->strandCount() / 2);
}

TEST(Curl, SpacingIsScaleInvariant) {
    // The spacing's noise runs in turns, the profile by the share of the strand: the strands k times
    // larger, with k times the radius and a k-th of the frequency, curl k times larger.
    const float k = 2.5f;
    for (const char* coherence : {"strand", "clump"}) {
        std::map<std::string, ParamValue> params{{"radius", 0.1},
                                                 {"frequency", 1.5},
                                                 {"spacingRandom", 0.8},
                                                 {"coherence", std::string(coherence)},
                                                 {"frequencyProfile", Ramp{{0.0f, 0.2f}, {0.5f, 0.55f}, {1.0f, 1.0f}}}};
        auto curl = OperatorRegistry::instance().create(op::kCurl);
        for (const auto& [name, v] : params) curl->params().set(name, v);
        EvalContext ctx;
        const std::shared_ptr<StrandSet> small = bends(33);
        const StrandsPtr base = valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(StrandsPtr(small))}));
        auto large = std::make_shared<StrandSet>(*small);
        for (Vec3f& p : large->mutablePositions()) p = p * k;
        curl->params().set("radius", 0.1 * k);
        curl->params().set("frequency", 1.5 / k);
        const StrandsPtr scaled = valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(StrandsPtr(large))}));
        ASSERT_EQ(scaled->offsets(), base->offsets());
        float worst = 0.0f;
        for (std::size_t p = 0; p < base->pointCount(); ++p) worst = std::max(worst, (scaled->positions()[p] - base->positions()[p] * k).length());
        EXPECT_LT(worst, 2e-5f * k) << coherence;
    }
}

// Six strands `length` long hanging from roots on a plane at y = 0, `n` points each: straight down
// (`sway` 0) or swaying in x and z, in two clumps of three (clumpId); with `up`, pointing up instead.
std::shared_ptr<StrandSet> hanging(std::size_t n, float length, float sway = 0.0f, bool up = false) {
    auto strands = std::make_shared<StrandSet>();
    std::vector<float>& ids = strands->addStrandChannel("clumpId", -1.0f);
    for (int k = 0; k < 6; ++k) {
        std::vector<Vec3f> pts(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float lean = sway > 0.0f ? 0.05f * static_cast<float>(k) : 0.0f;
            pts[i] = Vec3f(0.5f * static_cast<float>(k) + sway * std::sin(1.5f * t), (up ? 1.0f : -1.0f) * length * t,
                           (sway + lean) * (1.0f - std::cos(2.0f * t * t)));
        }
        strands->addStrand(static_cast<StrandId>(k + 1), pts);
        ids.push_back(static_cast<float>(k / 3));
    }
    return strands;
}
StrandsPtr curled(const StrandsPtr& in, const std::map<std::string, ParamValue>& params) {
    auto curl = OperatorRegistry::instance().create(op::kCurl);
    for (const auto& [k, v] : params) curl->params().set(k, v);
    EvalContext ctx;
    return valueAs<StrandsPtr>(curl->evaluate(ctx, {Value(in)}));
}

TEST(Curl, WeightZeroIsTheCurlAsBefore) {
    // Weight 0 (the default), or no weightDirection: exactly the curl Curl always made, by strand
    // and by clump, spaced and profiled or not, for every shape.
    for (const char* coherence : {"strand", "clump"}) {
        for (const char* shape : {"helix", "zigzag", "coil"}) {
            for (const double spacing : {0.0, 0.6}) {
                SCOPED_TRACE(std::string(coherence) + " " + shape + " " + std::to_string(spacing));
                std::map<std::string, ParamValue> params{{"radius", 0.1},
                                                         {"frequency", 3.0},
                                                         {"coilWidths", 0.1},
                                                         {"shape", std::string(shape)},
                                                         {"coherence", std::string(coherence)},
                                                         {"spacingRandom", spacing}};
                if (spacing > 0.0) params["frequencyProfile"] = Ramp{{0.0f, 0.3f}, {1.0f, 1.0f}};
                const StrandsPtr in = hanging(17, 3.0f, 0.4f);
                const StrandsPtr plain = curled(in, params);
                auto zero = params;
                zero["weight"] = 0.0;
                zero["weightDirection"] = Vec3f{1.0f, 0.0f, 0.0f};
                auto nowhere = params;
                nowhere["weight"] = 1.0;
                nowhere["weightDirection"] = Vec3f{0.0f, 0.0f, 0.0f};
                EXPECT_EQ(curled(in, zero)->positions(), plain->positions());
                EXPECT_EQ(curled(in, nowhere)->positions(), plain->positions());
            }
        }
    }
}

TEST(Curl, WeightSpreadsTheTurnsWhereHairHangsBelowThem) {
    // Straight strands hanging down (length L, f turns a unit): with weight w the local pitch at
    // arc s is (1 + (w / 10) f (L - s)) times the plain one (all the turns below it hang straight
    // down), so the angle is 2 pi f W(s), W = ln((1 + k f L) / (1 + k f (L - s))) / (k f) with k =
    // w / 10: loose near the root, the plain spacing at the tip. The radius keeps a turn's wire
    // length: r' = sqrt(r² - (q² - 1) / (2 pi f)²) at the stretch q, narrowing toward the root
    // (here never below half of r, where it would ease to straight).
    const float length = 10.0f, f = 1.0f, r = 0.3f, w = 1.0f, k = w / 10.0f;
    const StrandsPtr in = hanging(257, length);
    const std::map<std::string, ParamValue> common{{"radius", double(r)},       {"frequency", double(f)},     {"randomness", 0.0},
                                                   {"phaseRandom", 0.0},        {"profile", Ramp::constant(1.0f)},
                                                   {"tolerance", 0.0}};
    auto weighted = common;
    weighted["weight"] = double(w);
    const StrandsPtr out = curled(in, weighted), plain = curled(in, common);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        auto angleAt = [&](const StrandSet& c, std::size_t i) {
            const Vec3f d = c.strandPoints(s)[i] - in->strandPoints(s)[i];
            return std::atan2(d.z, d.x);
        };
        auto expected = [&](float arc) { return kTwoPi / k * std::log((1.0f + k * f * length) / (1.0f + k * f * (length - arc))); };
        const std::size_t from = (n - 1) / 10;
        float unwrapped = 0.0f, previous = angleAt(*out, from);
        float rootTurns = 0.0f, plainRootTurns = 0.0f;
        for (std::size_t i = from + 1; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1), arc = t * length;
            const float angle = angleAt(*out, i);
            float turn = angle - previous;
            while (turn <= -kTwoPi / 2.0f) turn += kTwoPi;
            while (turn > kTwoPi / 2.0f) turn -= kTwoPi;
            unwrapped += turn;
            previous = angle;
            EXPECT_NEAR(std::fabs(unwrapped), expected(arc) - expected(static_cast<float>(from) / static_cast<float>(n - 1) * length), 2e-3f) << "strand " << s << " point " << i;
            // The radius (past the ease) a turn's wire keeps at the stretch.
            const float q = 1.0f + k * f * (length - arc);
            const float want = std::sqrt(std::max(0.0f, r * r - (q * q - 1.0f) / (kTwoPi * f * kTwoPi * f)));
            const Vec3f d = out->strandPoints(s)[i] - in->strandPoints(s)[i];
            if (t >= 0.25f) EXPECT_NEAR(d.length(), want, 1e-4f) << "strand " << s << " point " << i;
            if (t <= 0.3f) rootTurns = std::fabs(unwrapped) / kTwoPi;
        }
        // Near the root, about half the turns of the plain curl (twice the pitch) and a narrower
        // radius; at the tip the plain spacing and radius.
        plainRootTurns = 0.2f * length * f;
        EXPECT_LT(rootTurns, 0.65f * plainRootTurns) << "strand " << s;
        const Vec3f nearRoot = out->strandPoints(s)[64] - in->strandPoints(s)[64];
        const Vec3f nearTip = out->strandPoints(s)[256] - in->strandPoints(s)[256];
        EXPECT_LT(nearRoot.length(), 0.7f * r) << "strand " << s;
        EXPECT_NEAR(nearTip.length(), (plain->strandPoints(s)[256] - in->strandPoints(s)[256]).length(), 1e-4f) << "strand " << s;
    }
    // Overstretched (weight 5: at a quarter of the way down the pitch 4.75 times the plain one, past
    // a turn's wire length): straight there.
    auto heavy = common;
    heavy["weight"] = 5.0;
    const StrandsPtr taut = curled(in, heavy);
    for (std::size_t s = 0; s < taut->strandCount(); ++s) EXPECT_EQ(taut->strandPoints(s)[64], in->strandPoints(s)[64]) << "strand " << s;
    // Long ringlets stretch more than short curls: twice the length carries twice the turns.
    const StrandsPtr shortIn = hanging(129, 0.5f * length);
    const StrandsPtr shortOut = curled(shortIn, weighted);
    auto rootRadius = [&](const StrandSet& c, const StrandSet& before, std::size_t i) { return (c.strandPoints(0)[i] - before.strandPoints(0)[i]).length(); };
    EXPECT_LT(rootRadius(*out, *in, 77), rootRadius(*shortOut, *shortIn, 39));  // both at 30% of their length
    // Every shape, by clump too, with randomness and phases at random: fewer turns near the root
    // than unweighted.
    for (const char* coherence : {"strand", "clump"}) {
        for (const char* shape : {"helix", "wave", "zigzag", "coil"}) {
            SCOPED_TRACE(std::string(coherence) + " " + shape);
            std::map<std::string, ParamValue> params{{"radius", 0.2},
                                                     {"frequency", 1.0},
                                                     {"coilWidths", 0.2},
                                                     {"shape", std::string(shape)},
                                                     {"coherence", std::string(coherence)},
                                                     {"phase", 0.3},
                                                     {"tolerance", 0.0},
                                                     {"spacingRandom", 0.4}};
            const StrandsPtr loose = hanging(65, length, 0.4f);
            const StrandsPtr a = curled(loose, params);
            params["weight"] = 1.0;
            const StrandsPtr b = curled(loose, params);
            // How far the curl wanders over the strand's first third: its path's length less the
            // axis's (tighter turns, more path).
            auto path = [&](const StrandSet& c, std::size_t s) {
                float sum = 0.0f;
                for (std::size_t i = 1; i <= c.strandPointCount(s) / 3; ++i) sum += (c.strandPoints(s)[i] - c.strandPoints(s)[i - 1]).length();
                return sum;
            };
            for (std::size_t s = 0; s < a->strandCount(); ++s) {
                ASSERT_EQ(a->strandPointCount(s), b->strandPointCount(s));
                EXPECT_LT(path(*b, s) - path(*loose, s), 0.8f * (path(*a, s) - path(*loose, s))) << "strand " << s;
            }
        }
    }
}

TEST(Curl, WeightLeavesStrandsPointingUpOrAcross) {
    // Strands pointing against weightDirection, or lying across it, carry no weight: the curl
    // unchanged.
    const std::map<std::string, ParamValue> common{{"radius", 0.2}, {"frequency", 2.0}, {"spacingRandom", 0.3}};
    auto weighted = common;
    weighted["weight"] = 2.0;
    const StrandsPtr up = hanging(33, 4.0f, 0.0f, true);
    const StrandsPtr a = curled(up, common), b = curled(up, weighted);
    ASSERT_EQ(a->offsets(), b->offsets());
    for (std::size_t p = 0; p < a->pointCount(); ++p) EXPECT_NEAR((a->positions()[p] - b->positions()[p]).length(), 0.0f, 1e-6f);
    weighted["weightDirection"] = Vec3f{0.0f, 0.0f, 1.0f};  // across the strands hanging down
    const StrandsPtr down = hanging(33, 4.0f);
    const StrandsPtr c = curled(down, common), d = curled(down, weighted);
    ASSERT_EQ(c->offsets(), d->offsets());
    for (std::size_t p = 0; p < c->pointCount(); ++p) EXPECT_NEAR((c->positions()[p] - d->positions()[p]).length(), 0.0f, 1e-6f);
}

TEST(Curl, WeightIsScaleInvariantAndDeterministic) {
    // The load counts turns, not length: strands 100 times larger, with 100 times the radius and a
    // hundredth of the frequency, curl 100 times larger. Two evaluations make the same strands.
    const float k = 100.0f;
    for (const char* coherence : {"strand", "clump"}) {
        SCOPED_TRACE(coherence);
        std::map<std::string, ParamValue> params{{"radius", 0.2},
                                                 {"frequency", 1.5},
                                                 {"weight", 1.5},
                                                 {"spacingRandom", 0.5},
                                                 {"coherence", std::string(coherence)},
                                                 {"frequencyProfile", Ramp{{0.0f, 0.5f}, {1.0f, 1.0f}}}};
        const std::shared_ptr<StrandSet> small = hanging(33, 4.0f, 0.5f);
        const StrandsPtr base = curled(small, params);
        EXPECT_EQ(curled(small, params)->positions(), base->positions());
        auto large = std::make_shared<StrandSet>(*small);
        for (Vec3f& p : large->mutablePositions()) p = p * k;
        params["radius"] = 0.2 * k;
        params["frequency"] = 1.5 / k;
        const StrandsPtr scaled = curled(large, params);
        ASSERT_EQ(scaled->offsets(), base->offsets());
        float worst = 0.0f;
        for (std::size_t p = 0; p < base->pointCount(); ++p) worst = std::max(worst, (scaled->positions()[p] - base->positions()[p] * k).length());
        EXPECT_LT(worst, 2e-5f * k);
    }
}

TEST(Curl, DefaultsAreTheHelixEachStrandOnItsOwn) {
    // (frequencyProfile 1 everywhere, however many keys say so, and spacingRandom 0: the spacing
    // as it always was.)
    ShapeGraph sg;
    const NodeId plain = sg.add(op::kCurl, sg.scatter);
    const NodeId spelled = sg.add(op::kCurl, sg.scatter,
                                  {{"shape", std::string("helix")}, {"phase", 0.0}, {"phaseRandom", 1.0},
                                   {"coherence", std::string("strand")}, {"coilWidths", 3.0}, {"spacingRandom", 0.0},
                                   {"frequencyProfile", Ramp{{0.0f, 1.0f}, {0.5f, 1.0f}, {1.0f, 1.0f}}}});
    EXPECT_EQ(sg.eval(plain)->positions(), sg.eval(spelled)->positions());
}

TEST(Curl, WavesAndZigzagsLieInAPlane) {
    // Straight strands, every curl starting at the same phase: a wave's offsets are r sin(angle)
    // along one direction across the strand, a zigzag's the triangle wave with sin's peaks (the
    // strand's frames are its rotation-minimizing ones: constant along a straight strand), eased
    // in from the root. (Tolerance 0: the strands' own points alone, no kinks added.)
    ShapeGraph sg;
    const StrandsPtr before = sg.eval(sg.scatter);
    for (const char* shape : {"wave", "zigzag"}) {
        const NodeId curl = sg.add(op::kCurl, sg.scatter,
                                   {{"shape", std::string(shape)}, {"radius", 0.05}, {"randomness", 0.0}, {"phaseRandom", 0.0},
                                    {"frequency", 3.0}, {"profile", Ramp::constant(1.0f)}, {"tolerance", 0.0}});
        const StrandsPtr out = sg.eval(curl);
        float peak = 0.0f;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            Vec3f across;
            float arc = 0.0f;
            for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
                arc += (before->strandPoints(s)[i] - before->strandPoints(s)[i - 1]).length();
                const Vec3f d = moved(*out, *before, s, i);
                const float angle = kTwoPi * 3.0f * arc;
                const float u = angle / kTwoPi + 0.25f;
                const float want = (std::string(shape) == "wave" ? std::sin(angle) : 1.0f - 4.0f * std::fabs(u - std::floor(u) - 0.5f)) *
                                   test::curlEase(arc, static_cast<float>(i) / static_cast<float>(out->strandPointCount(s) - 1), 3.0f);
                EXPECT_NEAR(d.length(), 0.05f * std::fabs(want), 1e-5f) << shape << " strand " << s << " point " << i;
                EXPECT_NEAR(d.dot(before->strandPoints(s)[i] - before->strandPoints(s)[0]), 0.0f, 1e-5f);  // across the strand
                if (d.length() > 1e-3f) {
                    if (across.length() == 0.0f) across = d.normalized();
                    EXPECT_GT(std::fabs(d.normalized().dot(across)), 0.9999f) << shape << ": out of its plane";
                }
                peak = std::max(peak, d.length());
            }
        }
        EXPECT_NEAR(peak, 0.05f, 2e-3f) << shape;
    }
    // Planes differ from strand to strand (a draw of each one's own).
    const NodeId wave = sg.add(op::kCurl, sg.scatter, {{"shape", std::string("wave")}, {"phaseRandom", 0.0}});
    const StrandsPtr out = sg.eval(wave);
    std::size_t turned = 0;
    for (std::size_t s = 1; s < out->strandCount(); ++s) {
        const Vec3f a = moved(*out, *before, 0, 20), b = moved(*out, *before, s, 20);
        turned += std::fabs(a.normalized().dot(b.normalized())) < 0.95f;
    }
    EXPECT_GT(turned, out->strandCount() / 2);
}

TEST(Curl, PhaseStartsEveryCurlAtTheSamePlace) {
    // No randomness of phase: every strand starts its helix at `phase`; half a turn further,
    // each offset is the opposite one.
    ShapeGraph sg;
    const StrandsPtr before = sg.eval(sg.scatter);
    const std::map<std::string, ParamValue> common{{"radius", 0.05}, {"randomness", 0.0}, {"phaseRandom", 0.0}};
    auto with = [&](double phase) {
        auto params = common;
        params["phase"] = phase;
        return sg.eval(sg.add(op::kCurl, sg.scatter, params));
    };
    const StrandsPtr zero = with(0.0), half = with(0.5);
    for (std::size_t s = 0; s < zero->strandCount(); ++s) {
        for (std::size_t i = 1; i < zero->strandPointCount(s); ++i) {
            const Vec3f a = moved(*zero, *before, s, i), b = moved(*half, *before, s, i);
            EXPECT_NEAR((a + b).length(), 0.0f, 1e-5f);
        }
    }
}

TEST(Curl, CoilsAFewStrandWidthsAcross) {
    ShapeGraph sg;
    const NodeId width = sg.add(op::kWidth, sg.scatter, {{"width", 0.01}, {"profile", Ramp::constant(1.0f)}});
    const NodeId coil = sg.add(op::kCurl, width,
                               {{"shape", std::string("coil")}, {"coilWidths", 4.0}, {"randomness", 0.0}, {"frequency", 20.0},
                                {"profile", Ramp::constant(1.0f)}, {"radius", 5.0}, {"tolerance", 0.0}});  // the radius is not a coil's
    const StrandsPtr before = sg.eval(width), out = sg.eval(coil);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        float arc = 0.0f;
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
            arc += (before->strandPoints(s)[i] - before->strandPoints(s)[i - 1]).length();
            EXPECT_NEAR(moved(*out, *before, s, i).length(), 0.04f * test::curlEase(arc, static_cast<float>(i) / static_cast<float>(out->strandPointCount(s) - 1), 20.0f), 1e-5f);
        }
    }
    EXPECT_TRUE(sg.note(coil).empty());
    // On strands without widths of their own (1, before any Width), a note says so.
    const NodeId bare = sg.add(op::kCurl, sg.scatter, {{"shape", std::string("coil")}});
    EXPECT_NE(sg.note(bare).find("Width"), std::string::npos);
}

TEST(Curl, ClumpCoherenceCurlsAClumpAsOne) {
    // Clumped strands (Clump at strength 0 only names their clumps): curling coherently, every
    // member of a clump takes the same offset at the same point; on their own they differ.
    ShapeGraph sg;
    const NodeId clump = sg.g.addNode(op::kClump);
    sg.g.connect(sg.scatter, clump, 0);
    sg.g.node(clump).params().set("count", std::int64_t{6});
    sg.g.node(clump).params().set("strength", 0.0);
    const std::map<std::string, ParamValue> common{{"radius", 0.05}, {"profile", Ramp::constant(1.0f)}};
    auto coherent = common;
    coherent["coherence"] = std::string("clump");
    const NodeId together = sg.add(op::kCurl, clump, coherent), alone = sg.add(op::kCurl, clump, common);
    const StrandsPtr before = sg.eval(clump), out = sg.eval(together), own = sg.eval(alone);
    const std::vector<float>& ids = *before->strandChannel("clumpId");
    std::map<long long, std::size_t> first;
    std::size_t members = 0, apartAlone = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const auto [it, fresh] = first.emplace(std::llround(ids[s]), s);
        if (fresh) continue;
        ++members;
        const std::size_t m = it->second;
        float worst = 0.0f, worstAlone = 0.0f;
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
            worst = std::max(worst, (moved(*out, *before, s, i) - moved(*out, *before, m, i)).length());
            worstAlone = std::max(worstAlone, (moved(*own, *before, s, i) - moved(*own, *before, m, i)).length());
        }
        EXPECT_LT(worst, 1e-5f) << "strand " << s << " and " << m;
        apartAlone += worstAlone > 1e-2f;
    }
    EXPECT_GT(members, out->strandCount() / 2);
    EXPECT_GT(apartAlone, members / 2);
    // No clumps to curl with: an error naming what to do.
    const NodeId lost = sg.add(op::kCurl, sg.scatter, coherent);
    EXPECT_THROW(sg.eval(lost), GraphError);
}


// Clumps of the strands (about 8 centers of the Clump's own) into round locks: strength 1 along the whole length,
// so every point sits in its lock's disk around the center. `extra`: Clump's other params;
// `combed`: the strands combed along the plane (+x) first.
struct Locks {
    ShapeGraph sg;
    NodeId clump = 0;
    explicit Locks(const std::map<std::string, ParamValue>& extra, bool combed = false) {
        const NodeId in = combed ? sg.add(op::kBend, sg.scatter, {{"angle", 180.0}, {"end", 0.0}, {"along", std::string("direction")},
                                                                   {"direction", Vec3f{1, 0, 0}}})
                                 : sg.scatter;
        clump = sg.g.addNode(op::kClump);
        sg.g.connect(in, clump, 0);
        ParamSet& p = sg.g.node(clump).params();
        p.set("count", std::int64_t{8});
        p.set("strength", 1.0);
        p.set("profile", Ramp::constant(1.0f));
        p.set("lockRadius", 0.1);
        for (const auto& [k, v] : extra) p.set(k, v);
    }
    // Each strand's offset at point i from its clump's center there: the mean of its clump's strands.
    std::vector<Vec3f> offsets(const StrandSet& out, std::size_t i) {
        const std::vector<float>& ids = *out.strandChannel("clumpId");
        std::map<long long, std::pair<Vec3f, int>> sums;
        for (std::size_t s = 0; s < out.strandCount(); ++s) {
            auto& [sum, count] = sums[std::llround(ids[s])];
            sum += out.strandPoints(s)[i];
            ++count;
        }
        std::vector<Vec3f> result(out.strandCount());
        for (std::size_t s = 0; s < out.strandCount(); ++s) {
            const auto& [sum, count] = sums[std::llround(ids[s])];
            result[s] = out.strandPoints(s)[i] - sum / static_cast<float>(count);
        }
        return result;
    }
};

TEST(Clump, DefaultsDoNotFlatten) {
    Locks plain({}), spelled({{"flatness", 0.0}, {"flatAlong", std::string("surface")}, {"flatDirection", Vec3f{1, 0, 0}}});
    EXPECT_EQ(plain.sg.eval(plain.clump)->positions(), spelled.sg.eval(spelled.clump)->positions());
}

TEST(Clump, FlatnessSquashesClumpsIntoRibbons) {
    // Upright locks flattened with their width along x: no thickness left along z, the width kept.
    Locks round({}), ribbon({{"flatness", 1.0}, {"flatAlong", std::string("direction")}, {"flatDirection", Vec3f{1, 0, 0}}});
    const StrandsPtr a = round.sg.eval(round.clump), b = ribbon.sg.eval(ribbon.clump);
    const std::size_t tip = a->strandPointCount(0) - 1;
    const std::vector<Vec3f> oa = round.offsets(*a, tip), ob = ribbon.offsets(*b, tip);
    float thick = 0.0f, wide = 0.0f, wideBefore = 0.0f;
    for (std::size_t s = 0; s < b->strandCount(); ++s) {
        thick = std::max(thick, std::fabs(ob[s].z));
        wide = std::max(wide, std::fabs(ob[s].x));
        wideBefore = std::max(wideBefore, std::fabs(oa[s].x));
    }
    EXPECT_LT(thick, 1e-4f);
    EXPECT_NEAR(wide, wideBefore, 0.02f);
    // Half flat: half the thickness.
    Locks half({{"flatness", 0.5}, {"flatAlong", std::string("direction")}, {"flatDirection", Vec3f{1, 0, 0}}});
    const std::vector<Vec3f> oh = half.offsets(*half.sg.eval(half.clump), tip);
    for (std::size_t s = 0; s < b->strandCount(); ++s) EXPECT_NEAR(oh[s].z, 0.5f * oa[s].z, 2e-3f);
}

TEST(Clump, FlatClumpsLieAlongTheScalpOrTurnAtRandom) {
    // Strands combed along the plane (+x): flattened along the surface, the ribbons lie flat on it
    // (no thickness along its normal, +y); turned at random, clumps lie every which way.
    auto flatOn = [&](const std::string& way) {
        Locks l({{"flatness", 1.0}, {"flatAlong", way}}, true);
        const StrandsPtr out = l.sg.eval(l.clump);
        return l.offsets(*out, out->strandPointCount(0) - 1);
    };
    float lying = 0.0f;
    for (const Vec3f& o : flatOn("surface")) lying = std::max(lying, std::fabs(o.y));
    EXPECT_LT(lying, 1e-4f);
    float standing = 0.0f;
    for (const Vec3f& o : flatOn("random")) standing = std::max(standing, std::fabs(o.y));
    EXPECT_GT(standing, 0.02f);
}


// A param's map: an expression, read 0-1.
MapRef expressionMap(const std::string& text) {
    MapRef map;
    map.source = MapRef::Source::Expression;
    map.name = text;
    return map;
}

// Segment i's length of strand s.
float segment(const StrandSet& set, std::size_t s, std::size_t i) {
    return (set.strandPoints(s)[i] - set.strandPoints(s)[i - 1]).length();
}

TEST(Gravity, StrandsHangFromTheRootKeepingTheirLengths) {
    // Strands combed out flat along +x over a plane: under gravity (-y) they hang, tips lowest; the
    // root segment keeps its own direction (stiff at the root), every segment its length.
    ShapeGraph sg;
    const NodeId combed = sg.add(op::kBend, sg.scatter, {{"angle", 180.0}, {"end", 0.0}, {"along", std::string("direction")},
                                                          {"direction", Vec3f{1, 0, 0}}});
    const StrandsPtr before = sg.eval(combed);
    auto tipDrop = [&](const StrandSet& out) {
        double drop = 0.0;
        for (std::size_t s = 0; s < out.strandCount(); ++s) {
            const std::size_t n = out.strandPointCount(s);
            drop += before->strandPoints(s)[n - 1].y - out.strandPoints(s)[n - 1].y;
        }
        return drop / static_cast<double>(out.strandCount());
    };
    const NodeId gravity = sg.add(op::kGravity, combed);
    const StrandsPtr out = sg.eval(gravity);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        EXPECT_EQ(out->strandPoints(s)[0], before->strandPoints(s)[0]);
        EXPECT_EQ(out->strandPoints(s)[1], before->strandPoints(s)[1]);  // the root's segment: fully stiff there
        for (std::size_t i = 1; i < n; ++i) EXPECT_NEAR(segment(*out, s, i), segment(*before, s, i), 1e-5f);
        for (std::size_t i = 2; i < n; ++i) EXPECT_LE(out->strandPoints(s)[i].y, out->strandPoints(s)[i - 1].y + 1e-6f);
    }
    const double hanging = tipDrop(*out);
    EXPECT_GT(hanging, 0.3 * before->extent().longestStrand);
    // Stronger, lower; stiffer, higher; fully stiff, not at all.
    sg.g.node(gravity).params().set("strength", 3.0);
    EXPECT_GT(tipDrop(*sg.eval(gravity)), hanging);
    sg.g.node(gravity).params().set("strength", 1.0);
    sg.g.node(gravity).params().set("stiffnessProfile", Ramp{{0.0f, 1.0f}, {0.25f, 0.8f}, {1.0f, 0.8f}});
    EXPECT_LT(tipDrop(*sg.eval(gravity)), hanging);
    sg.g.node(gravity).params().set("stiffnessProfile", Ramp::constant(1.0f));
    EXPECT_EQ(sg.eval(gravity)->positions(), before->positions());
}

TEST(Gravity, TheShapeDoesNotDependOnThePointCount) {
    // The same strands at 16 and 64 points: their tips hang to within a few percent of the length.
    auto tips = [](std::int64_t segments) {
        ShapeGraph sg;
        sg.g.node(sg.segmentsNode).params().set("segments", segments);
        const NodeId combed = sg.add(op::kBend, sg.scatter, {{"angle", 180.0}, {"end", 0.0}, {"along", std::string("direction")},
                                                              {"direction", Vec3f{1, 0, 0}}});
        const StrandsPtr out = sg.eval(sg.add(op::kGravity, combed));
        std::vector<Vec3f> result;
        for (std::size_t s = 0; s < out->strandCount(); ++s) result.push_back(out->strandPoints(s)[out->strandPointCount(s) - 1]);
        return std::make_pair(result, out->extent().longestStrand);
    };
    const auto [coarse, length] = tips(15);
    const auto fine = tips(63).first;
    ASSERT_EQ(coarse.size(), fine.size());
    for (std::size_t s = 0; s < coarse.size(); ++s) EXPECT_LT((coarse[s] - fine[s]).length(), 0.05f * length) << "strand " << s;
}

TEST(Displace, MovesStrandsAlongTheNormalOrADirection) {
    ShapeGraph sg;
    const StrandsPtr before = sg.eval(sg.scatter);
    const NodeId lift = sg.add(op::kDisplace, sg.scatter, {{"distance", 0.1}, {"profile", Ramp::constant(1.0f)}});
    const NodeId side = sg.add(op::kDisplace, sg.scatter, {{"distance", 0.1}, {"profile", Ramp::constant(1.0f)},
                                                           {"along", std::string("direction")}, {"direction", Vec3f{2, 0, 0}}});
    const NodeId ramped = sg.add(op::kDisplace, sg.scatter,
                                 {{"distance", expressionMap("0.2 * t")}, {"profile", Ramp::constant(1.0f)}});
    const StrandsPtr up = sg.eval(lift), across = sg.eval(side), byT = sg.eval(ramped);
    for (std::size_t s = 0; s < before->strandCount(); ++s) {
        const std::size_t n = before->strandPointCount(s);
        EXPECT_EQ(up->strandPoints(s)[0], before->strandPoints(s)[0]);
        for (std::size_t i = 1; i < n; ++i) {
            EXPECT_NEAR((moved(*up, *before, s, i) - Vec3f(0, 0.1f, 0)).length(), 0.0f, 1e-6f);      // the plane's normal: +y
            EXPECT_NEAR((moved(*across, *before, s, i) - Vec3f(0.1f, 0, 0)).length(), 0.0f, 1e-6f);  // the direction, unit
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            EXPECT_NEAR(moved(*byT, *before, s, i).y, 0.2f * t, 1e-3f);  // per point: t by arc length
        }
    }
}


// A sphere's strands (symmetric about x = 0), mirrored by `params`.
struct Mirrored {
    Graph g;
    ExternalInputs ext;
    MeshPtr scalp = test::makeSphere(24, 48, 1.0f);
    NodeId scatter = 0, mirror = 0;
    explicit Mirrored(const std::map<std::string, ParamValue>& params) {
        ext.set("scalp", scalp, scalp->contentHash());
        const NodeId scalpNode = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", 40.0);
        g.connect(scalpNode, scatter, 0);
        mirror = g.addNode(op::kMirror);
        g.connect(scatter, mirror, 0);
        for (const auto& [k, v] : params) g.node(mirror).params().set(k, v);
    }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
};

TEST(Mirror, OneSideMirroredOntoTheOther) {
    // Replace: the strands rooted at x > 0 (and those on the plane) kept, the others replaced by
    // the mirrors of the kept, rooted on the sphere where they land (it is symmetric: right there).
    Mirrored m({});
    const StrandsPtr in = m.eval(m.scatter), out = m.eval(m.mirror);
    std::size_t positive = 0, onPlane = 0;
    auto planar = [](float x) { return std::fabs(x) <= 1e-5f * (1.0f + std::fabs(x)); };
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const float x = in->strandPoints(s)[0].x;
        positive += x > 0.0f && !planar(x);
        onPlane += planar(x);
    }
    ASSERT_EQ(out->strandCount(), 2 * positive + onPlane);
    std::set<StrandId> ids(out->ids().begin(), out->ids().end());
    EXPECT_EQ(ids.size(), out->strandCount());  // every ID its own
    const std::size_t kept = positive + onPlane;
    for (std::size_t s = 0; s < kept; ++s) EXPECT_GE(out->strandPoints(s)[0].x, -1e-5f);
    std::map<StrandId, std::size_t> byId;
    for (std::size_t s = 0; s < in->strandCount(); ++s) byId[in->ids()[s]] = s;
    std::size_t k = kept;
    for (std::size_t s = 0; s < kept; ++s) {
        const std::size_t source = byId.at(out->ids()[s]);
        if (planar(in->strandPoints(source)[0].x)) continue;
        // Its mirror: the points mirrored across x = 0, rooted where they start.
        ASSERT_LT(k, out->strandCount());
        for (std::size_t i = 0; i < out->strandPointCount(k); ++i) {
            const Vec3f p = in->strandPoints(source)[i];
            EXPECT_NEAR((out->strandPoints(k)[i] - Vec3f(-p.x, p.y, p.z)).length(), 0.0f, 1e-4f);
        }
        ASSERT_GE(out->roots()[k].face, 0);
        EXPECT_NEAR((m.scalp->position(out->roots()[k]) - out->strandPoints(k)[0]).length(), 0.0f, 1e-4f);
        ++k;
    }
    // The other side kept instead, and every strand's mirror added.
    Mirrored negative({{"side", std::string("negative")}});
    for (const Vec3f& p : {negative.eval(negative.mirror)->strandPoints(0)[0]}) EXPECT_LE(p.x, 0.0f);
    Mirrored add({{"mode", std::string("add")}});
    EXPECT_EQ(add.eval(add.mirror)->strandCount(), 2 * in->strandCount() - onPlane);
    // Across another plane (z = 0.3), the mirrors start where the sphere is: moved onto it.
    Mirrored off({{"axis", std::string("z")}, {"offset", 0.3}});
    const StrandsPtr moved = off.eval(off.mirror);
    for (std::size_t s = 0; s < moved->strandCount(); ++s) {
        ASSERT_GE(moved->roots()[s].face, 0);
        EXPECT_NEAR((off.scalp->position(moved->roots()[s]) - moved->strandPoints(s)[0]).length(), 0.0f, 1e-4f);
    }
}

TEST(Mirror, GuidesInGuidesOut) {
    // Fed guides, it gives guides (a Curves node): Guides -> Mirror -> Interpolate's guides.
    GroomData data;
    data.scalp = test::makeSphere(24, 48, 1.0f);
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.6f, 0.8f, 0.0f}, {0.6f, 0.8f, 0.0f}, 4));
    guides->add(2, test::straightCurve({0.0f, 0.8f, 0.6f}, {0.0f, 0.8f, 0.6f}, 4));
    guides->rootOnMesh(*data.scalp);
    data.guides = guides;
    data.graphJson = R"({"version": 1, "output": "out", "nodes": [
        {"name": "guides", "type": "Guides"}, {"name": "mirror", "type": "Mirror"},
        {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 20.0}},
        {"name": "interpolate", "type": "Interpolate"}, {"name": "out", "type": "Output"}],
      "connections": [{"from": "guides", "to": "mirror", "input": "strands"}, {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"}, {"from": "mirror", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"}, {"from": "interpolate", "to": "out", "input": "strands"}]})";
    GroomSession session(data);
    const ViewResult& view = session.evaluate(ViewRequest{});
    ASSERT_TRUE(view.errors.empty()) << view.errors.front();
    const GraphDocument& doc = *session.viewDocument();
    const GuidesPtr mirrored = valueAs<GuidesPtr>(doc.graph.cachedValue(doc.nodesByName.at("mirror")));
    ASSERT_TRUE(mirrored);
    ASSERT_EQ(mirrored->guides().size(), 3u);  // the one at x = 0.6, its mirror, the one on the plane
    const Guide& made = mirrored->guides().back();
    EXPECT_NEAR(made.points.back().x, -1.2f, 1e-3f);
    EXPECT_GE(made.root.face, 0);
}

TEST(Noise, KinkyIsStraightBetweenRandomKnots) {
    // Strands of 33 points, kinks 4 to the length: between knots (every 8 points) each point's
    // offset is the straight blend of the knots' (no bend), at the knots the strand kinks.
    ShapeGraph sg;
    sg.g.node(sg.segmentsNode).params().set("segments", std::int64_t{32});
    const StrandsPtr before = sg.eval(sg.scatter);
    const NodeId smooth = sg.add(op::kNoise, sg.scatter);
    const NodeId spelled = sg.add(op::kNoise, sg.scatter, {{"mode", std::string("smooth")}});
    EXPECT_EQ(sg.eval(smooth)->positions(), sg.eval(spelled)->positions());  // the default: as before
    const NodeId kinky = sg.add(op::kNoise, sg.scatter, {{"mode", std::string("kinky")}, {"frequency", 4.0}, {"amplitude", 0.1},
                                                         {"profile", Ramp::constant(1.0f)}, {"octaves", std::int64_t{1}}});
    const StrandsPtr out = sg.eval(kinky);
    std::size_t kinks = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        ASSERT_EQ(out->strandPointCount(s), 33u);
        for (std::size_t i = 2; i + 1 < 33; ++i) {
            const Vec3f bend = moved(*out, *before, s, i + 1) - moved(*out, *before, s, i) * 2.0f + moved(*out, *before, s, i - 1);
            if (i % 8 != 0) {
                EXPECT_LT(bend.length(), 1e-5f) << "strand " << s << " point " << i;  // straight between knots
            } else {
                kinks += bend.length() > 1e-3f;
            }
        }
    }
    EXPECT_GT(kinks, out->strandCount() * 2);  // sharp at the knots
    // World space: neighbours kink alike (their knots from the noise at their roots); strand
    // space: each its own.
    auto spread = [&](const std::string& space) {
        const NodeId n = sg.add(op::kNoise, sg.scatter, {{"mode", std::string("kinky")}, {"space", space}, {"frequency", 2.0},
                                                         {"amplitude", 0.1}, {"profile", Ramp::constant(1.0f)}});
        const StrandsPtr k = sg.eval(n);
        double sum = 0.0;
        for (std::size_t s = 1; s < k->strandCount(); ++s) {
            if ((before->strandPoints(s)[0] - before->strandPoints(s - 1)[0]).length() > 0.05f) continue;
            sum += (moved(*k, *before, s, 16) - moved(*k, *before, s - 1, 16)).length();
        }
        return sum;
    };
    EXPECT_LT(spread("world") * 3.0, spread("strand"));
}

TEST(Width, RandomnessVariesEachStrandsWidth) {
    // Each strand's widths times one factor of its own, within the randomness either way; none
    // without it (the widths as before), others with another seed.
    ShapeGraph sg;
    const NodeId plain = sg.add(op::kWidth, sg.scatter, {{"width", 0.01}});
    const NodeId none = sg.add(op::kWidth, sg.scatter, {{"width", 0.01}, {"randomness", 0.0}, {"seed", std::int64_t{4}}});
    EXPECT_EQ(sg.eval(plain)->widths(), sg.eval(none)->widths());
    const NodeId varied = sg.add(op::kWidth, sg.scatter, {{"width", 0.01}, {"randomness", 0.4}});
    const NodeId reseeded = sg.add(op::kWidth, sg.scatter, {{"width", 0.01}, {"randomness", 0.4}, {"seed", std::int64_t{9}}});
    const StrandsPtr base = sg.eval(plain), out = sg.eval(varied), other = sg.eval(reseeded);
    float lo = 2.0f, hi = 0.0f;
    std::size_t differ = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t begin = out->strandBegin(s), n = out->strandPointCount(s);
        const float factor = out->widths()[begin] / base->widths()[begin];
        for (std::size_t k = 0; k < n; ++k) EXPECT_NEAR(out->widths()[begin + k], base->widths()[begin + k] * factor, 1e-8f);
        lo = std::min(lo, factor);
        hi = std::max(hi, factor);
        differ += std::fabs(other->widths()[begin] / base->widths()[begin] - factor) > 1e-3f;
    }
    EXPECT_GE(lo, 0.6f - 1e-5f);
    EXPECT_LE(hi, 1.4f + 1e-5f);
    EXPECT_LT(lo, 0.7f);
    EXPECT_GT(hi, 1.3f);
    EXPECT_GT(differ, out->strandCount() / 2);
}

TEST(Performance, GravityAndDisplaceAt100kStrands) {
    // Not in the spec's budget; printed so the docs can quote it (as CombAndCollideAt100kStrands).
    auto scalp = test::makeSphere(60, 120, 1.0f);
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    ctx.strandSegments = 15;
    auto scatter = OperatorRegistry::instance().create(op::kScatter);
    scatter->params().set("density", 8000.0);
    const StrandsPtr strands = valueAs<StrandsPtr>(scatter->evaluate(ctx, {MeshPtr(scalp)}));
    ASSERT_GT(strands->strandCount(), 90000u);
    auto timed = [&](Operator& op) {
        const auto t0 = std::chrono::steady_clock::now();
        const StrandsPtr out = valueAs<StrandsPtr>(op.evaluate(ctx, {strands}));
        EXPECT_TRUE(out);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    auto gravity = OperatorRegistry::instance().create(op::kGravity);
    auto displace = OperatorRegistry::instance().create(op::kDisplace);
    const double gravityMs = timed(*gravity), displaceMs = timed(*displace);
    std::printf("[ perf ] %zu strands x 16: Gravity %.1f ms, Displace %.1f ms\n", strands->strandCount(), gravityMs, displaceMs);
    EXPECT_LT(gravityMs, 2000.0);
}

}  // namespace
}  // namespace groomlab
