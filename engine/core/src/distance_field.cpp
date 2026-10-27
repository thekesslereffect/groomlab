// SPDX-License-Identifier: Apache-2.0
#include "groomlab/distance_field.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <thread>

#include "groomlab/follow_scalp.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/process_caches.h"

// A push's steps (locate, regionAt, brickOf), inlined into it also where a build inlines only what
// it is told to (MSVC's RelWithDebInfo, /Ob1): most points a strand's walk asks are settled within a
// few loads of the region or brick they fall in, and the calls around those loads cost as much.
#if defined(_MSC_VER)
#define GROOMLAB_FIELD_INLINE __forceinline
#else
#define GROOMLAB_FIELD_INLINE inline __attribute__((always_inline))
#endif

namespace groomlab {

namespace {

constexpr int kBrickCells = 4;                    // cells per brick side
constexpr int kBrickNodes = kBrickCells + 1;      // nodes per brick side (each brick holds its own faces)
constexpr int kNodesPerBrick = kBrickNodes * kBrickNodes * kBrickNodes;
constexpr int kRegionBricks = 4;                  // bricks per region side
constexpr int kBricksPerRegion = kRegionBricks * kRegionBricks * kRegionBricks;
constexpr std::size_t kMaxRegions = std::size_t{1} << 20;  // the region table's size at most
constexpr float kSqrt3 = 1.7320508f;
constexpr float kInf = std::numeric_limits<float>::infinity();
// Corners of a cell whose normals are further apart than this (about 2.6 degrees) are across a
// crease, a sliver of triangles or the medial axis: the normal between them is not their blend.
constexpr float kCreaseCosine = 0.999f;
// Probe::relaxed's (about 26 degrees), squared as the test takes it (look.slang RELAXED_CREASE2).
constexpr float kRelaxedCrease2 = 0.81f;
constexpr float kCrease2 = kCreaseCosine * kCreaseCosine;
// The grid's nodes sit half a cell off the world's whole multiples of the cell: a mesh symmetric
// about a plane through the origin (a head about x = 0) has its middle, where the closest point
// jumps from one side to the other, inside cells, whose corners on both sides tell it, not on
// their faces (a cell on one side alone would blend across it).
constexpr float kGridShift = 0.5f;

// Corner k of a cell (bit 0 x, bit 1 y, bit 2 z), as a node offset in a brick.
constexpr int cornerOffset(int k) {
    return (k & 1) + ((k >> 1) & 1) * kBrickNodes + ((k >> 2) & 1) * kBrickNodes * kBrickNodes;
}

// A brick slot table's or region's state: not made, being made by a thread, made.
constexpr std::uint8_t kEmpty = 0, kMaking = 1, kMade = 2;
// A node's word before it holds a node: not sampled, being sampled by a thread (no node's bits:
// its distance, a half float rounded up to at most infinity, is never 0xFFFF).
constexpr std::uint64_t kUnsampled = ~std::uint64_t{0}, kSampling = ~std::uint64_t{0} - 1;

// Waits for another thread to finish making what `state` guards (it makes it from exact queries
// alone: the same as it would be here).
void waitFor(const std::atomic<std::uint8_t>& state) {
    while (state.load(std::memory_order_acquire) != kMade) std::this_thread::yield();
}

// Half floats for the nodes' distances (only ever compared against the search radius, a few
// cells either side of it: 3 significant digits do), rounded up so a distance is never made
// smaller than it is; infinity for a miss.
std::uint16_t halfUp(float v) {
    if (!(v < 65000.0f)) return 0x7C00;  // infinity
    if (v <= 0.0f) return 0;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    if (exponent <= 0) return 0x0400;  // below the smallest normal half: up to it
    const std::uint32_t mantissa = bits & 0x7FFFFF;
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1FFF) ++half;  // round up (a carry into the exponent is still right)
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(half, 0x7C00));
}
inline float fromHalf(std::uint16_t h) {
    if (h >= 0x7C00) return kInf;
    const std::uint32_t exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0) return static_cast<float>(mantissa) * (1.0f / 16777216.0f);  // 2^-24
    const std::uint32_t bits = ((exponent + 112u) << 23) | (mantissa << 13);  // rebias 15 -> 127
    float v = 0.0f;
    std::memcpy(&v, &bits, 4);
    return v;
}

// How far a cell's corner k is from its corner 0, in cells.
constexpr float kCornerApart[8] = {0.0f, 1.0f, 1.0f, 1.4142136f, 1.0f, 1.4142136f, 1.4142136f, 1.7320508f};

std::int16_t unitShort(float v) { return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

// A unit normal in two numbers (the octahedral map: the octahedron |x| + |y| + |z| = 1 unfolded
// onto a square), and back: within about 1e-4 radians at 16 bits.
void toOctahedral(const Vec3f& n, std::int16_t out[2]) {
    const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    float u = n.x / l1, v = n.y / l1;
    if (n.z < 0.0f) {
        const float fu = (1.0f - std::fabs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
        const float fv = (1.0f - std::fabs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        u = fu;
        v = fv;
    }
    out[0] = unitShort(u);
    out[1] = unitShort(v);
}
// Unnormalized (on the octahedron): the blends and crease tests take it as it is, the push's
// normal is normalized once.
inline Vec3f fromOctahedral(const std::int16_t in[2]) {
    const float u = static_cast<float>(in[0]) * (1.0f / 32767.0f), v = static_cast<float>(in[1]) * (1.0f / 32767.0f);
    Vec3f n(u, v, 1.0f - std::fabs(u) - std::fabs(v));
    if (n.z < 0.0f) {
        n.x = (1.0f - std::fabs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
        n.y = (1.0f - std::fabs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
    }
    return n;
}

}  // namespace

// A fine brick's nodes (x fastest), each sampled the first time a query needs it: a Node's 8
// bytes in one atomic word (a query reads each corner with one load), kUnsampled until then; each
// node's closest face, written before its word.
struct MeshDistanceField::Nodes {
    std::atomic<std::uint64_t> words[kNodesPerBrick];
    std::uint32_t faces[kNodesPerBrick];
    Nodes() {
        for (std::atomic<std::uint64_t>& w : words) w.store(kUnsampled, std::memory_order_relaxed);
        std::fill(faces, faces + kNodesPerBrick, kTableNone);
    }
};

struct MeshDistanceField::Brick {
    bool fine = false;
    // No point in it is nearer the surface than distanceLow, and every point within the search
    // radius is at least heightLow above it (-inf: no such claim): a far brick's bounds, which a
    // fine one holds too.
    float heightLow = -kInf, distanceLow = 0.0f;
    std::uint32_t face = kTableNone;  // its middle's closest face (kTableNone: none within the reach)
    // A fine brick: its first node's position and its nodes, made the first time a query needs
    // one (nodesOf): most fine bricks a strand's walk meets are settled by their bounds alone
    // (keepClearance's band reaches well above the skin), and never sample a node.
    Vec3f corner;
    mutable std::atomic<Nodes*> nodes{nullptr};
    ~Brick() { delete nodes.load(std::memory_order_acquire); }
};

// Each vertex's triangles (in order), for the answers from closest faces, and each triangle's
// bound (triangleBound).
struct MeshDistanceField::Rings {
    std::vector<std::uint32_t> start, faces;
    std::vector<TriangleBound> bounds;
};

MeshDistanceField::TriangleBound MeshDistanceField::triangleBound(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    // The point closestBarycentric makes, rounded, is off the triangle by up to about
    // 70 eps e^3 L^2 / |n|^2 (e its longest edge, n twice its area, L p's distance from its
    // corners: the rounding of the products its weights are made of, against the area squared they
    // divide by) plus a few eps of the coordinates' size. The bound holds while that is within
    // kSlack of the radius (taken 15 times over), up to the distance from the center `within` says:
    // a sliver's or a degenerate triangle's, none.
    constexpr double kEps = 1.0 / 16777216.0, kSlack = 0.02;
    const double ax = a.x, ay = a.y, az = a.z, bx = b.x, by = b.y, bz = b.z, cx = c.x, cy = c.y, cz = c.z;
    TriangleBound t;
    t.center = Vec3f(static_cast<float>((ax + bx + cx) / 3.0), static_cast<float>((ay + by + cy) / 3.0),
                     static_cast<float>((az + bz + cz) / 3.0));
    auto apart2 = [](double x0, double y0, double z0, double x1, double y1, double z1) {
        return (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0);
    };
    const double mx = t.center.x, my = t.center.y, mz = t.center.z;
    const double radius = std::sqrt(std::max({apart2(mx, my, mz, ax, ay, az), apart2(mx, my, mz, bx, by, bz), apart2(mx, my, mz, cx, cy, cz)}));
    const double edge = std::sqrt(std::max({apart2(ax, ay, az, bx, by, bz), apart2(bx, by, bz, cx, cy, cz), apart2(cx, cy, cz, ax, ay, az)}));
    const double ux = bx - ax, uy = by - ay, uz = bz - az, vx = cx - ax, vy = cy - ay, vz = cz - az;
    const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const double n2 = nx * nx + ny * ny + nz * nz;
    double size = 0.0;
    for (const double v : {ax, ay, az, bx, by, bz, cx, cy, cz}) size = std::max(size, std::fabs(v));
    t.reach = static_cast<float>((radius * (1.0 + kSlack) + 32.0 * kEps * size) * (1.0 + 1e-6) + 1e-30);
    t.within = -1.0f;
    if (n2 > 0.0 && radius > 0.0 && std::isfinite(n2) && std::isfinite(size)) {
        const double k = 1024.0 * kEps * edge * edge * edge / n2;
        const double farthest = std::sqrt(kSlack * radius / k) - radius;  // |p - center| at most
        if (farthest > 0.0) t.within = static_cast<float>(farthest * farthest * 0.999);
    }
    return t;
}

struct MeshDistanceField::Region {
    std::atomic<std::uint8_t> state{kEmpty};
    bool far = false;  // nothing in it within a few cells of the surface: these bounds hold for it all
    float heightLow = -kInf, distanceLow = 0.0f;
    std::atomic<std::uint8_t> slotsState{kEmpty};
    std::unique_ptr<std::atomic<Brick*>[]> bricks;  // its bricks' slots, made when first needed
    ~Region() {
        if (!bricks) return;
        for (int k = 0; k < kBricksPerRegion; ++k) delete bricks[k].load(std::memory_order_acquire);
    }
};

struct MeshDistanceField::Located {
    const Region* region = nullptr;
    int bx = 0, by = 0, bz = 0;  // the brick, in the whole grid
    int cell = 0;                // its cell's first corner, as a node of the brick
    Vec3f f;                     // p's place in the cell, 0-1 per axis
};

namespace {
std::atomic<std::uint64_t> fieldSerials{0};  // fields made in the process
}  // namespace

std::size_t MeshDistanceField::madeCount() { return static_cast<std::size_t>(fieldSerials.load()); }

MeshDistanceField::MeshDistanceField(std::shared_ptr<const ScalpMesh> mesh, float cellSize, float reach, int bandCells)
    : mesh_(std::move(mesh)), cell_(cellSize), reach_(reach) {
    serial_ = ++fieldSerials;
    brickSize_ = cell_ * static_cast<float>(kBrickCells);
    regionSize_ = brickSize_ * static_cast<float>(kRegionBricks);
    band_ = static_cast<float>(std::max(bandCells, 1)) * cell_;
    heightScale_ = 8.0f * band_ / 32767.0f;  // heights to +-8 bands at 1/1000th of a cell
    Vec3f lo{kInf, kInf, kInf}, hi{-kInf, -kInf, -kInf};
    for (const Vec3f& p : mesh_->points()) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    if (mesh_->points().empty() || !(cell_ > 0.0f) || !std::isfinite(cell_)) return;
    inverseCell_ = 1.0f / cell_;
    // Around the mesh, as far as the reach goes and half the mesh's size (strands rarely stray
    // farther, and points outside the grid ask the mesh), within the table's size; from and to
    // whole regions of the world's grid.
    const Vec3f extent = hi - lo;
    float margin = std::min(reach_, 0.5f * std::max({extent.x, extent.y, extent.z})) + regionSize_;
    constexpr double kMaxRegionIndex = 1 << 19;  // cells within 2^23 of the origin: whole floats
    std::size_t count = 0;
    double first[3] = {0, 0, 0};
    const double regionCells = kBrickCells * kRegionBricks;
    for (;;) {
        count = 1;
        bool inRange = true;
        for (int a = 0; a < 3; ++a) {
            const double l = a == 0 ? lo.x : a == 1 ? lo.y : lo.z, h = a == 0 ? hi.x : a == 1 ? hi.y : hi.z;
            first[a] = std::floor(((l - margin) / cell_ - kGridShift) / regionCells);
            const double last = std::ceil(((h + margin) / cell_ - kGridShift) / regionCells);
            inRange = inRange && std::fabs(first[a]) <= kMaxRegionIndex && std::fabs(last) <= kMaxRegionIndex;
            regionDims_[a] = inRange ? std::max(1, static_cast<int>(last - first[a])) : 1;
            count *= static_cast<std::size_t>(regionDims_[a]);
        }
        if (!inRange) count = kMaxRegions + 1;
        if (count <= kMaxRegions || margin <= regionSize_) break;
        margin = std::max(regionSize_, margin * 0.75f);
    }
    if (count > kMaxRegions) {  // a mesh too big for this cell size (or too far out): no grid, every query asks
        regionDims_[0] = regionDims_[1] = regionDims_[2] = 0;
        return;
    }
    for (int a = 0; a < 3; ++a) {
        originRegion_[a] = static_cast<int>(first[a]);
        cellLo_[a] = static_cast<float>(originRegion_[a] * kBrickCells * kRegionBricks);
        cellHi_[a] = static_cast<float>((originRegion_[a] + regionDims_[a]) * kBrickCells * kRegionBricks);
    }
    for (int a = 0; a < 3; ++a) (a == 0 ? origin_.x : a == 1 ? origin_.y : origin_.z) = (cellLo_[a] + kGridShift) * cell_;
    regions_.reset(new Region[count]);
}

MeshDistanceField::~MeshDistanceField() = default;

float MeshDistanceField::cellSizeFor(const ScalpMesh& mesh, float offset) {
    Vec3f lo{kInf, kInf, kInf}, hi{-kInf, -kInf, -kInf};
    for (const Vec3f& p : mesh.points()) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const float diagonal = mesh.points().empty() ? 1.0f : std::max((hi - lo).length(), 1e-6f);
    const float wanted = std::clamp(offset, diagonal / 1024.0f, diagonal / 128.0f);
    return pm::exp2(std::round(pm::log2(wanted)));
}

float MeshDistanceField::reachFor(float searchRadius) {
    return pm::exp2(std::ceil(pm::log2(std::max(searchRadius, 1e-6f))));
}

GROOMLAB_FIELD_INLINE const MeshDistanceField::Region& MeshDistanceField::regionAt(int bx, int by, int bz) const {
    // (Bricks of the grid: none negative, divided as unsigned.)
    auto regionOf = [](int b) { return static_cast<int>(static_cast<unsigned>(b) / kRegionBricks); };
    const int rx = regionOf(bx), ry = regionOf(by), rz = regionOf(bz);
    const std::size_t index = static_cast<std::size_t>(rx) +
                              static_cast<std::size_t>(regionDims_[0]) * (ry + static_cast<std::size_t>(regionDims_[1]) * rz);
    const Region& r = regions_[index];
    if (r.state.load(std::memory_order_acquire) == kMade) return r;
    return makeRegion(index, rx, ry, rz);
}

const MeshDistanceField::Region& MeshDistanceField::makeRegion(std::size_t index, int rx, int ry, int rz) const {
    Region& r = regions_[index];
    std::uint8_t expected = kEmpty;
    if (!r.state.compare_exchange_strong(expected, kMaking, std::memory_order_acq_rel)) {
        waitFor(r.state);
        return r;
    }
    const float halfDiagonal = 0.5f * kSqrt3 * regionSize_;
    const Vec3f middle =
        origin_ + Vec3f(static_cast<float>(rx) + 0.5f, static_cast<float>(ry) + 0.5f, static_cast<float>(rz) + 0.5f) * regionSize_;
    float bound = 0.0f;
    const SurfacePoint at = mesh_->closestPointBounded(middle, reach_ + halfDiagonal, &bound);
    queries_.fetch_add(1, std::memory_order_relaxed);
    if (at.root.face < 0) {
        r.far = true;  // nothing within the reach of any point in it
        r.distanceLow = bound - halfDiagonal;
    } else {
        // The distance changes no faster than the point moves; the height (the distance times the
        // cosine to the closest point's normal, which turns as the point moves around a crease or
        // an open edge) at most about twice as fast.
        const float distance = (middle - at.position).length();
        r.far = distance > band_ + halfDiagonal;
        r.distanceLow = distance - halfDiagonal;
        r.heightLow = (middle - at.position).dot(at.normal) - 2.0f * halfDiagonal;
    }
    regionsMade_.fetch_add(1, std::memory_order_relaxed);
    r.state.store(kMade, std::memory_order_release);
    return r;
}

MeshDistanceField::Brick* MeshDistanceField::makeBrick(int bx, int by, int bz) const {
    auto brick = std::make_unique<Brick>();
    const float halfDiagonal = 0.5f * kSqrt3 * brickSize_;
    brick->corner = origin_ + Vec3f(static_cast<float>(bx), static_cast<float>(by), static_cast<float>(bz)) * brickSize_;
    const Vec3f middle = brick->corner + Vec3f(0.5f, 0.5f, 0.5f) * brickSize_;
    float bound = 0.0f;
    const SurfacePoint at = mesh_->closestPointBounded(middle, reach_ + halfDiagonal, &bound);
    queries_.fetch_add(1, std::memory_order_relaxed);
    if (at.root.face < 0) {
        brick->distanceLow = bound - halfDiagonal;
        far_.fetch_add(1, std::memory_order_relaxed);
        return brick.release();
    }
    brick->face = static_cast<std::uint32_t>(at.root.face);
    const float distance = (middle - at.position).length();
    if (distance > band_ + halfDiagonal) {  // as a region's bounds, tighter
        brick->distanceLow = distance - halfDiagonal;
        brick->heightLow = (middle - at.position).dot(at.normal) - 2.0f * halfDiagonal;
        far_.fetch_add(1, std::memory_order_relaxed);
        return brick.release();
    }
    brick->fine = true;
    // Its bounds too, as a far brick's (a caller probing may skip its nodes where they settle a point).
    brick->distanceLow = distance - halfDiagonal;
    brick->heightLow = (middle - at.position).dot(at.normal) - 2.0f * halfDiagonal;
    fine_.fetch_add(1, std::memory_order_relaxed);
    return brick.release();
}

MeshDistanceField::Nodes& MeshDistanceField::nodesOf(const Brick& brick) const {
    if (Nodes* n = brick.nodes.load(std::memory_order_acquire)) return *n;
    // Made here; when another thread made them first, theirs (nothing sampled in ours yet).
    auto made = std::make_unique<Nodes>();
    Nodes* expected = nullptr;
    if (brick.nodes.compare_exchange_strong(expected, made.get(), std::memory_order_acq_rel)) {
        nodeBlocks_.fetch_add(1, std::memory_order_relaxed);
        return *made.release();
    }
    return *expected;
}

const MeshDistanceField::Brick& MeshDistanceField::brickAt(const Region& region, int bx, int by, int bz) const {
    Region& r = const_cast<Region&>(region);  // its slots are made on demand, guarded by their state
    if (r.slotsState.load(std::memory_order_acquire) != kMade) {
        std::uint8_t expected = kEmpty;
        if (r.slotsState.compare_exchange_strong(expected, kMaking, std::memory_order_acq_rel)) {
            r.bricks.reset(new std::atomic<Brick*>[kBricksPerRegion]);
            for (int k = 0; k < kBricksPerRegion; ++k) r.bricks[k].store(nullptr, std::memory_order_relaxed);
            r.slotsState.store(kMade, std::memory_order_release);
        } else {
            waitFor(r.slotsState);
        }
    }
    const int local = bx % kRegionBricks + (by % kRegionBricks) * kRegionBricks + (bz % kRegionBricks) * kRegionBricks * kRegionBricks;
    std::atomic<Brick*>& slot = r.bricks[local];
    if (Brick* b = slot.load(std::memory_order_acquire)) return *b;
    // Made here; when another thread made it first, theirs (from the same exact queries: the same).
    std::unique_ptr<Brick> made(makeBrick(bx, by, bz));
    Brick* expected = nullptr;
    if (slot.compare_exchange_strong(expected, made.get(), std::memory_order_acq_rel)) return *made.release();
    (made->fine ? fine_ : far_).fetch_sub(1, std::memory_order_relaxed);
    return *expected;
}

MeshDistanceField::Node MeshDistanceField::nodeAt(const Brick& brick, int index, float within, std::uint32_t seedFace) const {
    Nodes& nodes = nodesOf(brick);
    std::atomic<std::uint64_t>& slot = nodes.words[index];
    std::uint64_t word = slot.load(std::memory_order_acquire);
    Node node;
    if (word < kSampling) {
        std::memcpy(&node, &word, sizeof(node));
        return node;
    }
    std::uint64_t expected = kUnsampled;
    if (word != kUnsampled || !slot.compare_exchange_strong(expected, kSampling, std::memory_order_acq_rel)) {
        // Another thread samples it (the same exact query: the same node).
        while ((word = slot.load(std::memory_order_acquire)) >= kSampling) std::this_thread::yield();
        std::memcpy(&node, &word, sizeof(node));
        return node;
    }
    const int x = index % kBrickNodes, y = (index / kBrickNodes) % kBrickNodes, z = index / (kBrickNodes * kBrickNodes);
    const Vec3f p = brick.corner + Vec3f(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) * cell_;
    // Within the reach of any point of the cells around it. A bound at or past the true distance
    // only prunes boxes no nearer triangle is in: the same triangle as without it. The distance to a
    // face known to be near (the brick's middle's closest, a neighbouring node's) is such a bound,
    // and a tight one: far fewer of the tree's boxes opened.
    float bound = std::min(reach_ + kSqrt3 * cell_, within);
    const float slack = 1e-6f * (std::fabs(p.x) + std::fabs(p.y) + std::fabs(p.z) + 1.0f);
    for (const std::uint32_t face : {brick.face, seedFace}) {
        if (face == kTableNone || face >= mesh_->triangleCount()) continue;
        const SurfacePoint near = mesh_->closestPointOnFace(p, face);
        bound = std::min(bound, (near.position - p).length() * 1.0001f + slack);
    }
    const SurfacePoint sp = mesh_->closestPoint(p, bound);
    if (sp.root.face < 0) {
        node = Node{32767, 0x7C00, {0, 0}};
    } else {
        const float height = (p - sp.position).dot(sp.normal) / heightScale_;
        node.height = static_cast<std::int16_t>(std::lround(std::clamp(height, -32767.0f, 32767.0f)));
        node.distance = halfUp((p - sp.position).length());
        toOctahedral(sp.normal, node.normal);
        nodes.faces[index] = static_cast<std::uint32_t>(sp.root.face);  // (released with the node)
    }
    nodes_.fetch_add(1, std::memory_order_relaxed);  // (a query each: stats counts them)
    std::memcpy(&word, &node, sizeof(node));  // a distance's bits never reach 0xFFFF: never a marker
    slot.store(word, std::memory_order_release);
    return node;
}

namespace {

// The bricks a thread met last (by field and grid place): a strand's walk meets the same few
// again on every pass, and the slot tables are then not read again. (No initializers: zeroed, a
// field serial no field has, and nothing to construct on a thread's first use.)
struct BrickMemo {
    std::uint64_t field;  // the field's serial (an address may be another field's later)
    int bx, by, bz;
    const void* brick;
};
constexpr int kBrickMemo = 32;
thread_local BrickMemo tBrickMemo[kBrickMemo];

// floor(g) for g within 2^23 of 0 (the grid's cells: whole floats), as the int it is.
inline int wholeBelow(float g) {
    const int i = static_cast<int>(g);
    return static_cast<float>(i) > g ? i - 1 : i;
}
// std::clamp(v, 0, 1), by value.
inline float unitClamp(float v) { return v < 0.0f ? 0.0f : 1.0f < v ? 1.0f : v; }

}  // namespace

GROOMLAB_FIELD_INLINE bool MeshDistanceField::locate(const Vec3f& p, Located* at) const {
    if (!regions_) return false;
    // In cells from the world's origin (a power-of-two cell: exact), from p alone: the same cell
    // and place in it whatever grid holds it.
    const float gx = p.x * inverseCell_ - kGridShift, gy = p.y * inverseCell_ - kGridShift, gz = p.z * inverseCell_ - kGridShift;
    if (!(gx >= cellLo_[0] && gx < cellHi_[0] && gy >= cellLo_[1] && gy < cellHi_[1] && gz >= cellLo_[2] && gz < cellHi_[2])) {
        return false;  // outside the grid (or not a number)
    }
    // The cell from the grid's first (exact: whole floats; at or past it, so none negative), and
    // p's place in it.
    const int wx = wholeBelow(gx), wy = wholeBelow(gy), wz = wholeBelow(gz);
    const unsigned lx = static_cast<unsigned>(wx - static_cast<int>(cellLo_[0]));
    const unsigned ly = static_cast<unsigned>(wy - static_cast<int>(cellLo_[1]));
    const unsigned lz = static_cast<unsigned>(wz - static_cast<int>(cellLo_[2]));
    at->bx = static_cast<int>(lx / kBrickCells);
    at->by = static_cast<int>(ly / kBrickCells);
    at->bz = static_cast<int>(lz / kBrickCells);
    at->region = &regionAt(at->bx, at->by, at->bz);
    at->cell = static_cast<int>(lx % kBrickCells + (ly % kBrickCells) * kBrickNodes + (lz % kBrickCells) * kBrickNodes * kBrickNodes);
    at->f = Vec3f(unitClamp(gx - static_cast<float>(wx)), unitClamp(gy - static_cast<float>(wy)),
                  unitClamp(gz - static_cast<float>(wz)));
    return true;
}

GROOMLAB_FIELD_INLINE const MeshDistanceField::Brick& MeshDistanceField::brickOf(const Located& at) const {
    BrickMemo& memo = tBrickMemo[(at.bx * 7 + at.by * 13 + at.bz * 31) & (kBrickMemo - 1)];
    if (memo.field == serial_ && memo.bx == at.bx && memo.by == at.by && memo.bz == at.bz) {
        return *static_cast<const Brick*>(memo.brick);
    }
    const Brick& brick = brickAt(*at.region, at.bx, at.by, at.bz);
    memo = BrickMemo{serial_, at.bx, at.by, at.bz, &brick};
    return brick;
}

bool MeshDistanceField::blend(const Brick& brick, int cell, const Vec3f& f, float maxDepth, float searchRadius, float offset,
                              float* height, Vec3f* normal, bool* clear, Probe* probe) const {
    *clear = false;
    Node n[8];
    float h[8], lowest = kInf;
    // Half floats of distances above 0 order as their bits do: the nearest and farthest read off
    // the bits, two decoded.
    std::uint16_t nearestBits = 0xFFFF, farthestBits = 0;
    // The corners, each one load; sampled first where one is not yet (nodeAt), each bounded by
    // the first corner's distance (rounded up) plus their way apart.
    const Nodes& nodes = nodesOf(brick);
    std::uint64_t words[8];
    bool sampled = true;
    for (int k = 0; k < 8; ++k) {
        words[k] = nodes.words[cell + cornerOffset(k)].load(std::memory_order_acquire);
        sampled = sampled && words[k] < kSampling;
    }
    if (sampled) {
        std::memcpy(n, words, sizeof(n));
    } else {
        float seed = kInf;
        const float slack = 1e-5f * (std::fabs(brick.corner.x) + std::fabs(brick.corner.y) + std::fabs(brick.corner.z) + 1.0f);
        for (int k = 0; k < 8; ++k) {
            n[k] = nodeAt(brick, cell + cornerOffset(k), seed == kInf ? kInf : (seed + kCornerApart[k] * cell_) * 1.001f + slack,
                          k == 0 ? kTableNone : nodes.faces[cell]);  // (the first corner's face, written with its node)
            if (k == 0) seed = fromHalf(n[0].distance);
        }
    }
    for (int k = 0; k < 8; ++k) {
        // A height held at the scale's end is that far or farther: as far as any test here goes.
        h[k] = n[k].height == 32767 ? kInf : n[k].height == -32767 ? -kInf : static_cast<float>(n[k].height) * heightScale_;
        nearestBits = std::min(nearestBits, n[k].distance);
        farthestBits = std::max(farthestBits, n[k].distance);
        lowest = std::min(lowest, h[k]);
    }
    const float nearest = fromHalf(nearestBits), farthest = fromHalf(farthestBits);
    const float cellDiagonal = kSqrt3 * cell_;
    if (nearest > searchRadius + cellDiagonal) {  // every point in the cell beyond the search
        *clear = true;
        return true;
    }
    if (farthest == kInf) return false;  // the reach's edge crosses the cell
    if (farthest > searchRadius - cellDiagonal) {
        // The search's edge may cross the cell: beyond it the mesh leaves points alone. Clear
        // when every point in it is well above the offset (left alone either way), else ask.
        if (lowest >= offset + 2.0f * cellDiagonal) {
            if (probe) probe->heightLow = lowest - 2.0f * cellDiagonal;
            *clear = true;
            return true;
        }
        return false;
    }
    // Deep under the surface (a long push, where the closest point's normal matters most), or
    // near the depth past which points are left alone: the mesh's.
    if (lowest < -band_ || lowest < -maxDepth + 2.0f * cellDiagonal) return false;
    if (!(h[0] < kInf && h[1] < kInf && h[2] < kInf && h[3] < kInf && h[4] < kInf && h[5] < kInf && h[6] < kInf && h[7] < kInf)) {
        // Some corner far above the band: clear when the lowest is above the offset, else ask.
        if (lowest >= offset + 2.0f * cellDiagonal) {
            if (probe) probe->heightLow = lowest - 2.0f * cellDiagonal;
            *clear = true;
            return true;
        }
        return false;
    }
    // Heights changing faster than the distance between the corners allows: the closest point
    // jumped between them (the medial axis, the two sides of a thin part).
    for (int k = 1; k < 8; ++k) {
        if (std::fabs(h[0] - h[k]) > 1.5f * kCornerApart[k] * cell_) return false;
    }
    // Trilinear: along x, then y, then z. (Over or under a faceted surface the height kinks where
    // the closest triangle changes, which the blend rounds off: cellSizeFor.)
    const float fx = f.x, fy = f.y, fz = f.z;
    *height = mix1(mix1(mix1(h[0], h[1], fx), mix1(h[2], h[3], fx), fy), mix1(mix1(h[4], h[5], fx), mix1(h[6], h[7], fx), fy), fz);
    if (probe) probe->heightLow = *height;
    if (*height >= offset) {
        *clear = true;
        return true;
    }
    if (probe && probe->beside > 0.0f) {
        // Beside the surface: the corners' distances (rounded up a little) blended as the heights.
        float d[8];
        for (int k = 0; k < 8; ++k) d[k] = fromHalf(n[k].distance);
        const float distance =
            mix1(mix1(mix1(d[0], d[1], fx), mix1(d[2], d[3], fx), fy), mix1(mix1(d[4], d[5], fx), mix1(d[6], d[7], fx), fy), fz);
        if (probe->besideAt(*height, distance)) {
            *clear = true;
            return true;
        }
    }
    // A push: along the blended normal, unless the corners' normals are apart (a crease, a
    // sliver of triangles: the normal between them is not their blend).
    Vec3f nv[8];
    for (int k = 0; k < 8; ++k) nv[k] = fromOctahedral(n[k].normal);
    const float first = nv[0].dot(nv[0]);
    const float crease2 = probe && probe->relaxed ? kRelaxedCrease2 : kCrease2;
    for (int k = 1; k < 8; ++k) {  // cos(a, b) < kCreaseCosine, without the square roots
        const float d = nv[0].dot(nv[k]);
        if (d <= 0.0f || d * d < crease2 * first * nv[k].dot(nv[k])) return false;
    }
    *normal = mix1(mix1(mix1(nv[0], nv[1], fx), mix1(nv[2], nv[3], fx), fy),
                    mix1(mix1(nv[4], nv[5], fx), mix1(nv[6], nv[7], fx), fy), fz)
                  .normalized();
    return true;
}

bool MeshDistanceField::bounds(const Vec3f& p, float radius, float* heightLow, float* distanceLow) const {
    Located at;
    if (!locate(p, &at)) {
        // Outside the grid (which reaches past the search: a push with Probe::fieldOnly leaves every
        // point outside it alone): beyond the search for every point within the radius, when the
        // grid is farther than that.
        if (!regions_) return false;
        const Vec3f hi = origin_ + Vec3f(static_cast<float>(regionDims_[0]), static_cast<float>(regionDims_[1]),
                                         static_cast<float>(regionDims_[2])) * regionSize_;
        const float gap = std::max({origin_.x - p.x, p.x - hi.x, origin_.y - p.y, p.y - hi.y, origin_.z - p.z, p.z - hi.z});
        if (!(gap > radius + cell_)) return false;
        *heightLow = -kInf;
        *distanceLow = kInf;
        return true;
    }
    float h = at.region->heightLow, d = at.region->distanceLow;
    if (!at.region->far) {
        const Brick& brick = brickOf(at);
        h = brick.heightLow;
        d = brick.distanceLow;
    }
    *heightLow = h - 2.0f * radius;
    *distanceLow = d - radius;
    return true;
}
MeshDistanceField::Push MeshDistanceField::push(const Vec3f& p, float offset, float maxDepth, float searchRadius,
                                                Vec3f* delta, Probe* probe) const {
    if (searchRadius > reach_) return Push::Ask;
    Located at;
    if (!locate(p, &at)) {
        if (!(probe && probe->fieldOnly && regions_)) return Push::Ask;
        probe->learnt(-kInf, searchRadius);  // beyond the search: the grid reaches past it
        return Push::Kept;
    }
    auto settles = [&](float heightLow, float distanceLow) {
        if (probe) probe->learnt(heightLow, std::max(distanceLow, 0.0f));
        return distanceLow > searchRadius || heightLow >= offset;
    };
    // A far region's bounds first (whether or not the thread's memo holds the brick: what the
    // probe learns is the same whichever thread asks).
    if (at.region->far && settles(at.region->heightLow, at.region->distanceLow)) return Push::Kept;
    const Brick& brick = brickOf(at);
    if (!brick.fine) {
        if (settles(brick.heightLow, brick.distanceLow)) return Push::Kept;
        if (!(probe && probe->fieldOnly)) return Push::Ask;
        return pushFar(p, at, brick, offset, maxDepth, searchRadius, delta, *probe);
    }
    // Probing (a strand walked point by point, most points well clear of the surface): a fine
    // brick's bounds first, as a far brick's, which settle a point without sampling its nodes.
    if (probe && settles(brick.heightLow, brick.distanceLow)) return Push::Kept;
    return pushFine(p, at, brick, offset, maxDepth, searchRadius, delta, probe);
}

MeshDistanceField::Push MeshDistanceField::pushFar(const Vec3f& p, const Located& at, const Brick& brick, float offset,
                                                   float maxDepth, float searchRadius, Vec3f* delta, Probe& probe) const {
    std::uint32_t seeds[8];
    farSeeds(at, brick, seeds);
    return answerNear(p, seeds, 8, offset, maxDepth, searchRadius, delta, probe);
}

MeshDistanceField::Push MeshDistanceField::pushFine(const Vec3f& p, const Located& at, const Brick& brick, float offset,
                                                    float maxDepth, float searchRadius, Vec3f* delta, Probe* probe) const {
    const bool fieldOnly = probe && probe->fieldOnly;
    float height = 0.0f;
    Vec3f normal;
    bool clear = false;
    if (probe) probe->forget();
    if (!blend(brick, at.cell, at.f, maxDepth, searchRadius, offset, &height, &normal, &clear, probe)) {
        if (!fieldOnly) return Push::Ask;
        std::uint32_t seeds[8];  // the corners' closest faces (blend sampled every corner)
        const Nodes& nodes = nodesOf(brick);
        for (int k = 0; k < 8; ++k) seeds[k] = nodes.faces[at.cell + cornerOffset(k)];
        return answerNear(p, seeds, 8, offset, maxDepth, searchRadius, delta, *probe);
    }
    if (clear) return Push::Kept;
    *delta = normal * (offset - height);
    return Push::Moved;
}

const MeshDistanceField::Rings& MeshDistanceField::rings() const {
    std::call_once(ringsMade_, [&] {
        auto r = std::make_unique<Rings>();
        const auto& tris = mesh_->triangles();
        r->start.assign(mesh_->pointCount() + 1, 0);
        for (const auto& t : tris) {
            for (const std::int32_t v : t) ++r->start[static_cast<std::size_t>(v) + 1];
        }
        for (std::size_t v = 0; v < mesh_->pointCount(); ++v) r->start[v + 1] += r->start[v];
        r->faces.resize(r->start.back());
        std::vector<std::uint32_t> at(r->start.begin(), r->start.end() - 1);
        for (std::size_t t = 0; t < tris.size(); ++t) {
            for (const std::int32_t v : tris[t]) r->faces[at[static_cast<std::size_t>(v)]++] = static_cast<std::uint32_t>(t);
        }
        const auto& points = mesh_->points();
        // (On this thread: the threads waiting for these may be the pool's.)
        r->bounds.resize(tris.size());
        for (std::size_t t = 0; t < tris.size(); ++t) {
            const auto& tri = tris[t];
            r->bounds[t] = triangleBound(points[static_cast<std::size_t>(tri[0])], points[static_cast<std::size_t>(tri[1])],
                                         points[static_cast<std::size_t>(tri[2])]);
            for (int c = 0; c < 3; ++c) r->bounds[t].corner[c] = tri[c];
        }
        rings_ = std::move(r);
    });
    return *rings_;
}

void MeshDistanceField::farSeeds(const Located& at, const Brick& brick, std::uint32_t* seeds) const {
    // p's side of its brick's middle along each axis (the middle is 2 cells in).
    const int cx = at.cell % kBrickNodes, cy = (at.cell / kBrickNodes) % kBrickNodes, cz = at.cell / (kBrickNodes * kBrickNodes);
    const int step[3] = {cx >= 2 ? 1 : -1, cy >= 2 ? 1 : -1, cz >= 2 ? 1 : -1};
    seeds[0] = brick.face;
    for (int k = 1; k < 8; ++k) {
        const int b[3] = {at.bx + ((k & 1) ? step[0] : 0), at.by + ((k & 2) ? step[1] : 0), at.bz + ((k & 4) ? step[2] : 0)};
        seeds[k] = kTableNone;
        bool inside = true;
        for (int a = 0; a < 3; ++a) inside = inside && b[a] >= 0 && b[a] < regionDims_[a] * kRegionBricks;
        if (!inside) continue;
        const Region& region = regionAt(b[0], b[1], b[2]);
        // Another region far from the surface: none (the tables may hold no bricks of it).
        if (&region != at.region && region.far) continue;
        seeds[k] = brickAt(region, b[0], b[1], b[2]).face;
    }
}

MeshDistanceField::Push MeshDistanceField::answerNear(const Vec3f& p, const std::uint32_t* seeds, int count, float offset,
                                                      float maxDepth, float searchRadius, Vec3f* delta, Probe& probe) const {
    const Rings& r = rings();
    SurfacePoint best;
    std::uint32_t bestFace = kTableNone;
    float bestDist = std::numeric_limits<float>::max();  // squared, as closestPoint weighs them
    // The corners whose rings were scanned (the first kScannedCorners of them): a triangle at one
    // of them was weighed already, and weighing it again changes nothing (the nearest only gets
    // nearer, the first of equals stays), so it is skipped. (The GPU's walk skips them alike,
    // look.slang scanAround: a seed's neighbors and the steps down the mesh share most corners.)
    constexpr int kScannedCorners = 16;
    std::int32_t scanned[kScannedCorners];
    int scannedCount = 0;
    auto wasScanned = [&](std::int32_t v) {
        for (int i = 0; i < scannedCount; ++i) {
            if (scanned[i] == v) return true;
        }
        return false;
    };
    // The nearest so far's distance (not squared), for the triangles' bounds: a triangle whose
    // sphere is farther from p than that cannot be nearer (closestBarycentric's point is in it), and
    // is skipped; past a margin for the rounding of the test (whichever triangles it skips, the
    // answer is the same).
    float nearest = std::sqrt(bestDist);
    constexpr float kBoundMargin = 1.0002f;
    // The triangles around face `around`'s corners, the nearest kept (the first of equals); true
    // when one is nearer than the nearest before.
    auto scan = [&](std::uint32_t around) {
        bool nearer = false;
        for (const std::int32_t v : r.bounds[around].corner) {
            if (wasScanned(v)) continue;
            for (std::uint32_t k = r.start[static_cast<std::size_t>(v)]; k < r.start[static_cast<std::size_t>(v) + 1]; ++k) {
                const TriangleBound& t = r.bounds[r.faces[k]];
                const Vec3f off = p - t.center;
                const float apart = off.dot(off), clear = t.reach + nearest;
                if (apart <= t.within && apart > clear * clear * kBoundMargin) continue;
                if (wasScanned(t.corner[0]) || wasScanned(t.corner[1]) || wasScanned(t.corner[2])) continue;
                const SurfacePoint sp = mesh_->closestPointOnFace(p, r.faces[k]);
                const float d = (sp.position - p).dot(sp.position - p);
                if (d < bestDist) {
                    bestDist = d;
                    nearest = std::sqrt(d);
                    best = sp;
                    bestFace = r.faces[k];
                    nearer = true;
                }
            }
            if (scannedCount < kScannedCorners) scanned[scannedCount++] = v;
        }
        return nearer;
    };
    for (int i = 0; i < count; ++i) {
        const std::uint32_t seed = seeds[i];
        bool again = seed == kTableNone;
        for (int j = 0; j < i && !again; ++j) again = seeds[j] == seed;
        if (again) continue;
        scan(seed);
    }
    // Then down the mesh: from the nearest so far, while a triangle around its corners is nearer
    // (deep under a surface, or near the middle of its curvature, p's closest face may be a few
    // rings from those the field holds nearby). kAnswerSteps at most.
    for (int step = 0; step < kAnswerSteps && bestFace != kTableNone; ++step) {
        if (!scan(bestFace)) break;
    }
    // As pushOutOf answers from the mesh's closest point within the search radius.
    if (best.root.face < 0 || !(bestDist < searchRadius * searchRadius)) {
        probe.learnt(-kInf, searchRadius);
        return Push::Kept;
    }
    best.normal = mesh_->normal(best.root);
    const float height = (p - best.position).dot(best.normal);
    const float distance = (p - best.position).length();
    probe.learnt(height, distance);
    if (height >= offset || height <= -maxDepth) return Push::Kept;
    if (probe.besideAt(height, distance)) return Push::Kept;
    *delta = best.normal * (offset - height);
    return Push::Moved;
}

bool MeshDistanceField::canMove(float offset, float maxDepth) const {
    // blend asks for any cell with a corner under -maxDepth + 2 diagonals; a push needs a height
    // under the offset, so a corner under it.
    return -maxDepth + 2.0f * kSqrt3 * cell_ < offset;
}

bool MeshDistanceField::sample(const Vec3f& p, float* height, Vec3f* normal) const {
    Located at;
    if (!locate(p, &at) || at.region->far) return false;
    const Brick& brick = brickAt(*at.region, at.bx, at.by, at.bz);
    if (!brick.fine) return false;
    bool clear = false;
    // No offset (every height blended) and no depth limit beyond the reach.
    return blend(brick, at.cell, at.f, reach_, reach_, kInf, height, normal, &clear) && !clear;
}

MeshDistanceField::Tables MeshDistanceField::tables(float maxOffset, float searchRadius) const {
    if (!regions_ || searchRadius > reach_) return nullptr;
    {
        const std::lock_guard<std::mutex> lock(tablesMutex_);
        if (tables_ && tablesOffset_ == maxOffset && tablesRadius_ == searchRadius) return tables_;
    }
    const std::size_t dx = static_cast<std::size_t>(regionDims_[0]), dy = static_cast<std::size_t>(regionDims_[1]);
    const std::size_t regionCount = dx * dy * static_cast<std::size_t>(regionDims_[2]);
    auto regionPlace = [&](std::size_t i, int* r) {
        r[0] = static_cast<int>(i % dx);
        r[1] = static_cast<int>((i / dx) % dy);
        r[2] = static_cast<int>(i / (dx * dy));
    };
    // Every region, made; the bricks of those whose bounds do not settle every push the tables
    // answer (an offset up to maxOffset), made; the nodes of their fine bricks whose bounds do not.
    parallelFor(0, regionCount, [&](std::size_t i) {
        int r[3];
        regionPlace(i, r);
        regionAt(r[0] * kRegionBricks, r[1] * kRegionBricks, r[2] * kRegionBricks);
    }, 64);
    auto settledAll = [&](float heightLow, float distanceLow) { return distanceLow > searchRadius || heightLow >= maxOffset; };
    std::vector<std::uint32_t> brickBase(regionCount, kTableNone);
    std::vector<std::size_t> withBricks;
    for (std::size_t i = 0; i < regionCount; ++i) {
        const Region& r = regions_[i];
        if (r.far && settledAll(r.heightLow, r.distanceLow)) continue;
        brickBase[i] = static_cast<std::uint32_t>(withBricks.size() * kBricksPerRegion);
        withBricks.push_back(i);
    }
    std::vector<const Brick*> bricks(withBricks.size() * kBricksPerRegion, nullptr);
    parallelFor(0, bricks.size(), [&](std::size_t b) {
        int r[3];
        regionPlace(withBricks[b / kBricksPerRegion], r);
        const int local = static_cast<int>(b % kBricksPerRegion);
        bricks[b] = &brickAt(regions_[withBricks[b / kBricksPerRegion]], r[0] * kRegionBricks + local % kRegionBricks,
                             r[1] * kRegionBricks + (local / kRegionBricks) % kRegionBricks,
                             r[2] * kRegionBricks + local / (kRegionBricks * kRegionBricks));
    }, 256);
    std::vector<std::uint32_t> nodeBase(bricks.size(), kTableNone);
    std::vector<std::size_t> sampled;
    for (std::size_t b = 0; b < bricks.size(); ++b) {
        if (!bricks[b]->fine || settledAll(bricks[b]->heightLow, bricks[b]->distanceLow)) continue;
        nodeBase[b] = static_cast<std::uint32_t>(sampled.size());
        sampled.push_back(b);
    }
    parallelFor(0, sampled.size(), [&](std::size_t i) {
        const Brick& brick = *bricks[sampled[i]];
        // Each node's query bounded by the first's distance (rounded up) plus their way apart.
        const float seed = fromHalf(nodeAt(brick, 0, kInf).distance);
        const float slack = 1e-5f * (std::fabs(brick.corner.x) + std::fabs(brick.corner.y) + std::fabs(brick.corner.z) + 1.0f);
        for (int k = 1; k < kNodesPerBrick; ++k) {
            const float x = static_cast<float>(k % kBrickNodes), y = static_cast<float>((k / kBrickNodes) % kBrickNodes),
                        z = static_cast<float>(k / (kBrickNodes * kBrickNodes));
            const float apart = std::sqrt(x * x + y * y + z * z) * cell_;
            nodeAt(brick, k, seed == kInf ? kInf : (seed + apart) * 1.001f + slack, nodesOf(brick).faces[0]);
        }
    }, 4);

    const auto& points = mesh_->points();
    const auto& normals = mesh_->vertexNormals();
    const auto& tris = mesh_->triangles();
    const Rings& rings = this->rings();
    std::size_t at = kTableHeaderWords;
    auto part = [&](std::size_t words) {
        const std::size_t from = at;
        at += words;
        return from;
    };
    const std::size_t regionsAt = part(regionCount * kTableRegionWords), bricksAt = part(bricks.size() * kTableBrickWords);
    const std::size_t nodesAt = part(sampled.size() * kNodesPerBrick * 2), facesAt = part(sampled.size() * kNodesPerBrick);
    const std::size_t pointsAt = part(points.size() * 3), normalsAt = part(normals.size() * 3);
    const std::size_t trianglesAt = part(tris.size() * kTableTriangleWords);
    const std::size_t ringStartAt = part(rings.start.size()), ringsAt = part(rings.faces.size());
    if (at > std::numeric_limits<std::uint32_t>::max()) return nullptr;
    auto words = std::make_shared<std::vector<std::uint32_t>>(at, 0u);
    std::uint32_t* w = words->data();
    auto putFloat = [](std::uint32_t* to, float v) { std::memcpy(to, &v, 4); };
    for (int a = 0; a < 3; ++a) {
        w[kTableDims + a] = static_cast<std::uint32_t>(regionDims_[a]);
        w[kTableOriginRegion + a] = static_cast<std::uint32_t>(originRegion_[a]);
    }
    putFloat(w + kTableCell, cell_);
    putFloat(w + kTableBand, band_);
    putFloat(w + kTableHeightScale, heightScale_);
    putFloat(w + kTableReach, reach_);
    putFloat(w + kTableInverseCell, inverseCell_);
    const std::size_t offsets[] = {regionsAt, bricksAt, nodesAt, facesAt, pointsAt, normalsAt, trianglesAt, ringStartAt, ringsAt};
    for (std::size_t k = 0; k < std::size(offsets); ++k) w[kTableRegions + k] = static_cast<std::uint32_t>(offsets[k]);
    for (std::size_t i = 0; i < regionCount; ++i) {
        const Region& r = regions_[i];
        std::uint32_t* to = w + regionsAt + i * kTableRegionWords;
        to[0] = r.far ? 1u : 0u;
        putFloat(to + 1, r.heightLow);
        putFloat(to + 2, r.distanceLow);
        to[3] = brickBase[i];
    }
    parallelFor(0, bricks.size(), [&](std::size_t b) {
        const Brick& brick = *bricks[b];
        std::uint32_t* to = w + bricksAt + b * kTableBrickWords;
        to[0] = brick.face;
        putFloat(to + 1, brick.heightLow);
        putFloat(to + 2, brick.distanceLow);
        to[3] = nodeBase[b];
        if (nodeBase[b] == kTableNone) return;
        const Nodes& nodes = nodesOf(brick);
        for (int k = 0; k < kNodesPerBrick; ++k) {
            const std::uint64_t word = nodes.words[k].load(std::memory_order_acquire);
            std::memcpy(w + nodesAt + (static_cast<std::size_t>(nodeBase[b]) * kNodesPerBrick + k) * 2, &word, 8);
            w[facesAt + static_cast<std::size_t>(nodeBase[b]) * kNodesPerBrick + k] = nodes.faces[k];
        }
    }, 256);
    std::memcpy(w + pointsAt, points.data(), points.size() * 12);
    std::memcpy(w + normalsAt, normals.data(), normals.size() * 12);
    static_assert(sizeof(TriangleBound) == kTableTriangleWords * 4, "a triangle's words are its TriangleBound");
    std::memcpy(w + trianglesAt, rings.bounds.data(), rings.bounds.size() * sizeof(TriangleBound));
    std::copy(rings.start.begin(), rings.start.end(), w + ringStartAt);
    std::copy(rings.faces.begin(), rings.faces.end(), w + ringsAt);

    const std::lock_guard<std::mutex> lock(tablesMutex_);
    tables_ = words;
    tablesOffset_ = maxOffset;
    tablesRadius_ = searchRadius;
    return words;
}

MeshDistanceField::Stats MeshDistanceField::stats() const {
    Stats s;
    s.regions = regionsMade_.load(std::memory_order_relaxed);
    s.farBricks = far_.load(std::memory_order_relaxed);
    s.fineBricks = fine_.load(std::memory_order_relaxed);
    s.nodes = nodes_.load(std::memory_order_relaxed);
    s.queries = queries_.load(std::memory_order_relaxed) + s.nodes;  // (regions' and bricks', and a node's each)
    const std::size_t regions = static_cast<std::size_t>(regionDims_[0]) * regionDims_[1] * regionDims_[2];
    s.bytes = regions * sizeof(Region) + (s.fineBricks + s.farBricks) * sizeof(Brick) +
              nodeBlocks_.load(std::memory_order_relaxed) * sizeof(Nodes);
    return s;
}

DistanceFieldPtr sharedDistanceField(const std::shared_ptr<const ScalpMesh>& mesh, float cellSize, float reach, int bandCells) {
    struct Entry {
        std::uint64_t surface;
        float cell, reach;
        int band;
        DistanceFieldPtr field;
    };
    static std::mutex mutex;
    static std::list<Entry> kept;  // most recently used first
    constexpr std::size_t kKept = 4;
    // A field grows with the queries it answers (its bricks): the ones kept besides the one handed
    // out are let go, oldest first, beyond this many bytes (GROOMLAB_FIELD_CACHE_MB: another).
    static const std::size_t budget = [] {
        std::size_t mb = 768;
        if (const char* text = std::getenv("GROOMLAB_FIELD_CACHE_MB")) mb = static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
        return mb << 20;
    }();
    static const bool registered = [] {
        registerProcessCache(
            "distance fields",
            [] {
                std::list<Entry> drop;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    drop.swap(kept);
                }
            },
            [] {
                const std::lock_guard<std::mutex> lock(mutex);
                std::size_t bytes = 0;
                for (const Entry& e : kept) bytes += e.field->stats().bytes;
                return bytes;
            });
        return true;
    }();
    (void)registered;
    const std::uint64_t surface = mesh->surfaceHash();
    std::list<Entry> dropped;  // (freed outside the lock)
    const std::lock_guard<std::mutex> lock(mutex);
    auto trim = [&] {
        std::size_t bytes = 0;
        for (const Entry& e : kept) bytes += e.field->stats().bytes;
        while (kept.size() > 1 && (kept.size() > kKept || bytes > budget)) {
            bytes -= std::min(bytes, kept.back().field->stats().bytes);
            dropped.splice(dropped.begin(), kept, std::prev(kept.end()));
        }
    };
    for (auto it = kept.begin(); it != kept.end(); ++it) {
        if (it->surface == surface && it->cell == cellSize && it->reach == reach && it->band == bandCells) {
            kept.splice(kept.begin(), kept, it);
            trim();
            return kept.front().field;
        }
    }
    // Cheap to make (its region table alone: the rest comes with the queries).
    auto field = std::make_shared<const MeshDistanceField>(mesh, cellSize, reach, bandCells);
    kept.push_front({surface, cellSize, reach, bandCells, field});
    trim();
    return field;
}

DistanceFieldPtr collisionField(const std::shared_ptr<const ScalpMesh>& mesh, float offset, float searchRadius,
                                const FieldGrid& grid) {
    if (!mesh || mesh->triangleCount() == 0 || (mesh->hasRest() && mesh->restPoints() != mesh->points())) return nullptr;
    const float cell = grid.cell > 0.0f ? grid.cell : MeshDistanceField::cellSizeFor(*mesh, offset);
    return sharedDistanceField(mesh, cell, MeshDistanceField::reachFor(searchRadius), grid.band);
}

bool pushOutOf(const ScalpMesh& mesh, const MeshDistanceField* field, Vec3f& p, float offset, float maxDepth, float searchRadius,
               MeshDistanceField::Probe* probe) {
    if (field) {
        Vec3f delta;
        switch (field->push(p, offset, maxDepth, searchRadius, &delta, probe)) {
            case MeshDistanceField::Push::Kept: return false;
            case MeshDistanceField::Push::Moved: p += delta; return true;
            case MeshDistanceField::Push::Ask: break;
        }
    }
    Vec3f delta;
    if (!meshPushOf(mesh, p, offset, maxDepth, searchRadius, &delta, probe)) return false;
    p += delta;
    return true;
}

bool meshPushOf(const ScalpMesh& mesh, const Vec3f& p, float offset, float maxDepth, float searchRadius, Vec3f* delta,
                MeshDistanceField::Probe* probe) {
    const SurfacePoint sp = mesh.closestPoint(p, searchRadius);
    if (probe) probe->learnt(-std::numeric_limits<float>::infinity(), sp.root.face < 0 ? searchRadius : 0.0f);
    if (sp.root.face < 0) return false;
    const float height = (p - sp.position).dot(sp.normal);
    const float distance = (p - sp.position).length();
    if (probe) probe->learnt(height, distance);
    if (height >= offset || height <= -maxDepth) return false;
    if (probe && probe->besideAt(height, distance)) return false;
    *delta = sp.normal * (offset - height);
    return true;
}

struct RestPoseCollision::Rest {
    MeshPtr mesh;                                // the rest pose
    std::vector<StrandRoot> corner;              // each vertex's frame: at this corner of a face (face -1: on none)
    std::vector<int> edge;                       // along this edge of the face (the vertex's longest at rest)
    std::vector<std::uint32_t> ringStart, ring;  // each vertex's neighbours along the edges
};

struct RestPoseCollision::Frame {
    std::shared_ptr<const Rest> rest;
    std::vector<Vec3f> points;       // the deformed mesh's
    std::vector<RootMotion> toRest;  // each vertex's motion to rest (invalid: on no triangle, or degenerate)
    float longestEdge = 0.0f;        // the deformed mesh's: every point of its surface is this near a vertex
    // The vertices on a triangle (their indices), for points without a cursor; made on first need.
    mutable std::once_flag treeMade;
    mutable PointIndex tree;
    mutable std::vector<std::uint32_t> treeIds;
};

namespace {

// The rest pose of `mesh`, each vertex's frame (follow-the-scalp's: its normal, and its longest
// edge at rest, so a short one never steers it) and the vertices' rings: what every frame of a
// deforming mesh shares.
std::shared_ptr<const RestPoseCollision::Rest> makeRest(const ScalpMesh& mesh) {
    auto rest = std::make_shared<RestPoseCollision::Rest>();
    rest->mesh = std::make_shared<const ScalpMesh>(mesh.restPose());
    const std::size_t n = mesh.pointCount();
    const auto& tris = mesh.triangles();
    const std::vector<Vec3f>& at = rest->mesh->points();
    rest->corner.assign(n, StrandRoot{-1, 0.0f, 0.0f});
    rest->edge.assign(n, 0);
    std::vector<float> longest(n, -1.0f);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(tris.size() * 6);
    auto cornerRoot = [](std::size_t t, int c) {
        return StrandRoot{static_cast<std::int32_t>(t), c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f};
    };
    for (std::size_t t = 0; t < tris.size(); ++t) {
        for (int c = 0; c < 3; ++c) {
            // Edge c of the face (corner c to c + 1): the x of a frame at either of its ends.
            const auto v = static_cast<std::uint32_t>(tris[t][c]);
            const auto w = static_cast<std::uint32_t>(tris[t][(c + 1) % 3]);
            edges.emplace_back(v, w);
            edges.emplace_back(w, v);
            const float length = (at[w] - at[v]).length();
            if (length > longest[v]) {
                longest[v] = length;
                rest->corner[v] = cornerRoot(t, c);
                rest->edge[v] = c;
            }
            if (length > longest[w]) {
                longest[w] = length;
                rest->corner[w] = cornerRoot(t, (c + 1) % 3);
                rest->edge[w] = c;
            }
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    rest->ringStart.assign(n + 1, 0);
    for (const auto& e : edges) ++rest->ringStart[e.first + 1];
    for (std::size_t v = 0; v < n; ++v) rest->ringStart[v + 1] += rest->ringStart[v];
    rest->ring.resize(edges.size());
    for (std::size_t k = 0; k < edges.size(); ++k) rest->ring[k] = edges[k].second;  // sorted: each vertex's in order
    return rest;
}

// One frame of the deforming mesh: its points and each vertex's motion to rest.
std::shared_ptr<const RestPoseCollision::Frame> makeFrame(const ScalpMesh& now, std::shared_ptr<const RestPoseCollision::Rest> rest) {
    auto frame = std::make_shared<RestPoseCollision::Frame>();
    const RestPoseCollision::Rest& r = *rest;
    frame->rest = std::move(rest);
    frame->points = now.points();
    const std::size_t n = now.pointCount();
    frame->toRest.resize(n);
    parallelFor(0, n, [&](std::size_t v) {
        if (r.corner[v].face < 0) return;  // on no triangle: no frame (invalid)
        frame->toRest[v] = {rootFrameOn(now, r.corner[v], r.edge[v]), rootFrameOn(*r.mesh, r.corner[v], r.edge[v])};
    }, 1024);
    for (const auto& t : now.triangles()) {
        for (int c = 0; c < 3; ++c) {
            const float length = (frame->points[static_cast<std::size_t>(t[(c + 1) % 3])] - frame->points[static_cast<std::size_t>(t[c])]).length();
            frame->longestEdge = std::max(frame->longestEdge, length);
        }
    }
    return frame;
}

float distance2(const Vec3f& a, const Vec3f& b) { return (a - b).dot(a - b); }

}  // namespace

std::shared_ptr<const RestPoseCollision> RestPoseCollision::of(const std::shared_ptr<const ScalpMesh>& mesh, float offset,
                                                               float maxDepth, float searchRadius, const FieldGrid& grid) {
    if (!mesh || mesh->triangleCount() == 0 || !mesh->hasRest() || mesh->restPoints() == mesh->points()) return nullptr;
    struct FieldEntry {
        std::uint64_t restSurface;
        float cell, reach;
        int band;
        DistanceFieldPtr field;
    };
    // Kept by rest surface (the rest poses; their fields, also by cell size and reach: held here,
    // so other meshes' fields filling sharedDistanceField's few slots never make one again) and by
    // deformed surface (the frames), the most recently used first.
    static std::mutex mutex;
    static std::list<std::pair<std::uint64_t, std::shared_ptr<const Rest>>> rests;
    static std::list<FieldEntry> fields;
    static std::list<std::pair<std::uint64_t, std::shared_ptr<const Frame>>> frames;
    constexpr std::size_t kKept = 4;
    auto find = [](auto& list, auto&& match) {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (match(*it)) {
                list.splice(list.begin(), list, it);
                return true;
            }
        }
        return false;
    };
    auto keep = [&](auto& list, auto entry) {
        list.push_front(std::move(entry));
        while (list.size() > kKept) list.pop_back();
    };
    const std::uint64_t restSurface = mesh->restSurfaceHash(), surface = mesh->surfaceHash();
    auto out = std::make_shared<RestPoseCollision>();
    std::shared_ptr<const Rest> rest;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (find(frames, [&](const auto& e) { return e.first == surface; })) out->frame_ = frames.front().second;
        if (out->frame_) {
            rest = out->frame_->rest;
        } else if (find(rests, [&](const auto& e) { return e.first == restSurface; })) {
            rest = rests.front().second;
        }
    }
    // Made outside the lock (other evaluations go on meanwhile; two threads may both make one).
    if (!rest) {
        rest = makeRest(*mesh);
        const std::lock_guard<std::mutex> lock(mutex);
        keep(rests, std::make_pair(restSurface, rest));
    }
    const float cell = grid.cell > 0.0f ? grid.cell : MeshDistanceField::cellSizeFor(*rest->mesh, offset);
    const float reach = MeshDistanceField::reachFor(searchRadius);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (find(fields, [&](const FieldEntry& e) {
                return e.restSurface == restSurface && e.cell == cell && e.reach == reach && e.band == grid.band;
            })) {
            out->field_ = fields.front().field;
        } else {
            out->field_ = sharedDistanceField(rest->mesh, cell, reach, grid.band);  // cheap to make: its region table
            keep(fields, FieldEntry{restSurface, cell, reach, grid.band, out->field_});
        }
    }
    if (!out->field_->canMove(offset, maxDepth)) return nullptr;
    if (!out->frame_) {
        out->frame_ = makeFrame(*mesh, rest);
        const std::lock_guard<std::mutex> lock(mutex);
        keep(frames, std::make_pair(surface, out->frame_));
    }
    return out;
}

RestPoseCollision::Cursor RestPoseCollision::cursor(const StrandRoot& root, const Vec3f& rootPoint) const {
    const Frame& f = *frame_;
    const auto& tris = f.rest->mesh->triangles();
    Cursor c;
    if (root.face < 0 || static_cast<std::size_t>(root.face) >= tris.size()) return c;
    float best = std::numeric_limits<float>::infinity();
    for (const std::int32_t v : tris[static_cast<std::size_t>(root.face)]) {
        const float d = distance2(rootPoint, f.points[static_cast<std::size_t>(v)]);
        if (d < best) {
            best = d;
            c.vertex = static_cast<std::size_t>(v);
        }
    }
    // Rooted on this mesh: the root's point on that face (another mesh's strands may name a face
    // of this one by its number alone).
    if (!(std::sqrt(best) <= f.longestEdge * 1.001f + 1e-6f)) c.vertex = Cursor::kNone;
    return c;
}

MeshDistanceField::Push RestPoseCollision::push(const Vec3f& p, float offset, float maxDepth, float searchRadius,
                                                Vec3f* delta, Cursor& cursor, MeshDistanceField::Probe* probe) const {
    if (probe) probe->forget();
    const Frame& f = *frame_;
    if (cursor.clear > 0.0f && (p - cursor.anchor).length() < cursor.clear) return MeshDistanceField::Push::Kept;
    std::size_t v = cursor.vertex;
    if (v != Cursor::kNone) {
        // Down the edges to a vertex none of whose neighbours is nearer p (a strand's next point
        // is a step or two from its last's).
        const Rest& r = *f.rest;
        float best = distance2(p, f.points[v]);
        int steps = 0;
        for (std::size_t from = Cursor::kNone; from != v && steps < 64; ++steps) {
            from = v;
            for (std::uint32_t k = r.ringStart[from]; k < r.ringStart[from + 1]; ++k) {
                const float d = distance2(p, f.points[r.ring[k]]);
                if (d < best) {
                    best = d;
                    v = r.ring[k];
                }
            }
        }
        if (steps == 64) v = Cursor::kNone;  // far to go: the tree's
    }
    if (v == Cursor::kNone) {
        std::call_once(f.treeMade, [&] {
            std::vector<Vec3f> points;
            for (std::size_t k = 0; k < f.points.size(); ++k) {
                if (f.rest->corner[k].face < 0) continue;
                f.treeIds.push_back(static_cast<std::uint32_t>(k));
                points.push_back(f.points[k]);
            }
            f.tree = PointIndex(std::move(points));
        });
        float d2 = 0.0f;
        const std::size_t k = f.tree.nearest(p, &d2);
        if (k >= f.treeIds.size()) return MeshDistanceField::Push::Ask;
        // Every point of the surface is within the longest edge of a vertex: a point that much
        // beyond the search radius from the nearest vertex is beyond it from the surface (the
        // mesh leaves it alone), and so is a point nearer it than the difference, with room for
        // rounding.
        const float distance = std::sqrt(d2);
        const float scale = std::max({std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
        const float clear = distance - f.longestEdge - searchRadius - (1e-4f * (searchRadius + distance + scale) + 1e-6f);
        if (clear > 0.0f) {
            cursor = Cursor{Cursor::kNone, p, clear};
            return MeshDistanceField::Push::Kept;
        }
        v = f.treeIds[k];
    }
    cursor.vertex = v;
    cursor.clear = -1.0f;
    const RootMotion& motion = f.toRest[v];
    if (!motion.from.valid || !motion.to.valid) return MeshDistanceField::Push::Ask;  // a degenerate face's
    Vec3f restDelta;
    const MeshDistanceField::Push push = field_->push(motion.point(p), offset, maxDepth, searchRadius, &restDelta, probe);
    if (push == MeshDistanceField::Push::Moved) *delta = motion.inverse().direction(restDelta);
    return push;
}

std::shared_ptr<const std::vector<std::uint32_t>> RestPoseCollision::walkWords() const {
    const Frame& f = *frame_;
    const Rest& r = *f.rest;
    const std::size_t n = f.points.size();
    std::vector<std::uint32_t> tree;
    for (std::size_t v = 0; v < n; ++v) {
        if (r.corner[v].face >= 0) tree.push_back(static_cast<std::uint32_t>(v));
    }
    auto words = std::make_shared<std::vector<std::uint32_t>>(kWalkHeader, 0u);
    std::vector<std::uint32_t>& w = *words;
    auto bits = [](float x) {
        std::uint32_t b;
        std::memcpy(&b, &x, 4);
        return b;
    };
    auto vec = [&](const Vec3f& v) { w.insert(w.end(), {bits(v.x), bits(v.y), bits(v.z)}); };
    w[kWalkVertices] = static_cast<std::uint32_t>(n);
    w[kWalkPoints] = static_cast<std::uint32_t>(w.size());
    for (const Vec3f& p : f.points) vec(p);
    w[kWalkRingStart] = static_cast<std::uint32_t>(w.size());
    w.insert(w.end(), r.ringStart.begin(), r.ringStart.end());
    w[kWalkRings] = static_cast<std::uint32_t>(w.size());
    w.insert(w.end(), r.ring.begin(), r.ring.end());
    w[kWalkMotions] = static_cast<std::uint32_t>(w.size());
    for (std::size_t v = 0; v < n; ++v) {
        const RootMotion& m = f.toRest[v];
        // (A vertex on no triangle is in no ring nor the tree: never asked.)
        const bool asked = r.corner[v].face >= 0;
        w.push_back(asked && !(m.from.valid && m.to.valid) ? kWalkNoMotion : m.still() ? 1u : 0u);
        for (const RootFrame* frame : {&m.from, &m.to}) {
            vec(frame->origin);
            vec(frame->x);
            vec(frame->y);
            vec(frame->z);
        }
    }
    w[kWalkTree] = static_cast<std::uint32_t>(w.size());
    w[kWalkTreeCount] = static_cast<std::uint32_t>(tree.size());
    w.insert(w.end(), tree.begin(), tree.end());
    w[kWalkLongestEdge] = bits(f.longestEdge);
    return words;
}

CollisionSurface CollisionSurface::of(const std::shared_ptr<const ScalpMesh>& mesh, float offset, float maxDepth, float searchRadius,
                                      const FieldGrid& grid) {
    CollisionSurface s;
    if (!mesh || mesh->triangleCount() == 0) return s;
    s.mesh = mesh;
    s.field = collisionField(mesh, offset, searchRadius, grid);
    if (!s.field) s.rest = RestPoseCollision::of(mesh, offset, maxDepth, searchRadius, grid);
    return s;
}

RestPoseCollision::Cursor CollisionSurface::cursor(const StrandRoot& root, const Vec3f& rootPoint) const {
    return rest ? rest->cursor(root, rootPoint) : RestPoseCollision::Cursor{};
}

bool CollisionSurface::pushOut(Vec3f& p, float offset, float maxDepth, float searchRadius, RestPoseCollision::Cursor& cursor,
                               MeshDistanceField::Probe* probe) const {
    if (rest) {
        Vec3f delta;
        switch (rest->push(p, offset, maxDepth, searchRadius, &delta, cursor, probe)) {
            case MeshDistanceField::Push::Kept: return false;
            case MeshDistanceField::Push::Moved: p += delta; return true;
            case MeshDistanceField::Push::Ask: break;  // the mesh's answer
        }
        return pushOutOf(*mesh, nullptr, p, offset, maxDepth, searchRadius, probe);
    }
    if (field) {  // (pushOutOf's, without the call)
        Vec3f delta;
        switch (field->push(p, offset, maxDepth, searchRadius, &delta, probe)) {
            case MeshDistanceField::Push::Kept: return false;
            case MeshDistanceField::Push::Moved: p += delta; return true;
            case MeshDistanceField::Push::Ask: break;
        }
    }
    return pushOutOf(*mesh, nullptr, p, offset, maxDepth, searchRadius, probe);
}

}  // namespace groomlab
