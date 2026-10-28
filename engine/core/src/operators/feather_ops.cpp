// SPDX-License-Identifier: Apache-2.0
// Feathers (Yeti-style): Propagate grows barbs off every strand it is fed (the shaft, from a
// Scatter or from guides drawn by hand), and Split routes strands into shares, so one set of
// shafts carries several feather designs.
//
// A barb is an ordinary strand: nodes after Propagate (Noise, Curl, Width, HairColor, Cards...)
// shape it like any hair. Each barb keeps its shaft's root (maps and textures sampled at the
// root read the feather's base) and its shaft's channels, and adds featherId, shaftU, side,
// barbIndex and isShaft (and clumpId: its vane, or its split of the vane), and its place in the
// feather laid flat (vaneU, vaneTipU, vaneTipV: what FeatherColor's patterns read). A down
// zone at the shaft's base grows soft, curly barbs of its own (isDown). For game engines
// (feathers.h), a shaftWidth records the shafts as rachis tubes and roots the barbs on their
// surface, and a groupId puts the feathers in a groom group of their own.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>

#include "groomlab/color_patterns.h"
#include "groomlab/curve.h"
#include "groomlab/feathers.h"
#include "groomlab/field.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kDegrees = 0.0174532925f;
constexpr float kGoldenAngle = 2.39996323f;  // radians: barbs "around" a shaft spiral by it
constexpr double kMaxFeatherPoints = 400.0e6;  // past this, memory runs out (as Scatter's limit)
constexpr float kMinRachisRadius = 0.05f;      // the rachis never thins below this share of its root width

// The channels Propagate writes.
constexpr const char* kFeatherIdChannel = "featherId";  // the shaft's number, from its ID (stableGroupNumbers; -1: none)
constexpr const char* kShaftUChannel = "shaftU";        // where the barb sits along its shaft, 0 root .. 1 tip
constexpr const char* kSideChannel = "side";            // -1 left, +1 right, 0 around (and shafts)
constexpr const char* kBarbIndexChannel = "barbIndex";  // the barb's place along its side (-1: a shaft)
constexpr const char* kIsShaftChannel = "isShaft";      // 1 for shafts kept in the output
constexpr const char* kClumpChannel = "clumpId";        // the barb's vane, or its split of the vane (numbered from the shaft's ID)
constexpr const char* kGroupChannel = "groupId";        // the groom group (with groupId set): engines' groups
// Also vane space's (vaneU, vaneTipU, vaneTipV) and the down zone's isDown: color_patterns.h.

bool writtenChannel(const std::string& name) {
    for (const char* c : {kFeatherIdChannel, kShaftUChannel, kSideChannel, kBarbIndexChannel, kIsShaftChannel, kRachisChannel,
                          kClumpChannel, kVaneUChannel, kVaneTipUChannel, kVaneTipVChannel, kIsDownChannel,
                          kVaneNormalChannels[0], kVaneNormalChannels[1], kVaneNormalChannels[2]}) {
        if (name == c) return true;
    }
    return false;
}

// The shafts on input 0: strands, or guides taken as strands.
StrandsPtr shaftsOf(const std::vector<Value>& inputs, const char* opName) {
    if (!inputs.empty()) {
        if (StrandsPtr s = valueAs<StrandsPtr>(inputs[0])) return s;
        if (GuidesPtr g = valueAs<GuidesPtr>(inputs[0])) return guidesAsStrands(*g);
    }
    throw GraphError(std::string(opName) + ": shafts input not connected");
}

struct Barb {
    StrandId id = 0;
    float u = 0.0f;       // along the shaft by arc length, 0 root .. 1 tip
    float length = 0.0f;
    float angle = 0.0f;   // radians from the shaft (toward the barb's side)
    float spin = 0.0f;    // "around": its turn about the shaft
    int side = 0;         // -1 left, +1 right, 0 around
    int index = 0;        // along its side
    int vane = 0;         // which of the shaft's vanes (side slots) it is on
    int group = 0;        // its split within the vane (0-based; a down barb: the vane's last)
    bool kept = true;     // false: thinned out of the preview (still shapes its split)
    bool down = false;    // a barb of the down zone: soft and curly, in no split
    float bend = 0.0f;    // radians it curls within the vane over its length
    std::size_t points = 0;
    float tipAlong = 0.0f, tipAcross = 0.0f;  // its tip in the feather laid flat, from its root (scene units)
};

// A shaft's barbs, in output order: vane by vane (each the down zone's barbs, then the vane's),
// root to tip.
struct ShaftPlan {
    std::vector<Barb> barbs;
    std::vector<int> groups;  // splits per vane (one more where the vane has down barbs)
    std::size_t keptBarbs = 0;
    std::size_t keptPoints = 0;
    float reachAlong = 0.0f, reachAcross = 0.0f;  // the feather laid flat: vane space's 1 on each axis
};

// The turn of a barb's segment j (its middle at t of the barb) within the vane (theta, from the
// shaft toward the barb's side) and out of it (rise, toward the vane's normal); a down barb adds
// its frizz, `kink` (radians in and out of the vane, summed from the root).
void barbTurn(const Barb& b, float t, float lift, float droop, float kinkIn, float kinkOut, float* theta, float* rise) {
    *theta = b.angle - b.bend * t + (b.side == 0 ? droop * t : 0.0f) + kinkIn;
    *rise = (b.side == 0 ? 0.0f : lift - droop * t) + kinkOut;
}

// A down barb's frizz at segment j: the kinks in and out of the vane summed from its root, drawn
// from its ID (the same in both of Propagate's passes).
void downKinks(const Barb& b, std::uint64_t seed, float frizz, std::size_t j, float* in, float* out) {
    *in = 0.0f;
    *out = 0.0f;
    if (!b.down || frizz <= 0.0f) return;
    for (std::size_t k = 1; k <= j; ++k) {
        const std::uint64_t h = hashCombine(hashCombine(b.id, seed ^ 0xD0F2), k);
        *in += frizz * (2.0f * unitFloat(h) - 1.0f);
        *out += frizz * (2.0f * unitFloat(hashCombine(h, 1)) - 1.0f);
    }
}

// The frame along a shaft at arc position u (0-1): point, tangent, vane normal and side.
struct ShaftFrame {
    Vec3f point, tangent, normal, side;  // side = tangent x normal: the right-hand vane
};

class ShaftWalker {
public:
    // `up`: the vane's normal at the root (the scalp normal there, else a guess from the curve).
    ShaftWalker(const Vec3f* pts, std::size_t n, const Vec3f& up) : pts_(pts), n_(n), cumulative_(n) {
        cumulativeArcLength(pts, n, cumulative_.data());
        const Vec3f t0 = (pts[1] - pts[0]).normalized();
        rotationMinimizingFramesInto(pts, n, facing(t0, up), frames_);
    }
    float length() const { return cumulative_.back(); }
    // The rotation-minimizing frame at shaft point k (binormal = tangent x normal: the side).
    const Frame& frame(std::size_t k) const { return frames_[k]; }
    // Shaft point k's arc position, 0 root .. 1 tip.
    float along(std::size_t k) const { return length() > 0.0f ? cumulative_[k] / length() : 0.0f; }

    ShaftFrame at(float u) const {
        const float s = std::clamp(u, 0.0f, 1.0f) * length();
        const auto it = std::upper_bound(cumulative_.begin(), cumulative_.end(), s);
        std::size_t k = static_cast<std::size_t>(std::max<std::ptrdiff_t>(it - cumulative_.begin() - 1, 0));
        k = std::min(k, n_ - 2);
        const float span = cumulative_[k + 1] - cumulative_[k];
        const float f = span > 0.0f ? std::clamp((s - cumulative_[k]) / span, 0.0f, 1.0f) : 0.0f;
        ShaftFrame out;
        out.point = pts_[k] + (pts_[k + 1] - pts_[k]) * f;
        const Frame& a = frames_[k];
        const Frame& b = frames_[k + 1];
        Vec3f t = (a.tangent + (b.tangent - a.tangent) * f).normalized();
        if (t.length() < 0.5f) t = (pts_[k + 1] - pts_[k]).normalized();
        Vec3f nrm = a.normal + (b.normal - a.normal) * f;
        nrm = (nrm - t * nrm.dot(t)).normalized();
        if (nrm.length() < 0.5f) nrm = anyPerpendicular(t);
        out.tangent = t;
        out.normal = nrm;
        out.side = t.cross(nrm);
        return out;
    }
    // Where along the shaft (segment k, fraction f) arc position u falls: for per-point values.
    void locate(float u, std::size_t* k, float* f) const {
        const float s = std::clamp(u, 0.0f, 1.0f) * length();
        const auto it = std::upper_bound(cumulative_.begin(), cumulative_.end(), s);
        *k = std::min(static_cast<std::size_t>(std::max<std::ptrdiff_t>(it - cumulative_.begin() - 1, 0)), n_ - 2);
        const float span = cumulative_[*k + 1] - cumulative_[*k];
        *f = span > 0.0f ? std::clamp((s - cumulative_[*k]) / span, 0.0f, 1.0f) : 0.0f;
    }

private:
    // `up` made perpendicular to the shaft; a shaft standing along it faces world up, else +Z.
    static Vec3f facing(const Vec3f& t, const Vec3f& up) {
        for (const Vec3f& c : {up, Vec3f{0.0f, 1.0f, 0.0f}, Vec3f{0.0f, 0.0f, 1.0f}}) {
            const Vec3f p = c - t * c.dot(t);
            if (p.length() > 0.2f * std::max(c.length(), 1e-12f)) return p.normalized();
        }
        return anyPerpendicular(t);
    }

    const Vec3f* pts_;
    std::size_t n_;
    std::vector<float> cumulative_;
    std::vector<Frame> frames_;
};

// The vane's normal at a shaft's root: the scalp normal under a rooted shaft; for a free curve,
// away from the way it bends (a feather drooping onto the body faces up), else world up.
Vec3f rootUp(const ScalpMesh* scalp, const StrandRoot& root, const Vec3f* pts, std::size_t n) {
    if (scalp && scalp->contains(root)) return scalp->normal(root);
    const Vec3f t0 = (pts[1] - pts[0]).normalized();
    const Vec3f chord = pts[n - 1] - pts[0];
    const Vec3f bend = chord - t0 * chord.dot(t0);
    if (bend.length() > 1e-3f * std::max(chord.length(), 1e-12f)) return -bend.normalized();
    return {0.0f, 1.0f, 0.0f};
}

// Grows barbs off every input strand, the shaft. Barbs sit from `start` to `end` along it, `count`
// per side, in the vane plane (the shaft and the scalp's tangent across it, so feathers lie on the
// body; a free curve takes the plane it bends out of), each at `angle` from the shaft, `length`
// long times `lengthProfile` along the shaft (the vane's outline), bent by `curl` within the vane
// and by `lift`/`droop` out of it. Splits gather neighbouring barbs into the gaps a real vane has.
class PropagateOp : public Operator {
public:
    std::string typeName() const override { return op::kPropagate; }
    // The shaft's channels come along; the feather's own under fixed names.
    std::vector<ChannelWrite> channelWrites() const override {
        std::vector<ChannelWrite> out;
        for (const char* name : {kFeatherIdChannel, kShaftUChannel, kSideChannel, kBarbIndexChannel, kIsShaftChannel,
                                 kClumpChannel, kVaneUChannel, kVaneTipUChannel, kVaneTipVChannel}) {
            out.push_back({name, ""});
        }
        if (params().getDouble("down") > params().getDouble("start")) out.push_back({kIsDownChannel, ""});
        if (params().getDouble("shaftWidth") > 0.0 && params().getBool("keepShaft")) out.push_back({kRachisChannel, ""});
        for (const char* name : kVaneNormalChannels) out.push_back({name, ""});
        if (params().getInt("groupId") > 0) out.push_back({kGroupChannel, ""});
        return out;
    }
    std::string category() const override { return "feathers"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    // Its feather layer carries its name (FeatherLayer::node): a renamed node is evaluated anew.
    std::uint64_t cacheSalt() const override { return hashString(hash64(0xFEA7Fu), nodePath()); }
    std::vector<PortSpec> inputs() const override { return {{"shafts", PortType::Curves}}; }
    PortSpec output() const override { return {"barbs", PortType::Strands}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"sides", std::string("both"),
             "Where barbs grow: both sides of the vane, one side, or all around the shaft (plumes, down)", "",
             {"both", "left", "right", "around"}},
            notAnimatable({"count", std::int64_t{40}, "Barbs per side, spread evenly from start to end (around: twice this, "
                                                      "spiralling round the shaft)"})
                .range(0, 100000)
                .soft(0, 200),
            ParamSpec{"start", 0.15, "Where barbs begin along the shaft, 0 root .. 1 tip"}.range(0, 1),
            ParamSpec{"end", 1.0, "Where barbs end along the shaft, 0 root .. 1 tip"}.range(0, 1),
            notAnimatable({"segments", std::int64_t{6}, "Segments per barb"}).range(1, 256).soft(1, 20),
            ParamSpec{"length", 0.3, "Barb length at the widest point of the vane (mappable, per shaft)", "length"}
                .atLeast(0)
                .soft(0, 1),
            {"lengthProfile", Ramp{{0.0f, 0.45f}, {0.45f, 1.0f}, {1.0f, 0.15f}},
             "Barb length along the shaft, root to tip: the vane's outline"},
            ParamSpec{"asymmetry", 0.0, "Left vane shorter, right longer (-1..1): flight feathers"}.range(-1, 1)
                .when("sides", {"both", "left", "right"}),
            ParamSpec{"angle", 50.0, "Degrees from the shaft toward the barb's side (mappable, per shaft); 90: straight "
                                     "out", "angle"}
                .soft(0, 90),
            {"angleProfile", Ramp::constant(1.0f), "Angle along the shaft, root to tip (times angle)"},
            ParamSpec{"curl", 20.0, "Degrees each barb bends within the vane over its length (positive: toward the tip)",
                      "angle"}
                .soft(-90, 90)
                .in("Shape"),
            ParamSpec{"curlProfile", Ramp::constant(1.0f), "Curl along the shaft, root to tip (times curl)"}.in("Shape").adv(),
            ParamSpec{"lift", 0.0, "Degrees the barbs rise out of the vane at the shaft (toward the scalp normal)", "angle"}
                .soft(-90, 90)
                .in("Shape")
                .when("sides", {"both", "left", "right"}),
            ParamSpec{"droop", 10.0, "Degrees the barbs bend down out of the vane over their length (a cupped vane; "
                                     "around: back toward the root)", "angle"}
                .soft(-90, 90)
                .in("Shape"),
            ParamSpec{"splits", 0.0, "Groups per vane: neighbouring barbs gather into about this many groups, as in a "
                                     "ruffled feather (0 or 1: none); written to clumpId"}
                .atLeast(0)
                .soft(0, 10)
                .in("Splits"),
            ParamSpec{"splitAmount", 0.6, "How far the barbs of a split gather toward its middle barb at their tips, 0-1",
                      "fraction"}
                .range(0, 1)
                .in("Splits"),
            ParamSpec{"lengthRandom", 0.1, "Each barb's length up to this share more or less, at random", "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"angleRandom", 3.0, "Up to this many degrees more or less angle per barb, at random", "angle"}
                .soft(0, 20)
                .in("Random"),
            ParamSpec{"down", 0.0, "The down zone (afterfeather): from start up to this far along the shaft (0 root .. 1 "
                                   "tip) the barbs are soft and curly, the vane's begin past it (0: none); isDown 1 on "
                                   "them, for FeatherColor's downColor"}
                .range(0, 1)
                .in("Down"),
            notAnimatable({"downCount", std::int64_t{20}, "Down barbs per side in the down zone (around: twice this)"})
                .range(0, 100000)
                .soft(0, 100)
                .in("Down"),
            ParamSpec{"downLength", 1.0, "Down barbs' length, times the vane's there"}.atLeast(0).soft(0, 2).in("Down"),
            ParamSpec{"downCurl", 140.0, "Degrees each down barb curls over its length", "angle"}
                .soft(-360, 360)
                .in("Down"),
            ParamSpec{"downFrizz", 25.0, "Degrees of random kink at every segment of a down barb, in and out of the vane",
                      "angle"}
                .atLeast(0)
                .soft(0, 90)
                .in("Down"),
            notAnimatable({"downSegments", std::int64_t{12}, "Segments per down barb"}).range(1, 256).soft(1, 32).in("Down"),
            ParamSpec{"width", 0.0, "Barb width (0: the shaft's width where the barb grows)", "width"}
                .atLeast(0)
                .soft(0, 0.05),
            notAnimatable({"keepShaft", true, "Keep the shafts in the output (isShaft 1)"}).in("Rachis"),
            ParamSpec{"shaftWidth", 0.0, "The rachis's width at the root (0: none): the shafts become tubes of this "
                                         "width, a mesh to skin into the character (groomLabShape's outRachis) for "
                                         "engines", "width"}
                .atLeast(0)
                .soft(0, 0.2)
                .in("Rachis"),
            ParamSpec{"shaftWidthProfile", Ramp{{0.0f, 1.0f}, {1.0f, 0.25f}}, "The rachis's width along the shaft, root "
                                                                              "to tip (times shaftWidth)"}
                .in("Rachis")
                .adv(),
            notAnimatable({"shaftSides", std::int64_t{6}, "Sides of the rachis tube (2: a flat ribbon in the vane plane)"})
                .range(2, 32)
                .in("Rachis")
                .adv(),
            ParamSpec{"rootOnShaft", true, "With a shaftWidth: barbs start on the rachis's surface, not its center line, "
                                           "so an engine binds them to their own rachis"}
                .in("Rachis")
                .adv(),
            notAnimatable({"groupId", std::int64_t{0}, "The groom group the feathers export in (written to groupId; 0: not set). "
                                                       "Unreal gives each group its own material, LODs and physics"})
                .range(0, 1 << 20)
                .soft(0, 16)
                .in("Export"),
            ParamSpec{"groupName", std::string(""), "The group's name in engines (groom_group_name)"}.in("Export"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Random").adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = shaftsOf(inputs, op::kPropagate);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField lengthField(params(), "length", scalp);
        lengthField.validate(*in);
        const ScalarField angleField(params(), "angle", scalp);
        angleField.validate(*in);

        const std::string sides = params().getString("sides");
        std::vector<int> vanes;  // the side of each vane
        if (sides == "both") vanes = {-1, 1};
        else if (sides == "left") vanes = {-1};
        else if (sides == "right") vanes = {1};
        else if (sides == "around") vanes = {0};
        else throw GraphError("Propagate: sides must be both, left, right or around");
        const auto count = static_cast<int>(params().getInt("count"));
        const float start = static_cast<float>(params().getDouble("start"));
        const float end = std::max(start, static_cast<float>(params().getDouble("end")));  // not before start
        const auto segments = static_cast<std::size_t>(params().getInt("segments"));
        const Ramp lengthProfile = params().getRamp("lengthProfile");
        const float asymmetry = static_cast<float>(params().getDouble("asymmetry"));
        const Ramp angleProfile = params().getRamp("angleProfile");
        const float curl = static_cast<float>(params().getDouble("curl")) * kDegrees;
        const Ramp curlProfile = params().getRamp("curlProfile");
        const float lift = static_cast<float>(params().getDouble("lift")) * kDegrees;
        const float droop = static_cast<float>(params().getDouble("droop")) * kDegrees;
        const float splits = static_cast<float>(params().getDouble("splits"));
        const float splitAmount = static_cast<float>(params().getDouble("splitAmount"));
        const float lengthRandom = static_cast<float>(params().getDouble("lengthRandom"));
        const float angleRandom = static_cast<float>(params().getDouble("angleRandom")) * kDegrees;
        const float width = static_cast<float>(params().getDouble("width"));
        const bool keepShaft = params().getBool("keepShaft");
        const float shaftWidth = static_cast<float>(params().getDouble("shaftWidth"));
        const Ramp shaftWidthProfile = params().getRamp("shaftWidthProfile");
        const int shaftSides = static_cast<int>(params().getInt("shaftSides"));
        const bool rachis = shaftWidth > 0.0f && std::isfinite(shaftWidth);
        const bool rootOnShaft = rachis && params().getBool("rootOnShaft");
        const auto groupId = static_cast<int>(params().getInt("groupId"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool around = vanes.size() == 1 && vanes[0] == 0;
        const int perVane = around ? 2 * count : count;
        const std::size_t barbPoints = segments + 1;
        // The down zone: from start to `down` along the shaft; the vane's barbs begin past it.
        const float downTo = std::min(static_cast<float>(params().getDouble("down")), end);
        const bool hasDown = downTo > start;
        const float vaneFrom = hasDown ? downTo : start;
        const auto downCount = static_cast<int>(params().getInt("downCount"));
        const int perDown = hasDown ? (around ? 2 * downCount : downCount) : 0;
        const float downLength = static_cast<float>(params().getDouble("downLength"));
        const float downCurl = static_cast<float>(params().getDouble("downCurl")) * kDegrees;
        const float downFrizz = static_cast<float>(params().getDouble("downFrizz")) * kDegrees;
        const auto downSegments = static_cast<std::size_t>(params().getInt("downSegments"));

        // Pass 1: every shaft's barbs (where, how long, which split, where they lie in the feather
        // laid flat), independent of the others.
        const std::size_t shaftCount = in->strandCount();
        std::vector<ShaftPlan> plans(shaftCount);
        parallelFor(0, shaftCount, [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s);
            if (n < 2 || perVane + perDown == 0) return;
            const float shaftLength = arcLength(in->strandPoints(s), n);
            if (!(shaftLength > 0.0f) || !std::isfinite(shaftLength)) return;
            const StrandId shaftId = in->ids()[s];
            const float baseLength = std::max(0.0f, lengthField.at(*in, s));
            const float baseAngle = angleField.at(*in, s) * kDegrees;
            const float phase = around ? 6.2831853f * unitFloat(hashCombine(shaftId, seed ^ 0xA70D)) : 0.0f;
            ShaftPlan& plan = plans[s];
            plan.groups.assign(vanes.size(), 0);
            for (std::size_t v = 0; v < vanes.size(); ++v) {
                const int side = vanes[v];
                const float sideScale = side < 0 ? 1.0f - asymmetry : side > 0 ? 1.0f + asymmetry : 1.0f;
                // The down zone's barbs first (root-most), then the vane's.
                const std::size_t firstDown = plan.barbs.size();
                for (int i = 0; i < perDown; ++i) {
                    Barb b;
                    b.side = side;
                    b.index = i;
                    b.vane = static_cast<int>(v);
                    b.down = true;
                    b.id = hashCombine(hashCombine(hashCombine(shaftId, 0xD0E7E5ull), static_cast<std::uint64_t>(side + 2)),
                                       static_cast<std::uint64_t>(i));
                    b.u = start + (downTo - start) * (static_cast<float>(i) + 0.5f) / static_cast<float>(perDown);
                    Rng rng(hashCombine(b.id, seed));
                    // Down is unruly: more varied than the vane, whatever the vane's jitter.
                    const float lengthVary = 1.0f + std::max(lengthRandom, 0.25f) * rng.uniform(-1.0f, 1.0f);
                    const float angleVary = std::max(angleRandom, 15.0f * kDegrees) * rng.uniform(-1.0f, 1.0f);
                    b.length = baseLength * lengthProfile(b.u) * sideScale * downLength * lengthVary;
                    if (!(b.length > 1e-6f * shaftLength) || !std::isfinite(b.length)) continue;
                    b.angle = baseAngle * angleProfile(b.u) + angleVary;
                    b.spin = around ? phase + kGoldenAngle * static_cast<float>(i) : 0.0f;
                    b.bend = downCurl * (0.6f + 0.8f * rng.nextFloat());
                    b.points = downSegments + 1;
                    plan.barbs.push_back(b);
                }
                const std::size_t first = plan.barbs.size();
                for (int i = 0; i < perVane; ++i) {
                    Barb b;
                    b.side = side;
                    b.index = perDown + i;
                    b.vane = static_cast<int>(v);
                    b.id = hashCombine(hashCombine(hashCombine(shaftId, 0xFEA7E5ull), static_cast<std::uint64_t>(side + 2)),
                                       static_cast<std::uint64_t>(i));
                    b.u = vaneFrom + (end - vaneFrom) * (static_cast<float>(i) + 0.5f) / static_cast<float>(perVane);
                    Rng rng(hashCombine(b.id, seed));
                    const float lengthVary = 1.0f + lengthRandom * rng.uniform(-1.0f, 1.0f);
                    const float angleVary = angleRandom * rng.uniform(-1.0f, 1.0f);
                    b.length = baseLength * lengthProfile(b.u) * sideScale * lengthVary;
                    if (!(b.length > 1e-6f * shaftLength) || !std::isfinite(b.length)) continue;  // no vane here
                    b.angle = baseAngle * angleProfile(b.u) + angleVary;
                    b.spin = around ? phase + kGoldenAngle * static_cast<float>(i) : 0.0f;
                    b.bend = curl * curlProfile(b.u);
                    b.points = barbPoints;
                    plan.barbs.push_back(b);
                }
                // Splits: a cut before a barb with the chance that makes `splits` groups on the vane.
                const std::size_t inVane = plan.barbs.size() - first;
                const float cut = splits > 1.0f && inVane > 1
                                      ? std::min(1.0f, (splits - 1.0f) / static_cast<float>(inVane - 1))
                                      : 0.0f;
                int group = 0;
                for (std::size_t k = first; k < plan.barbs.size(); ++k) {
                    if (k > first && cut > 0.0f && unitFloat(hashCombine(plan.barbs[k].id, seed ^ 0x5B17)) < cut) ++group;
                    plan.barbs[k].group = group;
                }
                plan.groups[v] = inVane > 0 ? group + 1 : 0;
                if (first > firstDown) {  // the down barbs: a clump of their own after the vane's splits
                    for (std::size_t k = firstDown; k < first; ++k) plan.barbs[k].group = plan.groups[v];
                    ++plan.groups[v];
                }
            }
            // The feather laid flat: each barb's tip from its root, bent as pass 2 bends it (what
            // leaves the vane foreshortened), and the farthest reach on each axis.
            plan.reachAlong = shaftLength;
            for (Barb& b : plan.barbs) {
                const float step = 1.0f / static_cast<float>(b.points - 1);
                float along = 0.0f, across = 0.0f;
                for (std::size_t j = 1; j < b.points; ++j) {
                    float kinkIn = 0.0f, kinkOut = 0.0f, theta = 0.0f, rise = 0.0f;
                    downKinks(b, seed, downFrizz, j, &kinkIn, &kinkOut);
                    barbTurn(b, (static_cast<float>(j) - 0.5f) * step, lift, droop, kinkIn, kinkOut, &theta, &rise);
                    const float flat = pm::cos(rise) * b.length * step;
                    along += pm::cos(theta) * flat;
                    across += pm::sin(theta) * flat;
                }
                b.tipAlong = along;
                b.tipAcross = std::abs(across);
                plan.reachAlong = std::max(plan.reachAlong, b.u * shaftLength + along);
                plan.reachAcross = std::max(plan.reachAcross, b.tipAcross);
                plan.keptPoints += b.points;
            }
            plan.keptBarbs = plan.barbs.size();
        }, 1);  // a shaft's barbs are heavy work and shafts few (hand-drawn): one per task

        // The viewport's point budget: thin the barbs (by ID, stable) to fit.
        double totalPoints = 0.0;
        for (std::size_t s = 0; s < shaftCount; ++s) {
            totalPoints += static_cast<double>(plans[s].keptPoints) +
                           (keepShaft ? static_cast<double>(in->strandPointCount(s)) : 0.0);
        }
        if (ctx.maxPoints > 0.0 && totalPoints > ctx.maxPoints) {
            const auto keep = static_cast<float>(ctx.maxPoints / totalPoints);
            parallelFor(0, shaftCount, [&](std::size_t s) {
                std::size_t kept = 0, points = 0;
                for (Barb& b : plans[s].barbs) {
                    b.kept = keepAtDensity(b.id, keep);
                    kept += b.kept ? 1 : 0;
                    points += b.kept ? b.points : 0;
                }
                plans[s].keptBarbs = kept;
                plans[s].keptPoints = points;
            }, 1);
            totalPoints = 0.0;
            for (std::size_t s = 0; s < shaftCount; ++s) {
                totalPoints += static_cast<double>(plans[s].keptPoints) +
                               (keepShaft ? static_cast<double>(in->strandPointCount(s)) : 0.0);
            }
        }
        if (totalPoints > kMaxFeatherPoints) {
            char message[256];
            std::snprintf(message, sizeof message,
                          "Propagate: %.0fM points is more than memory holds: fewer barbs (count), fewer segments or "
                          "fewer shafts", totalPoints / 1e6);
            throw GraphError(message);
        }

        // Where each shaft's strands start in the output, and every strand's point count.
        std::vector<std::size_t> firstStrand(shaftCount + 1, 0);
        std::vector<int> firstGroup(shaftCount + 1, 0);
        for (std::size_t s = 0; s < shaftCount; ++s) {
            firstStrand[s + 1] = firstStrand[s] + (keepShaft ? 1 : 0) + plans[s].keptBarbs;
            int groups = 0;
            for (const int g : plans[s].groups) groups += g;
            firstGroup[s + 1] = firstGroup[s] + groups;
        }
        // Each shaft's feather number and each of its vanes' (or splits') clump numbers, from the
        // shaft's ID: an edit of the scalp in one place renumbers no feather elsewhere.
        std::vector<std::uint64_t> keys(shaftCount);
        for (std::size_t s = 0; s < shaftCount; ++s) keys[s] = in->ids()[s];
        const std::vector<float> featherNumbers = stableGroupNumbers(keys);
        keys.assign(static_cast<std::size_t>(firstGroup[shaftCount]), 0);
        for (std::size_t s = 0; s < shaftCount; ++s) {
            for (int g = firstGroup[s]; g < firstGroup[s + 1]; ++g) {
                keys[static_cast<std::size_t>(g)] = hashCombine(hash64(in->ids()[s] ^ 0xFEA7Cull), static_cast<std::uint64_t>(g - firstGroup[s]));
            }
        }
        const std::vector<float> groupNumbers = stableGroupNumbers(keys);
        std::vector<std::size_t> pointCounts(firstStrand[shaftCount]);
        for (std::size_t s = 0; s < shaftCount; ++s) {
            std::size_t o = firstStrand[s];
            if (keepShaft) pointCounts[o++] = in->strandPointCount(s);
            for (const Barb& b : plans[s].barbs) {
                if (b.kept) pointCounts[o++] = b.points;
            }
        }

        auto out = std::make_shared<StrandSet>();
        // The shaft's channels come along (with their defaults), then the feather's own.
        const std::vector<std::string> strandNames = in->strandChannelNames();
        const std::vector<std::string> pointNames = in->pointChannelNames();
        std::vector<std::pair<const std::vector<float>*, std::vector<float>*>> strandCopies, pointCopies;
        for (const std::string& name : strandNames) {
            if (!writtenChannel(name)) out->addStrandChannel(name, in->strandChannelDefault(name));
        }
        for (const std::string& name : pointNames) out->addPointChannel(name, in->pointChannelDefault(name));
        out->addStrandChannel(kFeatherIdChannel, -1.0f);
        out->addStrandChannel(kShaftUChannel, 0.0f);
        out->addStrandChannel(kSideChannel, 0.0f);
        out->addStrandChannel(kBarbIndexChannel, -1.0f);
        out->addStrandChannel(kIsShaftChannel, 0.0f);
        out->addStrandChannel(kClumpChannel, -1.0f);
        out->addStrandChannel(kVaneUChannel, 0.0f);
        out->addStrandChannel(kVaneTipUChannel, 0.0f);
        out->addStrandChannel(kVaneTipVChannel, 0.0f);
        if (hasDown) out->addStrandChannel(kIsDownChannel, 0.0f);
        if (rachis && keepShaft) out->addStrandChannel(kRachisChannel, 0.0f);
        for (const char* name : kVaneNormalChannels) out->addStrandChannel(name, 0.0f);
        out->resize(pointCounts);
        for (const std::string& name : strandNames) {
            if (!writtenChannel(name)) strandCopies.emplace_back(in->strandChannel(name), out->mutableStrandChannel(name));
        }
        for (const std::string& name : pointNames) pointCopies.emplace_back(in->pointChannel(name), out->mutablePointChannel(name));
        std::vector<float>& featherIds = *out->mutableStrandChannel(kFeatherIdChannel);
        std::vector<float>& shaftUs = *out->mutableStrandChannel(kShaftUChannel);
        std::vector<float>& sidesOut = *out->mutableStrandChannel(kSideChannel);
        std::vector<float>& barbIndices = *out->mutableStrandChannel(kBarbIndexChannel);
        std::vector<float>& isShaft = *out->mutableStrandChannel(kIsShaftChannel);
        std::vector<float>* rachisShafts = out->mutableStrandChannel(kRachisChannel);  // (with a rachis kept)
        std::vector<float>& clumps = *out->mutableStrandChannel(kClumpChannel);
        std::vector<float>& vaneUs = *out->mutableStrandChannel(kVaneUChannel);
        std::vector<float>& tipUs = *out->mutableStrandChannel(kVaneTipUChannel);
        std::vector<float>& tipVs = *out->mutableStrandChannel(kVaneTipVChannel);
        std::vector<float>* isDown = hasDown ? out->mutableStrandChannel(kIsDownChannel) : nullptr;
        std::array<std::vector<float>*, 3> vaneNormals{};
        for (int k = 0; k < 3; ++k) vaneNormals[static_cast<std::size_t>(k)] = out->mutableStrandChannel(kVaneNormalChannels[k]);
        auto setVaneNormal = [&](std::size_t strand, const Vec3f& n) {
            (*vaneNormals[0])[strand] = n.x;
            (*vaneNormals[1])[strand] = n.y;
            (*vaneNormals[2])[strand] = n.z;
        };
        std::vector<StrandId>& ids = out->mutableIds();
        std::vector<StrandRoot>& roots = out->mutableRoots();
        std::vector<Vec3f>& positions = out->mutablePositions();
        std::vector<float>& widths = out->mutableWidths();
        const bool shaftWidths = in->widths().size() == in->pointCount();

        // Pass 2: each shaft writes its own strands (and, with a shaftWidth, its rachis rings).
        std::vector<std::vector<RachisRing>> shaftRings(shaftCount);
        parallelFor(0, shaftCount, [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s);
            const Vec3f* pts = in->strandPoints(s);
            const std::size_t begin = in->strandBegin(s);
            std::size_t o = firstStrand[s];
            auto copyStrandChannels = [&](std::size_t dst) {
                for (const auto& [from, to] : strandCopies) (*to)[dst] = (*from)[s];
            };
            if (keepShaft) {
                ids[o] = in->ids()[s];
                roots[o] = in->roots()[s];
                const std::size_t dst = out->strandBegin(o);
                std::copy(pts, pts + n, positions.begin() + static_cast<std::ptrdiff_t>(dst));
                if (shaftWidths) {
                    std::copy(in->widths().begin() + static_cast<std::ptrdiff_t>(begin),
                              in->widths().begin() + static_cast<std::ptrdiff_t>(begin + n),
                              widths.begin() + static_cast<std::ptrdiff_t>(dst));
                }
                for (const auto& [from, to] : pointCopies) {
                    std::copy(from->begin() + static_cast<std::ptrdiff_t>(begin),
                              from->begin() + static_cast<std::ptrdiff_t>(begin + n),
                              to->begin() + static_cast<std::ptrdiff_t>(dst));
                }
                copyStrandChannels(o);
                featherIds[o] = featherNumbers[s];
                isShaft[o] = 1.0f;
                // The shaft in vane space: up the middle, root to its own end.
                const float reach = plans[s].reachAlong;
                tipUs[o] = reach > 0.0f && n >= 2 ? std::min(1.0f, arcLength(pts, n) / reach) : 1.0f;
                ++o;
            }
            const ShaftPlan& plan = plans[s];
            const float shaftLength = n >= 2 ? arcLength(pts, n) : 0.0f;
            if (!(shaftLength > 0.0f) || !std::isfinite(shaftLength)) return;
            if (plan.barbs.empty() && !rachis) return;
            const ShaftWalker walker(pts, n, rootUp(scalp, in->roots()[s], pts, n));
            if (keepShaft) setVaneNormal(firstStrand[s], walker.frame(0).normal);
            // The rachis: a ring at every shaft point, in the frames the barbs grow in.
            std::vector<RachisRing>& rings = shaftRings[s];
            if (rachis) {
                rings.resize(n);
                for (std::size_t k = 0; k < n; ++k) {
                    const Frame& fr = walker.frame(k);
                    RachisRing& r = rings[k];
                    r.center = pts[k];
                    r.tangent = fr.tangent;
                    r.normal = fr.normal;
                    r.side = fr.binormal;
                    r.u = walker.along(k);
                    r.radius = std::max(0.5f * shaftWidth * shaftWidthProfile(r.u), kMinRachisRadius * 0.5f * shaftWidth);
                }
                // The kept shaft as wide as its rachis, point by point: a renderer drawing it as a
                // curve draws the rachis (a Width node after leaves it so).
                if (keepShaft) {
                    const std::size_t dst = out->strandBegin(firstStrand[s]);
                    for (std::size_t k = 0; k < n; ++k) widths[dst + k] = 2.0f * rings[k].radius;
                    (*rachisShafts)[firstStrand[s]] = 1.0f;
                }
            }
            if (plan.barbs.empty()) return;
            const float step = 1.0f / static_cast<float>(segments);

            // One vane at a time: every barb's points (the thinned ones too: they shape their split).
            thread_local std::vector<Vec3f> vanePoints, centers;
            std::size_t b0 = 0;
            int groupBase = firstGroup[s];
            for (std::size_t v = 0; v < vanes.size(); ++v) {
                std::size_t b1 = b0, stride = barbPoints;  // stride: the vane's longest barb (down barbs have more)
                while (b1 < plan.barbs.size() && plan.barbs[b1].vane == static_cast<int>(v)) stride = std::max(stride, plan.barbs[b1++].points);
                vanePoints.assign((b1 - b0) * stride, Vec3f{});
                for (std::size_t k = b0; k < b1; ++k) {
                    const Barb& b = plan.barbs[k];
                    const ShaftFrame f = walker.at(b.u);
                    // The barb's sideways direction, and the one it lifts toward.
                    const Vec3f across = b.side == 0 ? f.side * pm::cos(b.spin) + f.normal * pm::sin(b.spin)
                                                     : f.side * static_cast<float>(b.side);
                    const float barbStep = b.down ? 1.0f / static_cast<float>(b.points - 1) : step;
                    Vec3f* q = vanePoints.data() + (k - b0) * stride;
                    q[0] = f.point;
                    for (std::size_t j = 1; j < b.points; ++j) {
                        const float t = (static_cast<float>(j) - 0.5f) * barbStep;  // the segment's middle
                        float kinkIn = 0.0f, kinkOut = 0.0f, theta = 0.0f, rise = 0.0f;
                        downKinks(b, seed, downFrizz, j, &kinkIn, &kinkOut);
                        barbTurn(b, t, lift, droop, kinkIn, kinkOut, &theta, &rise);
                        const Vec3f inPlane = f.tangent * pm::cos(theta) + across * pm::sin(theta);
                        const Vec3f dir = inPlane * pm::cos(rise) + f.normal * pm::sin(rise);
                        q[j] = q[j - 1] + dir * (b.length * barbStep);
                    }
                    if (rootOnShaft) {
                        // Out of the rachis: the whole barb moves to where its first segment's way
                        // across the shaft leaves the tube (its length and shape as designed).
                        std::size_t seg = 0;
                        float frac = 0.0f;
                        walker.locate(b.u, &seg, &frac);
                        const RachisRing& ra = rings[seg];
                        Vec3f away = b.points > 1 ? q[1] - q[0] : across;
                        away = away - ra.tangent * away.dot(ra.tangent);
                        if (!(away.length() > 1e-6f * shaftLength)) away = across - ra.tangent * across.dot(ra.tangent);
                        const float angle = pm::atan2(away.dot(ra.normal), away.dot(ra.side));
                        const Vec3f shift = rachisSurfacePoint(ra, rings[seg + 1], frac, angle, shaftSides) - q[0];
                        for (std::size_t j = 0; j < b.points; ++j) q[j] += shift;
                    }
                }
                // Splits: each barb's tip gathers toward the middle barb of its split.
                const int splitGroups = plan.groups[v] - (b1 > b0 && plan.barbs[b0].down ? 1 : 0);
                if (splitAmount > 0.0f && splitGroups > 1) {
                    centers = vanePoints;
                    std::size_t g0 = b0;
                    while (g0 < b1) {
                        std::size_t g1 = g0;
                        while (g1 < b1 && plan.barbs[g1].group == plan.barbs[g0].group) ++g1;
                        if (plan.barbs[g0].down) {  // down barbs gather into no split
                            g0 = g1;
                            continue;
                        }
                        const Vec3f* c = centers.data() + ((g0 + g1) / 2 - b0) * stride;
                        for (std::size_t k = g0; k < g1; ++k) {
                            Vec3f* q = vanePoints.data() + (k - b0) * stride;
                            for (std::size_t j = 1; j < plan.barbs[k].points; ++j) {
                                const float w = splitAmount * static_cast<float>(j) * step;
                                q[j] += (c[j] - q[j]) * w;
                            }
                        }
                        g0 = g1;
                    }
                }
                for (std::size_t k = b0; k < b1; ++k) {
                    const Barb& b = plan.barbs[k];
                    if (!b.kept) continue;
                    ids[o] = b.id;
                    roots[o] = in->roots()[s];
                    const std::size_t dst = out->strandBegin(o);
                    const Vec3f* q = vanePoints.data() + (k - b0) * stride;
                    std::copy(q, q + b.points, positions.begin() + static_cast<std::ptrdiff_t>(dst));
                    std::size_t seg = 0;
                    float frac = 0.0f;
                    walker.locate(b.u, &seg, &frac);
                    auto along = [&](const std::vector<float>& values) {
                        const float a = values[begin + seg], c = values[begin + seg + 1];
                        return a + (c - a) * frac;
                    };
                    const float w = width > 0.0f ? width : shaftWidths ? along(in->widths()) : 1.0f;
                    std::fill(widths.begin() + static_cast<std::ptrdiff_t>(dst),
                              widths.begin() + static_cast<std::ptrdiff_t>(dst + b.points), w);
                    for (const auto& [from, to] : pointCopies) {
                        std::fill(to->begin() + static_cast<std::ptrdiff_t>(dst),
                                  to->begin() + static_cast<std::ptrdiff_t>(dst + b.points), along(*from));
                    }
                    copyStrandChannels(o);
                    featherIds[o] = featherNumbers[s];
                    shaftUs[o] = b.u;
                    sidesOut[o] = static_cast<float>(b.side);
                    barbIndices[o] = static_cast<float>(b.index);
                    clumps[o] = groupNumbers[static_cast<std::size_t>(groupBase + b.group)];
                    // Vane space: the root on the shaft, the tip where the barb reaches laid flat.
                    const float reachAlong = plan.reachAlong, reachAcross = plan.reachAcross;
                    vaneUs[o] = reachAlong > 0.0f ? b.u * shaftLength / reachAlong : 0.0f;
                    tipUs[o] = reachAlong > 0.0f ? (b.u * shaftLength + b.tipAlong) / reachAlong : 0.0f;
                    tipVs[o] = reachAcross > 0.0f ? b.tipAcross / reachAcross : 0.0f;
                    if (isDown) (*isDown)[o] = b.down ? 1.0f : 0.0f;
                    setVaneNormal(o, walker.at(b.u).normal);
                    ++o;
                }
                groupBase += plan.groups[v];
                b0 = b1;
            }
        }, 1);
        if (keepShaft) out->setInstances(in->instances());  // instances sit on the shafts, which stay
        for (const auto& layer : in->cardLayers()) out->addCardLayer(layer);
        for (const auto& layer : in->featherLayers()) out->addFeatherLayer(layer);
        // The groom group: every strand made here, and the layer naming it (with the rachis).
        if (groupId > 0) {
            std::vector<float>* groups = out->mutableStrandChannel(kGroupChannel);
            if (!groups) groups = &out->addStrandChannel(kGroupChannel, 0.0f);
            std::fill(groups->begin(), groups->end(), static_cast<float>(groupId));
        }
        if (rachis || groupId > 0) {
            auto layer = std::make_shared<FeatherLayer>();
            layer->node = nodePath();
            layer->groupId = groupId;
            layer->groupName = params().getString("groupName");
            layer->sides = shaftSides;
            for (std::size_t s = 0; s < shaftCount; ++s) {
                if (shaftRings[s].size() < 2) continue;
                layer->rings.insert(layer->rings.end(), shaftRings[s].begin(), shaftRings[s].end());
                layer->offsets.push_back(layer->rings.size());
                layer->featherIds.push_back(static_cast<int>(featherNumbers[s]));
            }
            out->addFeatherLayer(std::move(layer));
        }
        return StrandsPtr(out);
    }
};

// Routes each strand to one of four outputs (a, b, c, d) by the shares their weights give: at
// random by strand ID (stable), or by a map's 0-1 value (`select`: where it falls among the
// shares, so weights 1 1 0 0 send values under 0.5 to a and the rest to b). Every strand goes to
// exactly one output. Strands or guides, as fed.
class SplitOp : public Operator {
public:
    static constexpr std::size_t kOutputs = 4;

    std::string typeName() const override { return op::kSplit; }
    std::string category() const override { return "utility"; }
    // 2: select 1 by default, so a map plugged into it reads as itself (a mappable param scales its
    // map by its own value; nodes saved before keep 0: ParamSpec::formerDefault).
    int version() const override { return 2; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"a", PortType::Curves}; }
    std::vector<PortSpec> outputs() const override {
        return {{"a", PortType::Curves}, {"b", PortType::Curves}, {"c", PortType::Curves}, {"d", PortType::Curves}};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"mode", std::string("random"), "random: each strand's output picked by its ID; map: by select", "",
             {"random", "map"}},
            notAnimatable({"weightA", 1.0, "Share of the strands sent to a"}).atLeast(0).soft(0, 1),
            notAnimatable({"weightB", 1.0, "Share of the strands sent to b"}).atLeast(0).soft(0, 1),
            notAnimatable({"weightC", 0.0, "Share of the strands sent to c"}).atLeast(0).soft(0, 1),
            notAnimatable({"weightD", 0.0, "Share of the strands sent to d"}).atLeast(0).soft(0, 1),
            notAnimatable({"select", 1.0, "With mode map: a 0-1 value per strand, where it falls among the shares picks the "
                                          "output (mappable: a painted map, a channel)"})
                .range(0, 1)
                .formerDefault(2, 0.0)
                .when("mode", {"map"}),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv()
                .when("mode", {"random"}),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        return evaluateOutputs(ctx, inputs)[0];
    }

    std::vector<Value> evaluateOutputs(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kSplit);
        const Picker picker = pickerFor(ctx, *in);
        std::vector<int> choice(in->strandCount());
        parallelFor(0, in->strandCount(), [&](std::size_t s) { choice[s] = picker.pick(*in, s); });
        auto members = std::make_shared<Members>();
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            (*members)[static_cast<std::size_t>(choice[s])].push_back(static_cast<std::uint32_t>(s));
        }
        std::vector<Value> out(kOutputs);
        for (std::size_t k = 0; k < kOutputs; ++k) out[k] = outputOf(in, (*members)[k]);
        remember(out, std::move(members));
        return out;
    }

    // A dab changes some strands' points, not their IDs: each changed strand that stays in its
    // output is written over its place in that output's last result; an output a changed strand
    // left or joined (a select read from what the dab changed) is cut again in full and says so.
    bool evaluateOutputsIncremental(const EvalContext& ctx, const std::vector<Value>& inputs,
                                    const IncrementalInputs& incremental, std::vector<Value>* outs,
                                    std::vector<StrandChanges>* changed) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kSplit);
        const StrandChanges* changes = someStrandsChanged(incremental, 0);
        if (!changes || incremental.previousOutputs.size() != kOutputs) return false;
        Made made;
        {
            const std::lock_guard<std::mutex> lock(madeMutex_);
            made = made_;
        }
        if (!made.members) return false;
        std::array<StrandsPtr, kOutputs> last;
        std::size_t held = 0;
        for (std::size_t k = 0; k < kOutputs; ++k) {
            last[k] = valueAs<StrandsPtr>(incremental.previousOutputs[k]);
            if (!last[k] || made.outputs[k].lock() != last[k] || last[k]->strandCount() != (*made.members)[k].size()) {
                return false;
            }
            held += last[k]->strandCount();
        }
        if (held != in->strandCount()) return false;
        const Picker picker = pickerFor(ctx, *in);
        if (picker.select && !picker.select->strandLocal()) return false;
        // Each changed strand's output before (its place there) and now.
        std::array<std::vector<std::size_t>, kOutputs> pick;
        std::array<std::vector<std::uint32_t>, kOutputs> at, joined, left;
        std::array<bool, kOutputs> cut{};  // a strand left or joined: the output in full
        for (const std::uint32_t s : changes->strands) {
            if (s >= in->strandCount()) return false;
            std::size_t was = kOutputs, place = 0;
            for (std::size_t k = 0; k < kOutputs && was == kOutputs; ++k) {
                const auto& list = (*made.members)[k];
                const auto it = std::lower_bound(list.begin(), list.end(), s);
                if (it != list.end() && *it == s) {
                    was = k;
                    place = static_cast<std::size_t>(it - list.begin());
                }
            }
            if (was == kOutputs) return false;
            const auto now = static_cast<std::size_t>(picker.pick(*in, s));
            if (now != was) {
                left[was].push_back(s);
                joined[now].push_back(s);
                cut[was] = cut[now] = true;
                continue;
            }
            pick[was].push_back(s);
            at[was].push_back(static_cast<std::uint32_t>(place));
        }
        std::shared_ptr<const Members> members = made.members;
        if (std::find(cut.begin(), cut.end(), true) != cut.end()) {
            // The outputs strands left or joined, their strands again (each list in strand order).
            auto next = std::make_shared<Members>();
            for (std::size_t k = 0; k < kOutputs; ++k) {
                const auto& before = (*made.members)[k];
                if (!cut[k]) {
                    (*next)[k] = before;
                    continue;
                }
                std::vector<std::uint32_t> kept;
                kept.reserve(before.size());
                std::set_difference(before.begin(), before.end(), left[k].begin(), left[k].end(), std::back_inserter(kept));
                auto& list = (*next)[k];
                list.reserve(kept.size() + joined[k].size());
                std::merge(kept.begin(), kept.end(), joined[k].begin(), joined[k].end(), std::back_inserter(list));
            }
            members = std::move(next);
        }
        outs->assign(kOutputs, Value{});
        changed->assign(kOutputs, StrandChanges{});
        for (std::size_t k = 0; k < kOutputs; ++k) {
            Value& out = (*outs)[k];
            if (cut[k]) {
                out = outputOf(in, (*members)[k]);  // its strands changed: all of it may differ
            } else if ((*members)[k].size() == in->strandCount()) {
                out = in;  // every strand, the input as it came: it changed as the input did
                (*changed)[k] = *changes;
            } else if (at[k].empty()) {
                out = last[k];
                (*changed)[k] = StrandChanges::listed({});
            } else {
                auto made2 = copyOfLast(last[k], incremental);
                writeStrands(*made2, in->subset(pick[k]), at[k]);
                out = StrandsPtr(made2);
                (*changed)[k] = StrandChanges::listed(std::move(at[k]));
            }
        }
        remember(*outs, std::move(members));
        return true;
    }
    void adoptKeptWork(const Operator& previous) override {
        const auto* from = dynamic_cast<const SplitOp*>(&previous);
        if (!from || from == this) return;
        Made made;
        {
            const std::lock_guard<std::mutex> lock(from->madeMutex_);
            made = from->made_;
        }
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (!made_.members) made_ = std::move(made);
    }

private:
    // The input strands each output holds, in strand order.
    using Members = std::array<std::vector<std::uint32_t>, kOutputs>;

    // Each strand's output, from the weights (and with mode map, the select map).
    struct Picker {
        float bounds[kOutputs] = {};
        int last = -1;  // the last output with a share: values at the very top go there (none: all to a)
        std::unique_ptr<ScalarField> select;  // map mode only
        std::uint64_t seed = 0;

        int pick(const StrandSet& in, std::size_t s) const {
            if (last < 0) return 0;
            const float v = select ? std::clamp(select->at(in, s), 0.0f, 1.0f)
                                   : unitFloat(hashCombine(in.ids()[s], seed ^ 0x5B11));
            for (std::size_t i = 0; i < kOutputs; ++i) {
                if (v < bounds[i] && bounds[i] > (i > 0 ? bounds[i - 1] : 0.0f)) return static_cast<int>(i);
            }
            return last;
        }
    };
    Picker pickerFor(const EvalContext& ctx, const StrandSet& in) const {
        const std::string mode = params().getString("mode");
        if (mode != "random" && mode != "map") throw GraphError("Split: mode must be random or map");
        const char* names[kOutputs] = {"weightA", "weightB", "weightC", "weightD"};
        const double defaults[kOutputs] = {1.0, 1.0, 0.0, 0.0};
        Picker picker;
        float total = 0.0f;
        for (std::size_t k = 0; k < kOutputs; ++k) {
            const float w = static_cast<float>(params().getDouble(names[k], defaults[k]));
            total += std::isfinite(w) ? w : 0.0f;
            picker.bounds[k] = total;
            if (w > 0.0f) picker.last = static_cast<int>(k);
        }
        if (picker.last < 0) return picker;  // no shares: everything to a
        for (float& b : picker.bounds) b /= total;
        if (mode == "map") {  // a map is loaded (and checked) only then
            picker.select = std::make_unique<ScalarField>(params(), "select", scalpOf(ctx));
            picker.select->validate(in);
        }
        picker.seed = static_cast<std::uint64_t>(params().getInt("seed"));
        return picker;
    }
    // An output holding `list` of the input's strands: the input itself when it holds them all.
    static Value outputOf(const StrandsPtr& in, const std::vector<std::uint32_t>& list) {
        if (list.size() == in->strandCount()) return in;
        return StrandsPtr(std::make_shared<StrandSet>(in->subset(std::vector<std::size_t>(list.begin(), list.end()))));
    }
    void remember(const std::vector<Value>& outs, std::shared_ptr<const Members> members) const {
        Made made;
        for (std::size_t k = 0; k < kOutputs; ++k) made.outputs[k] = valueAs<StrandsPtr>(outs[k]);
        made.members = std::move(members);
        const std::lock_guard<std::mutex> lock(madeMutex_);
        made_ = std::move(made);
    }

    // The last results (by identity) and the input strands each holds.
    struct Made {
        std::array<std::weak_ptr<const StrandSet>, kOutputs> outputs;
        std::shared_ptr<const Members> members;
    };
    mutable std::mutex madeMutex_;
    mutable Made made_;
};

}  // namespace

void registerPropagateOperator(OperatorRegistry& registry) {
    registry.registerType(op::kPropagate,
                          {"feathers",
                           "Grows barbs off every strand it is fed (the shafts): feathers, ferns, plumes.",
                           "barbs fern feather"},
                          [] { return std::make_unique<PropagateOp>(); });
}

void registerSplitOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSplit,
                          {"utility",
                           "Sends each strand to one of four outputs by share or by a map: several designs on one set.",
                           "outputs share designs"},
                          [] { return std::make_unique<SplitOp>(); });
}

}  // namespace groomlab
