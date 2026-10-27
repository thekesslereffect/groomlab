// SPDX-License-Identifier: Apache-2.0
// Cinematic look operators: Flyaway (stray hairs), Tangle (mats, wet strands
// sticking in pairs), Flow and Cards. (HairColor lives in color_ops.cpp.)
//
// Random choices hash the stable strand ID with the seed, so results never
// depend on thread order or strand order.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>

#include "groomlab/cards.h"
#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/field.h"
#include "groomlab/flow.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/noise.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {



// ---------------------------------------------------------------------------

// Stray hairs: a fraction of strands leave the groom's flow from `start` on,
// in big slow arcs, a little longer than their neighbors. Writes a flyaway
// channel (1 for strays, an earlier Flyaway's kept) for the shader or later
// nodes. Put it after Clump. Two Flyaways on the same strands with the same
// seed pick the same strays (the graph editor gives a new one a fresh seed).
class FlyawayOp : public Operator {
public:
    std::string typeName() const override { return op::kFlyaway; }
    // Its strays flagged, on top of an earlier Flyaway's.
    std::vector<ChannelWrite> channelWrites() const override { return {{"flyaway", "", true}}; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"fraction", 0.03, "Fraction of strands that stray (mappable)", "fraction"}.soft(0, 1),
            ParamSpec{"stray", 0.75, "How far strays wander from the flow at the tip, scene units (mappable)", "length"}
                .soft(0, 3),
            ParamSpec{"frequency", 1.5, "Arcs per strand length"}.soft(0, 5),
            ParamSpec{"start", 0.25, "Where along the length strays leave the flow, 0 root to 1 tip"}.range(0, 0.99),
            ParamSpec{"standOut", 0.5, "How much strays stand out along their root direction, 0-1", "fraction"}.range(0, 1),
            ParamSpec{"lengthen", 0.15, "Extra length of strays, a fraction of their length (up to)"}.atLeast(0).soft(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }

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
        const StrandsPtr in = requireStrands(inputs, op::kFlyaway);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField fractionField(params(), "fraction", scalp);
        const ScalarField strayField(params(), "stray", scalp);
        fractionField.validate(*in);
        strayField.validate(*in);
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !fractionField.strandLocal() || !strayField.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        // An earlier Flyaway's strays stay flagged: this node only adds its own. (Incrementally the
        // last result's flags, and each strand made again starts from its earlier flag.)
        const std::vector<float>* had = in->strandChannel("flyaway");
        if (had && had->size() != in->strandCount()) had = nullptr;
        std::vector<float>* flags = last ? out->copyStrandChannelFrom("flyaway", *last) : nullptr;
        if (!flags) {
            if (last) return false;
            std::vector<float> earlier;
            if (had) earlier = *had;
            flags = &out->addStrandChannel("flyaway", 0.0f);
            if (earlier.size() == flags->size()) *flags = std::move(earlier);
        }
        std::vector<float>& flag = *flags;
        // look.slang's FlyawayCS, the GPU preview's own (look_cpu.h): its strays flagged here by the
        // gate it draws (the strand data's), on the points it took; in full, or the strands the input
        // changed in (each from its earlier flag), into the last result's copy.
        auto strayed = [&](std::size_t s) {
            return unitFloat(hashCombine(out->ids()[s], seed ^ 0xF1A7)) < fractionField.at(*out, s) && out->strandPointCount(s) >= 2;
        };
        if (!last) {
            std::vector<std::uint8_t> strays(out->strandCount(), 0);
            parallelFor(0, out->strandCount(), [&](std::size_t s) { strays[s] = strayed(s); });
            gpu::runStageInto(gpu::StageKind::Flyaway, params(), in, scalp, *out);
            for (std::size_t s = 0; s < strays.size(); ++s) {
                if (strays[s]) flag[s] = 1.0f;
            }
        } else {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
            gpu::runStageOnStrands(gpu::StageKind::Flyaway, params(), in, changes->strands, scalp, *out);
            for (const std::uint32_t s : changes->strands) flag[s] = strayed(s) ? 1.0f : had ? (*had)[s] : 0.0f;
            *changed = *changes;
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Mats and wet strands: a fraction of strands stick to a neighbor (a core strand, preview_core.h: the
// same at every preview density; roots
// within `radius`) over a span of their length. A strand moves onto its
// partner there, by `strength`; when the two picked each other, both move to
// the pair's middle over one shared span. At strength 1 they meet (unless the
// partner is itself pulled toward a third strand). Several Tangles with
// different spans build up matting; after a tight Clump it reads as wet hair.
// After a Clump, partners come from the strand's own clump (`sameClump`, the
// clumpId channel): strands of neighbouring clumps would bridge the gap between
// them. With regions (the `regionChannel` an Interpolate's regionMap writes) partners come from
// the strand's own region. A partner farther than `reach` at a point does not pull it (a strand
// sticks to hair it touches), and the strand keeps its segment lengths.
class TangleOp : public Operator {
public:
    std::string typeName() const override { return op::kTangle; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"fraction", 0.5, "Fraction of strands that stick to a neighbor (mappable)", "fraction"}.soft(0, 1),
            ParamSpec{"radius", 0.4, "How far apart the roots of stuck strands can be, scene units", "length"}
                .atLeast(1e-6)
                .soft(0.01, 2),
            ParamSpec{"strength", 0.8, "How close stuck strands come, 0-1 (1: they meet) (mappable)", "fraction"}
                .range(0, 1),
            ParamSpec{"start", 0.3, "Where along the length spans can begin, 0 root to 1 tip"}.range(0, 1),
            ParamSpec{"span", 0.4, "Length of the stuck span, a fraction of the strand", "fraction"}.range(0.01, 1),
            {"sameClump", true, "Strands stick only to strands of their own clump (the clumpId channel a Clump writes), "
                                "never across the gap to the next clump"},
            ParamSpec{"regionChannel", std::string("region"),
                      "Channel numbering each strand's region (an Interpolate's regionMap writes region): strands stick "
                      "only to strands of their own region; empty, or a channel the strands lack: across regions"}
                .adv()
                .readsChannel(true),
            ParamSpec{"reach", 0.25, "How far a strand is pulled to its partner at most, scene units: farther apart "
                                     "there, they do not stick (0: no limit)", "length"}
                .atLeast(0)
                .soft(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kTangle);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField fractionField(params(), "fraction", scalp);
        const ScalarField strengthField(params(), "strength", scalp);
        fractionField.validate(*in);
        strengthField.validate(*in);
        const float radius = static_cast<float>(params().getDouble("radius"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const std::vector<float>* clumps = params().getBool("sameClump") ? in->strandChannel(kClumpIdChannel) : nullptr;
        if (clumps && clumps->size() != in->strandCount()) clumps = nullptr;
        const std::vector<float>* regions = regionsOf(*in, params().getString("regionChannel"));

        remember(nullptr, nullptr);
        const std::size_t count = in->strandCount();
        std::vector<Vec3f> roots(count);
        // Partners come from the preview core (preview_core.h: the strands every preview holds), so
        // a strand sticks to the same neighbour at every preview density and in the render. Each
        // strand's root in parallel, then the pool in strand order.
        parallelFor(0, count, [&](std::size_t s) { roots[s] = in->strandPointCount(s) ? in->strandPoints(s)[0] : Vec3f{}; });
        const PreviewCore core(*in);
        std::vector<std::size_t> pool;
        std::vector<Vec3f> poolRoots;
        for (std::size_t s = 0; s < count; ++s) {
            if (!core.core(s)) continue;
            pool.push_back(s);
            poolRoots.push_back(roots[s]);
        }
        if (pool.empty()) {
            // No strand to stick to, whatever the other params.
            auto none = std::make_shared<TangleState>();
            none->partners.assign(count, kNoTanglePartner);
            none->draws.assign(count, 0.0f);
            remember(in, std::move(none));
            return in;
        }
        const PointIndex index(poolRoots);
        // Up to 6 candidates within the radius; of the strand's own clump (or region), from the 24 nearest.
        constexpr std::size_t kCandidates = 6, kSearched = 24;
        const std::size_t searched = clumps || regions ? kSearched : kCandidates + 1;
        // Each strand's pick (its partner and the draw placing the middle of its span), so a pair
        // that picked each other can share one span and meet in the middle (look.slang TangleCS).
        constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> partnerOf(count, kNone);
        std::vector<float> drawOf(count, 0.0f);
        parallelFor(0, count, [&](std::size_t s) {
            const std::uint64_t h = hashCombine(in->ids()[s], seed ^ 0x7A61);
            if (in->strandPointCount(s) < 2 || unitFloat(h) >= fractionField.at(*in, s)) return;
            std::array<std::size_t, kSearched> near{};
            std::array<float, kSearched> d2{};
            const std::size_t found = index.kNearest(roots[s], searched, near.data(), d2.data());
            std::array<std::size_t, kCandidates> partners{};
            std::size_t partnerCount = 0;
            for (std::size_t k = 0; k < found && partnerCount < kCandidates; ++k) {
                const std::size_t candidate = pool[near[k]];
                if (candidate != s && d2[k] <= radius * radius && in->strandPointCount(candidate) >= 2 &&
                    (!clumps || (*clumps)[candidate] == (*clumps)[s]) && (!regions || (*regions)[candidate] == (*regions)[s])) {
                    partners[partnerCount++] = candidate;
                }
            }
            if (partnerCount == 0) return;
            // The candidate drawing the least of this strand's draws for each (a hash of the pair),
            // not the one at a random place in the list: a candidate coming or going (a change of
            // `radius`) changes the partner only when it is, or was, the one picked.
            std::uint64_t least = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t k = 0; k < partnerCount; ++k) {
                const std::uint64_t draw = hashCombine(h, in->ids()[partners[k]]);
                if (draw < least) {
                    least = draw;
                    partnerOf[s] = partners[k];
                }
            }
            drawOf[s] = Rng(h).nextFloat();  // (where its span's middle lies: the stage's draw, look.slang TangleCS)
        });
        auto state = std::make_shared<TangleState>();
        state->partners.resize(count);
        for (std::size_t s = 0; s < count; ++s) {
            state->partners[s] = partnerOf[s] == kNone ? kNoTanglePartner : static_cast<std::uint32_t>(partnerOf[s]);
        }
        state->draws = std::move(drawOf);

        // look.slang's TangleCS, the GPU preview's own (look_cpu.h), on the partners and draws picked
        // here.
        gpu::StageInputs partners;
        partners.tangle = gpu::tangleInputs(*in, *state);
        const StrandsPtr made = gpu::runStage(gpu::StageKind::Tangle, params(), in, scalpOf(ctx), std::move(partners));
        remember(made, std::move(state));
        return made;
    }

    // operator_state.h tangleState.
    bool keptState(const StrandsPtr& output, std::shared_ptr<const TangleState>* state) const {
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

private:
    // The last result (by identity), and the partners it picked.
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        std::shared_ptr<const TangleState> state;
    };
    void remember(const StrandsPtr& output, std::shared_ptr<const TangleState> state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        kept_.output = output;
        kept_.state = std::move(state);
    }
    mutable std::mutex keptMutex_;
    mutable Kept kept_;
};

// Lays strands along the flow arrows on the scalp (the Flow tool): each
// leaves its root at `lift` degrees off the skin and is traced along the field
// like a streamline, curving as the arrows turn and around the scalp's shape,
// and bends toward the skin along its length (`bend`). Segment lengths are
// kept. Without arrows, strands lie along their own direction. Affect blends from the incoming
// shape to lying along the flow.
// With Scatter, Length and Width this grooms short fur and vellus without
// guides.
class FlowOp : public Operator {
public:
    std::string typeName() const override { return op::kFlow; }
    std::string category() const override { return "grow"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"lift", 12.0, "Angle off the skin at the root, degrees (mappable)", "angle"}.soft(0, 90),
            ParamSpec{"bend", 0.6, "How much the angle falls toward the tip, 0-1 (1: the tip lies on the skin)",
                      "fraction"}.range(0, 1),
            ParamSpec{"rollRandom", 6.0, "Up to this many degrees of random turn of each strand around the scalp's "
                                         "normal, off the flow", "angle"}
                .atLeast(0)
                .soft(0, 45),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp", "flow"}; }

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
        const StrandsPtr in = requireStrands(inputs, op::kFlow);
        const ScalpMesh* scalp = scalpOf(ctx);
        const FlowPtr flow = valueAs<FlowPtr>(ctx.external("flow"));
        const ScalarField liftField(params(), "lift", scalp);
        liftField.validate(*in);

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !liftField.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        const bool arrows = flow && !flow->empty();
        if (!arrows) ctx.note("no flow arrows: lay some with the Flow tool (Groom Tools > Create > Flow); strands lie along their own "
                                "direction");
        // look.slang's FlowCS, the GPU preview's own (look_cpu.h), on the core's field and scalp (each
        // strand's root traced here): in full, or the strands the input changed in.
        gpu::StageInputs traced;
        if (!last) {
            traced.flow = gpu::flowInputs(*in, scalp, arrows ? flow : nullptr);
            *result = gpu::runStage(gpu::StageKind::Flow, params(), in, scalp, std::move(traced));
            remember(valueAs<StrandsPtr>(*result), arrows ? flow : nullptr, scalp);
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        traced.flow = gpu::flowInputs(*in, scalp, arrows ? flow : nullptr, &changes->strands);
        gpu::runStageOnStrands(gpu::StageKind::Flow, params(), in, changes->strands, scalp, *out, std::move(traced));
        *changed = *changes;
        *result = StrandsPtr(out);
        remember(valueAs<StrandsPtr>(*result), arrows ? flow : nullptr, scalp);
        return true;
    }

public:
    // operator_state.h flowState.
    bool keptState(const StrandsPtr& output, FlowState* state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (!output || !kept_.made || !sameOrShown(kept_.output.lock(), output)) return false;
        *state = kept_.state;
        return true;
    }
    // Masked: its state answers for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (made && kept_.output.lock() == made) kept_.output = mixed;
    }

private:
    // The last result (by identity), and the field and scalp it was
    // laid along.
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        FlowState state;
        bool made = false;
    };
    void remember(const StrandsPtr& output, const FlowPtr& flow, const ScalpMesh* scalp) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        kept_.output = output;
        kept_.state.flow = flow;
        kept_.state.scalp = scalp;
        kept_.made = true;
    }
    mutable std::mutex keptMutex_;
    mutable Kept kept_;
};


// Game cards from the strands coming in: cards per clump (or group of strands) with
// the controls below; the strands pass through unchanged. The host
// lays out every Cards node's cards in one UV atlas (outCards) and bakes the
// strands each card was made from into it. Cards are built only when asked
// for (the host's card evaluation), so the node costs nothing otherwise.
class CardsOp : public Operator {
public:
    std::string typeName() const override { return op::kCards; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"clumpChannel", std::string("clumpId"), "Group strands by this channel (Clump writes clumpId); "
                                                              "without it, strands group by root into as many groups as "
                                                              "maxCards allows (0: 60)"}
                .adv()
                .when("featherMode", {"auto", "perVane", "off"})
                .readsChannel(),
            notAnimatable({"cardsPerClump", std::int64_t{1}, "Cards per clump: its strands split into this many groups lying together, "
                                                              "each a card along its own mean curve (1: one card per clump; a hero "
                                                              "groom wants 4 to 8)"})
                .range(1, 64)
                .soft(1, 8)
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"layers", std::int64_t{1}, "Depth layers per clump (1 to 3): inner and outer shells of cards, each showing "
                                                       "its own strands, alternately turned; cards per clump in all: cardsPerClump x "
                                                       "layers"})
                .range(1, 3)
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"segments", std::int64_t{8}, "Quads along each card"}).range(1, 256).soft(1, 32).in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"widthSegments", std::int64_t{1}, "Quads across each card (2 or more for curvature)"})
                .range(1, 16)
                .in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"widthScale", 1.0, "Card width, times its strands' spread"}.soft(0, 3).in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"widthProfile", Ramp::constant(1.0f), "Width along the card, root to tip (taper)"}.in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"margin", 0.1, "Share of the width kept clear of strands each side, so the bake never touches the "
                                     "texture's edges", "fraction"}
                .range(0, 0.45)
                .in("Shape"),
            ParamSpec{"fitTwist", 1.0, "How much each station turns to the way its strands lie across (their principal "
                                       "direction): 1 follows them, 0 faces out of the scalp", "fraction"}
                .range(0, 1)
                .in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"twist", 0.0, "Degrees the card turns around its length, root to tip (on top of the fit)", "angle"}
                .soft(-180, 180)
                .in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"curvature", 0.0, "0 flat, 1 a half-tube bowed out of the scalp"}.range(0, 1).in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"ruffle", 0.3, "Alternate cards of a clump push in and out along their normal (a share of their "
                                     "width) and turn a little the other way, so layered cards never share a plane",
                      "fraction"}
                .range(0, 1)
                .in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"minStrands", std::int64_t{3}, "Groups with fewer strands make no card (their strands are flyaways)"})
                .atLeast(1)
                .soft(1, 20)
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"flyawayCards", std::int64_t{0}, "Thin cards for strands no card holds (small clumps, strands without "
                                                             "one), the farthest from any card first; the rest join the nearest card "
                                                             "within reach"})
                .range(0, 100000)
                .soft(0, 200)
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"minWidth", 0.002, "Narrowest card", "width"}).soft(0, 0.05).in("Shape"),
            notAnimatable({"maxCards", std::int64_t{0}, "At most this many cards (sub-cards, crossed and flyaway cards count): clumps "
                                                         "give up sub-cards, then neighbouring clumps share wider cards (0: no "
                                                         "limit). Lower LODs use fewer. The one budget: without clumps, the "
                                                         "strands group into as many as it allows"})
                .range(0, 100000)
                .soft(0, 5000),
            notAnimatable({"variations", std::int64_t{16}, "Textures the cards share: alike cards (length, width, how their strands "
                                                           "lie, color) use one, for sharper textures in the same atlas (0: every "
                                                           "card its own)"})
                .range(0, 4096)
                .soft(0, 64)
                .in("Texture"),
            ParamSpec{"mirror", true, "Cards sharing a texture alternate mirrored, so the repeat doesn't show"}.in("Texture"),
            ParamSpec{"crossStyle", std::string("auto"), "Cards per clump: single (one ribbon), cross (crossCount crossed "
                                                "around its axis) or auto (a cross for round tufts standing off the "
                                                "scalp, one ribbon elsewhere)", "", {"auto", "single", "cross"}}
                .when("featherMode", {"auto", "perVane", "off"}),
            // Shown whatever the crossStyle: braid cards and down feathers cross by it too.
            notAnimatable({"crossCount", std::int64_t{2}, "Cards in a cross (crossStyle, a braid's cards, a down feather's): 2 "
                                                          "an X, 3 a star, up to 6"})
                .range(2, 6),
            notAnimatable({"braids", true, "Braided strands (a Braid node's braidId channel) make one card set per braid: "
                                           "cards crossed along the braid, a cornrow one card lying on the scalp; off, they "
                                           "are carded by their clumps"})
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"segmentsByLength", false, "Segments in proportion to each card's length: `segments` for the longest, "
                                                      "fewer for short ones (the UVs don't change)"})
                .in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            notAnimatable({"minSegments", std::int64_t{2}, "With segmentsByLength: the fewest segments a card gets"})
                .range(1, 256)
                .soft(1, 16)
                .in("Shape")
                .when("segmentsByLength", {"true"}),
            ParamSpec{"volumeNormals", true, "Shade cards like the hair volume (normals out from the scalp) rather than "
                                             "as flat planes"}.in("Shape")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"featherMode", std::string("auto"),
                      "Feathers (a Propagate node's barbs): auto cards each feather as one strip along its shaft spanning "
                      "both vanes (down feathers crossed cards) and the rest as hair; perFeather cards only the feathers; "
                      "perVane cards everything as hair (a card per vane or split); off cards only the hair", "",
                      {"auto", "perFeather", "perVane", "off"}}
                .in("Feathers"),
            notAnimatable({"featherSegments", std::int64_t{6}, "Quads along each feather's card (it bends with the shaft)"})
                .range(1, 256)
                .soft(1, 16)
                .in("Feathers")
                .when("featherMode", {"auto", "perFeather"}),
            notAnimatable({"featherMinLength", 0.0, "Feathers shorter than this share of the longest make no card (drop small down "
                                                    "feathers at low LODs)", "fraction"})
                .range(0, 1)
                .in("Feathers")
                .when("featherMode", {"auto", "perFeather"}),
            notAnimatable({"featherSpine", true, "A vertex column down each feather card's shaft, each half folding to follow its "
                                                 "vane (lift, droop, cupping): two quads per segment. Off: a flat strip"})
                .in("Feathers")
                .when("featherMode", {"auto", "perFeather"}),
            ParamSpec{"separation", 0.0, "Cards closer than this gap to another card, or passing through it, are pushed apart "
                                         "along their normals, roots held (a share of the narrower card's width; 0: off). "
                                         "Only the mesh moves: the texture stays as baked", "fraction"}
                .range(0, 1)
                .soft(0, 0.2)
                .in("Separation")
                .when("featherMode", {"auto", "perVane", "off"}),
            ParamSpec{"featherSeparation", 0.05, "Feather cards closer than this gap to another card, or passing through it, are "
                                                 "pushed apart along their normals, roots held, so overlapping feathers lie on each "
                                                 "other instead of clipping (a share of the narrower card's width; 0: off). Only the "
                                                 "mesh moves: the texture stays as baked", "fraction"}
                .range(0, 1)
                .soft(0, 0.2)
                .in("Separation")
                .when("featherMode", {"auto", "perFeather"}),
            notAnimatable({"separationIterations", std::int64_t{8}, "Passes of the cards' separation (separation, "
                                                                    "featherSeparation): more settle crowded cards further"})
                .range(1, 64)
                .soft(1, 16)
                .in("Separation"),
            notAnimatable({"atlasSize", std::int64_t{2048}, "Atlas resolution to bake (pixels); several Cards nodes share one "
                                                            "atlas and bake, at the largest of theirs", "pixels"})
                .range(16, 16384)
                .soft(256, 8192)
                .in("Texture"),
            notAnimatable({"thinnestStrand", 1.0, "The narrowest a strand bakes, in the atlas's pixels, solid: 1 keeps every strand at "
                                                  "least a pixel wide, so cards cut out at a cutoff (alpha tested) don't break up, though they "
                                                  "read denser than the groom; 0 bakes the strands to scale, a thinner one fainter (as "
                                                  "renders and the viewport's width coverage show them), for blended or dithered cards. "
                                                  "Several Cards nodes share one bake, at the largest of theirs", "pixels"})
                .range(0, 8)
                .soft(0, 2)
                .in("Texture"),
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::uint64_t contextSalt(const EvalContext& ctx) const override { return ctx.buildCards ? 0xCA5D5u : 0u; }
    // Its cards carry its name (CardMesh::nodes: the baked atlas and the stale check go by it).
    std::uint64_t cacheSalt() const override { return hashString(hash64(0xCA4Du), nodePath()); }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kCards);
        const std::string channel = params().getString("clumpChannel");
        if (!channel.empty() && !in->empty() && !in->strandChannel(channel)) {
            ctx.note("the strands have no '" + channel + "' channel (put a Clump before it): the cards group them by "
                     "root instead");
        }
        if (!ctx.buildCards) return in;
        CardSettings s;
        s.channel = channel;
        s.cardsPerClump = static_cast<int>(params().getInt("cardsPerClump"));
        s.layers = static_cast<int>(params().getInt("layers"));
        s.segments = static_cast<int>(params().getInt("segments"));
        s.widthSegments = static_cast<int>(params().getInt("widthSegments"));
        s.widthScale = static_cast<float>(params().getDouble("widthScale"));
        s.widthProfile = params().getRamp("widthProfile");
        s.margin = static_cast<float>(params().getDouble("margin"));
        s.fitTwist = static_cast<float>(params().getDouble("fitTwist"));
        s.twist = static_cast<float>(params().getDouble("twist"));
        s.curvature = static_cast<float>(params().getDouble("curvature"));
        s.ruffle = static_cast<float>(params().getDouble("ruffle"));
        s.minStrands = static_cast<int>(params().getInt("minStrands"));
        s.flyawayCards = static_cast<int>(params().getInt("flyawayCards"));
        s.minWidth = static_cast<float>(params().getDouble("minWidth"));
        s.maxCards = static_cast<int>(params().getInt("maxCards"));
        s.variations = static_cast<int>(params().getInt("variations"));
        s.mirror = params().getBool("mirror");
        s.volumeNormals = params().getBool("volumeNormals");
        const std::string style = params().getString("crossStyle");
        s.crossStyle = style == "single" ? CardSettings::CrossStyle::Single
                       : style == "cross" ? CardSettings::CrossStyle::Cross
                                          : CardSettings::CrossStyle::Auto;
        s.crossCount = static_cast<int>(params().getInt("crossCount"));
        s.braids = params().getBool("braids");
        s.segmentsByLength = params().getBool("segmentsByLength");
        s.minSegments = static_cast<int>(params().getInt("minSegments"));
        const std::string featherMode = params().getString("featherMode");
        s.featherMode = featherMode == "perFeather" ? CardSettings::FeatherMode::PerFeather
                        : featherMode == "perVane"  ? CardSettings::FeatherMode::PerVane
                        : featherMode == "off"      ? CardSettings::FeatherMode::Off
                                                    : CardSettings::FeatherMode::Auto;
        s.featherSegments = static_cast<int>(params().getInt("featherSegments"));
        s.featherMinLength = static_cast<float>(params().getDouble("featherMinLength"));
        s.featherSpine = params().getBool("featherSpine");
        s.separation = static_cast<float>(params().getDouble("separation"));
        s.featherSeparation = static_cast<float>(params().getDouble("featherSeparation"));
        s.separationIterations = static_cast<int>(params().getInt("separationIterations"));
        // Only the feathers (perFeather) or only the hair (off): the cards are made from those
        // strands alone, so what the others do never makes this node's bake stale.
        StrandsPtr source = in;
        if (s.featherMode == CardSettings::FeatherMode::PerFeather || s.featherMode == CardSettings::FeatherMode::Off) {
            const std::vector<float>* ids = in->strandChannel("featherId");
            const bool feathers = s.featherMode == CardSettings::FeatherMode::PerFeather;
            std::vector<std::size_t> keep;
            for (std::size_t i = 0; i < in->strandCount(); ++i) {
                if ((ids && (*ids)[i] >= 0.0f) == feathers) keep.push_back(i);
            }
            if (keep.size() != in->strandCount()) source = std::make_shared<StrandSet>(in->subset(keep));
        }
        auto layer = std::make_shared<CardMesh>();
        layer->nodes = {nodePath()};  // every card: node 0
        layer->cards = makeCards(*source, scalpOf(ctx), s);
        for (CardMesh::Card& card : layer->cards) card.source = source;
        layer->atlasSize = static_cast<int>(params().getInt("atlasSize"));
        layer->thinnestStrand = static_cast<float>(params().getDouble("thinnestStrand"));
        auto out = std::make_shared<StrandSet>(*in);
        out->addCardLayer(std::move(layer));
        return StrandsPtr(out);
    }
};

}  // namespace

bool flowState(const Operator& op, const StrandsPtr& output, FlowState* state) {
    const auto* flow = dynamic_cast<const FlowOp*>(&op);
    return flow && flow->keptState(output, state);
}

bool tangleState(const Operator& op, const StrandsPtr& output, std::shared_ptr<const TangleState>* state) {
    const auto* tangle = dynamic_cast<const TangleOp*>(&op);
    return tangle && tangle->keptState(output, state);
}

void registerCardsOperator(OperatorRegistry& registry) {
    registry.registerType(op::kCards,
                          {"games", "Hair cards for game engines, with baked maps and LODs.", "game unreal unity cards lod"},
                          [] { return std::make_unique<CardsOp>(); });
}

void registerFlowOperator(OperatorRegistry& registry) {
    registry.registerType(op::kFlow,
                          {"shape",
                           "Lays short hair along the flow arrows on the scalp. The arrows are kept on the groom, laid with "
                           "the Flow tool (Groom Tools > Create > Flow); GroomLab > Utilities > Delete All Flow Arrows "
                           "removes them.",
                           "arrows comb short hair direction"},
                          [] { return std::make_unique<FlowOp>(); });
}

void registerFlyawayOperator(OperatorRegistry& registry) {
    registry.registerType(op::kFlyaway,
                          {"look", "Stray hairs leaving the flow. After Clump.", "strays frizz loose"},
                          [] { return std::make_unique<FlyawayOp>(); });
}

void registerTangleOperator(OperatorRegistry& registry) {
    registry.registerType(op::kTangle,
                          {"look", "Strands stuck together: mats and wet hair.", "mats matted stuck"},
                          [] { return std::make_unique<TangleOp>(); });
}

}  // namespace groomlab
