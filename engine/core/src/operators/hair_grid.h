// SPDX-License-Identifier: Apache-2.0
// The hair grid: what the nodes reading hair as a material share (SelfCollide, Static). The core
// strands' segments, sampled on the Catmull-Rom curve through their points, splatted into a sparse
// grid of cubes `h` wide (blocks of 4 x 4 x 4 nodes where hair is) with fixed-point sums, so the
// same on any number of threads; the scalp at each node (solid under it); a red-black SOR solve
// over a list of nodes; and the walk that moves a strand's points from its root, keeping its
// lengths and keeping it out of the scalp.
//
// The strands splatted are the core (preview_core.h: those every preview holds, each standing for
// the strands of a render it is a share of), at most about `most` of them (a power-of-two share of
// those by ID): the grid is the same at every preview density, and costs no more for a million
// strands than for a few thousand. The cell is about kCellSpacings root spacings of the strands
// splatted (their share of the scalp's area), between 1/128 and 1/16 of the scalp's size, rounded
// on a decimal lattice: the same strands at any scale.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/distance_field.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_run.h"
#include "groomlab/math.h"
#include "groomlab/parallel.h"
#include "groomlab/preview_core.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {
namespace hair_grid {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCellSpacings = 2.0;      // the cell: about this many root spacings of the splatted strands
constexpr double kFinestCell = 1.0 / 128.0;   // ... at least this share of the scalp's size
constexpr double kCoarsestCell = 1.0 / 16.0;  // ... at most this
constexpr double kCellWithoutScalp = 1.0 / 64.0;  // without a scalp: this share of the strands' bounding box
constexpr double kNominalCover = 0.02;     // a strand without widths: this share of its root area, in section
constexpr double kNominalWidth = 1.0 / 4000.0;  // ... without a scalp, a width this share of the strands' size
constexpr std::int64_t kMostBlocks = std::int64_t{1} << 23;  // the block table's cells at most (coarser cells beyond)
constexpr double kFixed = 4294967296.0;    // the sums' fixed point (2^32 a unit of rho)

constexpr int kB = 4;  // a block's nodes along each axis
constexpr int kBlockNodes = kB * kB * kB;

// A node's grid: the core strands it splats at most (a power-of-two share by ID beyond), the salt
// of that share, the nodes laid past the samples each way, the rings of blocks around those, and
// how far (cells) a node looks for the scalp. (One place: the nodes and hairGridLayout read them.)
struct GridSpec {
    std::size_t most;
    std::uint64_t thinSalt;
    std::int64_t margin;
    int dilate;
    double wallReach;
};
constexpr GridSpec kSelfCollideGrid{8192, 0x5E1FC011DE5ull, 2, 0, 1.5};
constexpr GridSpec kStaticGrid{8192, 0x57A71C5EEDull, 2 + 4, 1, 1.5};  // (2 + its kReach; the reach's blocks)
constexpr double kStaticAreaWithoutScalp = 0.125;  // Static without a scalp: this share of the strands' size squared for its area
// A cell's sums: mass, mass x offset (3), mass x offset x offset (xx yy zz xy xz yz), mass x width.
// A grid may keep only the first few (kMx + 3: mass and where it is).
enum : int { kMass = 0, kMx = 1, kMxx = 4, kMw = 10, kSums = 11 };

// Whether every point of a strand is a finite number (a strand that is not is left as it came).
bool finitePoints(const Vec3f* p, std::size_t n);

// The scalp's size: its rest pose's bounding box diagonal, on the decimal lattice (0: none).
float scalpSize(const ScalpMesh* scalp, double scale = 1.0);  // scale: EvalContext::scale, the lattice's anchor

// What keeps pushed points out of the scalp (Interpolate keepClearance's surface and floor).
struct ScalpFloor {
    CollisionSurface surface;
    float floor = 0.0f, depth = 0.0f, searchRadius = 0.0f, beside = 0.0f;

    static ScalpFloor of(const std::shared_ptr<const ScalpMesh>& scalp, float size);
    explicit operator bool() const { return static_cast<bool>(surface); }
};

// The strands a grid splats and how it is laid: `strands` (the core, thinned by ID), `weight` (the
// strands of a render each stands for), the cell `h`, `nominal` (a strand's cross-section without
// widths), whether the strands have widths, the scalp's size and area (0 without one) and, without
// a scalp, the strands' size (their bounding box's diagonal, on the decimal lattice).
struct Layout {
    std::vector<std::size_t> strands;
    double weight = 1.0, h = 0.0, nominal = 0.0, area = 0.0, extent = 0.0;
    float size = 0.0f;
    bool widths = false;
};
// The layout of `in`'s grid: false when there is nothing to splat (no strand of two finite points,
// or a set of no size).
bool layoutOf(const StrandSet& in, const ScalpMesh* scalp, std::size_t most, std::uint64_t thinSalt, Layout* out,
              double scale = 1.0);  // scale: EvalContext::scale, the lattices' anchor

// The sparse grid: nodes at whole multiples of h (the world's), local node (i, j, k) the world's
// (o + i, ...); blocks of 4^3 nodes where samples fall, numbered in the order of the block table.
struct Grid {
    double h = 0.0;
    std::int64_t ox = 0, oy = 0, oz = 0, nx = 0, ny = 0, nz = 0, bx = 0, by = 0, bz = 0;
    std::vector<std::int32_t> blockOf;                 // per block cell: its block, or -1
    std::vector<std::array<std::int32_t, 3>> blockAt;  // per block: its cell

    std::size_t nodes() const { return blockAt.size() * kBlockNodes; }
    std::int64_t blockCell(std::int64_t i, std::int64_t j, std::int64_t k) const { return ((k >> 2) * by + (j >> 2)) * bx + (i >> 2); }
    // A local node's storage index (its block's 64, x fastest), or -1 where no block is.
    std::int64_t node(std::int64_t i, std::int64_t j, std::int64_t k) const {
        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return -1;
        const std::int32_t b = blockOf[static_cast<std::size_t>(blockCell(i, j, k))];
        if (b < 0) return -1;
        return std::int64_t{b} * kBlockNodes + (((k & 3) << 4) | ((j & 3) << 2) | (i & 3));
    }
    // Node `at`'s local coordinates.
    void coordinates(std::size_t at, std::int64_t* c) const {
        const auto& b = blockAt[at / kBlockNodes];
        const std::size_t l = at % kBlockNodes;
        c[0] = std::int64_t{b[0]} * kB + static_cast<std::int64_t>(l & 3);
        c[1] = std::int64_t{b[1]} * kB + static_cast<std::int64_t>((l >> 2) & 3);
        c[2] = std::int64_t{b[2]} * kB + static_cast<std::int64_t>(l >> 4);
    }
    Vec3f position(const std::int64_t* c) const {
        return Vec3f(static_cast<float>(static_cast<double>(c[0] + ox) * h), static_cast<float>(static_cast<double>(c[1] + oy) * h),
                     static_cast<float>(static_cast<double>(c[2] + oz) * h));
    }
    // The local cell of p and where in it p is (0-1 along each axis).
    void cellOf(const Vec3f& p, std::int64_t* c, double* f) const {
        const double q[3] = {p.x / h - static_cast<double>(ox), p.y / h - static_cast<double>(oy), p.z / h - static_cast<double>(oz)};
        for (int a = 0; a < 3; ++a) {
            const double fl = std::floor(q[a]);
            c[a] = static_cast<std::int64_t>(fl);
            f[a] = q[a] - fl;
        }
    }
};

// A sample of a splatted segment: where, the amount it carries (hair's volume, a charge) and its
// strand's width.
struct Sample {
    Vec3f p;
    float volume;
    float width;
};

// The samples of `strands` (indices into s, their points in `positions`), about `spacing` apart
// along each segment, on the Catmull-Rom curve through the points (not their chords, which cut
// inside a bend). Each strand's in its own place: the same layout on any number of threads.
// `amount(k, i, t, len, m)` gives the sample's volume and width: strand strands[k], segment i (len
// long, in m samples), at t along it.
template <class Amount>
std::vector<Sample> sampleStrands(const StrandSet& s, const std::vector<Vec3f>& positions, const std::vector<std::size_t>& strands,
                                  double spacing, const Amount& amount) {
    std::vector<std::size_t> first(strands.size() + 1, 0);
    parallelFor(0, strands.size(), [&](std::size_t k) {
        const std::size_t n = s.strandPointCount(strands[k]);
        const Vec3f* p = positions.data() + s.strandBegin(strands[k]);
        std::size_t m = 0;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            m += static_cast<std::size_t>(std::max(1.0, std::ceil((p[i + 1] - p[i]).length() / spacing)));
        }
        first[k + 1] = m;
    }, 64);
    for (std::size_t k = 0; k < strands.size(); ++k) first[k + 1] += first[k];
    std::vector<Sample> samples(first.back());
    parallelFor(0, strands.size(), [&](std::size_t k) {
        const std::size_t n = s.strandPointCount(strands[k]), b = s.strandBegin(strands[k]);
        const Vec3f* p = positions.data() + b;
        Sample* at = samples.data() + first[k];
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const float len = (p[i + 1] - p[i]).length();
            const auto m = static_cast<std::size_t>(std::max(1.0, std::ceil(len / spacing)));
            const Vec3f p0 = i > 0 ? p[i - 1] : p[i] * 2.0f - p[i + 1];
            const Vec3f p3 = i + 2 < n ? p[i + 2] : p[i + 1] * 2.0f - p[i];
            const Vec3f c1 = (p[i + 1] - p0) * 0.5f, c2 = p0 * 1.0f - p[i] * 2.5f + p[i + 1] * 2.0f - p3 * 0.5f,
                        c3 = (p3 - p0) * 0.5f + (p[i] - p[i + 1]) * 1.5f;
            for (std::size_t j = 0; j < m; ++j) {
                const float t = (static_cast<float>(j) + 0.5f) / static_cast<float>(m);
                const std::pair<float, float> a = amount(k, i, t, len, m);
                *at++ = {p[i] + (c1 + (c2 + c3 * t) * t) * t, a.first, a.second};
            }
        }
    }, 64);
    return samples;
}

// Lays g (its cell already h) over the samples: `margin` nodes beyond them each way (2: a cell's
// far corners and the gradient's neighbours), blocks where their cells' corners are and `dilate`
// blocks around those. Cells twice as coarse while the block table would pass kMostBlocks. False
// when a sample is not finite.
bool layGrid(const std::vector<Sample>& samples, std::int64_t margin, int dilate, Grid* g);

// The scalp near a grid's nodes as the CPU engine takes it (hairGridWalls), kept while the scalp
// and the cell are (the engine uploads it again only for another).
class EngineWalls {
public:
    std::shared_ptr<const std::vector<std::uint32_t>> of(const MeshPtr& scalp, double h, double reach);

private:
    std::mutex lock_;
    MeshPtr scalp_;
    double h_ = 0.0, reach_ = 0.0;
    std::shared_ptr<const std::vector<std::uint32_t>> walls_;
};
// Node `type` (SelfCollide or Static, its stage `kind`, its params) on the CPU engine (look.slang's
// hair grid: one source with the GPU preview's): its grid laid on `in` (the engine's room; not where
// hair spread far wider than the scalp takes coarser cells) and the scalp near its nodes. Nothing
// moved: `in` itself. Null where the engine cannot run it (the operator's C++ then).
StrandsPtr onEngine(gpu::StageKind kind, const std::string& type, const ParamSet& params, const EvalContext& ctx, const StrandsPtr& in,
                    const MeshPtr& scalp, EngineWalls& walls);

}  // namespace hair_grid
}  // namespace groomlab
