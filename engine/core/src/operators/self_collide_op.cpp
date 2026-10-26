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
// the splat, the scalp's walls, the solve and the walk are hair_grid.h's, shared with Static. It
// runs on the CPU engine (look.slang's hair grid, one source with the GPU preview's, its doubles the
// C++ here's to the bit: hair_grid::onEngine); the C++ here where the engine cannot.

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
constexpr double kMostStep = 1.0;          // the farthest a point moves in a pass, in cells
constexpr double kJitter = 0.15;           // a strand's own offset (by its ID) for telling it apart, in cells
constexpr double kSlab = 0.6;              // a slab: its least variance under this share of the next
constexpr double kWallReach = kSelfCollideGrid.wallReach;  // a slab within this many cells of the scalp spreads away from it
constexpr std::uint64_t kThinSalt = kSelfCollideGrid.thinSalt;
constexpr std::uint64_t kJitterSalt = 0x51AB0FF5E7ull;

// A strand's own small offset across a slab, by its ID (in cells, -kJitter/2 to kJitter/2): telling
// apart strands lying exactly together, in the same order wherever they meet.
float rankOf(StrandId id) { return static_cast<float>(kJitter) * (unitFloat(hash64(static_cast<std::uint64_t>(id) ^ kJitterSalt)) - 0.5f); }

// The least eigenvalue of the symmetric c (xx yy zz xy xz yz) and its unit eigenvector, and the
// middle eigenvalue (Smith's closed form, then the eigenvector from c - lambda I's rows).
void leastAxis(const double c[6], double* least, double* middle, Vec3f* axis) {
    const double a = c[0], b = c[1], d = c[2], xy = c[3], xz = c[4], yz = c[5];
    const double off = xy * xy + xz * xz + yz * yz;
    const double q = (a + b + d) / 3.0;
    const double p2 = (a - q) * (a - q) + (b - q) * (b - q) + (d - q) * (d - q) + 2.0 * off;
    if (!(p2 > 1e-30)) {  // a multiple of I
        *least = *middle = q;
        *axis = Vec3f(0.0f, 0.0f, 0.0f);
        return;
    }
    const double p = std::sqrt(p2 / 6.0);
    const double ba = (a - q) / p, bb = (b - q) / p, bd = (d - q) / p, bxy = xy / p, bxz = xz / p, byz = yz / p;
    const double det = ba * (bb * bd - byz * byz) - bxy * (bxy * bd - byz * bxz) + bxz * (bxy * byz - bb * bxz);
    const double phi = pm::acos(std::clamp(det / 2.0, -1.0, 1.0)) / 3.0;
    const double high = q + 2.0 * p * pm::cos(phi);
    const double low = q + 2.0 * p * pm::cos(phi + 2.0 * kPi / 3.0);
    *least = low;
    *middle = 3.0 * q - high - low;
    const double r0[3] = {a - low, xy, xz}, r1[3] = {xy, b - low, yz}, r2[3] = {xz, yz, d - low};
    auto cross = [](const double* u, const double* v, double* w) {
        w[0] = u[1] * v[2] - u[2] * v[1];
        w[1] = u[2] * v[0] - u[0] * v[2];
        w[2] = u[0] * v[1] - u[1] * v[0];
    };
    double best[3] = {0, 0, 0}, w[3];
    double bestLength = 0.0;
    const double* rows[3][2] = {{r0, r1}, {r0, r2}, {r1, r2}};
    for (const auto& pair : rows) {
        cross(pair[0], pair[1], w);
        const double l = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
        if (l > bestLength) {
            bestLength = l;
            std::copy(w, w + 3, best);
        }
    }
    if (!(bestLength > 0.0)) {
        *axis = Vec3f(0.0f, 0.0f, 0.0f);
        return;
    }
    const double l = std::sqrt(bestLength);
    *axis = Vec3f(static_cast<float>(best[0] / l), static_cast<float>(best[1] / l), static_cast<float>(best[2] / l));
}

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
        if (gpu::cpuEngineOn()) {
            if (StrandsPtr made = onEngine(gpu::StageKind::SelfCollide, op::kSelfCollide, params(), ctx, in, scalp, walls_)) return made;
        }
        const double packing = std::max(params().getDouble("packing"), 1e-3);
        const RampAlong profile(params().getRamp("profile"), *in);
        const std::size_t count = in->strandCount();

        // The strands splatted (the core, at most about kMostSplatted of it), the cell, and the
        // cross-section of a strand without widths.
        Layout layout;
        if (!layoutOf(*in, scalp.get(), kMostSplatted, kThinSalt, &layout, ctx.scale)) return in;
        const std::vector<std::size_t>& splatted = layout.strands;
        const double weight = layout.weight, h = layout.h, nominal = layout.nominal;
        const bool widths = layout.widths;
        const float size = layout.size;
        const ScalpFloor floor = ScalpFloor::of(scalp, size);

        auto out = std::make_shared<StrandSet>(*in);
        std::vector<Vec3f>& positions = out->mutablePositions();
        const std::vector<Vec3f>& original = in->positions();  // each segment's length as it came
        std::vector<char> touched(count, 0);

        for (int pass = 0; pass < kPasses; ++pass) {
            // 1. Splat. The samples first (each strand's in its own place: the same layout on any
            // number of threads; the points' offsets across a slab are measured where they are, on
            // the curve), then the grid around them.
            const std::vector<Sample> samples = sampleStrands(*out, positions, splatted, 0.5 * h,
                [&](std::size_t k, std::size_t i, float t, float len, std::size_t m) {
                    double section = nominal, width = std::sqrt(4.0 * nominal / kPi);
                    if (widths) {
                        const float* w = out->widths().data() + out->strandBegin(splatted[k]);
                        width = static_cast<double>(w[i] + (w[i + 1] - w[i]) * t);
                        section = 0.25 * kPi * width * width;
                    }
                    return std::pair<float, float>(static_cast<float>(section * weight * len / static_cast<double>(m)),
                                                   static_cast<float>(width));
                });
            Grid g;
            g.h = h;
            if (!layGrid(samples, kSelfCollideGrid.margin, kSelfCollideGrid.dilate, &g)) return in;
            const std::size_t nodes = g.nodes();
            // Each node's hair (rho, spread trilinearly), and each cell's sums (where its samples
            // are in it: their mass, offsets and products of offsets, and widths).
            const Splat hair = splat(g, samples, kSums);
            const std::atomic<std::int64_t>* mass = hair.mass.get();
            const std::atomic<std::int64_t>* sums = hair.sums.get();

            // 2. The scalp at each node (within kWallReach cells of it): the node's height over it
            // and its normal there. A node under it is solid: no hair's pressure reaches through (its
            // gradient across the surface 0).
            const std::vector<Wall> walls = wallsOf(g, scalp.get(), size, kWallReach);
            auto solid = [&](std::int64_t at) { return hair_grid::solid(walls, at); };

            // Each node: its pressure unknown (overpacked, not solid) and (4.) its spread of a slab
            // (thinner than its hair needs).
            const auto limit = static_cast<std::int64_t>(std::ceil(packing * kFixed));
            std::vector<Spread> spreads(nodes);
            std::vector<std::uint32_t> perBlock(g.blockAt.size() + 1, 0);
            parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
                std::uint32_t overpacked = 0;
                for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
                    const std::int64_t here = mass[at].load(std::memory_order_relaxed);
                    if (here <= 0) continue;
                    if (here > limit && !solid(static_cast<std::int64_t>(at))) ++overpacked;
                    const double rho = static_cast<double>(here) / kFixed;
                    // Its samples: the 8 cells around it, each cell's sums moved to the node (a cell
                    // below it on an axis lies a cell before it there).
                    std::int64_t c[3];
                    g.coordinates(at, c);
                    double sum[kSums] = {};
                    for (int d = 0; d < 8; ++d) {
                        const int dx = d & 1, dy = (d >> 1) & 1, dz = (d >> 2) & 1;
                        const std::int64_t o = g.node(c[0] - dx, c[1] - dy, c[2] - dz);
                        if (o < 0) continue;
                        const std::atomic<std::int64_t>* cs = sums + static_cast<std::size_t>(o) * kSums;
                        double v[kSums];
                        for (int e = 0; e < kSums; ++e) v[e] = static_cast<double>(cs[e].load(std::memory_order_relaxed));
                        if (!(v[kMass] > 0.0)) continue;
                        const double off[3] = {-static_cast<double>(dx), -static_cast<double>(dy), -static_cast<double>(dz)};
                        sum[kMass] += v[kMass];
                        sum[kMw] += v[kMw];
                        for (int e = 0; e < 3; ++e) sum[kMx + e] += v[kMx + e] + v[kMass] * off[e];
                        const int pairs[6][2] = {{0, 0}, {1, 1}, {2, 2}, {0, 1}, {0, 2}, {1, 2}};
                        for (int e = 0; e < 6; ++e) {
                            const int a = pairs[e][0], bb = pairs[e][1];
                            sum[kMxx + e] += v[kMxx + e] + v[kMx + a] * off[bb] + v[kMx + bb] * off[a] + v[kMass] * off[a] * off[bb];
                        }
                    }
                    const double m = sum[kMass];
                    if (!(m > 0.0)) continue;
                    // The thickest its hair could need (all of it off the node by half a cell) against
                    // the thinnest it can be (its strands' width and their own offsets): no spread.
                    const double wide = sum[kMw] / m;
                    if (!(rho / (0.5 * packing) > wide + kJitter)) continue;
                    double mean[3], cov[6];
                    for (int e = 0; e < 3; ++e) mean[e] = sum[kMx + e] / m;
                    const int pairs[6][2] = {{0, 0}, {1, 1}, {2, 2}, {0, 1}, {0, 2}, {1, 2}};
                    for (int e = 0; e < 6; ++e) cov[e] = sum[kMxx + e] / m - mean[pairs[e][0]] * mean[pairs[e][1]];
                    double least = 0.0, middle = 0.0;
                    Vec3f axis;
                    leastAxis(cov, &least, &middle, &axis);
                    if (!(axis.length() > 0.5f) || !(least < kSlab * middle)) continue;  // not a slab
                    least = std::max(least, 0.0) + kJitter * kJitter / 12.0;  // with the strands' own offsets
                    const double offAxis = std::fabs(mean[0] * axis.x + mean[1] * axis.y + mean[2] * axis.z);
                    // The slab's hair per area (in cells), over the node's share of it.
                    const double perArea = rho / std::max(1.0 - offAxis, 0.5);
                    const double between = std::sqrt(12.0 * least);  // the strands' middles' thickness
                    const double needed = perArea / packing;
                    if (!(needed > between + wide) || !(between > 0.0)) continue;
                    // (No thicker than a pass may move it: spaced as they were, not crowded at the limit.)
                    const double k = std::min(needed - wide, between + kMostStep) / between;
                    const Vec3f node = g.position(c);
                    Vec3f middleAt = node + Vec3f(static_cast<float>(mean[0]), static_cast<float>(mean[1]), static_cast<float>(mean[2])) *
                                                static_cast<float>(g.h);
                    if (!walls.empty() && walls[at].near && walls[at].height > -static_cast<float>(g.h)) {
                        // On the scalp: from the slab's bottom, away from it.
                        if (axis.dot(walls[at].normal) < 0.0f) axis = axis * -1.0f;
                        middleAt -= axis * static_cast<float>(0.5 * between * g.h);
                    }
                    spreads[at] = {axis, middleAt, static_cast<float>(k - 1.0)};
                }
                perBlock[b + 1] = overpacked;
            }, 16);
            for (std::size_t b = 0; b < g.blockAt.size(); ++b) perBlock[b + 1] += perBlock[b];
            const std::size_t unknowns = perBlock.back();

            // 3. The pressure, over the overpacked nodes (a solid neighbour: the node's own, no flow
            // across).
            std::vector<float> p(nodes, 0.0f);
            if (unknowns > 0) {
                // (Each one's source: h^2 times the spread asked, (rho - packing) / rho.)
                std::vector<Unknown> unknown(unknowns);
                std::vector<std::uint8_t> color(unknowns);
                const double h2 = g.h * g.h;
                parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
                    std::uint32_t next = perBlock[b];
                    for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
                        const std::int64_t here = mass[at].load(std::memory_order_relaxed);
                        if (here <= limit || solid(static_cast<std::int64_t>(at))) continue;
                        std::int64_t c[3];
                        g.coordinates(at, c);
                        const std::int64_t i = c[0], j = c[1], k = c[2];
                        const double rho = static_cast<double>(here) / kFixed;
                        Unknown& u = unknown[next];
                        u.at = static_cast<std::int64_t>(at);
                        u.around = {g.node(i - 1, j, k), g.node(i + 1, j, k), g.node(i, j - 1, k),
                                    g.node(i, j + 1, k), g.node(i, j, k - 1), g.node(i, j, k + 1)};
                        for (std::int64_t& o : u.around) o = solid(o) ? -2 : o;
                        u.source = h2 * (rho - packing) / rho;
                        color[next] = static_cast<std::uint8_t>((i + j + k + g.ox + g.oy + g.oz) & 1);
                        ++next;
                    }
                }, 16);
                solve(unknown, color, kSweeps, kOmega, &p);
            }
            // Its gradient at every node (central differences; 0 where no pressure is near, and in
            // solid nodes).
            std::vector<Vec3f> gradient(nodes);
            if (unknowns > 0) {
                const auto inv2h = static_cast<float>(0.5 / g.h);
                parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
                    for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
                        if (solid(static_cast<std::int64_t>(at))) continue;
                        std::int64_t c[3];
                        g.coordinates(at, c);
                        auto pAt = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
                            const std::int64_t o = g.node(x, y, z);
                            return o < 0 ? 0.0f : solid(o) ? p[at] : p[static_cast<std::size_t>(o)];
                        };
                        gradient[at] = Vec3f(pAt(c[0] + 1, c[1], c[2]) - pAt(c[0] - 1, c[1], c[2]),
                                             pAt(c[0], c[1] + 1, c[2]) - pAt(c[0], c[1] - 1, c[2]),
                                             pAt(c[0], c[1], c[2] + 1) - pAt(c[0], c[1], c[2] - 1)) * inv2h;
                    }
                }, 4);
            }

            // Whether p may be under the scalp's floor: its height over the scalp from its cell's
            // corners (each one's, carried along its normal) under a quarter of a cell over the floor
            // (a corner without the scalp within reach: as far as the reach).
            const auto margin = static_cast<float>(0.25 * g.h + floor.floor);
            auto nearScalp = [&](const Vec3f& p) { return mayTouchScalp(g, walls, kWallReach, margin, p); };

            // 5. Push every strand, walked from its root keeping its lengths.
            const auto most = static_cast<float>(kMostStep * g.h);
            const auto cell = static_cast<float>(g.h);
            bool movedAny = false;
            std::vector<char> movedNow(count, 0);
            parallelFor(0, count, [&](std::size_t s) {
                const std::size_t n = out->strandPointCount(s);
                if (n < 2) return;
                const float amount = std::clamp(strength.at(*in, s), 0.0f, 1.0f);
                if (!(amount > 0.0f)) return;
                Vec3f* pts = positions.data() + out->strandBegin(s);
                if (!finitePoints(pts, n)) return;
                const float own = rankOf(out->ids()[s]) * cell;
                thread_local std::vector<Vec3f> moves;
                moves.assign(n, Vec3f{});
                std::size_t from = n;  // the first point moved
                for (std::size_t i = 1; i < n; ++i) {
                    const float along = amount * std::clamp(profile(i, n), 0.0f, 1.0f);
                    if (!(along > 0.0f)) continue;
                    std::int64_t c[3];
                    double f[3];
                    g.cellOf(pts[i], c, f);
                    Vec3f move{};
                    bool any = false;
                    for (int d = 0; d < 8; ++d) {
                        const int dx = d & 1, dy = (d >> 1) & 1, dz = (d >> 2) & 1;
                        const std::int64_t o = g.node(c[0] + dx, c[1] + dy, c[2] + dz);
                        if (o < 0) continue;
                        const Vec3f& gn = gradient[static_cast<std::size_t>(o)];
                        const Spread& sp = spreads[static_cast<std::size_t>(o)];
                        const bool pushed = gn.x != 0.0f || gn.y != 0.0f || gn.z != 0.0f;
                        if (!pushed && !(sp.spread > 0.0f)) continue;
                        const auto w = static_cast<float>((dx ? f[0] : 1.0 - f[0]) * (dy ? f[1] : 1.0 - f[1]) * (dz ? f[2] : 1.0 - f[2]));
                        if (pushed) move -= gn * w;
                        if (sp.spread > 0.0f) move += sp.axis * (sp.spread * (sp.axis.dot(pts[i] - sp.from) + own) * w);
                        any = true;
                    }
                    if (!any) continue;
                    move = move * along;
                    const float len = move.length();
                    if (!(len > 0.0f)) continue;
                    if (len > most) move = move * (most / len);
                    moves[i] = move;
                    from = std::min(from, i);
                }
                if (from == n) return;
                touched[s] = 1;
                movedNow[s] = 1;
                // Follow the leader from the first point moved, each segment its length as it came.
                walkStrand(*out, s, pts, original.data() + in->strandBegin(s), from, moves.data(), floor, nearScalp);
            }, 64);
            for (const char m : movedNow) movedAny = movedAny || m;
            if (!movedAny) break;  // nothing overpacked: every later pass would find the same
        }
        bool any = false;
        for (const char t : touched) any = any || t;
        if (!any) return in;  // nothing overpacked: the strands as they came
        return StrandsPtr(out);
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
