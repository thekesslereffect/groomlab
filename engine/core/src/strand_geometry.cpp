// SPDX-License-Identifier: Apache-2.0
#include "groomlab/strand_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>

#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

std::vector<std::size_t> thinStrandsById(const StrandSet& strands, std::size_t maxCount) {
    const std::size_t n = strands.strandCount();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (n <= maxCount) return order;
    const std::vector<StrandId>& ids = strands.ids();
    // Lowest hashed IDs first (ties: the earlier strand), as keepAtDensity keeps them; each
    // ID hashed once, not at every comparison.
    std::vector<std::uint64_t> keys(n);
    for (std::size_t i = 0; i < n; ++i) keys[i] = hash64(ids[i]);
    auto before = [&](std::size_t a, std::size_t b) { return keys[a] != keys[b] ? keys[a] < keys[b] : a < b; };
    std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(maxCount), order.end(), before);
    order.resize(maxCount);
    std::sort(order.begin(), order.end());
    return order;
}

namespace {

constexpr float kTwoPi = 6.28318531f;  // feathers.cpp's: the points come out as buildRachisMesh makes them

// Strand k's rings (one per point; two points or more) into `out`: rotation-minimizing frames from
// the scalp's normal at its root (`scalp`: the strands are rooted on it; null: any direction across
// the root), the widths times widthScale, u by arc length.
void strandRings(const StrandSet& strands, std::size_t k, const ScalpMesh* scalp, const StrandMeshOptions& options,
                 std::vector<Frame>& frames, std::vector<float>& arc, RachisRing* out) {
    const std::size_t count = strands.strandPointCount(k);
    const Vec3f* p = strands.strandPoints(k);
    const std::size_t begin = strands.strandBegin(k);
    const std::vector<float>& widths = strands.widths();
    Vec3f up = anyPerpendicular((p[1] - p[0]).normalized());
    if (scalp) {
        const StrandRoot& root = strands.roots()[k];
        if (root.face >= 0 && static_cast<std::size_t>(root.face) < scalp->triangleCount()) {
            const Vec3f normal = scalp->normal(root);
            if (normal.length() > 0.5f) up = normal;
        }
    }
    rotationMinimizingFramesInto(p, count, up, frames);
    arc.assign(count, 0.0f);
    for (std::size_t i = 1; i < count; ++i) arc[i] = arc[i - 1] + (p[i] - p[i - 1]).length();
    const float total = arc.back() > 0.0f ? arc.back() : 1.0f;
    for (std::size_t i = 0; i < count; ++i) {
        RachisRing& ring = out[i];
        ring.center = p[i];
        ring.tangent = frames[i].tangent;
        ring.normal = frames[i].normal;
        ring.side = frames[i].binormal;
        const float w = begin + i < widths.size() ? widths[begin + i] : 1.0f;
        ring.radius = 0.5f * std::max(w * options.widthScale, options.minWidth);
        ring.u = arc[i] / total;
    }
}

int meshSides(const StrandMeshOptions& options) {
    return options.style == StrandMeshOptions::Style::Ribbon ? 2 : std::max(3, options.sides);
}

// The vertices a strand of `count` points makes: a ring at each point, and a tube's tip (none for
// fewer than two points).
std::size_t strandVertexCount(std::size_t count, int sides) {
    if (count < 2) return 0;
    return count * static_cast<std::size_t>(sides) + (sides == 2 ? 0 : 1);
}

const ScalpMesh* rootingScalp(const StrandSet& strands, const ScalpMesh* scalp) {
    return scalp && scalp->triangleCount() > 0 && strands.roots().size() == strands.strandCount() ? scalp : nullptr;
}

}  // namespace

namespace {

// Tubes or ribbons round every strand (the Strands make, and the Feathers make's barbs).
StrandMesh strandTubes(const StrandSet& strands, const ScalpMesh* scalp, const StrandMeshOptions& options) {
    auto layer = std::make_shared<FeatherLayer>();
    layer->sides = meshSides(options);
    const std::size_t n = strands.strandCount();
    layer->offsets.reserve(n + 1);
    layer->featherIds.reserve(n);
    layer->rings.reserve(strands.pointCount());
    const ScalpMesh* rooted = rootingScalp(strands, scalp);
    std::vector<Frame> frames;
    std::vector<float> arc;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t count = strands.strandPointCount(k);
        if (count >= 2) {
            const std::size_t at = layer->rings.size();
            layer->rings.resize(at + count);
            strandRings(strands, k, rooted, options, frames, arc, layer->rings.data() + at);
        }
        layer->offsets.push_back(layer->rings.size());
        layer->featherIds.push_back(static_cast<int>(k));
    }

    StrandMesh out;
    out.mesh = buildRachisMesh({layer});
    if (options.strandUVs && n > 0) {
        const auto grid = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
        const float cell = 1.0f / static_cast<float>(grid);
        constexpr float kMargin = 0.05f;  // of a cell, on each side
        out.u2.assign(out.mesh.u.size(), 0.0f);
        out.v2.assign(out.mesh.v.size(), 0.0f);
        std::size_t corner = 0;
        for (std::size_t f = 0; f < out.mesh.faceCounts.size(); ++f) {
            const auto strand = static_cast<std::size_t>(out.mesh.faceFeather[f]);
            const float cu = static_cast<float>(strand % grid) * cell, cv = static_cast<float>(strand / grid) * cell;
            for (int c = 0; c < out.mesh.faceCounts[f]; ++c, ++corner) {
                const auto uv = static_cast<std::size_t>(out.mesh.faceUVs[corner]);
                out.u2[uv] = cu + cell * (kMargin + (1.0f - 2.0f * kMargin) * std::clamp(out.mesh.u[uv], 0.0f, 1.0f));
                out.v2[uv] = cv + cell * (kMargin + (1.0f - 2.0f * kMargin) * std::clamp(out.mesh.v[uv], 0.0f, 1.0f));
            }
        }
    }
    out.faceParts.assign(out.mesh.faceCounts.size(), static_cast<int>(MeshPart::Strands));
    return out;
}

// Triangles a and b (three corners each) as one quad when b is the other half of a quad split
// along a diagonal: they share an edge, wound opposite ways (a's x->y is b's y->x), and the
// quad is convex (every corner turns the same way about the pair's normal). The built-in plates
// (scalePlate) and a host's triangulated quads come as such pairs, one after the other.
bool quadOf(const std::uint32_t* a, const std::uint32_t* b, const std::vector<Vec3f>& points, std::array<std::uint32_t, 4>& quad) {
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t x = a[i], y = a[(i + 1) % 3];
        for (int j = 0; j < 3; ++j) {
            if (b[j] != y || b[(j + 1) % 3] != x) continue;
            const std::uint32_t other = b[(j + 2) % 3];
            if (other == x || other == y || other == a[(i + 2) % 3]) return false;
            quad = {x, other, y, a[(i + 2) % 3]};
            // (Corners relative to the first: a small scale far from the origin keeps its precision.)
            std::array<Vec3f, 4> q;
            for (int k = 0; k < 4; ++k) q[static_cast<std::size_t>(k)] = points[quad[static_cast<std::size_t>(k)]] - points[quad[0]];
            const Vec3f normal = (q[2] - q[0]).cross(q[3] - q[1]);  // the diagonals': the quad's normal
            if (normal.length() <= 0.0f) return false;
            for (std::size_t k = 0; k < 4; ++k) {
                const Vec3f in = q[k] - q[(k + 3) % 4], out = q[(k + 1) % 4] - q[k];
                if (in.cross(out).dot(normal) <= 0.0f) return false;
            }
            return true;
        }
    }
    return false;
}

// The scales' plates as one mesh: quads where two triangles in a row make one (quadOf), else
// triangles; each vertex its own uv.
StrandMesh scalePlates(const StrandSet& strands, const ScalpMesh* scalp) {
    StrandMesh out;
    // (buildScaleMesh keeps no pointer: the scalp only while it builds.)
    const std::shared_ptr<const ScalpMesh> skin(std::shared_ptr<const ScalpMesh>(), scalp);
    const std::shared_ptr<const ScaleMesh> scales = buildScaleMesh(strands, skin);
    if (!scales) return out;
    RachisMesh& m = out.mesh;
    m.points = scales->points;
    m.u.resize(scales->points.size());
    m.v.resize(scales->points.size());
    for (std::size_t i = 0; i < scales->uvs.size() && i < m.u.size(); ++i) {
        m.u[i] = scales->uvs[i][0];
        m.v[i] = scales->uvs[i][1];
    }
    m.featherIndex.assign(m.points.size(), 0.0f);
    m.groupIds.assign(m.points.size(), 0.0f);
    m.faceVertices.reserve(scales->triangles.size());
    const std::uint32_t* tri = scales->triangles.data();
    for (std::size_t s = 0; s < scales->scales.size(); ++s) {
        const ScaleMesh::Scale& scale = scales->scales[s];
        for (std::uint32_t v = 0; v < scale.vertexCount; ++v) m.featherIndex[scale.firstVertex + v] = static_cast<float>(s);
        // A scale's own triangles, paired into quads where they can be (never across scales).
        const std::size_t end = static_cast<std::size_t>(scale.firstTriangle) + scale.triangleCount;
        for (std::size_t t = scale.firstTriangle; t < end;) {
            std::array<std::uint32_t, 4> quad{};
            if (t + 1 < end && quadOf(tri + 3 * t, tri + 3 * (t + 1), m.points, quad)) {
                for (std::uint32_t v : quad) m.faceVertices.push_back(static_cast<int>(v));
                m.faceCounts.push_back(4);
                t += 2;
            } else {
                for (int c = 0; c < 3; ++c) m.faceVertices.push_back(static_cast<int>(tri[3 * t + static_cast<std::size_t>(c)]));
                m.faceCounts.push_back(3);
                t += 1;
            }
            m.faceFeather.push_back(static_cast<int>(s));
        }
    }
    m.faceUVs = m.faceVertices;
    m.featherCount = scales->scales.size();
    out.faceParts.assign(m.faceCounts.size(), static_cast<int>(MeshPart::Scales));
    return out;
}

// `extra` after `into` in one mesh (its points, uvs and faces renumbered).
void appendMesh(StrandMesh& into, const RachisMesh& extra, MeshPart part) {
    RachisMesh& m = into.mesh;
    const int points = static_cast<int>(m.points.size()), uvs = static_cast<int>(m.u.size());
    const int feathers = static_cast<int>(m.featherCount);
    m.points.insert(m.points.end(), extra.points.begin(), extra.points.end());
    m.u.insert(m.u.end(), extra.u.begin(), extra.u.end());
    m.v.insert(m.v.end(), extra.v.begin(), extra.v.end());
    if (!into.u2.empty()) {
        into.u2.resize(m.u.size(), 0.0f);
        into.v2.resize(m.v.size(), 0.0f);
    }
    for (float f : extra.featherIndex) m.featherIndex.push_back(f + static_cast<float>(feathers));
    m.groupIds.insert(m.groupIds.end(), extra.groupIds.begin(), extra.groupIds.end());
    m.faceCounts.insert(m.faceCounts.end(), extra.faceCounts.begin(), extra.faceCounts.end());
    for (int v : extra.faceVertices) m.faceVertices.push_back(v + points);
    for (int uv : extra.faceUVs) m.faceUVs.push_back(uv + uvs);
    for (int f : extra.faceFeather) m.faceFeather.push_back(f + feathers);
    m.featherCount += extra.featherCount;
    into.faceParts.resize(m.faceCounts.size(), static_cast<int>(part));
}

}  // namespace

StrandMeshOptions::Make resolveMeshMake(const StrandSet& strands, const StrandMeshOptions& options) {
    if (options.make != StrandMeshOptions::Make::Auto) return options.make;
    if (strands.instances() && strands.instances()->hasScales()) return StrandMeshOptions::Make::Scales;
    if (hasRachis(strands.featherLayers())) return StrandMeshOptions::Make::Feathers;
    return StrandMeshOptions::Make::Strands;
}

StrandMesh buildStrandMesh(const StrandSet& strands, const ScalpMesh* scalp, const StrandMeshOptions& options) {
    switch (resolveMeshMake(strands, options)) {
        case StrandMeshOptions::Make::Scales: return scalePlates(strands, scalp);
        case StrandMeshOptions::Make::Feathers: {
            StrandMesh out = strandTubes(strands, scalp, options);
            if (hasRachis(strands.featherLayers())) appendMesh(out, buildRachisMesh(strands.featherLayers()), MeshPart::Rachis);
            return out;
        }
        default: return strandTubes(strands, scalp, options);
    }
}

std::size_t strandMeshPointCount(const StrandSet& strands, const StrandMeshOptions& options) {
    if (resolveMeshMake(strands, options) != StrandMeshOptions::Make::Strands) {
        return buildStrandMesh(strands, nullptr, options).mesh.points.size();
    }
    const int sides = meshSides(options);
    std::size_t total = 0;
    for (std::size_t k = 0; k < strands.strandCount(); ++k) total += strandVertexCount(strands.strandPointCount(k), sides);
    return total;
}

void strandMeshPoints(const StrandSet& strands, const ScalpMesh* scalp, const StrandMeshOptions& options, Vec3f* out) {
    if (resolveMeshMake(strands, options) != StrandMeshOptions::Make::Strands) {
        const StrandMesh whole = buildStrandMesh(strands, scalp, options);
        std::copy(whole.mesh.points.begin(), whole.mesh.points.end(), out);
        return;
    }
    const std::size_t n = strands.strandCount();
    const int sides = meshSides(options);
    const ScalpMesh* rooted = rootingScalp(strands, scalp);
    std::vector<std::size_t> first(n + 1, 0);
    for (std::size_t k = 0; k < n; ++k) first[k + 1] = first[k] + strandVertexCount(strands.strandPointCount(k), sides);
    // The angles buildRachisMesh works out for a ring's vertices.
    std::vector<float> cosines, sines;
    for (int j = 0; sides != 2 && j < sides; ++j) {
        const float phi = kTwoPi * static_cast<float>(j) / static_cast<float>(sides);
        cosines.push_back(pm::cos(phi));
        sines.push_back(pm::sin(phi));
    }
    constexpr std::size_t kBlock = 64;  // strands a task takes, with scratch of its own
    parallelFor(
        0, (n + kBlock - 1) / kBlock,
        [&](std::size_t b) {
            std::vector<Frame> frames;
            std::vector<float> arc;
            std::vector<RachisRing> rings;
            for (std::size_t k = b * kBlock; k < std::min(n, (b + 1) * kBlock); ++k) {
                const std::size_t count = strands.strandPointCount(k);
                if (count < 2) continue;
                rings.resize(count);
                strandRings(strands, k, rooted, options, frames, arc, rings.data());
                Vec3f* o = out + first[k];
                for (const RachisRing& r : rings) {
                    if (sides == 2) {
                        *o++ = r.center + r.side * r.radius;
                        *o++ = r.center + r.side * -r.radius;
                        continue;
                    }
                    for (std::size_t j = 0; j < cosines.size(); ++j) *o++ = r.center + (r.side * cosines[j] + r.normal * sines[j]) * r.radius;
                }
                if (sides != 2) *o = rings.back().center;  // the tip's fan
            }
        },
        1);
}

std::uint64_t strandMeshTopologyKey(const StrandSet& strands, const StrandMeshOptions& options) {
    const StrandMeshOptions::Make make = resolveMeshMake(strands, options);
    if (make != StrandMeshOptions::Make::Strands) {
        // Scales and feathers: their topology is these strands' own (a new strand set, a new mesh).
        std::uint64_t key = hashCombine(hash64(0x70E5u), static_cast<std::uint64_t>(make));
        key = hashCombine(key, static_cast<std::uint64_t>(meshSides(options)));
        return hashCombine(key, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&strands)));
    }
    std::uint64_t key = hashCombine(hash64(0x57A4D5u), static_cast<std::uint64_t>(meshSides(options)));
    key = hashCombine(key, options.strandUVs ? 1u : 0u);
    key = hashCombine(key, strands.strandCount());
    const std::vector<StrandId>& ids = strands.ids();
    for (std::size_t k = 0; k < strands.strandCount(); ++k) {
        key = hashCombine(key, k < ids.size() ? static_cast<std::uint64_t>(ids[k]) : 0u);
        key = hashCombine(key, strands.strandPointCount(k));
    }
    return key;
}

bool sameStrandMeshes(const std::vector<StrandMeshSource>& a, const std::vector<StrandMeshSource>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const StrandMeshOptions &x = a[i].options, &y = b[i].options;
        if (a[i].node != b[i].node || a[i].strands != b[i].strands || x.make != y.make || x.style != y.style || x.sides != y.sides ||
            x.widthScale != y.widthScale || x.minWidth != y.minWidth || x.strandUVs != y.strandUVs) {
            return false;
        }
    }
    return true;
}

StrandMeshSettings strandMeshSettings(const ParamSet& params) {
    StrandMeshSettings s;
    const std::string make = params.getString("make", "auto");
    s.options.make = make == "strands"    ? StrandMeshOptions::Make::Strands
                     : make == "scales"   ? StrandMeshOptions::Make::Scales
                     : make == "feathers" ? StrandMeshOptions::Make::Feathers
                                          : StrandMeshOptions::Make::Auto;
    s.options.style = params.getString("style", "tube") == "ribbon" ? StrandMeshOptions::Style::Ribbon
                                                                    : StrandMeshOptions::Style::Tube;
    s.options.sides = static_cast<int>(params.getInt("sides", 4));
    s.options.widthScale = static_cast<float>(params.getDouble("widthScale", 1.0));
    s.options.strandUVs = params.getBool("strandUVs", false);
    s.density = std::clamp(static_cast<float>(params.getDouble("density", 1.0)), 0.0f, 1.0f);
    s.maxStrands = static_cast<std::size_t>(std::max<std::int64_t>(0, params.getInt("maxStrands", 0)));
    return s;
}

}  // namespace groomlab
