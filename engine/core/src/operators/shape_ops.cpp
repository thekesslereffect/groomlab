// SPDX-License-Identifier: Apache-2.0
// Per-strand shaping operators: Length, Width, Resample, Output.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "groomlab/curve.h"
#include "groomlab/feathers.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

// How much of the curvature placement is curvature (the rest is arc length,
// so straight runs between bends still get points).
constexpr float kBendShare = 0.6f;

// Each strand's turning: the angle (radians) at every point between the
// segments on either side of it (0 at the ends).
void turning(const Vec3f* p, std::size_t n, std::vector<float>& angle) {
    angle.assign(n, 0.0f);
    for (std::size_t k = 1; k + 1 < n; ++k) {
        const Vec3f a = p[k] - p[k - 1], b = p[k + 1] - p[k];
        const float la = a.length(), lb = b.length();
        if (la > 0.0f && lb > 0.0f) angle[k] = pm::acos(std::clamp(a.dot(b) / (la * lb), -1.0f, 1.0f));
    }
}

// Each strand's segments for Resample's curvature mode: about one per `perSegment` radians it
// turns, `least` to `most`.
std::size_t curvaturePoints(const Vec3f* p, std::size_t n, float perSegment, int least, int most) {
    thread_local std::vector<float> angle;  // scratch, reused across strands
    turning(p, n, angle);
    float need = 0.0f;
    for (const float a : angle) need += a;
    const float wanted = std::ceil(need / perSegment);
    return static_cast<std::size_t>(std::clamp(static_cast<int>(std::min(wanted, 1e6f)), least, most)) + 1;
}

// Strand s of `in` resampled into `out`'s points for it (as many as `out` has), widths
// (`widths`: `in` has them) and point channels (`inPoint`, `outPoint`) following. Points are
// spaced evenly by arc length, or with `byCurvature` closer together where the strand bends (by a
// measure mixing arc length and turning angle, kBendShare of it turning).
void resampleStrand(const StrandSet& source, StrandSet& out, std::size_t s, bool byCurvature, bool widths,
                    const std::vector<const std::vector<float>*>& inPoint, const std::vector<std::vector<float>*>& outPoint,
                    std::vector<float>& outWidths) {
    const StrandSet* in = &source;
    {
        const std::size_t n = in->strandPointCount(s), m = out.strandPointCount(s);
        const Vec3f* src = in->strandPoints(s);
        Vec3f* dst = out.strandPoints(s);
        if (n < 2 || m < 2) {
            std::fill(dst, dst + m, n > 0 ? src[0] : Vec3f{});
            return;
        }
        // Evenly, a strand at its count already stays as it is, as every strand does when all are
        // (ResampleOp hands the input on): one strand's count elsewhere never re-spaces it.
        if (!byCurvature && n == m) {
            std::copy(src, src + n, dst);
            auto copy = [&](const float* values, float* to) { std::copy(values, values + n, to); };
            if (widths) copy(in->widths().data() + in->strandBegin(s), outWidths.data() + out.strandBegin(s));
            for (std::size_t c = 0; c < outPoint.size(); ++c) {
                copy(inPoint[c]->data() + in->strandBegin(s), outPoint[c]->data() + out.strandBegin(s));
            }
            return;
        }
        // The measure along the strand at each point, 0 at the root to 1 at the tip.
        thread_local std::vector<float> along, angle, bend;
        thread_local std::vector<std::size_t> seg;
        thread_local std::vector<float> at;
        along.resize(n);
        arcParams(src, n, along.data());
        if (byCurvature) {
            turning(src, n, angle);
            bend.assign(n, 0.0f);  // half of each point's turn on the segment either side
            for (std::size_t k = 1; k < n; ++k) bend[k] = bend[k - 1] + 0.5f * (angle[k - 1] + angle[k]);
            if (bend.back() > 1e-4f) {
                for (std::size_t k = 0; k < n; ++k) {
                    along[k] = (1.0f - kBendShare) * along[k] + kBendShare * bend[k] / bend.back();
                }
            }
        }
        // Where each new point falls: a segment of the old strand and how far along it.
        seg.resize(m);
        at.resize(m);
        std::size_t k = 0;
        for (std::size_t j = 0; j < m; ++j) {
            const float want = static_cast<float>(j) / static_cast<float>(m - 1);
            while (k + 2 < n && along[k + 1] < want) ++k;
            const float span = along[k + 1] - along[k];
            seg[j] = k;
            at[j] = span > 0.0f ? std::clamp((want - along[k]) / span, 0.0f, 1.0f) : 0.0f;
        }
        for (std::size_t j = 0; j < m; ++j) dst[j] = src[seg[j]] + (src[seg[j] + 1] - src[seg[j]]) * at[j];
        auto sample = [&](const float* values, float* to) {
            for (std::size_t j = 0; j < m; ++j) to[j] = values[seg[j]] * (1.0f - at[j]) + values[seg[j] + 1] * at[j];
        };
        if (widths) sample(in->widths().data() + in->strandBegin(s), outWidths.data() + out.strandBegin(s));
        for (std::size_t c = 0; c < outPoint.size(); ++c) {
            sample(inPoint[c]->data() + in->strandBegin(s), outPoint[c]->data() + out.strandBegin(s));
        }
    }
}

}  // namespace

// Every strand resampled to points[s] points, widths and per-point channels following; IDs, roots
// and per-strand channels stay (resampleStrand).
StrandsPtr resampleStrands(const StrandSet& source, const std::vector<std::size_t>& points, bool byCurvature) {
    const StrandSet* in = &source;
    const std::size_t count = in->strandCount();
    // The new layout, then every strand resampled into it in parallel.
    auto out = std::make_shared<StrandSet>();
    // The layout; IDs and roots copied below, every point written by resampleStrand in parallel
    // (so the positions come as they are; the widths not: a strand of one point keeps the default).
    out->resize(points, StrandSet::kLeavePositions);
    out->mutableIds() = in->ids();
    out->mutableRoots() = in->roots();
    const std::vector<std::string> pointChannels = in->pointChannelNames();
    std::vector<std::vector<float>*> outPoint;
    std::vector<const std::vector<float>*> inPoint;  // looked up once, not per strand
    // Each channel keeps its default (Clump's -1 "no clump", say), so a later Merge fills with it.
    for (const std::string& name : pointChannels) {
        outPoint.push_back(&out->addPointChannel(name, in->pointChannelDefault(name)));
        inPoint.push_back(in->pointChannel(name));
    }
    for (const std::string& name : in->strandChannelNames()) {
        out->addStrandChannel(name, in->strandChannelDefault(name)) = *in->strandChannel(name);
    }
    const bool widths = in->widths().size() == in->pointCount();
    std::vector<float>& outWidths = out->mutableWidths();
    parallelFor(0, count, [&](std::size_t s) { resampleStrand(*in, *out, s, byCurvature, widths, inPoint, outPoint, outWidths); });
    out->setInstances(in->instances());
    for (const auto& layer : in->cardLayers()) out->addCardLayer(layer);
    for (const auto& layer : in->featherLayers()) out->addFeatherLayer(layer);
    return StrandsPtr(out);
}

namespace {

// Trims strands: each keeps `keep` of its length from the root (mappable). A `fraction` of the
// strands (mappable; picked by ID, so raising it only adds strands) are trimmed at random, each
// losing a share of its kept length drawn between trimMin and trimMax (its own draw: the range
// scales it smoothly): keep x (1 - trim). Lengthening is Transform's scale.
class LengthOp : public Operator {
public:
    std::string typeName() const override { return op::kLength; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"keep", 1.0, "Share of each strand's length kept from the root, 0-1 (mappable; to lengthen, "
                                   "Transform's scale)", "fraction"}
                .range(0, 1),
            ParamSpec{"fraction", 1.0, "Share of the strands trimmed at random, 0-1 (mappable)", "fraction"}.soft(0, 1),
            ParamSpec{"trimMin", 0.0, "The least share of its kept length a strand trimmed at random loses, 0-1", "fraction"}
                .range(0, 1),
            ParamSpec{"trimMax", 0.0, "The most share of its kept length a strand trimmed at random loses, 0-1 (0: no "
                                      "random trim)", "fraction"}
                .range(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
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
        const StrandsPtr in = requireStrands(inputs, op::kLength);
        const ScalarField lengthField(params(), "keep", scalpOf(ctx));
        lengthField.validate(*in);
        const ScalarField fractionField(params(), "fraction", scalpOf(ctx));
        fractionField.validate(*in);
        // The random trim's range (either way round).
        const auto trimA = static_cast<float>(params().getDouble("trimMin")), trimB = static_cast<float>(params().getDouble("trimMax"));
        const float trimLow = std::min(trimA, trimB), trimHigh = std::max(trimA, trimB);
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !lengthField.strandLocal() || !fractionField.strandLocal()) return false;
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
        auto shape = [&](std::size_t s) {
            float keep = lengthField.at(*out, s);
            if (trimHigh > 0.0f) {
                // Picked when its draw is under the fraction; its trim from a draw of its own.
                const float fraction = fractionField.at(*out, s);
                const StrandId id = out->ids()[s];
                if (fraction >= 1.0f || unitFloat(hashCombine(id, seed ^ 0x1E46)) < fraction) {
                    const float trim = trimLow + (trimHigh - trimLow) * unitFloat(hashCombine(id, seed ^ 0x7A11));
                    keep *= 1.0f - trim;
                }
            }
            keep = std::max(0.0f, keep);
            const std::size_t n = out->strandPointCount(s);
            Vec3f* pts = out->strandPoints(s);
            if (n < 2) return;
            thread_local std::vector<Vec3f> original;  // scratch, reused across strands
            original.assign(pts, pts + n);
            resampleByArcLengthInto(original.data(), n, pts, n, std::min(keep, 1.0f));
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                shape(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), shape);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Sets widths (diameters, scene units): `width` times `profile` along the
// length (t by arc length, root 0 to tip 1). `width` is mappable per strand
// or per point (an expression of t, a point channel), so a painted per-point
// layer or an expression can shape the width along each strand directly. With
// `randomness` each strand's width varies by up to that share either way (a
// draw from its ID and `seed`), as real hair's does.
class WidthOp : public Operator {
public:
    std::string typeName() const override { return op::kWidth; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"width", 0.007, "Width, scene units (mappable, per point: e.g. an expression of t or a point channel)",
                      "width"}
                .soft(0, 0.05),
            {"profile", Ramp::linear(1.0f, 0.3f), "Width along the length, root to tip, times width"},
            ParamSpec{"randomness", 0.0, "Per-strand variation of the width, 0-1: up to that share thinner or thicker",
                      "fraction"}
                .range(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
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
        const StrandsPtr in = requireStrands(inputs, op::kWidth);
        const ScalarField widthField(params(), "width", scalpOf(ctx));
        widthField.validate(*in);
        const bool perPoint = widthField.perPoint();
        const Ramp profile = params().getRamp("profile");
        const float randomness = static_cast<float>(params().getDouble("randomness"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !widthField.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        // Feathers' shafts with a rachis (Propagate's shaftWidth: kRachisChannel) are as wide as
        // it: they keep their widths (a barb width set after the feathers is the barbs').
        const std::vector<float>* isShaft = in->strandChannel(kRachisChannel);
        if (!last && !isShaft && gpu::cpuEngineOn()) {
            // look.slang's WidthCS, the GPU preview's own (look_cpu.h).
            gpu::LookRun look;
            gpu::StageParams& s = look.params.stages.emplace_back();
            s.stage.kind = gpu::StageKind::Width;
            s.value[0] = widthField.isConstant() ? widthField.maximum() : 1.0f;
            s.value[1] = randomness;
            s.seed = seed;
            s.ramp = profile;
            look.fields.stages.resize(1);
            look.inputs.resize(1);
            if (!widthField.isConstant()) gpu::readFieldValues(widthField, *in, true, &look.fields.stages[0].field[0]);
            gpu::runOnCpuEngine(look, in, scalpOf(ctx), *out, true);
            *result = StrandsPtr(out);
            return true;
        }
        // Every strand's widths are written (the strands left alone keep the last result's).
        std::vector<float>& widths = last ? widthsOfLast(*out, last, *incremental) : out->mutableWidths();
        const bool inWidths = in->widths().size() == in->pointCount();
        auto shape = [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s);
            if (n == 0) return;
            if (isShaft && (*isShaft)[s] >= 0.5f) {
                const std::size_t begin = in->strandBegin(s);
                for (std::size_t k = 0; k < n; ++k) widths[begin + k] = inWidths ? in->widths()[begin + k] : 1.0f;
                return;
            }
            // The strand's variation (none without randomness: the width as it is).
            float vary = 1.0f;
            if (randomness > 0.0f) {
                Rng rng(hashCombine(out->ids()[s], seed ^ 0x3D17ull));
                vary = 1.0f + randomness * rng.uniform(-1.0f, 1.0f);
            }
            const float width = perPoint ? 0.0f : widthField.at(*out, s) * vary;
            float* w = widths.data() + in->strandBegin(s);
            // t by arc length: the profile follows the hair, not the point spacing.
            thread_local std::vector<float> along;  // scratch, reused across strands
            along.resize(n);
            arcParams(in->strandPoints(s), n, along.data());
            for (std::size_t k = 0; k < n; ++k) {
                w[k] = (perPoint ? widthField.atPoint(*out, s, k) * vary : width) * profile(along[k]);
            }
        };
        if (last) {
            forEachListed(changes->strands, shape);
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), shape);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Sets the strands' resolution, best after Interpolate: strands (or guides)
// through it are resampled to `segments`, and every Scatter before it grows its
// strands with them in the first place (the most any Resample after the Scatter
// asks for on the way to the Output: each branch its own, EvalContext::strandSegments; one
// fed nothing, or reaching no Output, sets none), so nodes before it, like Interpolate, already
// work at that resolution. `mode` (even by default: what the GPU look preview hands
// on, look_chain.h) or fewer points where they aren't needed, for render and export:
//   even       every strand gets `segments`, evenly spaced
//   length     segments in proportion to each strand's length (the longest get
//              `segments`, at least minSegments): short hair stops carrying the
//              points long hair needs
//   curvature  as many segments as each strand needs to hold its shape: about
//              one per maxAngle it turns (straight ones minSegments, at most
//              `segments`), placed closer together where it bends and farther
//              apart on straight runs. `segments` stays the working resolution
//              (Scatter grows with it), so curls are shaped in full detail first
class ResampleOp : public Operator {
public:
    std::string typeName() const override { return op::kResample; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"segments", std::int64_t{16}, "Segments per strand (points = segments + 1); every Scatter before it "
                                                        "grows its strands with them (the most any Resample after it, on "
                                                        "the way to the Output, asks for). 0: keep each strand's"})
                .range(0, 1024)
                .soft(0, 64),
            {"mode", std::string("even"), "even: segments for every strand; curvature: fewer points for render and export, "
                                          "fewer for straighter strands, placed where they bend; length: fewer for "
                                          "shorter strands",
             "", {"even", "curvature", "length"}},
            notAnimatable({"minSegments", std::int64_t{3}, "With length or curvature: the fewest segments a strand gets"})
                .atLeast(1)
                .soft(1, 16)
                .when("mode", {"length", "curvature"}),
            notAnimatable({"maxAngle", 15.0, "With curvature: about how far (degrees) a strand may turn per segment; smaller keeps "
                                             "curls rounder with more points", "angle"})
                .atLeast(0.5)
                .soft(1, 45)
                .when("mode", {"curvature"}),
        };
    }

    // Even, or by curvature with every changed strand keeping its point count: only the changed
    // strands are resampled again, into a copy of the last result (the same layout). By length,
    // each strand's count depends on every strand's length: in full.
    bool evaluateIncremental(const EvalContext&, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        const StrandsPtr in = requireStrands(inputs, op::kResample);
        const auto segments = static_cast<int>(params().getInt("segments"));
        const std::string mode = params().getString("mode");
        const StrandChanges* changes = someStrandsChanged(incremental, 0);
        const StrandsPtr last = valueAs<StrandsPtr>(incremental.previous);
        if ((mode != "even" && mode != "curvature") || !changes || !last || last == in || in->empty() ||
            last->strandCount() != in->strandCount() || (segments == 0 && mode == "even")) {
            return false;
        }
        const std::size_t count = in->strandCount();
        if (mode == "even") {
            // Every strand at its count already: a full evaluation hands the input on as it is.
            bool same = true;
            for (std::size_t s = 0; s < count && same; ++s) same = in->strandPointCount(s) == static_cast<std::size_t>(segments) + 1;
            if (same) return false;
        } else {
            // Each changed strand's count by its turning, as a full evaluation finds it: the last
            // result's, or the layout changed.
            std::size_t mostPoints = 2;
            const PreviewCore core(*in);
            for (std::size_t s = 0; s < count; ++s) {
                if (core.core(s)) mostPoints = std::max(mostPoints, in->strandPointCount(s));
            }
            const int most = segments > 0 ? segments : static_cast<int>(std::min<std::size_t>(mostPoints - 1, 1024));
            const int least = std::clamp(static_cast<int>(params().getInt("minSegments")), 1, most);
            const float perSegment = static_cast<float>(params().getDouble("maxAngle")) * 0.0174532925f;
            std::atomic<bool> moved{false};
            forEachListed(changes->strands, [&](std::size_t s) {
                if (curvaturePoints(in->strandPoints(s), in->strandPointCount(s), perSegment, least, most) != last->strandPointCount(s)) {
                    moved = true;
                }
            });
            if (moved) return false;
        }
        if (changes->none()) {
            *out = last;
            *changed = *changes;
            return true;
        }
        // The last result's layout and arrays (IDs, roots, instances and layers are the input's,
        // unchanged), each array this writes copied, then the changed strands resampled again.
        auto made = std::make_shared<StrandSet>(*last);
        made->copyPositionsFrom(*last);
        const bool widths = in->widths().size() == in->pointCount();
        std::vector<float>& outWidths = made->copyWidthsFrom(*last);
        std::vector<std::vector<float>*> outPoint;
        std::vector<const std::vector<float>*> inPoint;
        for (const std::string& name : in->pointChannelNames()) {
            outPoint.push_back(made->copyPointChannelFrom(name, *last));
            inPoint.push_back(in->pointChannel(name));
            if (!outPoint.back()) return false;
        }
        std::vector<std::pair<const std::vector<float>*, std::vector<float>*>> strandValues;
        for (const std::string& name : in->strandChannelNames()) {
            std::vector<float>* to = made->copyStrandChannelFrom(name, *last);
            if (!to) return false;
            strandValues.emplace_back(in->strandChannel(name), to);
        }
        forEachListed(changes->strands, [&](std::size_t s) {
            for (const auto& [from, to] : strandValues) (*to)[s] = (*from)[s];
            resampleStrand(*in, *made, s, mode == "curvature", widths, inPoint, outPoint, outWidths);
        });
        *out = StrandsPtr(made);
        *changed = *changes;
        return true;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kResample);
        const auto segments = static_cast<int>(params().getInt("segments"));
        const std::string mode = params().getString("mode");
        if (mode != "even" && mode != "length" && mode != "curvature") {
            throw GraphError("Resample: mode must be even, length or curvature");
        }
        const bool adaptive = mode != "even";
        if ((segments == 0 && !adaptive) || in->empty()) return in;
        const std::size_t count = in->strandCount();
        std::vector<std::size_t> points(count, static_cast<std::size_t>(segments) + 1);
        if (adaptive) {
            // Each strand's need: its length, or its total turning angle.
            std::vector<float> need(count);
            std::size_t mostPoints = 2;
            parallelFor(0, count, [&](std::size_t s) {
                const Vec3f* p = in->strandPoints(s);
                const std::size_t n = in->strandPointCount(s);
                if (mode == "length") {
                    need[s] = arcLength(p, n);
                } else {
                    thread_local std::vector<float> angle;  // scratch, reused across strands
                    turning(p, n, angle);
                    float sum = 0.0f;
                    for (const float a : angle) sum += a;
                    need[s] = sum;
                }
            });
            // What sizes every strand's count, from the core strands (preview_core.h): the same at
            // every preview density.
            const PreviewCore core(*in);
            for (std::size_t s = 0; s < count; ++s) {
                if (core.core(s)) mostPoints = std::max(mostPoints, in->strandPointCount(s));
            }
            const int most = segments > 0 ? segments : static_cast<int>(std::min<std::size_t>(mostPoints - 1, 1024));
            const int least = std::clamp(static_cast<int>(params().getInt("minSegments")), 1, most);
            if (mode == "length") {
                std::vector<float> sorted;
                for (std::size_t s = 0; s < count; ++s) {
                    if (core.core(s)) sorted.push_back(need[s]);
                }
                if (sorted.empty()) sorted.push_back(1.0f);
                const std::size_t at = std::min(sorted.size() - 1, static_cast<std::size_t>(0.95f * static_cast<float>(sorted.size())));
                std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(at), sorted.end());
                // The long ones (not a stray), in 1% steps: a few strands' lengths changing keeps it.
                const float reference = roundToPercentStep(std::max(sorted[at], 1e-9f), ctx.scale);
                for (std::size_t s = 0; s < count; ++s) {
                    points[s] = static_cast<std::size_t>(std::clamp(
                                    static_cast<int>(std::lround(static_cast<float>(most) * need[s] / reference)), least, most)) + 1;
                }
            } else {
                const float perSegment =
                    static_cast<float>(params().getDouble("maxAngle")) * 0.0174532925f;
                for (std::size_t s = 0; s < count; ++s) {
                    const float wanted = std::ceil(need[s] / perSegment);
                    points[s] = static_cast<std::size_t>(std::clamp(static_cast<int>(std::min(wanted, 1e6f)), least, most)) + 1;
                }
            }
        }
        if (mode != "curvature") {  // curvature moves the points even at the same count
            bool same = true;
            for (std::size_t s = 0; s < count && same; ++s) same = in->strandPointCount(s) == points[s];
            if (same) return in;
        }
        return resampleStrands(*in, points, mode == "curvature");
    }
};

// Marks the graph result: what the viewport, renders and every export get.
class OutputOp : public Operator {
public:
    std::string typeName() const override { return op::kOutput; }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }

    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        return requireStrands(inputs, op::kOutput);
    }
};

}  // namespace

void registerLengthOperator(OperatorRegistry& registry) {
    registry.registerType(op::kLength,
                          {"shape", "Trims strands to a share of their length.", "trim cut shorten haircut"},
                          [] { return std::make_unique<LengthOp>(); });
}

void registerWidthOperator(OperatorRegistry& registry) {
    registry.registerType(op::kWidth,
                          {"shape", "Strand thickness, root to tip.", "thickness radius taper"},
                          [] { return std::make_unique<WidthOp>(); });
}

void registerResampleOperator(OperatorRegistry& registry) {
    registry.registerType(op::kResample,
                          {"shape", "Evens out the points along each strand.", "points segments cvs even"},
                          [] { return std::make_unique<ResampleOp>(); });
}

void registerOutputOperator(OperatorRegistry& registry) {
    registry.registerType(op::kOutput,
                          {"utility", "What the groom shows and renders.", "result render final"},
                          [] { return std::make_unique<OutputOp>(); });
}

}  // namespace groomlab
