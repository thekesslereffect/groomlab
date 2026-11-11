// SPDX-License-Identifier: Apache-2.0
#include "groomlab/portable_math.h"
#include "groomlab/surface_paths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <utility>

#include "groomlab/frames.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/process_caches.h"

namespace groomlab {

namespace {

constexpr float kLean = 0.5f;     // sin 30 degrees: how far a straight piece may lean out of the skin at its ends
constexpr float kDetour = 1.3f;   // how much longer than it the edges walked to its end may be
constexpr float kSlackEdges = 2.0f;  // (plus this many mean edges: a gap narrower is no gap)
// The twist through the bends counts from ends whose normals face apart by acos(-kTwistFrom)
// (about 145 degrees), wholly from acos(-kTwistFull) (about 162).
constexpr float kTwistFrom = 0.82f, kTwistFull = 0.95f;
constexpr float kInf = std::numeric_limits<float>::infinity();

// The straight piece `chord` (of length `length`) between points whose normals are `a` and `b`
// leaves neither tangent plane steeply, and they do not face apart by more than such a piece on
// a round surface turns (twice the lean: two sheets of a fold face apart, and a shallow line
// from one to the other lies along both).
bool onSkin(const Vec3f& chord, float length, const Vec3f& a, const Vec3f& b) {
    const float most = kLean * length;
    return std::abs(chord.dot(a)) <= most && std::abs(chord.dot(b)) <= most && a.dot(b) >= 1.0f - 2.0f * kLean * kLean;
}

bool sameBits(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }

std::size_t slotHash(std::uint32_t v) { return static_cast<std::size_t>((v * 2654435761u) >> 7); }

// The squared distance from p to triangle abc: to its closest point, found by which of the
// triangle's regions (a corner's, an edge's, the inside) p projects into.
float distance2ToTriangle(const Vec3f& p, const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    const Vec3f ab = b - a, ac = c - a, ap = p - a;
    const float d1 = ab.dot(ap), d2 = ac.dot(ap);
    Vec3f q;
    if (d1 <= 0.0f && d2 <= 0.0f) {
        q = a;
    } else {
        const Vec3f bp = p - b;
        const float d3 = ab.dot(bp), d4 = ac.dot(bp);
        const Vec3f cp = p - c;
        const float d5 = ab.dot(cp), d6 = ac.dot(cp);
        const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d3 >= 0.0f && d4 <= d3) {
            q = b;
        } else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
            q = a + ab * (d1 - d3 != 0.0f ? d1 / (d1 - d3) : 0.0f);
        } else if (d6 >= 0.0f && d5 <= d6) {
            q = c;
        } else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
            q = a + ac * (d2 - d6 != 0.0f ? d2 / (d2 - d6) : 0.0f);
        } else if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
            const float den = (d4 - d3) + (d5 - d6);
            q = b + (c - b) * (den != 0.0f ? (d4 - d3) / den : 0.0f);
        } else if (!(std::abs(va + vb + vc) > 0.0f)) {  // a degenerate triangle (on a line): the nearest corner
            return std::min({ap.dot(ap), bp.dot(bp), cp.dot(cp)});
        } else {
            const float inverse = 1.0f / (va + vb + vc);
            q = a + ab * (vb * inverse) + ac * (vc * inverse);
        }
    }
    const Vec3f d = p - q;
    return d.dot(d);
}

std::atomic<std::size_t> frontCount{0};

}  // namespace

// ---- The graph ----------------------------------------------------------------------------------

SurfaceGraph::SurfaceGraph(const ScalpMesh& scalp)
    : triangles_(scalp.triangles()), restPoints_(scalp.hasRest() ? scalp.restPoints() : scalp.points()) {
    const std::vector<Vec3f>& points = restPoints_;
    const std::size_t n = points.size();
    // The rest normals per point as the scalp makes its own (ScalpMesh::buildGeometry).
    restNormals_.assign(n, Vec3f{});
    for (const ScalpMesh::Triangle& tri : triangles_) {
        const Vec3f& a = points[tri[0]];
        const Vec3f cross = (points[tri[1]] - a).cross(points[tri[2]] - a);
        for (const std::int32_t idx : tri) restNormals_[idx] += cross;
    }
    for (Vec3f& nrm : restNormals_) nrm = nrm.normalized();

    // Welded: points within a millionth of the mesh's size of an earlier one are that one. Cells
    // twice that wide; a point looks in its own cell and the neighbours it is that close to.
    Vec3f lo(kInf, kInf, kInf), hi(-kInf, -kInf, -kInf);
    for (const Vec3f& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const double diagonal = lo.x <= hi.x ? (hi - lo).length() : 0.0;
    const double tolerance = std::max(diagonal * 1e-6, 1e-12);
    const double cellSize = 2.0 * tolerance;
    auto cellOf = [&](double v) { return static_cast<std::int64_t>(std::floor(v / cellSize)); };
    auto key = [](std::int64_t x, std::int64_t y, std::int64_t z) {
        return hashCombine(hashCombine(hash64(static_cast<std::uint64_t>(x)), static_cast<std::uint64_t>(y)), static_cast<std::uint64_t>(z));
    };
    auto finite = [](const Vec3f& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    // The points by cell (a cell's key, then the point), found by a binary search.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> byCell;
    byCell.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f& p = points[i];
        if (finite(p)) byCell.push_back({key(cellOf(p.x), cellOf(p.y), cellOf(p.z)), static_cast<std::uint32_t>(i)});
    }
    std::sort(byCell.begin(), byCell.end());
    // Each cell's first point in byCell, in a table open-addressed by the cell's key.
    std::size_t tableSize = 16;
    while (tableSize < 2 * byCell.size()) tableSize *= 2;
    constexpr std::uint32_t kEmpty = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::pair<std::uint64_t, std::uint32_t>> table(tableSize, {0, kEmpty});
    for (std::size_t i = 0; i < byCell.size(); ++i) {
        if (i > 0 && byCell[i].first == byCell[i - 1].first) continue;
        std::size_t at = static_cast<std::size_t>(byCell[i].first) & (tableSize - 1);
        while (table[at].second != kEmpty) at = (at + 1) & (tableSize - 1);
        table[at] = {byCell[i].first, static_cast<std::uint32_t>(i)};
    }
    auto firstInCell = [&](std::uint64_t k) -> std::size_t {
        for (std::size_t at = static_cast<std::size_t>(k) & (tableSize - 1);; at = (at + 1) & (tableSize - 1)) {
            if (table[at].second == kEmpty) return byCell.size();
            if (table[at].first == k) return table[at].second;
        }
    };
    std::vector<std::uint32_t> weld(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f& p = points[i];
        // The earliest point before it within the tolerance names its vertex; else it is a new one.
        std::uint32_t earliest = static_cast<std::uint32_t>(i);
        if (finite(p)) {
            const double c[3] = {p.x, p.y, p.z};
            std::int64_t span[3][2];
            for (int k = 0; k < 3; ++k) {
                span[k][0] = cellOf(c[k] - tolerance);
                span[k][1] = cellOf(c[k] + tolerance);
            }
            for (std::int64_t x = span[0][0]; x <= span[0][1]; ++x) {
                for (std::int64_t y = span[1][0]; y <= span[1][1]; ++y) {
                    for (std::int64_t z = span[2][0]; z <= span[2][1]; ++z) {
                        const std::uint64_t k = key(x, y, z);
                        for (auto it = byCell.begin() + static_cast<std::ptrdiff_t>(firstInCell(k));
                             it != byCell.end() && it->first == k && it->second < earliest; ++it) {
                            if ((points[it->second] - p).length() <= tolerance) earliest = it->second;
                        }
                    }
                }
            }
        }
        if (earliest == i) {
            weld[i] = static_cast<std::uint32_t>(positions_.size());
            positions_.push_back(p);
        } else {
            weld[i] = weld[earliest];
        }
    }
    const std::size_t vertices = positions_.size();

    // Corners, normals over every face around a welded vertex, edges (each vertex's neighbours
    // gathered from its triangles, then sorted, once each).
    corners_.resize(triangles_.size() * 3);
    normals_.assign(vertices, Vec3f{});
    std::vector<std::uint32_t> degree(vertices + 1, 0);
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
        const ScalpMesh::Triangle& tri = triangles_[t];
        std::uint32_t c[3];
        for (int k = 0; k < 3; ++k) c[k] = corners_[3 * t + k] = weld[tri[k]];
        const Vec3f& a = points[tri[0]];
        const Vec3f cross = (points[tri[1]] - a).cross(points[tri[2]] - a);
        if (c[0] != c[1] && c[1] != c[2] && c[0] != c[2]) {
            for (const std::uint32_t v : c) normals_[v] += cross;
        }
        for (int k = 0; k < 3; ++k) degree[c[k] + 1] += 2;
    }
    for (Vec3f& nrm : normals_) nrm = nrm.normalized();
    for (std::size_t v = 0; v < vertices; ++v) degree[v + 1] += degree[v];
    std::vector<std::uint32_t> around(degree[vertices]);
    {
        std::vector<std::uint32_t> at(degree.begin(), degree.end() - 1);
        for (std::size_t t = 0; t < triangles_.size(); ++t) {
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t u = corners_[3 * t + k];
                around[at[u]++] = corners_[3 * t + (k + 1) % 3];
                around[at[u]++] = corners_[3 * t + (k + 2) % 3];
            }
        }
    }
    adjacencyBegin_.assign(vertices + 1, 0);
    adjacency_.clear();
    adjacency_.reserve(around.size() / 2);
    for (std::size_t v = 0; v < vertices; ++v) {
        auto first = around.begin() + degree[v], last = around.begin() + degree[v + 1];
        std::sort(first, last);
        for (auto it = first; it != last; ++it) {
            if (*it == v || (it != first && *it == *(it - 1))) continue;  // (itself, through a degenerate triangle; twice)
            adjacency_.push_back(*it);
        }
        adjacencyBegin_[v + 1] = static_cast<std::uint32_t>(adjacency_.size());
    }
    edgeLengths_.resize(adjacency_.size());
    longest_.assign(vertices, 0.0f);
    double total = 0.0;
    for (std::size_t v = 0; v < vertices; ++v) {
        for (std::uint32_t i = adjacencyBegin_[v]; i < adjacencyBegin_[v + 1]; ++i) {
            const float length = (positions_[adjacency_[i]] - positions_[v]).length();
            edgeLengths_[i] = length;
            if (std::isfinite(length)) {
                longest_[v] = std::max(longest_[v], length);
                total += length;
            }
        }
    }
    const std::size_t edgeCount = adjacency_.size();
    // Rounded to an eighth of an octave: an edit of one part of the mesh changes it (and the
    // paths' slack everywhere) only when it steps.
    const double mean = edgeCount == 0 ? 0.0 : total / static_cast<double>(edgeCount);
    meanEdge_ = mean > 0.0 && std::isfinite(mean) ? static_cast<float>(pm::exp2(std::round(pm::log2(mean) * 8.0) / 8.0)) : 0.0f;

    for (const float e : edgeLengths_) {
        if (std::isfinite(e)) maxEdge_ = std::max(maxEdge_, e);
    }
    // The triangles around each vertex.
    const std::size_t triangleCount = triangles_.size();
    trianglesBegin_.assign(vertices + 1, 0);
    for (std::size_t c = 0; c < corners_.size(); ++c) ++trianglesBegin_[corners_[c] + 1];
    for (std::size_t v = 0; v < vertices; ++v) trianglesBegin_[v + 1] += trianglesBegin_[v];
    trianglesAround_.resize(corners_.size());
    {
        std::vector<std::uint32_t> at(trianglesBegin_.begin(), trianglesBegin_.end() - 1);
        for (std::size_t c = 0; c < corners_.size(); ++c) trianglesAround_[at[corners_[c]]++] = static_cast<std::uint32_t>(c / 3);
    }
    (void)triangleCount;

    // The grid: each vertex in its cell, cells about two mean edges wide (fewer than 2^21 of them
    // over the box).
    Vec3f gridLo(kInf, kInf, kInf), gridHi(-kInf, -kInf, -kInf);
    for (const Vec3f& p : positions_) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        gridLo = {std::min(gridLo.x, p.x), std::min(gridLo.y, p.y), std::min(gridLo.z, p.z)};
        gridHi = {std::max(gridHi.x, p.x), std::max(gridHi.y, p.y), std::max(gridHi.z, p.z)};
    }
    if (gridLo.x > gridHi.x) return;  // no vertex
    origin_ = gridLo;
    const Vec3f extent = gridHi - gridLo;
    cell_ = std::max(2.0f * meanEdge_, 1e-6f * std::max({extent.x, extent.y, extent.z, 1e-6f}));
    for (;;) {
        for (int k = 0; k < 3; ++k) {
            const float e = k == 0 ? extent.x : k == 1 ? extent.y : extent.z;
            dims_[k] = static_cast<std::int64_t>(e / cell_) + 1;
        }
        if (dims_[0] * dims_[1] * dims_[2] <= (std::int64_t{1} << 21)) break;
        cell_ *= 1.26f;
    }
    auto gridCellOf = [&](const Vec3f& p) -> std::int64_t {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return -1;
        const Vec3f d = p - origin_;
        const std::int64_t x = std::min<std::int64_t>(dims_[0] - 1, static_cast<std::int64_t>(d.x / cell_));
        const std::int64_t y = std::min<std::int64_t>(dims_[1] - 1, static_cast<std::int64_t>(d.y / cell_));
        const std::int64_t z = std::min<std::int64_t>(dims_[2] - 1, static_cast<std::int64_t>(d.z / cell_));
        return (z * dims_[1] + y) * dims_[0] + x;
    };
    const std::size_t gridCells = static_cast<std::size_t>(dims_[0] * dims_[1] * dims_[2]);
    cellBegin_.assign(gridCells + 1, 0);
    for (const Vec3f& p : positions_) {
        const std::int64_t c = gridCellOf(p);
        if (c >= 0) ++cellBegin_[static_cast<std::size_t>(c) + 1];
    }
    for (std::size_t c = 0; c < gridCells; ++c) cellBegin_[c + 1] += cellBegin_[c];
    cellVertices_.resize(cellBegin_[gridCells]);
    std::vector<std::uint32_t> cursor(cellBegin_.begin(), cellBegin_.end() - 1);
    for (std::size_t v = 0; v < vertices; ++v) {
        const std::int64_t c = gridCellOf(positions_[v]);
        if (c >= 0) cellVertices_[cursor[static_cast<std::size_t>(c)]++] = static_cast<std::uint32_t>(v);
    }
}

Vec3f SurfaceGraph::rootPosition(const StrandRoot& root) const {
    const ScalpMesh::Triangle& tri = triangles_[static_cast<std::size_t>(root.face)];
    const float w = 1.0f - root.u - root.v;
    return restPoints_[tri[0]] * w + restPoints_[tri[1]] * root.u + restPoints_[tri[2]] * root.v;
}

Vec3f SurfaceGraph::rootNormal(const StrandRoot& root) const {
    const ScalpMesh::Triangle& tri = triangles_[static_cast<std::size_t>(root.face)];
    const float w = 1.0f - root.u - root.v;
    const Vec3f n = restNormals_[tri[0]] * w + restNormals_[tri[1]] * root.u + restNormals_[tri[2]] * root.v;
    return n.normalized();
}

SurfaceGraph::FrontKey SurfaceGraph::frontKey(const StrandRoot& root, float limit) {
    std::uint32_t u, v, l;
    std::memcpy(&u, &root.u, 4);
    std::memcpy(&v, &root.v, 4);
    std::memcpy(&l, &limit, 4);
    return {(static_cast<std::uint64_t>(static_cast<std::uint32_t>(root.face)) << 32) ^ (static_cast<std::uint64_t>(u) << 16) ^ v ^
                (static_cast<std::uint64_t>(v) << 40),
            (static_cast<std::uint64_t>(l) << 32) | u};
}

std::shared_ptr<const SurfaceFront> SurfaceGraph::keptFront(const StrandRoot& root, float limit) const {
    const std::lock_guard<std::mutex> lock(keptMutex_);
    const auto it = kept_.find(frontKey(root, limit));
    if (it == kept_.end()) return nullptr;
    const SurfaceFront& f = *it->second;  // (the key is a hash of the root: the root itself must match)
    return sameBits(&f.root, &root, sizeof root) && sameBits(&f.limit, &limit, sizeof limit) ? it->second : nullptr;
}

void SurfaceGraph::keepFront(const std::shared_ptr<const SurfaceFront>& front) const {
    constexpr std::size_t kKeptFronts = 1 << 15;
    const std::lock_guard<std::mutex> lock(keptMutex_);
    if (kept_.size() >= kKeptFronts) kept_.clear();
    kept_[frontKey(front->root, front->limit)] = front;
}

std::shared_ptr<const SurfaceGraph> sharedSurfaceGraph(const ScalpMesh& scalp) {
    struct Entry {
        std::uint64_t surface;
        std::shared_ptr<const SurfaceGraph> graph;
    };
    static std::mutex mutex;
    static std::list<Entry> kept;  // most recently used first
    constexpr std::size_t kKept = 4;
    static const bool registered = (registerProcessCache("surface graphs", [] {
                                        std::list<Entry> drop;
                                        const std::lock_guard<std::mutex> lock(mutex);
                                        drop.swap(kept);
                                    }),
                                    true);
    (void)registered;
    const std::uint64_t surface = scalp.restSurfaceHash();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (it->surface == surface) {
                kept.splice(kept.begin(), kept, it);
                return kept.front().graph;
            }
        }
    }
    auto graph = std::make_shared<const SurfaceGraph>(scalp);  // (made outside the lock: two threads may both make one)
    const std::lock_guard<std::mutex> lock(mutex);
    for (const Entry& e : kept) {
        if (e.surface == surface) return e.graph;
    }
    kept.push_front({surface, graph});
    while (kept.size() > kKept) kept.pop_back();
    return graph;
}

// ---- A front ------------------------------------------------------------------------------------

std::shared_ptr<const SurfaceFront> surfaceFront(const SurfaceGraph& graph, const StrandRoot& root, float limit) {
    ++frontCount;
    auto out = std::make_shared<SurfaceFront>();
    SurfaceFront& front = *out;
    front.root = root;
    const Vec3f position = graph.rootPosition(root);
    front.position = position;
    front.normal = graph.rootNormal(root);
    front.limit = limit;
    const float slack = kSlackEdges * graph.meanEdge();

    // Each vertex's slot in the thread's working arrays, stamped per front (no clearing between
    // fronts); the arrays kept from front to front.
    // The open vertices as one number each: a distance's bits (ordered as the distances are, all
    // being finite and not negative) over the vertex: nearest first, then by vertex.
    using Item = std::uint64_t;
    auto item = [](float d, std::uint32_t v) {
        std::uint32_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        return (static_cast<std::uint64_t>(bits) << 32) | v;
    };
    struct Scratch {
        std::vector<std::uint32_t> slot, stamp;
        std::uint32_t current = 0;
        std::vector<SurfaceFront::Reached> reached;  // per slot
        std::vector<char> done;
        std::vector<std::int32_t> anchorAt;  // per slot: the anchor made at the vertex, -1 none
        std::vector<SurfaceFront::Anchor> anchors;
        std::vector<Item> open;              // a heap
    };
    thread_local Scratch scratch;
    if (scratch.slot.size() < graph.vertexCount()) {
        scratch.slot.assign(graph.vertexCount(), 0);
        scratch.stamp.assign(graph.vertexCount(), 0);
    }
    if (++scratch.current == 0) {  // (wrapped: every stamp is stale)
        std::fill(scratch.stamp.begin(), scratch.stamp.end(), 0u);
        scratch.current = 1;
    }
    std::vector<SurfaceFront::Reached>& reached = scratch.reached;
    std::vector<char>& done = scratch.done;
    std::vector<std::int32_t>& anchorAt = scratch.anchorAt;
    std::vector<SurfaceFront::Anchor>& anchors = scratch.anchors;
    reached.clear();
    done.clear();
    anchorAt.clear();
    anchors.clear();
    anchors.push_back({position, front.normal, anyPerpendicular(front.normal), 0.0f, 0.0f});
    auto slotOf = [&](std::uint32_t v) -> std::uint32_t {
        if (scratch.stamp[v] != scratch.current) {
            scratch.stamp[v] = scratch.current;
            scratch.slot[v] = static_cast<std::uint32_t>(reached.size());
            reached.push_back({v, kInf, 0.0f, 0});
            done.push_back(0);
            anchorAt.push_back(-1);
        }
        return scratch.slot[v];
    };
    std::vector<Item>& open = scratch.open;
    open.clear();
    auto push = [&](float d, std::uint32_t v) {
        open.push_back(item(d, v));
        std::push_heap(open.begin(), open.end(), std::greater<Item>());
    };
    // The source's own triangle: its corners by the straight line.
    for (int k = 0; k < 3; ++k) {
        const std::uint32_t v = graph.corner(static_cast<std::size_t>(root.face), k);
        const std::uint32_t s = slotOf(v);
        const float d = (graph.position(v) - position).length();
        if (d < reached[s].distance) {
            reached[s].distance = d;
            reached[s].path = d;
            push(d, v);
        }
    }
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), std::greater<Item>());
        const std::uint32_t u = static_cast<std::uint32_t>(open.back());
        open.pop_back();
        const std::uint32_t su = scratch.slot[u];
        if (done[su]) continue;  // (an older, longer entry of it)
        done[su] = 1;
        const float d = reached[su].distance;
        if (!(d <= limit + graph.longestEdge(u))) continue;  // reached, not expanded
        const Vec3f& pu = graph.position(u);
        std::size_t count = 0;
        const std::uint32_t* next = graph.neighbors(u, &count);
        const float* edges = graph.edgeLengths(u);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t v = next[i];
            const std::uint32_t sv = slotOf(v);
            if (done[sv]) continue;
            const Vec3f& pv = graph.position(v);
            const float edge = edges[i];  // (pv - pu).length()
            const float walked = reached[su].path + edge;
            // Straight from u's anchor, if that stays on the skin; else bent at u.
            const std::uint32_t a = reached[su].anchor;
            const SurfaceFront::Anchor& anchor = anchors[a];
            const Vec3f chord = pv - anchor.position;
            const float length = chord.length();
            float distance;
            std::uint32_t by;
            if (onSkin(chord, length, anchor.normal, graph.normal(v)) && walked - anchor.path <= kDetour * length + slack) {
                distance = anchor.distance + length;
                by = a;
            } else {
                distance = reached[su].distance + edge;
                if (anchorAt[su] < 0) {
                    const Vec3f& n = graph.normal(u);
                    anchorAt[su] = static_cast<std::int32_t>(anchors.size());
                    // (made first: the push may move the anchors)
                    const SurfaceFront::Anchor bend{pu, n, Rotation(anchor.normal, n)(anchor.tangent), reached[su].distance, reached[su].path};
                    anchors.push_back(bend);
                }
                by = static_cast<std::uint32_t>(anchorAt[su]);
            }
            if (distance < reached[sv].distance) {
                reached[sv].distance = distance;
                reached[sv].path = walked;
                reached[sv].anchor = by;
                push(distance, v);
            }
        }
    }
    // The triangles within the limit in a straight line not reached straight at all three corners.
    // (The scratch slots still name this front's vertices.)
    auto straightAt = [&](std::uint32_t v) {
        return scratch.stamp[v] == scratch.current && reached[scratch.slot[v]].anchor == 0 && std::isfinite(reached[scratch.slot[v]].distance);
    };
    // Such a triangle's corners all lie within the limit and its longest edge of the source: the
    // triangles round the vertices there not reached straight.
    const float limit2 = limit * limit;
    graph.forEachVertexNear(position, limit + graph.longestEdge(), [&](std::uint32_t v) {
        if (straightAt(v)) return;
        std::size_t count = 0;
        const std::uint32_t* around = graph.trianglesAround(v, &count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t t = around[i];
            // (A degenerate triangle's distance may come out NaN: taken as near.)
            if (distance2ToTriangle(position, graph.position(graph.corner(t, 0)), graph.position(graph.corner(t, 1)),
                                    graph.position(graph.corner(t, 2))) > limit2) {
                continue;
            }
            front.bent.push_back(t);
        }
    });
    std::sort(front.bent.begin(), front.bent.end());
    front.bent.erase(std::unique(front.bent.begin(), front.bent.end()), front.bent.end());
    // Every vertex reached (a vertex relaxed is always settled: the queue drains), and a table to
    // find them by vertex.
    front.reached = reached;
    std::size_t size = 16;
    while (size < 2 * reached.size()) size *= 2;
    front.table.assign(size, 0);
    for (std::size_t i = 0; i < reached.size(); ++i) {
        std::size_t at = slotHash(reached[i].vertex) & (size - 1);
        while (front.table[at] != 0) at = (at + 1) & (size - 1);
        front.table[at] = static_cast<std::uint32_t>(i + 1);
    }
    front.anchors = anchors;
    return out;
}

const SurfaceFront::Reached* SurfaceFront::find(std::uint32_t v) const {
    const std::size_t mask = table.size() - 1;
    for (std::size_t at = slotHash(v) & mask;; at = (at + 1) & mask) {
        const std::uint32_t i = table[at];
        if (i == 0) return nullptr;
        if (reached[i - 1].vertex == v) return &reached[i - 1];
    }
}

std::size_t surfaceFrontCount() { return frontCount.load(); }

// ---- Many fronts --------------------------------------------------------------------------------

SurfaceFronts::SurfaceFronts(std::shared_ptr<const SurfaceGraph> graph, const std::vector<StrandRoot>& roots,
                             const std::vector<float>& limits, const SurfaceFronts* previous)
    : graph_(std::move(graph)) {
    const std::size_t sources = roots.size();
    fronts_.resize(sources);
    const bool reuse = previous && previous->graph_ == graph_ && previous->fronts_.size() == sources;
    std::vector<std::size_t> make;
    for (std::size_t i = 0; i < sources; ++i) {
        if (!graph_->contains(roots[i]) || !(limits[i] >= 0.0f)) continue;
        if (reuse) {
            const auto& was = previous->fronts_[i];
            if (was && sameBits(&was->root, &roots[i], sizeof(StrandRoot)) && sameBits(&was->limit, &limits[i], sizeof(float))) {
                fronts_[i] = was;
                continue;
            }
        }
        if ((fronts_[i] = graph_->keptFront(roots[i], limits[i]))) continue;
        make.push_back(i);
    }
    parallelFor(0, make.size(), [&](std::size_t k) {
        const std::size_t i = make[k];
        fronts_[i] = surfaceFront(*graph_, roots[i], limits[i]);
        graph_->keepFront(fronts_[i]);
    }, 1);
    made_ = make.size();
    within2_.assign(sources, kInf);
    for (std::size_t i = 0; i < sources; ++i) {
        if (!fronts_[i]) continue;
        const float within = 0.999f * fronts_[i]->limit;  // (room for the caller's rounding of the roots)
        within2_[i] = within * within;
    }
    // Per triangle, the fronts that do not reach it straight where they might: the previous
    // counts, less the fronts gone and plus the ones made, when there are previous ones.
    std::vector<std::size_t> changed;
    if (reuse && previous->bentCounts_) {
        for (std::size_t i = 0; i < sources; ++i) {
            if (fronts_[i] != previous->fronts_[i]) changed.push_back(i);
        }
        if (changed.empty()) {
            bentCounts_ = previous->bentCounts_;
            return;
        }
    }
    auto counts = std::make_shared<std::vector<std::uint32_t>>();
    if (reuse && previous->bentCounts_) {
        *counts = *previous->bentCounts_;
        for (const std::size_t i : changed) {
            if (const SurfaceFront* was = previous->fronts_[i].get()) {
                for (const std::uint32_t t : was->bent) --(*counts)[t];
            }
            if (const SurfaceFront* now = fronts_[i].get()) {
                for (const std::uint32_t t : now->bent) ++(*counts)[t];
            }
        }
    } else {
        counts->assign(graph_->triangleCount(), 0);
        for (const auto& f : fronts_) {
            if (!f) continue;
            for (const std::uint32_t t : f->bent) ++(*counts)[t];
        }
    }
    bentCounts_ = std::move(counts);
}

SurfacePath SurfaceFronts::path(std::size_t source, const SurfacePathEnd& end, float straight2) const {
    SurfacePath out;
    const SurfaceFront* front = fronts_[source].get();
    if (!front || !graph_->contains(end.root)) return out;  // Unknown
    const std::size_t face = static_cast<std::size_t>(end.root.face);
    // Within the limit in a straight line on a triangle every front within its limit reaches
    // straight, or on the source's own: the straight line.
    if (static_cast<std::size_t>(front->root.face) == face || ((*bentCounts_)[face] == 0 && straight2 < within2_[source])) {
        out.kind = SurfacePath::Kind::Straight;
        return out;
    }
    const SurfaceFront::Reached* entry[3];
    int found = 0;
    bool straight = true;
    for (int k = 0; k < 3; ++k) {
        entry[k] = front->find(graph_->corner(face, k));
        if (entry[k]) ++found;
        straight = straight && entry[k] && entry[k]->anchor == 0;
    }
    if (found == 0) {
        out.kind = SurfacePath::Kind::Beyond;
        return out;
    }
    // Every corner reached by the straight line: so is the point.
    if (straight) {
        out.kind = SurfacePath::Kind::Straight;
        return out;
    }
    if (!end.hasNormal) {
        end.normal = graph_->rootNormal(end.root);
        end.hasNormal = true;
    }
    // From each corner reached: straight from its anchor if that stays on the skin, else bent at
    // the corner. The shortest; the straight line from the source first among equals.
    const float slack = kSlackEdges * graph_->meanEdge();
    float best = kInf;
    int bestCorner = -1;
    bool bestBent = false;
    std::uint32_t bestAnchor = 0;
    for (int k = 0; k < 3; ++k) {
        if (!entry[k]) continue;
        const std::uint32_t a = entry[k]->anchor;
        const SurfaceFront::Anchor& anchor = front->anchors[a];
        const Vec3f chord = end.position - anchor.position;
        const float length = chord.length();
        const float edge = (end.position - graph_->position(graph_->corner(face, k))).length();
        float distance;
        bool bent;
        if (onSkin(chord, length, anchor.normal, end.normal) && entry[k]->path + edge - anchor.path <= kDetour * length + slack) {
            distance = anchor.distance + length;
            bent = false;
        } else {
            distance = entry[k]->distance + edge;
            bent = true;
        }
        const bool fromSource = !bent && a == 0;
        const bool bestFromSource = bestCorner >= 0 && !bestBent && bestAnchor == 0;
        if (distance < best || (distance == best && fromSource && !bestFromSource)) {
            best = distance;
            bestCorner = k;
            bestBent = bent;
            bestAnchor = a;
        }
    }
    if (!bestBent && bestAnchor == 0) {
        out.kind = SurfacePath::Kind::Straight;
        return out;
    }
    out.kind = SurfacePath::Kind::Bent;
    out.distance = best;
    // The twist: where the ends' normals face apart (more than about 145 degrees: the two sides of
    // a flap or a shell), the plain turn between them is a guess (opposite normals turn half way
    // round any axis), and the path's own turns say which way round the skin went: the source's
    // tangent where the path last turned, turned onto the end's normal, against the plain turn of
    // it. Elsewhere the plain turn is the turn along the path (on a convex or a cylindrical
    // skin exactly so), and the bends' vertices would only add their own placement's.
    const float facing = front->normal.dot(end.normal);
    const float share = smoothstep(kTwistFrom, kTwistFull, -facing);
    if (share <= 0.0f) return out;
    const SurfaceFront::Anchor& anchor = front->anchors[bestAnchor];
    Vec3f normal = anchor.normal, tangent = anchor.tangent;
    if (bestBent) {
        normal = graph_->normal(graph_->corner(face, bestCorner));
        tangent = Rotation(anchor.normal, normal)(anchor.tangent);
    }
    const Vec3f plain = Rotation(front->normal, end.normal)(front->anchors[0].tangent);
    const Vec3f carried = Rotation(normal, end.normal)(tangent);
    out.twist = share * pm::atan2(end.normal.dot(plain.cross(carried)), plain.dot(carried));
    return out;
}

}  // namespace groomlab
