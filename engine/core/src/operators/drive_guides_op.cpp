// SPDX-License-Identifier: Apache-2.0
// DriveGuides: guides (or strands) posed by driver curves an animator rigs in the host (drive_guides.h).
// Each guide attached to drivers by hand (the bind's attachments; a child follows its parent's) binds
// to them as they were bound (the groom's driver bind, made at the bind frame) and is carried along
// them as they are now: every point but the root keeps its offset from its closest place on each
// driver, in the driver's rotation-minimizing frame there. Guides attached to nothing stay. A
// Simulate after it pulls toward the driven guides, so the simulation plays on top of the rig.
//
// The drivers come in as the externals "drivers" (now) and "driverBind" (as bound), both in the
// space the graph evaluates in, and "driverAttach" (GroomSession). No bind: the curves pass through.
// The binding is kept between evaluations while the curves and the bound drivers stay the same (on
// a following scalp, every frame), so a frame costs the carry alone.
//
// CPU only: it feeds guides, which the GPU preview reads from the CPU's cache.

#include <memory>
#include <mutex>
#include <vector>

#include "groomlab/drive_guides.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "internal.h"

namespace groomlab {

namespace {

class DriveGuidesOp : public Operator {
public:
    std::string typeName() const override { return op::kDriveGuides; }
    std::string category() const override { return "motion"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // guides or strands, as fed
    std::vector<std::string> externalInputs() const override {
        return {"scalp", kDriversExternal, kDriverBindExternal, kDriverAttachExternal};
    }

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How much the attached guides follow their drivers (mappable); 0: as groomed"}
                .atLeast(0)
                .soft(0, 1),
            {"profile", Ramp::constant(1.0f), "How much each part follows, root to tip (the root always stays on the scalp)"},
        };
    }

    std::string note(const EvalContext& ctx) const override {
        const GuidesPtr bound = valueAs<GuidesPtr>(ctx.external(kDriverBindExternal));
        if (!bound || bound->empty()) {
            return "no driver bind: attach guides to driver curves (Simulate > Drivers); the guides pass through";
        }
        const DriverAttachPtr attached = valueAs<DriverAttachPtr>(ctx.external(kDriverAttachExternal));
        if (!attached || attached->byGuide.empty()) return "no guides attached to the drivers: the guides pass through";
        return {};
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kDriveGuides);
        const GuidesPtr bound = valueAs<GuidesPtr>(ctx.external(kDriverBindExternal));
        const GuidesPtr now = valueAs<GuidesPtr>(ctx.external(kDriversExternal));
        if (!bound || bound->empty() || in->strandCount() == 0) return in;
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        if (strength.isConstant() && strength.maximum() <= 0.0f) return in;

        const Ramp profile = params().getRamp("profile");
        const DriverAttachPtr attachedPtr = valueAs<DriverAttachPtr>(ctx.external(kDriverAttachExternal));
        const DriverAttachments none;
        const DriverAttachments& attached = attachedPtr ? *attachedPtr : none;
        if (attached.byGuide.empty()) return in;
        std::shared_ptr<const DriveBinding> binding;
        {
            const std::uint64_t key = driveBindingKey(*in, *bound, attached);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!binding_ || binding_->key != key) binding_ = std::make_shared<const DriveBinding>(bindToDrivers(*in, *bound, attached));
            binding = binding_;
        }
        if (binding->perPoint == 0) return in;
        std::vector<float> amount(in->strandCount());
        for (std::size_t s = 0; s < amount.size(); ++s) amount[s] = strength.at(*in, s);
        auto out = std::make_shared<StrandSet>(*in);
        driveCurves(*out, *binding, *bound, now ? *now : *bound, amount, profile);
        return StrandsPtr(out);
    }

private:
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const DriveBinding> binding_;  // the last binding made (its key says for what)
};

}  // namespace

void registerDriveGuidesOperator(OperatorRegistry& registry) {
    registry.registerType(op::kDriveGuides,
                          {"motion",
                           "Guides attached to driver curves an animator rigs follow them (bound at the bind frame); a "
                           "Simulate after it plays on top of them.",
                           "rig animate pose controls drivers bind hair rig"},
                          [] { return std::make_unique<DriveGuidesOp>(); });
}

}  // namespace groomlab
