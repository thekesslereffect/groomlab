// SPDX-License-Identifier: Apache-2.0
// Modeled tubes read off a mesh, their curves and the sections fitted to them (tube_mesh.h): a port
// of Maya's Python (tube_mesh.py, utils.py's Tubes to Curves helpers), step for step, so the hosts
// read the same tubes from the same mesh. The math runs in doubles, as the Python did.

#include "groomlab/tube_mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <unordered_set>

namespace groomlab {

namespace {

constexpr double kPi = 3.14159265358979323846;

double dist(const Vec3d& a, const Vec3d& b) { return (a - b).length(); }
Vec3d lerp(const Vec3d& a, const Vec3d& b, double f) { return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f}; }

// The points' mean (each sum divided by the count, as the Python's).
Vec3d mean(const Vec3d* pts, std::size_t count) {
    Vec3d sum;
    for (std::size_t i = 0; i < count; ++i) sum = sum + pts[i];
    const double n = static_cast<double>(count);
    return {sum.x / n, sum.y / n, sum.z / n};
}
Vec3d mean(const std::vector<Vec3d>& pts) { return mean(pts.data(), pts.size()); }

// To the nearest whole number, a half to the even one (Python's round).
double roundHalfEven(double x) {
    const double r = std::floor(x + 0.5);
    if (x + 0.5 == r && std::fmod(r, 2.0) != 0.0) return r - 1.0;
    return r;
}

Vec2d toPlane(const Vec3d& p, const Vec3d& center, const Vec3d& u, const Vec3d& v) {
    const Vec3d d = p - center;
    return {d.dot(u), d.dot(v)};
}
Vec3d fromPlane(const Vec2d& q, const Vec3d& center, const Vec3d& u, const Vec3d& v) { return center + (u * q.x + v * q.y); }

// Which gaps are bevels: under `ratio` of the larger gap beside them.
std::vector<bool> closeGaps(const std::vector<double>& gaps, double ratio, bool cyclic) {
    const std::size_t n = gaps.size();
    std::vector<bool> out(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        bool any = false;
        double most = 0.0;
        if (cyclic || i > 0) most = gaps[(i + n - 1) % n], any = true;
        if (cyclic || i + 1 < n) most = any ? std::max(most, gaps[(i + 1) % n]) : gaps[(i + 1) % n], any = true;
        out[i] = any && gaps[i] < ratio * most;
    }
    return out;
}

using Edge = std::pair<int, int>;  // (the lower vertex, the higher)

// A mesh's edges in the order its polygons first walk them, each with the faces on it.
struct MeshEdges {
    std::vector<Edge> edges;
    std::vector<std::vector<int>> faces;
    std::map<Edge, std::size_t> index;

    explicit MeshEdges(const std::vector<std::vector<int>>& polygons) {
        for (std::size_t f = 0; f < polygons.size(); ++f) {
            const std::vector<int>& poly = polygons[f];
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const int a = poly[i], b = poly[(i + 1) % poly.size()];
                const Edge key = a < b ? Edge{a, b} : Edge{b, a};
                const auto [it, added] = index.emplace(key, edges.size());
                if (added) {
                    edges.push_back(key);
                    faces.emplace_back();
                }
                faces[it->second].push_back(static_cast<int>(f));
            }
        }
    }
};

void checkIndices(std::size_t pointCount, const std::vector<std::vector<int>>& polygons) {
    for (const std::vector<int>& poly : polygons) {
        for (const int v : poly) {
            if (v >= 0 && static_cast<std::size_t>(v) < pointCount) continue;
            throw std::invalid_argument("a polygon names a vertex the mesh has not");
        }
    }
}

// The mesh's shells: each one's vertices, rising; the shells by their lowest vertex.
std::vector<std::vector<int>> shellsOf(std::size_t vertexCount, const std::vector<Edge>& edges) {
    std::vector<int> parent(vertexCount);
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&parent](int a) {
        while (parent[static_cast<std::size_t>(a)] != a) {
            int& up = parent[static_cast<std::size_t>(a)];
            up = parent[static_cast<std::size_t>(up)];
            a = up;
        }
        return a;
    };
    for (const Edge& e : edges) {
        const int ra = find(e.first), rb = find(e.second);
        if (ra != rb) parent[static_cast<std::size_t>(ra)] = rb;
    }
    std::vector<std::vector<int>> groups;
    std::map<int, std::size_t> at;
    for (std::size_t v = 0; v < vertexCount; ++v) {
        const auto [it, added] = at.emplace(find(static_cast<int>(v)), groups.size());
        if (added) groups.emplace_back();
        groups[it->second].push_back(static_cast<int>(v));
    }
    return groups;
}

// Boundary edges as ordered vertex loops.
std::vector<std::vector<int>> boundaryLoops(const std::vector<Edge>& boundary) {
    std::map<int, std::vector<int>> nbrs;
    for (const Edge& e : boundary) {
        nbrs[e.first].push_back(e.second);
        nbrs[e.second].push_back(e.first);
    }
    for (const auto& [v, list] : nbrs) {
        if (list.size() != 2) throw TubeMeshError("its open edges touch at a vertex (non-manifold)");
    }
    std::vector<std::vector<int>> loops;
    std::set<int> seen;
    for (const auto& [start, unused] : nbrs) {
        (void)unused;
        if (seen.count(start)) continue;
        std::vector<int> loop{start};
        seen.insert(start);
        int prev = -1, cur = start;
        while (true) {
            const std::vector<int>& two = nbrs[cur];
            const int next = two[0] != prev ? two[0] : two[1];
            if (next == start) break;
            loop.push_back(next);
            seen.insert(next);
            prev = cur;
            cur = next;
        }
        loops.push_back(std::move(loop));
    }
    return loops;
}

// Rings from boundary loop `start`, across the tube, to the boundary loop in `ends` or a pointed tip.
std::vector<std::vector<int>> walk(const std::vector<int>& start, const std::vector<std::vector<int>>& nbrs,
                                   const std::vector<std::set<int>>& ends, std::size_t shellSize, bool* pointed) {
    std::vector<std::vector<int>> rings{start};
    std::set<int> used(start.begin(), start.end());
    std::set<int> prev;
    while (true) {
        const std::vector<int>& cur = rings.back();
        const std::set<int> curSet(cur.begin(), cur.end());
        std::vector<int> next;
        for (const int v : cur) {
            int ahead = 0, to = -1;
            for (const int w : nbrs[static_cast<std::size_t>(v)]) {
                if (!curSet.count(w) && !prev.count(w)) ++ahead, to = w;
            }
            if (ahead != 1) {
                if (ahead == 0 && rings.size() > 1) throw TubeMeshError("an edge loop stops short of the tube's end");
                throw TubeMeshError("it is not a clean quad tube (a vertex where " + std::to_string(ahead) + " edges run on, not 1)");
            }
            next.push_back(to);
        }
        const std::set<int> distinct(next.begin(), next.end());
        if (distinct.size() == 1 && cur.size() > 1) {  // a pointed tip
            rings.push_back(next);
            used.insert(distinct.begin(), distinct.end());
            *pointed = true;
            break;
        }
        if (distinct.size() != next.size()) {
            throw TubeMeshError("its edge loops change how many points they have along the tube (only a pointed tip may close them)");
        }
        for (const int v : distinct) {
            if (used.count(v)) throw TubeMeshError("its edge loops run back on themselves");
        }
        rings.push_back(next);
        used.insert(distinct.begin(), distinct.end());
        prev = curSet;
        if (std::any_of(ends.begin(), ends.end(), [&distinct](const std::set<int>& e) { return e == distinct; })) {
            *pointed = false;
            break;
        }
        if (used.size() >= shellSize) throw TubeMeshError("its edge loops never reach its other end");
    }
    if (used.size() != shellSize) throw TubeMeshError("it has vertices off its edge loops");
    return rings;
}

// Each plan entry's rings as one: itself, or the mean of its group, point by point.
std::vector<TubeRing> mergedLoops(const std::vector<TubeRing>& old, const std::vector<std::vector<std::size_t>>& plan, std::size_t sides) {
    std::vector<TubeRing> out;
    out.reserve(plan.size());
    for (const std::vector<std::size_t>& group : plan) {
        if (group.size() == 1) {
            out.push_back(old[group[0]]);
            continue;
        }
        TubeRing ring;
        std::vector<Vec3d> pts(group.size());
        for (std::size_t i = 0; i < sides; ++i) {
            for (std::size_t j = 0; j < group.size(); ++j) pts[j] = old[group[j]][i];
            ring.push_back(mean(pts));
        }
        out.push_back(std::move(ring));
    }
    return out;
}

std::vector<TubeRing> mergedRuns(const std::vector<TubeRing>& old, const std::vector<std::vector<std::size_t>>& groups) {
    std::vector<TubeRing> out;
    out.reserve(old.size());
    for (const TubeRing& r : old) {
        TubeRing ring;
        for (const std::vector<std::size_t>& g : groups) {
            std::vector<Vec3d> pts;
            for (const std::size_t i : g) pts.push_back(r[i]);
            ring.push_back(mean(pts));
        }
        out.push_back(std::move(ring));
    }
    return out;
}

void mergeLoops(ModeledTube& tube, double ratio) {
    const std::vector<TubeRing>& rings = tube.rings;
    if (rings.size() < 3) return;
    const std::size_t sides = tube.sides();
    std::vector<double> gaps;
    for (std::size_t k = 0; k + 1 < rings.size(); ++k) {
        double sum = 0.0;
        for (std::size_t i = 0; i < sides; ++i) sum += dist(rings[k][i], rings[k + 1][i]);
        gaps.push_back(sum / static_cast<double>(sides));
    }
    const std::vector<bool> close = closeGaps(gaps, ratio, false);
    std::vector<std::vector<std::size_t>> plan;  // each new loop: the old loops it is the mean of
    for (std::size_t k = 0; k < rings.size();) {
        std::vector<std::size_t> group{k};
        while (group.back() < gaps.size() && close[group.back()]) group.push_back(group.back() + 1);
        const std::size_t last = group.back();
        if (group.size() == 1) {
            plan.push_back(group);
        } else if (group.front() == 0) {
            plan.push_back({0});  // the root's loop stays where it is
        } else if (last == rings.size() - 1) {
            plan.push_back({rings.size() - 1});  // and the tip's
        } else {
            plan.push_back(group);
        }
        k = last + 1;
    }
    tube.rings = mergedLoops(tube.rings, plan, sides);
    if (tube.smoothRings) tube.smoothRings = mergedLoops(*tube.smoothRings, plan, sides);
}

void mergeRuns(ModeledTube& tube, double ratio) {
    const std::size_t n = tube.sides();
    if (n <= 3) return;
    std::vector<const TubeRing*> shaped;
    for (const TubeRing& r : tube.rings) {
        if (sectionRadius(r) > 1e-9) shaped.push_back(&r);
    }
    if (shaped.empty()) {
        for (const TubeRing& r : tube.rings) shaped.push_back(&r);
    }
    std::vector<double> gaps;
    for (std::size_t i = 0; i < n; ++i) {
        double sum = 0.0;
        for (const TubeRing* r : shaped) sum += dist((*r)[i], (*r)[(i + 1) % n]);
        gaps.push_back(sum / static_cast<double>(shaped.size()));
    }
    const std::vector<bool> close = closeGaps(gaps, ratio, true);
    const std::size_t closed = static_cast<std::size_t>(std::count(close.begin(), close.end(), true));
    if (closed == 0 || closed == n) return;
    // Groups of runs joined by close gaps, round the tube from a gap that is not close.
    const std::size_t open = static_cast<std::size_t>(std::find(close.begin(), close.end(), false) - close.begin());
    const std::size_t start = (open + 1) % n;
    std::vector<std::vector<std::size_t>> groups;
    std::vector<std::size_t> current{start};
    for (std::size_t step = 1; step < n; ++step) {
        const std::size_t i = (start + step) % n;
        if (close[(i + n - 1) % n]) {
            current.push_back(i);
        } else {
            groups.push_back(current);
            current = {i};
        }
    }
    groups.push_back(current);
    if (groups.size() < 3) return;
    tube.rings = mergedRuns(tube.rings, groups);
    if (tube.smoothRings) tube.smoothRings = mergedRuns(*tube.smoothRings, groups);
}

// Least squares y = slope * x + offset (slope 0 or more): its squared error.
double regressError(const std::vector<double>& xs, const std::vector<double>& ys) {
    const double n = static_cast<double>(xs.size());
    double mx = 0.0, my = 0.0;
    for (const double x : xs) mx += x;
    for (const double y : ys) my += y;
    mx /= n;
    my /= n;
    double sxx = 0.0, sxy = 0.0;
    for (const double x : xs) sxx += (x - mx) * (x - mx);
    for (std::size_t i = 0; i < xs.size(); ++i) sxy += (xs[i] - mx) * (ys[i] - my);
    const double slope = sxx > 1e-18 ? std::max(sxy / sxx, 0.0) : 0.0;
    const double offset = my - slope * mx;
    double error = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double e = slope * xs[i] + offset - ys[i];
        error += e * e;
    }
    return error;
}

// What fitting a shape to points at any turn needs, summed once: per axis the shape's centered
// coordinates' square sum, and the points' sums against them (so each turn is a few sums: fitAt).
struct Fit {
    double n = 0, mu = 0, mv = 0, suu = 0, svv = 0, sx = 0, sy = 0, xu = 0, yu = 0, xv = 0, yv = 0, sxx = 0, syy = 0, sxy = 0;
    Fit(const std::vector<Vec2d>& points, const std::vector<Vec2d>& shape) {
        n = static_cast<double>(points.size());
        for (const Vec2d& p : shape) mu += p.x, mv += p.y;
        mu /= n;
        mv /= n;
        for (std::size_t i = 0; i < points.size() && i < shape.size(); ++i) {
            const double du = shape[i].x - mu, dv = shape[i].y - mv;
            const Vec2d& p = points[i];
            suu += du * du;
            svv += dv * dv;
            sx += p.x;
            sy += p.y;
            xu += p.x * du;
            yu += p.y * du;
            xv += p.x * dv;
            yv += p.y * dv;
            sxx += p.x * p.x;
            syy += p.y * p.y;
            sxy += p.x * p.y;
        }
    }
};

struct FitAt {
    double error, w, h, ox, oy;
};
// The points turned by -angle, each axis a least squares line on the shape's.
FitAt fitAt(double angle, const Fit& f) {
    const double c = std::cos(-angle), s = std::sin(-angle);
    const double mqx = (f.sx * c - f.sy * s) / f.n;
    const double mqy = (f.sx * s + f.sy * c) / f.n;
    // sum((q - mean q)^2) per axis, from the turned second moments.
    const double qxx = c * c * f.sxx - 2.0 * c * s * f.sxy + s * s * f.syy - f.n * mqx * mqx;
    const double qyy = s * s * f.sxx + 2.0 * c * s * f.sxy + c * c * f.syy - f.n * mqy * mqy;
    const double cxu = c * f.xu - s * f.yu;  // sum(du * qx)
    const double cyv = s * f.xv + c * f.yv;  // sum(dv * qy)
    const double w = f.suu > 1e-18 ? std::max(cxu / f.suu, 0.0) : 0.0;
    const double h = f.svv > 1e-18 ? std::max(cyv / f.svv, 0.0) : 0.0;
    const double ex = std::max(qxx - w * cxu, 0.0), ey = std::max(qyy - h * cyv, 0.0);
    return {ex + ey, w, h, mqx - w * f.mu, mqy - h * f.mv};
}

}  // namespace

double Vec3d::length() const { return std::sqrt(dot(*this)); }

Vec3d Vec3d::normalized() const {
    const double n = length();
    return n > 1e-12 ? *this * (1.0 / n) : Vec3d{};
}

// ---- Sections ----------------------------------------------------------------------------------------

Vec3d sectionCenter(const TubeRing& ring) {
    const std::size_t n = ring.size();
    if (n == 0) return {};
    const Vec3d mid = mean(ring);
    Vec3d normal;
    for (std::size_t i = 0; i < n; ++i) {  // Newell's normal
        const Vec3d& a = ring[i];
        const Vec3d& b = ring[(i + 1) % n];
        normal = normal + Vec3d{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
    }
    normal = normal.normalized();
    if (normal == Vec3d{}) return mid;
    double total = 0.0;
    Vec3d acc;
    for (std::size_t i = 0; i < n; ++i) {  // a fan round the mean: each triangle's centroid weighted by its area
        const Vec3d& a = ring[i];
        const Vec3d& b = ring[(i + 1) % n];
        const double area = (a - mid).cross(b - mid).dot(normal) * 0.5;
        total += area;
        const Vec3d corners[3] = {mid, a, b};
        acc = acc + mean(corners, 3) * area;
    }
    if (std::abs(total) < 1e-12) return mid;
    return acc * (1.0 / total);
}

double sectionRadius(const TubeRing& ring) {
    if (ring.empty()) return 0.0;
    const Vec3d c = sectionCenter(ring);
    double sum = 0.0;
    for (const Vec3d& p : ring) sum += dist(p, c);
    return sum / static_cast<double>(ring.size());
}

Vec3d ringPoint(const TubeRing& ring, double turn) {
    const std::size_t n = ring.size();
    if (n == 0) return {};
    std::vector<double> edges(n);
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        edges[i] = dist(ring[i], ring[(i + 1) % n]);
        total += edges[i];
    }
    if (total < 1e-12) return ring[0];
    double share = std::fmod(turn, 1.0);
    if (share < 0.0) share += 1.0;
    double left = share * total;
    for (std::size_t i = 0; i < n; ++i) {
        const double e = edges[i];
        if (left <= e || i == n - 1) return lerp(ring[i], ring[(i + 1) % n], e > 1e-12 ? std::min(1.0, left / e) : 0.0);
        left -= e;
    }
    return ring[0];
}

double volumeReach(int count) { return std::min(kTubeVolumeReach, kTubeVolumeBase + kTubeVolumeSpread * std::sqrt(std::max(1, count))); }

std::vector<std::pair<double, double>> volumeLayout(int count) {
    const int extra = std::max(0, count - 1);
    if (extra == 0) return {};
    int rings = 1;
    while (3 * rings * (rings + 1) < extra) ++rings;
    const double weightSum = static_cast<double>(rings * (rings + 1) / 2);
    std::vector<double> exact;
    std::vector<int> sizes;
    for (int w = 1; w <= rings; ++w) {
        exact.push_back(extra * w / weightSum);
        sizes.push_back(std::max(1, static_cast<int>(exact.back())));
    }
    auto held = [&sizes] { return std::accumulate(sizes.begin(), sizes.end(), 0); };
    while (held() > extra) --*std::max_element(sizes.begin(), sizes.end());  // (each ring kept with at least one)
    std::vector<int> order(static_cast<std::size_t>(rings));
    std::iota(order.begin(), order.end(), 0);
    auto fraction = [&exact](int j) { return exact[static_cast<std::size_t>(j)] - std::floor(exact[static_cast<std::size_t>(j)]); };
    std::stable_sort(order.begin(), order.end(), [&fraction](int a, int b) { return fraction(a) > fraction(b); });
    const int spare = extra - held();
    for (int k = 0; k < spare && k < rings; ++k) ++sizes[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];
    std::vector<std::pair<double, double>> out;
    const double reach = volumeReach(count);
    for (int j = 0; j < rings; ++j) {
        const int size = sizes[static_cast<std::size_t>(j)];
        const double radius = reach * (j + 1) / static_cast<double>(rings);
        const double offset = j % 2 ? 0.5 / size : 0.0;
        for (int i = 0; i < size; ++i) out.emplace_back(radius, offset + i / static_cast<double>(size));
    }
    return out;
}

TubeRing bsplineLoop(const TubeRing& points, int perSpan) {
    const std::size_t n = points.size();
    TubeRing out;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3d& p0 = points[(i + n - 1) % n];
        const Vec3d& p1 = points[i];
        const Vec3d& p2 = points[(i + 1) % n];
        const Vec3d& p3 = points[(i + 2) % n];
        for (int k = 0; k < perSpan; ++k) {
            const double t = k / static_cast<double>(perSpan);
            const double t2 = t * t, t3 = t * t * t;
            const double b0 = std::pow(1.0 - t, 3.0) / 6.0;
            const double b1 = (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
            const double b2 = (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
            const double b3 = t3 / 6.0;
            out.push_back({b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y,
                           b0 * p0.z + b1 * p1.z + b2 * p2.z + b3 * p3.z});
        }
    }
    return out;
}

std::vector<Vec3d> limitPoints(const std::vector<Vec3d>& points, const std::vector<std::vector<int>>& polygons) {
    checkIndices(points.size(), polygons);
    const std::size_t count = points.size();
    std::vector<Vec3d> facePoint;
    facePoint.reserve(polygons.size());
    std::vector<std::vector<int>> vertexFaces(count);
    for (std::size_t f = 0; f < polygons.size(); ++f) {
        std::vector<Vec3d> corners;
        for (const int v : polygons[f]) {
            corners.push_back(points[static_cast<std::size_t>(v)]);
            vertexFaces[static_cast<std::size_t>(v)].push_back(static_cast<int>(f));
        }
        facePoint.push_back(corners.empty() ? Vec3d{} : mean(corners));
    }
    const MeshEdges mesh(polygons);
    std::vector<std::vector<std::size_t>> vertexEdges(count);
    std::vector<Vec3d> edgePoint(mesh.edges.size());
    for (std::size_t e = 0; e < mesh.edges.size(); ++e) {
        const Edge& edge = mesh.edges[e];
        vertexEdges[static_cast<std::size_t>(edge.first)].push_back(e);
        vertexEdges[static_cast<std::size_t>(edge.second)].push_back(e);
        const Vec3d& a = points[static_cast<std::size_t>(edge.first)];
        const Vec3d& b = points[static_cast<std::size_t>(edge.second)];
        const std::vector<int>& faces = mesh.faces[e];
        if (faces.size() == 2) {
            const Vec3d four[4] = {a, b, facePoint[static_cast<std::size_t>(faces[0])], facePoint[static_cast<std::size_t>(faces[1])]};
            edgePoint[e] = mean(four, 4);
        } else {
            const Vec3d two[2] = {a, b};
            edgePoint[e] = mean(two, 2);
        }
    }

    std::vector<Vec3d> out;
    out.reserve(count);
    for (std::size_t v = 0; v < count; ++v) {
        const std::vector<std::size_t>& edges = vertexEdges[v];
        const std::vector<int>& faces = vertexFaces[v];
        const Vec3d& p = points[v];
        if (edges.empty()) {
            out.push_back(p);
            continue;
        }
        std::vector<std::size_t> borders;
        for (const std::size_t e : edges) {
            if (mesh.faces[e].size() == 1) borders.push_back(e);
        }
        if (!borders.empty()) {
            if (borders.size() != 2) {  // (a corner or worse: left where it is)
                out.push_back(p);
                continue;
            }
            Vec3d ends[2];
            for (int k = 0; k < 2; ++k) {
                const Edge& e = mesh.edges[borders[static_cast<std::size_t>(k)]];
                ends[k] = points[static_cast<std::size_t>(static_cast<std::size_t>(e.second) == v ? e.first : e.second)];
            }
            const Vec3d child = ((ends[0] + ends[1]) + p * 6.0) * (1.0 / 8.0);
            out.push_back(((edgePoint[borders[0]] + edgePoint[borders[1]]) + child * 4.0) * (1.0 / 6.0));
            continue;
        }
        const double n = static_cast<double>(edges.size());
        std::vector<Vec3d> around;
        for (const int f : faces) around.push_back(facePoint[static_cast<std::size_t>(f)]);
        const Vec3d q = mean(around);
        around.clear();
        for (const std::size_t e : edges) {
            const Edge& edge = mesh.edges[e];
            const Vec3d two[2] = {points[static_cast<std::size_t>(edge.first)], points[static_cast<std::size_t>(edge.second)]};
            around.push_back(mean(two, 2));
        }
        const Vec3d r = mean(around);
        const Vec3d child = ((q + r * 2.0) + p * (n - 3.0)) * (1.0 / n);
        // After one step every face round the child is a quad: child, edge point, face point, edge point.
        Vec3d sumE, sumF;
        for (const std::size_t e : edges) sumE = sumE + edgePoint[e];
        for (const int f : faces) sumF = sumF + facePoint[static_cast<std::size_t>(f)];
        const Vec3d total = (child * (n * n) + sumE * 4.0) + sumF;
        out.push_back(total * (1.0 / (n * (n + 5.0))));
    }
    return out;
}

// ---- Tubes -------------------------------------------------------------------------------------------

void ModeledTube::reverse() {
    std::reverse(rings.begin(), rings.end());
    if (smoothRings) std::reverse(smoothRings->begin(), smoothRings->end());
    std::swap(pointed, pointedRoot);
}

void ModeledTube::useSmooth() {
    if (!smoothRings) return;
    cageRings = std::move(rings);
    rings = std::move(*smoothRings);
    smoothRings.reset();
}

std::pair<double, double> ModeledTube::endSizes() const {
    if (rings.empty()) return {0.0, 0.0};
    return {sectionRadius(rings.front()), sectionRadius(rings.back())};
}

std::vector<Vec3d> ModeledTube::centers() const {
    std::vector<Vec3d> out;
    out.reserve(rings.size());
    for (const TubeRing& r : rings) out.push_back(sectionCenter(r));
    return out;
}

std::vector<std::vector<Vec3d>> ModeledTube::volumeCurves(int count) const {
    const std::vector<Vec3d> middle = centers();
    std::vector<std::vector<Vec3d>> curves{middle};
    for (const auto& [radius, turn] : volumeLayout(count)) {
        std::vector<Vec3d> curve;
        curve.reserve(rings.size());
        for (std::size_t k = 0; k < rings.size(); ++k) curve.push_back(lerp(middle[k], ringPoint(rings[k], turn), radius));
        curves.push_back(std::move(curve));
    }
    return curves;
}

std::vector<TubeRing> ModeledTube::sectionLoops(int perSpan) const {
    if (!cageRings) return rings;
    const std::vector<TubeRing>& cage = *cageRings;
    const std::size_t n = cage.size();
    std::vector<TubeRing> loops;
    loops.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        const TubeRing& ring = cage[k];
        if (sectionRadius(ring) <= 1e-12) {  // a pointed end: its one point, as often as the others have
            loops.emplace_back(ring.size() * static_cast<std::size_t>(perSpan), rings[k][0]);
            continue;
        }
        if (k == 0 || k == n - 1) {  // an open end: its own border curve
            loops.push_back(bsplineLoop(ring, perSpan));
            continue;
        }
        const TubeRing& below = cage[k - 1];
        const TubeRing& above = cage[k + 1];
        TubeRing control(ring.size());
        for (std::size_t i = 0; i < ring.size(); ++i) {
            control[i] = {(below[i].x + 4.0 * ring[i].x + above[i].x) / 6.0, (below[i].y + 4.0 * ring[i].y + above[i].y) / 6.0,
                          (below[i].z + 4.0 * ring[i].z + above[i].z) / 6.0};
        }
        loops.push_back(bsplineLoop(control, perSpan));
    }
    return loops;
}

std::vector<std::vector<Vec3d>> ModeledTube::columns() const {
    std::vector<std::vector<Vec3d>> out(sides());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i].reserve(rings.size());
        for (const TubeRing& r : rings) out[i].push_back(r[i]);
    }
    return out;
}

std::pair<int, int> ModeledTube::mergeBevels(double ratio) {
    const int loopsBefore = static_cast<int>(rings.size()), runsBefore = static_cast<int>(sides());
    mergeLoops(*this, ratio);
    mergeRuns(*this, ratio);
    const int loops = loopsBefore - static_cast<int>(rings.size()), runs = runsBefore - static_cast<int>(sides());
    mergedLoops += loops;
    mergedRuns += runs;
    return {loops, runs};
}

std::vector<ModeledTube> readTubes(const std::vector<Vec3d>& points, const std::vector<std::vector<int>>& polygons,
                                   const std::string& name, bool smooth) {
    checkIndices(points.size(), polygons);
    const std::vector<Vec3d> smoothPoints = smooth ? limitPoints(points, polygons) : std::vector<Vec3d>{};
    const MeshEdges mesh(polygons);
    for (const std::vector<int>& faces : mesh.faces) {
        if (faces.size() > 2) throw TubeMeshError(name + " has edges shared by more than two faces (non-manifold)");
    }
    std::vector<bool> used(points.size(), false);
    for (const std::vector<int>& poly : polygons) {
        for (const int v : poly) used[static_cast<std::size_t>(v)] = true;
    }
    std::vector<std::vector<int>> nbrs(points.size());
    for (const Edge& e : mesh.edges) {
        nbrs[static_cast<std::size_t>(e.first)].push_back(e.second);
        nbrs[static_cast<std::size_t>(e.second)].push_back(e.first);
    }
    std::vector<std::vector<int>> shells;
    for (std::vector<int>& shell : shellsOf(points.size(), mesh.edges)) {
        if (used[static_cast<std::size_t>(shell.front())]) shells.push_back(std::move(shell));
    }
    std::vector<ModeledTube> tubes;
    for (std::size_t index = 0; index < shells.size(); ++index) {
        const std::vector<int>& shell = shells[index];
        const std::string label = shells.size() == 1 ? name : name + "_" + std::to_string(index + 1);
        const std::unordered_set<int> members(shell.begin(), shell.end());
        std::vector<std::vector<int>> rings;
        bool pointed = false;
        try {
            std::vector<Edge> boundary;
            for (std::size_t e = 0; e < mesh.edges.size(); ++e) {
                if (mesh.faces[e].size() == 1 && members.count(mesh.edges[e].first)) boundary.push_back(mesh.edges[e]);
            }
            const std::vector<std::vector<int>> loops = boundaryLoops(boundary);
            if (loops.empty()) throw TubeMeshError("it is closed (capped): delete its cap faces so its ends are open");
            if (loops.size() > 2) {
                throw TubeMeshError("it has " + std::to_string(loops.size()) +
                                    " open edge loops; a tube has one at each end (or one and a pointed tip)");
            }
            std::vector<std::set<int>> ends;
            for (std::size_t l = 1; l < loops.size(); ++l) ends.emplace_back(loops[l].begin(), loops[l].end());
            rings = walk(loops[0], nbrs, ends, shell.size(), &pointed);
            if (loops.size() == 2 && pointed) throw TubeMeshError("it has an open end and a pointed tip besides its other open end");
            if (rings.size() < 2) throw TubeMeshError("it is a single edge loop");
        } catch (const TubeMeshError& e) {
            throw TubeMeshError(label + ": " + e.what());
        }
        auto ringsOf = [&rings](const std::vector<Vec3d>& from) {
            std::vector<TubeRing> out;
            out.reserve(rings.size());
            for (const std::vector<int>& r : rings) {
                TubeRing ring;
                ring.reserve(r.size());
                for (const int v : r) ring.push_back(from[static_cast<std::size_t>(v)]);
                out.push_back(std::move(ring));
            }
            return out;
        };
        ModeledTube tube;
        tube.rings = ringsOf(points);
        if (smooth) tube.smoothRings = ringsOf(smoothPoints);
        tube.name = label;
        tube.pointed = pointed;
        tube.shell = static_cast<int>(index);
        tube.readLoops = static_cast<int>(tube.rings.size());
        tube.readSides = static_cast<int>(tube.sides());
        tubes.push_back(std::move(tube));
    }
    return tubes;
}

bool orientTube(ModeledTube& tube, const std::function<double(const Vec3d&)>& rootDistance) {
    tube.flipped = false;
    if (tube.rings.empty() || (tube.pointed && !rootDistance)) return false;
    if (rootDistance) {
        const Vec3d a = sectionCenter(tube.rings.front()), b = sectionCenter(tube.rings.back());
        if (rootDistance(b) < rootDistance(a)) tube.flipped = true;
    } else {
        const auto [root, tip] = tube.endSizes();
        if (tip > root) tube.flipped = true;
    }
    if (tube.flipped) tube.reverse();
    return tube.flipped;
}

void prepareTube(ModeledTube& tube, bool ignoreBevels, const std::function<double(const Vec3d&)>& rootDistance) {
    orientTube(tube, rootDistance);
    if (ignoreBevels) tube.mergeBevels();
    tube.useSmooth();
}

// ---- Guides from tubes --------------------------------------------------------------------------------

std::vector<Vec3d> rootedCurve(const std::vector<Vec3d>& points, const Vec3d& rootOnScalp) {
    if (points.size() < 2) return points;
    const Vec3d move = rootOnScalp - points[0];
    std::vector<double> along{0.0};
    for (std::size_t i = 1; i < points.size(); ++i) along.push_back(along.back() + dist(points[i - 1], points[i]));
    const double fade = kTubeRootFade * along.back();
    if (fade <= 1e-12) return points;
    std::vector<Vec3d> out;
    out.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double k = std::max(0.0, 1.0 - along[i] / fade);
        out.push_back(points[i] + move * (k * k * (3.0 - 2.0 * k)));  // smooth to nothing
    }
    return out;
}

std::vector<Vec3d> tubeGuidePath(const std::vector<Vec3d>& centers, const Vec3d& root, const Vec3d& normal) {
    if (centers.size() < 2) return centers;
    const double step = dist(centers[1], root);
    std::vector<Vec3d> out{root, {root.x + normal.x * kTubeRootLift * step, root.y + normal.y * kTubeRootLift * step,
                                  root.z + normal.z * kTubeRootLift * step}};
    out.insert(out.end(), centers.begin() + 1, centers.end());
    return out;
}

// Python's round(value, 4): the value as it is exactly (not its product with 10^4 as a double
// rounds it), a half to the even digit.
double roundedTubeRegionValue(double value) {
    const double scaled = value * 1e4;
    const double rest = std::fma(value, 1e4, -scaled);  // value * 10^4 is exactly scaled + rest
    const double below = std::floor(scaled);
    const double past = scaled - below - 0.5;  // (exact)
    const bool up = past != 0.0 ? past > 0.0 : rest != 0.0 ? rest > 0.0 : std::fmod(below, 2.0) != 0.0;
    return (up ? below + 1.0 : below) / 1e4;
}

std::vector<double> tubeRegionValues(int count) {
    if (count <= 0) return {};
    std::vector<double> values;
    for (int k = 0; k < count; ++k) values.push_back(roundedTubeRegionValue((k + 1.0) / (count + 1.0)));
    int step = std::max(1, static_cast<int>(roundHalfEven(count * 0.381966)));
    while (std::gcd(step, count) != 1) ++step;
    std::vector<double> out;
    for (int t = 0; t < count; ++t) {
        out.push_back(values[static_cast<std::size_t>((static_cast<long long>(t) * step) % count)]);
    }
    return out;
}

std::vector<double> extraTubeRegionValues(const std::vector<double>& used, int count) {
    if (count <= 0) return {};
    for (int n = static_cast<int>(used.size()) + count;; ++n) {
        const double gap = 0.5 / (n + 1.0);
        std::vector<double> clear;
        for (const double v : tubeRegionValues(n)) {
            if (std::all_of(used.begin(), used.end(), [v, gap](double u) { return std::abs(v - u) > gap; })) clear.push_back(v);
        }
        if (clear.size() >= static_cast<std::size_t>(count)) {
            clear.resize(static_cast<std::size_t>(count));
            return clear;
        }
    }
}

bool isTubeMapName(const std::string& name) {
    const std::string base = kTubeRegionMap;
    if (name.compare(0, base.size(), base) != 0) return false;
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(base.size()), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string unusedName(const std::string& base, const std::vector<std::string>& taken) {
    std::string name = base;
    for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++n) name = base + std::to_string(n);
    return name;
}

std::string tubeShapeNameFor(const std::string& base, const std::vector<std::string>& taken, const std::vector<std::string>& builtin) {
    auto lower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    std::string clean;
    for (const char c : base) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) == 0x80) continue;  // (the rest of a character of several bytes: one '_' for it)
        const bool kept = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' ||
                          c == '-';
        clean.push_back(kept ? c : '_');
    }
    const std::size_t first = clean.find_first_not_of(' ');
    clean = first == std::string::npos ? std::string() : clean.substr(first, clean.find_last_not_of(' ') - first + 1);
    if (clean.empty()) clean = "tube";
    clean = clean.substr(0, 56);
    for (const std::string& b : builtin) {
        if (lower(b) == lower(clean)) {
            clean += "_shape";
            break;
        }
    }
    std::string name = clean;
    for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++n) name = clean + "_" + std::to_string(n);
    return name;
}

// ---- Fitting GroomLab sections -----------------------------------------------------------------------

double signedArea(const std::vector<Vec2d>& points) {
    double a = 0.0;
    const std::size_t n = points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2d& p = points[i];
        const Vec2d& q = points[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

SectionFit fitSection(const std::vector<Vec2d>& points, const std::vector<Vec2d>& shape, const double* nearTwist) {
    if (points.empty()) return {nearTwist ? *nearTwist : 0.0, 0.0, 0.0, {}};
    double spread = 0.0, mx = 0.0, my = 0.0;
    for (const Vec2d& p : points) {
        spread = std::max(spread, std::hypot(p.x, p.y));
        mx += p.x;
        my += p.y;
    }
    mx /= static_cast<double>(points.size());
    my /= static_cast<double>(points.size());
    double most = 0.0;
    for (const Vec2d& p : points) most = std::max(most, std::hypot(p.x - mx, p.y - my));
    if (most < 1e-9 * std::max(spread, 1.0)) return {nearTwist ? *nearTwist : 0.0, 0.0, 0.0, {mx, my}};
    const Fit fit(points, shape);
    const int steps = 120;
    const double step = 2.0 * kPi / steps;
    double bestAngle = 0.0, bestError = 0.0;
    for (int i = 0; i < steps; ++i) {
        const FitAt f = fitAt(step * i, fit);
        if (i == 0 || f.error < bestError) bestError = f.error, bestAngle = step * i;
    }
    double lo = bestAngle - step, hi = bestAngle + step;
    const double golden = 0.5 * (std::sqrt(5.0) - 1.0);
    for (int it = 0; it < 30; ++it) {  // golden section search round the best step
        const double a1 = hi - golden * (hi - lo), a2 = lo + golden * (hi - lo);
        if (fitAt(a1, fit).error < fitAt(a2, fit).error) {
            hi = a2;
        } else {
            lo = a1;
        }
    }
    double angle = 0.5 * (lo + hi);
    const FitAt f = fitAt(angle, fit);
    if (nearTwist) angle += 2.0 * kPi * roundHalfEven((*nearTwist - angle) / (2.0 * kPi));
    const double c = std::cos(angle), s = std::sin(angle);
    return {angle, f.w, f.h, {f.ox * c - f.oy * s, f.ox * s + f.oy * c}};
}

std::vector<Vec2d> shapePoints(const std::vector<Vec2d>& points, std::vector<std::size_t>* indices, std::size_t limit) {
    const std::size_t n = points.size();
    std::vector<std::size_t> picked(n);
    std::iota(picked.begin(), picked.end(), std::size_t{0});
    if (n > limit) {
        std::set<std::size_t> evenly;
        for (std::size_t i = 0; i < limit; ++i) {
            evenly.insert(static_cast<std::size_t>(roundHalfEven(static_cast<double>(i * n) / static_cast<double>(limit))) % n);
        }
        picked.assign(evenly.begin(), evenly.end());
    }
    std::vector<Vec2d> pts;
    pts.reserve(picked.size());
    for (const std::size_t i : picked) pts.push_back(points[i]);
    if (signedArea(pts) < 0.0) {
        std::reverse(picked.begin(), picked.end());
        std::reverse(pts.begin(), pts.end());
    }
    if (indices) *indices = std::move(picked);
    return pts;
}

std::vector<Vec2d> matchFitted(const std::vector<Vec2d>& given, const std::vector<Vec2d>& fitted) {
    const std::size_t n = given.size();
    if (fitted.size() != n || n == 0) return {};
    std::vector<double> gx(n), gy(n), cx(n), cy(n);
    for (std::size_t i = 0; i < n; ++i) gx[i] = given[i].x, gy[i] = given[i].y;
    bool any = false;
    double best = 0.0;
    std::vector<Vec2d> match;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<Vec2d> order = fitted;
        if (pass == 1) std::reverse(order.begin(), order.end());
        for (std::size_t shift = 0; shift < n; ++shift) {
            for (std::size_t i = 0; i < n; ++i) cx[i] = order[(i + shift) % n].x, cy[i] = order[(i + shift) % n].y;
            const double err = regressError(gx, cx) + regressError(gy, cy);
            if (!any || err < best) {
                any = true;
                best = err;
                match.resize(n);
                for (std::size_t i = 0; i < n; ++i) match[i] = order[(i + shift) % n];
            }
        }
    }
    return best < 1e-4 * static_cast<double>(n) ? match : std::vector<Vec2d>{};
}

std::vector<Vec2d> placeOutline(const std::vector<Vec2d>& outline, double twist, double width, double height, const Vec2d& center) {
    const double c = std::cos(twist), s = std::sin(twist);
    std::vector<Vec2d> out;
    out.reserve(outline.size());
    for (const Vec2d& p : outline) {
        out.push_back({center.x + p.x * width * c - p.y * height * s, center.y + p.x * width * s + p.y * height * c});
    }
    return out;
}

std::pair<Vec2d, double> closestOnOutline(const Vec2d& q, const std::vector<Vec2d>& placed, const std::vector<Vec2d>& outline) {
    const std::size_t n = std::min(placed.size(), outline.size());
    bool any = false;
    double best = 0.0;
    Vec2d at;
    for (std::size_t j = 0; j < n; ++j) {
        const Vec2d& a = placed[j];
        const Vec2d& b = placed[(j + 1) % n];
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double length = ex * ex + ey * ey;
        const double f = length > 1e-24 ? std::max(0.0, std::min(1.0, ((q.x - a.x) * ex + (q.y - a.y) * ey) / length)) : 0.0;
        const double d = std::hypot(a.x + ex * f - q.x, a.y + ey * f - q.y);
        if (!any || d < best) {
            const Vec2d& oa = outline[j];
            const Vec2d& ob = outline[(j + 1) % n];
            any = true;
            best = d;
            at = {oa.x + (ob.x - oa.x) * f, oa.y + (ob.y - oa.y) * f};
        }
    }
    return {at, best};
}

TubeRing planeSection(const std::vector<std::vector<Vec3d>>& columns, const Vec3d& origin, const Vec3d& normal) {
    TubeRing out;
    out.reserve(columns.size());
    for (const std::vector<Vec3d>& col : columns) {
        if (col.empty()) {
            out.push_back(origin);
            continue;
        }
        std::vector<double> ds;
        ds.reserve(col.size());
        for (const Vec3d& p : col) ds.push_back((p - origin).dot(normal));
        bool any = false;
        double best = 0.0;
        Vec3d at;
        for (std::size_t k = 0; k + 1 < col.size(); ++k) {
            const double a = ds[k], b = ds[k + 1];
            if ((a <= 0.0 && 0.0 <= b) || (b <= 0.0 && 0.0 <= a)) {
                const double f = a != b ? a / (a - b) : 0.0;
                const Vec3d q = lerp(col[k], col[k + 1], f);
                const double d = dist(q, origin);
                if (!any || d < best) any = true, best = d, at = q;
            }
        }
        if (!any) {
            std::size_t k = 0;
            for (std::size_t i = 1; i < col.size(); ++i) {
                if (std::abs(ds[i]) < std::abs(ds[k])) k = i;
            }
            at = col[k];
        }
        out.push_back(at);
    }
    return out;
}

std::optional<double> rayReach(const std::vector<Vec2d>& polygon, double angle) {
    const double dx = std::cos(angle), dy = std::sin(angle);
    std::optional<double> best;
    const std::size_t n = polygon.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2d& a = polygon[i];
        const Vec2d& b = polygon[(i + 1) % n];
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double den = dx * ey - dy * ex;
        if (std::abs(den) < 1e-15) continue;
        const double s = (a.x * ey - a.y * ex) / den;  // along the ray
        const double f = (a.x * dy - a.y * dx) / den;  // along the edge
        if (s > 0.0 && -1e-9 <= f && f <= 1.0 + 1e-9 && (!best || s > *best)) best = s;
    }
    return best;
}

double coverScale(const std::vector<Vec2d>& section, const std::vector<Vec2d>& outline, double uncovered, double least, double most) {
    std::vector<double> ratios;
    for (int j = 0; j < kTubeCoverDirections; ++j) {
        const double a = 2.0 * kPi * j / kTubeCoverDirections;
        const std::optional<double> there = rayReach(section, a), drawn = rayReach(outline, a);
        if (there && drawn && *there != 0.0 && *drawn != 0.0) ratios.push_back(*there / *drawn);
    }
    if (ratios.empty()) return least;
    double total = 0.0;
    for (const double r : ratios) total += r * r;
    auto outside = [&ratios](double k) {
        double sum = 0.0;
        for (const double r : ratios) sum += std::max(0.0, r * r - k * k);
        return sum;
    };
    double lo = least, hi = most;
    if (outside(lo) <= uncovered * total) return lo;
    for (int it = 0; it < 30; ++it) {
        const double k = 0.5 * (lo + hi);
        if (outside(k) > uncovered * total) {
            lo = k;
        } else {
            hi = k;
        }
    }
    return hi;
}

SectionFit sectionBox(const std::vector<Vec2d>& points, double twist) {
    if (points.empty()) return {twist, 0.0, 0.0, {}};
    const double c = std::cos(twist), s = std::sin(twist);
    double loX = 0.0, hiX = 0.0, loY = 0.0, hiY = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double x = points[i].x * c + points[i].y * s, y = -points[i].x * s + points[i].y * c;
        if (i == 0) loX = hiX = x, loY = hiY = y;
        loX = std::min(loX, x), hiX = std::max(hiX, x), loY = std::min(loY, y), hiY = std::max(hiY, y);
    }
    const double mx = 0.5 * (hiX + loX), my = 0.5 * (hiY + loY);
    return {twist, 0.5 * (hiX - loX), 0.5 * (hiY - loY), {mx * c - my * s, mx * s + my * c}};
}

double polylineParam(const std::vector<Vec3d>& points, const Vec3d& p) {
    const std::size_t n = points.size();
    bool any = false;
    double best = 0.0, param = 0.0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const Vec3d& a = points[i];
        const Vec3d ab = points[i + 1] - a;
        const double span = ab.dot(ab);
        const double f = span > 1e-18 ? std::min(std::max((p - a).dot(ab) / span, 0.0), 1.0) : 0.0;
        const double d = dist(p, a + ab * f);
        if (!any || d < best) any = true, best = d, param = (static_cast<double>(i) + f) / static_cast<double>(n - 1);
    }
    return param;
}

TubeFrame frameAt(const TubeFrames& frames, double t) {
    const std::size_t n = frames.points.size();
    if (n < 2 || frames.u.size() < n || frames.v.size() < n) {
        throw std::invalid_argument("a guide's frames: 2 or more points, an axis pair each");
    }
    const double x = std::min(std::max(t, 0.0), 1.0) * static_cast<double>(n - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(x), n - 2);
    const double f = x - static_cast<double>(i);
    const Vec3d center = lerp(frames.points[i], frames.points[i + 1], f);
    const Vec3d u = lerp(frames.u[i], frames.u[i + 1], f).normalized();
    Vec3d v = lerp(frames.v[i], frames.v[i + 1], f);
    v = (v - u * v.dot(u)).normalized();
    return {center, u, v};
}

std::vector<std::vector<Vec3d>> tubeSurfaceColumns(const ModeledTube& tube, int* perVertex) {
    const int per = tube.smooth() ? kTubeShapeSamples : 1;
    if (perVertex) *perVertex = per;
    const std::vector<TubeRing> loops = tube.sectionLoops(per);
    std::vector<std::vector<Vec3d>> out(loops.empty() ? 0 : loops.front().size());
    for (std::size_t j = 0; j < out.size(); ++j) {
        out[j].reserve(loops.size());
        for (const TubeRing& loop : loops) out[j].push_back(loop[j]);
    }
    return out;
}

TubeRootShape tubeRootShape(const ModeledTube& tube, const TubeFrames& frames, double t) {
    if (tube.rings.empty()) throw std::invalid_argument("a tube with no rings has no root shape");
    const TubeFrame at = frameAt(frames, t);
    std::vector<Vec2d> flat;
    flat.reserve(tube.rings.front().size());
    for (const Vec3d& p : tube.rings.front()) flat.push_back(toPlane(p, at.center, at.u, at.v));
    TubeRootShape out;
    out.points = shapePoints(flat, &out.vertices);
    out.smooth = tube.smooth();
    return out;
}

std::vector<TubeSectionFit> fitTubeSections(const TubeFrames& frames, const ModeledTube& tube, const std::vector<Vec2d>& shapeUv,
                                            const std::vector<std::size_t>& vertices, std::size_t rootPoints,
                                            const std::vector<Vec2d>& outline, const std::optional<double>& uncovered) {
    const std::size_t n = frames.points.size();
    if (n < 2) return {};
    int per = 1;
    const std::vector<std::vector<Vec3d>> surface = tubeSurfaceColumns(tube, &per);
    for (const std::size_t k : vertices) {
        if (k * static_cast<std::size_t>(per) >= surface.size()) throw std::invalid_argument("a shape point's vertex is not on the tube");
    }
    std::vector<TubeSectionFit> sections;
    sections.reserve(n);
    double last = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const TubeFrame at = frameAt(frames, t);
        TubeRing cut;
        if (i < rootPoints) {
            for (const std::vector<Vec3d>& col : surface) cut.push_back(col.front());
        } else {
            cut = planeSection(surface, at.center, at.u.cross(at.v).normalized());
        }
        std::vector<Vec2d> flat;
        flat.reserve(cut.size());
        for (const Vec3d& p : cut) flat.push_back(toPlane(p, at.center, at.u, at.v));
        std::vector<Vec2d> own;
        own.reserve(vertices.size());
        for (const std::size_t k : vertices) own.push_back(flat[k * static_cast<std::size_t>(per)]);
        SectionFit fit = fitSection(own, shapeUv, &last);
        const bool past = i >= rootPoints && !outline.empty();
        if (fit.width > 0.0) {
            fit = sectionBox(flat, fit.twist);
            if (past && fit.width > 0.0 && fit.height > 0.0) {
                const double held = std::abs(signedArea(outline)) * fit.width * fit.height;
                const double cutArea = std::abs(signedArea(flat));
                if (held > 1e-18 && cutArea > 0.0) {
                    const double k = std::sqrt(cutArea / held);
                    fit.width *= k;
                    fit.height *= k;
                }
            }
        }
        double cover = 1.0;
        if (uncovered && past && fit.width > 0.0 && fit.height > 0.0) {
            const std::vector<Vec2d> drawn = placeOutline(outline, fit.twist, fit.width, fit.height);
            std::vector<Vec2d> about;
            about.reserve(flat.size());
            for (const Vec2d& p : flat) about.push_back({p.x - fit.center.x, p.y - fit.center.y});
            cover = coverScale(about, drawn, *uncovered);
        }
        last = fit.twist;
        sections.push_back({t, fit.twist, fit.width, fit.height, fromPlane(fit.center, at.center, at.u, at.v), cover});
    }
    return sections;
}

std::vector<double> smoothedCover(const std::vector<TubeSectionFit>& sections, std::size_t keep) {
    const std::size_t n = sections.size();
    std::vector<double> k(n);
    for (std::size_t i = 0; i < n; ++i) k[i] = i < keep ? 1.0 : sections[i].cover;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<double> next(n);
        for (std::size_t i = 0; i < n; ++i) {
            next[i] = i < keep ? k[i] : (k[i > 0 ? i - 1 : 0] + 2.0 * k[i] + k[std::min(i + 1, n - 1)]) * 0.25;
        }
        k = std::move(next);
    }
    for (double& f : k) f = std::max(1.0, f);
    return k;
}

std::vector<double> sectionAspects(const std::vector<TubeSectionFit>& sections) {
    std::vector<double> out;
    out.reserve(sections.size());
    double prev = 1.0;
    for (const TubeSectionFit& s : sections) {
        const double a = s.width > 1e-9 ? s.height / s.width : prev;
        prev = std::min(std::max(a, 0.01), 100.0);
        out.push_back(prev);
    }
    return out;
}

}  // namespace groomlab
