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

constexpr float kRadians = 3.14159265358979f / 180.0f;

// The frame at a strand's root: the scalp's normal there (else the strand's first segment), a
// tangent (the scalp's U direction from its UVs when `byUV`, else `reference` laid onto the
// tangent plane, else any perpendicular) and the bitangent across them (tangent x normal).
struct RootAxes {
    Vec3f tangent{1.0f, 0.0f, 0.0f}, normal{0.0f, 1.0f, 0.0f}, bitangent{0.0f, 0.0f, 1.0f};
    bool onScalp = false;
};

// The direction U grows in across face `face` of a scalp with UVs (zero where the UVs give none).
Vec3f uvTangent(const ScalpMesh& scalp, std::int32_t face) {
    const auto& tri = scalp.triangles()[static_cast<std::size_t>(face)];
    const auto& uv = scalp.cornerUVs();
    const std::size_t c = static_cast<std::size_t>(face) * 3;
    if (c + 2 >= uv.size()) return {};
    const Vec3f a = scalp.points()[static_cast<std::size_t>(tri[0])];
    const Vec3f e1 = scalp.points()[static_cast<std::size_t>(tri[1])] - a;
    const Vec3f e2 = scalp.points()[static_cast<std::size_t>(tri[2])] - a;
    const float du1 = uv[c + 1][0] - uv[c][0], dv1 = uv[c + 1][1] - uv[c][1];
    const float du2 = uv[c + 2][0] - uv[c][0], dv2 = uv[c + 2][1] - uv[c][1];
    const float det = du1 * dv2 - du2 * dv1;
    if (!(std::fabs(det) > 1e-12f)) return {};
    return (e1 * dv2 - e2 * dv1) / det;
}

// The first segment of a strand with any length, as a unit vector (zero for none).
Vec3f firstDirection(const Vec3f* pts, std::size_t n) {
    for (std::size_t i = 1; i < n; ++i) {
        const Vec3f d = pts[i] - pts[0];
        if (d.length() > 1e-12f) return d.normalized();
    }
    return {};
}

RootAxes rootAxes(const ScalpMesh* scalp, const StrandRoot& root, const Vec3f* pts, std::size_t n, const Vec3f& reference,
                  bool byUV) {
    RootAxes a;
    if (scalp && scalp->contains(root)) {
        a.normal = scalp->normal(root).normalized();
        a.onScalp = true;
    } else {
        a.normal = firstDirection(pts, n);
    }
    if (a.normal.length() < 0.5f) a.normal = {0.0f, 1.0f, 0.0f};
    auto laid = [&](const Vec3f& v) { return v - a.normal * v.dot(a.normal); };
    Vec3f t;
    if (byUV && a.onScalp && scalp->hasUVs()) t = laid(uvTangent(*scalp, root.face));
    if (!(t.length() > 1e-6f)) t = laid(reference);
    if (!(t.length() > 1e-6f)) t = anyPerpendicular(a.normal);
    a.tangent = t.normalized();
    a.bitangent = a.tangent.cross(a.normal);
    return a;
}

// A value in [-1, 1) of strand `id`, draw `k`, per seed and node kind.
float signedRandom(StrandId id, std::uint64_t seed, std::uint64_t salt, std::uint64_t k) {
    return 2.0f * unitFloat(hashCombine(hashCombine(id, seed ^ salt), k)) - 1.0f;
}

// The run every shaper here shares: in full, or (with `incremental`) only the strands the input
// changed in, into the last result's copy; false when that cannot be done. `identity`: the strands
// pass through as they came (nothing to do). `local`: every mapped value reads each strand alone.
// shape(out, s) reshapes strand s of `out` (its incoming points there already). `engine`: the
// operator's own stage, run on the CPU engine (look_cpu.h) for the strands an incremental evaluation
// makes again (a full one runs it before).
struct EngineStage {
    gpu::StageKind kind;
    const ParamSet& params;
    const ScalpMesh* scalp;
    // Each strand's direction (StageInputs::directions) of strand s, where the node takes one per
    // strand (Bend's vector field); null: the param's one.
    std::function<Vec3f(std::size_t)> direction = nullptr;
    // Its stage on the CPU engine for the strands `which` (all: null) of `in`, into `out`.
    bool run(const StrandsPtr& in, const std::vector<std::uint32_t>* which, StrandSet* out, StrandsPtr* made) const {
        gpu::StageParamReader reader(params, "");
        gpu::StageInputs inputs;
        if (direction) {
            reader.directions = true;
            auto ways = std::make_shared<std::vector<Vec3f>>(which ? which->size() : in->strandCount());
            parallelFor(0, ways->size(), [&](std::size_t k) { (*ways)[k] = direction(which ? (*which)[k] : k); }, 4096);
            inputs.directions = std::move(ways);
        }
        if (which) return gpu::runStageOnStrands(kind, reader, in, *which, scalp, *out, std::move(inputs));
        *made = gpu::runStage(kind, reader, in, scalp, std::move(inputs));
        return *made != nullptr;
    }
};
template <class Shape>
bool reshapeEach(const StrandsPtr& in, bool identity, bool local, const IncrementalInputs* incremental, bool otherInputsSame,
                 Value* result, StrandChanges* changed, const Shape& shape, const EngineStage* engine = nullptr) {
    if (identity || in->empty()) {
        if (incremental) return false;  // the input handed on: the graph passes its changes on
        *result = in;
        return true;
    }
    const StrandChanges* changes = nullptr;
    StrandsPtr last;  // the last result, to make again only the strands the input changed in
    if (incremental) {
        changes = someStrandsChanged(*incremental, 0);
        last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
        if (!last || !local || !otherInputsSame) return false;
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
    if (last) {
        if (!(engine && gpu::cpuEngineOn() && engine->run(in, &changes->strands, out.get(), nullptr))) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                shape(*out, s);
            });
        }
        *changed = *changes;
    } else {
        parallelFor(0, out->strandCount(), [&](std::size_t s) { shape(*out, s); });
    }
    *result = StrandsPtr(out);
    return true;
}

// A 3x3 linear map as its three columns: the images of x, y and z.
struct Linear {
    Vec3f x{1.0f, 0.0f, 0.0f}, y{0.0f, 1.0f, 0.0f}, z{0.0f, 0.0f, 1.0f};
    Vec3f operator()(const Vec3f& v) const { return x * v.x + y * v.y + z * v.z; }
};

// Turns of `degrees` about axes a, b and c in that order (each about the fixed axis, not a turned one).
Linear turnsAbout(const Vec3f& degrees, const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    auto turn = [&](Vec3f v) {
        if (degrees.x != 0.0f) v = rotateAxisAngle(v, a, degrees.x * kRadians);
        if (degrees.y != 0.0f) v = rotateAxisAngle(v, b, degrees.y * kRadians);
        if (degrees.z != 0.0f) v = rotateAxisAngle(v, c, degrees.z * kRadians);
        return v;
    };
    Linear m;
    m.x = turn({1.0f, 0.0f, 0.0f});
    m.y = turn({0.0f, 1.0f, 0.0f});
    m.z = turn({0.0f, 0.0f, 1.0f});
    return m;
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
        const bool world = params().getString("space") == "world";
        const Vec3f translate = params().getVec3("translate");
        const Vec3f rotate = params().getVec3("rotate");
        const Vec3f pivot = params().getVec3("pivot");
        const Vec3f translateRandom = params().getVec3("translateRandom");
        const Vec3f rotateRandom = params().getVec3("rotateRandom");
        const auto scaleRandom = static_cast<float>(params().getDouble("scaleRandom"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const Vec3f zero;
        const bool randomMove = translateRandom != zero, randomTurn = rotateRandom != zero;
        const bool identity = translate == zero && rotate == zero && !randomMove && !randomTurn && scaleRandom <= 0.0f &&
                              scale.isConstant() && scale.maximum() == 1.0f;
        // One map for every strand when nothing varies per strand (the whole set, no randomness).
        const Linear fixedTurn = turnsAbout(rotate, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        auto shape = [&](StrandSet& out, std::size_t s) {
            const std::size_t n = out.strandPointCount(s);
            if (n == 0) return;
            float k = scale.at(out, s);
            const StrandId id = out.ids()[s];
            constexpr std::uint64_t kSalt = 0x7A45F0ull;
            Vec3f move = translate, turn = rotate;
            if (randomMove) {
                move += Vec3f{translateRandom.x * signedRandom(id, seed, kSalt, 0), translateRandom.y * signedRandom(id, seed, kSalt, 1),
                              translateRandom.z * signedRandom(id, seed, kSalt, 2)};
            }
            if (randomTurn) {
                turn += Vec3f{rotateRandom.x * signedRandom(id, seed, kSalt, 3), rotateRandom.y * signedRandom(id, seed, kSalt, 4),
                              rotateRandom.z * signedRandom(id, seed, kSalt, 5)};
            }
            if (scaleRandom > 0.0f) k *= std::max(0.0f, 1.0f + scaleRandom * signedRandom(id, seed, kSalt, 6));
            Vec3f* pts = out.strandPoints(s);
            Vec3f origin = pivot, offset = move;
            Linear m = fixedTurn;
            if (world) {
                if (randomTurn) m = turnsAbout(turn, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            } else {
                const RootAxes a = rootAxes(scalp, out.roots()[s], pts, n, {0.0f, -1.0f, 0.0f}, true);
                origin = pts[0];
                offset = a.tangent * move.x + a.normal * move.y + a.bitangent * move.z;
                m = turnsAbout(turn, a.tangent, a.normal, a.bitangent);
            }
            for (std::size_t i = 0; i < n; ++i) {
                pts[i] = origin + m((pts[i] - origin) * k) + offset;
            }
        };
        if (!incremental && !identity && gpu::cpuEngineOn()) {  // look.slang's, the GPU preview's own (look_cpu.h)
            if (StrandsPtr made = gpu::runStage(gpu::StageKind::Transform, params(), in, scalp)) {
                *result = made;
                return true;
            }
        }
        const EngineStage engine{gpu::StageKind::Transform, params(), scalp};
        return reshapeEach(in, identity, scale.strandLocal(), incremental, true, result, changed, shape, &engine);
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
        const auto start = static_cast<float>(params().getDouble("start"));
        const auto end = static_cast<float>(params().getDouble("end"));
        const std::string along = params().getString("along");
        const bool alongNormal = along == "normal", alongDirection = along == "direction";
        const auto randomness = static_cast<float>(params().getDouble("randomness"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool identity = angle.isConstant() && angle.maximum() == 0.0f;
        // The share of the bend made by t: 0 before start, 1 past end, even between.
        auto share = [&](float t) {
            if (end > start) return std::clamp((t - start) / (end - start), 0.0f, 1.0f);
            return t >= start ? 1.0f : 0.0f;
        };

        auto shape = [&](StrandSet& out, std::size_t s) {
            const std::size_t n = out.strandPointCount(s);
            if (n < 2) return;
            float degrees = angle.at(out, s);
            if (randomness > 0.0f) degrees *= 1.0f - randomness * unitFloat(hashCombine(out.ids()[s], seed ^ 0xBE4Dull));
            if (degrees == 0.0f) return;
            const Vec3f way = direction.at(out, s);
            Vec3f* pts = out.strandPoints(s);
            const Vec3f chord = (pts[n - 1] - pts[0]).normalized();
            if (chord.length() < 0.5f) return;
            const RootAxes a = rootAxes(scalp, out.roots()[s], pts, n, way, true);
            Vec3f target = a.normal;
            if (alongDirection) {
                // Laid onto the scalp at the root (as given off it); none when it stands along the normal.
                target = a.onScalp ? way - a.normal * way.dot(a.normal) : way;
                if (!(target.length() > 1e-6f * way.length())) return;
                target = target.normalized();
            } else if (!alongNormal) {
                target = a.tangent;
            }
            if (target.length() < 0.5f) return;
            Vec3f axis = chord.cross(target);
            if (axis.length() < 1e-5f) {
                if (!alongNormal || chord.dot(target) < 0.0f) return;  // along it already
                // Standing straight up, it can only bend over: along the root frame's x (the
                // scalp's U), by the angle's size whatever its sign.
                axis = a.bitangent;
                degrees = -std::fabs(degrees);
            }
            axis = axis.normalized();
            // Up to pointing along the target (the chord's angle from it), and away no further than
            // its opposite (or, along the normal, the scalp's tangent plane: that angle less 90
            // degrees, when above it).
            const float current = pm::acos(std::clamp(chord.dot(target), -1.0f, 1.0f)) / kRadians;
            degrees = std::clamp(degrees, alongNormal ? std::min(0.0f, current - 90.0f) : current - 180.0f, current);
            if (degrees == 0.0f) return;
            // Each segment, from the root out, turned by the bend reached at its end point.
            Vec3f previous = pts[0], bent = pts[0];
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f segment = pts[i] - previous;
                previous = pts[i];
                const float turn = degrees * kRadians * share(paramAt(i, n));
                bent = bent + (turn == 0.0f ? segment : rotateAxisAngle(segment, axis, turn));
                pts[i] = bent;
            }
        };
        // (A direction per strand: each strand's, as `shape` reads it.)
        EngineStage engine{gpu::StageKind::Bend, params(), scalp};
        if (!direction.isConstant()) engine.direction = [&](std::size_t s) { return direction.at(*in, s); };
        if (!incremental && !identity && gpu::cpuEngineOn()) {  // look.slang's, the GPU preview's own (look_cpu.h)
            StrandsPtr made;
            if (engine.run(in, nullptr, nullptr, &made)) {
                *result = made;
                return true;
            }
        }
        return reshapeEach(in, identity, angle.strandLocal() && direction.strandLocal(), incremental, true, result, changed,
                           shape, &engine);
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
            {"along", std::string("tangent"),
             "Where roll 0 points: tangent: the scalp's U direction (its UVs; without them the direction below); "
             "direction: the direction below, laid onto the scalp", "", {"tangent", "direction"}},
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
        const bool byUV = params().getString("along") == "tangent";
        const Vec3f direction = params().getVec3("direction");
        const auto inclinationRandom = static_cast<float>(params().getDouble("inclinationRandom"));
        const auto rollRandom = static_cast<float>(params().getDouble("rollRandom"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool identity = strength.isConstant() && strength.maximum() <= 0.0f;

        auto shape = [&](StrandSet& out, std::size_t s) {
            const std::size_t n = out.strandPointCount(s);
            if (n < 2 || !(scalp && scalp->contains(out.roots()[s]))) return;
            const float w = std::min(strength.at(out, s), 1.0f);
            if (!(w > 0.0f)) return;
            Vec3f* pts = out.strandPoints(s);
            const Vec3f current = firstDirection(pts, n);
            if (current.length() < 0.5f) return;
            const StrandId id = out.ids()[s];
            constexpr std::uint64_t kSalt = 0xD12EC7ull;
            float incline = inclination.at(out, s), turn = roll.at(out, s);
            if (inclinationRandom > 0.0f) incline += inclinationRandom * signedRandom(id, seed, kSalt, 0);
            if (rollRandom > 0.0f) turn += rollRandom * signedRandom(id, seed, kSalt, 1);
            const RootAxes a = rootAxes(scalp, out.roots()[s], pts, n, direction, byUV);
            const float i = incline * kRadians, r = turn * kRadians;
            const Vec3f target = a.normal * pm::cos(i) + (a.tangent * pm::cos(r) + a.bitangent * pm::sin(r)) * pm::sin(i);
            const float c = std::clamp(current.dot(target), -1.0f, 1.0f);
            Vec3f axis = current.cross(target);
            if (axis.length() < 1e-6f) {
                if (c > 0.0f) return;  // already there
                axis = anyPerpendicular(current);
            }
            axis = axis.normalized();
            const float angle = pm::acos(c) * w;
            for (std::size_t k = 1; k < n; ++k) pts[k] = pts[0] + rotateAxisAngle(pts[k] - pts[0], axis, angle);
        };
        if (!incremental && !identity && gpu::cpuEngineOn()) {  // look.slang's, the GPU preview's own (look_cpu.h)
            if (StrandsPtr made = gpu::runStage(gpu::StageKind::Direction, params(), in, scalp)) {
                *result = made;
                return true;
            }
        }
        const EngineStage engine{gpu::StageKind::Direction, params(), scalp};
        return reshapeEach(in, identity, inclination.strandLocal() && roll.strandLocal() && strength.strandLocal(), incremental, true,
                           result, changed, shape, &engine);
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
