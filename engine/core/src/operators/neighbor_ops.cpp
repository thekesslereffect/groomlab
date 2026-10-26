// SPDX-License-Identifier: Apache-2.0
// Shaping operators that read other strands: BlendNeighbors (each strand's
// shape pulled toward its nearest neighbours' mean shape, for a coherent
// flow after noisy interpolation or sculpting) and RestoreLength (segment
// lengths or the whole length brought back from a reference set of the same
// strands, after deformers that stretch them).

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "groomlab/curve.h"
#include "groomlab/field.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/parts.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr std::size_t kMaxNeighbors = 16;
// With parts, some of the nearest roots may be across one: look at more.
constexpr std::size_t kMaxCandidates = kMaxNeighbors * 2 + 1;

// BlendNeighbors: each strand's shape (its points as offsets from its root)
// moves toward the inverse-distance-weighted mean of its `neighbors` nearest core
// strands' shapes (preview_core.h: those every preview holds, so its neighbours are the same at
// every preview density) by `strength` times `profile` along the length. Neighbours
// are found by root (on the rest scalp when it has one, so a deforming scalp
// keeps the same neighbours), their offsets turned from their root normal to
// this strand's, and resampled to this strand's point count when theirs
// differs. Roots never move; with preserveLength each strand keeps its own
// arc length. Every neighbour is read from the input set, so the result does
// not depend on the order strands are processed in. Neighbours are never across a part
// (respectParts) nor, with regions (the `regionChannel` an Interpolate's regionMap writes), in
// another region: the nearest of the strand's own region.
class BlendNeighborsOp : public Operator {
public:
    std::string typeName() const override { return op::kBlendNeighbors; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override {
        return params().getBool("respectParts") ? std::vector<std::string>{"scalp", "parts"}
                                                      : std::vector<std::string>{"scalp"};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"neighbors", std::int64_t{4}, "Nearest strands (by root, of those every preview holds) each strand blends toward, 1-16"}
                .range(1, static_cast<double>(kMaxNeighbors)),
            ParamSpec{"strength", 1.0, "How far each strand moves toward its neighbours' mean shape, 0-1 (mappable)",
                      "fraction"}
                .range(0, 1),
            {"profile", Ramp::constant(1.0f), "Amount along the length, root to tip"},
            ParamSpec{"preserveLength", true, "Scale the blended strand back to its own length"}.adv(),
            {"respectParts", true, "Never blend with a strand across a part"},
            ParamSpec{"regionChannel", std::string("region"),
                      "Channel numbering each strand's region (an Interpolate's regionMap writes region): strands blend "
                      "only with strands of their own region; empty, or a channel the strands lack: across regions"}
                .adv()
                .readsChannel(true),
        };
    }

    // operator_state.h blendNeighborsState.
    bool keptState(const StrandsPtr& output, std::shared_ptr<const BlendNeighborsState>* state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (!output || !kept_.state || !sameOrShown(kept_.output.lock(), output)) return false;
        *state = kept_.state;
        return true;
    }
    // Masked: its state answers for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (made && kept_.output.lock() == made) kept_.output = mixed;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kBlendNeighbors);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField amount(params(), "strength", scalp);
        amount.validate(*in);
        const std::vector<float>* regions = regionsOf(*in, params().getString("regionChannel"));
        remember(nullptr, nullptr);
        if (in->strandCount() < 2) {
            // Nothing to blend with, whatever the strength.
            auto none = std::make_shared<BlendNeighborsState>();
            none->offsets.assign(in->strandCount() + 1, 0u);
            none->normals.assign(in->strandCount(), Vec3f{});
            remember(in, std::move(none));
            return in;
        }
        if (amount.isConstant() && amount.maximum() <= 0.0f) return in;
        const std::size_t requested = static_cast<std::size_t>(params().getInt("neighbors"));
        const Ramp profile = params().getRamp("profile");
        const bool preserveLength = params().getBool("preserveLength");
        const PartsPtr parts = params().getBool("respectParts") ? valueAs<PartsPtr>(ctx.external("parts")) : nullptr;

        // Roots to search (rest pose where there is one), root normals (zero when unrooted), sides of parts.
        const std::size_t count = in->strandCount();
        std::vector<Vec3f> roots(count), normals(count);
        std::vector<PartLines::Side> sides(parts ? count : 0);
        parallelFor(0, count, [&](std::size_t s) {
            const StrandRoot& root = in->roots()[s];
            const Vec3f p = in->strandPointCount(s) > 0 ? in->strandPoints(s)[0] : Vec3f{};
            roots[s] = restRootOr(scalp, root, p);
            normals[s] = scalp && scalp->contains(root) ? scalp->normal(root) : Vec3f{};
            if (parts) sides[s] = parts->sideOf(roots[s]);
        });
        // The strands each strand may blend with: the core ones (preview_core.h: those every preview
        // holds, so a strand's neighbours are the same at every preview density), all of them or with
        // regions those of its own region (a group each, searched on its own).
        const PreviewCore core(*in);
        struct Group {
            std::vector<std::uint32_t> strands;  // its core strands
            std::optional<PointIndex> index;
            std::size_t k = 0, candidates = 0;
        };
        std::vector<Group> groups;
        std::vector<std::uint32_t> groupOf(count, 0);
        std::map<float, std::uint32_t> ids;  // (a NaN region: a group of its own per strand)
        if (!regions) groups.emplace_back();
        for (std::size_t s = 0; s < count; ++s) {
            std::uint32_t id = 0;
            if (regions) {
                const float r = (*regions)[s];
                id = r != r ? static_cast<std::uint32_t>(groups.size())
                            : ids.emplace(r, static_cast<std::uint32_t>(groups.size())).first->second;
                if (id == groups.size()) groups.emplace_back();
            }
            if (core.core(s)) groups[id].strands.push_back(static_cast<std::uint32_t>(s));
            groupOf[s] = id;
        }
        for (Group& g : groups) {
            if (g.strands.empty()) continue;
            std::vector<Vec3f> at;
            at.reserve(g.strands.size());
            for (const std::uint32_t s : g.strands) at.push_back(roots[s]);
            g.index.emplace(std::move(at));
            // The strand itself may be among the nearest; with parts, some may be across one.
            g.k = std::min(g.strands.size(), requested);
            g.candidates = std::min(g.strands.size(), parts ? 2 * g.k + 1 : g.k + 1);
        }

        // Each strand's neighbours and their shares (kept for a host blending elsewhere, the GPU
        // preview: operator_state.h blendNeighborsState), whatever its strength.
        std::vector<std::array<std::uint32_t, kMaxNeighbors>> nearOf(count);
        std::vector<std::array<float, kMaxNeighbors>> shareOf(count);
        std::vector<std::uint32_t> keptOf(count, 0u);
        parallelFor(0, count, [&](std::size_t s) {
            if (in->strandPointCount(s) < 2) return;
            const Group& group = groups[groupOf[s]];
            if (!group.index) return;
            const std::size_t k = group.k;
            std::array<std::size_t, kMaxCandidates> idx{};
            std::array<float, kMaxCandidates> d2{};
            const std::size_t found = group.index->kNearest(roots[s], group.candidates, idx.data(), d2.data());
            // Keep the k nearest others, on this strand's side of any part.
            std::array<float, kMaxNeighbors> weight{};
            std::size_t kept = 0;
            float wsum = 0.0f;
            for (std::size_t i = 0; i < found && kept < k; ++i) {
                const std::size_t g = group.strands[idx[i]];
                if (g == s || in->strandPointCount(g) < 2) continue;
                if (parts && PartLines::separated(sides[s], sides[g])) continue;
                nearOf[s][kept] = static_cast<std::uint32_t>(g);
                weight[kept] = 1.0f / std::max(std::sqrt(d2[i]), 1e-6f);
                wsum += weight[kept];
                ++kept;
            }
            if (kept == 0 || wsum <= 0.0f) return;
            for (std::size_t i = 0; i < kept; ++i) shareOf[s][i] = weight[i] / wsum;
            keptOf[s] = static_cast<std::uint32_t>(kept);
        });
        auto state = std::make_shared<BlendNeighborsState>();
        state->offsets.assign(count + 1, 0u);
        for (std::size_t s = 0; s < count; ++s) state->offsets[s + 1] = state->offsets[s] + keptOf[s];
        state->neighbors.resize(state->offsets[count]);
        state->weights.resize(state->offsets[count]);
        for (std::size_t s = 0; s < count; ++s) {
            std::copy(nearOf[s].begin(), nearOf[s].begin() + keptOf[s], state->neighbors.begin() + state->offsets[s]);
            std::copy(shareOf[s].begin(), shareOf[s].begin() + keptOf[s], state->weights.begin() + state->offsets[s]);
        }
        state->normals = normals;

        auto out = std::make_shared<StrandSet>(*in);
        const RampAlong along(profile, *in);
        parallelFor(0, count, [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s);
            if (n < 2) return;
            const float a = std::clamp(amount.at(*in, s), 0.0f, 1.0f);
            if (a <= 0.0f) return;
            const std::size_t kept = keptOf[s];
            if (kept == 0) return;

            const Vec3f* own = in->strandPoints(s);
            const Vec3f root = own[0];
            thread_local std::vector<Vec3f> mean, resampled;
            mean.assign(n, Vec3f{});
            const bool haveNormal = normals[s].length() > 0.0f;
            for (std::size_t i = 0; i < kept; ++i) {
                const std::size_t g = nearOf[s][i];
                const float w = shareOf[s][i];
                const Vec3f* pts = in->strandPoints(g);
                if (in->strandPointCount(g) != n) {
                    resampled.resize(n);
                    resampleByArcLengthInto(pts, in->strandPointCount(g), resampled.data(), n);
                    pts = resampled.data();
                }
                const Vec3f groot = pts[0];
                if (haveNormal && normals[g].length() > 0.0f) {
                    const Rotation turn(normals[g], normals[s]);
                    for (std::size_t j = 1; j < n; ++j) mean[j] += turn(pts[j] - groot) * w;
                } else {
                    for (std::size_t j = 1; j < n; ++j) mean[j] += (pts[j] - groot) * w;
                }
            }

            Vec3f* dst = out->strandPoints(s);
            for (std::size_t j = 1; j < n; ++j) {
                const Vec3f offset = own[j] - root;
                const float t = a * std::clamp(along(j, n), 0.0f, 1.0f);
                dst[j] = root + offset + (mean[j] - offset) * t;
            }
            if (preserveLength) {
                const float target = arcLength(own, n);
                const float blended = arcLength(dst, n);
                if (target > 1e-8f && blended > 1e-6f * target) {
                    const float scale = target / blended;
                    for (std::size_t j = 1; j < n; ++j) dst[j] = root + (dst[j] - root) * scale;
                }
            }
        });
        const StrandsPtr result(out);
        remember(result, std::move(state));
        return result;
    }

private:
    // The last result (by identity), and the neighbours it was
    // blended with.
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        std::shared_ptr<const BlendNeighborsState> state;
    };
    void remember(const StrandsPtr& output, std::shared_ptr<const BlendNeighborsState> state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        kept_.output = output;
        kept_.state = std::move(state);
    }
    mutable std::mutex keptMutex_;
    mutable Kept kept_;
};

// RestoreLength: brings each strand's length back from the `reference`
// strand with the same ID (from before the deformers that stretched it).
// `segments` mode restores every segment's length walking from the root
// along the deformed directions (follow the leader); `scale` scales the
// whole strand about its root to the reference's arc length. Strands with
// no reference are left alone. Without a reference, `even` respaces each
// strand's own points evenly by arc length, keeping its length. Affect blends between the deformed
// and the restored strand.
class RestoreLengthOp : public Operator {
public:
    std::string typeName() const override { return op::kRestoreLength; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Curves}, {"reference", PortType::Curves, true}};
    }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"mode", std::string("segments"),
             "segments: each segment's length from the reference, along the deformed directions; scale: the whole "
             "strand scaled about its root to the reference's length; even: the strand's own points respaced "
             "evenly (no reference needed)",
             "", {"segments", "scale", "even"}},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand is restored from itself and the reference curve of its ID: only the strands the
    // input changed in are restored again, with the same reference (unchanged), or with reference
    // strands that changed in some strands (their own strands restored again too) when both hold
    // the same IDs in the same order, each once.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the inputs changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& /*ctx*/, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kRestoreLength);
        // The input handed on (nothing to restore): the graph passes its changes on.
        auto handOn = [&] {
            if (incremental) return false;
            *result = in;
            return true;
        };
        const std::string mode = params().getString("mode");
        if (mode != "segments" && mode != "scale" && mode != "even") {
            throw GraphError("RestoreLength: mode must be segments, scale or even");
        }
        if (in->empty()) return handOn();
        const bool even = mode == "even";

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the inputs changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last) return false;
            // Evenly, no reference is read; else one that changed in some strands is followed below.
            if (!even && !inputUnchanged(*incremental, 1) && !someStrandsChanged(*incremental, 1)) return false;
            if (changes->none() && (even || inputUnchanged(*incremental, 1))) {  // the same in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }

        // The reference curves by ID: strands, or guides fed to the Curves input.
        struct Ref {
            const Vec3f* points;
            std::size_t count;
        };
        std::unordered_map<StrandId, Ref> reference;
        const StrandsPtr refStrands = inputs.size() > 1 ? valueAs<StrandsPtr>(inputs[1]) : nullptr;
        const GuidesPtr refGuides = inputs.size() > 1 ? valueAs<GuidesPtr>(inputs[1]) : nullptr;
        if (refStrands) {
            reference.reserve(refStrands->strandCount());
            for (std::size_t s = 0; s < refStrands->strandCount(); ++s) {
                if (refStrands->strandPointCount(s) >= 2) {
                    reference[refStrands->ids()[s]] = {refStrands->strandPoints(s), refStrands->strandPointCount(s)};
                }
            }
        } else if (refGuides) {
            reference.reserve(refGuides->guides().size());
            for (const Guide& g : refGuides->guides()) {
                if (g.points.size() >= 2) reference[g.id] = {g.points.data(), g.points.size()};
            }
        }
        if (!even && reference.empty()) return handOn();  // nothing to restore from
        const bool scale = mode == "scale";

        // Incrementally: the strands the input changed in, and those whose reference strand
        // changed. A reference strand stands for the input's strand of its index when both hold
        // the same IDs in the same order and every reference strand is one (two points or more,
        // its ID its own).
        std::vector<std::uint32_t> remade;
        if (last) {
            const StrandChanges* refChanges = even ? nullptr : someStrandsChanged(*incremental, 1);
            if (refChanges && !refChanges->none()) {
                if (!refStrands || reference.size() != refStrands->strandCount() || refStrands->ids() != in->ids()) {
                    return false;
                }
                std::set_union(changes->strands.begin(), changes->strands.end(), refChanges->strands.begin(),
                               refChanges->strands.end(), std::back_inserter(remade));
            } else {
                remade = changes->strands;
            }
        }

        auto out = std::make_shared<StrandSet>(*in);
        if (last) positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        auto restore = [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s);
            if (n < 2) return;
            const Vec3f* src = in->strandPoints(s);
            thread_local std::vector<Vec3f> restored, refSampled;
            restored.assign(src, src + n);

            if (even) {
                refSampled.resize(n);
                resampleByArcLengthInto(src, n, refSampled.data(), n);
                std::copy(refSampled.begin(), refSampled.end(), restored.begin());
            } else {
                auto it = reference.find(in->ids()[s]);
                if (it == reference.end()) return;
                const Ref& ref = it->second;
                if (scale) {
                    const float own = arcLength(src, n);
                    const float target = arcLength(ref.points, ref.count);
                    if (own <= 1e-8f) return;
                    const float factor = target / own;
                    for (std::size_t i = 1; i < n; ++i) restored[i] = src[0] + (src[i] - src[0]) * factor;
                } else {
                    // The reference's segment lengths at this strand's point count.
                    const Vec3f* refPts = ref.points;
                    if (ref.count != n) {
                        refSampled.resize(n);
                        resampleByArcLengthInto(ref.points, ref.count, refSampled.data(), n);
                        refPts = refSampled.data();
                    }
                    // Lengths from the root out, along the deformed directions.
                    Vec3f previous = src[0];
                    for (std::size_t i = 1; i < n; ++i) {
                        const float length = (refPts[i] - refPts[i - 1]).length();
                        Vec3f d = src[i] - previous;
                        float l = d.length();
                        if (l <= 1e-8f) {
                            // A collapsed segment: carry on in the previous direction (or the reference's).
                            d = i > 1 ? restored[i - 1] - restored[i - 2] : refPts[i] - refPts[i - 1];
                            l = d.length();
                        }
                        const Vec3f moved = l > 1e-8f ? previous + d * (length / l) : previous;
                        restored[i] = moved;
                        previous = moved;
                    }
                }
            }

            std::copy(restored.begin() + 1, restored.end(), out->strandPoints(s) + 1);
        };
        if (last) {
            forEachListed(remade, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                restore(s);
            });
            *changed = StrandChanges::listed(std::move(remade));
        } else {
            parallelFor(0, out->strandCount(), restore);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

}  // namespace

bool blendNeighborsState(const Operator& op, const StrandsPtr& output,
                         std::shared_ptr<const BlendNeighborsState>* state) {
    const auto* blend = dynamic_cast<const BlendNeighborsOp*>(&op);
    return blend && blend->keptState(output, state);
}

void registerBlendNeighborsOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBlendNeighbors,
                          {"deform",
                           "Pulls each strand toward its neighbours' shape for a coherent flow.",
                           "coherent flow neighbours"},
                          [] { return std::make_unique<BlendNeighborsOp>(); });
}

void registerRestoreLengthOperator(OperatorRegistry& registry) {
    registry.registerType(op::kRestoreLength,
                          {"shape", "Gives strands their length back after deformers.", "preserve length stretch"},
                          [] { return std::make_unique<RestoreLengthOp>(); });
}

}  // namespace groomlab
