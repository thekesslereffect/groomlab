// SPDX-License-Identifier: Apache-2.0
// SkinResponse: fur reacting to the skin under it as the scalp deforms. Where the skin squashes (an
// armpit, the inside of a bent knee, a neck turned) the strands rise toward its normal; where it
// stretches they lie down; and they lean away from where the stretch peaks, so a part opens along
// the outside of a bend. Each strand turns about its root, as its follicle would, all of it with a
// constant profile or more toward the root or the tip with another, its segments' lengths kept.
//
// The squash and stretch are the skin's principal stretches against its rest pose (skin_strain.h),
// read from the external "scalpPose" (the scalp as it is now, with its rest pose: groom.h
// setScalpPoseExternal), or from "scalp" itself when no host hands that in. While the groom follows
// the scalp the graph runs on the rest pose and this node turns the strands there, about the rest
// pose's normals and along its surface; the session then carries each strand to the scalp by its
// root's frame. Without a rest pose the strands pass through; at rest (and wherever a strand's skin
// did not change) its points pass through to the bit. No cache: each frame is worked out from the
// scalp at that frame alone.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/math.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/skin_strain.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kRadiansPerDegree = 0.017453292519943295f;
constexpr float kQuarterTurn = 1.5707963267948966f;

// The mesh the skin's strain is read from: the pose a host hands in, else the scalp itself (a posed
// scalp carrying its rest pose, as the graph sees it when not following); null for neither.
const ScalpMesh* poseOf(const EvalContext& ctx) {
    if (const MeshPtr pose = valueAs<MeshPtr>(ctx.external(kScalpPoseExternal))) return pose.get();
    return scalpOf(ctx);
}

class SkinResponseOp : public Operator {
public:
    std::string typeName() const override { return op::kSkinResponse; }
    std::string category() const override { return "motion"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<std::string> externalInputs() const override { return {"scalp", kScalpPoseExternal}; }

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How much the strands respond to the skin (mappable); 0: not at all"}.soft(0, 2),
            ParamSpec{"lift", 25.0, "Degrees the strands rise toward the skin's normal where it squashes fully, never past "
                                    "the normal",
                      "angle"}
                .range(0, 90),
            ParamSpec{"flatten", 10.0, "Degrees the strands lie down where the skin stretches fully, never below the skin",
                      "angle"}
                .range(0, 90),
            ParamSpec{"spread", 15.0, "Degrees the strands lean away from where the skin stretches most, opening parts "
                                      "along the outside of bends",
                      "angle"}
                .range(0, 90),
            ParamSpec{"range", 0.3, "The squash or stretch that gives the full response: 0.3, the skin 30% shorter or longer "
                                    "than at rest"}
                .range(0.001, 4)
                .soft(0.05, 1),
            ParamSpec{"smoothing", 1.0, "How far the skin's squash and stretch are blurred over the scalp, scene units: "
                                        "larger reacts to broad bends, not single faces",
                      "length"}
                .atLeast(0)
                .soft(0, 5),
            {"profile", Ramp::constant(1.0f), "How much of the turn along the length, root to tip (constant: each strand "
                                              "turns whole about its root, as its follicle does)"},
            ParamSpec{"squashChannel", std::string("skinSquash"), "The strand channel it writes each strand's squash to, "
                                                                  "0-1 of range (empty: none)"}
                .writesChannel()
                .adv(),
            ParamSpec{"stretchChannel", std::string("skinStretch"), "The strand channel it writes each strand's stretch to, "
                                                                    "0-1 of range (empty: none)"}
                .writesChannel()
                .adv(),
        };
    }

    std::string note(const EvalContext& ctx) const override {
        const ScalpMesh* pose = poseOf(ctx);
        if (pose && pose->hasRest()) return {};
        return "no rest pose on the scalp: the strands pass through (give the groom a rest mesh)";
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kSkinResponse);
        const ScalpMesh* scalp = scalpOf(ctx);  // where the strands are: the rest pose while following
        const ScalpMesh* pose = poseOf(ctx);
        if (in->empty() || !scalp || !pose || !pose->hasRest()) return in;
        // The pose's faces are the scalp's (the same mesh, or its rest pose): else nothing to read.
        if (pose->pointCount() != scalp->pointCount() || pose->triangleCount() != scalp->triangleCount()) return in;

        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const auto lift = static_cast<float>(params().getDouble("lift")) * kRadiansPerDegree;
        const auto flatten = static_cast<float>(params().getDouble("flatten")) * kRadiansPerDegree;
        const auto spread = static_cast<float>(params().getDouble("spread")) * kRadiansPerDegree;
        const auto range = static_cast<float>(params().getDouble("range"));
        const auto smoothing = static_cast<float>(params().getDouble("smoothing"));
        const Ramp profile = params().getRamp("profile");
        const std::string squashName = params().getString("squashChannel");
        const std::string stretchName = params().getString("stretchChannel");

        const SkinStrain strain = skinStrain(*pose, skinStrainPasses(*pose, smoothing));
        auto out = std::make_shared<StrandSet>(*in);
        const std::size_t count = out->strandCount();
        if (!squashName.empty()) out->addStrandChannel(squashName, 0.0f);
        if (!stretchName.empty()) out->addStrandChannel(stretchName, 0.0f);
        if (strain.empty()) return StrandsPtr(out);  // at rest: the channels 0, the points as they came
        // (Both added first: adding one may move the other.)
        std::vector<float>* squashOut = squashName.empty() ? nullptr : out->mutableStrandChannel(squashName);
        std::vector<float>* stretchOut = stretchName.empty() ? nullptr : out->mutableStrandChannel(stretchName);

        // Where the stretch falls off fastest, on the mesh the strands are on, and over what length a
        // change of `range` turns the spread full: the smoothing radius, at least an edge.
        const std::vector<Vec3f> slopes = spread > 0.0f ? vertexGradients(*scalp, strain.stretch) : std::vector<Vec3f>{};
        const float across = std::max(smoothing, meanRestEdge(*scalp));

        out->mutablePositions();
        const RampAlong along(profile, *out);
        parallelFor(0, count, [&](std::size_t s) {
            const StrandRoot& root = out->roots()[s];
            if (!scalp->contains(root)) return;
            const float squash = std::clamp(SkinStrain::at(strain.squash, *scalp, root) / range, 0.0f, 1.0f);
            const float stretch = std::clamp(SkinStrain::at(strain.stretch, *scalp, root) / range, 0.0f, 1.0f);
            if (squashOut) (*squashOut)[s] = squash;
            if (stretchOut) (*stretchOut)[s] = stretch;
            const float amount = strength.at(*in, s);
            const std::size_t n = out->strandPointCount(s);
            if (n < 2 || amount == 0.0f) return;
            const Vec3f normal = scalp->normal(root).normalized();
            if (normal == Vec3f{}) return;
            Vec3f* points = out->strandPoints(s);

            // Rise or lie down: about the axis across the strand's lean, never past the normal or the skin.
            Vec3f turn;  // the rotation vector at full profile (axis times radians)
            const Vec3f whole = points[n - 1] - points[0];
            const float up = whole.dot(normal);
            const Vec3f lean = whole - normal * up;
            const float leanLength = lean.length();
            if (leanLength > 0.0f) {
                const float elevation = pm::atan2(up, leanLength);  // -90..90 degrees above the skin
                const float rise = std::min(lift * squash * amount, std::max(0.0f, kQuarterTurn - elevation));
                const float fall = std::min(flatten * stretch * amount, std::max(0.0f, elevation));
                if (rise != fall) turn += (lean / leanLength).cross(normal) * (rise - fall);
            }
            // Lean away from where the stretch peaks: down its slope along the skin.
            if (!slopes.empty()) {
                const Vec3f slope = interpolated(slopes, *scalp, root);
                const Vec3f onSkin = slope - normal * slope.dot(normal);
                const float steep = onSkin.length();
                if (steep > 0.0f) {
                    const float angle = spread * amount * std::min(1.0f, steep * across / range);
                    if (angle != 0.0f) turn += normal.cross(-onSkin / steep) * angle;
                }
            }
            const float angle = turn.length();
            if (!(angle > 0.0f)) return;
            const Vec3f axis = turn / angle;

            // Each segment turned by the profile's share of the angle at its start, chained from the root.
            Vec3f previous = points[0], next = points[0];
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f segment = points[i] - previous;
                previous = points[i];
                next += rotateAxisAngle(segment, axis, angle * along(i - 1, n));
                points[i] = next;
            }
        });
        return StrandsPtr(out);
    }

private:
    static Vec3f interpolated(const std::vector<Vec3f>& values, const ScalpMesh& mesh, const StrandRoot& root) {
        const ScalpMesh::Triangle& tri = mesh.triangles()[static_cast<std::size_t>(root.face)];
        for (const std::int32_t i : tri) {
            if (i < 0 || static_cast<std::size_t>(i) >= values.size()) return {};
        }
        const float w = 1.0f - root.u - root.v;
        return values[static_cast<std::size_t>(tri[0])] * w + values[static_cast<std::size_t>(tri[1])] * root.u +
               values[static_cast<std::size_t>(tri[2])] * root.v;
    }
};

}  // namespace

void registerSkinResponseOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSkinResponse,
                          {"motion",
                           "Fur reacts to the skin as the scalp deforms: rises where it squashes, lies down where it "
                           "stretches, parts along bends.",
                           "skin squash stretch bend fold fur creature follicle deform"},
                          [] { return std::make_unique<SkinResponseOp>(); });
}

}  // namespace groomlab
