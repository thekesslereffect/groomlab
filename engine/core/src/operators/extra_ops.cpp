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

constexpr std::size_t kAxisSamples = 32;
constexpr float kTwoPi = 6.28318530718f;

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
        const Ramp profile = params().getRamp("profile");
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

        if (!last && gpu::cpuEngineOn()) {
            // look.slang's TwistCS (around clumps: TwistAxesCS making their middles first), the GPU
            // preview's own (look_cpu.h).
            std::string whyNot;
            gpu::StageInputs axesOf;
            axesOf.twist = gpu::twistInputs(*in, axesIn.get(), params().getString("clumpChannel"), false, &whyNot);
            if (axesOf.twist && (!aroundAxes || axesOf.twist->count > 0)) {
                if (StrandsPtr made = gpu::runStage(gpu::StageKind::Twist, params(), in, scalpOf(ctx), std::move(axesOf))) {
                    *result = made;
                    return true;
                }
            }
        }
        // Axis curves, kAxisSamples points each, and which one each strand turns around (made
        // again: only the changed strands' axes, each found as in full).
        std::vector<Vec3f> axes;
        std::vector<std::size_t> axisOf(in->strandCount(), 0);
        if (aroundAxes) {
            std::vector<Vec3f> roots;
            for (const Guide& g : axesIn->guides()) {
                if (g.points.size() < 2) continue;
                const auto pts = resampleByArcLength(g.points.data(), g.points.size(), kAxisSamples);
                roots.push_back(pts.front());
                axes.insert(axes.end(), pts.begin(), pts.end());
            }
            if (roots.empty()) return handOn();
            const PointIndex index(roots);
            auto find = [&](std::size_t s) { axisOf[s] = index.nearest(in->strandPoints(s)[0]); };
            if (last) {
                forEachListed(changes->strands, find);
            } else {
                parallelFor(0, in->strandCount(), find);
            }
        } else {
            const std::string channel = params().getString("clumpChannel");
            const std::vector<float>* groups = in->strandChannel(channel);
            if (!groups) {
                throw GraphError("Twist: connect axes (guides), or put a Clump before it (no '" + channel +
                                 "' channel to find the clumps)");
            }
            // Each group's mean curve, sampled at the same share of every strand's length, over its
            // core strands (preview_core.h: the same at every preview density; a group without
            // one does not turn). The groups' members in strand order, then the groups in
            // parallel, each summing its members in that order (the same sums as one pass over the
            // strands).
            const PreviewCore core(*in);
            std::map<long long, std::size_t> slot;
            std::vector<std::vector<std::size_t>> members;
            for (std::size_t s = 0; s < in->strandCount(); ++s) {
                const auto key = std::llround((*groups)[s]);
                auto it = slot.find(key);
                if (it == slot.end()) {
                    it = slot.emplace(key, members.size()).first;
                    members.emplace_back();
                }
                axisOf[s] = it->second;
                if (core.core(s)) members[it->second].push_back(s);
            }
            for (std::size_t s = 0; s < in->strandCount(); ++s) {
                if (members[axisOf[s]].empty()) axisOf[s] = SIZE_MAX;
            }
            axes.assign(members.size() * kAxisSamples, Vec3f{});
            parallelFor(0, members.size(), [&](std::size_t a) {
                if (members[a].empty()) return;
                Vec3f* axis = axes.data() + a * kAxisSamples;
                for (const std::size_t s : members[a]) {
                    const Vec3f* pts = in->strandPoints(s);
                    const std::size_t n = in->strandPointCount(s);
                    for (std::size_t k = 0; k < kAxisSamples; ++k) {
                        axis[k] += pointAlong(pts, n, static_cast<float>(k) / (kAxisSamples - 1));
                    }
                }
                for (std::size_t k = 0; k < kAxisSamples; ++k) axis[k] = axis[k] / static_cast<float>(members[a].size());
            }, 4);
        }
        std::vector<float> axisLength(axes.size() / kAxisSamples);
        for (std::size_t a = 0; a < axisLength.size(); ++a) axisLength[a] = arcLength(axes.data() + a * kAxisSamples, kAxisSamples);

        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->mutablePositions();  // cloned once here: the loop writes points while fields read the set
        }
        const RampAlong along(profile, *out);
        auto twist = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 2 || axisOf[s] == SIZE_MAX) return;
            Vec3f* pts = out->strandPoints(s);
            const Vec3f* axis = axes.data() + axisOf[s] * kAxisSamples;
            const float rate = kTwoPi * turns.at(*out, s) * axisLength[axisOf[s]];
            if (rate == 0.0f) return;
            for (std::size_t i = 1; i < n; ++i) {
                const float t = paramAt(i, n);
                const Vec3f center = pointAlong(axis, kAxisSamples, t);
                const float dt = 1.0f / (kAxisSamples - 1);
                Vec3f tangent = pointAlong(axis, kAxisSamples, std::min(t + dt, 1.0f)) -
                                pointAlong(axis, kAxisSamples, std::max(t - dt, 0.0f));
                const float l = tangent.length();
                if (l < 1e-8f) continue;
                tangent = tangent / l;
                pts[i] = center + rotateAxisAngle(pts[i] - center, tangent, rate * t * along(i, n));
            }
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                twist(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), twist);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Smooth's grid: cells along a strand's length, four to the deviation (scale x its length), from 8
// to 128 of them; one count for every strand (it depends on the scale alone), so a strand smooths
// alike at any point count.
std::size_t smoothCells(float scale) {
    return static_cast<std::size_t>(std::clamp(std::ceil(4.0f / std::max(scale, 1e-3f)), 8.0f, 128.0f));
}

// A strand's shape with its bumps up to `scale` of its length taken out (into `smoothed`, n
// points from the root, each segment its old length). Its direction along its length, by arc
// length (so any resampling of the same curve smooths alike), is averaged by three box filters
// 2 x scale of its length wide, as a Gaussian of deviation scale x its length would, over the
// direction reflected at both ends (a period of twice the strand): at scale 1 the first box spans
// the whole period, so every segment points along the strand's mean direction, the line from the
// root to the tip. Directions are summed in `cells` cells of the length (their increments, the
// polyline sampled at even arc lengths), so the filters run the same on any resampling.
//
// The filters' window ends fall at the same places in every strand's cells (they depend on the
// scale and the cell count alone): SmoothFilter finds them once for a whole evaluation.
struct SmoothFilter {
    using End = gpu::SmoothEnd;
    std::size_t cells = 0;
    double width = 0.0;  // the boxes' width, in cells
    std::vector<End> ends;  // each cell's window's upper end, then each one's lower (look_stage_params.h smoothEnds)
    const End* upper = nullptr;
    const End* lower = nullptr;

    SmoothFilter(float scale, std::size_t cellCount)
        : cells(cellCount), width(gpu::smoothWidth(scale, cellCount)), ends(gpu::smoothEnds(scale, cellCount)) {
        upper = ends.data();
        lower = ends.data() + 2 * cells;
    }
};

void smoothShape(const Vec3f* pts, std::size_t n, const SmoothFilter& filter, std::vector<Vec3f>& smoothed) {
    struct D3 {
        double x = 0.0, y = 0.0, z = 0.0;
        D3 operator+(const D3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        D3 operator-(const D3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        D3 operator*(double k) const { return {x * k, y * k, z * k}; }
    };
    // Scratch, reused across strands, read and written through plain pointers (the compiler
    // reloads a vector's storage after every store it cannot tell apart from it).
    thread_local std::vector<double> arcs;
    thread_local std::vector<D3> periods, boxes, sumsOf;
    smoothed.assign(pts, pts + n);
    arcs.resize(n);
    double* const arc = arcs.data();
    arc[0] = 0.0;
    for (std::size_t i = 1; i < n; ++i) arc[i] = arc[i - 1] + static_cast<double>((pts[i] - pts[i - 1]).length());
    const double length = arc[n - 1];
    if (!(length > 0.0)) return;
    const std::size_t m = filter.cells, wrap = 2 * m;
    const double cell = length / static_cast<double>(m);
    periods.resize(wrap);
    boxes.resize(wrap);
    sumsOf.resize(wrap + 1);
    D3* period = periods.data();
    D3* boxed = boxes.data();
    D3* const sums = sumsOf.data();
    // The polyline at arc length a (a in [0, length]); `seg` walks forward with a.
    std::size_t seg = 1;
    auto pointAt = [&](double a) {
        while (seg + 1 < n && arc[seg] < a) ++seg;
        const double span = arc[seg] - arc[seg - 1];
        const double t = span > 0.0 ? std::clamp((a - arc[seg - 1]) / span, 0.0, 1.0) : 1.0;
        const Vec3f& p = pts[seg - 1];
        const Vec3f& q = pts[seg];
        return D3{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
    };
    // Each cell's increment, then the same reflected: a period of 2m cells.
    D3 previous = pointAt(0.0);
    for (std::size_t k = 0; k < m; ++k) {
        const D3 next = k + 1 == m ? D3{pts[n - 1].x, pts[n - 1].y, pts[n - 1].z} : pointAt(cell * static_cast<double>(k + 1));
        const D3 step = next - previous;
        period[k] = step;
        period[wrap - 1 - k] = step;
        previous = next;
    }
    // Three box filters `width` cells wide over the period, each from its running sums: the
    // integral up to a window's end (in cells, any real number) of the cells as steps.
    const double width = filter.width;
    const SmoothFilter::End* const upperEnds = filter.upper;
    const SmoothFilter::End* const lowerEnds = filter.lower;
    if (width > 0.0) {
        for (int pass = 0; pass < 3; ++pass) {
            sums[0] = D3{};
            D3 running;  // (in a register: each sum waits on the one before)
            for (std::size_t k = 0; k < wrap; ++k) sums[k + 1] = running = running + period[k];
            const D3 total = running;  // sums[wrap]
#if GROOMLAB_SMOOTH_SSE2
            // x and y side by side in SSE2 lanes, z alone: each value by the same operations in the
            // same order as the scalar loop's, so the same bits.
            const __m128d totalXY = _mm_loadu_pd(&total.x), inverseXY = _mm_set1_pd(1.0 / width);
            for (std::size_t k = 0; k < wrap; ++k) {
                const SmoothFilter::End& hi = upperEnds[k];
                const SmoothFilter::End& lo = lowerEnds[k];
                const D3 &sumHi = sums[hi.cell], &stepHi = period[hi.cell], &sumLo = sums[lo.cell], &stepLo = period[lo.cell];
                const __m128d upper = _mm_add_pd(_mm_add_pd(_mm_mul_pd(totalXY, _mm_set1_pd(hi.turns)), _mm_loadu_pd(&sumHi.x)),
                                                 _mm_mul_pd(_mm_loadu_pd(&stepHi.x), _mm_set1_pd(hi.into)));
                const __m128d lower = _mm_add_pd(_mm_add_pd(_mm_mul_pd(totalXY, _mm_set1_pd(lo.turns)), _mm_loadu_pd(&sumLo.x)),
                                                 _mm_mul_pd(_mm_loadu_pd(&stepLo.x), _mm_set1_pd(lo.into)));
                const double z = ((total.z * hi.turns + sumHi.z + stepHi.z * hi.into) - (total.z * lo.turns + sumLo.z + stepLo.z * lo.into)) *
                                 (1.0 / width);
                _mm_storeu_pd(&boxed[k].x, _mm_mul_pd(_mm_sub_pd(upper, lower), inverseXY));
                boxed[k].z = z;
            }
#else
            auto integral = [&](const SmoothFilter::End& e) { return total * e.turns + sums[e.cell] + period[e.cell] * e.into; };
            for (std::size_t k = 0; k < wrap; ++k) boxed[k] = (integral(upperEnds[k]) - integral(lowerEnds[k])) * (1.0 / width);
#endif
            std::swap(period, boxed);
        }
    }
    // Each segment along the smoothed increments over its arc, its own length kept.
    sums[0] = D3{};
    D3 running;
    for (std::size_t k = 0; k < m; ++k) sums[k + 1] = running = running + period[k];
    auto upTo = [&](double a) {  // the smoothed increments summed up to arc length a
        const double x = std::clamp(a / cell, 0.0, static_cast<double>(m));
        const std::size_t j = std::min(static_cast<std::size_t>(x), m - 1);
        return sums[j] + period[j] * (x - static_cast<double>(j));
    };
    Vec3f* const out = smoothed.data();
    D3 behind = upTo(0.0);
    for (std::size_t i = 1; i < n; ++i) {
        const D3 ahead = upTo(arc[i]);
        const D3 d = ahead - behind;
        behind = ahead;
        const auto segment = static_cast<float>(arc[i] - arc[i - 1]);
        const Vec3f way{static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)};
        const Vec3f old = pts[i] - pts[i - 1];
        // A segment whose smoothed directions cancel (a loop straightened) keeps its own.
        const Vec3f unit = way.length() > 1e-6f * segment ? way.normalized() : segment > 0.0f ? old / segment : Vec3f{};
        out[i] = out[i - 1] + unit * segment;
    }
}

// Smooth: each strand toward its shape with the bumps up to `scale` of its length taken out
// (smoothShape): small scales take out kinks from noise, interpolation or sculpting, 1 straightens
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
        const Ramp profile = params().getRamp("profile");
        if (!(scale > 0.0f) || in->empty()) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            *result = in;
            return true;
        }
        if (!incremental && gpu::cpuEngineOn()) {
            // look.slang's SmoothCS, the GPU preview's own (look_cpu.h).
            if (StrandsPtr made = gpu::runStage(gpu::StageKind::Smooth, params(), in, scalpOf(ctx))) {
                *result = made;
                return true;
            }
        }
        const SmoothFilter filter(scale, smoothCells(scale));

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !strength.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->mutablePositions();  // cloned once here: the loop writes points while fields read the set
        }
        const RampAlong along(profile, *out);
        auto smooth = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 3) return;
            Vec3f* pts = out->strandPoints(s);
            const float k = std::clamp(strength.at(*out, s), 0.0f, 1.0f);
            if (k <= 0.0f) return;
            thread_local std::vector<float> lengths;
            thread_local std::vector<Vec3f> target;
            lengths.resize(n);
            for (std::size_t i = 1; i < n; ++i) lengths[i] = (pts[i] - pts[i - 1]).length();
            smoothShape(pts, n, filter, target);
            for (std::size_t i = 1; i < n; ++i) {
                const float w = k * std::clamp(along(i, n), 0.0f, 1.0f);
                pts[i] = pts[i] + (target[i] - pts[i]) * w;
            }
            // Lengths back, from the root out, along the blended directions.
            Vec3f previous = pts[0];
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f d = pts[i] - previous;
                const float l = d.length();
                const Vec3f moved = l > 1e-6f * lengths[i] ? previous + d * (lengths[i] / l) : previous + (target[i] - target[i - 1]);
                previous = moved;
                pts[i] = moved;
            }
        };
        if (last) {
            if (!(gpu::cpuEngineOn() && gpu::runStageOnStrands(gpu::StageKind::Smooth, params(), in, changes->strands, scalpOf(ctx), *out))) {
                forEachListed(changes->strands, [&](std::size_t s) {
                    copyStrandPoints(*out, *in, s);
                    smooth(s);
                });
            }
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), smooth);
        }
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
