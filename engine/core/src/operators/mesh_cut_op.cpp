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
        // look.slang's MeshCutCS, the GPU preview's own (look_cpu.h), on the mesh's ray tree: in full,
        // or the strands the input changed in.
        gpu::StageInputs tree;
        tree.meshCut = gpu::meshCutInputs(mesh);
        if (!last) {
            *result = gpu::runStage(gpu::StageKind::MeshCut, params(), in, nullptr, std::move(tree));
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        gpu::runStageOnStrands(gpu::StageKind::MeshCut, params(), in, changes->strands, nullptr, *out, std::move(tree));
        *changed = *changes;
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
