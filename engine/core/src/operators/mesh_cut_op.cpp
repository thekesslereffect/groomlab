// SPDX-License-Identifier: Apache-2.0
// MeshCut (XGen's Mesh Cut): strands trimmed where they cross a mesh. With `keep` outside (an
// obstacle: a body, a collar) each strand is cut where it first goes into the mesh; with inside (a
// hull: a haircut's shape) where it first comes out of it. A strand rooted on the side cut away
// is cut to nothing (every point at its root). The kept part, `offset` shorter, is spread over the
// strand's points again (evenly by length), so a strand keeps its point count, IDs and widths
// along it: each strand is cut alone, and a dab cuts again only the strands it moved.
//
// Where a strand crosses: each segment, root to tip, as a ray against the mesh's closest-point
// tree (ScalpMesh::intersectRay: the tree Collide's exact method and the brushes search). Which
// side a root is on: the closest point's normal there.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "groomlab/curve.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

class MeshCutOp : public Operator {
public:
    std::string typeName() const override { return op::kMeshCut; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}, {"mesh", PortType::Mesh, true}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"keep", std::string("outside"),
             "outside: the mesh is an obstacle, strands are cut where they first go into it; inside: the mesh is a hull "
             "(a haircut's shape), strands are cut where they first come out of it. A strand rooted on the side cut away "
             "is cut to nothing (with nothing on the mesh input the strands pass through)", "", {"outside", "inside"}},
            ParamSpec{"offset", 0.0, "Cut this much shorter than the crossing, along the strand", "length"}.atLeast(0).soft(0, 1),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand is cut against the mesh alone: with the same mesh in, only the strands the input
    // changed in are cut again.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kMeshCut);
        const MeshPtr mesh = inputs.size() > 1 ? valueAs<MeshPtr>(inputs[1]) : nullptr;
        if (!mesh || mesh->triangleCount() == 0 || in->empty()) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            if (!mesh) ctx.note("no mesh: connect a mesh (a ColliderMesh) to cut by; the strands pass through");
            *result = in;
            return true;
        }
        const bool keepInside = params().getString("keep") == "inside";
        const auto offset = static_cast<float>(params().getDouble("offset"));

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !inputUnchanged(*incremental, 1)) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        if (!last && gpu::cpuEngineOn()) {  // look.slang's MeshCutCS, the GPU preview's own (look_cpu.h)
            gpu::StageInputs tree;
            tree.meshCut = gpu::meshCutInputs(mesh);
            if (StrandsPtr made = gpu::runStage(gpu::StageKind::MeshCut, params(), in, nullptr, std::move(tree))) {
                *result = made;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->mutablePositions();
        }
        auto cut = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 2) return;
            Vec3f* pts = out->strandPoints(s);
            // Which side the root is on: on the surface (or within a hair of it) counts as outside.
            const SurfacePoint near = mesh->closestPoint(pts[0]);
            // A hair: a ten-thousandth of the root's segment, or the rounding of where it is (both
            // scale with the scene).
            const float hair = std::max(1e-4f * (pts[1] - pts[0]).length(),
                                        4e-6f * std::max({std::abs(pts[0].x), std::abs(pts[0].y), std::abs(pts[0].z)}));
            const bool rootInside = near.root.face >= 0 && (pts[0] - near.position).dot(near.normal) < -hair;
            if (rootInside != keepInside) {  // rooted where the mesh cuts away: nothing kept
                for (std::size_t i = 1; i < n; ++i) pts[i] = pts[0];
                return;
            }
            // The first crossing, root to tip: its length along the strand.
            float along = 0.0f, crossing = -1.0f;
            for (std::size_t i = 1; i < n && crossing < 0.0f; ++i) {
                const Vec3f segment = pts[i] - pts[i - 1];
                const float length = segment.length();
                if (!(length > 0.0f)) continue;
                SurfacePoint hit;
                float t = 0.0f;
                // A root on the surface itself (the mesh is the scalp) starts on it: not a crossing.
                const float skip = i == 1 ? 1e-4f : 0.0f;
                const Vec3f from = pts[i - 1] + segment * skip;
                if (mesh->intersectRay(from, segment * (1.0f - skip), &hit, &t) && t <= 1.0f) {
                    crossing = along + length * (skip + t * (1.0f - skip));
                }
                along += length;
            }
            if (crossing < 0.0f) return;  // never crosses: whole
            const float total = arcLength(pts, n);
            if (!(total > 0.0f)) return;
            const float kept = std::max(crossing - offset, 0.0f);
            thread_local std::vector<Vec3f> source;
            source.assign(pts, pts + n);
            resampleByArcLengthInto(source.data(), n, pts, n, std::min(kept / total, 1.0f));
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                cut(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), cut);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

}  // namespace

void registerMeshCutOperator(OperatorRegistry& registry) {
    registry.registerType(op::kMeshCut,
                          {"collide",
                           "Cuts strands where they cross a mesh: an obstacle, or a haircut's hull.",
                           "cut hull obstacle trim"},
                          [] { return std::make_unique<MeshCutOp>(); });
}

}  // namespace groomlab
