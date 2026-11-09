// SPDX-License-Identifier: Apache-2.0
#include "groomlab/follicle_map.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/png.h"
#include "groomlab/point_index.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

namespace {

constexpr int kBandRows = 32;  // rows a task bakes: a texel is written by one task only

// A skin triangle in its tile's texels: its corners' texel coordinates (x right, y down).
struct Footprint {
    std::size_t face = 0;
    float x[3], y[3];
    int x0 = 0, x1 = -1, y0 = 0, y1 = -1;  // the texels it may cover (inclusive)
};

struct TileWork {
    int udim = 1001;
    std::vector<Footprint> triangles;
    std::vector<std::vector<std::uint32_t>> bands;  // triangles touching each band of rows
    std::vector<std::uint8_t> filled;               // 1: a skin texel (baked), 0: empty
};

// The roots of `strands` on `skin`, as points on it.
std::vector<Vec3f> rootPoints(const ScalpMesh& skin, const StrandSet& strands) {
    std::vector<Vec3f> points;
    points.reserve(strands.strandCount());
    for (const StrandRoot& root : strands.roots()) {
        if (skin.contains(root)) points.push_back(skin.position(root));
    }
    return points;
}

float medianNearest(const PointIndex& index, const std::vector<Vec3f>& points) {
    if (points.size() < 2) return 0.0f;
    // At most 4096 roots, evenly through the list, each one's nearest other root.
    const std::size_t step = std::max<std::size_t>(1, points.size() / 4096);
    std::vector<float> nearest;
    for (std::size_t i = 0; i < points.size(); i += step) {
        std::size_t idx[2];
        float d2[2];
        if (index.kNearest(points[i], 2, idx, d2) < 2) continue;
        nearest.push_back(std::sqrt(d2[1]));
    }
    if (nearest.empty()) return 0.0f;
    std::nth_element(nearest.begin(), nearest.begin() + static_cast<std::ptrdiff_t>(nearest.size() / 2), nearest.end());
    return nearest[nearest.size() / 2];
}

}  // namespace

float follicleMapAutoRadius(const ScalpMesh& skin, const StrandSet& strands) {
    const std::vector<Vec3f> points = rootPoints(skin, strands);
    if (points.size() < 2) return 0.0f;
    return 0.4f * medianNearest(PointIndex(points), points);
}

FollicleMap bakeFollicleMap(const ScalpMesh& skin, const StrandSet& strands, const FollicleMapOptions& options) {
    if (!skin.hasUVs()) throw std::invalid_argument("the scalp has no uvs to bake the follicles onto");
    if (options.resolution < 16 || options.resolution > 8192) {
        throw std::invalid_argument("the follicle map's resolution must be 16 to 8192");
    }
    const std::vector<Vec3f> roots = rootPoints(skin, strands);
    if (roots.empty()) throw std::invalid_argument("no strand is rooted on the scalp: nothing to bake");
    const PointIndex index(roots);
    float radius = options.radius;
    if (!(radius > 0.0f)) radius = 0.4f * medianNearest(index, roots);
    if (!(radius > 0.0f)) {  // one root: a dot a thousandth of the scalp's size
        Vec3f lo = skin.points().empty() ? Vec3f{} : skin.points().front(), hi = lo;
        for (const Vec3f& p : skin.points()) {
            lo = Vec3f{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = Vec3f{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        radius = 1e-3f * (hi - lo).length();
    }
    if (!(radius > 0.0f)) radius = 1.0f;
    const float hardness = std::clamp(options.hardness, 0.0f, 1.0f);
    const int res = options.resolution;
    FollicleMap out;
    out.resolution = res;
    out.radius = radius;
    out.follicles = roots.size();

    // Each skin triangle to the tile under its middle; the tiles in UDIM order.
    const auto& uvs = skin.cornerUVs();
    std::map<int, TileWork> tiles;
    for (std::size_t f = 0; f < skin.triangleCount(); ++f) {
        const ScalpMesh::UV* c = &uvs[3 * f];
        const float mu = (c[0][0] + c[1][0] + c[2][0]) / 3.0f, mv = (c[0][1] + c[1][1] + c[2][1]) / 3.0f;
        const int tu = static_cast<int>(std::floor(mu)), tv = static_cast<int>(std::floor(mv));
        if (tu < 0 || tu > 9 || tv < 0 || tv > 9) continue;  // off the tile grid
        TileWork& tile = tiles[1001 + tu + 10 * tv];
        tile.udim = 1001 + tu + 10 * tv;
        Footprint fp;
        fp.face = f;
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int k = 0; k < 3; ++k) {
            fp.x[k] = (c[k][0] - static_cast<float>(tu)) * static_cast<float>(res);
            fp.y[k] = (1.0f - (c[k][1] - static_cast<float>(tv))) * static_cast<float>(res);
            lo[0] = std::min(lo[0], fp.x[k]);
            hi[0] = std::max(hi[0], fp.x[k]);
            lo[1] = std::min(lo[1], fp.y[k]);
            hi[1] = std::max(hi[1], fp.y[k]);
        }
        fp.x0 = std::max(0, static_cast<int>(std::floor(lo[0] - 0.5f)));
        fp.x1 = std::min(res - 1, static_cast<int>(std::ceil(hi[0] - 0.5f)));
        fp.y0 = std::max(0, static_cast<int>(std::floor(lo[1] - 0.5f)));
        fp.y1 = std::min(res - 1, static_cast<int>(std::ceil(hi[1] - 0.5f)));
        if (fp.x1 < fp.x0 || fp.y1 < fp.y0) continue;
        tile.triangles.push_back(fp);
    }
    out.udim = !tiles.empty() && (tiles.size() > 1 || tiles.begin()->first != 1001);

    const std::size_t texelCount = static_cast<std::size_t>(res) * static_cast<std::size_t>(res);
    const int bandCount = (res + kBandRows - 1) / kBandRows;
    std::vector<TileWork*> work;
    for (auto& [udim, tile] : tiles) {
        tile.bands.resize(static_cast<std::size_t>(bandCount));
        for (std::size_t t = 0; t < tile.triangles.size(); ++t) {
            const Footprint& fp = tile.triangles[t];
            for (int b = fp.y0 / kBandRows; b <= fp.y1 / kBandRows; ++b) tile.bands[static_cast<std::size_t>(b)].push_back(static_cast<std::uint32_t>(t));
        }
        tile.filled.assign(texelCount, 0);
        FollicleMap::Tile baked;
        baked.udim = udim;
        baked.value.assign(texelCount, 0);
        out.tiles.push_back(std::move(baked));
        work.push_back(&tile);
    }

    // Every skin texel: the point under it, the nearest root's gradient there.
    const float inner = hardness * radius;
    std::atomic<std::size_t> skinTexels{0}, touchedTexels{0};
    parallelFor(0, work.size() * static_cast<std::size_t>(bandCount), [&](std::size_t job) {
        const std::size_t ti = job / static_cast<std::size_t>(bandCount);
        const int band = static_cast<int>(job % static_cast<std::size_t>(bandCount));
        TileWork& tile = *work[ti];
        FollicleMap::Tile& baked = out.tiles[ti];
        const int rowLo = band * kBandRows, rowHi = std::min(res, rowLo + kBandRows) - 1;
        std::size_t skinCount = 0, touchedCount = 0;
        for (const std::uint32_t t : tile.bands[static_cast<std::size_t>(band)]) {
            const Footprint& fp = tile.triangles[t];
            const float area = (fp.x[1] - fp.x[0]) * (fp.y[2] - fp.y[0]) - (fp.x[2] - fp.x[0]) * (fp.y[1] - fp.y[0]);
            if (std::fabs(area) < 1e-12f) continue;
            for (int py = std::max(fp.y0, rowLo); py <= std::min(fp.y1, rowHi); ++py) {
                for (int px = fp.x0; px <= fp.x1; ++px) {
                    const float cx = static_cast<float>(px) + 0.5f, cy = static_cast<float>(py) + 0.5f;
                    // Barycentrics of the texel's center (StrandRoot's u at corner 1, v at corner 2).
                    const float w1 = ((cx - fp.x[0]) * (fp.y[2] - fp.y[0]) - (fp.x[2] - fp.x[0]) * (cy - fp.y[0])) / area;
                    const float w2 = ((fp.x[1] - fp.x[0]) * (cy - fp.y[0]) - (cx - fp.x[0]) * (fp.y[1] - fp.y[0])) / area;
                    constexpr float kEdge = -1e-5f;
                    if (w1 < kEdge || w2 < kEdge || 1.0f - w1 - w2 < kEdge) continue;
                    const std::size_t i = static_cast<std::size_t>(py) * static_cast<std::size_t>(res) + static_cast<std::size_t>(px);
                    if (tile.filled[i]) continue;  // (a texel on two triangles' shared edge: baked once)
                    tile.filled[i] = 1;
                    ++skinCount;
                    const StrandRoot at{static_cast<std::int32_t>(fp.face), std::clamp(w1, 0.0f, 1.0f), std::clamp(w2, 0.0f, 1.0f)};
                    float d2 = 0.0f;
                    index.nearest(skin.position(at), &d2);
                    const float d = std::sqrt(d2);
                    if (!(d < radius)) continue;
                    float v = 1.0f;
                    if (d > inner) {
                        const float t = 1.0f - (d - inner) / std::max(radius - inner, 1e-12f);  // 1 at inner, 0 at the radius
                        v = t * t * (3.0f - 2.0f * t);
                    }
                    baked.value[i] = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
                    if (baked.value[i] > 0) ++touchedCount;
                }
            }
        }
        skinTexels += skinCount;
        touchedTexels += touchedCount;
    }, 1);
    out.texels = skinTexels.load();
    out.touched = touchedTexels.load();

    // Padding: empty texels next to baked ones take a neighbor's value, a ring per pass.
    for (std::size_t ti = 0; ti < work.size(); ++ti) {
        std::vector<std::uint8_t>& filled = work[ti]->filled;
        std::vector<std::uint16_t>& value = out.tiles[ti].value;
        const std::size_t n = static_cast<std::size_t>(res);
        for (int pass = 0; pass < options.padding; ++pass) {
            std::vector<std::vector<std::pair<std::size_t, std::size_t>>> rows(n);
            parallelFor(0, n, [&](std::size_t y) {
                for (std::size_t x = 0; x < n; ++x) {
                    const std::size_t i = y * n + x;
                    if (filled[i]) continue;
                    std::size_t from = i;
                    if (x > 0 && filled[i - 1] == 1) from = i - 1;
                    else if (x + 1 < n && filled[i + 1] == 1) from = i + 1;
                    else if (y > 0 && filled[i - n] == 1) from = i - n;
                    else if (y + 1 < n && filled[i + n] == 1) from = i + n;
                    if (from != i) rows[y].emplace_back(i, from);
                }
            }, 16);
            bool grew = false;
            for (const auto& row : rows) {
                for (const auto& [to, from] : row) {
                    value[to] = value[from];
                    filled[to] = 2;  // (not a source until the next pass)
                    grew = true;
                }
            }
            if (!grew) break;
            for (std::uint8_t& f : filled) f = f ? 1 : 0;
        }
    }
    return out;
}

std::string follicleMapPath(const std::string& prefix, bool udim) {
    return prefix + kFollicleMapName + (udim ? ".<UDIM>" : "") + ".png";
}

std::vector<std::string> writeFollicleMap(const FollicleMap& map, const std::string& prefix) {
    std::vector<std::string> written;
    for (const FollicleMap::Tile& tile : map.tiles) {
        const std::string path = prefix + kFollicleMapName + (map.udim ? "." + std::to_string(tile.udim) : std::string()) + ".png";
        writeCompressedPng16(path, map.resolution, map.resolution, 1, tile.value);
        written.push_back(path);
    }
    return written;
}

}  // namespace groomlab
