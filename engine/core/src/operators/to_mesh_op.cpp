// SPDX-License-Identifier: Apache-2.0
// ToMesh: what it is handed as a polygon mesh (strand_geometry.h buildStrandMesh): the strands as
// tubes or flat ribbons, the scales as their plates bent onto the skin, feathers as their barbs and
// rachis tubes, a Cards node's cards, an Instance node's meshes; `make` auto picks by what the
// strands carry. The mesh follows the groom's animation
// (the scalp, Simulate, keyed params). The node hands its strands on as they are, so it sits
// anywhere in the chain, like Cards: where it sits decides what the mesh shows (after Simulate,
// before Noise...). Hosts make the mesh, only while something takes it
// (GroomSession::evaluateStrandMeshes; Maya's groom outMesh), from an evaluation at the node's
// density; a frame where only the strands' points moved keeps a tube mesh's topology and UVs.

#include <memory>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "internal.h"

namespace groomlab {

namespace {

class ToMeshOp : public Operator {
public:
    std::string typeName() const override { return op::kToMesh; }
    std::string category() const override { return "convert"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"make", std::string("auto"), "What the mesh is made of. auto: the scales when there are some, "
                                                        "else other instances, else cards (a Cards node before it), else "
                                                        "feathers (barbs and rachis), else the strands; strands: tubes or "
                                                        "ribbons round every strand; scales: the scales' plates bent onto "
                                                        "the skin; feathers: the barbs as tubes or ribbons and the rachis "
                                                        "tubes; cards: the Cards nodes' cards, in one atlas; instances: "
                                                        "every instance's mesh where it sits",
                           "", {"auto", "strands", "scales", "feathers", "cards", "instances"}}),
            notAnimatable({"style", std::string("tube"), "tube: a closed tube round each strand (sides around, a fan at the "
                                                         "tip); ribbon: a flat strip lying along the scalp",
                           "", {"tube", "ribbon"}})
                .when("make", {"auto", "strands", "feathers"}),
            notAnimatable({"sides", std::int64_t{4}, "A tube's sides"}).range(3, 64).soft(3, 12).when("style", {"tube"}),
            ParamSpec{"widthScale", 1.0, "Times each point's width (the strand's diameter)"}
                .range(0.0001, 1000)
                .soft(0.1, 10)
                .when("make", {"auto", "strands", "feathers"}),
            notAnimatable({"density", 1.0, "The share of the strands evaluated for the mesh (as the preview density thins "
                                           "them: a subset of a higher density's)",
                           "fraction"})
                .range(0.0001, 1),
            notAnimatable({"maxStrands", std::int64_t{20000}, "At most this many strands make the mesh (by ID, as density "
                                                              "picks them; 0: all). Tube meshes are heavy: 4 sides make "
                                                              "about 4 vertices per strand point"})
                .range(0, 100000000)
                .soft(0, 100000),
            notAnimatable({"strandUVs", false, "A second UV set (strandUVs) laying every strand out in its own cell of a "
                                               "grid, for baking per strand"})
                .when("make", {"auto", "strands", "feathers"}),
        };
    }

    // Its strands as they came: the mesh is the host's, made from them (GroomSession::evaluateStrandMeshes).
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        return requireStrands(inputs, op::kToMesh);
    }
};

}  // namespace

void registerToMeshOperator(OperatorRegistry& registry) {
    registry.registerType(op::kToMesh,
                          {"convert",
                           "What it is handed as a polygon mesh that follows the animation: the strands as tubes or "
                           "ribbons, the scales as their plates, feathers as barbs and rachis, cards, instances. Show it "
                           "in the viewport, "
                           "put it in the scene (Maya's groom outMesh) or export it (OBJ, FBX, Alembic): for ZBrush, "
                           "baking, printing or a renderer without curves.",
                           "polygon mesh tubes ribbons scales feathers cards instances geometry convert export obj fbx"},
                          [] { return std::make_unique<ToMeshOp>(); });
}

}  // namespace groomlab
