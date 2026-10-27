// SPDX-License-Identifier: Apache-2.0
// Static: hair charged with static electricity repels itself, like dry winter hair. Strands spread
// apart, the outer layer puffs out, charged tips lift away and a clumped lock frays into its
// strands. A closed form plus one solve, not a simulation: every strand carries charge (more
// toward its tips, where dry ends carry most: `profile`; some strands more than others:
// `randomness`), the charge's electric field is solved on a grid, and each strand bends from its
// root under the field's push on its own charge.
//
//  1. Charge. The core strands' segments (hair_grid.h: those every preview holds, at most about
//     kMostSplatted of them) are sampled half a cell apart and splatted into the hair grid, each
//     sample carrying its strand's charge cross-section (kNominalCover of the scalp's area per
//     strand of a render: charge rides on how much hair there is, not on how thick it is, so a
//     Width before it changes nothing) times its share of the segment times its charge there,
//     q = profile x the strand's own charge (1 + randomness (3 u^2 - 1), u by its ID: mean 1, a few
//     strands up to 1 + 2 randomness). rho at a node: the charge per unit volume. Each cell also
//     sums where its charge is in it.
//  2. Solve. The electric potential, -lap phi = rho, by red-black SOR over every node within
//     kReach nodes of charge (by Chebyshev distance: the boundary as far from hair everywhere).
//     Beyond them phi is the potential of all the charge seen from far away (a point charge at its
//     middle, softened over kSoften cells): the grid's padding is not far away, and phi = 0 there
//     would balance a hollow of hair (the hanging part under the head) with as much field inside it
//     as outside, pushing it nowhere. The scalp lets no field into it (a node under it takes its
//     neighbour's phi, as SelfCollide's no flow): a grounded conductor would draw the charged hair
//     onto it (its image charge cancelling the hair's own field outside), so hair is pushed off
//     the head instead, the outer layer most.
//  3. The field. E = -grad phi at every node (central differences).
//  4. Below the grid's scale. The grid's field moves strands a cell apart almost alike: it puffs a
//     lock out but cannot part its strands. So each node also takes its own charge as a uniformly
//     charged ball about the charge's middle there (the 8 cells around it), whose field is rho r / 3
//     (r from the middle), plus a strand's own offset (by its ID, kSplay of a cell in a direction
//     across the strand) for strands lying exactly together: each strand pushed from the local mean
//     of its neighbours, by the same charge. Only across the strand and across the grid's field
//     (which spreads the hair along itself already), so a lock fans out sideways into strands.
//  5. Push. Each point is pushed by kGain x strength x q x (E + kLocal x the ball's field) (the
//     nodes' blended trilinearly; within kWallReach cells of the scalp, without its part into the
//     scalp), eased to at most kMostShare of the strand's length (most tanh(push / most)). The
//     strand bends under those pushes as a cantilever from its root: each segment turns by the
//     moment of the pushes beyond it times kBend len / L^3, eased to at most kMostTurn len / L,
//     the turns above carried down the strand (as Gravity carries them). Curvature follows the
//     moment, which changes smoothly along the strand: no kinks, and the segments keep their
//     lengths. Points are kept out of the scalp (the walk's floor, as SelfCollide's).
// rho is a share of space, phi a length squared and E a length: the same strands at any scale (the
// cell and the charge are shares of the scalp). Strength 0 hands the strands on as they came.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/math.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "hair_grid.h"
#include "internal.h"

namespace groomlab {

namespace {

using namespace hair_grid;

constexpr std::size_t kMostSplatted = kStaticGrid.most;  // core strands splatted at most (a power-of-two share by ID beyond)
constexpr int kReach = 4;                   // the potential is solved within this many nodes of charge
constexpr int kSweeps = 64;                 // SOR sweeps of the solve
constexpr double kOmega = 1.8;              // over-relaxation
constexpr double kSoften = 2.0;             // the far charge softened over this many cells
constexpr double kWallReach = kStaticGrid.wallReach;  // within this many cells of the scalp, no push into it
  // without a scalp: this share of the strands' size squared for its area
static_assert(kStaticGrid.margin == 2 + kReach && kStaticGrid.dilate == (kReach + kB - 1) / kB, "hair_grid.h kStaticGrid");
constexpr double kGain = 48.0;              // a point's push: this times strength x q x E
constexpr double kLocal = 3.0;              // the ball's field below the grid's scale, against the grid's
constexpr double kSplay = 0.25;             // a strand's own offset across itself (by its ID), in cells
constexpr double kBend = 8.0;               // a segment's turn: this x the moment beyond it x len / L^3 (8: a uniform push's tip moves as far)
constexpr double kMostTurn = 3.14159265;    // a strand turns at most this far over its length (eased)
constexpr double kMostShare = 0.4;          // a point's push at most, a share of its strand's length (eased)
constexpr std::uint64_t kThinSalt = kStaticGrid.thinSalt;
constexpr std::uint64_t kChargeSalt = 0xC4A26E5A17ull;
constexpr std::uint64_t kSplaySalt = 0x5B1A7F0E5Dull;

// A strand's own charge, by its ID: 1 + randomness (3 u^2 - 1) (mean 1; a few strands up to
// 1 + 2 randomness, most a little under 1).
float chargeOf(StrandId id, float randomness) {
    const float u = unitFloat(hash64(static_cast<std::uint64_t>(id) ^ kChargeSalt));
    return std::max(0.0f, 1.0f + randomness * (3.0f * u * u - 1.0f));
}

// A strand's own direction for splaying, by its ID (a unit vector, uniform over the sphere).
Vec3f splayOf(StrandId id) {
    const std::uint64_t h = hash64(static_cast<std::uint64_t>(id) ^ kSplaySalt);
    const float z = 2.0f * unitFloat(h) - 1.0f;
    const float a = 6.28318531f * unitFloat(hash64(h));
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vec3f(r * pm::cos(a), r * pm::sin(a), z);
}

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
