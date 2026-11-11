// SPDX-License-Identifier: Apache-2.0
#include "groomlab/portable_math.h"
#include "groomlab/scalp_paint.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include "groomlab/parallel.h"
#include "groomlab/png.h"
#include "groomlab/point_index.h"

namespace groomlab {

float compositePaintValue(float current, float value, PaintMode mode) {
    switch (mode) {
        case PaintMode::Add: return std::clamp(current + value, 0.0f, 1.0f);
        case PaintMode::Subtract: return std::clamp(current - value, 0.0f, 1.0f);
        case PaintMode::ClampMaximum: return std::min(current, std::clamp(value, 0.0f, 1.0f));
        case PaintMode::Replace: return std::clamp(value, 0.0f, 1.0f);
    }
    return std::clamp(value, 0.0f, 1.0f);
}

void fillMapValues(std::vector<float>& values, int channels, unsigned mask, const PaintColor& color, PaintMode mode) {
    if (channels != kGrayMap && channels != kColorMap) {
        throw std::invalid_argument("fillMapValues: a map has 1 channel (gray) or 4 (color)");
    }
    const auto n = static_cast<std::size_t>(channels);
    if (values.size() % n != 0) throw std::invalid_argument("fillMapValues: incomplete map value");
    const unsigned m = channels == kGrayMap ? 1u : (mask & kAllChannels);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (m & (1u << (i % n))) values[i] = compositePaintValue(values[i], color[i % n], mode);
    }
}

namespace {

// Scalp maps are one image over the UV square: a UDIM tile set is refused.
void requireOneImage(const Texture& image) {
    if (image.isUdim()) {
        throw std::invalid_argument("a scalp map is one image over UVs 0-1: UDIM tile sets can't be used as scalp maps");
    }
}

float luminance(const std::array<float, 4>& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// The image's luminance at a UV, clamped to the texel centers at the border so an edge of
// the scalp on the UV border does not blend with the opposite edge (Texture wraps).
float luminanceAt(const Texture& image, float u, float v) {
    const float hu = 0.5f / static_cast<float>(std::max(image.width(), 1));
    const float hv = 0.5f / static_cast<float>(std::max(image.height(), 1));
    return luminance(image.sample(std::clamp(u, hu, 1.0f - hu), std::clamp(v, hv, 1.0f - hv)));
}

// The image's color at a UV, clamped to the texel centers as luminanceAt.
PaintColor imageColorAt(const Texture& image, float u, float v) {
    const float hu = 0.5f / static_cast<float>(std::max(image.width(), 1));
    const float hv = 0.5f / static_cast<float>(std::max(image.height(), 1));
    return image.sample(std::clamp(u, hu, 1.0f - hu), std::clamp(v, hv, 1.0f - hv));
}

void requireChannels(int channels, const char* where) {
    if (channels != kGrayMap && channels != kColorMap) {
        throw std::invalid_argument(std::string(where) + ": a map has 1 channel (gray) or 4 (color)");
    }
}

// The channels a stamp changes: all of a gray map's one, the mask's of a color map.
unsigned effectiveMask(int channels, unsigned mask) { return channels == kGrayMap ? 1u : (mask & kAllChannels); }

// The smoothing of touched texels (index, weight) into `next` (N values each), from the values
// before it: each texel toward the mean of itself and its neighbors on the surface (the 8 beside
// it inside its island, a seam texel's across its seams), every channel summed per neighbor in
// the same order. N fixed, so the channel loops unroll. Each texel's result is its own: big dabs
// run in parallel, the same values.
template <int N>
void smoothTexels(const float* values, const TexelTable& table, unsigned mask,
                  const std::vector<std::pair<std::size_t, float>>& touched, float* next) {
    const auto width = static_cast<std::size_t>(table.width);
    auto one = [&](std::size_t t) {
        const std::size_t i = touched[t].first;
        const float w = touched[t].second;
        const float* own = values + i * N;
        float sum[N];
        for (int c = 0; c < N; ++c) sum[c] = own[c];
        int n = 1;
        if (table.covered[i] == 2) {
            const auto at = std::lower_bound(table.seamTexels.begin(), table.seamTexels.end(), static_cast<std::uint32_t>(i));
            const std::uint32_t* around = table.seamNeighbors.data() + static_cast<std::size_t>(at - table.seamTexels.begin()) * 8;
            for (int k = 0; k < 8; ++k) {
                if (around[k] == TexelTable::kNoTexel) continue;
                const float* v = values + static_cast<std::size_t>(around[k]) * N;
                for (int c = 0; c < N; ++c) sum[c] += v[c];
                ++n;
            }
        } else {
            // Inside an island: all 8 beside it are covered by it (else it would be a seam texel).
            for (const std::size_t row : {i - width, i, i + width}) {
                for (const std::size_t j : {row - 1, row, row + 1}) {
                    if (j == i) continue;
                    const float* v = values + j * N;
                    for (int c = 0; c < N; ++c) sum[c] += v[c];
                    ++n;
                }
            }
        }
        float* out = next + t * N;
        for (int c = 0; c < N; ++c) {
            out[c] = (mask & (1u << c)) ? std::clamp(own[c] + (sum[c] / static_cast<float>(n) - own[c]) * w, 0.0f, 1.0f) : own[c];
        }
    };
    if (touched.size() >= 8192) {
        parallelFor(0, touched.size(), one, 2048);
    } else {
        for (std::size_t t = 0; t < touched.size(); ++t) one(t);
    }
}

}  // namespace

// ---- Channels -----------------------------------------------------------------

unsigned channelMask(const std::string& letters) {
    unsigned mask = 0;
    for (const char c : letters) {
        const char* at = std::strchr("rgba", std::tolower(static_cast<unsigned char>(c)));
        if (c == '\0' || !at) throw std::invalid_argument("channels are letters of rgba, not '" + letters + "'");
        mask |= 1u << static_cast<unsigned>(at - "rgba");
    }
    if (mask == 0) throw std::invalid_argument("name at least one channel of rgba");
    return mask;
}

std::string channelLetters(unsigned mask) {
    std::string out;
    for (unsigned c = 0; c < 4; ++c) {
        if (mask & (1u << c)) out += "rgba"[c];
    }
    return out;
}

float paintLuminance(const PaintColor& c) { return luminance(c); }

std::vector<float> convertMapChannels(const std::vector<float>& values, int from, int to) {
    requireChannels(from, "convertMapChannels");
    requireChannels(to, "convertMapChannels");
    if (from == to) return values;
    const std::size_t n = values.size() / static_cast<std::size_t>(from);
    std::vector<float> out;
    out.reserve(n * static_cast<std::size_t>(to));
    for (std::size_t i = 0; i < n; ++i) {
        if (to == kColorMap) {
            const float v = values[i];
            out.insert(out.end(), {v, v, v, 1.0f});
        } else {
            const float* c = &values[i * 4];
            out.push_back(std::clamp(luminance({c[0], c[1], c[2], c[3]}), 0.0f, 1.0f));
        }
    }
    return out;
}

// ---- The surface a brush walks ------------------------------------------------

namespace {

// Each point's stand-in: the first point within a millionth of the points' extent of it (a grid
// of cells that size, the 27 around a point searched).
std::vector<std::uint32_t> weldPoints(const std::vector<Vec3f>& points) {
    const std::size_t n = points.size();
    std::vector<std::uint32_t> weld(n);
    if (n == 0) return weld;
    // The bounds of the finite points (one point not finite would make them NaN, and nothing weld).
    auto finite = [](const Vec3f& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    Vec3f lo, hi;
    bool any = false;
    for (const Vec3f& p : points) {
        if (!finite(p)) continue;
        lo = any ? Vec3f{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)} : p;
        hi = any ? Vec3f{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)} : p;
        any = true;
    }
    const float extent = any ? (hi - lo).length() * 1e-6f : 0.0f;
    const float eps = std::isfinite(extent) && extent > 1e-20f ? extent : 1e-20f;
    constexpr std::uint64_t kAlong = 1u << 21;  // cells along an axis: (extent / eps) + 3 < 2^21
    auto cellOf = [&](const Vec3f& p, int axis) {
        const float v = axis == 0 ? p.x - lo.x : axis == 1 ? p.y - lo.y : p.z - lo.z;
        return static_cast<std::uint64_t>(std::max(0.0f, std::floor(v / eps))) + 1;
    };
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells;
    cells.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f& p = points[i];
        if (!finite(p)) {  // its own (an infinite coordinate's cell would be a cast of infinity)
            weld[i] = static_cast<std::uint32_t>(i);
            continue;
        }
        const std::uint64_t cx = cellOf(p, 0), cy = cellOf(p, 1), cz = cellOf(p, 2);
        std::uint32_t found = static_cast<std::uint32_t>(i);
        for (std::uint64_t z = cz - 1; z <= cz + 1 && found == i; ++z) {
            for (std::uint64_t y = cy - 1; y <= cy + 1 && found == i; ++y) {
                for (std::uint64_t x = cx - 1; x <= cx + 1 && found == i; ++x) {
                    const auto it = cells.find((z * kAlong + y) * kAlong + x);
                    if (it == cells.end()) continue;
                    for (const std::uint32_t r : it->second) {
                        const Vec3f d = points[r] - p;
                        if (d.dot(d) <= eps * eps) {
                            found = r;
                            break;
                        }
                    }
                }
            }
        }
        weld[i] = found;
        if (found == i) cells[(cz * kAlong + cy) * kAlong + cx].push_back(static_cast<std::uint32_t>(i));
    }
    return weld;
}

std::shared_ptr<SurfaceTopology> buildTopology(const ScalpMesh& scalp) {
    auto out = std::make_shared<SurfaceTopology>();
    SurfaceTopology& s = *out;
    s.points = scalp.points();
    s.triangles = scalp.triangles();
    s.weld = weldPoints(s.points);
    const std::size_t tris = s.triangles.size();
    s.across.assign(tris, {-1, -1, -1});
    // The edges by their welded ends: two triangles on one edge are neighbors (the first two of
    // more, at a non-manifold edge).
    std::vector<std::pair<std::uint64_t, std::uint32_t>> edges;  // (key, triangle * 3 + edge)
    edges.reserve(tris * 3);
    for (std::size_t t = 0; t < tris; ++t) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t a = s.weld[static_cast<std::size_t>(s.triangles[t][k])];
            const std::uint32_t b = s.weld[static_cast<std::size_t>(s.triangles[t][(k + 1) % 3])];
            if (a == b) continue;  // a collapsed edge
            edges.emplace_back(static_cast<std::uint64_t>(std::min(a, b)) << 32 | std::max(a, b), static_cast<std::uint32_t>(t * 3 + k));
        }
    }
    std::sort(edges.begin(), edges.end());
    std::vector<std::uint32_t> parent(tris);
    for (std::size_t t = 0; t < tris; ++t) parent[t] = static_cast<std::uint32_t>(t);
    auto find = [&](std::uint32_t t) {
        while (parent[t] != t) t = parent[t] = parent[parent[t]];
        return t;
    };
    const bool uvs = scalp.hasUVs();
    const auto& cornerUVs = scalp.cornerUVs();
    auto sameUV = [&](std::size_t c0, std::size_t c1) {
        return std::abs(cornerUVs[c0][0] - cornerUVs[c1][0]) <= 1e-6f && std::abs(cornerUVs[c0][1] - cornerUVs[c1][1]) <= 1e-6f;
    };
    for (std::size_t e = 0; e < edges.size();) {
        std::size_t end = e + 1;
        while (end < edges.size() && edges[end].first == edges[e].first) ++end;
        if (end - e >= 2) {
            const std::uint32_t t0 = edges[e].second / 3, k0 = edges[e].second % 3;
            const std::uint32_t t1 = edges[e + 1].second / 3, k1 = edges[e + 1].second % 3;
            if (t0 != t1) {
                s.across[t0][k0] = static_cast<std::int32_t>(t1);
                s.across[t1][k1] = static_cast<std::int32_t>(t0);
                if (uvs) {
                    // One island when both ends have the same UVs on both sides.
                    const std::uint32_t a = s.weld[static_cast<std::size_t>(s.triangles[t0][k0])];
                    const std::size_t a1 = s.weld[static_cast<std::size_t>(s.triangles[t1][k1])] == a ? k1 : (k1 + 1) % 3;
                    const std::size_t b1 = a1 == k1 ? (k1 + 1) % 3 : k1;
                    if (sameUV(t0 * 3 + k0, t1 * 3 + a1) && sameUV(t0 * 3 + (k0 + 1) % 3, t1 * 3 + b1)) parent[find(t0)] = find(t1);
                }
            }
        }
        e = end;
    }
    s.island.assign(tris, 0);
    if (uvs) {
        for (std::size_t t = 0; t < tris; ++t) s.island[t] = find(static_cast<std::uint32_t>(t));
    }
    return out;
}

// The squared distance from p to the triangle abc (its closest point: Ericson's regions).
float distanceSquaredToTriangle(const Vec3f& p, const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    auto toward = [&](const Vec3f& q) { return (p - q).dot(p - q); };
    const Vec3f ab = b - a, ac = c - a, ap = p - a;
    const float d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return ap.dot(ap);
    const Vec3f bp = p - b;
    const float d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0f && d4 <= d3) return bp.dot(bp);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return toward(a + ab * (d1 - d3 != 0.0f ? d1 / (d1 - d3) : 0.0f));
    const Vec3f cp = p - c;
    const float d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0f && d5 <= d6) return cp.dot(cp);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return toward(a + ac * (d2 - d6 != 0.0f ? d2 / (d2 - d6) : 0.0f));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
        const float den = (d4 - d3) + (d5 - d6);
        return toward(b + (c - b) * (den != 0.0f ? (d4 - d3) / den : 0.0f));
    }
    const float sum = va + vb + vc;
    if (!(std::abs(sum) > 0.0f)) return std::min({ap.dot(ap), bp.dot(bp), cp.dot(cp)});
    return toward(a + ab * (vb / sum) + ac * (vc / sum));
}

// The topologies asked for lately, by surface: shared while held, the last one built kept.
struct TopologyCache {
    std::mutex mutex;
    std::map<std::uint64_t, std::weak_ptr<const SurfaceTopology>> topologies;
    std::shared_ptr<const SurfaceTopology> last;
};

TopologyCache& topologies() {
    static TopologyCache cache;
    return cache;
}

}  // namespace

std::size_t SurfaceTopology::bytes() const {
    return points.size() * sizeof(Vec3f) + triangles.size() * sizeof(ScalpMesh::Triangle) + weld.size() * sizeof(std::uint32_t) +
           across.size() * sizeof(across[0]) + island.size() * sizeof(std::uint32_t);
}

std::shared_ptr<const SurfaceTopology> surfaceTopology(const ScalpMesh& scalp) {
    TopologyCache& cache = topologies();
    const std::uint64_t key = scalp.surfaceHash();
    {
        const std::lock_guard<std::mutex> lock(cache.mutex);
        const auto it = cache.topologies.find(key);
        if (it != cache.topologies.end()) {
            if (std::shared_ptr<const SurfaceTopology> t = it->second.lock()) return t;
        }
    }
    std::shared_ptr<const SurfaceTopology> built = buildTopology(scalp);
    const std::lock_guard<std::mutex> lock(cache.mutex);
    for (auto it = cache.topologies.begin(); it != cache.topologies.end();) {
        it = it->second.expired() ? cache.topologies.erase(it) : std::next(it);
    }
    cache.topologies[key] = built;
    cache.last = built;
    return built;
}

std::vector<std::int32_t> reachedFaces(const SurfaceTopology& surface, std::int32_t seed, const Vec3f& center, float radius,
                                       std::vector<std::uint8_t>& reached) {
    const std::size_t tris = surface.triangles.size();
    reached.assign(tris, 0);
    std::vector<std::int32_t> out;
    if (seed < 0 || static_cast<std::size_t>(seed) >= tris) return out;
    const float r2 = radius * radius;
    reached[static_cast<std::size_t>(seed)] = 1;
    out.push_back(seed);
    for (std::size_t k = 0; k < out.size(); ++k) {
        for (const std::int32_t next : surface.across[static_cast<std::size_t>(out[k])]) {
            if (next < 0 || reached[static_cast<std::size_t>(next)]) continue;
            const auto& tri = surface.triangles[static_cast<std::size_t>(next)];
            const float d2 = distanceSquaredToTriangle(center, surface.points[static_cast<std::size_t>(tri[0])],
                                                       surface.points[static_cast<std::size_t>(tri[1])],
                                                       surface.points[static_cast<std::size_t>(tri[2])]);
            if (d2 > r2) continue;
            reached[static_cast<std::size_t>(next)] = 1;
            out.push_back(next);
        }
    }
    return out;
}

// ---- Vertex maps ------------------------------------------------------------

std::size_t stampVertexMap(std::vector<float>& values, const ScalpMesh& scalp, const Vec3f& center, float radius,
                           float strength, float value, const BrushWeightFn& weight) {
    return stampVertexMap(values, kGrayMap, 1u, scalp, center, radius, strength, {value, value, value, value}, weight);
}

std::size_t stampVertexMap(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                           const Vec3f& center, float radius, float strength, const PaintColor& target,
                           const BrushWeightFn& weight) {
    requireChannels(channels, "stampVertexMap");
    if (values.size() != scalp.pointCount() * static_cast<std::size_t>(channels)) {
        throw std::invalid_argument("stampVertexMap: one value per scalp vertex and channel");
    }
    return stampVertexReach(values, channels, mask, scalp, vertexReach(scalp, center, radius, weight), strength, target);
}

VertexReach vertexReach(const ScalpMesh& scalp, const Vec3f& center, float radius, const BrushWeightFn& weight) {
    const std::vector<Vec3f>& points = scalp.points();
    const float r2 = radius * radius;
    VertexReach out;
    if (!(radius > 0.0f) || scalp.triangleCount() == 0) return out;
    // The corners of the triangles reached from the one closest to the center.
    const SurfacePoint hit = scalp.closestPoint(center, radius);
    if (hit.root.face < 0) return out;
    const std::shared_ptr<const SurfaceTopology> surface = surfaceTopology(scalp);
    std::vector<std::uint8_t> reached;
    std::vector<std::uint32_t> corners;
    for (const std::int32_t face : reachedFaces(*surface, hit.root.face, center, radius, reached)) {
        for (const std::int32_t v : scalp.triangles()[static_cast<std::size_t>(face)]) corners.push_back(static_cast<std::uint32_t>(v));
    }
    std::sort(corners.begin(), corners.end());
    corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
    for (const std::uint32_t i : corners) {
        const Vec3f d = points[i] - center;
        if (d.dot(d) > r2) continue;
        // A weight of 0 does nothing whatever the strength: left out.
        const float w = brushWeight(points[i], center, radius, 0.0f, weight);
        if (w != 0.0f) out.emplace_back(i, w);
    }
    return out;
}

ScalpPolygons ScalpPolygons::of(const ScalpMesh& scalp) {
    ScalpPolygons out;
    const std::size_t tris = scalp.triangleCount();
    out.polygonOf.resize(tris);
    out.firstTriangle.push_back(0);
    const std::vector<std::int32_t>& counts = scalp.polygons();
    if (counts.empty()) {  // every triangle a face of its own
        for (std::size_t t = 0; t < tris; ++t) {
            out.polygonOf[t] = static_cast<std::uint32_t>(t);
            out.firstTriangle.push_back(static_cast<std::uint32_t>(t + 1));
        }
        return out;
    }
    std::size_t t = 0;
    for (std::size_t p = 0; p < counts.size(); ++p) {
        for (std::int32_t k = 0; k < counts[p] && t < tris; ++k, ++t) out.polygonOf[t] = static_cast<std::uint32_t>(p);
        out.firstTriangle.push_back(static_cast<std::uint32_t>(t));
    }
    return out;
}

std::vector<std::uint8_t> polygonTriangles(const ScalpMesh& scalp, const std::vector<std::int32_t>& faces) {
    const ScalpPolygons polygons = ScalpPolygons::of(scalp);
    std::vector<std::uint8_t> out(scalp.triangleCount(), 0);
    for (const std::int32_t face : faces) {
        if (face < 0 || static_cast<std::size_t>(face) >= polygons.count()) continue;
        for (std::uint32_t t = polygons.firstTriangle[static_cast<std::size_t>(face)];
             t < polygons.firstTriangle[static_cast<std::size_t>(face) + 1]; ++t) {
            out[t] = 1;
        }
    }
    return out;
}

std::vector<std::uint32_t> trianglesVertices(const ScalpMesh& scalp, const std::vector<std::uint8_t>& triangles) {
    std::vector<std::uint8_t> used(scalp.pointCount(), 0);
    const std::size_t n = std::min(triangles.size(), scalp.triangleCount());
    for (std::size_t t = 0; t < n; ++t) {
        if (!triangles[t]) continue;
        for (const std::int32_t v : scalp.triangles()[t]) used[static_cast<std::size_t>(v)] = 1;
    }
    std::vector<std::uint32_t> out;
    for (std::size_t v = 0; v < used.size(); ++v) {
        if (used[v]) out.push_back(static_cast<std::uint32_t>(v));
    }
    return out;
}

std::vector<std::uint8_t> polygonsUnderDab(const ScalpMesh& scalp, const Vec3f& center, float radius) {
    std::vector<std::uint8_t> out(scalp.triangleCount(), 0);
    if (!(radius > 0.0f) || scalp.triangleCount() == 0) return out;
    const SurfacePoint hit = scalp.closestPoint(center, radius);
    if (hit.root.face < 0) return out;
    const ScalpPolygons polygons = ScalpPolygons::of(scalp);
    const std::shared_ptr<const SurfaceTopology> surface = surfaceTopology(scalp);
    std::vector<std::uint8_t> reached;
    std::vector<std::uint32_t> candidates;
    for (const std::int32_t t : reachedFaces(*surface, hit.root.face, center, radius, reached)) {
        candidates.push_back(polygons.polygonOf[static_cast<std::size_t>(t)]);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    const std::uint32_t under = polygons.polygonOf[static_cast<std::size_t>(hit.root.face)];
    const std::vector<Vec3f>& points = scalp.points();
    const float r2 = radius * radius;
    for (const std::uint32_t p : candidates) {
        const std::uint32_t first = polygons.firstTriangle[p], last = polygons.firstTriangle[p + 1];
        bool take = p == under;
        if (!take) {
            // The polygon's center: the mean of its triangles' corners (each corner once).
            std::vector<std::int32_t> corners;
            for (std::uint32_t t = first; t < last; ++t) {
                for (const std::int32_t v : scalp.triangles()[t]) corners.push_back(v);
            }
            std::sort(corners.begin(), corners.end());
            corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
            Vec3f sum{0.0f, 0.0f, 0.0f};
            for (const std::int32_t v : corners) sum = sum + points[static_cast<std::size_t>(v)];
            const Vec3f d = sum * (1.0f / static_cast<float>(std::max<std::size_t>(corners.size(), 1))) - center;
            take = d.dot(d) <= r2;
        }
        if (!take) continue;
        for (std::uint32_t t = first; t < last; ++t) out[t] = 1;
    }
    return out;
}

std::size_t fillVertexMapFaces(std::vector<float>& values, int channels, unsigned mask, const PaintColor& color,
                               PaintMode mode, const ScalpMesh& scalp, const std::vector<std::int32_t>& faces) {
    requireChannels(channels, "fillVertexMapFaces");
    const auto n = static_cast<std::size_t>(channels);
    if (values.size() != scalp.pointCount() * n) {
        throw std::invalid_argument("fillVertexMapFaces: one value per scalp vertex and channel");
    }
    const unsigned m = channels == kGrayMap ? 1u : (mask & kAllChannels);
    std::size_t changed = 0;
    for (const std::uint32_t v : trianglesVertices(scalp, polygonTriangles(scalp, faces))) {
        bool any = false;
        for (std::size_t c = 0; c < n; ++c) {
            if (!(m & (1u << c))) continue;
            float& x = values[v * n + c];
            const float next = compositePaintValue(x, color[c], mode);
            if (next != x) {
                x = next;
                any = true;
            }
        }
        if (any) ++changed;
    }
    return changed;
}

std::size_t stampVertexReach(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                             const VertexReach& reach, float strength, const PaintColor& target, PaintMode mode,
                             const std::vector<float>* strokeStart) {
    requireChannels(channels, "stampVertexMap");
    const auto n = static_cast<std::size_t>(channels);
    if (values.size() != scalp.pointCount() * n) throw std::invalid_argument("stampVertexMap: one value per scalp vertex and channel");
    if (strokeStart && strokeStart->size() != values.size()) {
        throw std::invalid_argument("stampVertexMap: the stroke's starting values have another size");
    }
    const unsigned m = effectiveMask(channels, mask);
    std::size_t changed = 0;
    for (const auto& [i, weightThere] : reach) {
        const float w = weightThere * strength;
        if (w <= 0.0f) continue;
        bool any = false;
        for (std::size_t c = 0; c < n; ++c) {
            if (!(m & (1u << c))) continue;
            float& x = values[i * n + c];
            const float start = strokeStart ? (*strokeStart)[i * n + c] : x;
            const float composed = compositePaintValue(start, target[c], mode);
            const float next = std::clamp(x + (composed - x) * std::min(w, 1.0f), 0.0f, 1.0f);
            if (next != x) {
                x = next;
                any = true;
            }
        }
        if (any) ++changed;
    }
    return changed;
}

VertexNeighbors VertexNeighbors::of(const ScalpMesh& scalp) {
    const std::size_t n = scalp.pointCount();
    const std::vector<std::uint32_t> weld = weldPoints(scalp.points());
    // Per welded point: the welded points beside any of its copies (each copy reads its stand-in,
    // so a position counts once; the copies, painted alike, smooth alike).
    std::vector<std::vector<std::uint32_t>> around(n);
    for (const auto& tri : scalp.triangles()) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = weld[static_cast<std::size_t>(tri[static_cast<std::size_t>(k)])];
            const std::uint32_t b = weld[static_cast<std::size_t>(tri[static_cast<std::size_t>((k + 1) % 3)])];
            if (a == b) continue;
            around[a].push_back(b);
            around[b].push_back(a);
        }
    }
    for (auto& list : around) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    VertexNeighbors out;
    out.start.assign(n + 1, 0);
    for (std::size_t v = 0; v < n; ++v) out.start[v + 1] = out.start[v] + static_cast<std::uint32_t>(around[weld[v]].size());
    out.list.reserve(out.start[n]);
    for (std::size_t v = 0; v < n; ++v) out.list.insert(out.list.end(), around[weld[v]].begin(), around[weld[v]].end());
    return out;
}

std::size_t smoothVertexMap(std::vector<float>& values, const ScalpMesh& scalp, const VertexNeighbors& neighbors,
                            const Vec3f& center, float radius, float strength, const BrushWeightFn& weight) {
    return smoothVertexMap(values, kGrayMap, 1u, scalp, neighbors, center, radius, strength, weight);
}

std::size_t smoothVertexMap(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                            const VertexNeighbors& neighbors, const Vec3f& center, float radius, float strength,
                            const BrushWeightFn& weight) {
    requireChannels(channels, "smoothVertexMap");
    if (values.size() != scalp.pointCount() * static_cast<std::size_t>(channels) ||
        neighbors.start.size() != scalp.pointCount() + 1) {
        throw std::invalid_argument("smoothVertexMap: one value per scalp vertex and channel, with the scalp's neighbors");
    }
    return smoothVertexReach(values, channels, mask, scalp, neighbors, vertexReach(scalp, center, radius, weight), strength);
}

std::size_t smoothVertexReach(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                              const VertexNeighbors& neighbors, const VertexReach& reach, float strength) {
    requireChannels(channels, "smoothVertexMap");
    const auto n = static_cast<std::size_t>(channels);
    if (values.size() != scalp.pointCount() * n || neighbors.start.size() != scalp.pointCount() + 1) {
        throw std::invalid_argument("smoothVertexMap: one value per scalp vertex and channel, with the scalp's neighbors");
    }
    const unsigned m = effectiveMask(channels, mask);
    std::vector<std::pair<std::size_t, float>> next;  // (value index, value) from the values before the stamp
    for (const auto& [i, weightThere] : reach) {
        const float w = std::min(weightThere * std::abs(strength), 1.0f);
        if (w <= 0.0f || neighbors.start[i + 1] == neighbors.start[i]) continue;
        for (std::size_t c = 0; c < n; ++c) {
            if (!(m & (1u << c))) continue;
            float sum = values[i * n + c];  // the vertex counts with its neighbors: a gentler blur
            for (std::uint32_t k = neighbors.start[i]; k < neighbors.start[i + 1]; ++k) sum += values[neighbors.list[k] * n + c];
            const float mean = sum / static_cast<float>(neighbors.start[i + 1] - neighbors.start[i] + 1);
            next.emplace_back(i * n + c, std::clamp(values[i * n + c] + (mean - values[i * n + c]) * w, 0.0f, 1.0f));
        }
    }
    std::size_t changed = 0, lastVertex = static_cast<std::size_t>(-1);
    for (const auto& [k, v] : next) {
        if (v != values[k]) {
            values[k] = v;
            if (k / n != lastVertex) ++changed;  // a vertex counts once, whatever channels changed
            lastVertex = k / n;
        }
    }
    return changed;
}

std::vector<ScalpMesh::Color> cornerColorsFromVertexMap(const std::vector<float>& values, const ScalpMesh& scalp, int channels) {
    requireChannels(channels, "cornerColorsFromVertexMap");
    const auto n = static_cast<std::size_t>(channels);
    if (values.size() != scalp.pointCount() * n) throw std::invalid_argument("cornerColorsFromVertexMap: one value per scalp vertex and channel");
    std::vector<ScalpMesh::Color> colors;
    colors.reserve(scalp.triangleCount() * 3);
    for (const auto& tri : scalp.triangles()) {
        for (const std::int32_t v : tri) {
            const float* x = &values[static_cast<std::size_t>(v) * n];
            if (n == 1) {
                colors.push_back({x[0], x[0], x[0], 1.0f});
            } else {
                colors.push_back({x[0], x[1], x[2], x[3]});
            }
        }
    }
    return colors;
}

PaintColor vertexColorAt(const std::vector<float>& values, int channels, const ScalpMesh& scalp, const StrandRoot& root) {
    const auto n = static_cast<std::size_t>(channels);
    if (channels != kGrayMap && channels != kColorMap) return {0.0f, 0.0f, 0.0f, 1.0f};
    if (!scalp.contains(root) || values.size() != scalp.pointCount() * n) return {0.0f, 0.0f, 0.0f, 1.0f};
    const auto& t = scalp.triangles()[static_cast<std::size_t>(root.face)];
    const float w[3] = {1.0f - root.u - root.v, root.u, root.v};
    PaintColor out{0.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 3; ++k) {
        const float* x = &values[static_cast<std::size_t>(t[static_cast<std::size_t>(k)]) * n];
        for (std::size_t c = 0; c < 4; ++c) out[c] += (n == 1 ? (c < 3 ? x[0] : 1.0f) : x[c]) * w[k];
    }
    return out;
}

float vertexMapAt(const std::vector<float>& values, const ScalpMesh& scalp, const StrandRoot& root) {
    if (!scalp.contains(root) || values.size() != scalp.pointCount()) return 0.0f;
    const auto& t = scalp.triangles()[static_cast<std::size_t>(root.face)];
    return values[static_cast<std::size_t>(t[0])] * (1.0f - root.u - root.v) + values[static_cast<std::size_t>(t[1])] * root.u +
           values[static_cast<std::size_t>(t[2])] * root.v;
}

// ---- Texture maps -----------------------------------------------------------

void TexelRect::unite(const TexelRect& other) {
    if (other.empty()) return;
    if (empty()) {
        *this = other;
        return;
    }
    x0 = std::min(x0, other.x0);
    y0 = std::min(y0, other.y0);
    x1 = std::max(x1, other.x1);
    y1 = std::max(y1, other.y1);
}

std::size_t TexelTable::bytes() const {
    return roots.size() * sizeof(StrandRoot) + covered.size() + positions.size() * sizeof(Vec3f) +
           (cellStart.size() + cellTexels.size() + seamTexels.size() + seamNeighbors.size() + padTexels.size() +
            padSources.size()) * sizeof(std::uint32_t);
}

std::array<std::uint32_t, 8> TexelTable::neighbors(std::size_t texel) const {
    std::array<std::uint32_t, 8> out;
    out.fill(kNoTexel);
    if (texel >= covered.size() || !covered[texel]) return out;
    if (covered[texel] == 2) {
        const auto at = std::lower_bound(seamTexels.begin(), seamTexels.end(), static_cast<std::uint32_t>(texel));
        std::copy_n(seamNeighbors.begin() + (at - seamTexels.begin()) * 8, 8, out.begin());
        return out;
    }
    const auto w = static_cast<std::ptrdiff_t>(width);
    const std::ptrdiff_t steps[8] = {-w - 1, -w, -w + 1, -1, 1, w - 1, w, w + 1};
    for (int k = 0; k < 8; ++k) out[static_cast<std::size_t>(k)] = static_cast<std::uint32_t>(static_cast<std::ptrdiff_t>(texel) + steps[k]);
    return out;
}

namespace {

// Every texel whose center lies in a UV triangle: its root and position on the scalp. Rows are
// built in bands, in parallel; each band takes the triangles reaching it in their order, so a
// texel two triangles cover (an edge) takes the later one's, as one pass over the triangles does.
void buildTexelRoots(TexelTable& t, const ScalpMesh& scalp) {
    if (!scalp.hasUVs()) throw std::invalid_argument("the scalp has no UVs to paint a texture map on");
    const int width = t.width, height = t.height;
    const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    t.roots.assign(texels, StrandRoot{});
    t.covered.assign(texels, 0);
    t.positions.assign(texels, Vec3f{});
    const auto& uvs = scalp.cornerUVs();
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    struct Span {
        float ax, ay, bx, by, cx, cy, inv;
        int x0, x1, y0, y1;
    };
    const std::size_t triangles = scalp.triangleCount();
    std::vector<Span> spans(triangles);
    std::vector<std::uint8_t> used(triangles, 0);
    parallelFor(0, triangles, [&](std::size_t tri) {
        // Pixel coordinates of the corners: x = u * width, y = (1 - v) * height (rows top-down).
        Span& s = spans[tri];
        s.ax = uvs[tri * 3][0] * w, s.ay = (1.0f - uvs[tri * 3][1]) * h;
        s.bx = uvs[tri * 3 + 1][0] * w, s.by = (1.0f - uvs[tri * 3 + 1][1]) * h;
        s.cx = uvs[tri * 3 + 2][0] * w, s.cy = (1.0f - uvs[tri * 3 + 2][1]) * h;
        const float det = (s.bx - s.ax) * (s.cy - s.ay) - (s.cx - s.ax) * (s.by - s.ay);
        if (std::abs(det) < 1e-12f) return;  // no UV area
        s.x0 = std::max(0, static_cast<int>(std::floor(std::min({s.ax, s.bx, s.cx}))));
        s.x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({s.ax, s.bx, s.cx}))));
        s.y0 = std::max(0, static_cast<int>(std::floor(std::min({s.ay, s.by, s.cy}))));
        s.y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({s.ay, s.by, s.cy}))));
        s.inv = 1.0f / det;
        used[tri] = s.x0 <= s.x1 && s.y0 <= s.y1 ? 1 : 0;
    });
    // The triangles reaching each band of rows, in triangle order.
    constexpr int kBand = 8;
    const std::size_t bands = static_cast<std::size_t>((height + kBand - 1) / kBand);
    std::vector<std::uint32_t> bandStart(bands + 1, 0);
    for (std::size_t tri = 0; tri < triangles; ++tri) {
        if (!used[tri]) continue;
        for (int b = spans[tri].y0 / kBand; b <= spans[tri].y1 / kBand; ++b) ++bandStart[static_cast<std::size_t>(b) + 1];
    }
    for (std::size_t b = 0; b < bands; ++b) bandStart[b + 1] += bandStart[b];
    std::vector<std::uint32_t> bandTriangles(bandStart[bands]);
    {
        std::vector<std::uint32_t> fill(bandStart.begin(), bandStart.end() - 1);
        for (std::size_t tri = 0; tri < triangles; ++tri) {
            if (!used[tri]) continue;
            for (int b = spans[tri].y0 / kBand; b <= spans[tri].y1 / kBand; ++b) {
                bandTriangles[fill[static_cast<std::size_t>(b)]++] = static_cast<std::uint32_t>(tri);
            }
        }
    }
    std::vector<std::size_t> bandCovered(bands, 0);
    parallelFor(0, bands, [&](std::size_t band) {
        const int rowStart = static_cast<int>(band) * kBand, rowEnd = std::min(height, rowStart + kBand);
        for (std::uint32_t k = bandStart[band]; k < bandStart[band + 1]; ++k) {
            const std::size_t tri = bandTriangles[k];
            const Span& s = spans[tri];
            // A texel on an edge belongs to both triangles: a small tolerance leaves no cracks.
            const float eps = -1e-3f;
            for (int y = std::max(s.y0, rowStart); y <= std::min(s.y1, rowEnd - 1); ++y) {
                const float py = static_cast<float>(y) + 0.5f;
                for (int x = s.x0; x <= s.x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float u = ((px - s.ax) * (s.cy - s.ay) - (s.cx - s.ax) * (py - s.ay)) * s.inv;
                    const float v = ((s.bx - s.ax) * (py - s.ay) - (px - s.ax) * (s.by - s.ay)) * s.inv;
                    if (u < eps || v < eps || u + v > 1.0f - eps) continue;
                    StrandRoot root;
                    root.face = static_cast<std::int32_t>(tri);
                    root.u = std::clamp(u, 0.0f, 1.0f);
                    root.v = std::clamp(v, 0.0f, 1.0f - root.u);
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                    t.roots[i] = root;
                    t.covered[i] = 1;
                    t.positions[i] = scalp.position(root);
                }
            }
        }
        std::size_t count = 0;
        for (std::size_t i = static_cast<std::size_t>(rowStart) * static_cast<std::size_t>(width);
             i < static_cast<std::size_t>(rowEnd) * static_cast<std::size_t>(width); ++i) {
            count += t.covered[i];
        }
        bandCovered[band] = count;
    }, 1);
    t.coveredCount = 0;
    for (const std::size_t c : bandCovered) t.coveredCount += c;
}

// A uniform grid over the covered texels' positions, for the stamps' sphere queries.
void buildTexelGrid(TexelTable& t) {
    t.cellStart.clear();
    t.cellTexels.clear();
    t.nx = t.ny = t.nz = 0;
    if (t.coveredCount == 0) return;
    Vec3f lo, hi;
    bool first = true;
    for (std::size_t i = 0; i < t.roots.size(); ++i) {
        if (t.roots[i].face < 0) continue;
        const Vec3f& p = t.positions[i];
        if (first) {
            lo = hi = p;
            first = false;
        } else {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    // Cells about as many as the covered texels' cube root squared: a few dozen texels each.
    const Vec3f extent = hi - lo;
    const float longest = std::max({extent.x, extent.y, extent.z, 1e-6f});
    const float cellsAlong = std::clamp(pm::cbrt(static_cast<float>(t.coveredCount)) * 0.5f, 4.0f, 256.0f);
    t.cell = longest / cellsAlong;
    t.gridLo = lo - Vec3f{t.cell, t.cell, t.cell} * 0.5f;
    t.nx = static_cast<int>(extent.x / t.cell) + 2;
    t.ny = static_cast<int>(extent.y / t.cell) + 2;
    t.nz = static_cast<int>(extent.z / t.cell) + 2;
    const std::size_t cells = static_cast<std::size_t>(t.nx) * static_cast<std::size_t>(t.ny) * static_cast<std::size_t>(t.nz);
    auto cellOf = [&](const Vec3f& p) {
        const int ix = std::clamp(static_cast<int>((p.x - t.gridLo.x) / t.cell), 0, t.nx - 1);
        const int iy = std::clamp(static_cast<int>((p.y - t.gridLo.y) / t.cell), 0, t.ny - 1);
        const int iz = std::clamp(static_cast<int>((p.z - t.gridLo.z) / t.cell), 0, t.nz - 1);
        return (static_cast<std::size_t>(iz) * static_cast<std::size_t>(t.ny) + static_cast<std::size_t>(iy)) * static_cast<std::size_t>(t.nx) + static_cast<std::size_t>(ix);
    };
    // Each covered texel's cell, found once (in parallel) for the count and the fill.
    std::vector<std::uint32_t> cellOfTexel(t.roots.size(), 0);
    parallelFor(0, t.roots.size(), [&](std::size_t i) {
        if (t.roots[i].face >= 0) cellOfTexel[i] = static_cast<std::uint32_t>(cellOf(t.positions[i]));
    }, 4096);
    t.cellStart.assign(cells + 1, 0);
    for (std::size_t i = 0; i < t.roots.size(); ++i) {
        if (t.roots[i].face >= 0) ++t.cellStart[cellOfTexel[i] + 1];
    }
    for (std::size_t c = 0; c < cells; ++c) t.cellStart[c + 1] += t.cellStart[c];
    t.cellTexels.resize(t.coveredCount);
    std::vector<std::uint32_t> fill(t.cellStart.begin(), t.cellStart.end() - 1);
    for (std::size_t i = 0; i < t.roots.size(); ++i) {
        if (t.roots[i].face >= 0) t.cellTexels[fill[cellOfTexel[i]]++] = static_cast<std::uint32_t>(i);
    }
}

// The 8 steps to a texel's neighbors, in TexelTable::seamNeighbors' order.
constexpr int kStepX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kStepY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Barycentrics of p in the triangle abc's plane (p projected onto it); false when it has no area.
bool planeBarycentrics(const Vec3f& p, const Vec3f& a, const Vec3f& b, const Vec3f& c, std::array<float, 3>& w) {
    const Vec3f v0 = b - a, v1 = c - a, v2 = p - a;
    const float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
    const float den = d00 * d11 - d01 * d01;
    if (!(std::abs(den) > 1e-30f)) return false;
    w[1] = (d11 * d20 - d01 * d21) / den;
    w[2] = (d00 * d21 - d01 * d20) / den;
    w[0] = 1.0f - w[1] - w[2];
    return true;
}

// The texel showing the surface one texel step (dx, dy) from covered texel i, walked on the
// surface: the step's end in i's triangle (its UVs' barycentrics, maybe outside it), carried over
// each edge it lies past into the triangle across (unfolded about the edge in 3D: the same
// distance along and away from it), until it lies in a triangle; then the texel at its UV there
// (one of that island's texels around it when that one is not covered by it). kNoTexel past the
// mesh's border.
std::uint32_t texelAcross(const TexelTable& t, const SurfaceTopology& s, const std::vector<ScalpMesh::UV>& uvs, std::size_t i,
                          int dx, int dy) {
    const int width = t.width, height = t.height;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    auto pixel = [&](std::int32_t tri, int k) {
        const auto& uv = uvs[static_cast<std::size_t>(tri) * 3 + static_cast<std::size_t>(k)];
        return std::array<float, 2>{uv[0] * w, (1.0f - uv[1]) * h};
    };
    const int x = static_cast<int>(i % static_cast<std::size_t>(width)), y = static_cast<int>(i / static_cast<std::size_t>(width));
    const float qx = static_cast<float>(x) + 0.5f + static_cast<float>(dx), qy = static_cast<float>(y) + 0.5f + static_cast<float>(dy);
    std::int32_t tri = t.roots[i].face;
    std::array<float, 3> bary;
    {
        const auto a = pixel(tri, 0), b = pixel(tri, 1), c = pixel(tri, 2);
        const float det = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
        if (!(std::abs(det) > 1e-12f)) return TexelTable::kNoTexel;
        bary[1] = ((qx - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (qy - a[1])) / det;
        bary[2] = ((b[0] - a[0]) * (qy - a[1]) - (qx - a[0]) * (b[1] - a[1])) / det;
        bary[0] = 1.0f - bary[1] - bary[2];
    }
    bool inside = false;
    for (int step = 0; step < 64; ++step) {
        const int k = bary[0] <= bary[1] ? (bary[0] <= bary[2] ? 0 : 2) : (bary[1] <= bary[2] ? 1 : 2);
        if (bary[static_cast<std::size_t>(k)] >= -1e-5f) {
            inside = true;
            break;
        }
        const int e = (k + 1) % 3;  // the edge opposite corner k: corners e, e + 1
        const std::int32_t next = s.across[static_cast<std::size_t>(tri)][static_cast<std::size_t>(e)];
        if (next < 0) return TexelTable::kNoTexel;
        const auto& T = s.triangles[static_cast<std::size_t>(tri)];
        const Vec3f X[3] = {s.points[static_cast<std::size_t>(T[0])], s.points[static_cast<std::size_t>(T[1])],
                            s.points[static_cast<std::size_t>(T[2])]};
        const Vec3f p = X[0] * bary[0] + X[1] * bary[1] + X[2] * bary[2];
        const Vec3f A = X[e], B = X[(e + 1) % 3];
        const std::uint32_t wa = s.weld[static_cast<std::size_t>(T[static_cast<std::size_t>(e)])];
        const std::uint32_t wb = s.weld[static_cast<std::size_t>(T[static_cast<std::size_t>((e + 1) % 3)])];
        const auto& N = s.triangles[static_cast<std::size_t>(next)];
        int far = -1;
        for (int m = 0; m < 3; ++m) {
            const std::uint32_t wm = s.weld[static_cast<std::size_t>(N[static_cast<std::size_t>(m)])];
            if (wm != wa && wm != wb) far = m;
        }
        if (far < 0) return TexelTable::kNoTexel;
        const Vec3f edge = B - A;
        const float length = edge.length();
        if (!(length > 0.0f)) return TexelTable::kNoTexel;
        const Vec3f along = edge * (1.0f / length);
        const float s0 = (p - A).dot(along);
        const float away = (p - A - along * s0).length();
        const Vec3f D = s.points[static_cast<std::size_t>(N[static_cast<std::size_t>(far)])];
        Vec3f inward = D - A - along * (D - A).dot(along);
        const float inwardLength = inward.length();
        if (!(inwardLength > 0.0f)) return TexelTable::kNoTexel;
        inward = inward * (1.0f / inwardLength);
        const Vec3f unfolded = A + along * s0 + inward * away;
        if (!planeBarycentrics(unfolded, s.points[static_cast<std::size_t>(N[0])], s.points[static_cast<std::size_t>(N[1])],
                               s.points[static_cast<std::size_t>(N[2])], bary)) {
            return TexelTable::kNoTexel;
        }
        tri = next;
    }
    if (!inside) return TexelTable::kNoTexel;
    // The step's end at its triangle's UVs, in pixels.
    float px = 0.0f, py = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const auto c = pixel(tri, k);
        px += c[0] * bary[static_cast<std::size_t>(k)];
        py += c[1] * bary[static_cast<std::size_t>(k)];
    }
    const std::uint32_t island = s.island[static_cast<std::size_t>(tri)];
    const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
    auto texel = [&](int tx, int ty) { return static_cast<std::size_t>(ty) * static_cast<std::size_t>(width) + static_cast<std::size_t>(tx); };
    // The texel there when its island's; else the nearest of its island's around it; else (UVs
    // overlapping another island's) the texel there.
    std::uint32_t best = TexelTable::kNoTexel;
    float bestDistance = std::numeric_limits<float>::max();
    for (int ty = iy - 1; ty <= iy + 1; ++ty) {
        for (int tx = ix - 1; tx <= ix + 1; ++tx) {
            if (tx < 0 || ty < 0 || tx >= width || ty >= height) continue;
            const std::size_t j = texel(tx, ty);
            if (!t.covered[j] || s.island[static_cast<std::size_t>(t.roots[j].face)] != island) continue;
            const float ex = static_cast<float>(tx) + 0.5f - px, ey = static_cast<float>(ty) + 0.5f - py;
            const float d = (tx == ix && ty == iy) ? -1.0f : ex * ex + ey * ey;
            if (d < bestDistance) {
                bestDistance = d;
                best = static_cast<std::uint32_t>(j);
            }
        }
    }
    if (best == TexelTable::kNoTexel && ix >= 0 && iy >= 0 && ix < width && iy < height && t.covered[texel(ix, iy)]) {
        best = static_cast<std::uint32_t>(texel(ix, iy));
    }
    return best;
}

// The seam texels (covered texels with a neighbor off their island) and their neighbors across.
void buildSeams(TexelTable& t, const ScalpMesh& scalp) {
    const SurfaceTopology& s = *t.surface;
    const int width = t.width, height = t.height;
    auto islandOf = [&](std::size_t i) { return s.island[static_cast<std::size_t>(t.roots[i].face)]; };
    // Whether texel i's neighbor k is beside it in its island (the texel itself when it is).
    auto besideIn = [&](std::size_t i, int x, int y, int k) -> std::uint32_t {
        const int nx = x + kStepX[k], ny = y + kStepY[k];
        if (nx < 0 || ny < 0 || nx >= width || ny >= height) return TexelTable::kNoTexel;
        const std::size_t j = static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx);
        return t.covered[j] && islandOf(j) == islandOf(i) ? static_cast<std::uint32_t>(j) : TexelTable::kNoTexel;
    };
    std::vector<std::vector<std::uint32_t>> rows(static_cast<std::size_t>(height));
    parallelFor(0, static_cast<std::size_t>(height), [&](std::size_t row) {
        const int y = static_cast<int>(row);
        for (int x = 0; x < width; ++x) {
            const std::size_t i = row * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (!t.covered[i]) continue;
            for (int k = 0; k < 8; ++k) {
                if (besideIn(i, x, y, k) == TexelTable::kNoTexel) {
                    rows[row].push_back(static_cast<std::uint32_t>(i));
                    break;
                }
            }
        }
    }, 8);
    t.seamTexels.clear();
    for (const auto& r : rows) t.seamTexels.insert(t.seamTexels.end(), r.begin(), r.end());
    rows.clear();
    rows.shrink_to_fit();
    t.seamNeighbors.assign(t.seamTexels.size() * 8, TexelTable::kNoTexel);
    const auto& uvs = scalp.cornerUVs();
    parallelFor(0, t.seamTexels.size(), [&](std::size_t n) {
        const std::size_t i = t.seamTexels[n];
        const int x = static_cast<int>(i % static_cast<std::size_t>(width)), y = static_cast<int>(i / static_cast<std::size_t>(width));
        for (int k = 0; k < 8; ++k) {
            std::uint32_t j = besideIn(i, x, y, k);
            if (j == TexelTable::kNoTexel) j = texelAcross(t, s, uvs, i, kStepX[k], kStepY[k]);
            t.seamNeighbors[n * 8 + static_cast<std::size_t>(k)] = j;
        }
    }, 256);
    for (const std::uint32_t i : t.seamTexels) t.covered[i] = 2;
}

// The uncovered texels within kPadding steps (4-neighbor) of the covered ones, each with the
// nearest covered texel of those its neighbors copy (ring by ring, as toBytes grows its padding).
void buildPadding(TexelTable& t) {
    const int width = t.width, height = t.height;
    const std::size_t texels = t.covered.size();
    std::vector<std::uint32_t> source(texels, TexelTable::kNoTexel);
    std::vector<std::uint8_t> queued(texels, 0);
    parallelFor(0, texels, [&](std::size_t i) {
        if (t.covered[i]) source[i] = static_cast<std::uint32_t>(i);
    }, 65536);
    const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    std::vector<std::vector<std::uint32_t>> rows(static_cast<std::size_t>(height));
    parallelFor(0, static_cast<std::size_t>(height), [&](std::size_t row) {
        const int y = static_cast<int>(row);
        for (int x = 0; x < width; ++x) {
            const std::size_t i = row * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (t.covered[i]) continue;
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                if (!t.covered[static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx)]) continue;
                rows[row].push_back(static_cast<std::uint32_t>(i));
                queued[i] = 1;
                break;
            }
        }
    }, 8);
    std::vector<std::uint32_t> frontier, next, ring;
    for (const auto& r : rows) frontier.insert(frontier.end(), r.begin(), r.end());
    rows.clear();
    rows.shrink_to_fit();
    t.padTexels.clear();
    t.padSources.clear();
    for (int pass = 0; pass < TexelTable::kPadding && !frontier.empty(); ++pass) {
        ring.assign(frontier.size(), TexelTable::kNoTexel);
        parallelFor(0, frontier.size(), [&](std::size_t f) {
            const std::size_t i = frontier[f];
            const int x = static_cast<int>(i % static_cast<std::size_t>(width)), y = static_cast<int>(i / static_cast<std::size_t>(width));
            float best = std::numeric_limits<float>::max();
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const std::uint32_t from = source[static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx)];
                if (from == TexelTable::kNoTexel) continue;
                const float ex = static_cast<float>(static_cast<int>(from % static_cast<std::uint32_t>(width)) - x);
                const float ey = static_cast<float>(static_cast<int>(from / static_cast<std::uint32_t>(width)) - y);
                const float d = ex * ex + ey * ey;
                if (d < best || (d == best && from < ring[f])) {
                    best = d;
                    ring[f] = from;
                }
            }
        }, 1024);
        for (std::size_t f = 0; f < frontier.size(); ++f) {
            source[frontier[f]] = ring[f];
            t.padTexels.push_back(frontier[f]);
            t.padSources.push_back(ring[f]);
        }
        next.clear();
        for (const std::uint32_t i : frontier) {
            const int x = static_cast<int>(i % static_cast<std::uint32_t>(width)), y = static_cast<int>(i / static_cast<std::uint32_t>(width));
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const std::size_t j = static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx);
                if (source[j] != TexelTable::kNoTexel || queued[j]) continue;
                queued[j] = 1;
                next.push_back(static_cast<std::uint32_t>(j));
            }
        }
        frontier.swap(next);
    }
    // Ascending, for the preview's rows.
    std::vector<std::uint32_t> order(t.padTexels.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = static_cast<std::uint32_t>(k);
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return t.padTexels[a] < t.padTexels[b]; });
    std::vector<std::uint32_t> texelsSorted(order.size()), sourcesSorted(order.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        texelsSorted[k] = t.padTexels[order[k]];
        sourcesSorted[k] = t.padSources[order[k]];
    }
    t.padTexels.swap(texelsSorted);
    t.padSources.swap(sourcesSorted);
}

// The tables asked for lately, by surface and size: shared while a map holds one, and the last
// one built kept (a map opened again after an undo or a scene change finds it).
struct TexelTableCache {
    struct Key {
        std::uint64_t surface;
        int width, height;
        bool operator<(const Key& o) const {
            return std::tie(surface, width, height) < std::tie(o.surface, o.width, o.height);
        }
    };
    std::mutex mutex;
    std::map<Key, std::weak_ptr<const TexelTable>> tables;
    std::shared_ptr<const TexelTable> last;
};

TexelTableCache& texelTables() {
    static TexelTableCache cache;
    return cache;
}

}  // namespace

std::shared_ptr<const TexelTable> texelTable(const ScalpMesh& scalp, int width, int height) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("TexturePaint: the image needs a positive size");
    if (!scalp.hasUVs()) throw std::invalid_argument("the scalp has no UVs to paint a texture map on");
    TexelTableCache& cache = texelTables();
    const TexelTableCache::Key key{scalp.surfaceHash(), width, height};
    {
        const std::lock_guard<std::mutex> lock(cache.mutex);
        const auto it = cache.tables.find(key);
        if (it != cache.tables.end()) {
            if (std::shared_ptr<const TexelTable> table = it->second.lock()) return table;
        }
    }
    auto table = std::make_shared<TexelTable>();
    table->width = width;
    table->height = height;
    buildTexelRoots(*table, scalp);
    buildTexelGrid(*table);
    table->surface = surfaceTopology(scalp);
    buildSeams(*table, scalp);
    buildPadding(*table);
    std::shared_ptr<const TexelTable> shared = std::move(table);
    const std::lock_guard<std::mutex> lock(cache.mutex);
    for (auto it = cache.tables.begin(); it != cache.tables.end();) {
        it = it->second.expired() ? cache.tables.erase(it) : std::next(it);
    }
    cache.tables[key] = shared;
    cache.last = shared;
    return shared;
}

TexturePaint::TexturePaint(const ScalpMesh& scalp, int width, int height, float value) : width_(width), height_(height) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("TexturePaint: the image needs a positive size");
    values_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), std::clamp(value, 0.0f, 1.0f));
    table_ = texelTable(scalp, width_, height_);
    dirty_ = TexelRect::all(width_, height_);
}

TexturePaint::TexturePaint(const ScalpMesh& scalp, int width, int height, const PaintColor& color)
    : width_(width), height_(height), channels_(kColorMap) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("TexturePaint: the image needs a positive size");
    values_.resize(texelCount() * 4);
    for (std::size_t i = 0; i < texelCount(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) values_[i * 4 + c] = std::clamp(color[c], 0.0f, 1.0f);
    }
    table_ = texelTable(scalp, width_, height_);
    dirty_ = TexelRect::all(width_, height_);
}

TexturePaint::TexturePaint(const ScalpMesh& scalp, const Texture& image, int channels)
    : width_(image.width()), height_(image.height()), channels_(channels) {
    requireOneImage(image);
    requireChannels(channels, "TexturePaint");
    if (width_ <= 0 || height_ <= 0) throw std::invalid_argument("TexturePaint: the image has no size");
    if (!scalp.hasUVs()) throw std::invalid_argument("the scalp has no UVs to paint a texture map on");
    const auto n = static_cast<std::size_t>(channels_);
    values_.resize(texelCount() * n);
    // Each texel samples the image at its center, clamped to the texel centers at the border
    // (luminanceAt, imageColorAt). Where the sample's position lands exactly on a texel center
    // (an image of this size: every texel, for sizes whose texel centers are exact in floats)
    // the bilinear weights are 0, and the four texels are read straight from the image with the
    // same arithmetic: the same values, without the sampling's wrapping and flooring.
    const Texture::Image* img = image.image();
    const int w = width_, h = height_;
    const float hu = 0.5f / static_cast<float>(std::max(w, 1));
    const float hv = 0.5f / static_cast<float>(std::max(h, 1));
    std::vector<int> column(static_cast<std::size_t>(w), -1);  // the image column a texel reads exactly, -1: sampled
    for (int x = 0; x < w; ++x) {
        const float u = std::clamp((static_cast<float>(x) + 0.5f) / static_cast<float>(w), hu, 1.0f - hu);
        const float fx = u * static_cast<float>(img->width) - 0.5f;
        if (fx == std::floor(fx) && fx >= 0.0f && fx < static_cast<float>(img->width)) column[static_cast<std::size_t>(x)] = static_cast<int>(fx);
    }
    parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t row) {
        const int y = static_cast<int>(row);
        const float v = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
        const float vc = std::clamp(v, hv, 1.0f - hv);
        const float fy = (1.0f - vc) * static_cast<float>(img->height) - 0.5f;
        const bool rowExact = fy == std::floor(fy) && fy >= 0.0f && fy < static_cast<float>(img->height);
        const int y0 = rowExact ? static_cast<int>(fy) : 0, y1 = (y0 + 1) % std::max(img->height, 1);
        for (int x = 0; x < w; ++x) {
            const std::size_t i = index(x, y);
            const int x0 = column[static_cast<std::size_t>(x)];
            std::array<float, 4> c;
            if (rowExact && x0 >= 0) {
                const int x1 = (x0 + 1) % img->width;
                const float* a = img->rgba.data() + (static_cast<std::size_t>(y0) * static_cast<std::size_t>(img->width) + static_cast<std::size_t>(x0)) * 4;
                const float* b = img->rgba.data() + (static_cast<std::size_t>(y0) * static_cast<std::size_t>(img->width) + static_cast<std::size_t>(x1)) * 4;
                const float* cc = img->rgba.data() + (static_cast<std::size_t>(y1) * static_cast<std::size_t>(img->width) + static_cast<std::size_t>(x0)) * 4;
                const float* d = img->rgba.data() + (static_cast<std::size_t>(y1) * static_cast<std::size_t>(img->width) + static_cast<std::size_t>(x1)) * 4;
                for (int k = 0; k < 4; ++k) {
                    const float top = a[k] + (b[k] - a[k]) * 0.0f;
                    const float bottom = cc[k] + (d[k] - cc[k]) * 0.0f;
                    c[static_cast<std::size_t>(k)] = top + (bottom - top) * 0.0f;
                }
            } else {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                c = imageColorAt(image, u, v);
            }
            if (n == 1) {
                values_[i] = std::clamp(luminance(c), 0.0f, 1.0f);
            } else {
                for (std::size_t k = 0; k < 4; ++k) values_[i * 4 + k] = std::clamp(c[k], 0.0f, 1.0f);
            }
        }
    }, 4);
    table_ = texelTable(scalp, width_, height_);
    dirty_ = TexelRect::all(width_, height_);
}

PaintColor TexturePaint::colorAt(int x, int y) const {
    const std::size_t i = index(x, y);
    if (channels_ == kGrayMap) return {values_[i], values_[i], values_[i], 1.0f};
    return {values_[i * 4], values_[i * 4 + 1], values_[i * 4 + 2], values_[i * 4 + 3]};
}

void TexturePaint::requireNotEditing(const char* what) const {
    if (editing_) throw std::logic_error(std::string("TexturePaint: ") + what + " while an edit is being recorded");
}

void TexturePaint::setChannels(int channels) {
    requireNotEditing("setChannels");
    values_ = convertMapChannels(values_, channels_, channels);
    channels_ = channels;
    dirty_ = TexelRect::all(width_, height_);
}

TexturePaint::Reach TexturePaint::reachOf(const Vec3f& center, float radius, const BrushWeightFn& weight,
                                          bool threadSafeWeight) const {
    Reach out;
    const TexelTable& t = *table_;
    if (t.nx == 0 || radius <= 0.0f) return out;
    const int ix0 = std::clamp(static_cast<int>((center.x - radius - t.gridLo.x) / t.cell), 0, t.nx - 1);
    const int ix1 = std::clamp(static_cast<int>((center.x + radius - t.gridLo.x) / t.cell), 0, t.nx - 1);
    const int iy0 = std::clamp(static_cast<int>((center.y - radius - t.gridLo.y) / t.cell), 0, t.ny - 1);
    const int iy1 = std::clamp(static_cast<int>((center.y + radius - t.gridLo.y) / t.cell), 0, t.ny - 1);
    const int iz0 = std::clamp(static_cast<int>((center.z - radius - t.gridLo.z) / t.cell), 0, t.nz - 1);
    const int iz1 = std::clamp(static_cast<int>((center.z + radius - t.gridLo.z) / t.cell), 0, t.nz - 1);
    const float r2 = radius * radius;
    // The covered texels within the sphere, in the grid's order...
    std::vector<std::uint32_t> inside;
    std::size_t nearest = 0;
    float nearestDistance = std::numeric_limits<float>::max();
    for (int iz = iz0; iz <= iz1; ++iz) {
        for (int iy = iy0; iy <= iy1; ++iy) {
            for (int ix = ix0; ix <= ix1; ++ix) {
                const std::size_t c = (static_cast<std::size_t>(iz) * static_cast<std::size_t>(t.ny) + static_cast<std::size_t>(iy)) * static_cast<std::size_t>(t.nx) + static_cast<std::size_t>(ix);
                for (std::uint32_t k = t.cellStart[c]; k < t.cellStart[c + 1]; ++k) {
                    const std::size_t i = t.cellTexels[k];
                    const Vec3f d = t.positions[i] - center;
                    const float d2 = d.dot(d);
                    if (d2 > r2) continue;
                    if (d2 < nearestDistance) nearestDistance = d2, nearest = i;
                    inside.push_back(static_cast<std::uint32_t>(i));
                }
            }
        }
    }
    if (inside.empty()) return out;
    // ...on the surface connected to the nearest one's triangle within the sphere (both sides of
    // a UV seam; not another part of the scalp the sphere reaches across a gap)...
    if (t.surface) {
        thread_local std::vector<std::uint8_t> reached;
        const std::vector<std::int32_t> faces = reachedFaces(*t.surface, t.roots[nearest].face, center, radius, reached);
        if (faces.size() < t.surface->triangles.size()) {
            inside.erase(std::remove_if(inside.begin(), inside.end(),
                                        [&](std::uint32_t i) { return !reached[static_cast<std::size_t>(t.roots[i].face)]; }),
                         inside.end());
        }
    }
    // ...and the brush's weight at each, in parallel when it may be (the sphere's always may).
    std::vector<float> weights(inside.size());
    auto weigh = [&](std::size_t k) { weights[k] = brushWeight(t.positions[inside[k]], center, radius, 0.0f, weight); };
    if ((!weight || threadSafeWeight) && inside.size() >= 4096) {
        parallelFor(0, inside.size(), weigh, 1024);
    } else {
        for (std::size_t k = 0; k < inside.size(); ++k) weigh(k);
    }
    out.reserve(inside.size());
    for (std::size_t k = 0; k < inside.size(); ++k) {
        if (weights[k] != 0.0f) out.emplace_back(inside[k], weights[k]);  // a weight of 0 does nothing: left out
    }
    return out;
}

std::size_t TexturePaint::stamp(const Vec3f& center, float radius, float strength, float value, const BrushWeightFn& weight) {
    return stamp(center, radius, strength, {value, value, value, value}, kAllChannels, weight);
}

std::size_t TexturePaint::stamp(const Vec3f& center, float radius, float strength, const PaintColor& target, unsigned mask,
                                const BrushWeightFn& weight) {
    return stampReach(reachOf(center, radius, weight), strength, target, mask);
}

void TexturePaint::keepTiles(const Reach& reach) {
    if (!editing_) return;
    const auto w = static_cast<std::size_t>(width_);
    const auto across = static_cast<std::size_t>(tilesAcross());
    const auto n = static_cast<std::size_t>(channels_);
    for (const auto& entry : reach) {
        const std::size_t i = entry.first;
        const std::size_t x = i % w, y = i / w;
        const std::size_t tile = (y / kEditTile) * across + x / kEditTile;
        if (editSlot_[tile] != 0) continue;
        editTiles_.push_back(static_cast<std::uint32_t>(tile));
        editSlot_[tile] = static_cast<std::uint32_t>(editTiles_.size());
        editOffset_[tile] = editBefore_.size() + 1;
        const std::size_t tx0 = (tile % across) * kEditTile, ty0 = (tile / across) * kEditTile;
        const std::size_t tx1 = std::min(tx0 + kEditTile, w), ty1 = std::min(ty0 + kEditTile, static_cast<std::size_t>(height_));
        for (std::size_t ty = ty0; ty < ty1; ++ty) {
            const float* from = values_.data() + (ty * w + tx0) * n;
            editBefore_.insert(editBefore_.end(), from, from + (tx1 - tx0) * n);
        }
    }
}

float TexturePaint::editStartValue(std::size_t texel, std::size_t channel) const {
    if (!editing_) return values_[texel * static_cast<std::size_t>(channels_) + channel];
    const auto w = static_cast<std::size_t>(width_);
    const auto n = static_cast<std::size_t>(channels_);
    const auto across = static_cast<std::size_t>(tilesAcross());
    const std::size_t x = texel % w, y = texel / w;
    const std::size_t tile = (y / kEditTile) * across + x / kEditTile;
    if (tile >= editOffset_.size() || editOffset_[tile] == 0) return values_[texel * n + channel];
    const std::size_t tx0 = (tile % across) * kEditTile, ty0 = (tile / across) * kEditTile;
    const std::size_t tx1 = std::min(tx0 + kEditTile, w);
    const std::size_t local = ((y - ty0) * (tx1 - tx0) + (x - tx0)) * n + channel;
    return editBefore_[editOffset_[tile] - 1 + local];
}

std::size_t TexturePaint::stampReach(const Reach& reach, float strength, const PaintColor& target, unsigned mask,
                                     PaintMode mode, bool fromEditStart) {
    const auto n = static_cast<std::size_t>(channels_);
    const unsigned m = effectiveMask(channels_, mask);
    keepTiles(reach);
    const auto w = static_cast<std::size_t>(width_);
    std::size_t changed = 0;
    std::size_t lo = std::numeric_limits<std::size_t>::max(), hi = 0;  // the changed texels' rows
    std::size_t left = std::numeric_limits<std::size_t>::max(), right = 0;
    for (const auto& [i, weightThere] : reach) {
        const float wt = std::min(weightThere * strength, 1.0f);
        if (!(wt > 0.0f)) continue;
        bool any = false;
        for (std::size_t c = 0; c < n; ++c) {
            if (!(m & (1u << c))) continue;
            float& x = values_[i * n + c];
            const float start = fromEditStart ? editStartValue(i, c) : x;
            const float composed = compositePaintValue(start, target[c], mode);
            const float next = std::clamp(x + (composed - x) * wt, 0.0f, 1.0f);
            if (next != x) {
                x = next;
                any = true;
            }
        }
        if (any) {
            ++changed;
            const std::size_t row = i / w, col = i - row * w;
            lo = std::min(lo, row);
            hi = std::max(hi, row);
            left = std::min(left, col);
            right = std::max(right, col);
        }
    }
    if (changed > 0) {
        dirty_.unite({static_cast<int>(left), static_cast<int>(lo), static_cast<int>(right) + 1, static_cast<int>(hi) + 1});
    }
    return changed;
}

TexturePaint::Reach TexturePaint::reachOfTriangles(const std::vector<std::uint8_t>& triangles, const Vec3f& center,
                                                   float radius) const {
    Reach out;
    const TexelTable& t = *table_;
    if (t.nx == 0 || radius <= 0.0f) return out;
    const auto flagged = [&](std::int32_t face) {
        return face >= 0 && static_cast<std::size_t>(face) < triangles.size() && triangles[static_cast<std::size_t>(face)];
    };
    const int ix0 = std::clamp(static_cast<int>((center.x - radius - t.gridLo.x) / t.cell), 0, t.nx - 1);
    const int ix1 = std::clamp(static_cast<int>((center.x + radius - t.gridLo.x) / t.cell), 0, t.nx - 1);
    const int iy0 = std::clamp(static_cast<int>((center.y - radius - t.gridLo.y) / t.cell), 0, t.ny - 1);
    const int iy1 = std::clamp(static_cast<int>((center.y + radius - t.gridLo.y) / t.cell), 0, t.ny - 1);
    const int iz0 = std::clamp(static_cast<int>((center.z - radius - t.gridLo.z) / t.cell), 0, t.nz - 1);
    const int iz1 = std::clamp(static_cast<int>((center.z + radius - t.gridLo.z) / t.cell), 0, t.nz - 1);
    for (int iz = iz0; iz <= iz1; ++iz) {
        for (int iy = iy0; iy <= iy1; ++iy) {
            for (int ix = ix0; ix <= ix1; ++ix) {
                const std::size_t c = (static_cast<std::size_t>(iz) * static_cast<std::size_t>(t.ny) + static_cast<std::size_t>(iy)) *
                                          static_cast<std::size_t>(t.nx) + static_cast<std::size_t>(ix);
                for (std::uint32_t k = t.cellStart[c]; k < t.cellStart[c + 1]; ++k) {
                    const std::size_t i = t.cellTexels[k];
                    if (flagged(t.roots[i].face)) out.emplace_back(i, 1.0f);
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t TexturePaint::fillTriangles(const std::vector<std::uint8_t>& triangles, const PaintColor& color, unsigned mask,
                                        PaintMode mode) {
    requireNotEditing("fillTriangles");
    Reach reach;
    const TexelTable& t = *table_;
    for (std::size_t i = 0; i < t.roots.size(); ++i) {
        const std::int32_t face = t.roots[i].face;
        if (face >= 0 && static_cast<std::size_t>(face) < triangles.size() && triangles[static_cast<std::size_t>(face)]) {
            reach.emplace_back(i, 1.0f);
        }
    }
    const PaintColor target = channels_ == kGrayMap ? PaintColor{color[0], color[0], color[0], color[0]} : color;
    return stampReach(reach, 1.0f, target, mask, mode, false);
}

std::size_t TexturePaint::smooth(const Vec3f& center, float radius, float strength, const BrushWeightFn& weight, unsigned mask) {
    return smoothReach(reachOf(center, radius, weight), strength, mask);
}

std::size_t TexturePaint::smoothReach(const Reach& reach, float strength, unsigned mask) {
    const auto channels = static_cast<std::size_t>(channels_);
    const unsigned m = effectiveMask(channels_, mask);
    const float k = std::abs(strength);
    keepTiles(reach);
    // The touched texels and their weights (strength applied, > 0), in the reach's order.
    std::vector<std::pair<std::size_t, float>> touched;
    touched.reserve(reach.size());
    for (const auto& [i, weightThere] : reach) {
        const float w = std::min(weightThere * k, 1.0f);
        if (w > 0.0f) touched.emplace_back(i, w);
    }
    std::vector<float> next(touched.size() * channels);  // from the values before the stamp, channels per touched texel
    if (channels_ == kColorMap) {
        smoothTexels<4>(values_.data(), *table_, m, touched, next.data());
    } else {
        smoothTexels<1>(values_.data(), *table_, m, touched, next.data());
    }
    const auto w = static_cast<std::size_t>(width_);
    std::size_t changed = 0;
    std::size_t lo = std::numeric_limits<std::size_t>::max(), hi = 0;
    std::size_t left = std::numeric_limits<std::size_t>::max(), right = 0;
    for (std::size_t t = 0; t < touched.size(); ++t) {
        const std::size_t i = touched[t].first;
        bool any = false;
        for (std::size_t c = 0; c < channels; ++c) {
            if (next[t * channels + c] != values_[i * channels + c]) {
                values_[i * channels + c] = next[t * channels + c];
                any = true;
            }
        }
        if (any) {
            ++changed;
            const std::size_t row = i / w, col = i - row * w;
            lo = std::min(lo, row);
            hi = std::max(hi, row);
            left = std::min(left, col);
            right = std::max(right, col);
        }
    }
    if (changed > 0) {
        dirty_.unite({static_cast<int>(left), static_cast<int>(lo), static_cast<int>(right) + 1, static_cast<int>(hi) + 1});
    }
    return changed;
}

void TexturePaint::fill(float value, PaintMode mode) {
    requireNotEditing("fill");
    if (channels_ == kGrayMap) {
        fillMapValues(values_, channels_, kAllChannels, {value, value, value, value}, mode);
        dirty_ = TexelRect::all(width_, height_);
    } else {
        fill({value, value, value, 1.0f}, 0x7u, mode);  // r g b; the alpha stays
    }
}

void TexturePaint::fill(const PaintColor& color, unsigned mask, PaintMode mode) {
    requireNotEditing("fill");
    fillMapValues(values_, channels_, mask, color, mode);
    dirty_ = TexelRect::all(width_, height_);
}

void TexturePaint::beginEdit() {
    requireNotEditing("beginEdit");
    editing_ = true;
    const std::size_t tiles = static_cast<std::size_t>(tilesAcross()) * static_cast<std::size_t>((height_ + kEditTile - 1) / kEditTile);
    editSlot_.assign(tiles, 0);
    editOffset_.assign(tiles, 0);
    editTiles_.clear();
    editBefore_.clear();
}

TextureEdit TexturePaint::endEdit() {
    TextureEdit edit;
    edit.width = width_;
    edit.height = height_;
    edit.channels = channels_;
    if (!editing_) return edit;
    editing_ = false;
    const auto w = static_cast<std::size_t>(width_);
    const auto across = static_cast<std::size_t>(tilesAcross());
    const auto n = static_cast<std::size_t>(channels_);
    // Where each kept tile's values before start in editBefore_ (tiles are cut at the edges).
    std::vector<std::size_t> offset(editTiles_.size() + 1, 0);
    auto tileSize = [&](std::size_t tile) {
        const std::size_t tx0 = (tile % across) * kEditTile, ty0 = (tile / across) * kEditTile;
        return (std::min(tx0 + kEditTile, w) - tx0) * (std::min(ty0 + kEditTile, static_cast<std::size_t>(height_)) - ty0) * n;
    };
    for (std::size_t k = 0; k < editTiles_.size(); ++k) offset[k + 1] = offset[k] + tileSize(editTiles_[k]);
    std::vector<std::size_t> order(editTiles_.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return editTiles_[a] < editTiles_[b]; });
    std::vector<float> now;
    for (const std::size_t k : order) {
        const std::size_t tile = editTiles_[k];
        const std::size_t tx0 = (tile % across) * kEditTile, ty0 = (tile / across) * kEditTile;
        const std::size_t tx1 = std::min(tx0 + kEditTile, w), ty1 = std::min(ty0 + kEditTile, static_cast<std::size_t>(height_));
        now.clear();
        for (std::size_t ty = ty0; ty < ty1; ++ty) {
            const float* from = values_.data() + (ty * w + tx0) * n;
            now.insert(now.end(), from, from + (tx1 - tx0) * n);
        }
        const float* before = editBefore_.data() + offset[k];
        if (std::memcmp(before, now.data(), now.size() * sizeof(float)) == 0) continue;  // came back as it was
        edit.tiles.push_back(static_cast<std::uint32_t>(tile));
        edit.before.insert(edit.before.end(), before, before + now.size());
        edit.after.insert(edit.after.end(), now.begin(), now.end());
    }
    editSlot_.clear();
    editSlot_.shrink_to_fit();
    editOffset_.clear();
    editOffset_.shrink_to_fit();
    editTiles_.clear();
    editBefore_.clear();
    editBefore_.shrink_to_fit();
    return edit;
}

void TexturePaint::applyEdit(const TextureEdit& edit, bool after) {
    requireNotEditing("applyEdit");
    if (edit.width != width_ || edit.height != height_ || edit.channels != channels_) {
        throw std::invalid_argument("TexturePaint::applyEdit: the edit is of another image");
    }
    const auto w = static_cast<std::size_t>(width_);
    const auto across = static_cast<std::size_t>(tilesAcross());
    const auto n = static_cast<std::size_t>(channels_);
    const std::vector<float>& from = after ? edit.after : edit.before;
    std::size_t at = 0;
    for (const std::uint32_t tile : edit.tiles) {
        const std::size_t tx0 = (tile % across) * kEditTile, ty0 = (tile / across) * kEditTile;
        const std::size_t tx1 = std::min(tx0 + kEditTile, w), ty1 = std::min(ty0 + kEditTile, static_cast<std::size_t>(height_));
        const std::size_t row = (tx1 - tx0) * n;
        if (at + row * (ty1 - ty0) > from.size()) throw std::invalid_argument("TexturePaint::applyEdit: the edit's values are short");
        for (std::size_t ty = ty0; ty < ty1; ++ty) {
            std::memcpy(values_.data() + (ty * w + tx0) * n, from.data() + at, row * sizeof(float));
            at += row;
        }
        dirty_.unite({static_cast<int>(tx0), static_cast<int>(ty0), static_cast<int>(tx1), static_cast<int>(ty1)});
    }
}

std::vector<std::uint8_t> TexturePaint::toGray8(int padding) const {
    if (channels_ == kGrayMap) return toBytes(padding);
    const std::vector<std::uint8_t> rgba = toBytes(padding);
    std::vector<std::uint8_t> gray(texelCount());
    for (std::size_t i = 0; i < gray.size(); ++i) {
        const PaintColor c{rgba[i * 4] / 255.0f, rgba[i * 4 + 1] / 255.0f, rgba[i * 4 + 2] / 255.0f, 1.0f};
        gray[i] = static_cast<std::uint8_t>(std::lround(std::clamp(luminance(c), 0.0f, 1.0f) * 255.0f));
    }
    return gray;
}

namespace {

std::uint8_t toByte(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

void TexturePaint::toBytesUnpadded(const TexelRect& rect, std::uint8_t* bytes) const {
    const auto channels = static_cast<std::size_t>(channels_);
    const int x0 = std::max(rect.x0, 0), x1 = std::min(rect.x1, width_);
    const int y0 = std::max(rect.y0, 0), y1 = std::min(rect.y1, height_);
    if (x1 <= x0 || y1 <= y0) return;
    const std::uint8_t* covered = table_->covered.data();
    parallelFor(static_cast<std::size_t>(y0), static_cast<std::size_t>(y1), [&](std::size_t y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = index(x, static_cast<int>(y));
            std::uint8_t* out = bytes + i * channels;
            if (!covered[i]) {
                for (std::size_t c = 0; c < channels; ++c) out[c] = 0;
                continue;
            }
            for (std::size_t c = 0; c < channels; ++c) out[c] = toByte(values_[i * channels + c]);
        }
    }, 16);
}

std::vector<std::uint8_t> TexturePaint::toBytes(int padding) const {
    const std::size_t texels = texelCount();
    const auto channels = static_cast<std::size_t>(channels_);
    std::vector<std::uint8_t> bytes(texels * channels, 0);
    if (padding <= 0) {  // the covered texels as they are, nothing grown: no copy of the values
        toBytesUnpadded(TexelRect::all(width_, height_), bytes.data());
        return bytes;
    }
    const std::vector<std::uint8_t>& covered = table_->covered;
    // Each pass grows the covered area by one texel (4-neighborhood), each new texel the mean of
    // its neighbors that had a value when the pass began. Only the texels at the border of what
    // has a value change in a pass (the frontier); the grown values sit beside the covered ones.
    // A texel's value depends only on which texels had one when its pass began, so the frontier
    // is found and grown in parallel, row by row and texel by texel.
    std::vector<std::uint8_t> have(covered);
    std::unique_ptr<float[]> grown(new float[texels * channels]);
    auto valueAt = [&](std::size_t j) { return covered[j] ? &values_[j * channels] : &grown[j * channels]; };
    const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    std::vector<std::uint8_t> queued(texels, 0);
    std::vector<std::vector<std::size_t>> rowFrontier(static_cast<std::size_t>(height_));
    parallelFor(0, static_cast<std::size_t>(height_), [&](std::size_t row) {
        const int y = static_cast<int>(row);
        for (int x = 0; x < width_; ++x) {
            const std::size_t i = index(x, y);
            if (have[i]) continue;
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width_ || ny >= height_ || !have[index(nx, ny)]) continue;
                rowFrontier[row].push_back(i);
                queued[i] = 1;
                break;
            }
        }
    }, 8);
    std::vector<std::size_t> frontier;
    for (auto& r : rowFrontier) frontier.insert(frontier.end(), r.begin(), r.end());
    rowFrontier.clear();
    rowFrontier.shrink_to_fit();
    std::vector<std::size_t> next;
    for (int pass = 0; pass < padding && !frontier.empty(); ++pass) {
        parallelFor(0, frontier.size(), [&](std::size_t f) {
            const std::size_t i = frontier[f];
            const int x = static_cast<int>(i % static_cast<std::size_t>(width_)), y = static_cast<int>(i / static_cast<std::size_t>(width_));
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            int n = 0;
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width_ || ny >= height_) continue;
                const std::size_t j = index(nx, ny);
                if (!have[j]) continue;
                const float* v = valueAt(j);
                for (std::size_t c = 0; c < channels; ++c) sum[c] += v[c];
                ++n;
            }
            for (std::size_t c = 0; c < channels; ++c) grown[i * channels + c] = sum[c] / static_cast<float>(n);
        }, 1024);
        next.clear();
        for (const std::size_t i : frontier) have[i] = 1;
        for (const std::size_t i : frontier) {
            const int x = static_cast<int>(i % static_cast<std::size_t>(width_)), y = static_cast<int>(i / static_cast<std::size_t>(width_));
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width_ || ny >= height_) continue;
                const std::size_t j = index(nx, ny);
                if (have[j] || queued[j]) continue;
                queued[j] = 1;
                next.push_back(j);
            }
        }
        frontier.swap(next);
    }
    parallelFor(0, static_cast<std::size_t>(height_), [&](std::size_t row) {
        const std::size_t start = row * static_cast<std::size_t>(width_);
        for (std::size_t i = start; i < start + static_cast<std::size_t>(width_); ++i) {
            if (!have[i]) continue;
            const float* v = valueAt(i);
            for (std::size_t c = 0; c < channels; ++c) bytes[i * channels + c] = toByte(v[c]);
        }
    }, 16);
    return bytes;
}

void TexturePaint::save(const std::string& path, int padding) const { writePng(path, width_, height_, channels_, toBytes(padding)); }

std::vector<std::uint8_t> TexturePaint::pngBytes(int padding) const {
    return storedPngBytes(width_, height_, channels_, toBytes(padding));
}

// ---- The viewport's image ------------------------------------------------------

namespace {

// One texel as the viewport shows it (MapDisplay): `src` holds `channels` bytes, `dst` gets r g b a.
inline void displayPixel(const std::uint8_t* src, int channels, const MapDisplay& display, std::uint8_t* dst) {
    std::uint8_t px[4];
    if (channels == 1) {
        px[0] = px[1] = px[2] = src[0];
        px[3] = 255;
    } else {
        px[0] = src[0];
        px[1] = src[1];
        px[2] = src[2];
        px[3] = src[3];
    }
    if (display.mapChannels == kColorMap) {
        // A color map: its color, or a channel (alpha too) as gray; nothing cut out.
        if (display.pick >= 0 && display.pick < 4) px[0] = px[1] = px[2] = px[display.pick];
        px[3] = 255;
    } else if (!display.keepAlpha) {
        px[3] = 255;
    }
    dst[0] = px[0];
    dst[1] = px[1];
    dst[2] = px[2];
    dst[3] = px[3];
}

}  // namespace

void displayPixels(const std::uint8_t* pixels, int width, int height, int channels, const MapDisplay& display,
                   const TexelRect& rect, std::uint8_t* rgba) {
    const int x0 = std::max(rect.x0, 0), x1 = std::min(rect.x1, width);
    const int y0 = std::max(rect.y0, 0), y1 = std::min(rect.y1, height);
    if (x1 <= x0 || y1 <= y0) return;
    const auto w = static_cast<std::size_t>(width);
    const auto n = static_cast<std::size_t>(channels);
    parallelFor(static_cast<std::size_t>(y0), static_cast<std::size_t>(y1), [&](std::size_t y) {
        const std::uint8_t* row = pixels + y * w * n;
        std::uint8_t* out = rgba + (static_cast<std::size_t>(height) - 1 - y) * w * 4;
        for (int x = x0; x < x1; ++x) displayPixel(row + static_cast<std::size_t>(x) * n, channels, display, out + static_cast<std::size_t>(x) * 4);
    }, 16);
}

namespace {

// Calls f(row, column, bytes) for each texel of `rect` with its 8-bit values as the preview shows
// them (TexturePaint::toPreviewBytes), rows in parallel.
template <class F>
void shownTexels(const TexelTable& table, const std::vector<float>& values, int channels, const TexelRect& rect, F&& f) {
    const auto n = static_cast<std::size_t>(channels);
    const auto w = static_cast<std::size_t>(table.width);
    parallelFor(static_cast<std::size_t>(rect.y0), static_cast<std::size_t>(rect.y1), [&](std::size_t y) {
        auto pad = std::lower_bound(table.padTexels.begin(), table.padTexels.end(), static_cast<std::uint32_t>(y * w + static_cast<std::size_t>(rect.x0)));
        for (int x = rect.x0; x < rect.x1; ++x) {
            const std::size_t i = y * w + static_cast<std::size_t>(x);
            std::uint8_t src[4] = {0, 0, 0, 0};
            std::size_t from = i;
            if (!table.covered[i]) {
                while (pad != table.padTexels.end() && *pad < i) ++pad;
                from = pad != table.padTexels.end() && *pad == i ? table.padSources[static_cast<std::size_t>(pad - table.padTexels.begin())]
                                                                 : TexelTable::kNoTexel;
            }
            if (from != TexelTable::kNoTexel) {
                for (std::size_t c = 0; c < n; ++c) src[c] = toByte(values[from * n + c]);
            }
            f(y, x, src);
        }
    }, 16);
}

}  // namespace

std::vector<std::uint8_t> TexturePaint::toPreviewBytes() const {
    const auto n = static_cast<std::size_t>(channels_);
    std::vector<std::uint8_t> bytes(texelCount() * n, 0);
    shownTexels(*table_, values_, channels_, TexelRect::all(width_, height_), [&](std::size_t y, int x, const std::uint8_t* src) {
        std::copy_n(src, n, bytes.data() + index(x, static_cast<int>(y)) * n);
    });
    return bytes;
}

TexelRect TexturePreview::update(TexturePaint& paint, const MapDisplay& display) {
    TexelRect rect = paint.dirty();
    if (paint.width() != width_ || paint.height() != height_ || display != display_ || rgba_.empty()) {
        width_ = paint.width();
        height_ = paint.height();
        display_ = display;
        rgba_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4, 0);
        rect = TexelRect::all(width_, height_);
        static std::atomic<std::uint64_t> layouts{0};
        layout_ = ++layouts;
        history_.clear();
    }
    paint.clearDirty();
    if (rect.empty()) return {};
    // The padding around the changed texels copies them too.
    rect = {rect.x0 - TexelTable::kPadding, rect.y0 - TexelTable::kPadding, rect.x1 + TexelTable::kPadding,
            rect.y1 + TexelTable::kPadding};
    rect.x0 = std::max(rect.x0, 0);
    rect.y0 = std::max(rect.y0, 0);
    rect.x1 = std::min(rect.x1, width_);
    rect.y1 = std::min(rect.y1, height_);
    if (rect.empty()) return {};
    // The map's 8-bit values as toPreviewBytes gives them, straight into the shown pixels.
    const int channels = paint.channels();
    const auto w = static_cast<std::size_t>(width_);
    shownTexels(*paint.table(), static_cast<const TexturePaint&>(paint).values(), channels, rect,
                [&](std::size_t y, int x, const std::uint8_t* src) {
                    displayPixel(src, channels, display_, rgba_.data() + (static_cast<std::size_t>(height_) - 1 - y) * w * 4 + static_cast<std::size_t>(x) * 4);
                });
    ++revision_;
    history_.emplace_back(revision_, rect);
    constexpr std::size_t kHistory = 64;
    if (history_.size() > kHistory) history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - kHistory));
    return rect;
}

TexelRect TexturePreview::changedSince(std::uint64_t revision, std::uint64_t layout) const {
    if (layout != layout_ || revision > revision_) return TexelRect::all(width_, height_);
    if (revision == revision_) return {};
    if (history_.empty() || history_.front().first > revision + 1) return TexelRect::all(width_, height_);
    TexelRect out;
    for (const auto& [r, rect] : history_) {
        if (r > revision) out.unite(rect);
    }
    return out;
}

// ---- PNG ----------------------------------------------------------------------

namespace {

// CRC-32 (PNG's, zlib's), eight bytes a step through eight tables (slicing-by-8): the same
// value as the byte-at-a-time loop.
struct CrcTables {
    std::uint32_t t[8][256];
    CrcTables() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (std::uint32_t i = 0; i < 256; ++i) {
            for (int k = 1; k < 8; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFFu];
        }
    }
};

std::uint32_t crc32Of(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
    static const CrcTables tables;
    const auto& t = tables.t;
    crc = ~crc;
    while (n >= 8) {
        const std::uint32_t lo = crc ^ (static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8 |
                                        static_cast<std::uint32_t>(data[2]) << 16 | static_cast<std::uint32_t>(data[3]) << 24);
        crc = t[7][lo & 0xFFu] ^ t[6][(lo >> 8) & 0xFFu] ^ t[5][(lo >> 16) & 0xFFu] ^ t[4][lo >> 24] ^ t[3][data[4]] ^
              t[2][data[5]] ^ t[1][data[6]] ^ t[0][data[7]];
        data += 8;
        n -= 8;
    }
    while (n-- > 0) crc = t[0][(crc ^ *data++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// A chunk whose data is out[start + 8 ..]: its length and type before it (made room for), its CRC after.
void closeChunk(std::vector<std::uint8_t>& out, std::size_t start, const char* type) {
    const auto length = static_cast<std::uint32_t>(out.size() - start - 8);
    for (int k = 0; k < 4; ++k) out[start + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(length >> (24 - 8 * k));
    std::memcpy(out.data() + start + 4, type, 4);
    put32(out, crc32Of(out.data() + start + 4, out.size() - start - 4));
}

}  // namespace

std::vector<std::uint8_t> storedPngBytes(int width, int height, int channels, const std::vector<std::uint8_t>& pixels) {
    if (width <= 0 || height <= 0 || channels < 1 || channels > 4) throw std::runtime_error("writePng: bad size or channels");
    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    if (pixels.size() != stride * static_cast<std::size_t>(height)) throw std::runtime_error("writePng: pixel count does not match the size");
    // The image data: filter byte 0 before each row, then zlib with stored (uncompressed) deflate
    // blocks of up to 65535 bytes of it, then its Adler-32. Made straight into the IDAT chunk.
    const std::size_t rawSize = (stride + 1) * static_cast<std::size_t>(height);
    const std::size_t blocks = std::max<std::size_t>(1, (rawSize + 65534) / 65535);
    std::vector<std::uint8_t> out;
    out.reserve(8 + 25 + 12 + 2 + blocks * 5 + rawSize + 4 + 12);
    out.insert(out.end(), {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    std::size_t chunk = out.size();
    out.resize(chunk + 8);
    put32(out, static_cast<std::uint32_t>(width));
    put32(out, static_cast<std::uint32_t>(height));
    out.push_back(8);  // bit depth
    const std::uint8_t colorType[5] = {0, 0, 4, 2, 6};  // gray, gray+alpha, rgb, rgba
    out.push_back(colorType[channels]);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    closeChunk(out, chunk, "IHDR");

    chunk = out.size();
    out.resize(chunk + 8);
    out.push_back(0x78);
    out.push_back(0x01);
    // Adler-32 with the modulo taken every 5552 bytes (zlib's NMAX: the sums cannot overflow
    // before it), the same value as taking it every byte.
    std::uint32_t a = 1, b = 0;
    std::size_t sinceModulo = 0;
    auto addAdler = [&](const std::uint8_t* data, std::size_t n) {
        while (n > 0) {
            const std::size_t run = std::min<std::size_t>(n, 5552 - sinceModulo);
            for (std::size_t k = 0; k < run; ++k) {
                a += data[k];
                b += a;
            }
            data += run;
            n -= run;
            sinceModulo += run;
            if (sinceModulo == 5552) {
                a %= 65521u;
                b %= 65521u;
                sinceModulo = 0;
            }
        }
    };
    // Copies raw[at, at + n) (row r's bytes at r * (stride + 1): its filter byte, then the row).
    const std::uint8_t zero = 0;
    auto copyRaw = [&](std::size_t at, std::size_t n) {
        while (n > 0) {
            const std::size_t row = at / (stride + 1), column = at % (stride + 1);
            const std::uint8_t* from = column == 0 ? &zero : pixels.data() + row * stride + (column - 1);
            const std::size_t run = column == 0 ? 1 : std::min(n, stride + 1 - column);
            out.insert(out.end(), from, from + run);
            addAdler(from, run);
            at += run;
            n -= run;
        }
    };
    std::size_t at = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, rawSize - at);
        const bool last = at + n >= rawSize;
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(n & 0xFFu));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(~n & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFFu));
        copyRaw(at, n);
        at += n;
    } while (at < rawSize);
    a %= 65521u;
    b %= 65521u;
    put32(out, (b << 16) | a);
    closeChunk(out, chunk, "IDAT");

    chunk = out.size();
    out.resize(chunk + 8);
    closeChunk(out, chunk, "IEND");
    return out;
}

void writePng(const std::string& path, int width, int height, int channels, const std::vector<std::uint8_t>& pixels) {
    const std::vector<std::uint8_t> out = storedPngBytes(width, height, channels, pixels);
    std::ofstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("can't write " + path);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) throw std::runtime_error("can't write " + path);
}

void writeImagePng(const Texture& image, const std::string& path) {
    requireOneImage(image);
    const int w = image.width(), h = image.height();
    if (w <= 0 || h <= 0) throw std::invalid_argument("writeImagePng: the image has no size");
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> rgba(count * 4);
    bool gray = true;
    auto byte = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (int y = 0; y < h; ++y) {
        const float v = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(h);  // row 0 is the top
        for (int x = 0; x < w; ++x) {
            const std::array<float, 4> c = image.sample((static_cast<float>(x) + 0.5f) / static_cast<float>(w), v);
            std::uint8_t* px = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
            for (int k = 0; k < 4; ++k) px[k] = byte(c[static_cast<std::size_t>(k)]);
            gray = gray && px[0] == px[1] && px[1] == px[2] && px[3] == 255;
        }
    }
    if (!gray) {
        writeCompressedPng(path, w, h, 4, rgba);
        return;
    }
    std::vector<std::uint8_t> values(count);
    for (std::size_t i = 0; i < count; ++i) values[i] = rgba[i * 4];
    writeCompressedPng(path, w, h, 1, values);
}

// ---- Conversions ----------------------------------------------------------------

std::vector<float> sampleGuideLayer(const GuideSet& guides, const std::string& name, const ScalpMesh* scalp,
                                    const std::vector<Vec3f>& points) {
    std::vector<Vec3f> roots;
    std::vector<float> values;
    for (const Guide& g : guides.guides()) {
        if (g.points.empty()) continue;
        const bool rooted = scalp && scalp->contains(g.root);
        roots.push_back(rooted ? scalp->restPositionOr(g.root, g.points.front()) : g.points.front());
        auto it = g.attributes.find(name);
        values.push_back(it == g.attributes.end() ? 0.0f : it->second);
    }
    return sampleRootValues(roots, values, points);
}

std::vector<float> sampleRootValues(const std::vector<Vec3f>& roots, const std::vector<float>& values,
                                    const std::vector<Vec3f>& points) {
    std::vector<float> out(points.size(), 0.0f);
    if (roots.empty()) return out;
    const PointIndex index(roots);
    for (std::size_t i = 0; i < points.size(); ++i) {
        std::array<std::size_t, 4> idx{};
        std::array<float, 4> d2{};
        const std::size_t found = index.kNearest(points[i], idx.size(), idx.data(), d2.data());
        float sum = 0.0f, wsum = 0.0f;
        bool exact = false;
        for (std::size_t k = 0; k < found; ++k) {
            if (d2[k] <= 1e-12f) {
                out[i] = values[idx[k]];
                exact = true;
                break;
            }
            const float w = 1.0f / d2[k];
            sum += values[idx[k]] * w;
            wsum += w;
        }
        if (!exact) out[i] = wsum > 0.0f ? sum / wsum : 0.0f;
    }
    return out;
}

std::vector<Vec3f> vertexSamplePoints(const ScalpMesh& scalp) { return scalp.hasRest() ? scalp.restPoints() : scalp.points(); }

std::vector<float> vertexMapFromTexture(const Texture& image, const ScalpMesh& scalp) {
    requireOneImage(image);
    if (!scalp.hasUVs()) throw std::invalid_argument("the scalp has no UVs to read a texture map at");
    std::vector<float> sum(scalp.pointCount(), 0.0f);
    std::vector<int> count(scalp.pointCount(), 0);
    const auto& uvs = scalp.cornerUVs();
    for (std::size_t tri = 0; tri < scalp.triangleCount(); ++tri) {
        for (std::size_t k = 0; k < 3; ++k) {
            const auto v = static_cast<std::size_t>(scalp.triangles()[tri][k]);
            const ScalpMesh::UV& uv = uvs[tri * 3 + k];
            sum[v] += luminanceAt(image, uv[0], uv[1]);
            ++count[v];
        }
    }
    for (std::size_t v = 0; v < sum.size(); ++v) sum[v] = count[v] ? std::clamp(sum[v] / static_cast<float>(count[v]), 0.0f, 1.0f) : 0.0f;
    return sum;
}

std::vector<float> vertexMapFromTexture(const Texture& image, const ScalpMesh& scalp, int channels) {
    requireChannels(channels, "vertexMapFromTexture");
    if (channels == kGrayMap) return vertexMapFromTexture(image, scalp);
    requireOneImage(image);
    if (!scalp.hasUVs()) throw std::invalid_argument("the scalp has no UVs to read a texture map at");
    std::vector<float> sum(scalp.pointCount() * 4, 0.0f);
    std::vector<int> count(scalp.pointCount(), 0);
    const auto& uvs = scalp.cornerUVs();
    for (std::size_t tri = 0; tri < scalp.triangleCount(); ++tri) {
        for (std::size_t k = 0; k < 3; ++k) {
            const auto v = static_cast<std::size_t>(scalp.triangles()[tri][k]);
            const ScalpMesh::UV& uv = uvs[tri * 3 + k];
            const PaintColor c = imageColorAt(image, uv[0], uv[1]);
            for (std::size_t ch = 0; ch < 4; ++ch) sum[v * 4 + ch] += c[ch];
            ++count[v];
        }
    }
    for (std::size_t v = 0; v < count.size(); ++v) {
        for (std::size_t ch = 0; ch < 4; ++ch) {
            float& x = sum[v * 4 + ch];
            x = count[v] ? std::clamp(x / static_cast<float>(count[v]), 0.0f, 1.0f) : (ch == 3 ? 1.0f : 0.0f);
        }
    }
    return sum;
}

TexturePaint textureFromVertexMap(const std::vector<float>& values, const ScalpMesh& scalp, int width, int height) {
    if (values.size() != scalp.pointCount()) throw std::invalid_argument("textureFromVertexMap: one value per scalp vertex");
    TexturePaint t(scalp, width, height, 0.0f);
    t.assign([&](const StrandRoot& root, const Vec3f&) { return vertexMapAt(values, scalp, root); });
    return t;
}

TexturePaint textureFromVertexMap(const std::vector<float>& values, int channels, const ScalpMesh& scalp, int width, int height) {
    requireChannels(channels, "textureFromVertexMap");
    if (channels == kGrayMap) return textureFromVertexMap(values, scalp, width, height);
    if (values.size() != scalp.pointCount() * 4) throw std::invalid_argument("textureFromVertexMap: four values per scalp vertex");
    TexturePaint t(scalp, width, height, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    t.assignColor([&](const StrandRoot& root, const Vec3f&) { return vertexColorAt(values, kColorMap, scalp, root); });
    return t;
}

std::vector<float> vertexMapFromGuides(const GuideSet& guides, const std::string& name, const ScalpMesh& scalp) {
    return sampleGuideLayer(guides, name, &scalp, vertexSamplePoints(scalp));
}

TexturePaint textureFromGuides(const GuideSet& guides, const std::string& name, const ScalpMesh& scalp, int width, int height) {
    TexturePaint t(scalp, width, height, 0.0f);
    std::vector<Vec3f> points;
    std::vector<std::size_t> texels;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!t.covered(x, y)) continue;
            const StrandRoot& root = t.root(x, y);
            points.push_back(scalp.restPositionOr(root, t.position(x, y)));
            texels.push_back(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x));
        }
    }
    const std::vector<float> sampled = sampleGuideLayer(guides, name, &scalp, points);
    for (std::size_t i = 0; i < texels.size(); ++i) t.values()[texels[i]] = std::clamp(sampled[i], 0.0f, 1.0f);
    return t;
}

std::vector<float> rootValuesFromVertexMap(const std::vector<float>& values, const ScalpMesh& scalp,
                                           const std::vector<StrandRoot>& roots) {
    std::vector<float> out(roots.size());
    for (std::size_t i = 0; i < roots.size(); ++i) out[i] = vertexMapAt(values, scalp, roots[i]);
    return out;
}

std::vector<float> rootValuesFromTexture(const Texture& image, const ScalpMesh& scalp, const std::vector<StrandRoot>& roots) {
    requireOneImage(image);
    if (!scalp.hasUVs()) throw std::invalid_argument("the scalp has no UVs to read a texture map at");
    std::vector<float> out(roots.size(), 0.0f);
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (!scalp.contains(roots[i])) continue;
        const ScalpMesh::UV uv = scalp.uvAt(roots[i]);
        out[i] = std::clamp(luminanceAt(image, uv[0], uv[1]), 0.0f, 1.0f);
    }
    return out;
}

// ---- Projection onto another scalp ----------------------------------------------------

namespace {

// The surface `from` is searched on and where `to`'s points are, in one pose: both rest poses
// when both meshes have one, else both meshes' points; the mapping rebuilt on that pose.
struct ProjectionPose {
    bool rest = false;
    ScalpMesh restFrom, restTo;  // the meshes at their rest poses (when rest), with their UVs
    std::unique_ptr<ScalpMapping> restMapping;

    explicit ProjectionPose(const ScalpMapping& mapping) : rest(mapping.from().hasRest() && mapping.to().hasRest()) {
        if (!rest) return;
        restFrom = ScalpMesh(mapping.from().restPoints(), mapping.from().triangles());
        restTo = ScalpMesh(mapping.to().restPoints(), mapping.to().triangles());
        if (mapping.from().hasUVs()) restFrom.setCornerUVs(mapping.from().cornerUVs());
        if (mapping.to().hasUVs()) restTo.setCornerUVs(mapping.to().cornerUVs());
        restMapping = std::make_unique<ScalpMapping>(mapping.onMeshes(restFrom, restTo));
    }
    const ScalpMapping& mapping(const ScalpMapping& given) const { return rest ? *restMapping : given; }
};

// Each vertex of a mesh as a root (its first corner), for the uv method's lookups.
std::vector<StrandRoot> vertexRoots(const ScalpMesh& mesh) {
    std::vector<StrandRoot> roots(mesh.pointCount());
    for (std::size_t f = 0; f < mesh.triangleCount(); ++f) {
        const auto& t = mesh.triangles()[f];
        const std::int32_t face = static_cast<std::int32_t>(f);
        const StrandRoot corners[3] = {{face, 0.0f, 0.0f}, {face, 1.0f, 0.0f}, {face, 0.0f, 1.0f}};
        for (int k = 0; k < 3; ++k) {
            if (roots[static_cast<std::size_t>(t[k])].face < 0) roots[static_cast<std::size_t>(t[k])] = corners[k];
        }
    }
    return roots;
}

}  // namespace

std::vector<float> projectVertexMap(const std::vector<float>& values, int channels, const ScalpMesh& from,
                                    const ScalpMesh& to, const ScalpFit& fit) {
    return projectVertexMap(values, channels, ScalpMapping(from, to, fit));
}

std::vector<float> projectVertexMap(const std::vector<float>& values, int channels, const ScalpMapping& given) {
    const ScalpMesh& from = given.from();
    const ScalpMesh& to = given.to();
    requireChannels(channels, "projectVertexMap");
    const auto n = static_cast<std::size_t>(channels);
    if (values.size() != from.pointCount() * n) {
        throw std::invalid_argument("the map has " + std::to_string(values.size() / n) + " vertices' values and the old scalp " +
                                    std::to_string(from.pointCount()) + " vertices");
    }
    if (from.triangleCount() == 0) throw std::invalid_argument("the old scalp has no faces");
    const ProjectionPose pose(given);
    const ScalpMapping& mapping = pose.mapping(given);
    const std::vector<Vec3f>& points = pose.rest ? to.restPoints() : to.points();
    const std::vector<StrandRoot> roots = mapping.method() == TransferMethod::UV ? vertexRoots(to) : std::vector<StrandRoot>();
    std::vector<float> out(points.size() * n, 0.0f);
    parallelFor(0, points.size(), [&](std::size_t i) {
        const SurfacePoint sp = mapping.back(points[i], roots.empty() ? StrandRoot{} : roots[i]);
        if (sp.root.face < 0) return;
        const auto& t = from.triangles()[static_cast<std::size_t>(sp.root.face)];
        const float w[3] = {1.0f - sp.root.u - sp.root.v, sp.root.u, sp.root.v};
        for (std::size_t c = 0; c < n; ++c) {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 3; ++k) sum += values[static_cast<std::size_t>(t[k]) * n + c] * w[k];
            out[i * n + c] = std::clamp(sum, 0.0f, 1.0f);
        }
    });
    return out;
}

TexturePaint projectTexture(const Texture& image, int channels, const ScalpMesh& from, const ScalpMesh& to,
                            const ScalpFit& fit, int width, int height) {
    return projectTexture(image, channels, ScalpMapping(from, to, fit), width, height);
}

TexturePaint projectTexture(const Texture& image, int channels, const ScalpMapping& given, int width, int height) {
    const ScalpMesh& from = given.from();
    const ScalpMesh& to = given.to();
    requireOneImage(image);
    requireChannels(channels, "projectTexture");
    if (!from.hasUVs()) throw std::invalid_argument("the old scalp has no UVs to read the texture map at");
    if (!to.hasUVs()) throw std::invalid_argument("the new scalp has no UVs to paint a texture map on");
    if (width <= 0) width = image.width();
    if (height <= 0) height = image.height();
    TexturePaint out = channels == kColorMap ? TexturePaint(to, width, height, PaintColor{0.0f, 0.0f, 0.0f, 1.0f})
                                             : TexturePaint(to, width, height, 0.0f);
    const ProjectionPose pose(given);
    const ScalpMapping& mapping = pose.mapping(given);
    const auto n = static_cast<std::size_t>(channels);
    std::vector<float>& values = out.values();
    parallelFor(0, static_cast<std::size_t>(height), [&](std::size_t row) {
        const int y = static_cast<int>(row);
        for (int x = 0; x < width; ++x) {
            if (!out.covered(x, y)) continue;
            const Vec3f p = pose.rest ? to.restPositionOr(out.root(x, y), out.position(x, y)) : out.position(x, y);
            const SurfacePoint sp = mapping.back(p, out.root(x, y));
            if (sp.root.face < 0) continue;
            const ScalpMesh::UV uv = from.uvAt(sp.root);
            const std::size_t i = row * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (n == 1) {
                values[i] = std::clamp(luminanceAt(image, uv[0], uv[1]), 0.0f, 1.0f);
            } else {
                const PaintColor c = imageColorAt(image, uv[0], uv[1]);
                for (std::size_t k = 0; k < 4; ++k) values[i * 4 + k] = std::clamp(c[k], 0.0f, 1.0f);
            }
        }
    }, 1);
    return out;
}

// ---- Opening a texture map's paint -------------------------------------------------------------

std::shared_ptr<TexturePaint> openTexturePaint(const ScalpMesh& scalp, const std::string& path, int channels, int resolution) {
    if (!scalp.hasUVs()) throw std::runtime_error("the scalp has no UVs to paint a texture map on");
    if (fileStamp(path) != 0) return std::make_shared<TexturePaint>(scalp, *loadTexture(path), channels);
    const int size = std::clamp(resolution, 4, 16384);
    if (channels == kColorMap) return std::make_shared<TexturePaint>(scalp, size, size, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    return std::make_shared<TexturePaint>(scalp, size, size, 0.0f);
}

void requirePaintablePng(const std::string& path) {
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".png") {
        throw std::runtime_error(path + " is not a PNG, and painting writes PNG: link the map to its image again "
                                        "(Relink Image), which paints a PNG copy");
    }
}

bool KeptTexturePaint::holds(const std::string& file, const ScalpMesh& on, int channels) const {
    return paint && path == file && stamp == fileStamp(file) && scalp == on.contentHash() && paint->channels() == channels;
}

KeptTexturePaint KeptTexturePaint::open(const ScalpMesh& on, const std::string& file, int channels, int resolution) {
    KeptTexturePaint kept;
    kept.stamp = fileStamp(file);  // before reading: a write meanwhile reads as a change
    kept.paint = openTexturePaint(on, file, channels, resolution);
    kept.path = file;
    kept.scalp = on.contentHash();
    return kept;
}

std::shared_ptr<TexturePaint> KeptTexturePaint::take(const ScalpMesh& on, const std::string& file, int channels, int resolution) {
    if (!holds(file, on, channels)) *this = open(on, file, channels, resolution);
    return paint;
}

void KeptTexturePaint::written() { stamp = fileStamp(path); }

// ---- Maps onto another scalp --------------------------------------------------------------------

std::vector<std::string> projectScalpMaps(ScalpMaps& maps, const std::set<std::string>& only, const ScalpMesh& from,
                                          const ScalpMesh& to, const ScalpFit& fit, const MapProjectionFiles& files,
                                          std::vector<std::string>* warnings) {
    return projectScalpMaps(maps, only, ScalpMapping(from, to, fit), files, warnings);
}

std::vector<std::string> projectScalpMaps(ScalpMaps& maps, const std::set<std::string>& only, const ScalpMapping& mapping,
                                          const MapProjectionFiles& files, std::vector<std::string>* warnings) {
    std::vector<std::string> projected;
    for (auto& [name, entry] : maps.maps) {
        if (!only.empty() && !only.count(name)) continue;
        try {
            const int channels = entry.channels == kColorMap ? kColorMap : kGrayMap;
            if (entry.kind == "vertices") {
                entry.values = projectVertexMap(entry.values, channels, mapping);
            } else if (entry.kind == "texture") {
                const std::string full = entry.texture.empty() || !files.resolve ? std::string() : files.resolve(entry.texture);
                if (full.empty() || fileStamp(full) == 0) {
                    throw std::runtime_error("its image is missing (" + (entry.texture.empty() ? std::string("no file") : full) + ")");
                }
                if (!files.write) throw std::runtime_error("nowhere to write its projected image");
                const TexturePtr image = loadTexture(full);
                const TexturePaint paint = projectTexture(*image, channels, mapping);
                // A new file: the old one may be read by other grooms, or by the scene before this.
                entry.texture = files.write(name, paint);
            } else {
                continue;
            }
            projected.push_back(name);
        } catch (const std::exception& e) {
            if (warnings) warnings->push_back("map " + name + " was not projected: " + e.what());
        }
    }
    return projected;
}

}  // namespace groomlab
