// SPDX-License-Identifier: Apache-2.0
// Rigid and bending shapers, as Yeti has them: Transform (strands moved, turned and scaled, each
// about its root in its root's frame, or the whole set about a pivot), Bend (strands bent toward
// or away from a direction between a start and an end along their length, never past it; along
// the normal with start and end 0, strands stood up or laid down at the root; toward a direction
// laid onto the scalp, combed) and Direction (each
// strand turned about its root to a direction given by its inclination from the scalp's normal
// and its roll around it).
//
// Every one works on each strand alone: random choices hash the stable strand ID with the seed
// (never thread or strand order), and a dab re-shapes only the strands it moved (incremental).

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "groomlab/field.h"
#include "groomlab/frames.h"
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

// The operator's own stage on the CPU engine (look_cpu.h; look.slang's, the GPU preview's own).
struct EngineStage {
    gpu::StageKind kind;
    const ParamSet& params;
    const ScalpMesh* scalp;
    // Each strand's direction (StageInputs::directions) of strand s, where the node takes one per
    // strand (Bend's vector field; Direction's three mapped params); null: the params'.
    std::function<Vec3f(std::size_t)> direction = nullptr;
    // Its stage for the strands `which` of `in`, into `out` (all of them: null, the result made).
    StrandsPtr run(const StrandsPtr& in, const std::vector<std::uint32_t>* which, StrandSet* out) const {
        gpu::StageParamReader reader(params, "");
        gpu::StageInputs inputs;
        if (direction) {
            reader.directions = true;
            auto ways = std::make_shared<std::vector<Vec3f>>(which ? which->size() : in->strandCount());
            parallelFor(0, ways->size(), [&](std::size_t k) { (*ways)[k] = direction(which ? (*which)[k] : k); }, 4096);
            inputs.directions = std::move(ways);
        }
        if (!which) return gpu::runStage(kind, reader, in, scalp, std::move(inputs));
        gpu::runStageOnStrands(kind, reader, in, *which, scalp, *out, std::move(inputs));
        return nullptr;
    }
};
// The run every shaper here shares: in full, or (with `incremental`) only the strands the input
// changed in, into the last result's copy; false when that cannot be done. `identity`: the strands
// pass through as they came (nothing to do). `local`: every mapped value reads each strand alone.
bool reshapeEach(const StrandsPtr& in, bool identity, bool local, const IncrementalInputs* incremental, bool otherInputsSame,
                 Value* result, StrandChanges* changed, const EngineStage& engine) {
    if (identity || in->empty()) {
        if (incremental) return false;  // the input handed on: the graph passes its changes on
        *result = in;
        return true;
    }
    if (!incremental) {
        *result = engine.run(in, nullptr, nullptr);
        return true;
    }
    const StrandChanges* changes = someStrandsChanged(*incremental, 0);
    const StrandsPtr last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
    if (!last || !local || !otherInputsSame) return false;
    if (changes->none()) {  // the same strands in: the same result
        *result = last;
        *changed = *changes;
        return true;
    }
    auto out = std::make_shared<StrandSet>(*in);
    positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
    engine.run(in, &changes->strands, out.get());
    *changed = *changes;
    *result = StrandsPtr(out);
    return true;
}

// Transform: each strand moved, turned and scaled about its root, in its root's frame (x along
// the scalp's U direction, y its normal, z across them), with random amounts per strand; or the
// whole set about `pivot` in world axes (affect blends each strand toward its transformed self).
class TransformOp : public Operator {
public:
    std::string typeName() const override { return op::kTransform; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"space", std::string("strand"),
             "strand: each strand about its own root, in its root's frame (x along the scalp's U, y its normal, z "
             "across); world: the whole set about the pivot, in world axes", "", {"strand", "world"}},
            ParamSpec{"translate", Vec3f{0.0f, 0.0f, 0.0f}, "Move, scene units: along the root's frame (strand) or world axes "
                                                   "(world)", "length"},
            {"rotate", Vec3f{0.0f, 0.0f, 0.0f}, "Turn, degrees about x, then y, then z: the root's frame's axes (strand) or "
                                                "the world's through the pivot (world)"},
            ParamSpec{"scale", 1.0, "Uniform scale about the root (strand) or the pivot (world) (mappable)"}.atLeast(0).soft(0, 2),
            ParamSpec{"pivot", Vec3f{0.0f, 0.0f, 0.0f}, "World: the point the set turns and scales about", "length"}
                .world()
                .when("space", {"world"}),
            ParamSpec{"translateRandom", Vec3f{0.0f, 0.0f, 0.0f}, "Up to this much more or less move per strand, per axis, scene "
                                                         "units", "length"}
                .in("Random"),
            ParamSpec{"rotateRandom", Vec3f{0.0f, 0.0f, 0.0f}, "Up to this many degrees more or less turn per strand, per axis"}
                .in("Random"),
            ParamSpec{"scaleRandom", 0.0, "Each strand's scale up to this share more or less, at random", "fraction"}
                .range(0, 1)
                .in("Random"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Random").adv(),
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
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kTransform);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField scale(params(), "scale", scalp);
        scale.validate(*in);
        const Vec3f translate = params().getVec3("translate");
        const Vec3f rotate = params().getVec3("rotate");
        const Vec3f translateRandom = params().getVec3("translateRandom");
        const Vec3f rotateRandom = params().getVec3("rotateRandom");
        const auto scaleRandom = static_cast<float>(params().getDouble("scaleRandom"));
        const Vec3f zero;
        const bool randomMove = translateRandom != zero, randomTurn = rotateRandom != zero;
        const bool identity = translate == zero && rotate == zero && !randomMove && !randomTurn && scaleRandom <= 0.0f &&
                              scale.isConstant() && scale.maximum() == 1.0f;
        const EngineStage engine{gpu::StageKind::Transform, params(), scalp};
        return reshapeEach(in, identity, scale.strandLocal(), incremental, true, result, changed, engine);
    }
};

// Bend: each strand bent about its root toward a direction (`along` the scalp's normal at the root,
// its U direction, or a `direction` laid onto the scalp there): its direction at t turns by angle x
// the share of [start, end] behind t, so the bend is spread evenly between the two and the strand
// holds its shape past the end. A negative angle bends away from the direction (with the normal:
// down onto the scalp). A strand never turns (root to tip) past the direction it bends toward, nor
// past its opposite bending away; along the normal not below the scalp's tangent plane either. So
// a big angle with start and end 0 points each strand along the direction, as a comb lays it
// (angle 180: the whole way; scales pointed down a body), and start and end 0 along the normal
// stand strands up for volume or lay them down. A strand standing straight along the normal bends
// over along its root frame's x (the scalp's U).
class BendOp : public Operator {
public:
    std::string typeName() const override { return op::kBend; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"angle", 30.0, "Degrees the strands bend between start and end (mappable), never past the direction; "
                                     "positive toward it (with the normal: away from the scalp), negative away from it "
                                     "(down onto the scalp); 180 with start and end 0 points them along it",
                      "angle"}
                .soft(-180, 180),
            ParamSpec{"start", 0.0, "Where the bend starts along the length, 0 root to 1 tip", "fraction"}.range(0, 1),
            ParamSpec{"end", 1.0, "Where it ends: the strand turns evenly between start and end and holds its shape past it",
                      "fraction"}
                .range(0, 1),
            {"along", std::string("normal"),
             "normal: the scalp's normal at the root (never below the scalp's tangent plane); tangent: the scalp's U "
             "direction (its UVs; without them the direction below); direction: the direction below, laid onto the "
             "scalp at the root (as a comb lays hair)", "",
             {"normal", "tangent", "direction"}},
            ParamSpec{"direction", Vec3f{1.0f, 0.0f, 0.0f}, "The way strands bend (world space; or an expression giving a "
                                                            "vector per strand)"}
                .world()
                .vectorExpr()
                .when("along", {"tangent", "direction"}),
            ParamSpec{"randomness", 0.0, "Each strand's angle scaled down by up to this share, at random", "fraction"}.range(0, 1),
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
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kBend);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField angle(params(), "angle", scalp);
        angle.validate(*in);
        const VectorField direction(params(), "direction", scalp);
        direction.validate(*in);
        const bool identity = angle.isConstant() && angle.maximum() == 0.0f;
        // (A direction per strand: each strand's.)
        EngineStage engine{gpu::StageKind::Bend, params(), scalp};
        if (!direction.isConstant()) engine.direction = [&](std::size_t s) { return direction.at(*in, s); };
        return reshapeEach(in, identity, angle.strandLocal() && direction.strandLocal(), incremental, true, result, changed, engine);
    }
};

// Direction: each strand turned about its root so its first segment points along the direction
// its inclination (degrees from the scalp's normal: 0 straight out, 90 flat along the scalp) and
// roll (degrees around the normal from where roll 0 points, `along` the scalp's U direction or a
// world `direction` laid onto the scalp) give, both mappable, with random spreads per strand.
// `strength` turns part of the way. Strands not rooted on the scalp stay.
class DirectionOp : public Operator {
public:
    std::string typeName() const override { return op::kDirection; }
    std::string category() const override { return "shape"; }
    // 2: along direction by default (nodes saved before keep tangent: ParamSpec::formerDefault).
    int version() const override { return 2; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"inclination", 45.0, "Degrees from the scalp's normal at the root: 0 straight out, 90 flat along the "
                                           "scalp (mappable)", "angle"}
                .soft(0, 90),
            ParamSpec{"roll", 0.0, "Degrees around the normal from where roll 0 points (along) (mappable)", "angle"}
                .soft(-180, 180),
            ParamSpec{"strength", 1.0, "How far each strand turns to its direction, 0-1: that share of the turn (mappable)",
                      "fraction"}
                .soft(0, 1),
            ParamSpec{"along", std::string("direction"),
                      "Where roll 0 points: direction: the direction below, laid onto the scalp (no seams); tangent: the "
                      "scalp's U direction (its UVs, so a UV seam draws a line through the hair; without UVs the "
                      "direction below)", "", {"tangent", "direction"}}
                .formerDefault(2, std::string("tangent")),
            ParamSpec{"direction", Vec3f{0.0f, -1.0f, 0.0f}, "Where roll 0 points (world space), laid onto the scalp at each root"}
                .world(),
            ParamSpec{"inclinationRandom", 0.0, "Up to this many degrees more or less inclination per strand", "angle"}
                .atLeast(0)
                .soft(0, 45)
                .in("Random"),
            ParamSpec{"rollRandom", 0.0, "Up to this many degrees more or less roll per strand", "angle"}
                .atLeast(0)
                .soft(0, 180)
                .in("Random"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Random").adv(),
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
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kDirection);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField inclination(params(), "inclination", scalp);
        const ScalarField roll(params(), "roll", scalp);
        const ScalarField strength(params(), "strength", scalp);
        inclination.validate(*in);
        roll.validate(*in);
        strength.validate(*in);
        const bool identity = strength.isConstant() && strength.maximum() <= 0.0f;

        // (All three mapped: each strand's three.)
        EngineStage engine{gpu::StageKind::Direction, params(), scalp};
        if (gpu::directionMapped(params()).size() > 2) {
            engine.direction = [&](std::size_t s) { return Vec3f(inclination.at(*in, s), roll.at(*in, s), strength.at(*in, s)); };
        }
        return reshapeEach(in, identity, inclination.strandLocal() && roll.strandLocal() && strength.strandLocal(), incremental, true,
                           result, changed, engine);
    }
};

}  // namespace

void registerTransformOperator(OperatorRegistry& registry) {
    registry.registerType(op::kTransform,
                          {"deform",
                           "Moves, turns and scales strands about their roots (or the whole set), with randomness.",
                           "move rotate scale translate"},
                          [] { return std::make_unique<TransformOp>(); });
}

void registerBendOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBend,
                          {"deform",
                           "Bends strands toward or away from the scalp, or combs them toward a direction, between a start and an end; start and end 0 stand them up at the root.",
                           "comb curve lift lay"},
                          [] { return std::make_unique<BendOp>(); });
}

void registerDirectionOperator(OperatorRegistry& registry) {
    registry.registerType(op::kDirection,
                          {"shape",
                           "Points strands by inclination from the scalp and roll around its normal, at the root.",
                           "inclination roll angle orient"},
                          [] { return std::make_unique<DirectionOp>(); });
}

}  // namespace groomlab
