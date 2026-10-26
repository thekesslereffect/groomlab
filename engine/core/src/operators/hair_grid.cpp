// SPDX-License-Identifier: Apache-2.0
// The hair grid (hair_grid.h): the sparse grid of the core strands' hair that SelfCollide and
// Static share.

#include "hair_grid.h"

#include <cstring>
#include <exception>

#include "groomlab/look/look_cpu.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "internal.h"

namespace groomlab {
namespace hair_grid {

namespace {

// The scalp's floor, as keepClearance's (interpolate.cpp): shares of the scalp's size.
constexpr float kFloor = 1.0f / 2048.0f;
constexpr float kFloorCap = 1.0f / 32.0f;
constexpr float kFloorDepth = 1.0f / 8.0f;
constexpr float kFloorCell = 1.0f / 256.0f;
constexpr int kFloorBandAboveCap = 4;

// The share of a render's strands each core strand stands for: 1 for a set without roles (every
// strand core); else the core share its Scatter took (coreShare: a power of two down to kCoreShare),
// the least of those above every core strand's hashed ID (each under it: previewRole). The same in
// every preview, which holds the same core.
double coreShareOf(const StrandSet& in, const PreviewCore& roles, const std::vector<std::size_t>& core) {
    if (roles.all()) return 1.0;
    float highest = 0.0f;
    for (const std::size_t s : core) highest = std::max(highest, unitFloat(hash64(in.ids()[s])));
    float share = 1.0f;
    while (share * 0.5f > kCoreShare && share * 0.5f > highest) share *= 0.5f;
    if (share * 0.5f <= kCoreShare && kCoreShare > highest && share > kCoreShare) share = kCoreShare;
    return share;
}

}  // namespace

bool finitePoints(const Vec3f* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) || !std::isfinite(p[i].z)) return false;
    }
    return true;
}

float scalpSize(const ScalpMesh* scalp, double scale) {
    if (!scalp || scalp->triangleCount() == 0) return 0.0f;
    const std::vector<Vec3f>& points = scalp->hasRest() ? scalp->restPoints() : scalp->points();
    if (points.empty()) return 0.0f;
    Vec3f lo = points.front(), hi = lo;
    for (const Vec3f& p : points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const float d = (hi - lo).length();
    return d > 0.0f && std::isfinite(d) ? roundToPercentStep(d, scale) : 0.0f;
}

ScalpFloor ScalpFloor::of(const std::shared_ptr<const ScalpMesh>& scalp, float size) {
    ScalpFloor f;
    if (!scalp || scalp->triangleCount() == 0 || !(size > 0.0f)) return f;
    const float cell = pm::exp2(std::round(pm::log2(size * kFloorCell)));
    const float cap = size * kFloorCap;
    f.floor = size * kFloor;
    f.depth = size * kFloorDepth;
    f.searchRadius = std::max(cap, f.depth) * 1.001f + 1e-6f;
    f.beside = 2.0f * cell;
    const int band = static_cast<int>(std::ceil(cap / cell)) + kFloorBandAboveCap;
    f.surface = CollisionSurface::of(scalp, f.floor, f.depth, f.searchRadius, FieldGrid{cell, band});
    return f;
}

bool ScalpFloor::push(Vec3f& p, RestPoseCollision::Cursor& cursor) const {
    MeshDistanceField::Probe probe;
    probe.beside = beside;
    probe.fieldOnly = true;
    return surface.pushOut(p, floor, depth, searchRadius, cursor, &probe);
}

bool layoutOf(const StrandSet& in, const ScalpMesh* scalp, std::size_t most, std::uint64_t thinSalt, Layout* out, double scale) {
    Layout& l = *out;
    // The strands splatted: the core, at most about `most` of it (a share by ID).
    const PreviewCore roles(in);
    const std::vector<std::size_t> core = coreStrands(in);
    const double share = coreShareOf(in, roles, core);
    double thin = 1.0;
    while (static_cast<double>(core.size()) * thin > static_cast<double>(most)) thin *= 0.5;
    l.strands.clear();
    l.strands.reserve(core.size());
    for (const std::size_t s : core) {
        if (in.strandPointCount(s) < 2 || !finitePoints(in.strandPoints(s), in.strandPointCount(s))) continue;
        if (thin < 1.0 && !(unitFloat(hash64(in.ids()[s] ^ thinSalt)) < static_cast<float>(thin))) continue;
        l.strands.push_back(s);
    }
    if (l.strands.empty()) return false;
    l.weight = 1.0 / (share * thin);  // the strands of a render each splatted one stands for

    // The cell, and the cross-section of a strand without widths.
    l.size = scalpSize(scalp, scale);
    l.area = scalp && l.size > 0.0f ? static_cast<double>(scalp->restTotalArea()) : 0.0;
    if (!std::isfinite(l.area)) l.area = 0.0;
    l.widths = false;
    for (const std::size_t s : l.strands) {
        const std::size_t b = in.strandBegin(s), n = in.strandPointCount(s);
        for (std::size_t i = 0; i < n && !l.widths; ++i) l.widths = in.widths()[b + i] != 1.0f;
        if (l.widths) break;
    }
    double h = 0.0, nominal = 0.0;  // nominal: one strand's cross-section without widths
    l.extent = 0.0;
    if (l.area > 0.0) {
        const double spacing = std::sqrt(l.area / static_cast<double>(l.strands.size()));
        h = std::clamp(kCellSpacings * spacing, l.size * kFinestCell, l.size * kCoarsestCell);
        nominal = kNominalCover * l.area / (static_cast<double>(l.strands.size()) * l.weight);
    } else {
        Vec3f lo = in.strandPoints(l.strands[0])[0], hi = lo;
        for (const std::size_t s : l.strands) {
            const Vec3f* p = in.strandPoints(s);
            for (std::size_t i = 0; i < in.strandPointCount(s); ++i) {
                lo = {std::min(lo.x, p[i].x), std::min(lo.y, p[i].y), std::min(lo.z, p[i].z)};
                hi = {std::max(hi.x, p[i].x), std::max(hi.y, p[i].y), std::max(hi.z, p[i].z)};
            }
        }
        const float d = (hi - lo).length();
        if (!(d > 0.0f) || !std::isfinite(d)) return false;
        const double extent = roundToPercentStep(d, scale);
        l.extent = extent;
        h = extent * kCellWithoutScalp;
        const double w = extent * kNominalWidth;
        nominal = 0.25 * kPi * w * w;
    }
    h = roundToPercentStep(static_cast<float>(h), scale);
    if (!(h > 0.0) || !std::isfinite(h) || !(nominal >= 0.0) || !std::isfinite(nominal)) return false;
    l.h = h;
    l.nominal = nominal;
    return true;
}

bool layGrid(const std::vector<Sample>& samples, std::int64_t margin, int dilate, Grid* grid) {
    Grid& g = *grid;
    g.blockOf.clear();
    g.blockAt.clear();
    if (samples.empty()) return false;
    {
        // The samples' bounds, a piece of them at a time.
        constexpr std::size_t kPiece = 16384;
        const std::size_t pieces = (samples.size() + kPiece - 1) / kPiece;
        std::vector<std::array<Vec3f, 2>> bounds(pieces);
        parallelFor(0, pieces, [&](std::size_t k) {
            Vec3f a = samples[k * kPiece].p, b = a;
            for (std::size_t i = k * kPiece; i < std::min(samples.size(), (k + 1) * kPiece); ++i) {
                const Vec3f& q = samples[i].p;
                a = {std::min(a.x, q.x), std::min(a.y, q.y), std::min(a.z, q.z)};
                b = {std::max(b.x, q.x), std::max(b.y, q.y), std::max(b.z, q.z)};
            }
            bounds[k] = {a, b};
        }, 1);
        Vec3f lo = bounds[0][0], hi = bounds[0][1];
        for (const auto& b : bounds) {
            lo = {std::min(lo.x, b[0].x), std::min(lo.y, b[0].y), std::min(lo.z, b[0].z)};
            hi = {std::max(hi.x, b[1].x), std::max(hi.y, b[1].y), std::max(hi.z, b[1].z)};
        }
        if (!std::isfinite(lo.x + lo.y + lo.z + hi.x + hi.y + hi.z)) return false;
        for (;;) {
            g.ox = static_cast<std::int64_t>(std::floor(lo.x / g.h)) - margin;
            g.oy = static_cast<std::int64_t>(std::floor(lo.y / g.h)) - margin;
            g.oz = static_cast<std::int64_t>(std::floor(lo.z / g.h)) - margin;
            g.nx = static_cast<std::int64_t>(std::floor(hi.x / g.h)) + 2 + margin - g.ox;
            g.ny = static_cast<std::int64_t>(std::floor(hi.y / g.h)) + 2 + margin - g.oy;
            g.nz = static_cast<std::int64_t>(std::floor(hi.z / g.h)) + 2 + margin - g.oz;
            g.bx = (g.nx + kB - 1) / kB;
            g.by = (g.ny + kB - 1) / kB;
            g.bz = (g.nz + kB - 1) / kB;
            if (g.bx * g.by * g.bz <= kMostBlocks) break;
            g.h *= 2.0;  // hair spread far wider than the scalp: coarser cells
        }
    }
    const auto blockCells = static_cast<std::size_t>(g.bx * g.by * g.bz);
    std::unique_ptr<std::atomic<std::uint8_t>[]> marks(new std::atomic<std::uint8_t>[blockCells]());
    parallelFor(0, samples.size(), [&](std::size_t k) {
        std::int64_t c[3];
        double f[3];
        g.cellOf(samples[k].p, c, f);
        // The blocks of its cell's corners (one, unless the cell is at a block's far side).
        for (std::int64_t l = c[2]; l <= c[2] + 1; l += (c[2] & 3) == 3 ? 1 : 2) {
            for (std::int64_t j = c[1]; j <= c[1] + 1; j += (c[1] & 3) == 3 ? 1 : 2) {
                for (std::int64_t i = c[0]; i <= c[0] + 1; i += (c[0] & 3) == 3 ? 1 : 2) {
                    std::atomic<std::uint8_t>& mark = marks[static_cast<std::size_t>(g.blockCell(i, j, l))];
                    if (!mark.load(std::memory_order_relaxed)) mark.store(1, std::memory_order_relaxed);
                }
            }
        }
    }, 4096);
    for (int ring = 0; ring < dilate; ++ring) {
        // The blocks around those marked (each ring a mark of its own, so a ring grows by one).
        const auto mark = static_cast<std::uint8_t>(ring + 1);
        for (std::size_t b = 0; b < blockCells; ++b) {
            if (marks[b].load(std::memory_order_relaxed) != mark) continue;
            const auto cell = static_cast<std::int64_t>(b);
            const std::int64_t i = cell % g.bx, j = (cell / g.bx) % g.by, k = cell / (g.bx * g.by);
            for (std::int64_t z = std::max<std::int64_t>(k - 1, 0); z <= std::min(k + 1, g.bz - 1); ++z) {
                for (std::int64_t y = std::max<std::int64_t>(j - 1, 0); y <= std::min(j + 1, g.by - 1); ++y) {
                    for (std::int64_t x = std::max<std::int64_t>(i - 1, 0); x <= std::min(i + 1, g.bx - 1); ++x) {
                        std::atomic<std::uint8_t>& m = marks[static_cast<std::size_t>((z * g.by + y) * g.bx + x)];
                        if (!m.load(std::memory_order_relaxed)) m.store(static_cast<std::uint8_t>(mark + 1), std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    g.blockOf.assign(blockCells, -1);
    for (std::size_t b = 0; b < blockCells; ++b) {
        if (!marks[b].load(std::memory_order_relaxed)) continue;
        g.blockOf[b] = static_cast<std::int32_t>(g.blockAt.size());
        const auto cell = static_cast<std::int64_t>(b);
        g.blockAt.push_back({static_cast<std::int32_t>(cell % g.bx), static_cast<std::int32_t>((cell / g.bx) % g.by),
                             static_cast<std::int32_t>(cell / (g.bx * g.by))});
    }
    return true;
}

Splat splat(const Grid& g, const std::vector<Sample>& samples, int sums) {
    Splat s;
    s.count = std::clamp(sums, 0, static_cast<int>(kSums));
    const std::size_t nodes = g.nodes();
    s.mass.reset(new std::atomic<std::int64_t>[nodes]());
    if (s.count > 0) s.sums.reset(new std::atomic<std::int64_t>[nodes * static_cast<std::size_t>(s.count)]());
    const double perVolume = kFixed / (g.h * g.h * g.h);
    const int count = s.count;
    parallelFor(0, samples.size(), [&](std::size_t k) {
        const Sample& q = samples[k];
        std::int64_t c[3];
        double f[3];
        g.cellOf(q.p, c, f);
        const double v = static_cast<double>(q.volume) * perVolume, wide = static_cast<double>(q.width) / g.h;
        for (int d = 0; d < 8; ++d) {
            const int dx = d & 1, dy = (d >> 1) & 1, dz = (d >> 2) & 1;
            const double m = (dx ? f[0] : 1.0 - f[0]) * (dy ? f[1] : 1.0 - f[1]) * (dz ? f[2] : 1.0 - f[2]) * v;
            const auto add = std::llround(m);
            if (add > 0) s.mass[static_cast<std::size_t>(g.node(c[0] + dx, c[1] + dy, c[2] + dz))].fetch_add(add, std::memory_order_relaxed);
        }
        if (count == 0) return;
        std::atomic<std::int64_t>* at = s.sums.get() + static_cast<std::size_t>(g.node(c[0], c[1], c[2])) * static_cast<std::size_t>(count);
        const double add[kSums] = {v, v * f[0], v * f[1], v * f[2], v * f[0] * f[0], v * f[1] * f[1], v * f[2] * f[2],
                                   v * f[0] * f[1], v * f[0] * f[2], v * f[1] * f[2], v * wide};
        for (int e = 0; e < count; ++e) at[e].fetch_add(std::llround(add[e]), std::memory_order_relaxed);
    }, 4096);
    return s;
}

std::vector<Wall> wallsOf(const Grid& g, const ScalpMesh* scalp, float size, double reachCells) {
    std::vector<Wall> walls(scalp && size > 0.0f ? g.nodes() : 0);
    if (walls.empty()) return walls;
    const auto reach = static_cast<float>(reachCells * g.h);
    parallelFor(0, g.blockAt.size(), [&](std::size_t b) {
        for (std::size_t at = b * kBlockNodes; at < (b + 1) * kBlockNodes; ++at) {
            std::int64_t c[3];
            g.coordinates(at, c);
            const Vec3f node = g.position(c);
            float bound = 0.0f;
            const SurfacePoint sp = scalp->closestPointBounded(node, reach, &bound);
            if (sp.root.face < 0) continue;
            walls[at] = {(node - sp.position).dot(sp.normal), sp.normal, true};
        }
    }, 4);
    return walls;
}

void solve(const std::vector<Unknown>& unknown, const std::vector<std::uint8_t>& color, int sweeps, double omega, std::vector<float>* field) {
    std::vector<float>& p = *field;
    std::array<std::vector<std::uint32_t>, 2> byColor;
    for (std::size_t u = 0; u < unknown.size(); ++u) byColor[color[u]].push_back(static_cast<std::uint32_t>(u));
    for (int sweep = 0; sweep < sweeps; ++sweep) {
        for (const std::vector<std::uint32_t>& list : byColor) {
            parallelFor(0, list.size(), [&](std::size_t q) {
                const Unknown& u = unknown[list[q]];
                float& here = p[static_cast<std::size_t>(u.at)];
                double around = 0.0;
                for (const std::int64_t o : u.around) {
                    around += o >= 0 ? static_cast<double>(p[static_cast<std::size_t>(o)]) : o == -2 ? static_cast<double>(here) : 0.0;
                }
                here = static_cast<float>(here + omega * ((around + u.source) / 6.0 - here));
            }, 1024);
        }
    }
}

bool mayTouchScalp(const Grid& g, const std::vector<Wall>& walls, double reach, float margin, const Vec3f& p) {
    std::int64_t c[3];
    double f[3];
    g.cellOf(p, c, f);
    float height = 0.0f;
    for (int d = 0; d < 8; ++d) {
        const int dx = d & 1, dy = (d >> 1) & 1, dz = (d >> 2) & 1;
        const std::int64_t o = g.node(c[0] + dx, c[1] + dy, c[2] + dz);
        if (o < 0) return true;  // (beyond the hair: never asked about the scalp)
        const Wall& wall = walls[static_cast<std::size_t>(o)];
        const auto w = static_cast<float>((dx ? f[0] : 1.0 - f[0]) * (dy ? f[1] : 1.0 - f[1]) * (dz ? f[2] : 1.0 - f[2]));
        if (!wall.near) {
            height += w * static_cast<float>(reach * g.h);  // farther than that
            continue;
        }
        std::int64_t at[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
        height += w * (wall.height + wall.normal.dot(p - g.position(at)));
    }
    return height < margin;
}

std::shared_ptr<const std::vector<std::uint32_t>> EngineWalls::of(const MeshPtr& scalp, double h, double reach) {
    const std::lock_guard<std::mutex> guard(lock_);
    if (!walls_ || scalp_ != scalp || h_ != h || reach_ != reach) {
        walls_ = hairGridWalls(*scalp, h, reach);
        scalp_ = scalp;
        h_ = h;
        reach_ = reach;
    }
    return walls_;
}

StrandsPtr onEngine(gpu::StageKind kind, const std::string& type, const ParamSet& params, const EvalContext& ctx, const StrandsPtr& in,
                    const MeshPtr& scalp, EngineWalls& walls) {
    try {
        auto layout = std::make_shared<HairGridLayout>();
        std::string why;
        if (!hairGridLayout(type, *in, scalp, layout.get(), &why, ctx.scale) || layout->coarsened) return nullptr;
        auto grid = std::make_shared<gpu::HairGridInputs>();
        if (scalp && !(grid->walls = walls.of(scalp, layout->h, layout->wallReach))) return nullptr;
        grid->layout = std::move(layout);
        gpu::StageInputs stage;
        stage.grid = std::move(grid);
        const StrandsPtr made = gpu::runStage(kind, params, in, scalp.get(), std::move(stage));
        if (!made) return nullptr;
        return std::memcmp(made->positions().data(), in->positions().data(), in->pointCount() * sizeof(Vec3f)) == 0 ? in : made;
    } catch (const std::exception&) {
        return nullptr;  // (a grid the engine cannot hold)
    }
}

}  // namespace hair_grid

bool hairGridLayout(const std::string& type, const StrandSet& in, const std::shared_ptr<const ScalpMesh>& scalp, HairGridLayout* out,
                    std::string* why, double scale) {
    using namespace hair_grid;
    const bool isStatic = type == op::kStatic;
    if (!isStatic && type != op::kSelfCollide) {
        *why = type + " lays no hair grid";
        return false;
    }
    const GridSpec& spec = isStatic ? kStaticGrid : kSelfCollideGrid;
    Layout layout;
    if (!layoutOf(in, scalp.get(), spec.most, spec.thinSalt, &layout, scale)) {
        *why = "nothing to splat";
        return false;
    }
    HairGridLayout l;
    l.strands = layout.strands.size();
    l.h = layout.h;
    l.wallReach = spec.wallReach;
    l.margin = spec.margin;
    l.dilate = spec.dilate;
    l.weight = layout.weight;
    l.nominal = layout.nominal;
    l.widths = layout.widths;
    if (isStatic) {
        const double area = layout.area > 0.0 ? layout.area : kStaticAreaWithoutScalp * layout.extent * layout.extent;
        l.section = kNominalCover * area / (static_cast<double>(layout.strands.size()) * layout.weight);
        if (!(l.section > 0.0) || !std::isfinite(l.section)) {
            *why = "no charge to splat";
            return false;
        }
    }
    for (const std::size_t s : layout.strands) {
        const std::size_t b = in.strandBegin(s), n = in.strandPointCount(s);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            l.segments.push_back(static_cast<std::uint32_t>(b + i));
            l.segments.push_back(static_cast<std::uint32_t>(s));
        }
    }
    // The grid the node lays on these strands (their samples as it takes them: the amounts do not
    // lay it).
    const std::vector<Sample> samples =
        sampleStrands(in, in.positions(), layout.strands, 0.5 * layout.h,
                      [](std::size_t, std::size_t, float, float, std::size_t) { return std::pair<float, float>(0.0f, 0.0f); });
    Grid g;
    g.h = layout.h;
    if (!layGrid(samples, spec.margin, spec.dilate, &g)) {
        *why = "its strands are not finite";
        return false;
    }
    l.nodes[0] = g.nx;
    l.nodes[1] = g.ny;
    l.nodes[2] = g.nz;
    l.blockCells[0] = g.bx;
    l.blockCells[1] = g.by;
    l.blockCells[2] = g.bz;
    l.blocks = g.blockAt.size();
    l.coarsened = g.h != layout.h;
    if (scalp && layout.size > 0.0f) {
        const ScalpFloor floor = ScalpFloor::of(scalp, layout.size);
        if (floor && !floor.surface.field) {
            *why = "its scalp is deformed away from its rest pose (the walk asks the rest pose's field)";
            return false;
        }
        if (floor) {
            l.floor = floor.floor;
            l.depth = floor.depth;
            l.searchRadius = floor.searchRadius;
            l.beside = floor.beside;
            l.tables = floor.surface.field->tables(floor.floor, floor.searchRadius);
            if (!l.tables) {
                *why = "its scalp is too big for the floor's field";
                return false;
            }
        }
    }
    *out = std::move(l);
    return true;
}

std::shared_ptr<const std::vector<std::uint32_t>> hairGridWalls(const ScalpMesh& scalp, double h, double reachCells) {
    if (scalp.triangleCount() == 0 || scalp.points().empty() || !(h > 0.0)) return nullptr;
    const auto reach = static_cast<float>(reachCells * h);  // (as wallsOf asks)
    // Every node within reach of a triangle: the scalp's box grown by the reach, a node more.
    Vec3f lo = scalp.points().front(), hi = lo;
    for (const Vec3f& p : scalp.points()) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    if (!std::isfinite(lo.x + lo.y + lo.z + hi.x + hi.y + hi.z)) return nullptr;
    const double r = static_cast<double>(reach);
    std::int64_t origin[3], dims[3];
    const double l[3] = {lo.x, lo.y, lo.z}, u[3] = {hi.x, hi.y, hi.z};
    std::int64_t count = 1;
    for (int a = 0; a < 3; ++a) {
        origin[a] = static_cast<std::int64_t>(std::floor((l[a] - r) / h)) - 1;
        dims[a] = static_cast<std::int64_t>(std::ceil((u[a] + r) / h)) + 2 - origin[a];
        count *= dims[a];
    }
    if (count <= 0 || count > (std::int64_t{1} << 24)) return nullptr;  // (a word a node: 64 MB at most)
    // The candidates: the nodes in each triangle's box grown by the reach.
    std::unique_ptr<std::atomic<std::uint8_t>[]> near(new std::atomic<std::uint8_t>[static_cast<std::size_t>(count)]());
    parallelFor(0, scalp.triangleCount(), [&](std::size_t t) {
        const auto& tri = scalp.triangles()[t];
        Vec3f a = scalp.points()[static_cast<std::size_t>(tri[0])], b = a;
        for (int c = 1; c < 3; ++c) {
            const Vec3f& p = scalp.points()[static_cast<std::size_t>(tri[c])];
            a = {std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z)};
            b = {std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z)};
        }
        const double ta[3] = {a.x, a.y, a.z}, tb[3] = {b.x, b.y, b.z};
        std::int64_t from[3], to[3];
        for (int k = 0; k < 3; ++k) {
            from[k] = std::max<std::int64_t>(static_cast<std::int64_t>(std::floor((ta[k] - r) / h)) - 1 - origin[k], 0);
            to[k] = std::min<std::int64_t>(static_cast<std::int64_t>(std::ceil((tb[k] + r) / h)) + 1 - origin[k], dims[k] - 1);
        }
        for (std::int64_t z = from[2]; z <= to[2]; ++z) {
            for (std::int64_t y = from[1]; y <= to[1]; ++y) {
                for (std::int64_t x = from[0]; x <= to[0]; ++x) near[static_cast<std::size_t>((z * dims[1] + y) * dims[0] + x)].store(1, std::memory_order_relaxed);
            }
        }
    }, 64);
    // Each candidate's wall, as wallsOf finds it, a slice of the box at a time (kept in order).
    const auto slice = static_cast<std::size_t>(dims[0] * dims[1]);
    std::vector<std::vector<std::pair<std::size_t, std::array<float, 4>>>> found(static_cast<std::size_t>(dims[2]));
    parallelFor(0, found.size(), [&](std::size_t z) {
        for (std::size_t at = z * slice; at < (z + 1) * slice; ++at) {
            if (!near[at].load(std::memory_order_relaxed)) continue;
            const auto cell = static_cast<std::int64_t>(at);
            const std::int64_t c[3] = {cell % dims[0] + origin[0], (cell / dims[0]) % dims[1] + origin[1], cell / (dims[0] * dims[1]) + origin[2]};
            const Vec3f node(static_cast<float>(static_cast<double>(c[0]) * h), static_cast<float>(static_cast<double>(c[1]) * h),
                             static_cast<float>(static_cast<double>(c[2]) * h));
            float bound = 0.0f;
            const SurfacePoint sp = scalp.closestPointBounded(node, reach, &bound);
            if (sp.root.face < 0) continue;
            found[z].push_back({at, {(node - sp.position).dot(sp.normal), sp.normal.x, sp.normal.y, sp.normal.z}});
        }
    }, 1);
    auto words = std::make_shared<std::vector<std::uint32_t>>(8 + static_cast<std::size_t>(count), 0xFFFFFFFFu);
    std::vector<std::uint32_t>& w = *words;
    for (int a = 0; a < 3; ++a) {
        w[static_cast<std::size_t>(a)] = static_cast<std::uint32_t>(static_cast<std::int32_t>(origin[a]));
        w[static_cast<std::size_t>(3 + a)] = static_cast<std::uint32_t>(dims[a]);
    }
    std::uint32_t walls = 0;
    for (const auto& z : found) {
        for (const auto& [at, wall] : z) {
            w[8 + at] = walls++;
            for (const float f : wall) {
                std::uint32_t bits;
                std::memcpy(&bits, &f, 4);
                w.push_back(bits);
            }
        }
    }
    w[6] = walls;
    w[7] = 0;
    return words;
}

}  // namespace groomlab
