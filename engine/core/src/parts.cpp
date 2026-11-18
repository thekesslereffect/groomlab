// SPDX-License-Identifier: Apache-2.0
#include "groomlab/parts.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/subdivision.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

// Face normal of the root's triangle on the rest pose (the current smooth normal without one).
Vec3f restNormal(const ScalpMesh& scalp, const StrandRoot& r) {
    if (!scalp.hasRest()) return scalp.normal(r);
    const auto& t = scalp.triangles()[static_cast<std::size_t>(r.face)];
    const auto& p = scalp.restPoints();
    const Vec3f n = (p[t[1]] - p[t[0]]).cross(p[t[2]] - p[t[0]]);
    return n.length() > 0.0f ? n.normalized() : scalp.normal(r);
}


}  // namespace

Part PartSet::fromPoints(const std::string& name, const std::vector<Vec3f>& points, const ScalpMesh& scalp) {
    Part part;
    part.name = name;
    for (const Vec3f& p : points) {
        const SurfacePoint sp = scalp.closestPoint(p);
        if (sp.root.face >= 0) part.points.push_back(sp.root);
    }
    if (part.points.size() < 2) throw std::invalid_argument("a part needs at least two points on the scalp");
    return part;
}

const Part* PartSet::find(const std::string& name) const {
    for (const Part& p : parts) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

std::uint64_t PartSet::contentHash() const {
    std::uint64_t h = hash64(parts.size());
    for (const Part& p : parts) {
        for (unsigned char c : p.name) h = hashCombine(h, c);
        for (const StrandRoot& r : p.points) {
            h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(r.face)));
            h = hashFloat(hashFloat(h, r.u), r.v);
        }
    }
    return h;
}

std::string PartSet::toJson() const {
    json out = json::array();
    for (const Part& p : parts) {
        json pts = json::array();
        for (const StrandRoot& r : p.points) pts.push_back({r.face, r.u, r.v});
        out.push_back({{"name", p.name}, {"points", pts}});
    }
    return json{{"version", 1}, {"parts", out}}.dump();
}

std::string PartSet::add(const std::string& base, const std::vector<Vec3f>& points, const ScalpMesh& scalp,
                          std::size_t samples) {
    if (points.size() < 2 || arcLength(points.data(), points.size()) < 1e-5f) {
        throw std::invalid_argument("a part needs a longer stroke");
    }
    std::string name = base.empty() ? "part" : base;
    for (int i = 1; find(name); ++i) name = (base.empty() ? "part" : base) + std::to_string(i);
    const std::size_t count = std::clamp<std::size_t>(points.size(), 2, std::max<std::size_t>(samples, 2));
    parts.push_back(fromPoints(name, resampleByArcLength(points.data(), points.size(), count), scalp));
    return name;
}

PartSet PartSet::onRefined(const SubdivisionPlan& plan) const {
    PartSet out = *this;
    for (Part& part : out.parts) {
        for (StrandRoot& r : part.points) r = plan.refinedRoot(r);
    }
    return out;
}

PartSet PartSet::onCage(const SubdivisionPlan& plan) const {
    PartSet out = *this;
    for (Part& part : out.parts) {
        for (StrandRoot& r : part.points) r = plan.cageRoot(r);
    }
    return out;
}

PartSet PartSet::transferred(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit) const {
    return transferred(ScalpMapping(from, to, fit));
}

PartSet PartSet::transferred(const ScalpMapping& mapping) const {
    const ScalpMesh& from = mapping.from();
    const ScalpMesh& to = mapping.to();
    if (to.triangleCount() == 0) throw std::invalid_argument("transfer: the new scalp has no faces");
    PartSet out;
    for (const Part& part : parts) {
        Part moved{part.name, {}};
        moved.points.reserve(part.points.size());
        for (const StrandRoot& r : part.points) {
            if (!from.contains(r)) continue;
            const StrandRoot root = mapping.byNormals() ? to.closestPoint(mapping.fit().apply(from.position(r))).root
                                                        : mapping.map(r).target.root;
            if (root.face >= 0) moved.points.push_back(root);
        }
        if (moved.points.size() >= 2) out.parts.push_back(std::move(moved));
    }
    return out;
}

PartSet PartSet::fromJson(const std::string& text) {
    PartSet set;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return set;
    try {
        const json root = json::parse(text);
        if (root.value("version", 1) > 1) throw std::runtime_error("part data version is newer than this GroomLab supports");
        const json parts = root.value("parts", json::array());
        for (const json& p : parts) {
            Part part;
            part.name = p.at("name").get<std::string>();
            for (const json& r : p.at("points")) {
                part.points.push_back({r.at(0).get<std::int32_t>(), r.at(1).get<float>(), r.at(2).get<float>()});
            }
            set.parts.push_back(std::move(part));
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("invalid part data: ") + e.what());
    }
    return set;
}

PartLines::PartLines(const PartSet& set, const ScalpMesh& scalp) {
    hash_ = hashCombine(set.contentHash(), scalp.surfaceHash());  // color sets play no part
    sideHash_ = hashCombine(hashCombine(set.contentHash(), 0x51DEu), scalp.restSurfaceHash());
    for (const Part& part : set.parts) {
        std::vector<Vec3f> rest, normals, current;
        for (const StrandRoot& r : part.points) {
            if (!scalp.contains(r)) continue;
            const Vec3f p = scalp.position(r);
            current.push_back(p);
            rest.push_back(scalp.restPositionOr(r, p));
            normals.push_back(restNormal(scalp, r));
        }
        if (rest.size() < 2) continue;
        for (const Vec3f& p : rest) extent_ = std::max({extent_, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
        std::vector<SegmentNode> tree;
        buildTree(tree, rest, 0, static_cast<std::uint32_t>(rest.size() - 1));
        trees_.push_back(std::move(tree));
        names_.push_back(part.name);
        rest_.push_back(std::move(rest));
        restNormals_.push_back(std::move(normals));
        current_.push_back(std::move(current));
    }
}

std::uint32_t PartLines::buildTree(std::vector<SegmentNode>& tree, const std::vector<Vec3f>& line, std::uint32_t first,
                                   std::uint32_t last) {
    constexpr std::uint32_t kLeafSegments = 4;
    const auto index = static_cast<std::uint32_t>(tree.size());
    tree.emplace_back();
    Vec3f lo = line[first], hi = lo;
    for (std::uint32_t i = first + 1; i <= last; ++i) {  // the segments' end points
        const Vec3f& p = line[i];
        lo = minPerAxis(lo, p);
        hi = maxPerAxis(hi, p);
    }
    tree[index].lo = lo;
    tree[index].hi = hi;
    tree[index].first = first;
    tree[index].last = last;
    if (last - first > kLeafSegments) {
        const std::uint32_t mid = first + (last - first) / 2;
        const std::uint32_t left = buildTree(tree, line, first, mid);
        const std::uint32_t right = buildTree(tree, line, mid, last);
        tree[index].left = left;
        tree[index].right = right;
    }
    return index;
}

PartLines::Side PartLines::sideOf(const Vec3f& q, const std::string& only) const {
    Side best;
    float bestDistance = std::numeric_limits<float>::max();
    float bestT = 0.0f;
    // A box is skipped only when it is farther than the best segment by more than rounding could
    // account for (the segments' distances are rounded on their own path), so every segment that
    // could tie or win is measured. The winner is the nearest; among equals, the first part's
    // first segment, as a scan of every segment in order picks.
    const float slack = 1e-5f * (1.0f + std::max({extent_, std::abs(q.x), std::abs(q.y), std::abs(q.z)}));
    auto boxDistance = [&](const SegmentNode& n) {
        const float dx = std::max({n.lo.x - q.x, 0.0f, q.x - n.hi.x});
        const float dy = std::max({n.lo.y - q.y, 0.0f, q.y - n.hi.y});
        const float dz = std::max({n.lo.z - q.z, 0.0f, q.z - n.hi.z});
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    for (std::size_t p = 0; p < rest_.size(); ++p) {
        if (!only.empty() && names_[p] != only) continue;
        const auto& line = rest_[p];
        const auto& tree = trees_[p];
        std::uint32_t stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const SegmentNode& node = tree[stack[--top]];
            if (boxDistance(node) > bestDistance + slack) continue;
            if (node.left != 0 && top + 2 <= 64) {
                // The nearer child first (pushed last).
                const bool leftFirst = boxDistance(tree[node.left]) <= boxDistance(tree[node.right]);
                stack[top++] = leftFirst ? node.right : node.left;
                stack[top++] = leftFirst ? node.left : node.right;
                continue;
            }
            for (std::size_t i = node.first; i < node.last; ++i) {
                const Vec3f a = line[i], ab = line[i + 1] - a;
                const float len2 = ab.dot(ab);
                const float t = len2 > 0.0f ? std::clamp((q - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
                const float d = (q - (a + ab * t)).length();
                const bool earlier = static_cast<int>(p) < best.part || (static_cast<int>(p) == best.part && i < best.segment);
                if (d < bestDistance || (d == bestDistance && best.part >= 0 && earlier)) {
                    bestDistance = d;
                    best.part = static_cast<int>(p);
                    best.segment = i;
                    bestT = t;
                }
            }
        }
    }
    if (best.part < 0) return best;
    best.distance = bestDistance;
    const auto& line = rest_[static_cast<std::size_t>(best.part)];
    const auto& normals = restNormals_[static_cast<std::size_t>(best.part)];
    // Past either end of the line: no side, hair flows around the end.
    const bool atStart = best.segment == 0 && bestT <= 0.0f;
    const bool atEnd = best.segment + 2 == line.size() && bestT >= 1.0f;
    if (atStart || atEnd) return best;
    const Vec3f a = line[best.segment], b = line[best.segment + 1];
    const Vec3f n = (normals[best.segment] * (1.0f - bestT) + normals[best.segment + 1] * bestT).normalized();
    const Vec3f across = (b - a).cross(n);
    const Vec3f nearest = a + (b - a) * bestT;
    const float s = (q - nearest).dot(across);
    best.side = s > 0.0f ? 1 : (s < 0.0f ? -1 : 0);
    return best;
}

Vec3f PartLines::awayFrom(const Side& side, const Vec3f& currentNormal) const {
    if (side.part < 0 || side.side == 0) return {};
    const auto& line = current_[static_cast<std::size_t>(side.part)];
    const Vec3f tangent = line[side.segment + 1] - line[side.segment];
    const Vec3f across = tangent.cross(currentNormal);
    return across.length() > 0.0f ? across.normalized() * static_cast<float>(side.side) : Vec3f{};
}

// ---- Auto parts, found where the hair splits --------------------------------------------------------------

namespace {

constexpr float kFlowRadius = 0.05f;         // R: a share of the scalp's size (sqrt of its area)
constexpr float kFlowRadiusSpacings = 3.0f;  // R at least this many root spacings
constexpr float kFlowCoherence = 0.2f;       // each side's mean direction at least this long
constexpr float kFlowGuard = 0.25f;          // the band either side of a crossing left out of its sides, x R
constexpr int kFlowSmoothPasses = 4;

// Points bucketed in cubic cells of one size from a corner (integer cell coordinates, so a groom
// k times as big buckets alike), sorted by cell: the points in a cell are a contiguous run.
class CellGrid {
public:
    CellGrid(const std::vector<Vec3f>& points, const Vec3f& lo, const Vec3f& hi, float cell) : lo_(lo), cell_(cell) {
        for (int a = 0; a < 3; ++a) {
            const float extent = a == 0 ? hi.x - lo.x : (a == 1 ? hi.y - lo.y : hi.z - lo.z);
            dims_[a] = static_cast<std::int64_t>(std::floor(std::max(extent, 0.0f) / cell)) + 1;
        }
        entries_.reserve(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) entries_.push_back({keyOf(points[i]), static_cast<std::uint32_t>(i)});
        std::sort(entries_.begin(), entries_.end());
    }
    std::uint64_t keyOf(const Vec3f& p) const {
        std::int64_t c[3];
        coords(p, c);
        return key(c);
    }
    // The sorted (cell, point) pairs: runs of one cell.
    const std::vector<std::pair<std::uint64_t, std::uint32_t>>& entries() const { return entries_; }
    // Every point in the 27 cells around q's (all within one cell of q's are there).
    template <typename Fn>
    void forEachNear(const Vec3f& q, const Fn& fn) const {
        std::int64_t c[3];
        coords(q, c);
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const std::int64_t n[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
                    if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= dims_[0] || n[1] >= dims_[1] || n[2] >= dims_[2]) continue;
                    const std::uint64_t k = key(n);
                    auto it = std::lower_bound(entries_.begin(), entries_.end(), std::make_pair(k, std::uint32_t{0}));
                    for (; it != entries_.end() && it->first == k; ++it) fn(it->second);
                }
            }
        }
    }

private:
    void coords(const Vec3f& p, std::int64_t* c) const {
        const float v[3] = {p.x - lo_.x, p.y - lo_.y, p.z - lo_.z};
        for (int a = 0; a < 3; ++a) {
            c[a] = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(v[a] / cell_)), 0, dims_[a] - 1);
        }
    }
    std::uint64_t key(const std::int64_t* c) const {
        return static_cast<std::uint64_t>((c[0] * dims_[1] + c[1]) * dims_[2] + c[2]);
    }

    Vec3f lo_;
    float cell_;
    std::int64_t dims_[3] = {1, 1, 1};
    std::vector<std::pair<std::uint64_t, std::uint32_t>> entries_;
};

Vec3f tangential(const Vec3f& v, const Vec3f& n) { return v - n * n.dot(v); }

}  // namespace

PartSet findAutoParts(const StrandSet& strands, const ScalpMesh& scalp, float splitAngleDegrees, float minLength) {
    PartSet out;
    const auto& points = scalp.points();
    const auto& triangles = scalp.triangles();
    const auto& normals = scalp.vertexNormals();
    const float area = scalp.totalArea();
    if (points.empty() || triangles.empty() || !(area > 0.0f) || normals.size() != points.size()) return out;
    const float size = std::sqrt(area);

    // 1. Each strand's direction on the scalp, spread to its root triangle's corners.
    std::vector<Vec3f> flow(points.size());
    std::vector<float> weight(points.size(), 0.0f);
    std::size_t rooted = 0;
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const StrandRoot& root = strands.roots()[s];
        if (strands.strandPointCount(s) < 2 || !scalp.contains(root)) continue;
        const Vec3f* p = strands.strandPoints(s);
        const Vec3f seg = p[1] - p[0];
        const float len = seg.length();
        if (!(len > 0.0f)) continue;
        const Vec3f along = tangential(seg, scalp.normal(root)) / len;  // the share of it along the scalp
        const auto& t = triangles[static_cast<std::size_t>(root.face)];
        const float w[3] = {1.0f - root.u - root.v, root.u, root.v};
        for (int k = 0; k < 3; ++k) {
            const auto v = static_cast<std::size_t>(t[k]);
            flow[v] += along * w[k];
            weight[v] += w[k];
        }
        ++rooted;
    }
    if (rooted == 0) return out;
    const float radius = std::max(kFlowRadius * size, kFlowRadiusSpacings * std::sqrt(area / static_cast<float>(rooted)));
    const float r2 = radius * radius;
    auto kernel = [&](float d2) {
        if (!(d2 < r2)) return 0.0f;
        const float x = 1.0f - d2 / r2;
        return x * x;
    };

    // The vertices with hair gathered into cells of an eighth of R: each cell's weighted center,
    // summed direction, weight and mean normal.
    Vec3f lo = points[0], hi = points[0];
    for (const Vec3f& p : points) {
        lo = minPerAxis(lo, p);
        hi = maxPerAxis(hi, p);
    }
    struct Cell {
        Vec3f center, flow, normal;
        float weight = 0.0f;
    };
    std::vector<Cell> cells;
    {
        const CellGrid fine(points, lo, hi, 0.125f * radius);
        const auto& entries = fine.entries();
        for (std::size_t a = 0; a < entries.size();) {
            std::size_t b = a;
            Cell c;
            Vec3f centerSum;
            for (; b < entries.size() && entries[b].first == entries[a].first; ++b) {
                const std::size_t v = entries[b].second;
                if (!(weight[v] > 0.0f)) continue;
                centerSum += points[v] * weight[v];
                c.flow += flow[v];
                c.normal += normals[v];
                c.weight += weight[v];
            }
            a = b;
            if (!(c.weight > 0.0f)) continue;
            c.center = centerSum / c.weight;
            c.normal = c.normal.normalized();
            cells.push_back(c);
        }
    }
    std::vector<Vec3f> centers(cells.size());
    for (std::size_t c = 0; c < cells.size(); ++c) centers[c] = cells[c].center;
    const CellGrid cellsNear(centers, lo, hi, radius);

    // 2. The smooth field at every vertex.
    std::vector<Vec3f> field(points.size());
    parallelFor(0, points.size(), [&](std::size_t v) {
        const Vec3f p = points[v], n = normals[v];
        Vec3f sum;
        cellsNear.forEachNear(p, [&](std::uint32_t c) {
            const Cell& cell = cells[c];
            const Vec3f d = cell.center - p;
            const float k = kernel(d.dot(d)) * std::max(0.0f, cell.normal.dot(n));
            if (k > 0.0f) sum += tangential(cell.flow, n) * k;
        });
        field[v] = sum;
    }, 512);

    // 3. The edges the hair parts at, and where along them.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(triangles.size() * 3);
    for (const auto& t : triangles) {
        for (int k = 0; k < 3; ++k) {
            const auto a = static_cast<std::uint32_t>(t[k]), b = static_cast<std::uint32_t>(t[(k + 1) % 3]);
            if (a != b) edges.emplace_back(std::min(a, b), std::max(a, b));
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    const float cosSplit = pm::cos(std::clamp(splitAngleDegrees, 0.0f, 180.0f) * 3.14159265358979f / 180.0f);
    std::vector<std::uint8_t> splits(edges.size(), 0);
    std::vector<Vec3f> crossing(edges.size());
    parallelFor(0, edges.size(), [&](std::size_t e) {
        // (The same test with i and j swapped: a and b swap and change sign.) An end exactly on
        // the line (a symmetric comb over a mesh's center loop) splits both edges through it.
        const std::uint32_t i = edges[e].first, j = edges[e].second;
        const float a = field[i].dot(points[j] - points[i]), b = field[j].dot(points[j] - points[i]);
        if (!(a <= 0.0f && b >= 0.0f && a < b)) return;
        const Vec3f m = points[i] + (points[j] - points[i]) * (a / (a - b));  // where it crosses
        const Vec3f n = (normals[i] + normals[j]).normalized();
        const Vec3f across = tangential(points[j] - points[i], n).normalized();
        if (across.length() == 0.0f) return;
        Vec3f sum[2];
        float total[2] = {0.0f, 0.0f};
        float nearest = r2;
        cellsNear.forEachNear(m, [&](std::uint32_t c) {
            const Cell& cell = cells[c];
            const Vec3f d = cell.center - m;
            nearest = std::min(nearest, d.dot(d));
            const float k = kernel(d.dot(d)) * std::max(0.0f, cell.normal.dot(n));
            const float off = d.dot(across);
            if (!(k > 0.0f) || std::abs(off) < kFlowGuard * radius) return;  // too near to tell the side
            const int side = off < 0.0f ? 0 : 1;
            sum[side] += tangential(cell.flow, n) * k;
            total[side] += cell.weight * k;
        });
        if (!(total[0] > 0.0f) || !(total[1] > 0.0f)) return;
        if (nearest > 0.25f * r2) return;  // no hair within R/2: past the hair's edge
        const Vec3f back = sum[0] / total[0], on = sum[1] / total[1];
        const float lb = back.length(), ln = on.length();
        if (lb < kFlowCoherence || ln < kFlowCoherence) return;
        if (!(back.dot(across) < 0.0f && on.dot(across) > 0.0f)) return;
        if (back.dot(on) > cosSplit * lb * ln) return;
        splits[e] = 1;
        crossing[e] = m;
    }, 1024);

    // 4. The longest paths through the split edges are parts.
    std::vector<std::size_t> split;  // the split edges, in edge order
    for (std::size_t e = 0; e < edges.size(); ++e) {
        if (splits[e]) split.push_back(e);
    }
    if (split.empty()) return out;
    // Split edges are joined when their crossings are within R/2 (or 1.5 times the longer edge,
    // on a scalp coarse for R): the crossings of one line, even where a vertex row lies along it
    // and the crossings step from one side of it to the other or skip a row.
    std::vector<Vec3f> crossings(split.size());
    std::vector<float> reach(split.size());
    float farthestLink = 0.0f;
    for (std::size_t k = 0; k < split.size(); ++k) {
        crossings[k] = crossing[split[k]];
        const auto& [a, b] = edges[split[k]];
        reach[k] = std::max(0.5f * radius, 1.5f * (points[b] - points[a]).length());
        farthestLink = std::max(farthestLink, reach[k]);
    }
    std::vector<std::vector<std::uint32_t>> links(split.size());
    {
        const CellGrid grid(crossings, lo, hi, farthestLink);
        parallelFor(0, split.size(), [&](std::size_t k) {
            grid.forEachNear(crossings[k], [&](std::uint32_t o) {
                const Vec3f d = crossings[o] - crossings[k];
                const float link = std::max(reach[k], reach[o]);
                if (o != k && d.dot(d) <= link * link) links[k].push_back(o);
            });
            std::sort(links[k].begin(), links[k].end());
        }, 64);
    }
    auto forEachNeighbour = [&](std::size_t k, const auto& fn) {
        for (std::uint32_t o : links[k]) fn(o);
    };
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<std::uint8_t> alive(split.size(), 1);
    std::vector<float> dist(split.size(), inf);
    std::vector<std::uint32_t> prev(split.size(), 0);
    std::vector<std::uint32_t> reached;
    // Shortest paths from `from` through the live split edges (between crossing points): the
    // farthest one reached (the lowest index among equals) and every one reached in `reached`.
    auto sweep = [&](std::uint32_t from) {
        for (std::uint32_t k : reached) dist[k] = inf;
        reached.clear();
        using Item = std::pair<float, std::uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        dist[from] = 0.0f;
        prev[from] = from;
        queue.push({0.0f, from});
        std::uint32_t farthest = from;
        while (!queue.empty()) {
            const auto [d, k] = queue.top();
            queue.pop();
            if (d > dist[k]) continue;
            reached.push_back(k);
            if (d > dist[farthest] || (d == dist[farthest] && k < farthest)) farthest = k;
            forEachNeighbour(k, [&](std::uint32_t o) {
                if (!alive[o]) return;
                const float nd = d + (crossings[o] - crossings[k]).length();
                if (nd < dist[o] || (nd == dist[o] && k < prev[o])) {
                    dist[o] = nd;
                    prev[o] = k;
                    queue.push({nd, o});
                }
            });
        }
        return farthest;
    };
    const float shortest = std::max(0.0f, minLength) * size;
    std::vector<std::vector<Vec3f>> chains;
    for (std::uint32_t start = 0; start < split.size(); ++start) {
        while (alive[start]) {
            const std::uint32_t end = sweep(sweep(start));
            const std::uint32_t begin = [&] {
                std::uint32_t k = end;
                while (prev[k] != k) k = prev[k];
                return k;
            }();
            const float length = dist[end];
            if (end != begin && length > 0.0f && length >= shortest) {
                std::vector<std::uint32_t> path;
                for (std::uint32_t k = end;; k = prev[k]) {
                    path.push_back(k);
                    if (k == begin) break;
                }
                std::vector<Vec3f> line;
                line.reserve(path.size());
                for (auto it = path.rbegin(); it != path.rend(); ++it) line.push_back(crossings[*it]);
                chains.push_back(std::move(line));
                for (std::uint32_t k : path) {
                    alive[k] = 0;
                    forEachNeighbour(k, [&](std::uint32_t o) { alive[o] = 0; });
                }
            } else {
                for (std::uint32_t k : reached) alive[k] = 0;  // a set too short for a part
            }
        }
    }

    // 5. Each part resampled every R/2, smoothed, bound to the scalp.
    for (const auto& chain : chains) {
        const float length = arcLength(chain);
        const auto count = static_cast<std::size_t>(std::clamp(std::ceil(length / (0.5f * radius)) + 1.0f, 2.0f, 256.0f));
        std::vector<Vec3f> line = resampleByArcLength(chain.data(), chain.size(), count);
        std::vector<Vec3f> was(line.size());
        for (int pass = 0; pass < kFlowSmoothPasses; ++pass) {
            was = line;
            for (std::size_t i = 1; i + 1 < line.size(); ++i) line[i] = was[i - 1] * 0.25f + was[i] * 0.5f + was[i + 1] * 0.25f;
        }
        try {
            out.parts.push_back(PartSet::fromPoints("auto" + std::to_string(out.parts.size() + 1), line, scalp));
        } catch (const std::invalid_argument&) {
            // fewer than two points on the scalp: no part
        }
    }
    return out;
}

}  // namespace groomlab
