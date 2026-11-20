// SPDX-License-Identifier: Apache-2.0
#include "groomlab/feathers.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>

#include "groomlab/strand_set.h"  // groupNumberBase

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.28318531f;

// Vertex j of ring r (j may be `sides`: vertex 0 again).
Vec3f ringVertex(const RachisRing& r, int j, int sides) {
    if (sides == 2) return r.center + r.side * (j % 2 == 0 ? r.radius : -r.radius);
    const float phi = kTwoPi * static_cast<float>(j) / static_cast<float>(sides);
    return r.center + (r.side * pm::cos(phi) + r.normal * pm::sin(phi)) * r.radius;
}

// The cosine and sine of vertex j's angle (j in 0..sides), the same values ringVertex works out.
struct RingAngles {
    std::vector<float> cosines, sines;
    explicit RingAngles(int sides) {
        if (sides == 2) return;
        for (int j = 0; j <= sides; ++j) {
            const float phi = kTwoPi * static_cast<float>(j) / static_cast<float>(sides);
            cosines.push_back(pm::cos(phi));
            sines.push_back(pm::sin(phi));
        }
    }
};

// ringVertex with the angles looked up.
Vec3f ringVertex(const RachisRing& r, int j, int sides, const RingAngles& angles) {
    if (sides == 2) return r.center + r.side * (j % 2 == 0 ? r.radius : -r.radius);
    const auto k = static_cast<std::size_t>(j);
    return r.center + (r.side * angles.cosines[k] + r.normal * angles.sines[k]) * r.radius;
}

// Where a ray at `angle` crosses the ring's polygon: the edge (j, j + 1) and how far along it.
void ringEdge(float angle, int sides, int* j, float* g) {
    if (sides == 2) {  // a ribbon: across it, +side (vertex 0) to -side (vertex 1)
        *j = 0;
        *g = 0.5f * (1.0f - pm::cos(angle));
        return;
    }
    const float step = kTwoPi / static_cast<float>(sides);
    float a = std::fmod(angle, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    int k = static_cast<int>(std::floor(a / step));
    k = std::clamp(k, 0, sides - 1);
    const float alpha = a - static_cast<float>(k) * step;
    const float beta = step - alpha;
    const float sa = pm::sin(alpha), sb = pm::sin(beta);
    *j = k;
    *g = sa + sb > 0.0f ? sa / (sa + sb) : 0.0f;  // the law of sines in the triangle center, v_j, v_j+1
}

}  // namespace

Vec3f rachisSurfacePoint(const RachisRing& a, const RachisRing& b, float f, float angle, int sides) {
    sides = std::max(2, sides);
    int j = 0;
    float g = 0.0f;
    ringEdge(angle, sides, &j, &g);
    const Vec3f pa = ringVertex(a, j, sides) * (1.0f - g) + ringVertex(a, j + 1, sides) * g;
    const Vec3f pb = ringVertex(b, j, sides) * (1.0f - g) + ringVertex(b, j + 1, sides) * g;
    return pa * (1.0f - f) + pb * f;
}

std::size_t RachisMesh::triangleCount() const {
    std::size_t n = 0;
    for (const int c : faceCounts) n += static_cast<std::size_t>(std::max(0, c - 2));
    return n;
}

RachisMesh buildRachisMesh(const std::vector<FeatherLayerPtr>& layers) {
    RachisMesh mesh;
    // The sizes first, so every array grows once.
    std::size_t points = 0, uvs = 0, faces = 0, corners = 0;
    for (const FeatherLayerPtr& layer : layers) {
        if (!layer || !layer->hasRachis()) continue;
        const auto sides = static_cast<std::size_t>(std::max(2, layer->sides));
        const bool ribbon = sides == 2;
        for (std::size_t k = 0; k < layer->shaftCount(); ++k) {
            const std::size_t r0 = layer->offsets[k], r1 = layer->offsets[k + 1];
            if (r1 < r0 + 2) continue;
            const std::size_t rings = r1 - r0, quads = ribbon ? 1 : sides;
            points += rings * (ribbon ? 2 : sides) + (ribbon ? 0 : 1);
            uvs += rings * (ribbon ? 2 : sides + 1) + (ribbon ? 0 : 1);
            faces += (rings - 1) * quads + (ribbon ? 0 : sides);
            corners += 4 * (rings - 1) * quads + (ribbon ? 0 : 3 * sides);
        }
    }
    mesh.points.reserve(points);
    mesh.featherIndex.reserve(points);
    mesh.groupIds.reserve(points);
    mesh.u.reserve(uvs);
    mesh.v.reserve(uvs);
    mesh.faceCounts.reserve(faces);
    mesh.faceFeather.reserve(faces);
    mesh.faceVertices.reserve(corners);
    mesh.faceUVs.reserve(corners);
    int featherBase = 0;
    for (const FeatherLayerPtr& layer : layers) {
        if (!layer || !layer->hasRachis()) continue;
        const int sides = std::max(2, layer->sides);
        const bool ribbon = sides == 2;
        const RingAngles angles(sides);
        int highest = -1;
        for (std::size_t k = 0; k < layer->shaftCount(); ++k) {
            const std::size_t r0 = layer->offsets[k], r1 = layer->offsets[k + 1];
            if (r1 < r0 + 2) continue;  // a tube needs two rings
            ++mesh.featherCount;
            const int feather = featherBase + std::max(0, layer->featherIds[k]);
            highest = std::max(highest, layer->featherIds[k]);
            const auto group = static_cast<float>(layer->groupId);
            const int firstPoint = static_cast<int>(mesh.points.size());
            const int firstUV = static_cast<int>(mesh.u.size());
            const int columns = ribbon ? 2 : sides + 1;  // UV columns: a seam column of its own
            for (std::size_t r = r0; r < r1; ++r) {
                const RachisRing& ring = layer->rings[r];
                for (int j = 0; j < (ribbon ? 2 : sides); ++j) {
                    mesh.points.push_back(ringVertex(ring, j, sides, angles));
                    mesh.featherIndex.push_back(static_cast<float>(feather));
                    mesh.groupIds.push_back(group);
                }
                for (int j = 0; j < columns; ++j) {
                    mesh.u.push_back(ribbon ? static_cast<float>(j) : static_cast<float>(j) / static_cast<float>(sides));
                    mesh.v.push_back(ring.u);
                }
            }
            const int perRing = ribbon ? 2 : sides;
            const int rings = static_cast<int>(r1 - r0);
            auto vertex = [&](int ring, int j) { return firstPoint + ring * perRing + (j % perRing); };
            auto uv = [&](int ring, int j) { return firstUV + ring * columns + j; };
            for (int r = 0; r + 1 < rings; ++r) {
                // Outward: (r, j), (r + 1, j), (r + 1, j + 1), (r, j + 1) (the ribbon faces the vane normal).
                const int quads = ribbon ? 1 : sides;
                for (int j = 0; j < quads; ++j) {
                    mesh.faceCounts.push_back(4);
                    for (const auto& [rr, jj] : {std::pair{r, j}, std::pair{r + 1, j}, std::pair{r + 1, j + 1},
                                                  std::pair{r, j + 1}}) {
                        mesh.faceVertices.push_back(vertex(rr, jj));
                        mesh.faceUVs.push_back(uv(rr, jj));
                    }
                    mesh.faceFeather.push_back(feather);
                }
            }
            if (!ribbon) {  // the tip closed by a fan round its center
                const RachisRing& tip = layer->rings[r1 - 1];
                const int apex = static_cast<int>(mesh.points.size());
                mesh.points.push_back(tip.center);
                mesh.featherIndex.push_back(static_cast<float>(feather));
                mesh.groupIds.push_back(group);
                const int apexUV = static_cast<int>(mesh.u.size());
                mesh.u.push_back(0.5f);
                mesh.v.push_back(tip.u);
                for (int j = 0; j < sides; ++j) {
                    mesh.faceCounts.push_back(3);
                    mesh.faceVertices.push_back(vertex(rings - 1, j));
                    mesh.faceVertices.push_back(apex);
                    mesh.faceVertices.push_back(vertex(rings - 1, j + 1));
                    mesh.faceUVs.push_back(uv(rings - 1, j));
                    mesh.faceUVs.push_back(apexUV);
                    mesh.faceUVs.push_back(uv(rings - 1, j + 1));
                    mesh.faceFeather.push_back(feather);
                }
            }
        }
        featherBase += static_cast<int>(groupNumberBase(highest));
    }
    return mesh;
}

bool hasRachis(const std::vector<FeatherLayerPtr>& layers) {
    return std::any_of(layers.begin(), layers.end(), [](const FeatherLayerPtr& l) { return l && l->hasRachis(); });
}

std::map<int, std::string> featherGroupNames(const std::vector<FeatherLayerPtr>& layers) {
    std::map<int, std::string> names;
    for (const FeatherLayerPtr& l : layers) {
        if (l && l->groupId > 0 && !l->groupName.empty()) names.emplace(l->groupId, l->groupName);
    }
    return names;
}

bool hasShafts(const StrandSet& strands) {
    const std::vector<float>* shafts = strands.strandChannel("isShaft");
    if (!shafts || shafts->size() != strands.strandCount()) return false;
    return std::any_of(shafts->begin(), shafts->end(), [](float v) { return v > 0.5f; });
}

void holdShafts(const StrandSet& in, StrandSet& out) {
    const std::vector<float>* shafts = in.strandChannel("isShaft");
    if (!shafts || shafts->size() != in.strandCount() || out.strandCount() != in.strandCount()) return;
    for (std::size_t s = 0; s < in.strandCount(); ++s) {
        if ((*shafts)[s] <= 0.5f || out.strandPointCount(s) != in.strandPointCount(s)) continue;
        std::copy_n(in.strandPoints(s), in.strandPointCount(s), out.strandPoints(s));
    }
}

}  // namespace groomlab
