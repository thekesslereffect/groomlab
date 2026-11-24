// SPDX-License-Identifier: Apache-2.0
// SculptStrands: brush strokes on the strands themselves, kept in the groom (sculpt_strands.h). Put
// it late in the chain, after the look nodes, to fix what shows. Like a Sculpt node it is pure: it
// reads its edits from the session (the external "sculpt:<node>", the groom's sculpts by node name,
// or a live stroke's) and lays each strand's relative form on the strand of the same ID it is fed;
// strands with none take their recorded neighbours' (`fill`: the strands a raised density adds, the
// render's past a thinned preview's). With no edits, or strength 0, the strands pass through.
//
// Each strand's edit and root frame are resolved once (resolveStrandSculpts: they depend on the
// edits, the strands' IDs and roots and the scalp, not on the shapes fed) and kept, so a drag above
// the node lays the same table on new shapes; the laying is look.slang's SculptStrandsCS (the GPU
// preview's stage, the same kernel on the CPU: look_cpu.h), which the preview reads the table from
// (operator_state.h sculptStrandsState).

#include <memory>
#include <mutex>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/preview_core.h"
#include "groomlab/sculpt.h"
#include "groomlab/sculpt_strands.h"
#include "internal.h"

namespace groomlab {

namespace {

class SculptStrandsOp : public Operator {
public:
    std::string typeName() const override { return op::kSculptStrands; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    PortSpec output() const override { return {"out", PortType::Strands}; }
    // The scalp (each strand's root frame, and the roots' places the fill reaches by) and its edits.
    std::vector<std::string> externalInputs() const override { return {"scalp", sculptExternal(nodePath())}; }

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How much of the sculpted edits applies: 0 the strands as fed, 1 as brushed, past 1 more "
                                       "(every bend and length change that share of it)"}
                .atLeast(0)
                .soft(0, 1.5),
            {"fill", true, "Strands with no edit of their own take their sculpted neighbours' (a raised density's new strands, the "
                           "render's strands past a thinned preview's), fading out within a strand spacing"},
        };
    }

    // operator_state.h sculptStrandsState.
    bool keptState(const StrandsPtr& output, std::shared_ptr<const ResolvedStrandSculpts>* state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (!output || !kept_.state || !sameOrShown(kept_.output.lock(), output)) return false;
        *state = kept_.state;
        return true;
    }
    // Masked: its state answers for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (made && kept_.output.lock() == made) kept_.output = mixed;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kSculptStrands);
        const SculptPtr edits = valueAs<SculptPtr>(ctx.external(sculptExternal(nodePath())));
        const bool fill = params().getBool("fill");
        const double strength = params().getDouble("strength");  // (the stage reads it too)
        const ScalpMesh* scalp = scalpOf(ctx);
        remember(nullptr, nullptr);
        const std::shared_ptr<const ResolvedStrandSculpts> table = resolved(*in, edits, scalp, fill);
        if (!table->any || strength == 0.0) {  // nothing to lay: the strands as fed (the preview's stage copies them)
            remember(in, table);
            return in;
        }
        gpu::StageInputs stage;
        stage.sculpts = table;
        const StrandsPtr made = gpu::runStage(gpu::StageKind::SculptStrands, params(), in, scalp, std::move(stage));
        remember(made, table);
        return made;
    }

private:
    // Each strand's edit and frame for `in`: the last table again while what it depends on stays
    // (the edits, the strands' IDs and roots, the scalp, the fill).
    std::shared_ptr<const ResolvedStrandSculpts> resolved(const StrandSet& in, const SculptPtr& edits, const ScalpMesh* scalp,
                                                          bool fill) const {
        const std::lock_guard<std::mutex> lock(tableMutex_);
        const void* ids = in.ids().data();
        const void* roots = in.roots().data();
        if (table_.made && table_.edits.lock() == edits && table_.ids == ids && table_.roots == roots &&
            table_.count == in.strandCount() && table_.scalp == scalp && table_.fill == fill) {
            return table_.made;
        }
        static const SculptEdits kNone;
        auto made = std::make_shared<ResolvedStrandSculpts>(resolveStrandSculpts(in, edits ? *edits : kNone, scalp, fill));
        table_ = {made, edits, ids, roots, in.strandCount(), scalp, fill};
        return made;
    }

    struct Table {
        std::shared_ptr<const ResolvedStrandSculpts> made;
        std::weak_ptr<const SculptEdits> edits;
        const void* ids = nullptr;
        const void* roots = nullptr;
        std::size_t count = 0;
        const ScalpMesh* scalp = nullptr;
        bool fill = true;
    };
    mutable std::mutex tableMutex_;
    mutable Table table_;

    // The last result (by identity), and the table it was laid with.
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        std::shared_ptr<const ResolvedStrandSculpts> state;
    };
    void remember(const StrandsPtr& output, std::shared_ptr<const ResolvedStrandSculpts> state) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        kept_.output = output;
        kept_.state = std::move(state);
    }
    mutable std::mutex keptMutex_;
    mutable Kept kept_;
};

}  // namespace

bool sculptStrandsState(const Operator& op, const StrandsPtr& output, std::shared_ptr<const ResolvedStrandSculpts>* state) {
    const auto* sculpt = dynamic_cast<const SculptStrandsOp*>(&op);
    return sculpt && sculpt->keptState(output, state);
}

void registerSculptStrandsOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSculptStrands,
                          {"shape",
                           "Brush the strands themselves, late in the chain: the strokes are kept in the node, follow the strands "
                           "when what feeds it changes, and fill in strands a raised density adds.",
                           "brush sculpt strands interactive groom xgen correctives strokes edit hair"},
                          [] { return std::make_unique<SculptStrandsOp>(); });
}

}  // namespace groomlab
