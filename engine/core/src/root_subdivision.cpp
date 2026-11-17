// SPDX-License-Identifier: Apache-2.0
// Subdividing a root tube into Voronoi cells (root_subdivision.h, issue 85).

#include "groomlab/root_subdivision.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "groomlab/random.h"
#include "groomlab/root_tubes.h"
#include "groomlab/tube.h"

namespace groomlab {

namespace {

constexpr double kDensityChanged = 0.02;  // a sample's density moved more than this: the map changed there
constexpr std::size_t kMaxCells = 400;    // a root cut into at most this many cells (the sizes grown to fit)
constexpr std::size_t kMaxGrid = 40000;   // the candidate grid's and the density grid's points, at most
constexpr int kBlendNearest = 3;          // a new guide's shape blended from this many old guides near it

using P = CutPoint;
P operator+(const P& a, const P& b) { return {a.x + b.x, a.y + b.y}; }
P operator-(const P& a, const P& b) { return {a.x - b.x, a.y - b.y}; }
P operator*(const P& a, double s) { return {a.x * s, a.y * s}; }
double dot(const P& a, const P& b) { return a.x * b.x + a.y * b.y; }
double dist(const P& a, const P& b) { return std::hypot(a.x - b.x, a.y - b.y); }

double signedArea(const std::vector<P>& poly) {
    double a = 0.0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const P& p = poly[i];
        const P& q = poly[(i + 1) % poly.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

P areaCentroid(const std::vector<P>& poly) {
    double a = 0.0, cx = 0.0, cy = 0.0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const P& p = poly[i];
        const P& q = poly[(i + 1) % poly.size()];
        const double c = p.x * q.y - q.x * p.y;
        a += c;
        cx += (p.x + q.x) * c;
        cy += (p.y + q.y) * c;
    }
    if (std::abs(a) < 1e-30) {
        P m;
        for (const P& p : poly) m = m + p;
        return poly.empty() ? m : m * (1.0 / static_cast<double>(poly.size()));
    }
    return {cx / (3.0 * a), cy / (3.0 * a)};
}

bool inside(const std::vector<P>& poly, const P& p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const P& a = poly[i];
        const P& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// Sutherland-Hodgman: `poly` clipped to the side of the line through `m` with dot(p - m, n) <= 0.
std::vector<P> clipHalf(const std::vector<P>& poly, const P& m, const P& n) {
    std::vector<P> out;
    const std::size_t count = poly.size();
    for (std::size_t i = 0; i < count; ++i) {
        const P& a = poly[i];
        const P& b = poly[(i + 1) % count];
        const double da = dot(a - m, n);
        const double db = dot(b - m, n);
        if (da <= 0.0) out.push_back(a);
        if ((da < 0.0 && db > 0.0) || (da > 0.0 && db < 0.0)) {
            const double t = da / (da - db);
            out.push_back(a + (b - a) * t);
        }
    }
    return out;
}

// Consecutive points closer than eps merged, and zero-area spikes taken out.
std::vector<P> tidy(std::vector<P> poly, double eps) {
    bool changed = true;
    while (changed && poly.size() >= 3) {
        changed = false;
        std::vector<P> out;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            if (!out.empty() && dist(out.back(), poly[i]) <= eps) {
                changed = true;
                continue;
            }
            out.push_back(poly[i]);
        }
        while (out.size() >= 2 && dist(out.front(), out.back()) <= eps) {
            out.pop_back();
            changed = true;
        }
        // A point whose two edges fold back on each other (the clip's bridge along a cut).
        for (std::size_t i = 0; out.size() >= 3 && i < out.size(); ++i) {
            const P& a = out[(i + out.size() - 1) % out.size()];
            const P& b = out[i];
            const P& c = out[(i + 1) % out.size()];
            const P ab = b - a, bc = c - b;
            const double cross = ab.x * bc.y - ab.y * bc.x;
            if (std::abs(cross) <= eps * (std::hypot(ab.x, ab.y) + std::hypot(bc.x, bc.y)) && dot(ab, bc) < 0.0) {
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                break;
            }
        }
        poly = std::move(out);
    }
    if (poly.size() < 3) poly.clear();
    return poly;
}

double polygonSize(const std::vector<P>& poly) {
    double lo[2] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    double hi[2] = {-lo[0], -lo[1]};
    for (const P& p : poly) {
        lo[0] = std::min(lo[0], p.x), lo[1] = std::min(lo[1], p.y);
        hi[0] = std::max(hi[0], p.x), hi[1] = std::max(hi[1], p.y);
    }
    return poly.empty() ? 0.0 : std::max(hi[0] - lo[0], hi[1] - lo[1]);
}

std::vector<P> ccw(std::vector<P> poly) {
    if (signedArea(poly) < 0.0) std::reverse(poly.begin(), poly.end());
    return poly;
}

// ---- The plane a root is cut in ------------------------------------------------------------------

struct Plane {
    Vec3f c, u, v, n;
    P to(const Vec3f& p) const {
        const Vec3f d = p - c;
        return {d.dot(u), d.dot(v)};
    }
    Vec3f from(const P& q) const { return c + u * static_cast<float>(q.x) + v * static_cast<float>(q.y); }
};

Plane planeOf(const ScalpMesh& scalp, const std::vector<Vec3f>& outline) {
    const ScalpPointNormal at = scalpPointNormal(scalp, outlineCentroid(outline));
    Plane pl;
    pl.c = at.point;
    pl.n = at.normal.normalized();
    const Vec3f side = std::abs(pl.n.x) < 0.9f ? Vec3f{1, 0, 0} : Vec3f{0, 1, 0};
    pl.u = pl.n.cross(side).normalized();
    pl.v = pl.n.cross(pl.u).normalized();
    return pl;
}

// A point of the plane taken onto the scalp along the plane's normal (the nearer hit either way),
// else the scalp's nearest point.
SurfacePoint lift(const ScalpMesh& scalp, const Plane& pl, const P& q) {
    const Vec3f p = pl.from(q);
    const SurfacePoint near = scalp.closestPoint(p);
    float best = std::numeric_limits<float>::infinity();
    SurfacePoint out = near;
    for (const Vec3f& way : {pl.n, pl.n * -1.0f}) {
        SurfacePoint hit;
        float t = 0.0f;
        if (scalp.intersectRay(p, way, &hit, &t) && std::abs(t) < best) {
            best = std::abs(t);
            out = hit;
        }
    }
    if (std::isinf(best) || best > 4.0f * (near.position - p).length() + 1e-6f) return near;
    return out;
}

// ---- Density ----------------------------------------------------------------------------------------

// The density on a regular grid over the outline's box (bilinear between its points): the cut's
// size(p), and the samples a subdivision remembers (the grid's points inside the outline).
struct DensityGrid {
    P lo;
    double step = 1.0;
    int nx = 0, ny = 0;
    std::vector<float> d;
    float at(const P& p) const {
        if (d.empty()) return kDefaultSubdivisionDensity;
        const double fx = std::clamp((p.x - lo.x) / step, 0.0, static_cast<double>(nx - 1));
        const double fy = std::clamp((p.y - lo.y) / step, 0.0, static_cast<double>(ny - 1));
        const int x0 = std::min(static_cast<int>(fx), nx - 1), y0 = std::min(static_cast<int>(fy), ny - 1);
        const int x1 = std::min(x0 + 1, nx - 1), y1 = std::min(y0 + 1, ny - 1);
        const double tx = fx - x0, ty = fy - y0;
        auto g = [&](int x, int y) { return static_cast<double>(d[static_cast<std::size_t>(y) * nx + x]); };
        return static_cast<float>((g(x0, y0) * (1 - tx) + g(x1, y0) * tx) * (1 - ty) + (g(x0, y1) * (1 - tx) + g(x1, y1) * tx) * ty);
    }
    P point(int x, int y) const { return {lo.x + step * x, lo.y + step * y}; }
};

DensityGrid densityGrid(const ScalpMesh& scalp, const Plane& pl, const std::vector<P>& outline, double step,
                        const std::string& map) {
    DensityGrid g;
    double lo[2] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    double hi[2] = {-lo[0], -lo[1]};
    for (const P& p : outline) {
        lo[0] = std::min(lo[0], p.x), lo[1] = std::min(lo[1], p.y);
        hi[0] = std::max(hi[0], p.x), hi[1] = std::max(hi[1], p.y);
    }
    const double span = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    step = std::max(step, span / std::sqrt(static_cast<double>(kMaxGrid)));
    g.step = std::max(step, 1e-9);
    g.lo = {lo[0], lo[1]};
    g.nx = static_cast<int>(std::ceil((hi[0] - lo[0]) / g.step)) + 1;
    g.ny = static_cast<int>(std::ceil((hi[1] - lo[1]) / g.step)) + 1;
    g.d.resize(static_cast<std::size_t>(g.nx) * g.ny, kDefaultSubdivisionDensity);
    if (!scalp.hasColorSet(map)) return g;
    for (int y = 0; y < g.ny; ++y) {
        for (int x = 0; x < g.nx; ++x) {
            g.d[static_cast<std::size_t>(y) * g.nx + x] = subdivisionDensityAt(scalp, pl.from(g.point(x, y)), map);
        }
    }
    return g;
}

// ---- Shapes carried -------------------------------------------------------------------------------------

Vec3f turn(const Vec3f& vec, const Vec3f& a0, const Vec3f& b0) {
    const Vec3f a = a0.normalized(), b = b0.normalized();
    const Vec3f axis = a.cross(b);
    const float s = axis.length(), c = a.dot(b);
    if (s < 1e-9f) return c > 0.0f ? vec : vec * -1.0f;
    const Vec3f k = axis * (1.0f / s);
    return vec * c + k.cross(vec) * s + k * (k.dot(vec) * (1.0f - c));
}

// An old guide whose shape new guides near it may take: its root, the scalp's normal there, its points.
struct Source {
    Vec3f root, normal;
    std::vector<Vec3f> points;
};

Source sourceOf(const ScalpMesh& scalp, const HierarchyGuide& g) {
    Source s;
    s.points = g.points;
    s.root = g.points.empty() ? Vec3f{} : g.points.front();
    s.normal = scalpPointNormal(scalp, s.root).normal;
    return s;
}

// The shape the sources near `root` give (kBlendNearest of them, by inverse square distance),
// each carried onto `root` turned from its normal onto `normal`, `count` points.
std::vector<Vec3f> blendedShape(const std::vector<Source>& sources, const Vec3f& root, const Vec3f& normal, std::size_t count) {
    std::vector<std::pair<float, const Source*>> near;
    for (const Source& s : sources) {
        if (s.points.size() >= 2) near.push_back({(s.root - root).length(), &s});
    }
    if (near.empty()) return {};
    std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    near.resize(std::min<std::size_t>(near.size(), kBlendNearest));
    if (count < 2) count = near.front().second->points.size();
    std::vector<Vec3f> out(count, Vec3f{});
    double total = 0.0;
    const float eps = std::max(near.front().first * 1e-3f, 1e-9f);
    for (const auto& [d, s] : near) {
        const double w = 1.0 / std::pow(static_cast<double>(d) + eps, 2.0);
        const std::vector<Vec3f> pts = resamplePolyline(s->points, count);
        for (std::size_t i = 0; i < count; ++i) out[i] = out[i] + turn(pts[i] - s->root, s->normal, normal) * static_cast<float>(w);
        total += w;
    }
    for (Vec3f& p : out) p = root + p * static_cast<float>(1.0 / total);
    return out;
}

// ---- The hierarchy under a root ---------------------------------------------------------------------

// Each level under `guide` (2, 3, ...) and the guides at it.
std::map<int, std::vector<GuideId>> levelsUnder(const GuideHierarchy& h, GuideId guide) {
    std::map<int, std::vector<GuideId>> out;
    std::vector<GuideId> level{guide};
    for (int depth = 2; !level.empty(); ++depth) {
        std::vector<GuideId> below;
        for (const GuideId g : level) {
            for (const GuideId k : h.childrenOf(g)) below.push_back(k);
        }
        if (!below.empty()) out[depth] = below;
        level = std::move(below);
    }
    return out;
}

// The cells split as deep as the old roots were (`sources`: per level, the old guides there; only
// how deep they went is read). Each level is made from the new cells alone, as + makes it (a drawn
// root's split: its root shape sliced in two, root_tubes.h addTubeLevel), never shaped after the
// old guides: the cells changed, so what was groomed under the old ones does not fit them.
void rebuildLevels(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& cells,
                   const std::map<int, std::vector<Source>>& sources) {
    if (cells.empty() || sources.empty()) return;
    const int deepest = sources.rbegin()->first;
    for (int level = 2; level <= deepest && level <= kMaxTubeLevels; ++level) addTubeLevel(h, &scalp, cells);
    h.evaluate(&scalp);
}

// ---- Cells to root tubes ------------------------------------------------------------------------------

// A cell's CVs, at most kMaxTubeShapePoints: the points that matter least to its shape taken out,
// those it shares with a neighbour or the old outline last.
std::vector<P> capped(std::vector<P> cell, const std::set<std::pair<double, double>>& keep) {
    while (cell.size() > kMaxTubeShapePoints) {
        double least = std::numeric_limits<double>::infinity();
        std::size_t at = 0;
        bool anyFree = false;
        for (std::size_t i = 0; i < cell.size(); ++i) {
            if (keep.count({cell[i].x, cell[i].y})) continue;
            anyFree = true;
        }
        for (std::size_t i = 0; i < cell.size(); ++i) {
            if (anyFree && keep.count({cell[i].x, cell[i].y})) continue;
            const P& a = cell[(i + cell.size() - 1) % cell.size()];
            const P& b = cell[i];
            const P& c = cell[(i + 1) % cell.size()];
            const double area = std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
            if (area < least) least = area, at = i;
        }
        cell.erase(cell.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return cell;
}

struct Cut {
    Plane plane;
    std::vector<Vec3f> outline3;  // the old root's CVs
    std::vector<P> outline;       // in the plane, counter-clockwise
    double rootSize = 0.0;        // sqrt of its area
};

Cut cutOf(const ScalpMesh& scalp, const std::vector<Vec3f>& outline3) {
    Cut c;
    c.outline3 = outline3;
    c.plane = planeOf(scalp, outline3);
    for (const Vec3f& p : outline3) c.outline.push_back(c.plane.to(p));
    if (signedArea(c.outline) < 0.0) {
        std::reverse(c.outline.begin(), c.outline.end());
        std::reverse(c.outline3.begin(), c.outline3.end());
    }
    c.rootSize = std::sqrt(std::abs(signedArea(c.outline)));
    return c;
}

// The cells' CVs on the scalp: the old outline's CVs as they were (the roots round it stay sealed
// to them), the rest taken onto the scalp, each point once (neighbours share them exactly).
std::vector<std::vector<Vec3f>> liftCells(const ScalpMesh& scalp, const Cut& cut, const std::vector<std::vector<P>>& cells) {
    std::map<std::pair<double, double>, Vec3f> lifted;
    for (std::size_t i = 0; i < cut.outline.size(); ++i) lifted[{cut.outline[i].x, cut.outline[i].y}] = cut.outline3[i];
    std::map<std::pair<double, double>, int> uses;
    for (const auto& cell : cells) {
        for (const P& p : cell) ++uses[{p.x, p.y}];
    }
    std::set<std::pair<double, double>> keep;
    for (const auto& [p, n] : uses) {
        if (n > 1 || lifted.count(p)) keep.insert(p);
    }
    std::vector<std::vector<Vec3f>> out;
    for (const auto& cell : cells) {
        std::vector<Vec3f> cvs;
        if (cell.size() >= 3) {
            for (const P& p : capped(cell, keep)) {
                const auto key = std::make_pair(p.x, p.y);
                auto it = lifted.find(key);
                if (it == lifted.end()) it = lifted.emplace(key, lift(scalp, cut.plane, p).position).first;
                cvs.push_back(it->second);
            }
        }
        out.push_back(std::move(cvs));
    }
    return out;
}

// A cell's guide inside the old tube: the path a strand rooted at the cell's middle takes there.
std::vector<Vec3f> pathInTube(const GuideHierarchy& h, const ScalpMesh& scalp, const HierarchyGuide& g, const Vec3f& at) {
    if (g.points.size() < 2) return {};
    TubeSpine spine(g.points.data(), g.points.size(), g.frameUp, g.tube, h.scale());
    if (scalp.contains(g.root)) spine.setRootOnScalp(scalp.normal(g.root), scalp.acrossAt(g.root));
    const SectionPoint uv = spine.rootCoords(at);
    std::vector<Vec3f> out;
    const std::size_t n = g.points.size();
    for (std::size_t i = 0; i < n; ++i) out.push_back(spine.at(static_cast<float>(i) / static_cast<float>(n - 1), uv.u, uv.v));
    out.front() = at;
    return out;
}

float taperOf(const TubeProfile& tube) {
    if (tube.width.size() < 2 || tube.width.front() <= 0.0f) return 1.0f;
    return std::clamp(tube.width.back() / tube.width.front(), 0.0f, 4.0f);
}

std::string groupName(const GuideHierarchy& h, GroupId id) {
    const GuideGroup* g = h.group(id);
    return g ? g->name : std::string();
}

// A cell made a root tube along `path` (its point count kept), into `group`. kNoGuide when the
// cell is too small to fit.
GuideId makeCell(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<Vec3f>& cvs, const std::vector<Vec3f>& path,
                 const std::string& group, float taper, float fallbackLength) {
    if (cvs.size() < 3) return kNoGuide;
    RootTubeOptions o;
    o.group = group;
    o.taper = taper;
    o.length = fallbackLength;
    double radius = 0.0;
    const Vec3f mid = outlineCentroid(cvs);
    for (const Vec3f& p : cvs) radius = std::max(radius, static_cast<double>((p - mid).length()));
    if (path.size() >= 2 && radius > 0.0) {
        const int base = rootPathSpans(path, static_cast<float>(radius), 1.0f);
        o.density = base > 0 ? std::clamp(static_cast<float>(path.size() - 1) / static_cast<float>(base), 0.02f, 20.0f) : 1.0f;
        o.segments = static_cast<int>(path.size()) - 1;
    }
    try {
        return makeRootTube(h, scalp, cvs, path.size() >= 2 ? path : std::vector<Vec3f>{}, o);
    } catch (const std::exception&) {
        return kNoGuide;  // (a sliver the fit cannot take: its neighbours cover it)
    }
}

float guideLength(const std::vector<Vec3f>& pts) {
    float total = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) total += (pts[i] - pts[i - 1]).length();
    return total;
}

std::uint32_t nextRecordId(const std::vector<RootSubdivisionRecord>& records) {
    std::uint32_t id = 0;
    for (const RootSubdivisionRecord& r : records) id = std::max(id, r.id);
    return id + 1;
}

std::vector<StrandRoot> bind(const ScalpMesh& scalp, const std::vector<Vec3f>& pts) {
    std::vector<StrandRoot> out;
    for (const Vec3f& p : pts) out.push_back(scalp.closestPoint(p).root);
    return out;
}

bool onScalp(const ScalpMesh& scalp, const std::vector<StrandRoot>& pts) {
    return std::all_of(pts.begin(), pts.end(), [&](const StrandRoot& r) { return scalp.contains(r); });
}

std::vector<Vec3f> positions(const ScalpMesh& scalp, const std::vector<StrandRoot>& pts) {
    std::vector<Vec3f> out;
    for (const StrandRoot& r : pts) out.push_back(scalp.position(r));
    return out;
}

// Sites thrown as subdivisionSites throws them, only where `allowed` says (none: everywhere).
std::vector<P> throwSites(const std::vector<P>& outlineIn, const std::function<float(const P&)>& size, std::uint64_t seed,
                          int relax, const std::vector<P>& kept, const std::function<bool(const P&)>& allowed) {
    const std::vector<P> outline = ccw(outlineIn);
    if (outline.size() < 3) return kept;
    double lo[2] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    double hi[2] = {-lo[0], -lo[1]};
    for (const P& p : outline) {
        lo[0] = std::min(lo[0], p.x), lo[1] = std::min(lo[1], p.y);
        hi[0] = std::max(hi[0], p.x), hi[1] = std::max(hi[1], p.y);
    }
    const double span = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    // The smallest size anywhere inside (a coarse look), the candidates a third of it apart.
    double smallest = std::numeric_limits<double>::infinity();
    for (int y = 0; y <= 16; ++y) {
        for (int x = 0; x <= 16; ++x) {
            const P p{lo[0] + (hi[0] - lo[0]) * x / 16.0, lo[1] + (hi[1] - lo[1]) * y / 16.0};
            smallest = std::min(smallest, static_cast<double>(size(p)));
        }
    }
    if (!(smallest > 0.0)) smallest = span;
    const double step = std::max(smallest / 3.0, span / std::sqrt(static_cast<double>(kMaxGrid)));
    Rng rng(seed);
    std::vector<P> candidates;
    for (double y = lo[1]; y <= hi[1]; y += step) {
        for (double x = lo[0]; x <= hi[0]; x += step) {
            const P p{x + step * rng.nextFloat(), y + step * rng.nextFloat()};
            if (inside(outline, p) && (!allowed || allowed(p))) candidates.push_back(p);
        }
    }
    for (std::size_t i = candidates.size(); i > 1; --i) {  // shuffled (Fisher-Yates)
        std::swap(candidates[i - 1], candidates[static_cast<std::size_t>(rng.nextU64() % i)]);
    }
    std::vector<P> sites = kept;
    std::vector<double> sizes;
    for (const P& k : kept) sizes.push_back(size(k));
    const std::size_t first = sites.size();
    for (const P& c : candidates) {
        const double sc = size(c);
        bool free = true;
        for (std::size_t i = 0; i < sites.size() && free; ++i) {
            if (dist(c, sites[i]) < 0.5 * (sc + sizes[i])) free = false;
        }
        if (free) {
            sites.push_back(c);
            sizes.push_back(sc);
        }
    }
    if (sites.empty()) sites.push_back(areaCentroid(outline));
    // Evened out: each thrown site half way to its cell's middle.
    for (int pass = 0; pass < relax; ++pass) {
        const std::vector<std::vector<P>> cells = voronoiCells(outline, sites);
        for (std::size_t i = first; i < sites.size(); ++i) {
            if (cells[i].size() < 3) continue;
            const P to = sites[i] + (areaCentroid(cells[i]) - sites[i]) * 0.5;
            if (inside(outline, to) && (!allowed || allowed(to))) sites[i] = to;
        }
    }
    return sites;
}

// Sizes at density 0 and 1 grown, if need be, so the root takes at most kMaxCells cells.
void fitSizes(double area, double* s0, double* s1) {
    const double smallest = std::min(*s0, *s1);
    if (smallest <= 0.0) return;
    const double cells = area / (smallest * smallest);
    if (cells > static_cast<double>(kMaxCells)) {
        const double k = std::sqrt(cells / static_cast<double>(kMaxCells));
        *s0 *= k, *s1 *= k;
    }
}

}  // namespace

// ---- The flat cut ---------------------------------------------------------------------------------------

std::vector<std::vector<CutPoint>> voronoiCells(const std::vector<CutPoint>& outlineIn, const std::vector<CutPoint>& sites) {
    const std::vector<P> outline = ccw(outlineIn);
    const double eps = 1e-9 * std::max(polygonSize(outline), 1e-12);
    std::vector<std::vector<P>> cells(sites.size());
    if (outline.size() < 3) return cells;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        if (!inside(outline, sites[i])) continue;
        std::vector<std::size_t> order;
        for (std::size_t j = 0; j < sites.size(); ++j) {
            if (j != i) order.push_back(j);
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return dist(sites[a], sites[i]) < dist(sites[b], sites[i]); });
        std::vector<P> cell = outline;
        for (const std::size_t j : order) {
            double reach = 0.0;
            for (const P& p : cell) reach = std::max(reach, dist(p, sites[i]));
            if (dist(sites[j], sites[i]) > 2.0 * reach) break;  // (sorted: none further can cut it)
            const P m = (sites[i] + sites[j]) * 0.5;
            cell = clipHalf(cell, m, sites[j] - sites[i]);
            if (cell.size() < 3) break;
        }
        cells[i] = tidy(cell, eps);
    }
    // Welded: points closer than a millionth of the root's size made one (the outline's own first).
    const double weld = 1e-6 * std::max(polygonSize(outline), 1e-12);
    std::vector<P> reps(outline.begin(), outline.end());
    const double cellSize = weld * 4.0;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> buckets;
    auto key = [&](double x, double y) {
        return (static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(x / cellSize))) << 32) ^
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(std::floor(y / cellSize))));
    };
    for (std::size_t r = 0; r < reps.size(); ++r) buckets[key(reps[r].x, reps[r].y)].push_back(r);
    auto weldPoint = [&](const P& p) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = buckets.find(key(p.x + dx * cellSize, p.y + dy * cellSize));
                if (it == buckets.end()) continue;
                for (const std::size_t r : it->second) {
                    if (dist(reps[r], p) <= weld) return reps[r];
                }
            }
        }
        reps.push_back(p);
        buckets[key(p.x, p.y)].push_back(reps.size() - 1);
        return p;
    };
    for (auto& cell : cells) {
        for (P& p : cell) p = weldPoint(p);
        cell = tidy(cell, 0.0);
    }
    return cells;
}

std::vector<CutPoint> subdivisionSites(const std::vector<CutPoint>& outline, const std::function<float(const CutPoint&)>& size,
                                       std::uint64_t seed, int relax, const std::vector<CutPoint>& kept) {
    return throwSites(outline, size, seed, relax, kept, nullptr);
}

float subdivisionDensityAt(const ScalpMesh& scalp, const Vec3f& p, const std::string& map) {
    if (!scalp.hasColorSet(map)) return kDefaultSubdivisionDensity;
    const SurfacePoint at = scalp.closestPoint(p);
    if (at.root.face < 0) return kDefaultSubdivisionDensity;
    return std::clamp(scalp.colorAt(map, at.root)[0], 0.0f, 1.0f);
}

// ---- Cutting roots -------------------------------------------------------------------------------------

namespace {

// Root tube `guide` cut afresh (the record of it made): see subdivideRootTubes.
void cutRoot(GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide, const RootSubdivisionOptions& o, SubdivisionResult& result) {
    const HierarchyGuide* g = h.guide(guide);
    if (!g) throw std::invalid_argument("no guide " + std::to_string(guide));
    const Cut cut = cutOf(scalp, rootTubeOutline(h, scalp, guide));
    if (cut.rootSize <= 0.0) throw std::invalid_argument("root tube " + std::to_string(guide) + " has no area to cut");
    const double denser = 1.0 / std::sqrt(std::max(0.01, static_cast<double>(o.multiplier > 0.0f ? o.multiplier : 1.0f)));
    double s0 = std::max(1e-4, static_cast<double>(o.sizeAtZero)) * cut.rootSize * denser;
    double s1 = std::max(1e-4, static_cast<double>(o.sizeAtOne)) * cut.rootSize * denser;
    fitSizes(cut.rootSize * cut.rootSize, &s0, &s1);
    const DensityGrid grid = densityGrid(scalp, cut.plane, cut.outline, 0.5 * std::min(s0, s1), o.map);
    auto size = [&](const P& p) { return static_cast<float>(s0 + (s1 - s0) * grid.at(p)); };
    RootSubdivisionRecord record;
    record.id = nextRecordId(h.rootSubdivisions());
    record.seed = hashCombine(o.seed, record.id);
    const std::vector<P> sites = throwSites(cut.outline, size, record.seed, o.relax, {}, nullptr);
    const std::vector<std::vector<Vec3f>> cells = liftCells(scalp, cut, voronoiCells(cut.outline, sites));

    // The old guide, its tube and the hierarchy under it, as they were.
    const HierarchyGuide old = *g;
    const std::string group = groupName(h, old.group);
    std::map<int, std::vector<Source>> sources;
    for (const auto& [level, ids] : levelsUnder(h, guide)) {
        for (const GuideId k : ids) {
            if (const HierarchyGuide* kid = h.guide(k)) sources[level].push_back(sourceOf(scalp, *kid));
        }
    }
    const float taper = taperOf(old.tube);
    const float length = guideLength(old.points);

    std::vector<GuideId> made;
    std::vector<StrandRoot> siteRoots;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (cells[i].size() < 3) continue;
        const Vec3f mid = scalpPointNormal(scalp, outlineCentroid(cells[i])).point;
        const GuideId id = makeCell(h, scalp, cells[i], pathInTube(h, scalp, old, mid), group, taper, length);
        if (id == kNoGuide) continue;
        made.push_back(id);
        siteRoots.push_back(lift(scalp, cut.plane, sites[i]).root);
    }
    if (made.empty()) throw std::invalid_argument("root tube " + std::to_string(guide) + " is too small to cut");
    h.evaluate(&scalp);
    // The old root and its hierarchy out (after the cells are in: its group never empties).
    const std::size_t before = h.guides().size();
    h.removeGuides({guide}, true);
    result.removed += before - h.guides().size();
    // Its record dropped from any record it was a cell of.
    std::vector<RootSubdivisionRecord> records = h.rootSubdivisions();
    for (RootSubdivisionRecord& r : records) {
        for (GuideId& c : r.cells) {
            if (c == guide) c = kNoGuide;
        }
    }
    h.evaluate(&scalp);
    rebuildLevels(h, scalp, made, sources);

    record.map = o.map;
    record.outline = bind(scalp, cut.outline3);
    record.sites = siteRoots;
    record.cells = made;
    for (int y = 0; y < grid.ny; ++y) {
        for (int x = 0; x < grid.nx; ++x) {
            const P p = grid.point(x, y);
            if (!inside(cut.outline, p)) continue;
            record.samples.push_back(lift(scalp, cut.plane, p).root);
            record.density.push_back(grid.d[static_cast<std::size_t>(y) * grid.nx + x]);
        }
    }
    record.sizeAtZero = static_cast<float>(s0);
    record.sizeAtOne = static_cast<float>(s1);
    record.multiplier = o.multiplier > 0.0f ? o.multiplier : 1.0f;
    records.push_back(std::move(record));
    h.setRootSubdivisions(std::move(records));
    result.cells.insert(result.cells.end(), made.begin(), made.end());
}

// A remembered subdivision cut again where its density changed. False when nothing changed.
bool recut(GuideHierarchy& h, const ScalpMesh& scalp, std::size_t index, const RootSubdivisionOptions& o, SubdivisionResult& result) {
    RootSubdivisionRecord r = h.rootSubdivisions()[index];
    if (!onScalp(scalp, r.outline) || !onScalp(scalp, r.sites) || !onScalp(scalp, r.samples) || r.outline.size() < 3) return false;
    const Cut cut = cutOf(scalp, positions(scalp, r.outline));
    // The density now at each sample, and where it changed.
    std::vector<float> now(r.samples.size());
    std::vector<std::pair<P, double>> changed;  // (point, reach)
    // Another multiplier: the sizes scaled to it, and the whole root cut again.
    const float had = r.multiplier > 0.0f ? r.multiplier : 1.0f;
    const bool remultiplied = o.multiplier > 0.0f && std::abs(o.multiplier - had) > 1e-4f;
    const double rescale = remultiplied ? std::sqrt(static_cast<double>(had) / static_cast<double>(o.multiplier)) : 1.0;
    double s0 = r.sizeAtZero * rescale, s1 = r.sizeAtOne * rescale;
    fitSizes(cut.rootSize * cut.rootSize, &s0, &s1);
    for (std::size_t k = 0; k < r.samples.size(); ++k) {
        const Vec3f at = scalp.position(r.samples[k]);
        now[k] = subdivisionDensityAt(scalp, at, r.map);
        if (remultiplied || std::abs(now[k] - r.density[k]) > kDensityChanged) {
            const double before = (r.sizeAtZero + (r.sizeAtOne - r.sizeAtZero) * r.density[k]) * std::max(1.0, rescale);
            const double after = s0 + (s1 - s0) * now[k];
            changed.push_back({cut.plane.to(at), std::max(before, after)});
        }
    }
    if (changed.empty()) return false;
    auto inChange = [&](const P& p) {
        return std::any_of(changed.begin(), changed.end(), [&](const auto& c) { return dist(p, c.first) <= c.second; });
    };
    const DensityGrid grid = densityGrid(scalp, cut.plane, cut.outline, 0.5 * std::min(s0, s1), r.map);
    auto size = [&](const P& p) { return static_cast<float>(s0 + (s1 - s0) * grid.at(p)); };

    // The sites kept (out of the change, or whose cell is gone: cut further, or removed), the rest replaced.
    std::vector<P> kept;
    std::vector<GuideId> keptCells, replaced;
    for (std::size_t i = 0; i < r.sites.size(); ++i) {
        const P p = cut.plane.to(scalp.position(r.sites[i]));
        const bool live = r.cells[i] != kNoGuide && h.guide(r.cells[i]);
        if (live && inChange(p)) {
            replaced.push_back(r.cells[i]);
            continue;
        }
        kept.push_back(p);
        keptCells.push_back(live ? r.cells[i] : kNoGuide);
    }
    const std::vector<P> sites = throwSites(cut.outline, size, hashCombine(r.seed, h.contentHash()), o.relax, kept, inChange);
    const std::vector<std::vector<Vec3f>> cells = liftCells(scalp, cut, voronoiCells(cut.outline, sites));

    // What the cells replaced looked like: the shapes new cells take, level by level.
    std::vector<Source> levelOne;
    std::map<int, std::vector<Source>> sources;
    std::string group;
    float taper = 1.0f, length = 1.0f;
    for (const GuideId c : replaced) {
        const HierarchyGuide* g = h.guide(c);
        if (!g) continue;
        levelOne.push_back(sourceOf(scalp, *g));
        if (group.empty()) group = groupName(h, g->group), taper = taperOf(g->tube), length = guideLength(g->points);
        for (const auto& [level, ids] : levelsUnder(h, c)) {
            for (const GuideId k : ids) {
                if (const HierarchyGuide* kid = h.guide(k)) sources[level].push_back(sourceOf(scalp, *kid));
            }
        }
    }
    if (levelOne.empty()) {  // (nothing replaced: new sites in a gap take the kept cells' shapes)
        for (const GuideId c : keptCells) {
            if (const HierarchyGuide* g = h.guide(c)) {
                levelOne.push_back(sourceOf(scalp, *g));
                if (group.empty()) group = groupName(h, g->group), taper = taperOf(g->tube), length = guideLength(g->points);
            }
        }
    }

    RootSubdivisionRecord next = r;
    next.sites.clear();
    next.cells.clear();
    std::vector<GuideId> made;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        const SurfacePoint siteAt = lift(scalp, cut.plane, sites[i]);
        if (i < kept.size()) {
            const GuideId c = keptCells[i];
            if (c != kNoGuide && cells[i].size() >= 3) {
                // A kept cell whose outline moved (a neighbour changed): its root refitted, its guide kept.
                const std::vector<Vec3f> had = rootTubeOutline(h, scalp, c);
                bool same = had.size() == cells[i].size();
                for (std::size_t k = 0; same && k < had.size(); ++k) {
                    same = std::any_of(had.begin(), had.end(), [&](const Vec3f& q) { return (q - cells[i][k]).length() <= 1e-4 * cut.rootSize; });
                }
                if (same) {
                    ++result.kept;
                } else {
                    try {
                        editRootTube(h, scalp, c, cells[i]);
                    } catch (const std::exception&) {
                    }
                }
            }
            next.sites.push_back(siteAt.root);
            next.cells.push_back(c);
            continue;
        }
        if (cells[i].size() < 3) continue;
        const ScalpPointNormal mid = scalpPointNormal(scalp, outlineCentroid(cells[i]));
        const std::vector<Vec3f> path = blendedShape(levelOne, mid.point, mid.normal, 0);
        const GuideId id = makeCell(h, scalp, cells[i], path, group.empty() ? std::string(kRootTubeGroup) : group, taper, length);
        if (id == kNoGuide) continue;
        made.push_back(id);
        next.sites.push_back(siteAt.root);
        next.cells.push_back(id);
    }
    h.evaluate(&scalp);
    // The cells replaced out with their hierarchies (after the new ones are in: the group never empties).
    if (!replaced.empty()) {
        const std::size_t before = h.guides().size();
        h.removeGuides(replaced, true);
        result.removed += before - h.guides().size();
        h.evaluate(&scalp);
    }
    rebuildLevels(h, scalp, made, sources);
    next.density = now;
    next.sizeAtZero = static_cast<float>(s0);
    next.sizeAtOne = static_cast<float>(s1);
    next.multiplier = remultiplied ? o.multiplier : had;
    std::vector<RootSubdivisionRecord> records = h.rootSubdivisions();
    records[index] = std::move(next);
    h.setRootSubdivisions(std::move(records));
    result.cells.insert(result.cells.end(), made.begin(), made.end());
    return true;
}

}  // namespace

SubdivisionResult subdivideRootTubes(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides,
                                     const RootSubdivisionOptions& options) {
    if (guides.empty()) throw std::invalid_argument("select a root tube to subdivide");
    const std::vector<GuideId> roots = rootTubesOf(h, guides);  // (a smaller tube of the hierarchy: its root tube)
    if (roots.empty()) throw std::invalid_argument("the selection holds no root tube to subdivide");
    h.evaluate(&scalp);
    SubdivisionResult result;
    for (const GuideId g : roots) cutRoot(h, scalp, g, options, result);
    h.evaluate(&scalp);
    return result;
}

SubdivisionResult applySubdivisionDensity(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides,
                                          const RootSubdivisionOptions& options) {
    h.evaluate(&scalp);
    SubdivisionResult result;
    for (std::size_t i = 0; i < h.rootSubdivisions().size(); ++i) {
        // (Given a map: only the subdivisions painted with it.)
        if (!options.map.empty() && h.rootSubdivisions()[i].map != options.map) continue;
        recut(h, scalp, i, options, result);
    }
    // The roots given that are not already cells of a subdivision: cut.
    std::set<GuideId> cells;
    for (const RootSubdivisionRecord& r : h.rootSubdivisions()) cells.insert(r.cells.begin(), r.cells.end());
    std::vector<GuideId> fresh;
    if (!guides.empty()) {
        for (const GuideId g : rootTubesOf(h, guides)) {
            if (!cells.count(g)) fresh.push_back(g);
        }
    }
    RootSubdivisionOptions fresh0 = options;
    if (fresh0.map.empty()) fresh0.map = subdivisionDensityMapName(1);
    for (const GuideId g : fresh) cutRoot(h, scalp, g, fresh0, result);
    h.evaluate(&scalp);
    return result;
}

std::string subdivisionDensityMapName(int n) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "%02d", std::max(n, 1));
    return std::string(kSubdivisionDensityMap) + digits;
}

std::string subdivisionDensityMapFor(const GuideHierarchy& h, const std::vector<GuideId>& guides,
                                     const std::vector<std::string>& taken, bool* fresh) {
    // A cell of a subdivision (or a tube of a cell's hierarchy): that subdivision's map.
    for (const GuideId g : rootTubesOf(h, guides)) {
        for (const RootSubdivisionRecord& r : h.rootSubdivisions()) {
            if (!r.map.empty() && std::find(r.cells.begin(), r.cells.end(), g) != r.cells.end()) {
                if (fresh) *fresh = false;
                return r.map;
            }
        }
    }
    std::set<std::string> used(taken.begin(), taken.end());
    for (const RootSubdivisionRecord& r : h.rootSubdivisions()) used.insert(r.map);
    for (int n = 1;; ++n) {
        const std::string name = subdivisionDensityMapName(n);
        if (!used.count(name)) {
            if (fresh) *fresh = true;
            return name;
        }
    }
}

}  // namespace groomlab
