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
constexpr double kAreaWithoutScalp = kStaticAreaWithoutScalp;  // without a scalp: this share of the strands' size squared for its area
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
        // On the CPU engine (look.slang's hair grid, its doubles the C++ below's to the bit).
        if (gpu::cpuEngineOn()) {
            if (StrandsPtr made = onEngine(gpu::StageKind::Static, op::kStatic, params(), ctx, in, scalp, walls_)) return made;
        }
        const auto randomness = static_cast<float>(std::clamp(params().getDouble("randomness"), 0.0, 1.0));
        const RampAlong profile(params().getRamp("profile"), *in);
        const std::size_t count = in->strandCount();

        Layout layout;
        if (!layoutOf(*in, scalp.get(), kMostSplatted, kThinSalt, &layout, ctx.scale)) return in;
        const std::vector<std::size_t>& splatted = layout.strands;
        // A strand's charge cross-section (kNominalCover of its share of the scalp's area).
        const double area = layout.area > 0.0 ? layout.area : kAreaWithoutScalp * layout.extent * layout.extent;
        const double section = kNominalCover * area / (static_cast<double>(splatted.size()) * layout.weight);
        if (!(section > 0.0) || !std::isfinite(section)) return in;
        const ScalpFloor floor = ScalpFloor::of(scalp, layout.size);

        // 1. Charge.
        const std::vector<Vec3f>& positions = in->positions();
        const std::vector<Sample> samples = sampleStrands(*in, positions, splatted, 0.5 * layout.h,
            [&](std::size_t k, std::size_t i, float t, float len, std::size_t m) {
                const std::size_t s = splatted[k], n = in->strandPointCount(s);
                const float q = chargeOf(in->ids()[s], randomness) *
                                (std::max(profile(i, n), 0.0f) * (1.0f - t) + std::max(profile(i + 1, n), 0.0f) * t);
                return std::pair<float, float>(static_cast<float>(section * layout.weight * len / static_cast<double>(m) * q), 0.0f);
            });
        Grid g;
        g.h = layout.h;
        if (!layGrid(samples, kStaticGrid.margin, kStaticGrid.dilate, &g)) return in;
        const std::size_t nodes = g.nodes();
        constexpr int kCellSums = kMx + 3;  // a cell's charge and where it is
        const Splat charge = splat(g, samples, kCellSums);
        const std::vector<Wall> walls = wallsOf(g, scalp.get(), layout.size, kWallReach);

        // Each node's distance to charge (Chebyshev, in nodes, up to kReach + 1): along x, then y,
        // then z.
        constexpr std::uint8_t kFar = kReach + 1;
        std::vector<std::uint8_t> reach(nodes), next(nodes);
        for (std::size_t at = 0; at < nodes; ++at) reach[at] = charge.mass[at].load(std::memory_order_relaxed) > 0 ? 0 : kFar;
        for (int axis = 0; axis < 3; ++axis) {
            parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
                for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
                    std::int64_t c[3];
                    g.coordinates(at, c);
                    std::uint8_t best = reach[at];
                    for (int d = 1; d <= kReach && best > d; ++d) {
                        for (const int sign : {-1, 1}) {
                            std::int64_t o[3] = {c[0], c[1], c[2]};
                            o[axis] += sign * d;
                            const std::int64_t there = g.node(o[0], o[1], o[2]);
                            if (there >= 0) best = std::min(best, std::max(static_cast<std::uint8_t>(d), reach[static_cast<std::size_t>(there)]));
                        }
                    }
                    next[at] = best;
                }
            }, 16);
            reach.swap(next);
        }

        // The charge seen from far away: all of it at its middle, as a point charge (softened over
        // kSoften cells, Plummer's: Q / (4 pi sqrt(r^2 + eps^2))). Summed a block at a time, then
        // in order: the same on any number of threads.
        std::vector<std::array<double, 4>> moments(g.blockAt.size());
        parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
            std::array<double, 4> m{};
            for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
                const auto q = static_cast<double>(charge.mass[at].load(std::memory_order_relaxed));
                if (!(q > 0.0)) continue;
                std::int64_t c[3];
                g.coordinates(at, c);
                m[0] += q;
                for (int e = 0; e < 3; ++e) m[1 + e] += q * static_cast<double>(c[e]);
            }
            moments[b] = m;
        }, 16);
        std::array<double, 4> all{};
        for (const auto& m : moments) {
            for (int e = 0; e < 4; ++e) all[e] += m[e];
        }
        if (!(all[0] > 0.0)) return in;
        const double total = all[0] / kFixed * g.h * g.h * g.h;  // charge density x volume
        const Vec3f centre(static_cast<float>((all[1] / all[0] + static_cast<double>(g.ox)) * g.h),
                           static_cast<float>((all[2] / all[0] + static_cast<double>(g.oy)) * g.h),
                           static_cast<float>((all[3] / all[0] + static_cast<double>(g.oz)) * g.h));
        const double soften = kSoften * kSoften * g.h * g.h;
        auto far = [&](const Vec3f& x) {
            const auto r2 = static_cast<double>((x - centre).dot(x - centre));
            return static_cast<float>(total / (4.0 * kPi * std::sqrt(r2 + soften)));
        };

        // 2. Solve, over the nodes near charge but not under the scalp: phi there from far away
        // beyond them, and no flux into the scalp (a node under it takes its neighbour's phi).
        std::vector<std::uint32_t> perBlock(g.blockAt.size() + 1, 0);
        auto isUnknown = [&](std::size_t at) { return reach[at] <= kReach && !solid(walls, static_cast<std::int64_t>(at)); };
        parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
            std::uint32_t k = 0;
            for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) k += isUnknown(at) ? 1 : 0;
            perBlock[b + 1] = k;
        }, 16);
        for (std::size_t b = 0; b < g.blockAt.size(); ++b) perBlock[b + 1] += perBlock[b];
        std::vector<Unknown> unknown(perBlock.back());
        std::vector<std::uint8_t> color(unknown.size());
        std::vector<float> phi(nodes, 0.0f);  // (from far away to start with, and beyond the unknowns)
        const double h2 = g.h * g.h;
        parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
            std::uint32_t slot = perBlock[b];
            for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
                std::int64_t c[3];
                g.coordinates(at, c);
                if (!solid(walls, static_cast<std::int64_t>(at))) phi[at] = far(g.position(c));
                if (!isUnknown(at)) continue;
                const std::int64_t i = c[0], j = c[1], k = c[2];
                Unknown& u = unknown[slot];
                u.at = static_cast<std::int64_t>(at);
                u.source = h2 * static_cast<double>(charge.mass[at].load(std::memory_order_relaxed)) / kFixed;
                const std::int64_t around[6][3] = {{i - 1, j, k}, {i + 1, j, k}, {i, j - 1, k}, {i, j + 1, k}, {i, j, k - 1}, {i, j, k + 1}};
                for (int e = 0; e < 6; ++e) {
                    const std::int64_t o = g.node(around[e][0], around[e][1], around[e][2]);
                    if (o >= 0 && isUnknown(static_cast<std::size_t>(o))) {
                        u.around[static_cast<std::size_t>(e)] = o;
                    } else if (solid(walls, o)) {
                        u.around[static_cast<std::size_t>(e)] = -2;
                    } else {
                        u.around[static_cast<std::size_t>(e)] = -1;
                        u.source += static_cast<double>(far(g.position(around[e])));
                    }
                }
                color[slot] = static_cast<std::uint8_t>((i + j + k + g.ox + g.oy + g.oz) & 1);
                ++slot;
            }
        }, 16);
        solve(unknown, color, kSweeps, kOmega, &phi);

        // 3. The field at each node, and (4.) its charge's middle and density.
        std::vector<Vec3f> field(nodes), middle(nodes);
        std::vector<float> density(nodes, 0.0f);
        const auto inv2h = static_cast<float>(0.5 / g.h);
        parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
            for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
                if (solid(walls, static_cast<std::int64_t>(at))) continue;
                std::int64_t c[3];
                g.coordinates(at, c);
                auto phiAt = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
                    const std::int64_t o = g.node(x, y, z);
                    if (o < 0) {
                        const std::int64_t q[3] = {x, y, z};
                        return far(g.position(q));
                    }
                    return solid(walls, o) ? phi[at] : phi[static_cast<std::size_t>(o)];
                };
                Vec3f e = Vec3f(phiAt(c[0] - 1, c[1], c[2]) - phiAt(c[0] + 1, c[1], c[2]),
                                phiAt(c[0], c[1] - 1, c[2]) - phiAt(c[0], c[1] + 1, c[2]),
                                phiAt(c[0], c[1], c[2] - 1) - phiAt(c[0], c[1], c[2] + 1)) * inv2h;
                field[at] = e;
                const std::int64_t here = charge.mass[at].load(std::memory_order_relaxed);
                if (here <= 0) continue;
                // Its charge's middle: the 8 cells around it, each cell's sums moved to the node.
                double sum[4] = {};
                for (int d = 0; d < 8; ++d) {
                    const int dx = d & 1, dy = (d >> 1) & 1, dz = (d >> 2) & 1;
                    const std::int64_t o = g.node(c[0] - dx, c[1] - dy, c[2] - dz);
                    if (o < 0) continue;
                    const std::atomic<std::int64_t>* cs = charge.sums.get() + static_cast<std::size_t>(o) * kCellSums;
                    const double m = static_cast<double>(cs[kMass].load(std::memory_order_relaxed));
                    if (!(m > 0.0)) continue;
                    sum[0] += m;
                    sum[1] += static_cast<double>(cs[kMx].load(std::memory_order_relaxed)) - m * dx;
                    sum[2] += static_cast<double>(cs[kMx + 1].load(std::memory_order_relaxed)) - m * dy;
                    sum[3] += static_cast<double>(cs[kMx + 2].load(std::memory_order_relaxed)) - m * dz;
                }
                if (!(sum[0] > 0.0)) continue;
                middle[at] = g.position(c) + Vec3f(static_cast<float>(sum[1] / sum[0]), static_cast<float>(sum[2] / sum[0]),
                                                   static_cast<float>(sum[3] / sum[0])) * static_cast<float>(g.h);
                density[at] = static_cast<float>(static_cast<double>(here) / kFixed);
            }
        }, 4);

        // 5. Push every strand by its own charge and bend it under the pushes.
        const auto splay = static_cast<float>(kSplay * g.h);
        const auto margin = static_cast<float>(0.25 * g.h + floor.floor);
        auto nearScalp = [&](const Vec3f& p) { return mayTouchScalp(g, walls, kWallReach, margin, p); };
        auto out = std::make_shared<StrandSet>(*in);
        std::vector<Vec3f>& moved = out->mutablePositions();
        std::vector<char> touched(count, 0);
        parallelFor(0, count, [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 2) return;
            const float amount = static_cast<float>(kGain) * std::max(strength.at(*in, s), 0.0f) * chargeOf(out->ids()[s], randomness);
            if (!(amount > 0.0f)) return;
            Vec3f* pts = moved.data() + out->strandBegin(s);
            if (!finitePoints(pts, n)) return;
            float length = 0.0f;  // as it came
            for (std::size_t i = 1; i < n; ++i) length += (pts[i] - pts[i - 1]).length();
            const float most = static_cast<float>(kMostShare) * length;
            if (!(most > 0.0f)) return;
            const Vec3f own = splayOf(out->ids()[s]);
            thread_local std::vector<Vec3f> moves;
            moves.assign(n, Vec3f{});
            std::size_t from = n;  // the first point moved
            for (std::size_t i = 1; i < n; ++i) {
                const float q = amount * std::max(profile(i, n), 0.0f);
                if (!(q > 0.0f)) continue;
                // Its own offset across the strand (the strand's heading here, as it came).
                Vec3f heading = pts[i] - pts[i - 1];
                const float hl = heading.length();
                heading = hl > 0.0f ? heading / hl : Vec3f{};
                const Vec3f across = (own - heading * own.dot(heading)) * splay;
                std::int64_t c[3];
                double f[3];
                g.cellOf(pts[i], c, f);
                Vec3f push{}, local{}, normal{};
                for (int d = 0; d < 8; ++d) {
                    const int dx = d & 1, dy = (d >> 1) & 1, dz = (d >> 2) & 1;
                    const std::int64_t o = g.node(c[0] + dx, c[1] + dy, c[2] + dz);
                    if (o < 0) continue;
                    const auto w = static_cast<float>((dx ? f[0] : 1.0 - f[0]) * (dy ? f[1] : 1.0 - f[1]) * (dz ? f[2] : 1.0 - f[2]));
                    const auto at = static_cast<std::size_t>(o);
                    push += field[at] * w;
                    if (density[at] > 0.0f) local += (pts[i] - middle[at] + across) * (w * density[at] * static_cast<float>(kLocal / 3.0));
                    if (!walls.empty() && walls[at].near) normal += walls[at].normal * w;
                }
                // Below the grid's scale, only what the grid cannot: across the strand and across
                // the field (which spreads the hair along itself already), so a lock fans out into
                // its strands sideways while the field lifts them.
                local -= heading * local.dot(heading);
                const float el = push.length();
                if (el > 0.0f) {
                    const Vec3f along = push / el;
                    local -= along * local.dot(along);
                }
                push += local;
                // No pull toward the scalp (within kWallReach cells of it): the part of the push
                // into it dropped.
                const float nl = normal.length();
                if (nl > 0.0f) {
                    normal = normal / nl;
                    const float into = push.dot(normal);
                    if (into < 0.0f) push -= normal * into;
                }
                Vec3f move = push * q;
                const float len = move.length();
                if (!(len > 0.0f) || !std::isfinite(len)) continue;
                moves[i] = move * (most * pm::tanh(len / most) / len);
                from = std::min(from, i);
            }
            if (from == n) return;
            // The strand bends under the pushes as a cantilever from its root: each segment turns
            // by the moment about its start of the pushes beyond it (sum (p_j - p_i) x f_j w_j, w_j
            // a point's share of the length) times kBend len / L^3, eased to at most kMostTurn
            // len / L, and the turns above carry down the strand. Curvature follows the moment,
            // which changes smoothly along the strand: it never kinks.
            thread_local std::vector<std::array<double, 6>> beyond;  // per point: sum of f w, of (p - p0) x f w
            beyond.assign(n + 1, std::array<double, 6>{});
            for (std::size_t j = n; j-- > 1;) {
                const float share = 0.5f * ((pts[j] - pts[j - 1]).length() + (j + 1 < n ? (pts[j + 1] - pts[j]).length() : 0.0f)) / length;
                const Vec3f fw = moves[j] * share, arm = pts[j] - pts[0];
                const Vec3f t = arm.cross(fw);
                const double add[6] = {fw.x, fw.y, fw.z, t.x, t.y, t.z};
                for (int e = 0; e < 6; ++e) beyond[j][static_cast<std::size_t>(e)] = beyond[j + 1][static_cast<std::size_t>(e)] + add[e];
            }
            thread_local std::vector<Vec3f> bent;
            bent.assign(pts, pts + n);
            Turn carried;
            const double cube = static_cast<double>(length) * length * length;
            std::size_t first = n;
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f segment = pts[i] - pts[i - 1];
                const float len = segment.length();
                if (!(len > 0.0f)) {
                    bent[i] = bent[i - 1];
                    continue;
                }
                const auto& b = beyond[i];
                const Vec3f start = pts[i - 1] - pts[0];
                const Vec3f moment = Vec3f(static_cast<float>(b[3]), static_cast<float>(b[4]), static_cast<float>(b[5])) -
                                     start.cross(Vec3f(static_cast<float>(b[0]), static_cast<float>(b[1]), static_cast<float>(b[2])));
                const float m = moment.length();
                const auto cap = static_cast<float>(kMostTurn * len / length);
                const auto raw = static_cast<float>(kBend * static_cast<double>(m) * len / cube);
                if (raw > 0.0f && std::isfinite(raw)) {
                    carried = carried.then(Turn::about(moment / m, cap * pm::tanh(raw / cap)));
                    first = std::min(first, i);
                }
                bent[i] = bent[i - 1] + carried(segment / len) * len;
            }
            if (first == n) return;
            for (std::size_t i = first; i < n; ++i) moves[i] = bent[i] - pts[i];
            touched[s] = 1;
            walkStrand(*out, s, pts, positions.data() + in->strandBegin(s), first, moves.data(), floor, nearScalp);
        }, 64);
        bool any = false;
        for (const char t : touched) any = any || t;
        if (!any) return in;  // no charge anywhere: the strands as they came
        return StrandsPtr(out);
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
