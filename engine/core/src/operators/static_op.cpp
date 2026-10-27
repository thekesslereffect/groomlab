// SPDX-License-Identifier: Apache-2.0
// Static: hair charged with static electricity repels itself, like dry winter hair. Its charge, field
// and push are look.slang's hair grid (Static), run on the CPU engine (hair_grid.h onEngine; one
// source with the GPU preview's).

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

class StaticOp : public Operator {
    mutable EngineWalls walls_;  // (the CPU engine's: hair_grid::onEngine)

public:
    std::string typeName() const override { return op::kStatic; }
    std::string category() const override { return "look"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 0.5,
                      "How charged the hair is (mappable): strands spread apart, the outer layer puffs out and the "
                      "tips lift away; 0 leaves the strands as they came", "fraction"}
                .atLeast(0)
                .soft(0, 1),
            ParamSpec{"randomness", 0.5, "How much the charge varies between strands, 0-1: some strands fly farther",
                      "fraction"}
                .range(0, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {1.0f, 1.0f}}, "Charge along the length, root to tip (dry ends carry most)"},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kStatic);
        const MeshPtr scalp = valueAs<MeshPtr>(ctx.external("scalp"));
        const ScalarField strength(params(), "strength", scalp.get());
        strength.validate(*in);
        if (in->empty() || (strength.isConstant() && !(strength.maximum() > 0.0f))) return in;
        // look.slang's hair grid, the GPU preview's own (hair_grid.h onEngine).
        return onEngine(gpu::StageKind::Static, op::kStatic, params(), ctx, in, scalp, walls_);
    }
};

}  // namespace

void registerStaticOperator(OperatorRegistry& registry) {
    registry.registerType(op::kStatic,
                          {"look",
                           "Hair charged with static repels itself, like dry winter hair: strands spread, the outer layer puffs out, tips lift and locks fray.",
                           "charged puff repel"},
                          [] { return std::make_unique<StaticOp>(); });
}

}  // namespace groomlab
