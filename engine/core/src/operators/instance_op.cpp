// SPDX-License-Identifier: Apache-2.0
// Instance (Strands -> Strands): geometry on the strands, e.g. snowflakes near
// the tips, dandruff near the roots, dew, burrs or beads. Each strand gets up
// to `count` instances, each kept with the (mappable) probability, somewhere
// between tMin and tMax along it; orientation, size, spin and offset vary per
// instance. The strands pass through unchanged, carrying the instances (see
// instances.h); hosts place them on the final strands. sizeMode fitStrand sizes
// each to its strand instead (leaves, feathers spanning it): a fraction of the
// strand's rest length along the prototype's +Y, `aspect` of that across.
// Several prototypes (names separated by spaces) are variants, one per
// instance, picked by its ID or by a map.
// Scales (Strands -> Strands): one scale at each strand's root instead, lying on the skin along the
// strand, as long as the strand and as wide as its root (times `size`; scales.h makes their
// meshes: tilted like roof tiles, bent onto the skin).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/instances.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr float kDegrees = 0.017453293f;

InstanceOrient orientFrom(const std::string& name) {
    if (name == "strand") return InstanceOrient::Strand;
    if (name == "up") return InstanceOrient::Up;
    if (name == "root") return InstanceOrient::Root;
    if (name == "random") return InstanceOrient::Random;
    throw GraphError("Instance: orient must be strand, up, root or random");
}


// Instance and Scales: one class, `scales_` telling them apart (each reads only its own params).
class InstanceOp : public Operator {
public:
    explicit InstanceOp(bool scales) : scales_(scales) {}
    std::string typeName() const override { return scales_ ? op::kScales : op::kInstance; }
    std::string category() const override { return "look"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        std::vector<ParamSpec> specs;
        if (scales_) {
            specs = {
                {"prototype", std::string("groomlab:scale"),
                 "The plate (a mesh's name in the scene); several separated by spaces are variants, one per scale "
                 "(variantBy). Built in: groomlab:scale, groomlab:scaleRound, groomlab:scaleShield"},
                ParamSpec{"variantBy", std::string("random"),
                          "With several prototypes: random (by each scale's ID and the seed) or map (the variant param, "
                          "0-1 over the list)", "", {"random", "map"}}
                    .in("Variants"),
                ParamSpec{"variant", 0.0, "variantBy map: which prototype, 0 the first to 1 the last (mappable)"}
                    .range(0, 1)
                    .in("Variants")
                    .when("variantBy", {"map"}),
                ParamSpec{"size", 1.0, "Each scale's size: times its strand's length along and its root width across "
                                       "(mappable)"}
                    .soft(0, 2),
                ParamSpec{"aspect", 0.1, "Width over length for strands without widths (mappable)"}.atLeast(0).soft(0, 1),
                ParamSpec{"lift", 0.0, "Degrees each tip turns up off the skin, along the flow (on top of tile's rise)",
                          "angle"}
                    .soft(-90, 90),
                ParamSpec{"tile", 1.0,
                          "How far each tip rises over the next scale's base, in plate heights (1: by its own height, as "
                          "roof tiles; 0: flat; lift turns it further). Overlap comes from the density and the strands' "
                          "length"}
                    .atLeast(0)
                    .soft(0, 2),
                ParamSpec{"stack", 0.02, "A random height of their own, a fraction of the plate's: overlapping scales "
                                         "never share a depth (no z-fighting)", "fraction"}
                    .range(0, 1)
                    .soft(0, 0.2),
                ParamSpec{"conform", true, "Bend each onto the skin, every vertex at its height above the surface under it"},
                ParamSpec{"follow", std::string("skin"),
                          "skin (flat on the skin, tilted by lift and tile) or strand (the base on the skin, the rest "
                          "bent along the strand's curve over the plate's length: swept scales, spikes and quills that "
                          "follow the groom)",
                          "", {"skin", "strand"}},
                ParamSpec{"aim", std::string("tip"),
                          "Flat on the skin: which way each points, toward its strand's tip (root to tip) or root (the "
                          "strand's direction near its root: strands that curl back). follow strand always aims by the "
                          "root",
                          "", {"tip", "root"}}
                    .when("follow", {"skin"}),
                ParamSpec{"offset", 0.0, "Their height above the skin, scene units", "length"}.soft(0, 0.5),
                ParamSpec{"showStrands", false, "The strands draw and render too (off: they only lay the scales out)"},
                ParamSpec{"material", std::string(),
                          "The shading group or shader they render with (empty: the prototype mesh's own; the built-in "
                          "plates a surface reading the scaleColor user data)"},
                ParamSpec{"preview", std::string("auto"),
                          "In the viewport: auto (their meshes, flat quads past the viewport's budget), meshes or quads "
                          "(fast while grooming); renders always take the meshes",
                          "", {"auto", "meshes", "quads"}}
                    .adv(),
                ParamSpec{"scaleRandom", 0.1, "Each scale's size up to this share more or less, at random", "fraction"}
                    .range(0, 1)
                    .in("Random"),
                ParamSpec{"spinRandom", 0.0, "Random turn about the skin's normal, either way, 0-1 (1: half a turn)",
                          "fraction"}
                    .range(0, 1)
                    .in("Random"),
            };
        } else {
            specs = {
                {"prototype", std::string("instance"),
                 "The mesh to instance (its name in the scene); several separated by spaces are variants, one per "
                 "instance (variantBy)"},
                ParamSpec{"variantBy", std::string("random"),
                          "With several prototypes: random (by each instance's ID and the seed) or map (the variant "
                          "param, 0-1 over the list)", "", {"random", "map"}}
                    .in("Variants"),
                ParamSpec{"variant", 0.0, "variantBy map: which prototype, 0 the first to 1 the last (mappable)"}
                    .range(0, 1)
                    .in("Variants")
                    .when("variantBy", {"map"}),
                notAnimatable({"count", std::int64_t{1}, "Instances tried per strand"}).range(0, 64).soft(0, 8),
                notAnimatable({"probability", 0.1, "Chance each one is kept, per strand (mappable)", "fraction"}).soft(0, 1),
                ParamSpec{"tMin", 0.6, "Nearest the root they sit, 0 root to 1 tip"}.range(0, 1),
                ParamSpec{"tMax", 1.0, "Nearest the tip they sit"}.range(0, 1),
                ParamSpec{"sizeMode", std::string("fixed"),
                          "fixed (scale: a size in scene units) or fitStrand (each spans its strand: fitScale of the "
                          "strand's rest length along its +Y, aspect of that across; stable while strands stretch)",
                          "", {"fixed", "fitStrand"}},
                ParamSpec{"scale", 0.02, "Size, scene units (mappable)", "length"}.soft(0, 0.2).when("sizeMode", {"fixed"}),
                ParamSpec{"fitScale", 1.0, "Length along +Y, a fraction of the strand's rest length (1: the whole strand) "
                                           "(mappable)"}
                    .soft(0, 2)
                    .when("sizeMode", {"fitStrand"}),
                ParamSpec{"aspect", 0.1, "Width across (X and Z) over the length along +Y (mappable)"}
                    .atLeast(0)
                    .soft(0, 1)
                    .when("sizeMode", {"fitStrand"}),
                ParamSpec{"scaleByWidth", true,
                          "Scale each one by the strand's width where it sits over its root width (the Width node's "
                          "profile): smaller toward thin tips"}
                    .in("Placement"),
                ParamSpec{"deform", std::string("none"),
                          "none: each one rigid, placed by its frame; alongStrand: bent along its strand (Yeti's "
                          "instance deform), the prototype's long axis (deformAxis) laid onto the strand from where it "
                          "sits, facing the vane (a feather's) or the skin: leaves on vines, feather meshes following a "
                          "shaft, ribbons",
                          "", {"none", "alongStrand"}}
                    .in("Placement"),
                notAnimatable({"deformAxis", std::string("y"), "deform alongStrand: the prototype's axis laid along the "
                                                               "strand (its length); the next two (cyclically) face up "
                                                               "and across",
                               "", {"x", "y", "z"}})
                    .in("Placement")
                    .when("deform", {"alongStrand"}),
                ParamSpec{"orient", std::string("strand"), "strand (along it), up (world up), root (the root direction) or "
                                                           "random", "", {"strand", "up", "root", "random"}}
                    .in("Placement"),
                ParamSpec{"offset", 0.0, "Distance off the strand, scene units", "length"}.soft(0, 0.5).in("Placement"),
                ParamSpec{"pinToRest", false,
                          "Keep each one at a fixed distance from the root, measured on the strand's rest pose, instead "
                          "of a fraction of its length now: strands that stretch (simulated guides) don't slide them"}
                    .in("Placement"),
                ParamSpec{"scaleRandom", 0.3, "Each one's size up to this share more or less, at random", "fraction"}
                    .range(0, 1)
                    .in("Random"),
                ParamSpec{"spinRandom", 1.0, "Random turn about their axis, either way, 0-1 (1: up to half a turn either way: "
                                             "any roll)", "fraction"}
                    .range(0, 1)
                    .in("Random"),
            };
        }
        specs.push_back(notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Random").adv());
        return specs;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // The strands pass through: with constant maps a strand's instances come from its ID alone
    // (the same IDs, the input's instances the same), so they are the last result's, and the
    // input's changes are the result's. A map may read what a dab changed (a channel, the shape):
    // in full.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

    // Masked: the mixed result the graph keeps is what the next run splices into, when it holds
    // the instances made.
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override { carried(made, mixed); }

private:
    bool scales_;  // a Scales node (else an Instance)
    // The last instances made: a full evaluation making the same again hands those on (the same
    // set, so the graph sees the strands' instances stayed and which strands changed:
    // strandChangesBetween).
    mutable std::mutex keptMutex_;
    mutable std::shared_ptr<const InstanceSet> kept_;
    mutable std::uint64_t keptHash_ = 0;
    mutable bool keptHashKnown_ = true;  // (a splice making another set leaves it to the next full run)
    // The arrays before last (their pages kept: a splice writes them again rather than faulting new
    // ones in), taken when nothing else holds them.
    mutable std::shared_ptr<const InstanceSet> spareSet_;
    mutable std::shared_ptr<const std::vector<std::size_t>> spareFirst_;
    // The last result made with no instances coming in, and per strand where its own begin in its
    // instances (one more: the end): a dab draws again only the strands it changed. (keptMutex_.)
    mutable std::weak_ptr<const StrandSet> keptOutput_;
    mutable std::shared_ptr<const std::vector<std::size_t>> keptFirst_;
    LongestKept longest_;  // the longest strand its maps read for the last result
    // A strand's maps and how many instances it gets.
    struct StrandDraw {
        float chance = 0.0f, size = 0.0f, ratio = 1.0f, variant = 0.0f;
        std::size_t count = 0;
    };

    // `made`'s place taken by `next`, a result holding its very instances (the same strands: its
    // longest the same), as what the next run splices into.
    void carried(const StrandsPtr& made, const StrandsPtr& next) const {
        longest_.mixed(made, next);
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (keptOutput_.lock() == made) keptOutput_ = next && next->instances() == made->instances() ? next : StrandsPtr();
    }

    // In full, or (with `incremental`) the input with the last result's instances; false when
    // that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const char* typeLabel = scales_ ? op::kScales : op::kInstance;
        const StrandsPtr in = requireStrands(inputs, 0, typeLabel);
        const ScalpMesh* scalp = valueAs<MeshPtr>(ctx.external("scalp")).get();
        const bool scales = scales_;
        const std::string sizeMode = scales ? std::string("fixed") : params().getString("sizeMode");
        if (sizeMode != "fixed" && sizeMode != "fitStrand") throw GraphError("Instance: sizeMode must be fixed or fitStrand");
        const bool fitStrand = !scales && sizeMode == "fitStrand";
        // Fitted and scales: a fraction of the rest length (fitScale, size) and a ratio (aspect); fixed: a size.
        const bool fitted = fitStrand || scales;
        // Scales: one on every strand (the field is not read).
        const ScalarField probability(params(), scales ? "size" : "probability", scalp);
        const ScalarField scale(params(), scales ? "size" : fitted ? "fitScale" : "scale", scalp);
        const ScalarField aspect(params(), "aspect", scalp);
        const std::string variantBy = params().getString("variantBy");
        if (variantBy != "random" && variantBy != "map") throw GraphError(std::string(typeLabel) + ": variantBy must be random or map");
        const std::vector<std::string> names = instancePrototypeNames(params().getString("prototype"));
        const bool byMap = variantBy == "map" && names.size() > 1;
        const ScalarField variant(params(), "variant", scalp);
        if (!scales) probability.validate(*in);
        scale.validate(*in);
        if (fitted) aspect.validate(*in);
        if (byMap) variant.validate(*in);
        if (incremental && !incremental->inputs.empty() && incremental->inputs[0] && incremental->inputs[0]->pointsKept) {
            // Only widths or channels changed (a Width or HairColor edit before it): where its maps
            // read neither (constants, textures, the strands' shape), each instance is where it was.
            const StrandsPtr last = previousStrandsFor(*incremental, *in);
            if (last && !(!scales && probability.readsAttributes()) && !scale.readsAttributes() &&
                !(fitted && aspect.readsAttributes()) && !(byMap && variant.readsAttributes())) {
                auto out = std::make_shared<StrandSet>(*in);
                out->setInstances(last->instances());
                carried(last, out);
                *result = StrandsPtr(out);
                *changed = *incremental->inputs[0];
                return true;
            }
        }
        // The fields read (one read twice for one that is not).
        const ScalarField& probabilityRead = scales ? scale : probability;
        const ScalarField& aspectRead = fitted ? aspect : scale;
        const ScalarField& variantRead = byMap ? variant : scale;
        // Incrementally with maps that read each strand alone (or with the longest strand, the
        // same as for the last result): the strands the input changed in drawn again (spliced).
        const StrandChanges* spliceChanges = nullptr;
        StrandsPtr spliceLast;
        std::shared_ptr<const std::vector<std::size_t>> spliceFirst;
        bool spliced = false;
        if (incremental) {
            const StrandChanges* changes = someStrandsChanged(*incremental, 0);
            const StrandsPtr last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            auto constant = [](const ScalarField& f) { return f.isConstant() && f.strandLocal(); };
            if (!last) return false;
            if ((!scales && !constant(probability)) || !constant(scale) || (fitted && !constant(aspect)) ||
                (byMap && !constant(variant))) {
                {
                    const std::lock_guard<std::mutex> lock(keptMutex_);
                    if (keptOutput_.lock() == last) spliceFirst = keptFirst_;
                }
                if (!spliceFirst || spliceFirst->size() != in->strandCount() + 1 || in->instances() || !last->instances() ||
                    !longest_.local(last, {&probabilityRead, &scale, &aspectRead, &variantRead})) {
                    return false;
                }
                spliceChanges = changes;
                spliceLast = last;
                spliced = true;
            }
        }
        if (incremental && !spliced) {
            const StrandChanges* changes = someStrandsChanged(*incremental, 0);
            const StrandsPtr last = previousStrandsFor(*incremental, *in);
            if (changes->none()) {
                *result = last;
            } else {
                auto out = std::make_shared<StrandSet>(*in);
                out->setInstances(last->instances());
                *result = StrandsPtr(out);
            }
            *changed = *changes;
            return true;
        }
        const auto count = scales ? std::size_t{1} : static_cast<std::size_t>(params().getInt("count"));
        const float tMin = scales ? 0.0f : static_cast<float>(params().getDouble("tMin"));
        const float tMax = scales ? 0.0f : std::max(static_cast<float>(params().getDouble("tMax")), tMin);  // never before tMin
        const float scaleRandom = static_cast<float>(params().getDouble("scaleRandom"));
        const float spinRandom = static_cast<float>(params().getDouble("spinRandom"));
        const float offset = static_cast<float>(params().getDouble("offset"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        // Instance's own.
        InstanceOrient orient = InstanceOrient::Strand;
        bool scaleByWidth = false, pinToRest = false, deformed = false;
        std::uint8_t axisCode = 1;
        // Scales' own.
        std::uint8_t previewCode = 0;
        float lift = 0.0f, tile = 0.0f, stack = 0.0f;
        bool conform = false, followStrand = false, aimRoot = false, showStrands = false;
        std::string material;
        if (scales) {
            const std::string preview = params().getString("preview");
            if (preview != "auto" && preview != "meshes" && preview != "quads") {
                throw GraphError("Scales: preview must be auto, meshes or quads");
            }
            previewCode = static_cast<std::uint8_t>(preview == "auto" ? 0 : preview == "meshes" ? 1 : 2);
            lift = static_cast<float>(params().getDouble("lift")) * kDegrees;
            tile = static_cast<float>(params().getDouble("tile"));
            stack = static_cast<float>(params().getDouble("stack"));
            conform = params().getBool("conform");
            const std::string follow = params().getString("follow");
            if (follow != "skin" && follow != "strand") throw GraphError("Scales: follow must be skin or strand");
            followStrand = follow == "strand";
            const std::string aim = params().getString("aim");
            if (aim != "tip" && aim != "root") throw GraphError("Scales: aim must be tip or root");
            aimRoot = aim == "root";
            material = params().getString("material");
            showStrands = params().getBool("showStrands");
        } else {
            orient = orientFrom(params().getString("orient"));
            scaleByWidth = params().getBool("scaleByWidth");
            pinToRest = params().getBool("pinToRest");
            const std::string deform = params().getString("deform");
            if (deform != "none" && deform != "alongStrand") throw GraphError("Instance: deform must be none or alongStrand");
            const std::string deformAxis = params().getString("deformAxis");
            if (deformAxis != "x" && deformAxis != "y" && deformAxis != "z") throw GraphError("Instance: deformAxis must be x, y or z");
            deformed = deform == "alongStrand";
            axisCode = static_cast<std::uint8_t>(deformAxis == "x" ? 0 : deformAxis == "y" ? 1 : 2);
        }

        auto instances = std::make_shared<InstanceSet>();
        if (const auto& existing = in->instances()) *instances = *existing;
        if (scales && !showStrands) instances->hidesStrands = true;
        std::vector<std::uint32_t> prototypes;
        for (const std::string& name : names) prototypes.push_back(instances->prototypeIndex(name));
        const std::int32_t materialIndex = scales && !material.empty() ? static_cast<std::int32_t>(instances->materialIndex(material)) : -1;
        const std::size_t strands = in->strandCount();
        auto instanceId = [&](std::size_t s, std::size_t k) {
            return hashCombine(hashCombine(in->ids()[s], seed ^ 0x1257A9CEull), k);
        };
        // A strand's maps and how many instances it gets, then its instances written from `at` on.
        auto drawOf = [&](std::size_t s) {
            StrandDraw d;
            d.chance = scales ? 1.0f : probability.at(*in, s);
            if (d.chance <= 0.0f) return d;
            d.size = scale.at(*in, s);
            d.ratio = fitted ? std::max(aspect.at(*in, s), 0.0f) : 1.0f;
            d.variant = byMap ? variant.at(*in, s) : 0.0f;
            for (std::size_t k = 0; k < count; ++k) d.count += scales || unitFloat(hash64(instanceId(s, k))) < d.chance ? 1 : 0;
            return d;
        };
        const std::size_t variantCount = prototypes.size();
        auto write = [&](std::size_t s, const StrandDraw& d, InstanceSet::Instance* at) {
            for (std::size_t k = 0; k < count; ++k) {
                const std::uint64_t id = instanceId(s, k);
                if (!scales && unitFloat(hash64(id)) >= d.chance) continue;
                Rng rng(id);
                InstanceSet::Instance& inst = *at++;  // default-made by the resize
                // The variant: by the map over the list, else by the ID (its own hash: the draws
                // below stay as they were for one prototype).
                std::size_t which = 0;
                if (variantCount > 1) {
                    const float pick = byMap ? std::clamp(d.variant, 0.0f, 1.0f) : unitFloat(hash64(id ^ 0x7A21A47ull));
                    which = std::min(static_cast<std::size_t>(pick * static_cast<float>(variantCount)), variantCount - 1);
                }
                inst.prototype = prototypes[which];
                inst.variant = static_cast<std::uint16_t>(which);
                inst.strand = in->ids()[s];
                inst.id = id;
                if (scales) {
                    const StrandRoot& root = in->roots()[s];
                    inst.onSkin = true;
                    inst.t = 0.0f;
                    inst.spin = 0.5f * kTwoPi * spinRandom * (2.0f * rng.nextFloat() - 1.0f);
                    inst.scale = d.size * (1.0f + scaleRandom * (2.0f * rng.nextFloat() - 1.0f));
                    inst.fitStrand = false;
                    inst.aspect = d.ratio;
                    inst.offset = offset;
                    inst.scaleByWidth = false;
                    inst.conform = conform;
                    inst.preview = previewCode;
                    inst.material = materialIndex;
                    inst.lift = lift;
                    inst.tile = tile;
                    inst.stack = stack;
                    inst.followStrand = followStrand;
                    inst.aimRoot = aimRoot;
                    // The normal the node sees, for a host placing it without the scalp.
                    inst.direction = scalp && scalp->contains(root) ? scalp->normal(root) : Vec3f{0, 1, 0};
                    continue;
                }
                inst.t = tMin + (tMax - tMin) * rng.nextFloat();
                inst.orient = orient;
                inst.spin = 0.5f * kTwoPi * spinRandom * (2.0f * rng.nextFloat() - 1.0f);  // either way, as Scales
                inst.scale = d.size * (1.0f + scaleRandom * (2.0f * rng.nextFloat() - 1.0f));
                inst.offset = offset;
                inst.scaleByWidth = scaleByWidth;
                inst.pinToRest = pinToRest;
                inst.fitStrand = fitStrand;
                inst.aspect = d.ratio;
                if (deformed) {
                    inst.deform = true;
                    inst.deformAxis = axisCode;
                }
                if (orient == InstanceOrient::Random) {
                    // Uniform on the sphere.
                    const float z = 2.0f * rng.nextFloat() - 1.0f;
                    const float a = kTwoPi * rng.nextFloat();
                    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                    inst.direction = {r * pm::cos(a), r * pm::sin(a), z};
                }
            }
        };
        // Per strand in parallel: its draw; then where its instances go (after the existing ones, in
        // strand order) and each written in place. Incrementally (the input's instances none, each
        // strand's maps its own, the last result's made here): the strands the input changed in drawn
        // again, the others' instances the last result's.
        std::shared_ptr<const InstanceSet> made;
        std::shared_ptr<const std::vector<std::size_t>> firstMade;
        const std::vector<std::uint32_t>* redrawn = nullptr;
        if (spliced) {
            const std::vector<std::uint32_t>& again = spliceChanges->strands;
            redrawn = &again;
            std::vector<StrandDraw> draws(again.size());
            parallelFor(0, again.size(), [&](std::size_t k) { draws[k] = drawOf(again[k]); }, 256);
            const std::vector<std::size_t>& before = *spliceFirst;
            const std::vector<InstanceSet::Instance>& was = spliceLast->instances()->instances;
            // Theirs on their own first: the same as the last result's (a dab seldom tips a strand's
            // chance past one of its draws), the last instances stay.
            std::vector<std::size_t> at(again.size() + 1, 0);
            for (std::size_t k = 0; k < again.size(); ++k) at[k + 1] = at[k] + draws[k].count;
            std::vector<InstanceSet::Instance> fresh(at.back());
            std::atomic<bool> differ{false};
            parallelFor(0, again.size(), [&](std::size_t k) {
                const std::size_t s = again[k];
                if (!draws[k].count) {
                    if (before[s + 1] != before[s]) differ = true;
                    return;
                }
                write(s, draws[k], fresh.data() + at[k]);
                if (before[s + 1] - before[s] != draws[k].count) {
                    differ = true;
                    return;
                }
                for (std::size_t i = 0; i < draws[k].count; ++i) {
                    if (InstanceSet::hashInstance(0, fresh[at[k] + i]) != InstanceSet::hashInstance(0, was[before[s] + i])) {
                        differ = true;
                        return;
                    }
                }
            }, 256);
            if (!differ) {
                made = spliceLast->instances();
                firstMade = spliceFirst;
            } else {
                std::shared_ptr<std::vector<std::size_t>> first;
                {
                    const std::lock_guard<std::mutex> lock(keptMutex_);
                    if (spareSet_ && spareSet_.use_count() == 1) {
                        instances->instances = std::move(const_cast<InstanceSet&>(*spareSet_).instances);
                    }
                    if (spareFirst_ && spareFirst_.use_count() == 1) {
                        first = std::const_pointer_cast<std::vector<std::size_t>>(std::move(spareFirst_));
                    }
                    spareSet_.reset();
                    spareFirst_.reset();
                }
                if (!first) first = std::make_shared<std::vector<std::size_t>>();
                first->resize(strands + 1);
                (*first)[0] = 0;
                for (std::size_t s = 0; s < strands; ++s) (*first)[s + 1] = before[s + 1] - before[s];
                for (std::size_t k = 0; k < again.size(); ++k) (*first)[again[k] + 1] = draws[k].count;
                for (std::size_t s = 0; s < strands; ++s) (*first)[s + 1] += (*first)[s];
                instances->instances.resize((*first)[strands]);  // (every one written below)
                // The runs of strands left alone, copied side by side; then the strands drawn again.
                std::vector<std::pair<std::size_t, std::size_t>> runs;  // [from, to) strands
                std::size_t from = 0;
                for (const std::uint32_t c : again) {
                    if (c > from) runs.emplace_back(from, c);
                    from = c + 1;
                }
                if (from < strands) runs.emplace_back(from, strands);
                parallelFor(0, runs.size(), [&](std::size_t r) {
                    const auto [lo, hi] = runs[r];
                    std::copy(was.begin() + static_cast<std::ptrdiff_t>(before[lo]), was.begin() + static_cast<std::ptrdiff_t>(before[hi]),
                              instances->instances.begin() + static_cast<std::ptrdiff_t>((*first)[lo]));
                }, 16);
                parallelFor(0, again.size(), [&](std::size_t k) {
                    std::copy(fresh.begin() + static_cast<std::ptrdiff_t>(at[k]), fresh.begin() + static_cast<std::ptrdiff_t>(at[k + 1]),
                              instances->instances.begin() + static_cast<std::ptrdiff_t>((*first)[again[k]]));
                }, 256);
                firstMade = std::move(first);
                // Another set than the last (its hash left to the next full run that asks).
                made = std::move(instances);
                const std::lock_guard<std::mutex> lock(keptMutex_);
                kept_ = made;
                keptHashKnown_ = false;
            }
        } else {
            auto first = std::make_shared<std::vector<std::size_t>>(strands + 1, 0);
            std::vector<StrandDraw> draws(strands);
            parallelFor(0, strands, [&](std::size_t s) {
                draws[s] = drawOf(s);
                (*first)[s + 1] = draws[s].count;
            });
            (*first)[0] = instances->instances.size();
            for (std::size_t s = 0; s < strands; ++s) (*first)[s + 1] += (*first)[s];
            instances->instances.resize((*first)[strands]);
            parallelFor(0, strands, [&](std::size_t s) {
                if (draws[s].count) write(s, draws[s], instances->instances.data() + (*first)[s]);
            });
            firstMade = std::move(first);
        }
        if (!made) {
            made = std::move(instances);
            const std::uint64_t hash = made->contentHash();
            const std::lock_guard<std::mutex> lock(keptMutex_);
            if (kept_ && !keptHashKnown_) {
                keptHash_ = kept_->contentHash();
                keptHashKnown_ = true;
            }
            if (kept_ && keptHash_ == hash && kept_->size() == made->size()) {
                made = kept_;
            } else {
                kept_ = made;
                keptHash_ = hash;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (redrawn) {
            *changed = StrandChanges::listed(*redrawn);
            changed->instancesChanged = made != spliceLast->instances();
        }
        out->setInstances(std::move(made));
        const StrandsPtr output = out;
        {
            const std::lock_guard<std::mutex> lock(keptMutex_);
            keptOutput_ = in->instances() ? StrandsPtr() : output;
            if (keptFirst_ != firstMade) spareFirst_ = std::move(keptFirst_);
            keptFirst_ = std::move(firstMade);
            if (spliceLast && spliceLast->instances() != output->instances()) spareSet_ = spliceLast->instances();
        }
        longest_.made(output, {&probabilityRead, &scale, &aspectRead, &variantRead});
        *result = output;
        return true;
    }
};

}  // namespace

void registerInstanceOperator(OperatorRegistry& registry) {
    registry.registerType(op::kInstance,
                          {"look", "Copies a mesh (leaves, feathers, beads) along the strands.", "copy mesh leaves beads"},
                          [] { return std::make_unique<InstanceOp>(false); });
}

void registerScalesOperator(OperatorRegistry& registry) {
    registry.registerType(op::kScales,
                          {"look",
                           "A scale at each strand's root, lying on the skin along the strand: reptile skin, armor plates.",
                           "reptile armor plates skin"},
                          [] { return std::make_unique<InstanceOp>(true); });
}

}  // namespace groomlab
