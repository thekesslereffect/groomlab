// SPDX-License-Identifier: Apache-2.0
// SelfCollide: hair that no longer passes through itself. Static, not a simulation: the strands are
// taken as an incompressible material, as a fluid's pressure projection takes water, and where hair
// is packed tighter than it can be it is pushed apart, so layers rest on one another and hair piles
// up (behind the ears, over a collar, at the crown) instead of overlapping. Hair that is not
// overpacked does not move.
//
// Each of kPasses passes:
//  1. Splat. The strands' segments, sampled about half a cell apart on the Catmull-Rom curve through
//     their points, are added into a sparse grid of cubes `h` wide (blocks of 4 x 4 x 4 nodes where
//     hair is), each sample its strand's cross-section times its share of the segment, spread
//     trilinearly over its cell's 8 nodes: the share of space hair fills there, rho. The
//     cross-section is pi w^2 / 4 for strands with widths (a Width before it), else a nominal one,
//     kNominalCover of the scalp's area per strand (a strand stands for the hair around it). Each
//     cell also sums where its samples are in it (their mass, offsets and products of offsets) and
//     how wide their strands are. Only the core strands are splatted (preview_core.h: those every
//     preview holds, each standing for the strands of a render it is a share of), at most about
//     kMostSplatted of them (a share of those by ID): the field is the same at every preview
//     density, and costs no more for a million strands than for a few thousand. The sums are fixed
//     point, so the same on any number of threads.
//  2. The scalp near each node (its closest point within kWallReach cells): the node's height over
//     it and its normal. Nodes under it are solid.
//  3. Solve, at the grid's scale. Where rho is above `packing`, the hair must spread by its excess:
//     a displacement d = -grad p with div d = (rho - packing) / rho there, p = 0 everywhere else
//     (hair that is not overpacked pushes back nothing: it moves aside) and no flow into a solid
//     node. kSweeps sweeps of red-black SOR over the overpacked nodes alone.
//  4. Below the grid's scale. A layer much thinner than a cell (hair combed flat, two sheets
//     passing through each other) fills its cell by less than it fills itself, and a field smooth
//     over cells moves its strands alike: it cannot part them. So where a node's samples (those of
//     the 8 cells around it) lie in a slab (their covariance's least axis n under kSlab of the
//     next), the slab's thickness is taken from them: sqrt(12 var_n) between the strands' middles,
//     plus their width. Packed to `packing`, its hair would take its mass per area over `packing`
//     (rho h, where the slab sits off the node: over the node's share of it); thinner, it spreads
//     along n about its middle, each strand's offset along n scaled by k (spaced as they were, now
//     as thick as the hair needs, at most a cell thicker a pass: the strands' order kept, so sheets
//     that crossed come apart in the order they lay). A strand's own offset across the slab (by its
//     ID, up to kJitter / 2 of a cell either way) tells apart strands lying exactly together, so
//     they stack, in the same order wherever they meet. A slab within kWallReach cells of the scalp
//     spreads away from it, from its bottom up.
//  5. Push. Every point moves by the two (the nodes' blended trilinearly) times `strength` and
//     `profile` along the strand, at most a cell a pass; the root stays and each strand is walked
//     from it keeping its segments' lengths (follow the leader), each point kept out of the scalp
//     (its field, as Interpolate's keepClearance asks it: pushed above a floor of a hair's width,
//     asked where the nodes' heights say the point may be that near).
// The grid's cell is about kCellSpacings root spacings of the strands splatted (their share of the
// scalp's area), between 1/128 and 1/16 of the scalp's size, rounded on a decimal lattice: the
// same strands at any scale. Strands no push reaches come out bit for bit as they came. The grid,
// the splat, the scalp's walls, the solve and the walk are look.slang's hair grid, shared with
// Static, run on the CPU engine (hair_grid::onEngine; one source with the GPU preview's).

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

constexpr int kPasses = 3;                 // splat, solve and push, this many times
constexpr int kSweeps = 48;                // SOR sweeps of a solve
constexpr double kOmega = 1.7;             // over-relaxation
constexpr std::size_t kMostSplatted = kSelfCollideGrid.most;  // core strands splatted at most (a power-of-two share by ID beyond)
          // the farthest a point moves in a pass, in cells
constexpr double kJitter = 0.15;           // a strand's own offset (by its ID) for telling it apart, in cells
constexpr double kSlab = 0.6;              // a slab: its least variance under this share of the next
constexpr double kWallReach = kSelfCollideGrid.wallReach;  // a slab within this many cells of the scalp spreads away from it
constexpr std::uint64_t kThinSalt = kSelfCollideGrid.thinSalt;
constexpr std::uint64_t kJitterSalt = 0x51AB0FF5E7ull;

// A strand's own small offset across a slab, by its ID (in cells, -kJitter/2 to kJitter/2): telling
// apart strands lying exactly together, in the same order wherever they meet.
float rankOf(StrandId id) { return static_cast<float>(kJitter) * (unitFloat(hash64(static_cast<std::uint64_t>(id) ^ kJitterSalt)) - 0.5f); }

// A node's spread of a slab: offsets along `axis` from `from` scaled by 1 + `spread`.
struct Spread {
    Vec3f axis, from;
    float spread = 0.0f;
};

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
