// SPDX-License-Identifier: Apache-2.0
#include "groomlab/scalp_mesh.h"

#include <cmath>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#define GROOMLAB_SCALP_SSE 1
#else
#define GROOMLAB_SCALP_SSE 0  // (arm64: Apple silicon)
#endif

#include "groomlab/random.h"

namespace groomlab {

namespace {

std::uint64_t hashFloats(std::uint64_t h, const float* f, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) h = hashFloat(h, f[i]);
    return h;
}

}  // namespace

ScalpMesh::ScalpMesh(std::vector<Vec3f> points, std::vector<Triangle> triangles)
    : points_(std::move(points)), triangles_(std::move(triangles)), bvh_(std::make_shared<LazyBvh>()) {
    buildGeometry(true);
    updateHash();
}

void ScalpMesh::buildGeometry(bool checkIndices) {
    const auto n = static_cast<std::int32_t>(points_.size());
    vertexNormals_.assign(points_.size(), Vec3f{});
    areas_.resize(triangles_.size());
    totalArea_ = 0.0f;

    geometryHash_ = hash64(points_.size());
    for (const Vec3f& p : points_) {
        const float xyz[3] = {p.x, p.y, p.z};
        geometryHash_ = hashFloats(geometryHash_, xyz, 3);
    }

    // The triangles are checked and hashed once: a mesh made by withPoints keeps its source's.
    if (checkIndices) topologyHash_ = hash64(triangles_.size() + 0x7090ull);
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
        const Triangle& tri = triangles_[t];
        for (std::int32_t idx : tri) {
            if (checkIndices) {
                if (idx < 0 || idx >= n) throw std::out_of_range("ScalpMesh: triangle index out of range");
                topologyHash_ = hashCombine(topologyHash_, static_cast<std::uint64_t>(idx));
            }
            geometryHash_ = hashCombine(geometryHash_, static_cast<std::uint64_t>(idx));
        }
        const Vec3f& a = points_[tri[0]];
        const Vec3f cross = (points_[tri[1]] - a).cross(points_[tri[2]] - a);
        const float area = 0.5f * cross.length();
        areas_[t] = std::isfinite(area) ? area : 0.0f;  // non-finite points: no strands there
        totalArea_ += areas_[t];
        // Unnormalized cross product weights each face normal by its area.
        for (std::int32_t idx : tri) vertexNormals_[idx] += cross;
    }
    for (Vec3f& nrm : vertexNormals_) nrm = nrm.normalized();
}

void ScalpMesh::updateHash() {
    surfaceHash_ = hashCombine(hashCombine(geometryHash_, uvHash_), restHash_);
    if (piecesHash_ != 0) surfaceHash_ = hashCombine(surfaceHash_, piecesHash_);  // several scalps only
    hash_ = surfaceHash_;
    for (const auto& [name, h] : colorHashes_) hash_ = hashCombine(hash_, h);
    if (renderSubdivision_ > 0) hash_ = hashCombine(hash_, hash64(0x5DB0ull + static_cast<std::uint64_t>(renderSubdivision_)));
}

void ScalpMesh::setPolygons(std::vector<std::int32_t> trianglesPerPolygon) {
    std::size_t total = 0;
    for (const std::int32_t n : trianglesPerPolygon) {
        if (n < 0) throw std::invalid_argument("ScalpMesh: a polygon with a negative triangle count");
        total += static_cast<std::size_t>(n);
    }
    if (!trianglesPerPolygon.empty() && total != triangles_.size()) {
        throw std::invalid_argument("ScalpMesh: the polygons' triangles (" + std::to_string(total) + ") are not the mesh's (" +
                                    std::to_string(triangles_.size()) + ")");
    }
    polygons_ = std::move(trianglesPerPolygon);
    polygonHash_ = 0;
    if (polygons_.empty()) return;
    polygonHash_ = hash64(0x9017ull + polygons_.size());
    for (const std::int32_t n : polygons_) polygonHash_ = hashCombine(polygonHash_, static_cast<std::uint64_t>(n));
}

std::uint64_t ScalpMesh::topologyFingerprint() const {
    // Stored in groom files: a change here changes every file's fingerprint (keep it stable).
    std::uint64_t h = hashCombine(hash64(0xF1A6E7ull), static_cast<std::uint64_t>(points_.size()));
    h = hashCombine(h, static_cast<std::uint64_t>(triangles_.size()));
    for (const Triangle& t : triangles_) {
        for (const std::int32_t v : t) h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(v)));
    }
    return h ? h : 1;
}

void ScalpMesh::setPieces(std::vector<ScalpPiece> pieces) {
    std::size_t triangle = 0, point = 0;
    std::uint64_t h = 0;
    if (!pieces.empty()) {
        h = hash64(0x5CA195ull + pieces.size());
        for (const ScalpPiece& p : pieces) {
            if (p.firstTriangle != triangle || p.firstPoint != point) {
                throw std::invalid_argument("ScalpMesh: the scalp '" + p.name + "' does not follow the one before it");
            }
            triangle += p.triangleCount;
            point += p.pointCount;
            h = hashString(h, p.name);
            for (const std::size_t n : {p.firstTriangle, p.triangleCount, p.firstPoint, p.pointCount}) {
                h = hashCombine(h, static_cast<std::uint64_t>(n));
            }
            h = hashCombine(h, static_cast<std::uint64_t>(p.renderSubdivision));
            if (p.subdivision >= 0) h = hashCombine(h, hash64(0x5D1Full + static_cast<std::uint64_t>(p.subdivision)));
        }
        if (triangle != triangles_.size() || point != points_.size()) {
            throw std::invalid_argument("ScalpMesh: the scalps' triangles and points are not the mesh's");
        }
    }
    pieces_ = pieces.empty() ? nullptr : std::make_shared<const std::vector<ScalpPiece>>(std::move(pieces));
    piecesHash_ = h;
    updateHash();
}

const std::vector<ScalpPiece>& ScalpMesh::pieces() const {
    static const std::vector<ScalpPiece> kNone;
    return pieces_ ? *pieces_ : kNone;
}

const ScalpPiece* ScalpMesh::piece(const std::string& name) const {
    if (!pieces_) return nullptr;
    for (const ScalpPiece& p : *pieces_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

void ScalpMesh::setRenderSubdivision(int levels, std::string from) {
    renderSubdivision_ = std::max(0, levels);
    renderSubdivisionFrom_ = renderSubdivision_ > 0 || !from.empty() ? std::move(from) : std::string();
    updateHash();
}

std::uint64_t ScalpMesh::colorSetHash(const std::string& name) const {
    auto it = colorHashes_.find(name);
    return it != colorHashes_.end() ? it->second : hashString(hash64(0xAB5E7C5E7ull), name);
}

// (Hot paths: callers check the face against triangleCount() first, so no bounds check here.)
Vec3f ScalpMesh::normal(const StrandRoot& root) const {
    assert(root.face >= 0 && static_cast<std::size_t>(root.face) < triangles_.size());
    const Triangle& tri = triangles_[static_cast<std::size_t>(root.face)];
    const float w = 1.0f - root.u - root.v;
    const Vec3f n = vertexNormals_[tri[0]] * w + vertexNormals_[tri[1]] * root.u + vertexNormals_[tri[2]] * root.v;
    return n.normalized();
}

// ---------------------------------------------------------------------------
// Per-corner data

void ScalpMesh::setCornerUVs(std::vector<UV> uvs) {
    if (!uvs.empty() && uvs.size() != triangles_.size() * 3) {
        throw std::invalid_argument("ScalpMesh: expected one UV per triangle corner");
    }
    cornerUVs_ = std::move(uvs);
    uvHash_ = cornerUVs_.empty() ? 0 : hash64(0x5576);
    for (const UV& uv : cornerUVs_) uvHash_ = hashFloats(uvHash_, uv.data(), 2);
    updateHash();
}

Vec3f ScalpMesh::uvTangent(std::int32_t face) const {
    if (face < 0 || static_cast<std::size_t>(face) >= triangles_.size()) return {};
    const auto& tri = triangles_[static_cast<std::size_t>(face)];
    const auto& uv = cornerUVs();
    const std::size_t c = static_cast<std::size_t>(face) * 3;
    if (c + 2 >= uv.size()) return {};
    const Vec3f a = points_[static_cast<std::size_t>(tri[0])];
    const Vec3f e1 = points_[static_cast<std::size_t>(tri[1])] - a;
    const Vec3f e2 = points_[static_cast<std::size_t>(tri[2])] - a;
    const float du1 = uv[c + 1][0] - uv[c][0], dv1 = uv[c + 1][1] - uv[c][1];
    const float du2 = uv[c + 2][0] - uv[c][0], dv2 = uv[c + 2][1] - uv[c][1];
    const float det = du1 * dv2 - du2 * dv1;
    if (!(std::fabs(det) > 1e-12f)) return {};
    return (e1 * dv2 - e2 * dv1) / det;
}

ScalpMesh::UV ScalpMesh::uvAt(const StrandRoot& root) const {
    if (cornerUVs_.empty() || !contains(root) || static_cast<std::size_t>(root.face) * 3 + 2 >= cornerUVs_.size()) {
        return {0.0f, 0.0f};
    }
    const std::size_t c = static_cast<std::size_t>(root.face) * 3;
    const float w = 1.0f - root.u - root.v;
    return {cornerUVs_[c][0] * w + cornerUVs_[c + 1][0] * root.u + cornerUVs_[c + 2][0] * root.v,
            cornerUVs_[c][1] * w + cornerUVs_[c + 1][1] * root.u + cornerUVs_[c + 2][1] * root.v};
}

const std::vector<ScalpMesh::Color>* ScalpMesh::cornerColors(const std::string& name) const {
    auto it = colorSets_.find(name);
    return it == colorSets_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Rest pose

void ScalpMesh::setRestPoints(std::vector<Vec3f> points) {
    if (!points.empty() && points.size() != points_.size()) {
        throw std::invalid_argument("ScalpMesh: rest points must match the mesh's point count");
    }
    restPoints_ = std::move(points);
    restAreas_.clear();
    restTotalArea_ = 0.0f;
    restHash_ = 0;
    if (restPoints_.empty()) {
        updateHash();
        return;
    }
    restAreas_.resize(triangles_.size());
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
        const Triangle& tri = triangles_[t];
        const Vec3f& a = restPoints_[tri[0]];
        const float area = 0.5f * (restPoints_[tri[1]] - a).cross(restPoints_[tri[2]] - a).length();
        restAreas_[t] = std::isfinite(area) ? area : 0.0f;
        restTotalArea_ += restAreas_[t];
    }
    restHash_ = hash64(0x2E57);
    for (const Vec3f& p : restPoints_) {
        const float xyz[3] = {p.x, p.y, p.z};
        restHash_ = hashFloats(restHash_, xyz, 3);
    }
    updateHash();
}

Vec3f ScalpMesh::restPositionOr(const StrandRoot& root, const Vec3f& fallback) const {
    if (restPoints_.empty() || root.face < 0 || static_cast<std::size_t>(root.face) >= triangles_.size()) {
        return fallback;
    }
    const Triangle& tri = triangles_[root.face];
    const float w = 1.0f - root.u - root.v;
    return restPoints_[tri[0]] * w + restPoints_[tri[1]] * root.u + restPoints_[tri[2]] * root.v;
}

std::uint64_t ScalpMesh::restPoseHash() const {
    if (restPoints_.empty()) return hash_;
    std::uint64_t h = hashCombine(hashCombine(hash64(0x2E5790), topologyHash_), uvHash_);
    h = hashCombine(h, restHash_);
    for (const auto& [name, c] : colorHashes_) h = hashCombine(h, c);
    return h;
}

std::uint64_t ScalpMesh::restSurfaceHash() const {
    if (restPoints_.empty()) return surfaceHash_;
    return hashCombine(hashCombine(hashCombine(hash64(0x2E5751), topologyHash_), uvHash_), restHash_);
}

void ScalpMesh::setCornerColors(const std::string& name, std::vector<Color> colors) {
    if (colors.size() != triangles_.size() * 3) {
        throw std::invalid_argument("ScalpMesh: expected one color per triangle corner");
    }
    std::uint64_t h = hashString(0, name);
    for (const Color& c : colors) h = hashFloats(h, c.data(), 4);
    colorHashes_[name] = h;
    colorSets_[name] = std::move(colors);
    updateHash();
}

void ScalpMesh::removeColorSet(const std::string& name) {
    if (!colorSets_.erase(name)) return;
    colorHashes_.erase(name);
    updateHash();
}

ScalpMesh::Color ScalpMesh::colorAt(const std::string& name, const StrandRoot& root) const {
    auto it = colorSets_.find(name);
    if (it == colorSets_.end() || !contains(root)) return {0.0f, 0.0f, 0.0f, 0.0f};
    const auto& colors = it->second;
    if (static_cast<std::size_t>(root.face) * 3 + 2 >= colors.size()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const std::size_t c = static_cast<std::size_t>(root.face) * 3;
    const float w = 1.0f - root.u - root.v;
    Color out{};
    for (int k = 0; k < 4; ++k) out[k] = colors[c][k] * w + colors[c + 1][k] * root.u + colors[c + 2][k] * root.v;
    return out;
}

std::vector<std::string> ScalpMesh::colorSetNames() const {
    std::vector<std::string> names;
    for (const auto& [name, colors] : colorSets_) names.push_back(name);
    return names;
}

// ---------------------------------------------------------------------------
// Closest point queries

namespace {

struct Aabb {
    Vec3f lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3f hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
             -std::numeric_limits<float>::max()};

    void expand(const Vec3f& p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    void expand(const Aabb& b) {
        expand(b.lo);
        expand(b.hi);
    }
    // Squared distance from p to the box (0 inside). The gap along each axis is the larger
    // of the distances past either face and 0, taken with the SSE instructions rather than
    // branches: a query tests dozens of boxes, and a mispredicted branch costs more than
    // all three axes' arithmetic.
    float distance2(const Vec3f& p) const {
#if GROOMLAB_SCALP_SSE
        const __m128 gap = _mm_max_ps(_mm_max_ps(_mm_sub_ps(_mm_setr_ps(lo.x, lo.y, lo.z, 0.0f), _mm_setr_ps(p.x, p.y, p.z, 0.0f)),
                                                 _mm_sub_ps(_mm_setr_ps(p.x, p.y, p.z, 0.0f), _mm_setr_ps(hi.x, hi.y, hi.z, 0.0f))),
                                      _mm_setzero_ps());
        alignas(16) float g[4];
        _mm_store_ps(g, _mm_mul_ps(gap, gap));
        return g[0] + g[1] + g[2];
#else
        // The same per axis: maxps(maxps(lo - p, p - hi), 0), each operation rounded alike.
        auto gap = [](float below, float above) {
            const float m = below > above ? below : above;
            return m > 0.0f ? m : 0.0f;
        };
        const float gx = gap(lo.x - p.x, p.x - hi.x), gy = gap(lo.y - p.y, p.y - hi.y), gz = gap(lo.z - p.z, p.z - hi.z);
        return gx * gx + gy * gy + gz * gz;
#endif
    }
};

constexpr std::uint32_t kLeafSize = 4;
constexpr int kMaxStack = 64;

}  // namespace

// Binary AABB tree over triangles: its shape (shared by every mesh made from this one by
// withPoints) and the boxes of one set of points. Leaves hold up to kLeafSize triangles (more
// when they all share one centroid); an inner node's children sit at `first` and `first + 1`,
// always after it, so walking the nodes backwards visits children before their parent.
struct ScalpMesh::BvhTree {
    struct Node {
        std::uint32_t first = 0;  // leaf: index into `order`; inner: left child
        std::uint32_t count = 0;  // leaf: triangle count; 0 for inner nodes
    };
    std::vector<Node> nodes;
    std::vector<std::uint32_t> order;  // triangle indices, grouped by leaf
};

struct ScalpMesh::Bvh {
    std::shared_ptr<const BvhTree> tree;
    std::vector<Aabb> boxes;  // per node, on this mesh's points
    bool refitted = false;    // the tree came from another mesh (withPoints)
    float scale = 0.0f;       // the largest coordinate magnitude of the points (query tolerances)
};

// A mesh's trees, made on first use. `bvh`: a tree built on its own points, or (withPoints of
// a mesh that had one) its source's tree refitted to these points. `own`: a tree built on its
// own points, made only when a query on a refitted tree is too close to call (see below).
struct ScalpMesh::LazyBvh {
    std::once_flag once;
    Bvh bvh;
    std::shared_ptr<const BvhTree> inherited;  // set by withPoints before the mesh is shared: refit this tree
    std::atomic<bool> built{false};             // bvh.tree is set (a mesh made from this one may inherit it)
    std::once_flag ownOnce;
    Bvh own;
    std::atomic<bool> ownBuilt{false};  // `own` is ready: later queries go straight to it
};

namespace {

// Each node's box on `points`, children before parents. Inner boxes are their children's
// union, which is the union of their triangles' boxes (min and max are exact): the boxes a
// build makes.
template <typename Tree, typename Triangles>
void fitBoxes(const Tree& tree, const Triangles& triangles, const std::vector<Vec3f>& points, std::vector<Aabb>& boxes) {
    boxes.assign(tree.nodes.size(), Aabb{});
    for (std::size_t i = tree.nodes.size(); i-- > 0;) {
        const auto& node = tree.nodes[i];
        Aabb& box = boxes[i];
        if (node.count > 0) {
            for (std::uint32_t k = node.first; k < node.first + node.count; ++k) {
                for (std::int32_t idx : triangles[tree.order[k]]) box.expand(points[static_cast<std::size_t>(idx)]);
            }
        } else if (node.first + 1 < boxes.size()) {
            box.expand(boxes[node.first]);
            box.expand(boxes[node.first + 1]);
        }
    }
}

float coordinateScale(const std::vector<Vec3f>& points) {
    float m = 0.0f;
    for (const Vec3f& p : points) m = std::max({m, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
    return std::isfinite(m) ? m : std::numeric_limits<float>::max();
}

// Rounding allowance of a query on a refitted tree: float error in points of this size is a few
// units in the last place; this is a hundred times more.
float tolerance(float scale, const Vec3f& p) {
    return 1e-5f * std::max({scale, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z), 1e-30f});
}

}  // namespace

void ScalpMesh::buildTree(Bvh& b) const {
    auto tree = std::make_shared<BvhTree>();
    const std::size_t n = triangles_.size();
    std::vector<Vec3f> centroids(n);
    for (std::size_t t = 0; t < n; ++t) {
        Aabb box;
        for (std::int32_t idx : triangles_[t]) box.expand(points_[idx]);
        centroids[t] = (box.lo + box.hi) * 0.5f;
    }
    BvhTree& tr = *tree;
    tr.order.resize(n);
    for (std::size_t t = 0; t < n; ++t) tr.order[t] = static_cast<std::uint32_t>(t);
    tr.nodes.reserve(2 * n / kLeafSize + 1);
    tr.nodes.emplace_back();

    // Iterative median-split build over ranges of `order`.
    struct Task {
        std::uint32_t node, begin, end;
    };
    std::vector<Task> stack{{0, 0, static_cast<std::uint32_t>(n)}};
    while (!stack.empty()) {
        const Task task = stack.back();
        stack.pop_back();
        Aabb centroidBox;
        for (std::uint32_t i = task.begin; i < task.end; ++i) centroidBox.expand(centroids[tr.order[i]]);
        const Vec3f extent = centroidBox.hi - centroidBox.lo;
        const std::uint32_t count = task.end - task.begin;
        if (count <= kLeafSize || std::max({extent.x, extent.y, extent.z}) <= 0.0f) {
            // A leaf's triangles in index order (the first of equally near ones wins a query).
            std::sort(tr.order.begin() + task.begin, tr.order.begin() + task.end);
            tr.nodes[task.node].first = task.begin;
            tr.nodes[task.node].count = count;
            continue;
        }
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        // Equal centroids in index order: a total order, so each half holds the same triangles
        // whatever the standard library's nth_element leaves them in, and the tree is the same.
        const std::uint32_t mid = task.begin + count / 2;
        std::nth_element(tr.order.begin() + task.begin, tr.order.begin() + mid, tr.order.begin() + task.end,
                         [&](std::uint32_t a, std::uint32_t c) {
                             const Vec3f& ca = centroids[a];
                             const Vec3f& cc = centroids[c];
                             const float ka = axis == 0 ? ca.x : axis == 1 ? ca.y : ca.z;
                             const float kc = axis == 0 ? cc.x : axis == 1 ? cc.y : cc.z;
                             return ka < kc || (ka == kc && a < c);
                         });
        const auto left = static_cast<std::uint32_t>(tr.nodes.size());
        tr.nodes.emplace_back();
        tr.nodes.emplace_back();
        tr.nodes[task.node].first = left;
        tr.nodes[task.node].count = 0;
        stack.push_back({left, task.begin, mid});
        stack.push_back({left + 1, mid, task.end});
    }
    fitBoxes(tr, triangles_, points_, b.boxes);
    b.tree = std::move(tree);
    b.refitted = false;
    b.scale = coordinateScale(points_);
}

const ScalpMesh::Bvh& ScalpMesh::bvh() const {
    std::call_once(bvh_->once, [this] {
        Bvh& b = bvh_->bvh;
        if (bvh_->inherited) {
            // The source mesh's tree on these points: no splitting, each box again.
            fitBoxes(*bvh_->inherited, triangles_, points_, b.boxes);
            b.tree = bvh_->inherited;
            b.refitted = true;
            b.scale = coordinateScale(points_);
        } else {
            buildTree(b);
        }
        bvh_->built.store(true, std::memory_order_release);
    });
    return bvh_->bvh;
}

const ScalpMesh::Bvh& ScalpMesh::ownBvh() const {
    const Bvh& b = bvh();
    if (!b.refitted) return b;
    std::call_once(bvh_->ownOnce, [this] {
        buildTree(bvh_->own);
        bvh_->ownBuilt.store(true, std::memory_order_release);
    });
    return bvh_->own;
}

bool ScalpMesh::treeRefitted() const { return bvh().refitted; }

ScalpMesh ScalpMesh::withPoints(std::vector<Vec3f> points) const {
    if (points.size() != points_.size()) {
        throw std::invalid_argument("ScalpMesh: new points must match the mesh's point count");
    }
    // What a mesh built from these points with the same UVs, color sets and rest pose would
    // hold; only what depends on the points is worked out again.
    ScalpMesh out;
    out.points_ = std::move(points);
    out.triangles_ = triangles_;
    out.topologyHash_ = topologyHash_;
    out.buildGeometry(false);
    out.cornerUVs_ = cornerUVs_;
    out.uvHash_ = uvHash_;
    out.colorSets_ = colorSets_;
    out.colorHashes_ = colorHashes_;
    out.restPoints_ = restPoints_;
    out.restAreas_ = restAreas_;
    out.restTotalArea_ = restTotalArea_;
    out.restHash_ = restHash_;
    out.polygons_ = polygons_;
    out.polygonHash_ = polygonHash_;
    out.renderSubdivision_ = renderSubdivision_;
    out.renderSubdivisionFrom_ = renderSubdivisionFrom_;
    out.subdivisionLevel_ = subdivisionLevel_;
    out.cageFaces_ = cageFaces_;
    out.cage_ = cage_;
    out.pieces_ = pieces_;
    out.piecesHash_ = piecesHash_;
    out.updateHash();
    out.bvh_ = std::make_shared<LazyBvh>();
    if (bvh_ && bvh_->inherited) {
        out.bvh_->inherited = bvh_->inherited;  // itself a refit (queried or not): the same tree
    } else if (bvh_ && bvh_->built.load(std::memory_order_acquire)) {
        out.bvh_->inherited = bvh_->bvh.tree;  // its own tree: refitted on the first query
    }
    return out;
}

bool ScalpMesh::samePointsLayout(const ScalpMesh& other) const {
    return points_.size() == other.points_.size() && triangles_.size() == other.triangles_.size() &&
           topologyHash_ == other.topologyHash_ && uvHash_ == other.uvHash_ && restHash_ == other.restHash_ &&
           colorHashes_ == other.colorHashes_ && polygonHash_ == other.polygonHash_ &&
           renderSubdivision_ == other.renderSubdivision_ && piecesHash_ == other.piecesHash_;
}

// Queries. On a tree built on the mesh's own points they search as they always have: the
// nearest triangle, ties to the first met. A refitted tree (another split of the same
// triangles) meets triangles in another order, so it gives its answer only when that answer
// does not depend on the order: no other triangle within a rounding allowance of the best (a
// point nearest a vertex shared by several faces, a ray through an edge) and nothing pruned
// that close. Otherwise the query runs on a tree built on the mesh's own points (made once,
// when first needed), so every answer is the one a freshly built mesh gives.

SurfacePoint ScalpMesh::closestPoint(const Vec3f& p, float maxDistance) const {
    return closestPointBounded(p, maxDistance, nullptr);
}

SurfacePoint ScalpMesh::closestOnTree(const Bvh& b, const Vec3f& p, float maxDistance, float* lowerBound) const {
    SurfacePoint best;
    const BvhTree& tree = *b.tree;
    float bestDist = maxDistance < std::sqrt(std::numeric_limits<float>::max()) ? maxDistance * maxDistance
                                                                                : std::numeric_limits<float>::max();
    float pruned = std::numeric_limits<float>::infinity();  // the nearest box skipped (squared)

    // Median splits keep depth near log2(n / kLeafSize), far below kMaxStack. Each entry
    // carries its box's distance, worked out once when its parent ordered its children.
    struct Entry {
        std::uint32_t node;
        float boxDist;
    };
    Entry stack[kMaxStack];
    int top = 0;
    stack[top++] = {0, b.boxes[0].distance2(p)};
    while (top > 0) {
        const Entry e = stack[--top];
        const std::uint32_t at = e.node;
        const BvhTree::Node& node = tree.nodes[at];
        const float boxDist = e.boxDist;
        if (boxDist >= bestDist) {
            pruned = std::min(pruned, boxDist);
            continue;
        }
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const std::uint32_t t = tree.order[i];
                const Triangle& tri = triangles_[t];
                const Vec3f &a = points_[tri[0]], &bb = points_[tri[1]], &c = points_[tri[2]];
                float u = 0, v = 0;
                closestBarycentric(p, a, bb, c, u, v);
                const Vec3f q = a * (1.0f - u - v) + bb * u + c * v;  // position(root), the vertices in hand
                const float d = (q - p).dot(q - p);
                if (d < bestDist) {
                    bestDist = d;
                    best.root = StrandRoot{static_cast<std::int32_t>(t), u, v};
                    best.position = q;
                } else {
                    pruned = std::min(pruned, d);  // at or past the bound: it bounds a miss too
                }
            }
            continue;
        }
        // Visit the nearer child first: push it last.
        const std::uint32_t l = node.first, r = node.first + 1;
        const float dl = b.boxes[l].distance2(p), dr = b.boxes[r].distance2(p);
        const bool leftNearer = dl <= dr;
        stack[top++] = leftNearer ? Entry{r, dr} : Entry{l, dl};
        stack[top++] = leftNearer ? Entry{l, dl} : Entry{r, dr};
    }
    if (best.root.face >= 0) best.normal = normal(best.root);
    if (lowerBound) {
        // A hit is the nearest point (its own distance); a miss pruned every box at maxDistance or
        // farther, so the nearest pruned box bounds every triangle.
        *lowerBound = std::sqrt(best.root.face >= 0 ? bestDist : pruned);
    }
    return best;
}

SurfacePoint ScalpMesh::closestPointBounded(const Vec3f& p, float maxDistance, float* lowerBound) const {
    if (triangles_.empty()) {
        if (lowerBound) *lowerBound = std::numeric_limits<float>::infinity();
        return SurfacePoint{};
    }
    const Bvh& b = bvh();
    if (!b.refitted) return closestOnTree(b, p, maxDistance, lowerBound);
    // A query already needed the mesh's own tree: it answers the rest too (as cheaply).
    if (bvh_->ownBuilt.load(std::memory_order_acquire)) return closestOnTree(bvh_->own, p, maxDistance, lowerBound);

    // The refitted tree: the nearest triangle, and the nearest of everything else (other
    // triangles, boxes not opened), in distance (not squared) for a rounding allowance in length.
    const BvhTree& tree = *b.tree;
    const float tol = tolerance(b.scale, p);
    const float limit = maxDistance < std::sqrt(std::numeric_limits<float>::max()) ? maxDistance * maxDistance
                                                                                  : std::numeric_limits<float>::max();
    SurfacePoint best;
    float bestDist = std::numeric_limits<float>::max();  // squared; the best triangle's, any distance
    float other = std::numeric_limits<float>::max();     // squared: the nearest other candidate
    // Boxes nearer than this may hold the answer or a near tie: the best (or the bound) plus the
    // allowance, squared. Worked out again only when the best changes.
    auto reachOf = [&](float dist) {
        const float reach = std::min(dist, limit);
        if (reach >= std::numeric_limits<float>::max()) return reach;
        const float r = std::sqrt(reach) + tol;
        return r * r;
    };
    float open = reachOf(bestDist);
    std::uint32_t stack[kMaxStack];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const std::uint32_t at = stack[--top];
        const BvhTree::Node& node = tree.nodes[at];
        const float boxDist = b.boxes[at].distance2(p);
        if (boxDist > open) {
            other = std::min(other, boxDist);
            continue;
        }
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const std::uint32_t t = tree.order[i];
                const Triangle& tri = triangles_[t];
                float u = 0, v = 0;
                closestBarycentric(p, points_[tri[0]], points_[tri[1]], points_[tri[2]], u, v);
                const StrandRoot root{static_cast<std::int32_t>(t), u, v};
                const Vec3f q = position(root);
                const float d = (q - p).dot(q - p);
                if (d < bestDist) {
                    if (!(q.x >= std::min({points_[tri[0]].x, points_[tri[1]].x, points_[tri[2]].x}) - tol &&
                          q.x <= std::max({points_[tri[0]].x, points_[tri[1]].x, points_[tri[2]].x}) + tol &&
                          q.y >= std::min({points_[tri[0]].y, points_[tri[1]].y, points_[tri[2]].y}) - tol &&
                          q.y <= std::max({points_[tri[0]].y, points_[tri[1]].y, points_[tri[2]].y}) + tol &&
                          q.z >= std::min({points_[tri[0]].z, points_[tri[1]].z, points_[tri[2]].z}) - tol &&
                          q.z <= std::max({points_[tri[0]].z, points_[tri[1]].z, points_[tri[2]].z}) + tol)) {
                        // Off its triangle's box (a degenerate triangle's): boxes do not bound it.
                        return closestOnTree(ownBvh(), p, maxDistance, lowerBound);
                    }
                    other = std::min(other, bestDist);
                    bestDist = d;
                    open = reachOf(bestDist);
                    best.root = root;
                    best.position = q;
                } else {
                    other = std::min(other, d);
                }
            }
            continue;
        }
        const std::uint32_t l = node.first, r = node.first + 1;
        const bool leftNearer = b.boxes[l].distance2(p) <= b.boxes[r].distance2(p);
        stack[top++] = leftNearer ? r : l;
        stack[top++] = leftNearer ? l : r;
    }
    // Clear: the best stands apart from everything else, and from the bound, by the allowance.
    const float near = best.root.face >= 0 ? std::sqrt(bestDist) + tol : 0.0f;
    const bool apart = best.root.face < 0 || other > near * near;
    const float edge = std::sqrt(limit);
    const bool clearOfBound = limit >= std::numeric_limits<float>::max() || best.root.face < 0 ||
                              std::fabs(std::sqrt(bestDist) - edge) > tol;
    if (!apart || !clearOfBound) return closestOnTree(ownBvh(), p, maxDistance, lowerBound);
    if (bestDist < limit) {
        best.normal = normal(best.root);
        if (lowerBound) *lowerBound = std::sqrt(bestDist);
        return best;
    }
    // A miss: nothing within maxDistance (the nearest triangle, if seen, is past it by more than
    // the allowance), and everything not looked at is at least as far as `other`.
    if (lowerBound) {
        const float bound = std::min(bestDist, other);
        *lowerBound = std::sqrt(std::max(bound, limit));
    }
    return SurfacePoint{};
}

SurfacePoint ScalpMesh::closestPointBruteForce(const Vec3f& p) const {
    SurfacePoint best;
    float bestDist = std::numeric_limits<float>::max();
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
        const Triangle& tri = triangles_[t];
        float u = 0, v = 0;
        closestBarycentric(p, points_[tri[0]], points_[tri[1]], points_[tri[2]], u, v);
        const StrandRoot root{static_cast<std::int32_t>(t), u, v};
        const Vec3f q = position(root);
        const float d = (q - p).dot(q - p);
        if (d < bestDist) {
            bestDist = d;
            best.root = root;
            best.position = q;
        }
    }
    if (best.root.face >= 0) best.normal = normal(best.root);
    return best;
}

namespace {

// Slab test: entry distance of the ray into the box, or +inf on a miss.
float rayBoxEntry(const Aabb& box, const Vec3f& origin, const Vec3f& invDir, float maxT) {
    float t0 = 0.0f, t1 = maxT;
    const float o[3] = {origin.x, origin.y, origin.z};
    const float inv[3] = {invDir.x, invDir.y, invDir.z};
    const float lo[3] = {box.lo.x, box.lo.y, box.lo.z};
    const float hi[3] = {box.hi.x, box.hi.y, box.hi.z};
    for (int a = 0; a < 3; ++a) {
        float tNear = (lo[a] - o[a]) * inv[a];
        float tFar = (hi[a] - o[a]) * inv[a];
        if (tNear > tFar) std::swap(tNear, tFar);
        t0 = std::max(t0, tNear);
        t1 = std::min(t1, tFar);
        if (t0 > t1) return std::numeric_limits<float>::infinity();
    }
    return t0;
}

// Whether `q` lies in the triangle's box grown by `pad`: an answer from a degenerate triangle (a
// garbage barycentric or ray distance) does not, and a tree's boxes do not bound it.
bool inTriangleBox(const Vec3f& q, const Vec3f& a, const Vec3f& b, const Vec3f& c, float pad) {
    return q.x >= std::min({a.x, b.x, c.x}) - pad && q.x <= std::max({a.x, b.x, c.x}) + pad &&
           q.y >= std::min({a.y, b.y, c.y}) - pad && q.y <= std::max({a.y, b.y, c.y}) + pad &&
           q.z >= std::min({a.z, b.z, c.z}) - pad && q.z <= std::max({a.z, b.z, c.z}) + pad;
}

// The box grown by `pad` on every side (a conservative test).
Aabb padded(const Aabb& box, float pad) {
    Aabb out;
    out.lo = box.lo - Vec3f(pad, pad, pad);
    out.hi = box.hi + Vec3f(pad, pad, pad);
    return out;
}

// Moller-Trumbore, two-sided. Returns t, or -1 on a miss; u, v follow StrandRoot.
float rayTriangle(const Vec3f& o, const Vec3f& d, const Vec3f& a, const Vec3f& b, const Vec3f& c, float& u, float& v) {
    const Vec3f e1 = b - a, e2 = c - a;
    const Vec3f p = d.cross(e2);
    const float det = e1.dot(p);
    // Parallel to the triangle (or a degenerate one): relative to the lengths, as det goes with
    // their product (a cube of the scene's scale).
    if (!(std::fabs(det) > 1e-7f * d.length() * e1.length() * e2.length())) return -1.0f;
    const float inv = 1.0f / det;
    const Vec3f s = o - a;
    u = s.dot(p) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const Vec3f q = s.cross(e1);
    v = d.dot(q) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    const float t = e2.dot(q) * inv;
    return t >= 0.0f ? t : -1.0f;
}

}  // namespace

bool ScalpMesh::rayOnTree(const Bvh& b, const Vec3f& origin, const Vec3f& direction, SurfacePoint* hit, float* tOut) const {
    const BvhTree& tree = *b.tree;
    const auto safeInv = [](float x) { return std::fabs(x) > 1e-30f ? 1.0f / x : std::copysign(1e30f, x); };
    const Vec3f invDir(safeInv(direction.x), safeInv(direction.y), safeInv(direction.z));

    float bestT = std::numeric_limits<float>::max();
    StrandRoot bestRoot;
    std::uint32_t stack[kMaxStack];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const std::uint32_t at = stack[--top];
        const BvhTree::Node& node = tree.nodes[at];
        if (rayBoxEntry(b.boxes[at], origin, invDir, bestT) == std::numeric_limits<float>::infinity()) continue;
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const std::uint32_t t = tree.order[i];
                const Triangle& tri = triangles_[t];
                float u = 0, v = 0;
                const float d = rayTriangle(origin, direction, points_[tri[0]], points_[tri[1]], points_[tri[2]], u, v);
                if (d >= 0.0f && d < bestT) {
                    bestT = d;
                    bestRoot = {static_cast<std::int32_t>(t), u, v};
                }
            }
            continue;
        }
        stack[top++] = node.first;
        stack[top++] = node.first + 1;
    }
    if (bestRoot.face < 0) return false;
    if (hit) {
        hit->root = bestRoot;
        hit->position = position(bestRoot);
        hit->normal = normal(bestRoot);
    }
    if (tOut) *tOut = bestT;
    return true;
}

std::shared_ptr<const std::vector<std::uint32_t>> ScalpMesh::rayTree() const {
    if (triangles_.empty()) return nullptr;
    const Bvh& b = ownBvh();
    const BvhTree& tree = *b.tree;
    auto words = std::make_shared<std::vector<std::uint32_t>>(kRayTreeHeader, 0u);
    auto put = [&](const void* data, std::size_t count) {
        const std::size_t at = words->size();
        words->resize(at + count);
        if (count) std::memcpy(words->data() + at, data, count * 4);
        return static_cast<std::uint32_t>(at);
    };
    std::vector<std::uint32_t> nodes(tree.nodes.size() * 2);
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        nodes[i * 2] = tree.nodes[i].first;
        nodes[i * 2 + 1] = tree.nodes[i].count;
    }
    std::vector<float> boxes(b.boxes.size() * 6);
    for (std::size_t i = 0; i < b.boxes.size(); ++i) {
        const Aabb& box = b.boxes[i];
        const float six[6] = {box.lo.x, box.lo.y, box.lo.z, box.hi.x, box.hi.y, box.hi.z};
        std::memcpy(boxes.data() + i * 6, six, sizeof six);
    }
    std::vector<std::uint32_t> tris(triangles_.size() * 3);
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
        for (int c = 0; c < 3; ++c) tris[t * 3 + c] = static_cast<std::uint32_t>(triangles_[t][static_cast<std::size_t>(c)]);
    }
    (*words)[kRayTreeNodeCount] = static_cast<std::uint32_t>(tree.nodes.size());
    (*words)[kRayTreeNodes] = put(nodes.data(), nodes.size());
    (*words)[kRayTreeBoxes] = put(boxes.data(), boxes.size());
    (*words)[kRayTreeOrder] = put(tree.order.data(), tree.order.size());
    (*words)[kRayTreePoints] = put(points_.data(), points_.size() * 3);
    (*words)[kRayTreeNormals] = put(vertexNormals_.data(), vertexNormals_.size() * 3);
    (*words)[kRayTreeTriangles] = put(tris.data(), tris.size());
    return words;
}

bool ScalpMesh::intersectRay(const Vec3f& origin, const Vec3f& direction, SurfacePoint* hit, float* tOut) const {
    if (triangles_.empty() || direction.length() == 0.0f) return false;
    const Bvh& b = bvh();
    if (!b.refitted) return rayOnTree(b, origin, direction, hit, tOut);
    if (bvh_->ownBuilt.load(std::memory_order_acquire)) return rayOnTree(bvh_->own, origin, direction, hit, tOut);

    // The refitted tree, conservatively (boxes grown by the allowance): the nearest hit and the
    // next one. Too close to call (a ray through an edge or vertex, or near one): the own tree.
    const BvhTree& tree = *b.tree;
    const float tol = tolerance(b.scale, origin);
    const float tolT = tol / direction.length();
    const auto safeInv = [](float x) { return std::fabs(x) > 1e-30f ? 1.0f / x : std::copysign(1e30f, x); };
    const Vec3f invDir(safeInv(direction.x), safeInv(direction.y), safeInv(direction.z));
    float bestT = std::numeric_limits<float>::max(), nextT = std::numeric_limits<float>::max();
    StrandRoot bestRoot;
    std::uint32_t stack[kMaxStack];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const std::uint32_t at = stack[--top];
        const BvhTree::Node& node = tree.nodes[at];
        const float reach = bestT < std::numeric_limits<float>::max() ? bestT + tolT : bestT;
        if (rayBoxEntry(padded(b.boxes[at], tol), origin, invDir, reach) == std::numeric_limits<float>::infinity()) continue;
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const std::uint32_t t = tree.order[i];
                const Triangle& tri = triangles_[t];
                float u = 0, v = 0;
                const float d = rayTriangle(origin, direction, points_[tri[0]], points_[tri[1]], points_[tri[2]], u, v);
                if (d < 0.0f) continue;
                if (d < bestT) {
                    // A hit off its triangle's box (a degenerate triangle's): boxes do not bound
                    // it, so only a tree built on these points answers as one always has.
                    if (!inTriangleBox(origin + direction * d, points_[tri[0]], points_[tri[1]], points_[tri[2]], tol)) {
                        return rayOnTree(ownBvh(), origin, direction, hit, tOut);
                    }
                    nextT = bestT;
                    bestT = d;
                    bestRoot = {static_cast<std::int32_t>(t), u, v};
                } else {
                    nextT = std::min(nextT, d);
                }
            }
            continue;
        }
        stack[top++] = node.first;
        stack[top++] = node.first + 1;
    }
    if (bestRoot.face < 0) {
        // No triangle passed its test. A box the ray grazes is the same on any tree up to the
        // allowance (grown here): nothing is near enough to hit either.
        return false;
    }
    // Near an edge or vertex of its triangle, a neighbor may hit at the same distance (or be the
    // one a built tree meets first): the own tree decides.
    const float w = 1.0f - bestRoot.u - bestRoot.v;
    const float kEdge = 1e-4f;
    const bool nearEdge = bestRoot.u < kEdge || bestRoot.v < kEdge || w < kEdge;
    if (nearEdge || nextT <= bestT + tolT) return rayOnTree(ownBvh(), origin, direction, hit, tOut);
    if (hit) {
        hit->root = bestRoot;
        hit->position = position(bestRoot);
        hit->normal = normal(bestRoot);
    }
    if (tOut) *tOut = bestT;
    return true;
}

double poseFreeArea(const ScalpMesh& scalp, const std::vector<Vec3f>& points) {
    const auto& triangles = scalp.triangles();
    if (!scalp.hasRest() || points.size() != scalp.restPoints().size()) return surfaceArea(points, triangles);
    std::vector<double> ratios;
    ratios.reserve(triangles.size());
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        const ScalpMesh::Triangle& tri = triangles[t];
        const double rest = scalp.restTriangleArea(t);
        if (!(rest > 0.0) || tri[0] < 0 || tri[1] < 0 || tri[2] < 0) continue;
        const auto a = static_cast<std::size_t>(tri[0]), b = static_cast<std::size_t>(tri[1]), c = static_cast<std::size_t>(tri[2]);
        if (a >= points.size() || b >= points.size() || c >= points.size()) continue;
        const double posed = 0.5 * (points[b] - points[a]).cross(points[c] - points[a]).length();
        ratios.push_back(posed / rest);
    }
    if (ratios.empty()) return surfaceArea(points, triangles);
    auto middle = ratios.begin() + static_cast<std::ptrdiff_t>(ratios.size() / 2);
    std::nth_element(ratios.begin(), middle, ratios.end());
    return restSurfaceArea(scalp) * *middle;
}

double restSurfaceArea(const ScalpMesh& scalp) {
    double area = 0.0;
    for (std::size_t t = 0; t < scalp.triangleCount(); ++t) area += scalp.restTriangleArea(t);
    return area;
}

double surfaceArea(const std::vector<Vec3f>& points, const std::vector<ScalpMesh::Triangle>& triangles) {
    double area = 0.0;
    for (const ScalpMesh::Triangle& t : triangles) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0) continue;
        const auto a = static_cast<std::size_t>(t[0]), b = static_cast<std::size_t>(t[1]), c = static_cast<std::size_t>(t[2]);
        if (a >= points.size() || b >= points.size() || c >= points.size()) continue;
        const float x = 0.5f * (points[b] - points[a]).cross(points[c] - points[a]).length();
        if (std::isfinite(x)) area += x;
    }
    return area;
}

bool scaleToArea(std::vector<Vec3f>& points, const std::vector<ScalpMesh::Triangle>& triangles, double area) {
    if (points.empty() || !(area > 0.0) || !std::isfinite(area)) return false;
    const double now = surfaceArea(points, triangles);
    if (!(now > 0.0) || std::abs(area / now - 1.0) < 1e-6) return false;
    const auto s = static_cast<float>(std::sqrt(area / now));
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (const Vec3f& p : points) {
        cx += p.x;
        cy += p.y;
        cz += p.z;
    }
    const auto n = static_cast<double>(points.size());
    const Vec3f center(static_cast<float>(cx / n), static_cast<float>(cy / n), static_cast<float>(cz / n));
    for (Vec3f& p : points) p = center + (p - center) * s;
    return true;
}

std::shared_ptr<const ScalpMesh> withRestArea(const std::shared_ptr<const ScalpMesh>& scalp, double area) {
    if (!scalp || !scalp->hasRest()) return scalp;
    std::vector<Vec3f> rest = scalp->restPoints();
    if (!scaleToArea(rest, scalp->triangles(), area)) return scalp;
    auto out = std::make_shared<ScalpMesh>(*scalp);
    out->setRestPoints(std::move(rest));
    return out;
}

bool isTriangulationOf(const std::vector<ScalpMesh::Triangle>& triangles, const std::vector<std::int32_t>& trianglesPerPolygon,
                       const std::vector<std::int32_t>& loopCounts, const std::vector<std::int32_t>& loops) {
    if (trianglesPerPolygon.size() != loopCounts.size()) return false;
    std::size_t t = 0, l = 0;
    std::vector<std::pair<int, int>> edges;  // a polygon's directed edges, as places along its loop
    for (std::size_t f = 0; f < loopCounts.size(); ++f) {
        const std::int32_t n = loopCounts[f], k = trianglesPerPolygon[f];
        if (n < 3 || k < 1 || l + static_cast<std::size_t>(n) > loops.size() || t + static_cast<std::size_t>(k) > triangles.size()) {
            return false;
        }
        const std::int32_t* loop = loops.data() + l;
        for (std::int32_t i = 0; i < n; ++i) {
            for (std::int32_t j = i + 1; j < n; ++j) {
                if (loop[i] == loop[j]) return false;
            }
        }
        auto place = [&](std::int32_t vertex) {
            for (std::int32_t i = 0; i < n; ++i) {
                if (loop[i] == vertex) return i;
            }
            return -1;
        };
        edges.clear();
        for (std::int32_t j = 0; j < k; ++j) {
            const ScalpMesh::Triangle& tri = triangles[t + static_cast<std::size_t>(j)];
            int corners[3];
            for (int c = 0; c < 3; ++c) {
                corners[c] = place(tri[c]);
                if (corners[c] < 0) return false;
            }
            for (int c = 0; c < 3; ++c) {
                if (corners[c] == corners[(c + 1) % 3]) return false;
                edges.emplace_back(corners[c], corners[(c + 1) % 3]);
            }
        }
        std::sort(edges.begin(), edges.end());
        if (std::adjacent_find(edges.begin(), edges.end()) != edges.end()) return false;  // an edge used twice the same way
        auto has = [&](int a, int b) { return std::binary_search(edges.begin(), edges.end(), std::make_pair(a, b)); };
        auto onLoop = [&](int a, int b) { return b == (a + 1) % n; };
        for (int i = 0; i < n; ++i) {
            if (!has(i, (i + 1) % n) || has((i + 1) % n, i)) return false;  // the loop's edges, each its way
        }
        for (const auto& [a, b] : edges) {
            if (!onLoop(a, b) && !has(b, a)) return false;  // a diagonal, both ways
        }
        t += static_cast<std::size_t>(k);
        l += static_cast<std::size_t>(n);
    }
    return t == triangles.size() && l == loops.size();
}

}  // namespace groomlab
