// SPDX-License-Identifier: Apache-2.0
// Mirror: guides or strands mirrored across a plane, procedurally (symmetry): one side mirrored
// onto the other, or the mirror added to what is there. Each mirrored strand is rooted again on
// the scalp where its mirrored root lands.

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "groomlab/color_patterns.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "internal.h"

namespace groomlab {

namespace {

// Mirrors strands or guides across the plane where coordinate `axis` equals `offset`.
//   replace  the strands rooted on `side` of the plane are kept and mirrored onto the other side,
//            which loses its own: a symmetric groom from one half (Ornatrix's Symmetry)
//   add      every strand is kept and its mirror added
// A strand rooted on the plane (within a rounding) is kept once, not mirrored. The side is read at each root on the
// rest scalp (stable while the scalp deforms). A mirrored strand is the mirror of its points,
// rooted again where its mirrored root lands on the scalp (the closest point: its face and
// barycentrics, so UVs and maps read there) and moved onto it; without a scalp it stays unrooted.
// It keeps the strand's widths and channels, under a new ID of its own (derived from the
// strand's, never one the input has), its clumpId, braidId and featherId numbered after the kept
// strands' (as Merge numbers its second input's). On guides (Guides -> Mirror -> Interpolate's guides) the
// whole groom grows from the mirrored guides and each side gets its own random look; on strands
// after the look, the look itself is mirrored exactly. In full on every evaluation (a dab on
// guides: the Interpolate after it tells which guides changed by itself).
class MirrorOp : public Operator {
public:
    std::string typeName() const override { return op::kMirror; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"axis", std::string("x"), "The plane's normal: x, y or z", "", {"x", "y", "z"}}.world(),
            ParamSpec{"offset", 0.0, "Where the plane crosses that axis (scene units)", "length"}.soft(-10, 10),
            {"mode", std::string("replace"),
             "replace: the side's strands mirrored onto the other side, which loses its own; add: every strand's mirror "
             "added", "", {"replace", "add"}},
            ParamSpec{"side", std::string("positive"), "Replace: the side kept and mirrored (by the roots)", "", {"positive", "negative"}}
                .when("mode", {"replace"}),
            {"reattach", true, "Root each mirrored strand again on the scalp where its mirrored root lands"},
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kMirror);
        const ScalpMesh* scalp = scalpOf(ctx);
        const std::string axisName = params().getString("axis");
        const int axis = axisName == "y" ? 1 : axisName == "z" ? 2 : 0;
        const auto offset = static_cast<float>(params().getDouble("offset"));
        const bool replace = params().getString("mode") != "add";
        const float sign = params().getString("side") == "negative" ? -1.0f : 1.0f;
        const bool reattach = params().getBool("reattach") && scalp && scalp->triangleCount() > 0;

        // Which side of the plane each root is on (the rest scalp's root, else the first point).
        auto sideOf = [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s);
            const Vec3f root = restRootOr(scalp, in->roots()[s], n ? in->strandPoints(s)[0] : Vec3f{});
            const float c = axis == 0 ? root.x : axis == 1 ? root.y : root.z;
            const float d = c - offset;
            // On the plane within a rounding (a root rooted on a mesh lands a little off it): 0.
            if (std::fabs(d) <= 1e-5f * (1.0f + std::fabs(c) + std::fabs(offset))) return 0.0f;
            return d * sign;  // above 0: the kept side's
        };
        std::vector<std::size_t> kept, mirrored;
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            const float side = sideOf(s);
            if (replace && side < 0.0f) continue;  // the other side's own: replaced by the mirror
            kept.push_back(s);
            if (in->strandPointCount(s) > 0 && side != 0.0f) mirrored.push_back(s);
        }
        auto out = std::make_shared<StrandSet>(kept.size() == in->strandCount() ? *in : in->subset(kept));
        if (mirrored.empty()) return StrandsPtr(out);
        StrandSet mirror = in->subset(mirrored);
        // New IDs, derived from the strands' own and apart from every ID the input has.
        std::unordered_set<StrandId> taken(in->ids().begin(), in->ids().end());
        std::vector<StrandId>& ids = mirror.mutableIds();
        const std::uint64_t salt = 0x4D1AAC0Full + static_cast<std::uint64_t>(axis);
        for (StrandId& id : ids) {
            StrandId fresh = hashCombine(id, salt);
            while (taken.count(fresh)) fresh = hash64(fresh + 1);
            taken.insert(fresh);
            id = fresh;
        }
        std::vector<StrandRoot>& roots = mirror.mutableRoots();
        mirror.mutablePositions();  // cloned once here, not by whichever thread writes first
        parallelFor(0, mirror.strandCount(), [&](std::size_t s) {
            const std::size_t n = mirror.strandPointCount(s);
            Vec3f* pts = mirror.strandPoints(s);
            for (std::size_t i = 0; i < n; ++i) pts[i] = mirrorPoint(pts[i], axis, offset);
            if (!reattach) {
                roots[s] = StrandRoot{};
                return;
            }
            const SurfacePoint at = scalp->closestPoint(pts[0]);
            if (at.root.face < 0) {
                roots[s] = StrandRoot{};
                return;
            }
            roots[s] = at.root;
            const Vec3f onto = at.position - pts[0];
            for (std::size_t i = 0; i < n; ++i) pts[i] += onto;
        });
        // A feather's vane normal (Propagate's vaneNormalX/Y/Z: Instance's alongStrand and Plume read
        // it) reflected with its strand.
        if (std::vector<float>* normal = mirror.mutableStrandChannel(kVaneNormalChannels[axis])) {
            for (float& v : *normal) v = -v;
        }
        offsetGroupIds(*out, mirror);  // its clumps, braids and feathers its own, apart from the kept side's
        out->append(mirror);
        return StrandsPtr(out);
    }
};

}  // namespace

void registerMirrorOperator(OperatorRegistry& registry) {
    registry.registerType(op::kMirror,
                          {"shape",
                           "Mirrors guides or strands across a plane: groom one half, get both.",
                           "symmetry symmetric flip half"},
                          [] { return std::make_unique<MirrorOp>(); });
}

}  // namespace groomlab
