// SPDX-License-Identifier: Apache-2.0
// SelfCollide: hair that no longer passes through itself: where hair is packed tighter than it can
// be it is pushed apart, so layers rest on one another. Its grid, solve and push are look.slang's
// hair grid (SelfCollide), run on the CPU engine (hair_grid.h onEngine; one source with the GPU
// preview's). Mode locks presses whole locks against each other instead (tube_press.h pressLocks):
// hair grouped into locks by a strand channel (lockChannel: a Tubes fill's tubeId, a Clump's clumpId, a
// Braid's braidSection...), each lock read as a tube and squashed flat where it meets another, its
// hair moved in toward its center with it (the CPU's alone).

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/tube_press.h"
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
            ParamSpec{"mode", std::string("hair"),
                      "hair: overpacked hair pushed apart strand by strand; locks: whole locks (hair grouped by "
                      "lockChannel) pressed flat against each other where they meet, each keeping its hair", "",
                      {"hair", "locks"}},
            ParamSpec{"strength", 1.0,
                      "How far overpacked hair is pushed apart, 0-1 (mappable): 1 spreads it to the packing, 0 leaves "
                      "the strands as they came", "fraction"}
                .range(0, 1),
            ParamSpec{"packing", 0.3,
                      "The share of space hair may fill, 0-1: where it is packed tighter it is pushed apart. Lower "
                      "gives more volume (real hair packs to about 0.2 to 0.5)", "fraction"}
                .range(0.01, 1)
                .when("mode", {"hair"}),
            ParamSpec{"lockChannel", std::string("tubeId"),
                      "The strand channel that groups hair into locks (a Tubes fill's tubeId, a Clump's clumpId, a "
                      "Braid's braidSection, any other): strands of one value are one lock; below 0, in none"}
                .when("mode", {"locks"}),
            ParamSpec{"distance", 1.0,
                      "How far along each lock its swell round a contact eases in and out (scene units)", "length"}
                .atLeast(0)
                .soft(0, 5)
                .when("mode", {"locks"}),
            ParamSpec{"smoothness", 0.5,
                      "How round each pressed lock's outline is, 0-1: its contacts' edges rounded (0: sharp)", "fraction"}
                .range(0, 1)
                .when("mode", {"locks"}),
            ParamSpec{"volume", 1.0,
                      "How much of the hair a contact presses out of a lock goes to its free sides, 0-1 (1: each "
                      "lock keeps its section's area, as a braid's sections do; it grows at most a quarter)", "fraction"}
                .range(0, 1)
                .when("mode", {"locks"}),
            {"profile", Ramp{{0.0f, 0.0f}, {0.15f, 1.0f}, {1.0f, 1.0f}}, "Push along the length, root to tip"},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kSelfCollide);
        const MeshPtr scalp = valueAs<MeshPtr>(ctx.external("scalp"));
        const ScalarField strength(params(), "strength", scalp.get());
        strength.validate(*in);
        if (in->empty() || (strength.isConstant() && !(strength.maximum() > 0.0f))) return in;
        if (params().getString("mode") == "locks") {
            // Locks pressed against each other (tube_press.h), the hair moved by its strand's
            // strength times the profile at its place along it.
            PressSettings settings;
            settings.distance = static_cast<float>(params().getDouble("distance"));
            settings.smoothness = static_cast<float>(params().getDouble("smoothness"));
            settings.volume = static_cast<float>(params().getDouble("volume"));
            const Ramp profile = params().getRamp("profile");
            const std::vector<float>* lockOf = in->strandChannel(params().getString("lockChannel"));
            if (!lockOf || lockOf->size() != in->strandCount()) return in;
            std::vector<float> strandStrength(in->strandCount());
            for (std::size_t s = 0; s < in->strandCount(); ++s) strandStrength[s] = std::clamp(strength.at(*in, s), 0.0f, 1.0f);
            return pressLocks(in, *lockOf, settings, [&](std::size_t s, float t) { return strandStrength[s] * profile(t); });
        }
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
