// SPDX-License-Identifier: Apache-2.0
#include "texel_padding.h"
#include "groomlab/portable_math.h"
#include "groomlab/scale_maps.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/png.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

namespace {

constexpr int kBandRows = 32;  // rows a task bakes: a texel is written by one task only

float linearToSrgb(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * pm::pow(x, 1.0f / 2.4f) - 0.055f;
}

std::uint8_t to8(float x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); }

// A skin triangle in its tile's texels: its corners' texel coordinates (x right, y down) and the
// tangent frame its uvs give.
struct Footprint {
    std::size_t face = 0;
    float x[3], y[3];
    int x0 = 0, x1 = -1, y0 = 0, y1 = -1;  // the texels it may cover (inclusive)
    Vec3f dpdu, dpdv;
};

struct TileWork {
    int udim = 1001;
    std::vector<Footprint> triangles;
    std::vector<std::vector<std::uint32_t>> bands;  // triangles touching each band of rows
    std::vector<std::uint8_t> filled;               // 1: a skin texel (baked), 0: empty
};

}  // namespace

float scalesMaxHeight(const ScalpMesh& skin, const ScaleMesh& scales) {
    const std::size_t n = scales.points.size();
    if (n == 0 || skin.triangleCount() == 0) return 0.0f;
    constexpr std::size_t kChunk = 4096;
    std::vector<float> highest((n + kChunk - 1) / kChunk, 0.0f);
    parallelFor(0, highest.size(), [&](std::size_t c) {
        float h = 0.0f;
        for (std::size_t i = c * kChunk; i < std::min(n, (c + 1) * kChunk); ++i) {
            const SurfacePoint sp = skin.closestPoint(scales.points[i]);
            if (sp.root.face >= 0) h = std::max(h, (scales.points[i] - sp.position).dot(sp.normal));
        }
        highest[c] = h;
    }, 1);
    return *std::max_element(highest.begin(), highest.end());
}

ScaleMaps bakeScaleMaps(const ScalpMesh& skin, const ScaleMesh& scales, const ScaleMapOptions& options) {
    if (!skin.hasUVs()) throw std::invalid_argument("the skin has no uvs to bake the scales onto");
    if (options.resolution < 16 || options.resolution > 8192) {
        throw std::invalid_argument("the scale maps' resolution must be 16 to 8192");
    }
    const int res = options.resolution;
    ScaleMaps out;
    out.resolution = res;
    out.maxHeight = options.maxHeight > 0.0f ? options.maxHeight : scalesMaxHeight(skin, scales);
    if (!(out.maxHeight > 0.0f)) out.maxHeight = 1.0f;  // no scales: a flat map of any height
    const float maxHeight = out.maxHeight;
    // The scales' triangles as a mesh with a ray tree (the scalp's), cast from above the skin.
    std::vector<ScalpMesh::Triangle> triangles(scales.triangleCount());
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        triangles[t] = {static_cast<std::int32_t>(scales.triangles[3 * t]), static_cast<std::int32_t>(scales.triangles[3 * t + 1]),
                        static_cast<std::int32_t>(scales.triangles[3 * t + 2])};
    }
    const ScalpMesh plates(scales.points, std::move(triangles));

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
        // dP/du and dP/dv from the triangle's edges in uv.
        const auto& tri = skin.triangles()[f];
        const Vec3f& a = skin.points()[static_cast<std::size_t>(tri[0])];
        const Vec3f e1 = skin.points()[static_cast<std::size_t>(tri[1])] - a, e2 = skin.points()[static_cast<std::size_t>(tri[2])] - a;
        const float du1 = c[1][0] - c[0][0], dv1 = c[1][1] - c[0][1], du2 = c[2][0] - c[0][0], dv2 = c[2][1] - c[0][1];
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-20f) continue;  // no area in uv: nothing to bake into
        fp.dpdu = (e1 * dv2 - e2 * dv1) / det;
        fp.dpdv = (e2 * du1 - e1 * du2) / det;
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
        ScaleMaps::Tile baked;
        baked.udim = udim;
        baked.normal.resize(texelCount * 3);
        baked.height.assign(texelCount, 0);
        baked.color.resize(texelCount * 4);
        for (std::size_t i = 0; i < texelCount; ++i) {
            baked.normal[3 * i] = 128;
            baked.normal[3 * i + 1] = 128;
            baked.normal[3 * i + 2] = 255;
            for (int k = 0; k < 3; ++k) baked.color[4 * i + static_cast<std::size_t>(k)] = to8(linearToSrgb(options.background[static_cast<std::size_t>(k)]));
            baked.color[4 * i + 3] = 0;
        }
        out.tiles.push_back(std::move(baked));
        work.push_back(&tile);
    }

    // Every skin texel cast down onto the scales, a band of rows at a time.
    const float above = maxHeight * 1.01f + 1e-5f;  // the ray starts this far above the skin
    std::atomic<std::size_t> skinTexels{0}, coveredTexels{0};
    parallelFor(0, work.size() * static_cast<std::size_t>(bandCount), [&](std::size_t job) {
        const std::size_t ti = job / static_cast<std::size_t>(bandCount);
        const int band = static_cast<int>(job % static_cast<std::size_t>(bandCount));
        TileWork& tile = *work[ti];
        ScaleMaps::Tile& baked = out.tiles[ti];
        const int rowLo = band * kBandRows, rowHi = std::min(res, rowLo + kBandRows) - 1;
        std::size_t skinCount = 0, coveredCount = 0;
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
                    if (!tile.filled[i]) ++skinCount;
                    tile.filled[i] = 1;
                    const StrandRoot root{static_cast<std::int32_t>(fp.face), std::clamp(w1, 0.0f, 1.0f), std::clamp(w2, 0.0f, 1.0f)};
                    const Vec3f p = skin.position(root);
                    Vec3f n = skin.normal(root);
                    if (n.length() < 1e-12f) continue;
                    n = n.normalized();
                    SurfacePoint hit;
                    float distance = 0.0f;
                    if (!plates.intersectRay(p + n * above, n * -1.0f, &hit, &distance)) continue;
                    const float height = above - distance;
                    if (height < -1e-4f * maxHeight) continue;  // under the skin: another part of the body
                    // The scale's own normal and color there, its triangle's corners blended.
                    const auto& tri = plates.triangles()[static_cast<std::size_t>(hit.root.face)];
                    const float b0 = 1.0f - hit.root.u - hit.root.v;
                    const auto corner = [&](int k) { return static_cast<std::size_t>(tri[static_cast<std::size_t>(k)]); };
                    Vec3f sn = scales.normals[corner(0)] * b0 + scales.normals[corner(1)] * hit.root.u + scales.normals[corner(2)] * hit.root.v;
                    sn = sn.length() > 1e-12f ? sn.normalized() : n;
                    if (sn.dot(n) < 0.0f) sn = sn * -1.0f;  // (seen from above: facing out)
                    Vec3f tangent = fp.dpdu - n * n.dot(fp.dpdu);
                    tangent = tangent.length() > 1e-20f ? tangent.normalized() : Vec3f{1, 0, 0};
                    Vec3f bitangent = n.cross(tangent);
                    if (bitangent.dot(fp.dpdv) < 0.0f) bitangent = bitangent * -1.0f;  // (a mirrored uv shell)
                    baked.normal[3 * i] = to8(0.5f + 0.5f * sn.dot(tangent));
                    baked.normal[3 * i + 1] = to8(0.5f + 0.5f * sn.dot(bitangent));
                    baked.normal[3 * i + 2] = to8(0.5f + 0.5f * sn.dot(n));
                    baked.height[i] = static_cast<std::uint16_t>(std::lround(std::clamp(height / maxHeight, 0.0f, 1.0f) * 65535.0f));
                    for (int k = 0; k < 3; ++k) {
                        const std::size_t ch = static_cast<std::size_t>(k);
                        const float c = scales.colors[corner(0)][ch] * b0 + scales.colors[corner(1)][ch] * hit.root.u +
                                        scales.colors[corner(2)][ch] * hit.root.v;
                        baked.color[4 * i + ch] = to8(linearToSrgb(c));
                    }
                    baked.color[4 * i + 3] = 255;
                    ++coveredCount;
                }
            }
        }
        skinTexels += skinCount;
        coveredTexels += coveredCount;
    }, 1);
    out.texels = skinTexels.load();
    out.covered = coveredTexels.load();

    // Padding: empty texels next to baked ones take a neighbor's values, a ring per pass.
    for (std::size_t ti = 0; ti < work.size(); ++ti) {
        std::vector<std::uint8_t>& filled = work[ti]->filled;
        ScaleMaps::Tile& baked = out.tiles[ti];
        padByNeighbors(filled, static_cast<std::size_t>(res), options.padding, [&](std::size_t to, std::size_t from) {
            std::copy_n(baked.normal.begin() + static_cast<std::ptrdiff_t>(3 * from), 3, baked.normal.begin() + static_cast<std::ptrdiff_t>(3 * to));
            std::copy_n(baked.color.begin() + static_cast<std::ptrdiff_t>(4 * from), 4, baked.color.begin() + static_cast<std::ptrdiff_t>(4 * to));
            baked.height[to] = baked.height[from];
        });
    }
    return out;
}

std::string scaleMapPath(const std::string& prefix, const std::string& map, bool udim) {
    return prefix + map + (udim ? ".<UDIM>" : "") + ".png";
}

std::vector<std::string> writeScaleMaps(const ScaleMaps& maps, const std::string& prefix) {
    std::vector<std::string> written;
    const int res = maps.resolution;
    for (const ScaleMaps::Tile& tile : maps.tiles) {
        const std::string tag = maps.udim ? "." + std::to_string(tile.udim) : std::string();
        std::vector<std::string> paths;
        for (const char* map : kScaleMapNames) paths.push_back(prefix + map + tag + ".png");
        writeCompressedPngs({paths[0], paths[2]}, {PngImage{res, res, 3, &tile.normal}, PngImage{res, res, 4, &tile.color}});
        writeCompressedPng16(paths[1], res, res, 1, tile.height);
        written.insert(written.end(), paths.begin(), paths.end());
    }
    return written;
}

}  // namespace groomlab
