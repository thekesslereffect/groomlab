// SPDX-License-Identifier: Apache-2.0
// Curl: wraps strands into curls around their current shape.
//
// The curl itself is look.slang's (CurlCS, CurlAxesCS: the curve, frames, spacing and weighted
// curls described there), run on the CPU engine (look_cpu.h). Here: where the points coming in are
// too few a turn for the tolerance (a coil over a strand of 40 segments), Curl first cuts each of the
// strand's segments into as many as its longest needs at the most the curl turns there (its
// frequency, randomness, frequencyProfile and spacingRandom at their most; a zigzag enough to keep
// its kinks' peaks), up to maxSegments, on the strand taken as a smooth curve (each point keeping
// its share of the strand: each segment a cubic Hermite curve between its points, the slope at a
// point the derivative there of the parabola through it and its neighbours, spaced by their
// distances), then the stage curls those points as its own.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265359f;

// ---- A polyline as a smooth curve (look.slang secantOf, slopeFrom, hermiteAt), the points Curl
// adds placed on it

// The unit direction from a to b, its length into h (zero: none).
Vec3f secantOf(const Vec3f& a, const Vec3f& b, float* h) {
    const Vec3f d = b - a;
    *h = d.length();
    return *h > 0.0f ? d * (1.0f / *h) : Vec3f{};
}

// The slope (d point / d arc) of the parabola through three points spaced h0 and h1 apart, the
// unit directions between them s0 and s1: at the first (`which` 0, a curve's root), the middle
// (1) or the last (2, its tip).
Vec3f slopeFrom(const Vec3f& s0, float h0, const Vec3f& s1, float h1, int which) {
    const float w = h0 + h1;
    if (which == 1) return w > 0.0f ? (s0 * h1 + s1 * h0) * (1.0f / w) : Vec3f{};
    if (which == 0) return w > 0.0f ? s0 + (s0 - s1) * (h0 / w) : s0;
    return w > 0.0f ? s1 + (s1 - s0) * (h1 / w) : s1;
}
// Which three points (from `first`) and which of them the slope at point i of n is taken at.
void slopeSpan(std::size_t i, std::size_t n, std::size_t* first, int* which) {
    *first = i == 0 ? 0 : i + 1 == n ? n - 3 : i - 1;
    *which = i == 0 ? 0 : i + 1 == n ? 2 : 1;
}

// The segment from p0 (slope d0) to p1 (slope d1), h long, at u (0 to 1): its point, and its
// derivative in u.
Vec3f hermiteAt(const Vec3f& p0, const Vec3f& p1, const Vec3f& d0, const Vec3f& d1, float h, float u) {
    const Vec3f dd = p1 - p0, a = d0 * h, b = d1 * h;
    const float u2 = u * u;
    return p0 + dd * (u2 * (3.0f - 2.0f * u)) + a * (u * (1.0f - u) * (1.0f - u)) + b * (u2 * (u - 1.0f));
}
struct Curve {
    const Vec3f* p = nullptr;
    std::size_t n = 0;
    std::vector<Vec3f> s, d;
    std::vector<float> h;
    void make(const Vec3f* points, std::size_t count) {
        size(points, count);
        for (std::size_t j = 0; j + 1 < n; ++j) s[j] = secantOf(p[j], p[j + 1], &h[j]);
        for (std::size_t i = 0; i < n; ++i) {
            if (n == 2) {
                d[i] = s[0];
            } else if (n > 2) {
                std::size_t a = 0;
                int which = 0;
                slopeSpan(i, n, &a, &which);
                d[i] = slopeFrom(s[a], h[a], s[a + 1], h[a + 1], which);
            }
        }
    }
    // Its points, and room for the rest (make's, or makeFour's).
    void size(const Vec3f* points, std::size_t count) {
        p = points;
        n = count;
        s.resize(n > 1 ? n - 1 : 0);
        h.resize(s.size());
        d.resize(n);
    }
    // Segment j at u (0 < u < 1).
    Vec3f at(std::size_t j, float u) const { return hermiteAt(p[j], p[j + 1], d[j], d[j + 1], h[j], u); }
    // The point at share t of the points (by index: as each node places a strand's points at
    // every resolution: look.slang curvePointAt).
    Vec3f pointAt(float t) const {
        if (n == 0) return {};
        if (n == 1) return p[0];
        const float x = t * static_cast<float>(n - 1);
        const std::size_t i = x <= 0.0f ? 0 : std::min(static_cast<std::size_t>(x), n - 2);
        const float u = x - static_cast<float>(i);
        return u == 0.0f ? p[i] : at(i, u);
    }
};

}  // namespace

// Wraps strands into curls around their current shape: a helix (the default), a planar wave or
// zigzag (kinky hair), or a coil, a tight helix a few strand widths across (coily hair). Each
// strand curls on its own, from a phase of its own; with `coherence` clump the strands of a clump
// (Clump's clumpId) curl as one around the clump's middle, with one phase, frequency and plane
// (ringlets).
class CurlOp : public Operator {
public:
    // The scalp's normal at each root seeds the frames the curls turn in.
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::string typeName() const override { return op::kCurl; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"radius", 0.5, "Curl radius, scene units (mappable, per point); a coil's is coilWidths", "length"}
                .soft(0, 2)
                .when("shape", {"helix", "wave", "zigzag"}),
            ParamSpec{"frequency", 0.4, "Turns per unit of length", "perLength"}.soft(0, 2),
            {"frequencyProfile", Ramp::constant(1.0f),
             "Frequency along the length, root to tip, a share of frequency (the tightest): curls loosening or tightening "
             "toward the tip"},
            ParamSpec{"randomness", 0.3, "Per-strand variation of radius and frequency, 0-1", "fraction"}.range(0, 1),
            ParamSpec{"spacingRandom", 0.0,
                      "Smooth random variation of the spacing along each strand (each clump's with coherence clump), 0-1: "
                      "the frequency varies by up to this share every turn or two",
                      "fraction"}
                .range(0, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {0.25f, 1.0f}, {1.0f, 1.0f}},
             "Radius along the length, root to tip (eased in from 0 at the root over the first quarter turn)"},
            {"shape", std::string("helix"),
             "helix: around the strand; wave: a planar sine; zigzag: sharp planar kinks (kinky hair); coil: a tight helix "
             "coilWidths strand widths across (coily hair)", "", {"helix", "wave", "zigzag", "coil"}},
            ParamSpec{"coilWidths", 3.0, "Coil: its radius in strand widths at each point (the widths the strands carry: put "
                                         "a Width node before it)"}
                .soft(0, 10)
                .when("shape", {"coil"}),
            ParamSpec{"phase", 0.0, "Where along a turn every curl starts, in turns (0-1)", "turns"}.soft(0, 1),
            ParamSpec{"phaseRandom", 1.0, "How much of a turn each strand's (or clump's) start varies at random, 0-1",
                      "fraction"}
                .range(0, 1),
            {"coherence", std::string("strand"),
             "strand: each strand curls on its own; clump: the strands of a clump curl together around its middle, with "
             "one phase, frequency and plane (ringlets; after a Clump)", "", {"strand", "clump"}},
            ParamSpec{"clumpChannel", std::string("clumpId"),
                      "Coherence clump: the channel naming each strand's clump (Clump writes clumpId)"}
                .adv()
                .when("coherence", {"clump"})
                .readsChannel(),
            ParamSpec{"weight", 0.0,
                      "How much each part of a curl stretches under the hair hanging below it, 0 or more: its turns spread "
                      "apart where more turns hang below them along weightDirection (loose at the root, tight at the tip; "
                      "long ringlets more than short curls, hair lying across it or pointing up barely), each turn keeping "
                      "its length, so the radius narrows (an overstretched curl goes straight). 0: none; 1: 10 turns hanging "
                      "straight below a point double its spacing"}
                .range(0, 10)
                .soft(0, 2),
            ParamSpec{"weightDirection", Vec3f{0.0f, -1.0f, 0.0f},
                      "The way the weight pulls (world space): curls stretch where the strands hang along it"}
                .world(),
            notAnimatable(ParamSpec{"tolerance", 0.5,
                                    "How far a straight segment may stray from the round curl, a share of its radius: where the "
                                    "points coming in are too few a turn for it, Curl cuts each segment evenly along the smooth "
                                    "strand first (0.5: 3 a turn, so no curl aliases into another; 0.3: 4; 0.1: 7; 0.03: 13; a "
                                    "zigzag more than 2 / tolerance, keeping its kinks' peaks). 0: only the points coming in. "
                                    "Points added take the GPU preview off this node: more Resample segments keep it",
                                    "fraction"}
                              .range(0, 1)
                              .soft(0, 0.5))
                .adv(),
            notAnimatable(ParamSpec{"maxSegments", std::int64_t{256}, "The most segments a strand may have with the points Curl adds"}
                              .range(1, 4096)
                              .soft(16, 1024))
                .adv(),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
        };
    }
    // A coil on strands that carry no widths of their own (every width 1, as before any Width); and
    // strands Curl added points to.
    std::string note(const EvalContext&) const override {
        std::string text;
        if (unitWidths_.load(std::memory_order_relaxed)) {
            text = "coil reads the strands' widths, and these have none set (1 everywhere): put a Width node before it";
        }
        if (const std::size_t added = added_.load(std::memory_order_relaxed)) {
            if (!text.empty()) text += "; ";
            text += "added points to " + std::to_string(added) + (added == 1 ? " strand" : " strands") +
                    " too few a turn for the tolerance (more Resample segments keep the GPU preview)";
        }
        return text;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand curls alone: only the strands the input changed in curl again (while Curl adds
    // no points to them). Coherent by clump, a strand's curl follows its clump's middle, which any
    // member moves: in full.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done. The points it adds first (pointsFor,
    // curlDensified), then each strand curled at its own points (runOwn).
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const;
    bool runOwn(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
                StrandChanges* changed) const;
    // The points a strand of `n` points `p` is curled at: its own, or with the tolerance, each
    // segment cut into as many as its longest needs at the most the curl turns (at most maxSegments).
    std::size_t pointsFor(const Vec3f* p, std::size_t n) const;

    mutable std::atomic<bool> unitWidths_{false};
    mutable std::atomic<std::size_t> added_{0};  // strands the last evaluation added points to
};

// The strands `which` of `in` (all: null), each at `counts[k]` points: its segments each cut into
// as many (counts[k] - 1 over its own segments) on the strand taken as a smooth curve (Curve), its
// own points kept; widths and point channels taken linearly along each segment there.
StrandsPtr curlDensified(const StrandSet& in, const std::vector<std::size_t>& counts, const std::vector<std::uint32_t>* which) {
    const std::size_t made = counts.size();
    auto out = std::make_shared<StrandSet>();
    out->resize(counts, StrandSet::kLeavePositions);
    auto source = [&](std::size_t k) { return which ? static_cast<std::size_t>((*which)[k]) : k; };
    std::vector<StrandId>& ids = out->mutableIds();
    std::vector<StrandRoot>& roots = out->mutableRoots();
    for (std::size_t k = 0; k < made; ++k) {
        ids[k] = in.ids()[source(k)];
        roots[k] = in.roots()[source(k)];
    }
    std::vector<std::vector<float>*> outPoint;
    std::vector<const std::vector<float>*> inPoint;
    for (const std::string& name : in.pointChannelNames()) {
        outPoint.push_back(&out->addPointChannel(name, in.pointChannelDefault(name)));
        inPoint.push_back(in.pointChannel(name));
    }
    for (const std::string& name : in.strandChannelNames()) {
        std::vector<float>& to = out->addStrandChannel(name, in.strandChannelDefault(name));
        const std::vector<float>& from = *in.strandChannel(name);
        for (std::size_t k = 0; k < made; ++k) to[k] = from[source(k)];
    }
    const bool hasWidths = in.widths().size() == in.pointCount();
    std::vector<float>& outWidths = out->mutableWidths();
    parallelFor(0, made, [&](std::size_t k) {
        const std::size_t s = source(k), n = in.strandPointCount(s), m = counts[k];
        const std::size_t begin = in.strandBegin(s), to = out->strandBegin(k);
        const Vec3f* pts = in.strandPoints(s);
        Vec3f* dst = out->strandPoints(k);
        const std::size_t q = m == n || n < 2 ? 1 : (m - 1) / (n - 1);
        auto values = [&](const float* from, float* into) {
            if (q == 1) {
                std::copy_n(from, n, into);
                return;
            }
            for (std::size_t j = 0; j + 1 < n; ++j) {
                for (std::size_t r = 0; r < q; ++r) {
                    const float u = static_cast<float>(r) / static_cast<float>(q);
                    into[j * q + r] = r == 0 ? from[j] : from[j] + (from[j + 1] - from[j]) * u;
                }
            }
            into[m - 1] = from[n - 1];
        };
        if (q == 1) {
            std::copy_n(pts, n, dst);
        } else {
            thread_local Curve curve;
            curve.make(pts, n);
            for (std::size_t j = 0; j + 1 < n; ++j) {
                for (std::size_t r = 0; r < q; ++r) dst[j * q + r] = r == 0 ? pts[j] : curve.at(j, static_cast<float>(r) / static_cast<float>(q));
            }
            dst[m - 1] = pts[n - 1];
        }
        if (hasWidths) values(in.widths().data() + begin, outWidths.data() + to);
        for (std::size_t c = 0; c < outPoint.size(); ++c) values(inPoint[c]->data() + begin, outPoint[c]->data() + to);
    }, 16);
    out->setInstances(in.instances());
    for (const auto& layer : in.cardLayers()) out->addCardLayer(layer);
    for (const auto& layer : in.featherLayers()) out->addFeatherLayer(layer);
    return out;
}

std::size_t CurlOp::pointsFor(const Vec3f* p, std::size_t n) const {
    const float tolerance = std::clamp(static_cast<float>(params().getDouble("tolerance")), 0.0f, 1.0f);
    const auto most = static_cast<std::size_t>(std::max<std::int64_t>(1, params().getInt("maxSegments")));
    if (!(tolerance > 0.0f) || n < 2 || most <= n - 1) return n;
    // A chord strays r (1 - cos(pi / m)) from a turn of m points: within tolerance r at
    // m = pi / acos(1 - tolerance) a turn. A zigzag's kinks: the point nearest one at most half a
    // spacing off loses about 2 / m of its peak, so more than 2 / tolerance a turn.
    int perTurn = std::max(3, static_cast<int>(std::ceil(kPi / pm::acos(1.0f - tolerance) - 1e-4f)));
    if (params().getString("shape") == "zigzag") perTurn = std::max(perTurn, static_cast<int>(std::ceil(2.0f / tolerance)) + 1);
    // The most the curl turns per unit of length: its frequency at its randomness's most, times the
    // profile's most and the spacing noise's.
    float profileMost = 0.0f;
    for (const auto& key : params().getRamp("frequencyProfile").keys) profileMost = std::max(profileMost, key.second);
    const float randomness = static_cast<float>(params().getDouble("randomness"));
    const float spacing = std::clamp(static_cast<float>(params().getDouble("spacingRandom")), 0.0f, 1.0f);
    const float turns = std::fabs(static_cast<float>(params().getDouble("frequency"))) * (1.0f + 0.5f * std::fabs(randomness)) * profileMost *
                        (1.0f + spacing);
    float longest = 0.0f;
    for (std::size_t j = 1; j < n; ++j) longest = std::max(longest, (p[j] - p[j - 1]).length());
    const float wanted = std::ceil(longest * turns * static_cast<float>(perTurn) - 1e-3f);
    if (!(wanted > 1.0f)) return n;
    const std::size_t q = std::min(static_cast<std::size_t>(std::min(wanted, 1e6f)), most / (n - 1));
    return q <= 1 ? n : n + (n - 1) * (q - 1);
}

bool CurlOp::run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
                 StrandChanges* changed) const {
    const StrandsPtr in = requireStrands(inputs, 0, op::kCurl);
    const std::size_t strands = in->strandCount();
    auto withStrands = [&](StrandsPtr given) {
        std::vector<Value> again = inputs;
        again[0] = Value(std::move(given));
        return again;
    };
    if (incremental) {
        const StrandChanges* changes = someStrandsChanged(*incremental, 0);
        const StrandsPtr last = changes ? valueAs<StrandsPtr>(incremental->previous) : nullptr;
        if (!last || last->strandCount() != strands) return false;
        std::atomic<bool> other{false};
        std::vector<std::size_t> counts(changes->strands.size());
        parallelFor(0, counts.size(), [&](std::size_t k) {
            const std::size_t s = changes->strands[k];
            counts[k] = pointsFor(in->strandPoints(s), in->strandPointCount(s));
            if (counts[k] != last->strandPointCount(s)) other = true;  // (another layout: in full)
        }, 256);
        if (other) return false;
        if (last->pointCount() == in->pointCount()) return runOwn(ctx, inputs, incremental, result, changed);  // (none added)
        // With the points it added: the changed strands made apart (as in full), written over the
        // last result's, while each strand's curl reads it alone (alone, its radius the same
        // everywhere).
        if (params().getString("coherence") == "clump" || !ScalarField(params(), "radius", scalpOf(ctx)).isConstant()) return false;
        if (changes->none()) {
            *result = last;
            *changed = *changes;
            return true;
        }
        Value made;
        if (!runOwn(ctx, withStrands(curlDensified(*in, counts, &changes->strands)), nullptr, &made, nullptr)) return false;
        auto out = copyOfLast(last, *incremental);
        writeStrands(*out, *valueAs<StrandsPtr>(made), changes->strands);
        *result = StrandsPtr(out);
        *changed = *changes;
        return true;
    }
    std::vector<std::size_t> counts(strands);
    std::atomic<std::size_t> added{0};
    parallelFor(0, strands, [&](std::size_t s) {
        counts[s] = pointsFor(in->strandPoints(s), in->strandPointCount(s));
        if (counts[s] != in->strandPointCount(s)) added.fetch_add(1, std::memory_order_relaxed);
    }, 256);
    if (added == 0) {
        added_.store(0, std::memory_order_relaxed);
        return runOwn(ctx, inputs, nullptr, result, nullptr);
    }
    if (!runOwn(ctx, withStrands(curlDensified(*in, counts, nullptr)), nullptr, result, nullptr)) return false;
    added_.store(added, std::memory_order_relaxed);
    return true;
}

bool CurlOp::runOwn(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
                    StrandChanges* changed) const {
    const StrandsPtr in = requireStrands(inputs, 0, op::kCurl);
    const ScalpMesh* scalp = scalpOf(ctx);
    const ScalarField radiusField(params(), "radius", scalp);
    radiusField.validate(*in);
    const bool byClump = params().getString("coherence") == "clump";
    if (params().getString("shape") == "coil") {
        const std::vector<float>& w = in->widths();
        unitWidths_.store(!w.empty() && std::all_of(w.begin(), w.end(), [](float v) { return v == 1.0f; }), std::memory_order_relaxed);
    } else {
        unitWidths_.store(false, std::memory_order_relaxed);
    }

    const StrandChanges* changes = nullptr;
    StrandsPtr last;  // the last result, to make again only the strands the input changed in
    if (incremental) {
        changes = someStrandsChanged(*incremental, 0);
        // (Laid out as its input, or with the points Curl added to some strands.)
        last = changes ? valueAs<StrandsPtr>(incremental->previous) : nullptr;
        if (!last || last->strandCount() != in->strandCount() || last->pointCount() < in->pointCount() || byClump ||
            !radiusField.strandLocal()) {
            return false;
        }
        if (changes->none()) {  // the same strands in: the same result
            *result = last;
            *changed = *changes;
            return true;
        }
    }
    // look.slang's CurlCS, the GPU preview's own (look_cpu.h): in full (coherent by clump: the
    // clumps' middles made first, CurlAxesCS), or the strands the input changed in, into the last
    // result's copy (laid out as the input).
    if (!last) {
        gpu::StageInputs inputs;
        if (byClump) {
            const std::string channel = params().getString("clumpChannel");
            inputs.curl = gpu::curlInputs(*in, scalp, channel, static_cast<std::uint64_t>(params().getInt("seed")));
            if (!inputs.curl) throw GraphError("Curl: coherence clump needs a Clump before it (no '" + channel + "' channel to find the clumps)");
        }
        *result = gpu::runStage(gpu::StageKind::Curl, params(), in, scalp, std::move(inputs));
        return true;
    }
    auto out = std::make_shared<StrandSet>(*in);
    positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
    gpu::runStageOnStrands(gpu::StageKind::Curl, params(), in, changes->strands, scalp, *out);
    *result = StrandsPtr(out);
    *changed = *changes;
    return true;
}

void registerCurlOperator(OperatorRegistry& registry) {
    registry.registerType(op::kCurl,
                          {"look", "Helical curls around each strand.", "coil wave helix kinky afro"},
                          [] { return std::make_unique<CurlOp>(); });
}

}  // namespace groomlab
