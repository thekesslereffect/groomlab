// SPDX-License-Identifier: Apache-2.0
// Motion without a simulation cache: Wind (strands bent by a wind moving through space and
// time) and Jiggle (lag and overshoot from the scalp's recent motion). Each frame is worked out
// on its own, from the time and the scalp's last few frames, so a shot scrubs, renders on a farm
// and caches the same every time. Both keep segment lengths and roots, and both hand their input
// back untouched (the same strands, to the bit) when there is nothing to do: no wind, a scalp
// that did not move.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/noise.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr double kTwoPi = 6.283185307179586;

// ---------------------------------------------------------------------------
// Wind
// ---------------------------------------------------------------------------

// Bends strands along a wind blowing toward `direction`. Its strength varies over space and time
// through a noise field that travels with the wind (`frequency`: gust size, `speed`: how fast
// gusts pass); `gust` is how much it varies, and sways the direction a little. `turbulence` adds
// a fast flutter that grows toward the tips. Each segment turns toward the wind by strength x
// gust x `profile` along the length, keeping its length; roots never move. With `loop` the wind
// repeats every loopEnd - loopStart frames, seamlessly (windPhase: the gusts walk a closed path
// through the noise, the flutter turns whole cycles), for looping animations. A pure function of
// the strand, the time and the params: strength 0 gives the input back as it is.
class WindOp : public Operator {
public:
    std::string typeName() const override { return op::kWind; }
    std::string category() const override { return "motion"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"direction", Vec3f{1.0f, 0.0f, 0.0f}, "Where the wind blows to (world space)"}.world(),
            ParamSpec{"strength", 0.3, "How far strands bend with the wind, 0-1 (mappable); 0: no wind"}.soft(0, 1),
            ParamSpec{"gust", 0.5, "How much the wind varies over space and time, 0-1 (0: steady)"}.range(0, 1),
            ParamSpec{"frequency", 0.1, "Gust size: noise features per scene unit (smaller: broader gusts)", "perLength"}
                .atLeast(0)
                .soft(0, 1),
            ParamSpec{"speed", 20.0, "How fast the gusts travel with the wind, scene units per second", "length"}
                .soft(0, 100),
            ParamSpec{"turbulence", 0.2, "A fast small flutter growing toward the tips, 0-1"}.atLeast(0).soft(0, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {0.4f, 0.25f}, {1.0f, 1.0f}}, "How much each part bends, root to tip"},
            notAnimatable({"loop", false, "Repeat the wind every loopEnd - loopStart frames, seamlessly: a looping "
                                          "animation (the gusts repeat speed x the loop's length apart along the wind)"}),
            notAnimatable({"loopStart", std::int64_t{1}, "The frame the loop starts at"}).when("loop", {"true"}),
            notAnimatable({"loopEnd", std::int64_t{49}, "The frame the loop ends at: the wind there is the wind at loopStart "
                                                        "(after loopStart; else no loop)"})
                .when("loop", {"true"}),
            notAnimatable({"seed", std::int64_t{0}, "Another wind with the same settings"}).adv(),
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }  // maps on strength

    // The wind moves with time; with no strength at all (a plain 0) nothing depends on it, so
    // the node and everything after it stay cached from frame to frame.
    std::uint64_t contextSalt(const EvalContext& ctx) const override {
        if (params().has("strength") && !params().field("strength")) {
            const double* s = std::get_if<double>(&params().get("strength"));
            if (s && *s == 0.0) return 0;
        }
        const std::uint64_t h = hashDouble(hash64(0x3171D), ctx.time);
        return params().getBool("loop") ? hashDouble(h, ctx.framesPerSecond) : h;  // its frames counted at that rate
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        remember(out, ctx);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        if (!run(ctx, inputs, &incremental, out, changed)) return false;
        remember(*out, ctx);
        return true;
    }

    // operator_state.h windTime.
    bool timeOf(const StrandsPtr& output, double* time, double* framesPerSecond) const {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (!output || !sameOrShown(made_.lock(), output)) return false;
        *time = madeTime_;
        if (framesPerSecond) *framesPerSecond = madeFps_;
        return true;
    }
    // Masked: its time answers for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (made && made_.lock() == made) made_ = mixed;
    }

private:
    // The last result (by identity) and the time (and frame rate) it was made at, for a host
    // running the wind elsewhere (the GPU preview).
    void remember(const Value& out, const EvalContext& ctx) const {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        made_ = valueAs<StrandsPtr>(out);
        madeTime_ = ctx.time;
        madeFps_ = ctx.framesPerSecond;
    }
    mutable std::mutex madeMutex_;
    mutable std::weak_ptr<const StrandSet> made_;
    mutable double madeTime_ = 0.0, madeFps_ = 24.0;

    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done. A strand's wind is its own (its root, its
    // strength, the time and the params), so the strands left alone keep the last result's points
    // even when it blew nowhere then (the input handed back): those are their points now.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kWind);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const Vec3f direction = params().getVec3("direction");
        if (direction.length() < 1e-6f || in->strandCount() == 0) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            *result = in;
            return true;
        }
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
        // look.slang's WindCS, the GPU preview's own (look_cpu.h), at the evaluation's time: in full
        // (no strength anywhere: the input as it is), or the strands the input changed in.
        gpu::StageParamReader own(params(), "");
        own.time = ctx.time;
        own.framesPerSecond = ctx.framesPerSecond;
        if (!last) {
            *result = strength.isConstant() && strength.maximum() <= 0.0f ? in : gpu::runStage(gpu::StageKind::Wind, own, in, scalp);
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        gpu::runStageOnStrands(gpu::StageKind::Wind, own, in, changes->strands, scalp, *out);
        *changed = *changes;
        *result = StrandsPtr(out);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Jiggle
// ---------------------------------------------------------------------------

using Mat3 = std::array<double, 9>;  // row major

// A triangle's frame: edge, in-plane perpendicular and normal as the columns. False when the
// triangle is degenerate.
bool triangleFrame(const Vec3f& a, const Vec3f& b, const Vec3f& c, Mat3* frame) {
    const double e1[3] = {double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z};
    const double e2[3] = {double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z};
    double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const double ln = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const double le = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    if (ln < 1e-12 || le < 1e-12) return false;
    const double x[3] = {e1[0] / le, e1[1] / le, e1[2] / le};
    for (double& v : n) v /= ln;
    const double y[3] = {n[1] * x[2] - n[2] * x[1], n[2] * x[0] - n[0] * x[2], n[0] * x[1] - n[1] * x[0]};
    *frame = {x[0], y[0], n[0], x[1], y[1], n[1], x[2], y[2], n[2]};
    return true;
}

// The lag of a damped spring behind what it hangs from, as weights on where that point was:
// lag = sum over k of weight[k] * (x(t - k dt) - x(t)), k = 1..frames. The spring (natural
// frequency `hz`, damping ratio `zeta`) starts settled at the oldest sample, moving with it as it
// moved over the first step (so steady motion gives no lag), then follows the samples, which
// move in straight lines between frames. Linear in the samples, so the weights are worked out
// once per evaluation and every strand shares them. `hz` at least 1e-3 and `zeta` at least 0 (the
// params' limits).
std::vector<double> springLagWeights(int frames, double dt, double hz, double zeta) {
    const double omega = kTwoPi * hz;
    // The free spring's step over dt (lag, lag velocity), by RK4 in small steps: any damping.
    double m[2][2];
    for (int col = 0; col < 2; ++col) {
        double e = col == 0 ? 1.0 : 0.0, v = col == 1 ? 1.0 : 0.0;
        const int steps = 64;
        const double h = dt / steps;
        auto accel = [&](double pe, double pv) { return -omega * omega * pe - 2.0 * zeta * omega * pv; };
        for (int i = 0; i < steps; ++i) {
            const double k1e = v, k1v = accel(e, v);
            const double k2e = v + 0.5 * h * k1v, k2v = accel(e + 0.5 * h * k1e, v + 0.5 * h * k1v);
            const double k3e = v + 0.5 * h * k2v, k3v = accel(e + 0.5 * h * k2e, v + 0.5 * h * k2v);
            const double k4e = v + h * k3v, k4v = accel(e + h * k3e, v + h * k3v);
            e += h / 6.0 * (k1e + 2.0 * k2e + 2.0 * k3e + k4e);
            v += h / 6.0 * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
        }
        m[0][col] = e;
        m[1][col] = v;
    }
    std::vector<double> weights(static_cast<std::size_t>(frames) + 1, 0.0);
    std::vector<double> samples(static_cast<std::size_t>(frames) + 1);  // oldest first
    for (int b = 1; b <= frames; ++b) {
        // Where the scalp was, relative to now: 1 at b frames ago, 0 elsewhere.
        for (int j = 0; j <= frames; ++j) samples[static_cast<std::size_t>(j)] = (frames - j == b) ? 1.0 : 0.0;
        double e = 0.0, v = 0.0;
        double previous = (samples[1] - samples[0]) / dt;
        for (int j = 0; j < frames; ++j) {
            const double velocity = (samples[static_cast<std::size_t>(j) + 1] - samples[static_cast<std::size_t>(j)]) / dt;
            v -= velocity - previous;  // a kink in the path jolts the spring
            previous = velocity;
            const double ne = m[0][0] * e + m[0][1] * v, nv = m[1][0] * e + m[1][1] * v;
            e = ne;
            v = nv;
        }
        weights[static_cast<std::size_t>(b)] = e;
    }
    return weights;
}

// The frames a Jiggle with `params` reads (past[k]: where the scalp was k + 1 frames ago): up to
// `history`, as far back as they have the scalp's points (`now`'s count). Looping, a frame before the
// loop's start is read from the end of the range: from the frames before now, now itself, or the
// loop's own frames the host sampled (by their times).
std::vector<const std::vector<Vec3f>*> jigglePast(const ParamSet& params, const ScalpHistory& history, const std::vector<Vec3f>& now,
                                                  double nowFrame, double fps) {
    const auto wanted = static_cast<std::size_t>(params.getInt("history"));
    const std::size_t plain = history.plainCount();
    std::vector<const std::vector<Vec3f>*> past;
    const double loopStart = static_cast<double>(params.getInt("loopStart")), loopEnd = static_cast<double>(params.getInt("loopEnd"));
    const bool looping = params.getBool("loop") && loopEnd > loopStart;
    for (std::size_t k = 0; k < wanted; ++k) {
        const std::vector<Vec3f>* was = k < plain ? &history.frames[k] : nullptr;
        if (looping) {
            const double frame = jiggleLoopFrame(nowFrame - static_cast<double>(k + 1), nowFrame, loopStart, loopEnd);
            const double back = nowFrame - frame;
            const double whole = std::round(back);
            was = nullptr;
            if (std::abs(back - whole) < 1e-3 && whole >= 0.0 && whole <= static_cast<double>(plain)) {
                was = whole == 0.0 ? &now : &history.frames[static_cast<std::size_t>(whole) - 1];
            } else {
                const double seconds = frameToSeconds(frame, fps);
                for (std::size_t i = 0; i < history.loopSeconds.size() && !was; ++i) {
                    if (std::abs(history.loopSeconds[i] - seconds) < 1e-4) was = &history.frames[plain + i];
                }
            }
        }
        if (!was || was->size() != now.size()) break;
        past.push_back(was);
    }
    return past;
}

// Per vertex v, the lag of a point riding it: sum of weight x (where it was - where it is). A vertex
// that never moved lags by exactly nothing.
Vec3f jiggleVertexLag(const std::vector<const std::vector<Vec3f>*>& past, const std::vector<Vec3f>& now, const std::vector<double>& weights,
                      std::size_t v) {
    double lag[3] = {0.0, 0.0, 0.0};
    for (std::size_t k = 0; k < past.size(); ++k) {
        const Vec3f& was = (*past[k])[v];
        if (was == now[v]) continue;
        const double w = weights[k + 1];
        lag[0] += w * (double(was.x) - now[v].x);
        lag[1] += w * (double(was.y) - now[v].y);
        lag[2] += w * (double(was.z) - now[v].z);
    }
    return Vec3f{static_cast<float>(lag[0]), static_cast<float>(lag[1]), static_cast<float>(lag[2])};
}

// Per face, how its turning drags a point hung off it: sum of weight x (R_k - I), R_k the turn from
// the face's frame now to its frame k frames ago. A face that kept its pose adds nothing (false).
bool jiggleFaceTurn(const std::vector<const std::vector<Vec3f>*>& past, const std::vector<Vec3f>& now, const std::vector<double>& weights,
                    const ScalpMesh::Triangle& t, Mat3* turn) {
    const auto a = static_cast<std::size_t>(t[0]), b = static_cast<std::size_t>(t[1]), c = static_cast<std::size_t>(t[2]);
    Mat3 current;
    if (!triangleFrame(now[a], now[b], now[c], &current)) return false;
    Mat3 sum{};
    bool any = false;
    for (std::size_t k = 0; k < past.size(); ++k) {
        const std::vector<Vec3f>& was = *past[k];
        if (was[a] == now[a] && was[b] == now[b] && was[c] == now[c]) continue;
        Mat3 then;
        if (!triangleFrame(was[a], was[b], was[c], &then)) continue;
        const double w = weights[k + 1];
        for (int r = 0; r < 3; ++r) {
            for (int col = 0; col < 3; ++col) {
                // R = then * current^T
                double rv = 0.0;
                for (int j = 0; j < 3; ++j) rv += then[r * 3 + j] * current[col * 3 + j];
                sum[r * 3 + col] += w * (rv - (r == col ? 1.0 : 0.0));
            }
        }
        any = true;
    }
    if (any) *turn = sum;
    return any;
}

// Secondary motion from the scalp's motion, with no simulation cache: every strand hangs from
// its root's frame on the scalp by a damped spring (`frequency` Hz, `damping` ratio). Where that
// frame was over the last `history` frames (the host's "scalpHistory" external) gives the spring's
// lag behind it now: a strand trails when the head moves, overshoots when it stops, and settles.
// The lag turns each strand about its root (by `profile` along it, so tips lag more), keeping its
// segment lengths. With `loop`, the frames before loopStart are read from the end of the range
// (jiggleLoopFrame: the host samples them too, scalpHistoryLoopFrames), so a scalp animation
// looping over loopStart to loopEnd jiggles the same at both ends: a looping animation. Without
// history, or where the scalp did not move, the strands go through as they are, to the bit.
class JiggleOp : public Operator {
public:
    std::string typeName() const override { return op::kJiggle; }
    std::string category() const override { return "motion"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How much of the lag and overshoot the strands take (mappable); 0: none"}.soft(0, 2),
            ParamSpec{"frequency", 2.0, "How fast the hair springs back: its swing's frequency in cycles per second (Hz)"}
                .atLeast(1e-3)
                .soft(0.1, 10),
            ParamSpec{"damping", 0.3, "How quickly the swinging dies out: 0 swings on, 1 settles without overshoot"}
                .atLeast(0)
                .soft(0, 1),
            ParamSpec{"history", std::int64_t{12}, "Earlier frames of the scalp's motion it reacts to (more: a longer "
                                                   "memory)"}
                .range(1, kMaxScalpHistory)
                .soft(1, 24)
                .adv(),
            {"profile", Ramp::linear(0.0f, 1.0f), "How much each part lags, root to tip (tips lag more)"},
            notAnimatable({"loop", false, "Read the frames before loopStart from the end of the range, so a scalp "
                                          "animation that loops from loopStart to loopEnd jiggles seamlessly: a looping "
                                          "animation"}),
            notAnimatable({"loopStart", std::int64_t{1}, "The frame the scalp's loop starts at"}).when("loop", {"true"}),
            notAnimatable({"loopEnd", std::int64_t{49}, "The frame the scalp's loop ends at (its pose there loopStart's; "
                                                        "after loopStart, else no loop)"})
                .when("loop", {"true"}),
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp", "scalpHistory"}; }

    std::string note(const EvalContext& ctx) const override {
        if (valueAs<ScalpHistoryPtr>(ctx.external("scalpHistory"))) return {};
        return "no scalp motion history here: the strands pass through (Maya samples the scalp's earlier frames; "
               "a groom file needs an animated scalp cache)";
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
    // result's copy; false when that cannot be done. A strand's lag is its root's (the scalp's
    // motion there: the scalp and its history are externals, the last result's) and its own chord:
    // a strand made again works out its face's and vertices' lag itself, as the full evaluation
    // works them out once for every face strands grow from.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kJiggle);
        // The input handed on (nothing moves): the graph passes its changes on.
        auto handOn = [&] {
            if (incremental) return false;
            *result = in;
            return true;
        };
        const ScalpHistoryPtr history = valueAs<ScalpHistoryPtr>(ctx.external("scalpHistory"));
        const MeshPtr scalpPtr = valueAs<MeshPtr>(ctx.external("scalp"));
        if (!history || !scalpPtr || history->step <= 0.0 || in->strandCount() == 0) return handOn();
        const ScalpMesh& scalp = *scalpPtr;
        const std::vector<Vec3f>& now = scalp.points();

        // The frames it reads (jigglePast).
        const double fps = ctx.framesPerSecond, nowFrame = secondsToFrame(ctx.time, fps);
        const std::vector<const std::vector<Vec3f>*> past = jigglePast(params(), *history, now, nowFrame, fps);
        const std::size_t frames = past.size();
        bool moved = false;
        for (std::size_t k = 0; k < frames && !moved; ++k) moved = *past[k] != now;
        if (!moved) return handOn();

        const ScalarField amount(params(), "strength", &scalp);
        amount.validate(*in);
        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !amount.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        const std::vector<double> weights =
            springLagWeights(static_cast<int>(frames), history->step, params().getDouble("frequency"),
                             params().getDouble("damping"));
        const auto& tris = scalp.triangles();

        // Per vertex, the lag of a point riding it; per face, how its turning drags a point hung off
        // it (jiggleVertexLag, jiggleFaceTurn).
        auto lagOfVertex = [&](std::size_t v) { return jiggleVertexLag(past, now, weights, v); };
        auto turnOfFace = [&](std::size_t f, Mat3* turn) { return jiggleFaceTurn(past, now, weights, tris[f], turn); };

        // Worked out once for the faces and vertices strands grow from, in full; each strand made
        // again works out its own.
        JiggleLags lags;
        std::vector<Vec3f>& vertexLag = lags.vertexLag;
        std::vector<Mat3>& faceTurn = lags.faceTurn;
        std::vector<char>& faceTurned = lags.faceTurned;
        // The lags of the faces and vertices the strands it runs grow from (all, or those the input
        // changed in), then look.slang's JiggleCS, the GPU preview's own (look_cpu.h), on them: in
        // full, or on a view of those strands.
        const std::vector<std::uint32_t>* which = last ? &changes->strands : nullptr;
        std::vector<char> usedFace(tris.size(), 0), usedVertex(now.size(), 0);
        auto use = [&](std::size_t s) {
            const StrandRoot& root = in->roots()[s];
            if (scalp.contains(root)) usedFace[static_cast<std::size_t>(root.face)] = 1;
        };
        if (which) {
            for (const std::uint32_t s : *which) use(s);
        } else {
            for (std::size_t s = 0; s < in->strandCount(); ++s) use(s);
        }
        std::vector<std::size_t> faces;
        for (std::size_t f = 0; f < tris.size(); ++f) {
            if (!usedFace[f]) continue;
            faces.push_back(f);
            for (const std::int32_t v : tris[f]) usedVertex[static_cast<std::size_t>(v)] = 1;
        }
        std::vector<std::size_t> vertices;
        for (std::size_t v = 0; v < now.size(); ++v) {
            if (usedVertex[v]) vertices.push_back(v);
        }
        vertexLag.assign(now.size(), Vec3f{});
        parallelFor(0, vertices.size(), [&](std::size_t i) { vertexLag[vertices[i]] = lagOfVertex(vertices[i]); });
        faceTurn.assign(tris.size(), Mat3{});
        faceTurned.assign(tris.size(), 0);
        parallelFor(0, faces.size(), [&](std::size_t i) {
            const std::size_t f = faces[i];
            faceTurned[f] = turnOfFace(f, &faceTurn[f]) ? 1 : 0;
        });
        lags.frames = frames;
        gpu::StageInputs lagged;
        lagged.jiggle = gpu::jiggleInputs(*in, scalp, lags, which);
        if (!last) {
            *result = gpu::runStage(gpu::StageKind::Jiggle, params(), in, &scalp, std::move(lagged));
            remember(valueAs<StrandsPtr>(*result), {scalpPtr, history, nowFrame, fps});
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        gpu::runStageOnStrands(gpu::StageKind::Jiggle, params(), in, changes->strands, &scalp, *out, std::move(lagged));
        *changed = *changes;
        *result = StrandsPtr(out);
        remember(StrandsPtr(out), {scalpPtr, history, nowFrame, fps});
        return true;
    }

public:
    // operator_state.h jiggleState.
    bool stateOf(const StrandsPtr& output, JiggleState* state) const {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (!output || !made_.scalp || !sameOrShown(madeOutput_.lock(), output)) return false;
        *state = made_;
        return true;
    }
    // Masked: its state answers for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (made && madeOutput_.lock() == made) madeOutput_ = mixed;
    }

private:
    // The last result it jiggled (by identity) and what it read for it, for a host running its
    // springs elsewhere (the GPU preview).
    void remember(const StrandsPtr& output, const JiggleState& state) const {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        madeOutput_ = output;
        made_ = state;
    }
    mutable std::mutex madeMutex_;
    mutable std::weak_ptr<const StrandSet> madeOutput_;
    mutable JiggleState made_;
};

}  // namespace

bool windTime(const Operator& op, const StrandsPtr& output, double* time, double* framesPerSecond) {
    const auto* wind = dynamic_cast<const WindOp*>(&op);
    return wind && wind->timeOf(output, time, framesPerSecond);
}

bool jiggleState(const Operator& op, const StrandsPtr& output, JiggleState* state) {
    const auto* jiggle = dynamic_cast<const JiggleOp*>(&op);
    return jiggle && jiggle->stateOf(output, state);
}

std::size_t jiggleFramesRead(const ParamSet& params, const JiggleState& state) {
    if (!state.scalp || !state.history) return 0;
    return jigglePast(params, *state.history, state.scalp->points(), state.nowFrame, state.framesPerSecond).size();
}

void jiggleLags(const ParamSet& params, const JiggleState& state, const std::vector<char>& used, JiggleLags* out) {
    *out = JiggleLags{};
    if (!state.scalp || !state.history || state.history->step <= 0.0) return;
    const ScalpMesh& scalp = *state.scalp;
    const std::vector<Vec3f>& now = scalp.points();
    const auto& tris = scalp.triangles();
    const std::vector<const std::vector<Vec3f>*> past = jigglePast(params, *state.history, now, state.nowFrame, state.framesPerSecond);
    out->frames = past.size();
    const std::vector<double> weights = springLagWeights(static_cast<int>(past.size()), state.history->step, params.getDouble("frequency"),
                                                         params.getDouble("damping"));
    std::vector<std::size_t> faces;
    std::vector<char> usedVertex(now.size(), 0);
    for (std::size_t f = 0; f < tris.size(); ++f) {
        if (!used.empty() && (f >= used.size() || !used[f])) continue;
        faces.push_back(f);
        for (const std::int32_t v : tris[f]) usedVertex[static_cast<std::size_t>(v)] = 1;
    }
    out->vertexLag.assign(now.size(), Vec3f{});
    parallelFor(0, now.size(), [&](std::size_t v) {
        if (usedVertex[v]) out->vertexLag[v] = jiggleVertexLag(past, now, weights, v);
    }, 1024);
    out->faceTurn.assign(tris.size(), Mat3{});
    out->faceTurned.assign(tris.size(), 0);
    parallelFor(0, faces.size(), [&](std::size_t i) {
        const std::size_t f = faces[i];
        out->faceTurned[f] = jiggleFaceTurn(past, now, weights, tris[f], &out->faceTurn[f]) ? 1 : 0;
    }, 256);
}

WindPhase windPhase(const Vec3f& direction, float frequency, double speed, double seconds, double framesPerSecond, bool loop,
                    double loopStart, double loopEnd) {
    WindPhase p;
    // In double so late frames keep their precision.
    const double flutterRate = std::max(1.0, std::abs(static_cast<double>(frequency) * speed)) * 2.0;  // Hz
    if (!loop || !(loopEnd > loopStart)) {
        p.shift = direction * static_cast<float>(static_cast<double>(frequency) * speed * seconds);
        p.flutterPhase = static_cast<float>(std::fmod(kTwoPi * flutterRate * seconds, kTwoPi));
        return p;
    }
    // How far into the loop the time is, 0 to 1 (repeating outside the range), by the frame to a
    // millionth (seconds back to frames may miss a whole frame by a hair): the loop's end is its
    // start again, and a frame a whole number of loops on the same frame, exactly.
    const double frames = loopEnd - loopStart;
    const double frame = std::round(secondsToFrame(seconds, framesPerSecond) * 1e6) / 1e6;
    double into = std::fmod(frame - loopStart, frames);
    if (into < 0.0) into += frames;
    const double share = into < frames ? into / frames : 0.0;
    const double length = frameToSeconds(frames, framesPerSecond);  // seconds
    // The field travels `around` noise units along the wind a loop: once round a circle of that
    // circumference, a spot's place along the wind turning it as much (no travel: no circle).
    const double around = static_cast<double>(frequency) * speed * length;
    p.looped = true;
    const Vec3f helper = std::abs(direction.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 1.0f, 0.0f};
    p.across1 = direction.cross(helper).normalized();
    p.across2 = direction.cross(p.across1);
    if (std::abs(around) > 1e-9) {
        p.radius = static_cast<float>(std::abs(around) / kTwoPi);
        p.turnsPer = static_cast<float>(kTwoPi / around);
    }
    p.angle = static_cast<float>(kTwoPi * share);
    // The flutter: a whole number of cycles a loop, as near its rate as that comes.
    const double cycles = std::max(1.0, std::round(flutterRate * length));
    p.flutterPhase = static_cast<float>(std::fmod(kTwoPi * cycles * share, kTwoPi));
    return p;
}

double jiggleLoopFrame(double frame, double now, double loopStart, double loopEnd) {
    const double frames = loopEnd - loopStart;
    if (!(frames > 0.0) || now < loopStart || frame >= loopStart) return frame;
    const double wrapped = frame + std::ceil((loopStart - frame) / frames) * frames;
    return wrapped >= loopEnd ? wrapped - frames : wrapped;
}

void registerWindOperator(OperatorRegistry& registry) {
    registry.registerType(op::kWind,
                          {"motion",
                           "Strands bend along a wind moving through space and time. Play the timeline to see it.",
                           "breeze air turbulence"},
                          [] { return std::make_unique<WindOp>(); });
}

void registerJiggleOperator(OperatorRegistry& registry) {
    registry.registerType(op::kJiggle,
                          {"motion",
                           "Strands lag behind the scalp's motion, overshoot and settle: no simulation cache.",
                           "secondary lag overshoot follow"},
                          [] { return std::make_unique<JiggleOp>(); });
}

}  // namespace groomlab
