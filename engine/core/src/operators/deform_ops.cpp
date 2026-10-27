// SPDX-License-Identifier: Apache-2.0
// Whole-groom deformers: Collide (push strands out of the scalp and any other
// mesh), Part (open the parts), Gravity (bend strands under their weight) and
// Displace (offset them along the scalp's normal).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "groomlab/distance_field.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

// The parts found in `strands`' flow with a Part node's params (findAutoParts, parts.h); null
// when none.
PartsPtr autoPartLines(const ParamSet& params, const StrandSet& strands, const ScalpMesh& scalp) {
    const PartSet found = findAutoParts(strands, scalp, static_cast<float>(params.getDouble("splitAngle")),
                                        static_cast<float>(params.getDouble("minLength")));
    return found.empty() ? nullptr : std::make_shared<const PartLines>(found, scalp);
}

// Collide's softness: a push of `delta` up to the offset plus the softness `s` (|delta| = u: how far
// below that the point is), eased: a point within the band of 2s below that rises u^2 / 4s (at its
// top it rises nothing, the rise growing smoothly), one deeper rises to the offset itself (u - s).
// Its new height is a smooth maximum of its height and the offset, never below either (look.slang
// softPush, the same arithmetic).
Vec3f softPush(const Vec3f& delta, float s) {
    if (!(s > 0.0f)) return delta;
    const float u = delta.length();
    if (!(u > 0.0f)) return delta;
    const float f = u < 2.0f * s ? u / (4.0f * s) : (u - s) / u;
    return delta * f;
}

// Whether p is too far from the point the mesh's closest point query was last asked at (the
// distance to a mesh changes no faster than the point moves): its answer, a miss, the same.
bool collideBeyondBound(const CollideRestState& state, const Vec3f& p, float searchRadius) {
    if (!(state.bound > searchRadius)) return false;
    const float scale = std::max({std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
    const float room = 1e-4f * (searchRadius + state.bound + scale) + 1e-6f;
    return state.bound - (p - state.anchor).length() > searchRadius + room;
}

// Walks each strand from the root: keeps the segment length from the
// (already fixed) previous point, pushes the point out along the surface
// normal when it is less than `offset` above the surface, then re-keeps the
// length toward the pushed spot. Staying outside wins over exact length.
// Points deeper than maxDepth are left alone: on an open scalp (a head cap)
// they are on its far side, not inside it. The scalp and the optional
// `collider` (any mesh: a ColliderMesh node) are pushed against in turn at
// every point of the walk. With `method` field (the default) each mesh's question at a point (how
// far above the surface along the closest point's normal) is a lookup in its signed distance
// field (distance_field.h: made once per mesh and kept across evaluations), answered by the field
// alone (MeshDistanceField::Probe::fieldOnly): where its blend cannot tell (deep under the surface,
// across a crease, the search's edge) the closest point among the triangles around the closest
// faces it holds there, the mesh's own almost everywhere. Points land within a few percent of the
// offset of where the mesh's closest point puts them, and the GPU's Collide (look.slang, from the
// field's tables) walks the same. `exact` asks the mesh's closest-point tree at every point, the
// reference. A mesh deformed away from its rest pose (an animated scalp; a collider given one) is
// asked through its rest pose's field, made once for every frame: each point taken to rest space by
// its nearest vertex's frame (RestPoseCollision), the deformed mesh asked where that field cannot
// tell (the GPU leaves such a Collide to the CPU). A deforming collider without a rest pose gets a
// field per frame. `softness` eases the push in over a band above the offset (softPush) and
// `smoothing` spreads the pushes toward the root (smoothCorrections); with either the walk carries
// each segment along its own heading once a point moved (a strand meeting a surface slides along
// it), both 0 the walk above, as it always was.
class CollideOp : public Operator {
public:
    std::string typeName() const override { return op::kCollide; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Curves}, {"scalp", PortType::Mesh, true}, {"collider", PortType::Mesh, true}};
    }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"offset", 0.05, "Distance kept between strands and the surfaces", "length"}.soft(0, 0.5),
            ParamSpec{"maxDepth", 3.0, "Points deeper under a surface than this are not pushed", "length"}.soft(0, 10),
            ParamSpec{"softness", 0.0, "Height above the offset where the collider's push starts to ease in: strands curve away "
                                       "before they touch instead of kinking there (0: a hard push; the scalp's always is)", "length"}
                .atLeast(0)
                .soft(0, 1),
            ParamSpec{"smoothing", 0.0, "Length along the strand, toward the root, over which the collider's push fades out: "
                                        "the strand curves onto it and slides along it, its segments' lengths kept (0: none)",
                      "length"}
                .atLeast(0)
                .soft(0, 5),
            ParamSpec{"iterations", std::int64_t{2}, "Root-to-tip passes"}.range(1, 16).soft(1, 8).adv(),
            ParamSpec{"method", std::string("field"), "field: each surface's signed distance field alone, made once and kept "
                                                      "(fast, within a small fraction of the offset of exact; what the GPU "
                                                      "preview runs); exact: the closest point on the mesh at every point "
                                                      "(the CPU's alone)", "", {"field", "exact"}}
                .adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand is pushed against the meshes alone, never against other strands: with the same
    // meshes in (the scalp's and the collider's inputs unchanged: the same surfaces, as the Scalp
    // node's key is its surface), only the strands the input changed in are walked again.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext&, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kCollide);
        // The surfaces to stay out of: the scalp, then the collider.
        std::vector<MeshPtr> meshes;
        std::vector<char> colliders;  // per mesh: the collider input's (softness and smoothing act on it)
        for (std::size_t slot = 1; slot < 3 && slot < inputs.size(); ++slot) {
            const MeshPtr mesh = valueAs<MeshPtr>(inputs[slot]);
            if (mesh && mesh->triangleCount() > 0) {
                meshes.push_back(mesh);
                colliders.push_back(slot == 2 ? 1 : 0);
            }
        }
        if (meshes.empty()) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            *result = in;
            return true;
        }
        // Only widths or channels changed (a Width or HairColor edit before it): each strand is
        // walked by its points alone, never its widths or channels, so its last points stand.
        if (incremental && incremental->inputs.size() > 0 && incremental->inputs[0] && incremental->inputs[0]->pointsKept &&
            inputUnchanged(*incremental, 1) && inputUnchanged(*incremental, 2)) {
            if (const StrandsPtr last = previousStrandsFor(*incremental, *in)) {
                *result = withPointsOf(*in, *last);
                *changed = *incremental->inputs[0];
                return true;
            }
        }
        const auto offset = static_cast<float>(params().getDouble("offset"));
        const auto maxDepth = static_cast<float>(params().getDouble("maxDepth"));
        const auto softness = static_cast<float>(params().getDouble("softness"));
        // A soft push asks how far below `reach` (the offset plus the softness) a point is.
        const float reach = offset + softness;
        // Only points this close to the surface can need a push; the bound keeps far points cheap.
        const float searchRadius = std::max(reach, maxDepth) * 1.001f + 1e-6f;
        // The meshes' fields (shared across evaluations, by surface): the cell size from the mesh
        // and the offset, reaching past the search radius. A mesh deformed away from its rest
        // pose (an animated scalp, a collider with a rest pose) is a new mesh on every frame,
        // whose own field would be made for one evaluation: it is asked through its rest pose's
        // field, kept across frames (RestPoseCollision; the tree alone for a maxDepth too shallow
        // for that field's cells, where it would only ask). A deforming collider without a rest
        // pose has nothing to keep: each frame's mesh gets a field of its own.
        std::vector<DistanceFieldPtr> fields(meshes.size());
        std::vector<std::shared_ptr<const RestPoseCollision>> rests(meshes.size());
        if (params().getString("method") != "exact") {
            for (std::size_t m = 0; m < meshes.size(); ++m) {
                fields[m] = collisionField(meshes[m], offset, searchRadius);
                if (!fields[m]) rests[m] = RestPoseCollision::of(meshes[m], offset, maxDepth, searchRadius);
            }
        }

        // look.slang's CollideCS, the GPU preview's own (look_cpu.h), on its surfaces' fields (a
        // deformed mesh through the host, its rest pose's walk; method exact or a field without a grid:
        // the mesh itself; collideRestPush): in full, or the strands the input changed in.
        const bool exact = params().getString("method") == "exact";
        gpu::StageParamReader reader(params(), "");
        reader.exactHost = exact;
        std::uint32_t colliderBits = 0;
        for (std::size_t m = 0; m < colliders.size(); ++m) colliderBits |= colliders[m] ? 1u << m : 0u;
        std::string whyNot;
        const std::shared_ptr<gpu::CollideInputs> surfaces =
            gpu::collideInputs(meshes, colliderBits, fields, offset, softness, &whyNot, rests, maxDepth, searchRadius, exact);
        if (!surfaces) throw GraphError("Collide: " + whyNot);
        if (!incremental) {
            gpu::StageInputs all;
            all.collide = gpu::collideHostRoots(*surfaces, *in);
            *result = gpu::runStage(gpu::StageKind::Collide, reader, in, nullptr, std::move(all));
            return true;
        }
        const StrandChanges* changes = someStrandsChanged(*incremental, 0);
        const StrandsPtr last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
        if (!last || !inputUnchanged(*incremental, 1) || !inputUnchanged(*incremental, 2)) return false;
        if (changes->none()) {  // the same strands in: the same result
            *result = last;
            *changed = *changes;
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        gpu::StageInputs listed;  // (each strand's root: those listed, in that order)
        listed.collide = gpu::collideHostRoots(*surfaces, *in, &changes->strands);
        gpu::runStageOnStrands(gpu::StageKind::Collide, reader, in, changes->strands, nullptr, *out, std::move(listed));
        *changed = *changes;
        *result = StrandsPtr(out);
        return true;
    }
};

// Opens the parts: strands near a part lean away from it, on their own side,
// fading out over `width` (rest-pose distance from the line), keeping lengths.
// The parts are the groom's painted ones (the "parts" external), or with partsFrom auto the ones
// found where the strands it takes split on the scalp (findAutoParts, parts.h), opened the same
// way. Auto parts are found anew from the whole input on every evaluation (no incremental run): a
// brushed strand can move them. Interpolate and Clump upstream never see auto parts (they are
// found after them): their respectParts keeps to painted parts only. The lines found are kept with
// the result (operator_state.h autoPartState): the GPU preview opens those while a drag or a
// stroke upstream moves the strands, until the release evaluates here and finds them again.
class PartOp : public Operator {
public:
    std::string typeName() const override { return op::kPart; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"partsFrom", std::string("painted"),
                      "Where the parts come from: painted: the lines drawn with the Part tool; auto: found where the "
                      "hair it takes splits on the scalp, flowing away to both sides of a line (a parting combed into "
                      "the guides)",
                      "", {"painted", "auto"}},
            ParamSpec{"width", 1.5, "Distance from the part over which strands lean away (scene units)", "length"}
                .atLeast(1e-6)
                .soft(0.1, 5),
            ParamSpec{"strength", 1.0, "How far strands at the part turn away from it, 0-1 (mappable)", "fraction"}.range(0, 1),
            {"profile", Ramp::linear(0.0f, 1.0f), "Turn along the length, root to tip"},
            ParamSpec{"part", std::string(), "Only this part (empty: every part)"}.when("partsFrom", {"painted"}),
            ParamSpec{"splitAngle", 90.0, "How sharply the hair must split to make a part: the angle between its flow "
                                          "on the two sides of the line, degrees (lower finds softer partings)", "angle"}
                .range(1, 180)
                .when("partsFrom", {"auto"}),
            ParamSpec{"minLength", 0.15, "The shortest part kept, as a share of the scalp's size (the square root of its "
                                         "area): shorter splits, like a whorl's center, are left alone", "fraction"}
                .range(0, 1)
                .when("partsFrom", {"auto"}),
        };
    }
    std::vector<std::string> externalInputs() const override {
        if (params().getString("partsFrom") == "auto") return {"scalp"};
        return {"scalp", "parts"};
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

    // operator_state.h autoPartState.
    bool autoLines(const StrandsPtr& output, PartsPtr* lines) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (!output || !kept_.found || !sameOrShown(kept_.output.lock(), output)) return false;
        *lines = kept_.lines;
        return true;
    }
    // Masked: its lines answer for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (made && kept_.output.lock() == made) kept_.output = mixed;
    }

private:
    // The last result (by identity) and, made with partsFrom auto (`found`), the lines it found
    // (null: none), for a host opening them elsewhere (the GPU preview).
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        PartsPtr lines;
        bool found = false;
    };
    void remember(const Value& out, bool found, const PartsPtr& lines) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        kept_.output = valueAs<StrandsPtr>(out);
        kept_.lines = found ? lines : nullptr;
        kept_.found = found;
    }
    mutable std::mutex keptMutex_;
    mutable Kept kept_;

    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done. (The parts and the scalp are externals: the
    // same ones as the last result's, or the node's key would differ.)
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kPart);
        const ScalpMesh* scalp = scalpOf(ctx);
        const bool flow = params().getString("partsFrom") == "auto";
        if (flow && incremental) return false;  // found from every strand: made anew
        const PartsPtr parts = flow ? (scalp ? autoPartLines(params(), *in, *scalp) : nullptr)
                                    : valueAs<PartsPtr>(ctx.external("parts"));
        if (!parts || parts->empty() || !scalp) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            if (flow && scalp) {
                ctx.note("no parts found: the hair it takes splits nowhere by splitAngle over minLength; the strands pass through");
            } else if (!flow && (!parts || parts->empty())) {
                ctx.note("no parts drawn: draw some with the Part tool; the strands pass through");
            }
            *result = in;
            remember(*result, flow, nullptr);
            return true;
        }
        const float width = static_cast<float>(params().getDouble("width"));
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const std::string only = flow ? std::string() : params().getString("part");

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !strength.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                remember(*result, false, nullptr);  // (incremental runs are never auto)
                return true;
            }
        }
        // look.slang's PartCS, the GPU preview's own (look_cpu.h), on each strand's side of the parts:
        // in full, or the strands the input changed in, into the last result's copy.
        gpu::StageInputs sides;
        if (!last) {
            sides.part = gpu::partInputs(*in, parts.get(), scalp, only, width);
            *result = gpu::runStage(gpu::StageKind::Part, params(), in, scalp, std::move(sides));
            remember(*result, flow, parts);
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        sides.part = gpu::partInputs(*in, parts.get(), scalp, only, width, &changes->strands);
        gpu::runStageOnStrands(gpu::StageKind::Part, params(), in, changes->strands, scalp, *out, std::move(sides));
        *changed = *changes;
        *result = StrandsPtr(out);
        remember(*result, flow, parts);
        return true;
    }
};



// Strands bend from the root under their weight, toward `direction` (world -Y, or painted per
// root in a color set). Walking from the root, each segment keeps its own shape relative to the
// one before and turns further toward gravity by a share that grows with the hair hanging beyond
// it (the load: the share of the length past the segment) and with how soft the strand is there
// (1 - stiffness x stiffnessProfile, by arc length); the turns made above carry down the strand,
// so a curl or a wave hangs as a whole. The rate is per unit of the strand's length, so the shape
// does not depend on the point count. Fully stiff at the root (the profile's 1 there), strands
// leave the scalp in their own direction. Strands bent into the scalp: a Collide after it.
// Implemented from the behaviour artists know from Yeti's and Ornatrix's gravity nodes;
// nothing of theirs is used.
class GravityOp : public Operator {
public:
    std::string typeName() const override { return op::kGravity; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How far strands bend under their weight (mappable): 0 none, 1 hanging, more heavier"}
                .soft(0, 2),
            ParamSpec{"direction", Vec3f{0.0f, -1.0f, 0.0f}, "Gravity's direction (world space; or an expression "
                                                             "giving a vector per strand)"}
                .world()
                .vectorExpr(),
            {"directionColorSet", std::string(), "Scalp color set holding a painted direction per root (RGB 0-1 = XYZ "
                                                 "-1..1; empty: none)"},
            ParamSpec{"stiffness", 1.0, "Bend stiffness: how much strands resist bending, 0-1, times stiffnessProfile (1 "
                                        "there: not at all)", "fraction"}
                .range(0, 1),
            {"stiffnessProfile", Ramp{{0.0f, 1.0f}, {0.25f, 0.5f}, {1.0f, 0.5f}},
             "Stiffness along the length, root to tip (1 at the root: strands leave the scalp in their own direction)"},
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    // The painted direction's color set (the scalp's key leaves its color sets out).
    std::uint64_t contextSalt(const EvalContext& ctx) const override {
        return colorSetSalt(ctx, params().getString("directionColorSet"));
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand bends alone (against the scalp alone): only the strands the input changed in.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kGravity);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const VectorField worldDirection(params(), "direction", scalp);
        worldDirection.validate(*in);
        const std::string colorSet = params().getString("directionColorSet");
        const bool painted = !colorSet.empty() && scalp && scalp->hasColorSet(colorSet);

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !strength.strandLocal() || !worldDirection.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        // Each strand's way down (its field's, a painted color set's over it): unit, or zero.
        auto downOf = [&](std::size_t s) {
            Vec3f down = worldDirection.at(*in, s);
            const StrandRoot& root = in->roots()[s];
            if (painted && scalp->contains(root)) {
                const ScalpMesh::Color c = scalp->colorAt(colorSet, root);
                const Vec3f p{c[0] * 2.0f - 1.0f, c[1] * 2.0f - 1.0f, c[2] * 2.0f - 1.0f};
                if (p.length() > 1e-3f) down = p;
            }
            return down.length() < 1e-6f ? Vec3f{} : down.normalized();
        };
        // On the CPU engine (look.slang GravityCS): one way down, or each strand's
        // (StageInputs::directions; `which`: those strands alone, in that order).
        const bool perStrand = !worldDirection.isConstant() || !colorSet.empty();
        auto engineRun = [&](const std::vector<std::uint32_t>* which, gpu::StageInputs* inputs) {
            gpu::StageParamReader reader(params(), "");
            reader.directions = perStrand;
            if (perStrand) {
                auto downs = std::make_shared<std::vector<Vec3f>>(which ? which->size() : in->strandCount());
                parallelFor(0, downs->size(), [&](std::size_t k) { (*downs)[k] = downOf(which ? (*which)[k] : k); }, 4096);
                inputs->directions = std::move(downs);
            }
            return reader;
        };
        // look.slang's GravityCS, the GPU preview's own (look_cpu.h): in full, or the strands the
        // input changed in, into the last result's copy.
        gpu::StageInputs stage;
        if (!last) {
            const gpu::StageParamReader reader = engineRun(nullptr, &stage);
            *result = gpu::runStage(gpu::StageKind::Gravity, reader, in, scalp, std::move(stage));
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        const gpu::StageParamReader reader = engineRun(&changes->strands, &stage);
        gpu::runStageOnStrands(gpu::StageKind::Gravity, reader, in, changes->strands, scalp, *out, std::move(stage));
        *changed = *changes;
        *result = StrandsPtr(out);
        return true;
    }
};

// Offsets strands along the scalp's normal at their root (or a world direction) by `distance`
// (mappable, per point) times `profile` along the length; the root stays. Lifts hair off the
// scalp for volume, or pushes a region in.
class DisplaceOp : public Operator {
public:
    std::string typeName() const override { return op::kDisplace; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"distance", 0.1, "How far each point moves, scene units (mappable, per point; negative: the other way)",
                      "length"}
                .soft(-1, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {0.2f, 1.0f}, {1.0f, 1.0f}}, "Distance along the length, root to tip"},
            {"along", std::string("normal"), "normal: along the scalp's normal at the root; direction: along `direction`", "",
             {"normal", "direction"}},
            ParamSpec{"direction", Vec3f{0.0f, 1.0f, 0.0f}, "The way strands move (world space; unrooted strands too; or an "
                                                            "expression giving a vector per strand)"}
                .world()
                .vectorExpr(),
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
        const StrandsPtr in = requireStrands(inputs, op::kDisplace);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField amount(params(), "distance", scalp);
        amount.validate(*in);
        const VectorField directionField(params(), "direction", scalp);
        directionField.validate(*in);
        auto unit = [](Vec3f d) { return d.length() > 1e-6f ? d.normalized() : Vec3f{}; };

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !amount.strandLocal() || !directionField.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        // On the CPU engine (look.slang DisplaceCS): one direction, or each strand's unit one
        // (StageInputs::directions; `which`: those strands alone, in that order).
        auto engineRun = [&](const std::vector<std::uint32_t>* which, gpu::StageInputs* inputs) {
            gpu::StageParamReader reader(params(), "");
            reader.directions = !directionField.isConstant();
            if (reader.directions) {
                auto ways = std::make_shared<std::vector<Vec3f>>(which ? which->size() : in->strandCount());
                parallelFor(0, ways->size(), [&](std::size_t k) { (*ways)[k] = unit(directionField.at(*in, which ? (*which)[k] : k)); }, 4096);
                inputs->directions = std::move(ways);
            }
            return reader;
        };
        // look.slang's DisplaceCS, the GPU preview's own (look_cpu.h): in full, or the strands the
        // input changed in, into the last result's copy.
        gpu::StageInputs stage;
        if (!last) {
            const gpu::StageParamReader reader = engineRun(nullptr, &stage);
            *result = gpu::runStage(gpu::StageKind::Displace, reader, in, scalp, std::move(stage));
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        const gpu::StageParamReader reader = engineRun(&changes->strands, &stage);
        gpu::runStageOnStrands(gpu::StageKind::Displace, reader, in, changes->strands, scalp, *out, std::move(stage));
        *changed = *changes;
        *result = StrandsPtr(out);
        return true;
    }
};

}  // namespace

PartsPtr partLinesFor(const ParamSet& part, const StrandSet& strands, const ScalpMesh& scalp, const PartsPtr& painted) {
    if (part.getString("partsFrom") == "auto") return autoPartLines(part, strands, scalp);
    return painted && !painted->empty() ? painted : nullptr;
}

bool autoPartState(const Operator& op, const StrandsPtr& output, PartsPtr* lines) {
    const auto* part = dynamic_cast<const PartOp*>(&op);
    return part && part->autoLines(output, lines);
}

void registerPartOperator(OperatorRegistry& registry) {
    registry.registerType(op::kPart,
                          {"shape", "Opens a gap along the parts drawn on the scalp.", "parting gap line"},
                          [] { return std::make_unique<PartOp>(); });
}

void registerCollideOperator(OperatorRegistry& registry) {
    registry.registerType(op::kCollide,
                          {"collide",
                           "Pushes strands out of the scalp and any collider. Best last.",
                           "collision penetration body scalp"},
                          [] { return std::make_unique<CollideOp>(); });
}

void registerGravityOperator(OperatorRegistry& registry) {
    registry.registerType(op::kGravity,
                          {"deform",
                           "Bends strands from the root under their weight; stiff at the root (a Collide after it keeps them off the scalp).",
                           "weight droop sag fall"},
                          [] { return std::make_unique<GravityOp>(); });
}

void registerDisplaceOperator(OperatorRegistry& registry) {
    registry.registerType(op::kDisplace,
                          {"deform",
                           "Moves strands along the scalp's normal (or a direction): volume off the scalp.",
                           "volume push offset normal"},
                          [] { return std::make_unique<DisplaceOp>(); });
}

bool collideRestPush(const RestPoseCollision* rest, const ScalpMesh& mesh, Vec3f& p, float to, bool soft, float softness, float maxDepth,
                     float searchRadius, CollideRestState& state) {
    if (rest) {
        // The mesh's bound first: it costs less than the walk to the point's vertex, and the answer
        // (a miss) is the mesh's own.
        if (collideBeyondBound(state, p, searchRadius)) return false;
        Vec3f delta;
        switch (rest->push(p, to, maxDepth, searchRadius, &delta, state.cursor)) {
            case MeshDistanceField::Push::Kept: return false;
            case MeshDistanceField::Push::Moved:
                if (soft) delta = softPush(delta, softness);
                p += delta;
                return true;
            case MeshDistanceField::Push::Ask: break;  // the deformed mesh's own answer, below
        }
    }
    // Where the rest field cannot tell: the deformed mesh itself (which the rest field alone would
    // miss by how the mesh bends); method exact: always.
    return collideMeshPush(mesh, p, to, soft, softness, maxDepth, searchRadius, state);
}

bool collideMeshPush(const ScalpMesh& mesh, Vec3f& p, float to, bool soft, float softness, float maxDepth, float searchRadius,
                     CollideRestState& state) {
    if (collideBeyondBound(state, p, searchRadius)) return false;
    Vec3f delta;
    float lowerBound = 0.0f;
    const SurfacePoint sp = mesh.closestPointBounded(p, searchRadius, &lowerBound);
    state.anchor = p;
    state.bound = lowerBound;
    if (sp.root.face < 0) return false;
    const float height = (p - sp.position).dot(sp.normal);
    if (height >= to || height <= -maxDepth) return false;
    delta = sp.normal * (to - height);
    if (soft) delta = softPush(delta, softness);
    p += delta;
    return true;
}

}  // namespace groomlab
