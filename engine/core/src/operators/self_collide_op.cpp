// SPDX-License-Identifier: Apache-2.0
// SelfCollide: hair that no longer passes through itself: where hair is packed tighter than it can
// be it is pushed apart, so layers rest on one another. Its grid, solve and push are look.slang's
// hair grid (SelfCollide), run on the CPU engine (hair_grid.h onEngine; one source with the GPU
// preview's).

#include <memory>
#include <string>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "hair_grid.h"
#include "internal.h"

namespace groomlab {

namespace {

using namespace hair_grid;

class SelfCollideOp : public Operator {
    mutable EngineWalls walls_;

public:
    std::string typeName() const override { return op::kSelfCollide; }
    std::string category() const override { return "shape"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0,
                      "How far overpacked hair is pushed apart, 0-1 (mappable): 1 spreads it to the packing, 0 leaves "
                      "the strands as they came", "fraction"}
                .range(0, 1),
            ParamSpec{"packing", 0.3,
                      "The share of space hair may fill, 0-1: where it is packed tighter it is pushed apart. Lower "
                      "gives more volume (real hair packs to about 0.2 to 0.5)", "fraction"}
                .range(0.01, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {0.15f, 1.0f}, {1.0f, 1.0f}}, "Push along the length, root to tip"},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kSelfCollide);
        const MeshPtr scalp = valueAs<MeshPtr>(ctx.external("scalp"));
        const ScalarField strength(params(), "strength", scalp.get());
        strength.validate(*in);
        if (in->empty() || (strength.isConstant() && !(strength.maximum() > 0.0f))) return in;
        // look.slang's hair grid, the GPU preview's own (hair_grid.h onEngine).
        return onEngine(gpu::StageKind::SelfCollide, op::kSelfCollide, params(), ctx, in, scalp, walls_);
    }
};

}  // namespace

void registerSelfCollideOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSelfCollide,
                          {"collide",
                           "Hair no longer passes through itself: where it is packed tighter than it can be, it is pushed apart, so layers rest on one another.",
                           "self collision layers packed intersect"},
                          [] { return std::make_unique<SelfCollideOp>(); });
}

}  // namespace groomlab
