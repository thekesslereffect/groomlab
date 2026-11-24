// SPDX-License-Identifier: Apache-2.0
// Sculpt: brush strokes on guides the graph made, kept in the groom (sculpt.h). The node is pure:
// it reads its edits from the session (the external "sculpt:<node>", made from the groom's sculpts
// by node name, or a live stroke's) and lays each edited guide's relative form on the guide of the
// same ID it is fed. With no edits, or strength 0, the guides pass through to the bit. An optional
// scalp gives each guide's root frame (its surface frame); without one, world axes do.
//
// CPU only: it feeds guides, which the GPU preview reads from the CPU's cache. A stroke's dabs and a
// strength drag evaluate it alone on the CPU (guides are few) and run the strands after it on the
// GPU (look_chain.h: sculptedGuides; look_preview.h: strokeShapesGuides).

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/sculpt.h"
#include "internal.h"

namespace groomlab {

namespace {

class SculptOp : public Operator {
public:
    std::string typeName() const override { return op::kSculptGuides; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override {
        return {{"guides", PortType::Guides}, {"scalp", PortType::Mesh, true}};
    }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How much of the sculpted edits applies: 0 the guides as fed, 1 as brushed, past 1 more "
                                       "(every bend and length change that share of it)"}
                .atLeast(0)
                .soft(0, 1.5),
        };
    }
    std::vector<std::string> externalInputs() const override { return {sculptExternal(nodePath())}; }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const GuidesPtr in = inputs.empty() ? nullptr : valueAs<GuidesPtr>(inputs[0]);
        if (!in) return GuidesPtr{};  // nothing fed: no guides, as an empty Guides node gives
        const SculptPtr edits = valueAs<SculptPtr>(ctx.external(sculptExternal(nodePath())));
        const float strength = static_cast<float>(params().getDouble("strength"));
        if (!edits || edits->empty() || strength == 0.0f) return in;
        const MeshPtr scalp = inputs.size() > 1 ? valueAs<MeshPtr>(inputs[1]) : nullptr;
        return applySculpts(in, *edits, strength, scalp.get());
    }
};

}  // namespace

void registerSculptGuidesOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSculptGuides,
                          {"shape",
                           "Brush the guides the graph made (ToGuides): the strokes are kept in the node and follow the guides when what feeds it changes.",
                           "brush correctives strokes edit guides"},
                          [] { return std::make_unique<SculptOp>(); });
}

}  // namespace groomlab
