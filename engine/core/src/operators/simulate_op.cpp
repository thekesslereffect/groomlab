// SPDX-License-Identifier: Apache-2.0
// Simulate: the guides as the core's hair solver moved them (simulation.h). The node is pure: it
// reads its frame from the session (the external "sim:<node>", made by GroomSession, which steps
// the solver frame by frame and keeps every frame) and lays each simulated guide's points on the
// guide of the same ID it is fed, so painted maps, roots and layers come through as they are.
// Without a frame (before the start frame, a frame not simulated in cacheMode "cached", a host
// that does not step simulations, inside a compound) the guides pass through, to the bit. With
// water or rain the frame's wetness comes along as each guide's "wetness" point layer (0 without
// a frame: the look's maps reading it always find it).
//
// Its stiffnesses, damping, drag, rootStiffness, mass and strength are mappable: per guide and
// per point (a painted guide layer, a texture, an expression, a Field node plugged in), read by the
// session on the guides it is fed each frame (simulationMaps, simulation.h).

#include <unordered_map>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/simulation.h"
#include "internal.h"

namespace groomlab {

namespace {

class SimulateOp : public Operator {
public:
    std::string typeName() const override { return op::kSimulate; }
    std::string category() const override { return "motion"; }
    std::vector<PortSpec> inputs() const override {
        return {{"guides", PortType::Guides}, {"collider", PortType::Mesh, true}, {"water", PortType::Mesh, true}};
    }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    // Every setting changes every frame after the start: none may be keyed.
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"stretchStiffness", 1.0, "How strongly segments keep their length, 0-1 (1: hair does not stretch) "
                                                    "(mappable, per point)"})
                .range(0, 1),
            notAnimatable({"bend", 0.5, "How strongly guides keep their groomed curves, 0-1 (0: a rope, 1: stiff) (mappable, "
                                        "per point)"})
                .range(0, 1),
            notAnimatable({"twist", 0.5, "How strongly guides keep their groomed twist, 0-1 (mappable, per point)"}).range(0, 1),
            notAnimatable({"damping", 0.1, "How quickly swinging dies out, 0-1 (mappable, per point)"}).range(0, 1),
            notAnimatable({"drag", 0.05, "Air drag, 0-1: slows the hair against the air (and carries it with the wind) "
                                         "(mappable, per point)"})
                .range(0, 1),
            notAnimatable({"mass", 1.0, "How heavy the hair is (mappable, per point): the air (drag, wind) and other hair "
                                        "move heavier hair less, and along a guide its heavier points move less; gravity "
                                        "pulls all hair alike"})
                .atLeast(0.001)
                .soft(0.1, 4),
            notAnimatable({"strength", 1.0, "How much the hair is simulated, 0-1 (mappable, per point: paint which guides "
                                            "move): 0 holds it to its input shape, moving with its root; in between, that "
                                            "share of its simulated motion", "fraction"})
                .range(0, 1),
            notAnimatable({"gravity", 980.0, "Gravity's pull, scene units per second squared (980 in centimeters)", "length"})
                .soft(0, 2000),
            notAnimatable({"gravityDirection", Vec3f{0.0f, -1.0f, 0.0f}, "Gravity's direction (world space)"}).world(),
            notAnimatable({"groomedUnderGravity", true,
                           "The groomed shape is how the hair rests under gravity at the start frame: it keeps that shape "
                           "while still, and only the head's motion (and gravity's change as the head tilts or turns over) "
                           "moves it. Off: gravity pulls on the groomed shape"}),
            notAnimatable({"windSpeed", 0.0, "The wind's speed, scene units per second (0: still air): the hair drags toward it",
                           "length"})
                .soft(0, 1000)
                .in("Wind"),
            notAnimatable({"windDirection", Vec3f{1.0f, 0.0f, 0.0f}, "Where the wind blows to (world space)"}).world().in("Wind"),
            notAnimatable({"windGust", 0.5, "How much the wind varies over space and time, 0-1 (as Wind's gust)"})
                .range(0, 1)
                .in("Wind"),
            notAnimatable({"windFrequency", 0.1, "Gust size: noise features per scene unit (smaller: broader gusts)", "perLength"})
                .atLeast(0)
                .soft(0, 1)
                .in("Wind"),
            notAnimatable({"rootStiffness", 1.0, "How firmly the hair is held to its groomed shape (moving with its root) each "
                                                 "frame, times rootStiffnessProfile (mappable, per point): 0 frees it, 2 "
                                                 "holds it twice as firmly"})
                .atLeast(0)
                .soft(0, 2),
            notAnimatable({"rootStiffnessProfile", Ramp{{0.0f, 1.0f}, {0.2f, 0.0f}},
                           "rootStiffness along the length, root to tip: 1 attached, 0 free"}),
            notAnimatable({"inertia", 1.0, "How much of the head's motion the hair feels, 0-1 (0: carried rigidly by its "
                                           "root; lower: less floppy)", "fraction"})
                .range(0, 1),
            notAnimatable({"collideScalp", true, "Keep the guides out of the scalp"}).in("Collisions"),
            notAnimatable({"collisionOffset", 0.05, "Distance kept from the scalp and the collider", "length"})
                .atLeast(0)
                .soft(0, 0.5)
                .in("Collisions"),
            notAnimatable({"friction", 0.3, "How much hair resting on a surface (the scalp, a collider) is kept from sliding, "
                                            "0-1 (a ColliderMesh node's own friction overrides it for its mesh)"})
                .range(0, 1)
                .in("Collisions"),
            notAnimatable({"repulsion", 0.0, "Guides pushed apart where closer than repulsionRadius, 0-1 (0: off)"})
                .range(0, 1)
                .in("Collisions"),
            notAnimatable({"repulsionRadius", 0.2, "Distance guides keep from each other while repulsion is on", "length"})
                .atLeast(0)
                .soft(0, 1)
                .in("Collisions")
                .adv(),
            notAnimatable({"volume", 0.0, "Hair volume, 0-1 (0: off): hair packed tighter than volumeCellSize is pushed "
                                          "apart, down the density of the hair around it, so long hair keeps its body"})
                .range(0, 1)
                .in("Volume"),
            notAnimatable({"volumeCellSize", 1.0, "The size of the cells hair volume counts the hair in: hair is kept about "
                                                  "this far apart (larger: fuller, smoother)", "length"})
                .atLeast(0.001)
                .soft(0.1, 5)
                .in("Volume")
                .adv(),
            notAnimatable({"substeps", std::int64_t{4}, "Solver steps per frame (more: steadier fast motion)"})
                .range(1, 64)
                .soft(1, 16)
                .in("Solver")
                .adv(),
            notAnimatable({"iterations", std::int64_t{6}, "Constraint passes per step (more: stiffer, closer to the shapes)"})
                .range(1, 64)
                .soft(1, 32)
                .in("Solver")
                .adv(),
            notAnimatable({"startFrame", std::int64_t{1}, "The frame the simulation starts at, at rest in the groomed shape"})
                .range(-1000000, 1000000)
                .soft(-100, 500),
            notAnimatable({"seed", std::int64_t{0}, "Another wind with the same settings"}).in("Wind").adv(),
            notAnimatable({"cacheMode", std::string("live"),
                           "live: playing or scrubbing past the cached frames simulates them; cached: only Simulate Range "
                           "(or a bake) fills the cache, other frames pass the guides through",
                           "", {"live", "cached"}}),
            notAnimatable({"water", false, "Water: hair below the surface (waterLevel, or the mesh on the water input) "
                                           "floats, drags, drifts with the current and gets wet"})
                .in("Water"),
            notAnimatable({"waterLevel", 0.0, "The water's surface height (world Y), while nothing is connected to the water "
                                              "input (a mesh there is the surface)", "length"})
                .soft(-100, 200)
                .in("Water")
                .when("water", {"true"}),
            notAnimatable({"buoyancy", 0.9, "Upward pull under water, a share of gravity (1: weightless, more: hair floats "
                                            "up and fans out)"})
                .atLeast(0)
                .soft(0, 2)
                .when("water", {"true"})
                .in("Water"),
            notAnimatable({"waterDrag", 0.5, "Drag in the water, 0-1: far stronger than the air's, motion slows and "
                                             "follows the current"})
                .range(0, 1)
                .in("Water")
                .when("water", {"true"}),
            notAnimatable({"currentSpeed", 0.0, "The water's flow, scene units per second (0: still water), with gentle "
                                                "variation (gust size: windFrequency)", "length"})
                .soft(0, 200)
                .in("Water")
                .when("water", {"true"}),
            notAnimatable({"currentDirection", Vec3f{1.0f, 0.0f, 0.0f}, "Where the current flows to (world space)"}).world()
                .in("Water")
                .when("water", {"true"}),
            notAnimatable({"dryTime", 10.0, "Seconds wet hair takes to dry most of the way out of the water", "seconds"})
                .atLeast(0.01)
                .soft(0.1, 60)
                .in("Water"),
            notAnimatable({"wetWeight", 0.5, "How much heavier and limper wet hair is above the water, 0-1"})
                .range(0, 1)
                .in("Water"),
            notAnimatable({"rain", 0.0, "Rain wetting the hair, 0-1 (1: soaked in about a second); no forces"})
                .range(0, 1)
                .in("Water"),
        };
    }
    std::vector<std::string> externalInputs() const override { return {simulationExternal(nodePath())}; }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const GuidesPtr in = inputs.empty() ? nullptr : valueAs<GuidesPtr>(inputs[0]);
        if (!in) return GuidesPtr{};  // nothing fed: no guides, as an empty Guides node gives
        const GuidesPtr simulated = valueAs<GuidesPtr>(ctx.external(simulationExternal(nodePath())));
        if (!simulated || simulated->empty()) {
            // Wet settings: the guides dry (a wetness of 0), so maps reading the wetness always find it.
            if (!(params().getBool("water") || params().getDouble("rain") > 0.0)) return in;
            auto dry = std::make_shared<GuideSet>(*in);
            for (Guide& guide : dry->guides()) guide.pointAttributes[kWetnessChannel].assign(guide.points.size(), 0.0f);
            return GuidesPtr(dry);
        }
        std::unordered_map<std::uint64_t, std::size_t> byId;
        byId.reserve(simulated->size());
        for (std::size_t g = 0; g < simulated->size(); ++g) byId.emplace(simulated->guides()[g].id, g);
        auto out = std::make_shared<GuideSet>(*in);
        for (Guide& guide : out->guides()) {
            const auto it = byId.find(guide.id);
            if (it == byId.end()) continue;
            const Guide& from = simulated->guides()[it->second];
            if (from.points.size() != guide.points.size()) continue;
            guide.points = from.points;
            const auto wet = from.pointAttributes.find(kWetnessChannel);
            if (wet != from.pointAttributes.end()) guide.pointAttributes[kWetnessChannel] = wet->second;
        }
        return GuidesPtr(out);
    }
};

}  // namespace

void registerSimulateOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSimulate,
                          {"motion",
                           "Hair dynamics on the guides: gravity, wind, the head's motion, collisions. Play, or Simulate Range; bake for renders.",
                           "simulation dynamics physics"},
                          [] { return std::make_unique<SimulateOp>(); });
}

}  // namespace groomlab
