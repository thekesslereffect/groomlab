// SPDX-License-Identifier: Apache-2.0
// Wet: wet hair's clumping as an elastocapillary cascade (Bico et al., "Elastocapillary
// coalescence in wet hair", Nature 2004; Py et al. 2007). Surface tension pulls wet fibres into
// bundles against their bending stiffness; a bundle of N fibres forms only where the free length
// past the point they join is long enough, a length set by the elastocapillary length
// L_ec = sqrt(B / gamma), longer for bigger bundles. So near the root only small clumps form, and
// toward the tip they merge into larger and larger ones: a hierarchy of clumps, small ones nested
// in big ones, their size growing along the strand.
//
// The node is a chain of Clumps (look_ops.cpp), one per level, run largest first:
//   level k = 0 .. levels-1 (0 the smallest), clump size r_k = clumpSize * 2^(k - (levels-1)),
//   each Clump picking its own centers about r_k apart (clumpBy size) and writing its clump IDs;
//   every smaller level clumps only within the clumps of the level above it (its regionChannel is
//   that level's clump channel), so the strands of one small clump are always in one big clump.
// The largest level pulls first, so the smaller levels' axes (strands too) have already moved
// toward it: each level's centers pull toward the next level's, small to large, and a strand's
// point ends up at p + w_0 (D_0 - p), D_k = C_k + w_{k+1} (D_{k+1} - C_k), C_k its level-k axis.
//
// Where level k engages (its Clump's profile, t the share of the strand from root to tip, as every
// Clump profile reads it):
//   a_k = sqrt(r_k / clumpSize)                    bigger bundles need a longer free length
//   start_k = a_k * lerp(kDryStart, kWetStart, wetness)
//                                                  more water: a shorter effective L_ec, so every
//                                                  level forms closer to the root
//   width_k = kBlend * start_k                     a smoothstep from start_k to start_k + width_k
//   tight(t) = wetness (2 - wetness) * (kRootTight + (kTipTight - kRootTight) t)
//                                                  wet clumps are tight: more so wetter, and toward the tip
//   profile_k(t) = tight(t) * smoothstep((t - start_k) / width_k)
// At wetness 0 the strands pass through as they came (no channel either): drying out is
// animating wetness from 1 to 0. Each point gets a `wetness` channel (the wetness) for a shader to
// darken or shine wet hair. Each result keeps what its levels' Clumps made (operator_state.h
// WetState): the GPU preview runs the levels as Clump stages from it (look_chain.h).

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "groomlab/export_view.h"
#include "groomlab/graph.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr std::int64_t kMaxLevels = 5;
// Where the largest level starts, as a share of the strand: barely damp (the curve's dry end) and
// soaked (wetness 1). Each smaller level starts sqrt(its size / clumpSize) of that.
constexpr float kDryStart = 0.9f;
constexpr float kWetStart = 0.3f;
constexpr float kBlend = 0.6f;     // a level's blend from nothing to all, as a share of its start
constexpr float kRootTight = 0.6f; // how tight a clump is (its pull) at the root and at the tip, soaked
constexpr float kTipTight = 0.92f;
constexpr std::size_t kProfileKeys = 32;  // the profile's keys, evenly along it (with its start and end)

// The clump channels' base when clumpChannel is empty: the levels need them to nest, removed after.
constexpr const char* kHiddenChannel = "__wetClump";

// Level k's (0 the smallest) of `levels`: the Clump's profile at `wetness`.
Ramp levelProfile(std::int64_t k, std::int64_t levels, float wetness) {
    const float a = pm::pow(2.0f, 0.5f * static_cast<float>(k - (levels - 1)));
    const float start = a * (kDryStart + (kWetStart - kDryStart) * wetness);
    const float width = kBlend * start;
    const float tight = wetness * (2.0f - wetness);
    auto at = [&](float t) {
        return tight * (kRootTight + (kTipTight - kRootTight) * t) * smoothstep(0.0f, 1.0f, (t - start) / width);
    };
    std::vector<float> ts;
    for (std::size_t i = 0; i < kProfileKeys; ++i) ts.push_back(static_cast<float>(i) / static_cast<float>(kProfileKeys - 1));
    if (start < 1.0f) ts.push_back(start);
    if (start + width < 1.0f) ts.push_back(start + width);
    std::sort(ts.begin(), ts.end());
    Ramp ramp;
    for (const float t : ts) {
        if (!ramp.keys.empty() && t - ramp.keys.back().first < 1e-4f) continue;
        ramp.keys.emplace_back(t, at(t));
    }
    return ramp;
}

class WetOp : public Operator {
public:
    // The Clumps' rest scalp (where roots are for the nearest center) and parts.
    std::vector<std::string> externalInputs() const override { return {"scalp", "parts"}; }
    std::string typeName() const override { return op::kWet; }
    // clumpChannel's name for the biggest clumps, with each smaller level's halvings after it
    // (clumpId, clumpId1, clumpId2...), and the wetness per point.
    std::vector<ChannelWrite> channelWrites() const override {
        std::vector<ChannelWrite> out;
        const std::string base = params().getString("clumpChannel");
        if (!base.empty()) {
            const std::int64_t levels = wetLevelCount(params());
            for (std::int64_t j = 0; j < levels; ++j) out.push_back({j == 0 ? base : base + std::to_string(j), "clumpChannel"});
        }
        out.push_back({kWetnessChannel, ""});
        return out;
    }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"wetness", 0.7,
                      "How wet the hair is, 0-1: small clumps near the root merging into bigger ones toward the tip, "
                      "forming closer to the root and tighter the wetter it is. 0: dry, the strands as they came "
                      "(animate it from 1 to 0 to dry the hair out)", "fraction"}
                .range(0, 1),
            notAnimatable({"clumpSize", 1.5,
                           "The biggest clumps' size (at the tips): about how far apart their centers are, as Clump's "
                           "size; each smaller level is half the one above", "length"})
                .atLeast(0)
                .soft(0, 5),
            notAnimatable({"levels", std::int64_t{3},
                           "How many sizes of clump nest inside one another, each half the size of the one above, "
                           "the smallest forming nearest the root"})
                .range(1, kMaxLevels),
            notAnimatable({"randomness", 0.3,
                           "How random the clumps' centers are: 0 evenly spread, 1 at random (some side by side), "
                           "up to 2. As Clump's", "fraction"})
                .range(0, 2)
                .soft(0, 1),
            ParamSpec{"clumpChannel", std::string("clumpId"),
                      "Per-strand channel receiving the biggest clumps' numbers (what a Cards, Twist or HairColor's "
                      "clumpShare after it follows); each smaller level writes it with the level's halvings after "
                      "it (clumpId1, clumpId2...). Empty: none"}
                .adv()
                .writesChannel(),
            notAnimatable({"seed", std::int64_t{0}, "Random seed for the clumps' centers"}).adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kWet);
        const auto wetness = static_cast<float>(params().getDouble("wetness"));
        if (!(wetness > 0.0f) || in->empty()) return in;  // dry: as it came
        const std::int64_t levels = wetLevelCount(params());
        const double size = params().getDouble("clumpSize");
        const double randomness = params().getDouble("randomness");
        const std::string named = params().getString("clumpChannel");
        const std::string base = named.empty() ? std::string(kHiddenChannel) : named;
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        auto channelOf = [&](std::int64_t halvings) { return halvings == 0 ? base : base + std::to_string(halvings); };

        StrandsPtr strands = in;
        // What each level's Clump made (operator_state.h WetState): a host running the levels as
        // Clumps (the GPU preview) takes their centers from it.
        auto kept = std::make_shared<WetState>();
        kept->wetness = wetness;
        bool keeps = true;
        {
            // The levels' Clumps, kept from one evaluation to the next (each keeps its pick of
            // centers, so a change of the wetness alone picks none again); one evaluation at a time.
            const std::lock_guard<std::mutex> lock(mutex_);
            ensureLevels(static_cast<std::size_t>(levels));
            for (std::int64_t j = 0; j < levels; ++j) {  // j halvings: the largest first
                Operator& clump = *levels_[static_cast<std::size_t>(j)];
                ParamSet& p = clump.params();
                const std::int64_t k = levels - 1 - j;
                p.set("strength", 1.0);
                p.set("profile", levelProfile(k, levels, wetness));
                p.set("centersFrom", std::string("pick"));
                p.set("clumpBy", std::string("size"));
                p.set("size", size * pm::pow(2.0, -static_cast<double>(j)));
                p.set("randomness", randomness);
                p.set("pastTip", std::string("extend"));  // long hair keeps its length past a shorter axis
                p.set("clumpChannel", channelOf(j));
                // Within the clumps of the level above; the largest within the strands' own regions.
                p.set("regionChannel", j == 0 ? std::string("region") : channelOf(j - 1));
                p.set("seed", static_cast<std::int64_t>(hashCombine(hash64(seed ^ 0x3E7C1u), static_cast<std::uint64_t>(j)) >> 1));
                strands = valueAs<StrandsPtr>(clump.evaluate(ctx, {Value(strands), Value{}}));
                if (!strands) throw GraphError("Wet: a level made no strands");
                kept->params.push_back(p);
                kept->levels.emplace_back();
                keeps = keeps && clumpState(clump, strands, &kept->levels.back());
            }
        }
        auto out = std::make_shared<StrandSet>(*strands);
        if (named.empty()) {
            for (std::int64_t j = 0; j < levels; ++j) out->removeStrandChannel(channelOf(j));
        }
        out->addPointChannel(kWetnessChannel, wetness);
        StrandsPtr result(out);
        {
            const std::lock_guard<std::mutex> lock(keptMutex_);
            kept_.output = result;
            kept_.state = keeps ? std::shared_ptr<const WetState>(std::move(kept)) : nullptr;
        }
        return result;
    }

    // What made `output` (operator_state.h).
    bool state(const StrandsPtr& output, std::shared_ptr<const WetState>* state) const {
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

    // The levels' center picks another copy of the node made (Operator::adoptKeptWork).
    void adoptKeptWork(const Operator& previous) override {
        const auto* from = dynamic_cast<const WetOp*>(&previous);
        if (!from || from == this) return;
        std::scoped_lock lock(mutex_, from->mutex_);
        ensureLevels(from->levels_.size());
        for (std::size_t j = 0; j < from->levels_.size(); ++j) levels_[j]->adoptKeptWork(*from->levels_[j]);
    }

private:
    // At least `count` levels' Clumps (the mutex held).
    void ensureLevels(std::size_t count) const {
        while (levels_.size() < count) {
            std::unique_ptr<Operator> clump = OperatorRegistry::instance().create(op::kClump);
            // Its levels pick their centers among the strands, as Clump's version 1 (a Wet's look
            // stays the one it was saved with; centers on the scalp are Clump's version 2).
            clump->setNodeVersion(1);
            levels_.push_back(std::move(clump));
        }
    }

    mutable std::mutex mutex_;
    mutable std::vector<std::unique_ptr<Operator>> levels_;  // the Clump of each level, by halvings
    // The last result (by identity) and what its levels made (null: a level kept nothing).
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        std::shared_ptr<const WetState> state;
    };
    mutable std::mutex keptMutex_;
    mutable Kept kept_;
};

}  // namespace

bool wetState(const Operator& op, const StrandsPtr& output, std::shared_ptr<const WetState>* state) {
    const auto* wet = dynamic_cast<const WetOp*>(&op);
    return wet && wet->state(output, state);
}

std::int64_t wetLevelCount(const ParamSet& wet) { return std::clamp<std::int64_t>(wet.getInt("levels"), 1, kMaxLevels); }

Ramp wetLevelProfile(std::int64_t j, std::int64_t levels, float wetness) { return levelProfile(levels - 1 - j, levels, wetness); }

void registerWetOperator(OperatorRegistry& registry) {
    registry.registerType(op::kWet,
                          {"look",
                           "Wet hair: small clumps near the root merging into bigger ones toward the tip, from one wetness slider.",
                           "water damp clumps"},
                          [] { return std::make_unique<WetOp>(); });
}

}  // namespace groomlab
