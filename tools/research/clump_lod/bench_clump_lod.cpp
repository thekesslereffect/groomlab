// SPDX-License-Identifier: Apache-2.0
// Research prototype (docs/research/clump_lod.md): screen-space level of detail for strands with
// analytic clump coverage, against the viewport's hashed width-as-coverage lines, on a CPU raster
// harness that draws exactly what the viewport's line shaders do (display.h's reference functions)
// and a physical reference (opaque ribbons of the true width, 64 stratified samples a pixel).
//
//   research_clump_lod [--strands 1000000] [--size 512] [--out dir] [--methods a,b] [--sphere] [--tiny] [--check]
//
// --methods draws and measures those alone; --sphere grows the groom on a radius-10 sphere without
// guides (the Maya GUI suite's lod_check); --tiny adds a fifth framing (the head 10 pixels across);
// --check is the picture test (ctest core.lod_pictures): the core's level of detail as the hosts
// draw it (coreSplit) within 2% of the reference's ink and its MAE no worse than current's, at
// every framing (exit code 1 otherwise).
//
// Methods, at near, mid and far framings of the hair_basics preset on the app's head:
//   reference   ribbons of the true projected width, box filtered (64 samples a pixel)
//   current     every strand, a pixel-wide line, coverage clamp(width px, 0.05, 1), hashed
//   unfloored   the same without the 0.05 floor (what the floor alone costs)
//   preview     the existing thinned preview at the LOD's strand budget (lineCoverageAtDensity)
//   clumpLod    per clump m of n strands, m from the clump's footprint and coverage, each
//               widened to 1 - (1 - c)^(n/m) from its true coverage
//   impostor    clumps whose members spread less than a pixel across the view: one strand along
//               the clump's axis, its coverage the Beer-Lambert ink of the members (others: clumpLod)
//   unflooredTwoPass  unfloored, the blended fragments in a second pass without depth writes
//   unflooredSplit    unfloored, the segments that may blend (exactly, at this view) drawn after
//               the rest without depth writes
//   coreLod, coreImpostor, coreSplit  the core's StrandLod (strand_lod.h) as the hosts draw it:
//               its classes (thinned strands hashed alone), with its impostors, and with class 0's
//               blendable segments' second pass (the hosts' drawing)

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>

#include "../common/research_common.h"
#include "groomlab/display.h"
#include "groomlab/png.h"
#include "groomlab/presets.h"
#include "groomlab/strand_lod.h"

using namespace research;

namespace {

struct Camera {
    Vec3f eye, right, up, fwd;
    float pixelScale = 1.0f;  // pixels a unit at depth 1
    float cx = 0.0f, cy = 0.0f;
    int W = 512, H = 512;
    // Screen x, y and depth; false behind the eye.
    bool project(const Vec3f& p, float& x, float& y, float& z) const {
        const Vec3f d = p - eye;
        z = d.dot(fwd);
        if (z <= 1e-4f) return false;
        x = cx + pixelScale * d.dot(right) / z;
        y = cy - pixelScale * d.dot(up) / z;
        return true;
    }
};

Camera makeCamera(const Vec3f& target, const Vec3f& dir, float distance, int size, float fovDeg, Vec3f shift = {}) {
    Camera c;
    c.W = c.H = size;
    c.fwd = (dir * -1.0f).normalized();
    c.eye = target + dir.normalized() * distance + shift;
    c.right = c.fwd.cross(Vec3f(0, 1, 0)).normalized();
    c.up = c.right.cross(c.fwd).normalized();
    c.pixelScale = 0.5f * float(size) / std::tan(0.5f * fovDeg * 3.14159265f / 180.0f);
    c.cx = 0.5f * float(size);
    c.cy = 0.5f * float(size);
    return c;
}

// A strand as drawn: its points on screen, per point the coverage the line takes and its "own"
// (lineKeepShare), its line number (hash pattern) and color.
struct DrawStrand {
    std::uint32_t first = 0, count = 0;  // into the point arrays
    std::uint32_t lineId = 0;
    Vec3f rootColor, tipColor;
};
struct DrawList {
    std::vector<DrawStrand> strands;
    std::vector<float> x, y, z, u, cov, own;
    // Impostor ribbons: per point the Beer-Lambert profile (members, fiber width px, spread sigma px);
    // empty for a line.
    std::vector<float> members, fiberPx, sigmaPx;
    std::vector<char> ribbon;  // per strand: 1 a wide impostor ribbon
    // Per point, the segment from it may blend (its own coverage between kLineHashedBelow and 1 at
    // either end): drawn in the second draw of the split mode, depth not written. Empty: none.
    std::vector<char> blendSeg;
    std::size_t segments() const {
        std::size_t s = 0;
        for (const auto& d : strands) s += d.count > 1 ? d.count - 1 : 0;
        return s;
    }
};

std::uint32_t packId(StrandId id) { return lineIdOf(id); }

float hash01(std::uint64_t v) { return float(hash64(v) >> 40) / float(1 << 24); }

// The coverage floor the viewport drew with before 2026-10-09 (display.h kLineCoverageFloor is 0 now).
constexpr float kOldFloor = 0.05f;

// ---- Rasterizers (tiles of 32 x 32, strands in order inside a tile) ------------------------------

constexpr int kTile = 32;

struct Image {
    int W = 0, H = 0;
    std::vector<Vec3f> rgb;  // composited over white
    std::vector<float> alpha;
};

struct TileBins {
    int tx = 0, ty = 0;
    std::vector<std::vector<std::uint32_t>> lists;
};

TileBins binStrands(const DrawList& dl, int W, int H, float pad) {
    TileBins b;
    b.tx = (W + kTile - 1) / kTile;
    b.ty = (H + kTile - 1) / kTile;
    b.lists.resize(std::size_t(b.tx) * b.ty);
    // Each strand's segments' boxes, the tiles they touch (deduplicated per strand).
    std::vector<std::vector<std::uint32_t>> perStrand(dl.strands.size());
    parallelFor(0, dl.strands.size(), [&](std::size_t s) {
        const DrawStrand& d = dl.strands[s];
        std::vector<std::uint32_t>& tiles = perStrand[s];
        for (std::uint32_t i = 0; i + 1 < d.count; ++i) {
            const std::uint32_t a = d.first + i, c = a + 1;
            if (!(dl.z[a] > 0.0f) || !(dl.z[c] > 0.0f)) continue;
            float extra = pad;
            if (!dl.ribbon.empty() && dl.ribbon[s]) extra += 4.0f * std::max(dl.sigmaPx[a], dl.sigmaPx[c]) + 1.0f;
            const int x0 = std::max(0, int(std::floor((std::min(dl.x[a], dl.x[c]) - extra) / kTile)));
            const int x1 = std::min(b.tx - 1, int(std::floor((std::max(dl.x[a], dl.x[c]) + extra) / kTile)));
            const int y0 = std::max(0, int(std::floor((std::min(dl.y[a], dl.y[c]) - extra) / kTile)));
            const int y1 = std::min(b.ty - 1, int(std::floor((std::max(dl.y[a], dl.y[c]) + extra) / kTile)));
            for (int ty = y0; ty <= y1; ++ty) {
                for (int tx = x0; tx <= x1; ++tx) tiles.push_back(std::uint32_t(ty * b.tx + tx));
            }
        }
        std::sort(tiles.begin(), tiles.end());
        tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    }, 256);
    for (std::size_t s = 0; s < perStrand.size(); ++s) {
        for (const std::uint32_t t : perStrand[s]) b.lists[t].push_back(std::uint32_t(s));
    }
    return b;
}

struct RasterStats {
    std::size_t fragments = 0;
    double ms = 0.0;
    double renderedAlpha = 0.0, expectedAlpha = 0.0;  // means over the image: drawn, and 1 - prod(1 - c) of the fragments
};

// The viewport's lines: one fragment a pixel along the major axis (half open: the next segment
// takes the shared end), coverage from the points, kept where lineKeepShare reaches the hashed
// threshold, blended with alpha c / share, depth tested and written. Impostor ribbons: every pixel
// within 3 sigma of the axis, its coverage 1 - exp(-members fiber N(d; sigma)), hashed alike.
// twoPass: the fragments drawn with an alpha of their own (the blended top of the range) in a
// second pass, after every opaque one, depth tested but not written (a blended fragment no longer
// hides the strands drawn after it behind it).
Image rasterLines(const DrawList& dl, int W, int H, RasterStats* stats, int mode = 0) {
    const bool twoPass = mode == 1 || mode == 3;  // (3: the second pass only the segments marked blendable)
    Image img;
    img.W = W;
    img.H = H;
    img.rgb.assign(std::size_t(W) * H, Vec3f(1, 1, 1));
    img.alpha.assign(std::size_t(W) * H, 0.0f);
    const auto t0 = std::chrono::steady_clock::now();
    const TileBins bins = binStrands(dl, W, H, 1.0f);
    std::vector<float> expected(std::size_t(W) * H, 0.0f);
    std::atomic<std::size_t> frags{0};
    parallelFor(0, bins.lists.size(), [&](std::size_t tile) {
        const int tx0 = int(tile % bins.tx) * kTile, ty0 = int(tile / bins.tx) * kTile;
        const int tx1 = std::min(W, tx0 + kTile), ty1 = std::min(H, ty0 + kTile);
        std::array<float, kTile * kTile> depth;
        std::array<Vec3f, kTile * kTile> col;
        std::array<float, kTile * kTile> alpha;
        std::array<float, kTile * kTile> logMiss;
        depth.fill(1e30f);
        col.fill(Vec3f(1, 1, 1));
        alpha.fill(0.0f);
        logMiss.fill(0.0f);
        std::size_t localFrags = 0;
        int pass = 0;  // (two passes: 0 the opaque fragments, 1 the blended ones)
        auto fragment = [&](int px, int py, float z, float c, float own, float u, float uPerPixel, std::uint32_t line, const Vec3f& color) {
            if (px < tx0 || px >= tx1 || py < ty0 || py >= ty1) return;
            if (pass == 0) {
                ++localFrags;
                logMiss[std::size_t((py - ty0) * kTile + (px - tx0))] += std::log1p(-std::min(c, 0.999999f));
            }
            const int k = (py - ty0) * kTile + (px - tx0);
            if (z >= depth[k]) return;
            const float share = lineKeepShare(c, own);
            const float threshold = lineHashThreshold(line, u, uPerPixel, 1);  // (one sample a pixel)
            if (share < threshold) return;
            const float a = std::min(1.0f, c / std::max(share, 1e-6f));
            if (twoPass && (pass == 0) != (a >= 1.0f)) return;
            col[k] = color * a + col[k] * (1.0f - a);
            alpha[k] = a + alpha[k] * (1.0f - a);
            if (mode == 0 || pass == 0) depth[k] = z;
        };
        for (pass = 0; pass < (mode != 0 ? 2 : 1); ++pass)
        for (const std::uint32_t s : bins.lists[tile]) {
            const DrawStrand& d = dl.strands[s];
            const bool wide = !dl.ribbon.empty() && dl.ribbon[s];
            for (std::uint32_t i = 0; i + 1 < d.count; ++i) {
                const std::uint32_t a = d.first + i, b = a + 1;
                if (!(dl.z[a] > 0.0f) || !(dl.z[b] > 0.0f)) continue;
                if (mode == 2 && (pass == 1) != (!dl.blendSeg.empty() && dl.blendSeg[a] != 0)) continue;
                if (mode == 3 && pass == 1 && !(!dl.blendSeg.empty() && dl.blendSeg[a] != 0)) continue;
                const float dx = dl.x[b] - dl.x[a], dy = dl.y[b] - dl.y[a];
                const float len = std::max(std::fabs(dx), std::fabs(dy));
                const float uPerPixel = std::fabs(dl.u[b] - dl.u[a]) / std::max(len, 1e-6f);
                if (!wide) {
                    // Pixel centers along the major axis, [start, end).
                    const bool xMajor = std::fabs(dx) >= std::fabs(dy);
                    const float p0 = xMajor ? dl.x[a] : dl.y[a], p1 = xMajor ? dl.x[b] : dl.y[b];
                    const float lo = std::min(p0, p1), hi = std::max(p0, p1);
                    int k0 = int(std::ceil(lo - 0.5f)), k1 = int(std::ceil(hi - 0.5f));
                    if (p1 < p0) {  // keep [start, end) in the strand's direction
                        k0 = int(std::floor(lo - 0.5f)) + 1;
                        k1 = int(std::floor(hi - 0.5f)) + 1;
                    }
                    for (int k = k0; k < k1; ++k) {
                        const float center = float(k) + 0.5f;
                        const float t = std::clamp((center - p0) / ((p1 - p0) != 0.0f ? (p1 - p0) : 1.0f), 0.0f, 1.0f);
                        const float ox = dl.x[a] + dx * t, oy = dl.y[a] + dy * t;
                        const int px = xMajor ? k : int(std::floor(ox)), py = xMajor ? int(std::floor(oy)) : k;
                        const float z = dl.z[a] + (dl.z[b] - dl.z[a]) * t;
                        const float c = dl.cov[a] + (dl.cov[b] - dl.cov[a]) * t;
                        const float own = dl.own[a] + (dl.own[b] - dl.own[a]) * t;
                        const float u = dl.u[a] + (dl.u[b] - dl.u[a]) * t;
                        const Vec3f color = d.rootColor + (d.tipColor - d.rootColor) * u;
                        fragment(px, py, z, c, own, u, uPerPixel, d.lineId, color);
                    }
                    continue;
                }
                // An impostor ribbon: pixels within 3 sigma of the axis segment.
                const float smax = 4.0f * std::max(dl.sigmaPx[a], dl.sigmaPx[b]) + 0.5f;
                const int x0 = std::max(tx0, int(std::floor(std::min(dl.x[a], dl.x[b]) - smax)));
                const int x1 = std::min(tx1 - 1, int(std::floor(std::max(dl.x[a], dl.x[b]) + smax)));
                const int y0 = std::max(ty0, int(std::floor(std::min(dl.y[a], dl.y[b]) - smax)));
                const int y1 = std::min(ty1 - 1, int(std::floor(std::max(dl.y[a], dl.y[b]) + smax)));
                const float len2 = dx * dx + dy * dy;
                for (int py = y0; py <= y1; ++py) {
                    for (int px = x0; px <= x1; ++px) {
                        const float qx = float(px) + 0.5f - dl.x[a], qy = float(py) + 0.5f - dl.y[a];
                        const float t = len2 > 0.0f ? (qx * dx + qy * dy) / len2 : 0.0f;
                        if (t < 0.0f || t >= 1.0f) continue;  // (the next segment takes its own)
                        const float ex = qx - dx * t, ey = qy - dy * t;
                        const float dist = std::sqrt(ex * ex + ey * ey);
                        const float sigma = std::max(dl.sigmaPx[a] + (dl.sigmaPx[b] - dl.sigmaPx[a]) * t, 0.05f);
                        if (dist > 4.0f * sigma + 0.5f) continue;
                        const float n = dl.members[a] + (dl.members[b] - dl.members[a]) * t;
                        const float w = dl.fiberPx[a] + (dl.fiberPx[b] - dl.fiberPx[a]) * t;
                        // The members' line density across the axis, n N(x; sigma) a pixel, and the
                        // share of this pixel's span across the axis, [dist - 1/2, dist + 1/2], the
                        // fibers cover: the integral of 1 - exp(-w n N(x)) over it (Beer-Lambert for
                        // fibers placed at random; 16-point midpoint rule over the part within 4 sigma).
                        const float lo = std::max(dist - 0.5f, -4.0f * sigma), hi = std::min(dist + 0.5f, 4.0f * sigma);
                        if (lo >= hi) continue;
                        float c = 0.0f;
                        for (int q = 0; q < 16; ++q) {
                            const float xq = lo + (hi - lo) * (float(q) + 0.5f) / 16.0f;
                            const float density = n * std::exp(-0.5f * xq * xq / (sigma * sigma)) / (2.5066283f * sigma);
                            c += 1.0f - std::exp(-w * density);
                        }
                        c = std::clamp(c * (hi - lo) / 16.0f, 0.0f, 1.0f);
                        if (c <= 0.0f) continue;
                        const float z = dl.z[a] + (dl.z[b] - dl.z[a]) * t;
                        const float u = dl.u[a] + (dl.u[b] - dl.u[a]) * t;
                        const Vec3f color = d.rootColor + (d.tipColor - d.rootColor) * u;
                        // A different hash per pixel across: the ribbon's pixels are its members'.
                        fragment(px, py, z, c, c, u, uPerPixel, d.lineId ^ std::uint32_t(int(std::floor(dist)) * 7919), color);
                    }
                }
            }
        }
        for (int py = ty0; py < ty1; ++py) {
            for (int px = tx0; px < tx1; ++px) {
                const int k = (py - ty0) * kTile + (px - tx0);
                img.rgb[std::size_t(py) * W + px] = col[k];
                img.alpha[std::size_t(py) * W + px] = alpha[k];
                expected[std::size_t(py) * W + px] = 1.0f - std::exp(logMiss[k]);
            }
        }
        frags += localFrags;
    }, 1);
    if (stats) {
        stats->ms = msSince(t0);
        stats->fragments = frags.load();
        double a = 0.0, e = 0.0;
        for (std::size_t q = 0; q < expected.size(); ++q) {
            a += img.alpha[q];
            e += expected[q];
        }
        stats->renderedAlpha = a / double(expected.size());
        stats->expectedAlpha = e / double(expected.size());
    }
    return img;
}

// The physical reference: each segment a ribbon of its true projected width (cov = width px),
// 8 x 8 stratified jittered samples a pixel, opaque, depth tested; box filtered.
Image rasterReference(const DrawList& dl, int W, int H, double* ms) {
    Image img;
    img.W = W;
    img.H = H;
    img.rgb.assign(std::size_t(W) * H, Vec3f(1, 1, 1));
    img.alpha.assign(std::size_t(W) * H, 0.0f);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int S = 8;
    const TileBins bins = binStrands(dl, W, H, 1.0f);
    parallelFor(0, bins.lists.size(), [&](std::size_t tile) {
        const int tx0 = int(tile % bins.tx) * kTile, ty0 = int(tile / bins.tx) * kTile;
        const int tx1 = std::min(W, tx0 + kTile), ty1 = std::min(H, ty0 + kTile);
        thread_local std::vector<float> depth;
        thread_local std::vector<Vec3f> col;
        thread_local std::vector<float> jx, jy;
        const int sw = kTile * S;
        depth.assign(std::size_t(sw) * sw, 1e30f);
        col.assign(std::size_t(sw) * sw, Vec3f(1, 1, 1));
        if (jx.size() != std::size_t(sw) * sw) {
            jx.resize(std::size_t(sw) * sw);
            jy.resize(std::size_t(sw) * sw);
        }
        for (int j = 0; j < sw; ++j) {
            for (int i = 0; i < sw; ++i) {
                const std::uint64_t key = (std::uint64_t(tx0 * S + i) << 32) | std::uint64_t(ty0 * S + j);
                jx[std::size_t(j) * sw + i] = (float(i) + hash01(key * 3 + 1)) / S;
                jy[std::size_t(j) * sw + i] = (float(j) + hash01(key * 3 + 2)) / S;
            }
        }
        for (const std::uint32_t s : bins.lists[tile]) {
            const DrawStrand& d = dl.strands[s];
            for (std::uint32_t i = 0; i + 1 < d.count; ++i) {
                const std::uint32_t a = d.first + i, b = a + 1;
                if (!(dl.z[a] > 0.0f) || !(dl.z[b] > 0.0f)) continue;
                const float h0 = 0.5f * dl.cov[a], h1 = 0.5f * dl.cov[b], hm = std::max(h0, h1);
                const float dx = dl.x[b] - dl.x[a], dy = dl.y[b] - dl.y[a], len2 = dx * dx + dy * dy;
                const float bx0 = std::max(float(tx0), std::min(dl.x[a], dl.x[b]) - hm), bx1 = std::min(float(tx1), std::max(dl.x[a], dl.x[b]) + hm);
                const float by0 = std::max(float(ty0), std::min(dl.y[a], dl.y[b]) - hm), by1 = std::min(float(ty1), std::max(dl.y[a], dl.y[b]) + hm);
                if (bx0 >= bx1 || by0 >= by1) continue;
                const int i0 = std::max(0, int(std::floor((bx0 - tx0) * S)) - 1), i1 = std::min(sw - 1, int(std::ceil((bx1 - tx0) * S)));
                const int j0 = std::max(0, int(std::floor((by0 - ty0) * S)) - 1), j1 = std::min(sw - 1, int(std::ceil((by1 - ty0) * S)));
                for (int j = j0; j <= j1; ++j) {
                    for (int ii = i0; ii <= i1; ++ii) {
                        const std::size_t k = std::size_t(j) * sw + ii;
                        const float sx = float(tx0) + jx[k], sy = float(ty0) + jy[k];
                        const float qx = sx - dl.x[a], qy = sy - dl.y[a];
                        const float t = len2 > 0.0f ? std::clamp((qx * dx + qy * dy) / len2, 0.0f, 1.0f) : 0.0f;
                        const float ex = qx - dx * t, ey = qy - dy * t;
                        const float h = h0 + (h1 - h0) * t;
                        if (ex * ex + ey * ey > h * h) continue;
                        const float z = dl.z[a] + (dl.z[b] - dl.z[a]) * t;
                        if (z >= depth[k]) continue;
                        depth[k] = z;
                        const float u = dl.u[a] + (dl.u[b] - dl.u[a]) * t;
                        col[k] = d.rootColor + (d.tipColor - d.rootColor) * u;
                    }
                }
            }
        }
        for (int py = ty0; py < ty1; ++py) {
            for (int px = tx0; px < tx1; ++px) {
                Vec3f sum;
                int hits = 0;
                for (int j = 0; j < S; ++j) {
                    for (int i = 0; i < S; ++i) {
                        const std::size_t k = std::size_t((py - ty0) * S + j) * sw + std::size_t((px - tx0) * S + i);
                        sum += col[k];
                        hits += depth[k] < 1e29f;
                    }
                }
                img.rgb[std::size_t(py) * W + px] = sum * (1.0f / (S * S));
                img.alpha[std::size_t(py) * W + px] = float(hits) / (S * S);
            }
        }
    }, 1);
    if (ms) *ms = msSince(t0);
    return img;
}

// ---- Metrics -----------------------------------------------------------------------------------

float lum(const Vec3f& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
float gammaOf(float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f); }

std::vector<float> blurred(const std::vector<float>& v, int W, int H, float sigma) {
    const int r = std::max(1, int(std::ceil(3.0f * sigma)));
    std::vector<float> k(std::size_t(2 * r + 1));
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) sum += k[std::size_t(i + r)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
    for (float& x : k) x /= sum;
    std::vector<float> tmp(v.size()), out(v.size());
    parallelFor(0, std::size_t(H), [&](std::size_t y) {
        for (int x = 0; x < W; ++x) {
            float a = 0.0f;
            for (int i = -r; i <= r; ++i) a += k[std::size_t(i + r)] * v[y * W + std::size_t(std::clamp(x + i, 0, W - 1))];
            tmp[y * W + x] = a;
        }
    }, 8);
    parallelFor(0, std::size_t(H), [&](std::size_t y) {
        for (int x = 0; x < W; ++x) {
            float a = 0.0f;
            for (int i = -r; i <= r; ++i) a += k[std::size_t(i + r)] * tmp[std::size_t(std::clamp(int(y) + i, 0, H - 1)) * W + x];
            out[y * W + x] = a;
        }
    }, 8);
    return out;
}

// SSIM on gamma luminance (Gaussian window sigma 1.5), standard constants.
double ssim(const std::vector<float>& a, const std::vector<float>& b, int W, int H) {
    std::vector<float> aa(a.size()), bb(a.size()), ab(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        aa[i] = a[i] * a[i];
        bb[i] = b[i] * b[i];
        ab[i] = a[i] * b[i];
    }
    const auto ma = blurred(a, W, H, 1.5f), mb = blurred(b, W, H, 1.5f), maa = blurred(aa, W, H, 1.5f), mbb = blurred(bb, W, H, 1.5f),
               mab = blurred(ab, W, H, 1.5f);
    const double C1 = 0.01 * 0.01, C2 = 0.03 * 0.03;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double va = maa[i] - ma[i] * ma[i], vb = mbb[i] - mb[i] * mb[i], cov = mab[i] - ma[i] * mb[i];
        sum += ((2 * ma[i] * mb[i] + C1) * (2 * cov + C2)) / ((ma[i] * ma[i] + mb[i] * mb[i] + C1) * (va + vb + C2));
    }
    return sum / double(a.size());
}

struct Compare {
    double maeBlur1 = 0, maeBlur2 = 0, ssimBlur1 = 0, inkRatio = 0, colorMae = 0;
};

// The images cropped to the hair's box in the reference (alpha above 0.002, 4 pixels around): the
// metrics are over the hair, not the empty background around a distant head.
Image crop(const Image& img, int x0, int y0, int x1, int y1) {
    Image out;
    out.W = x1 - x0;
    out.H = y1 - y0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            out.rgb.push_back(img.rgb[std::size_t(y) * img.W + x]);
            out.alpha.push_back(img.alpha[std::size_t(y) * img.W + x]);
        }
    }
    return out;
}

Compare compareIn(const Image& ref, const Image& img);

Compare compare(const Image& refFull, const Image& imgFull) {
    int x0 = refFull.W, y0 = refFull.H, x1 = 0, y1 = 0;
    for (int y = 0; y < refFull.H; ++y) {
        for (int x = 0; x < refFull.W; ++x) {
            if (refFull.alpha[std::size_t(y) * refFull.W + x] <= 0.002f) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    x0 = std::max(0, x0 - 4);
    y0 = std::max(0, y0 - 4);
    x1 = std::min(refFull.W, x1 + 4);
    y1 = std::min(refFull.H, y1 + 4);
    if (x1 <= x0 || y1 <= y0) return compareIn(refFull, imgFull);
    return compareIn(crop(refFull, x0, y0, x1, y1), crop(imgFull, x0, y0, x1, y1));
}

Compare compareIn(const Image& ref, const Image& img) {
    const int W = ref.W, H = ref.H;
    std::vector<float> lr(ref.rgb.size()), li(ref.rgb.size());
    double inkR = 0, inkI = 0;
    for (std::size_t i = 0; i < lr.size(); ++i) {
        lr[i] = lum(ref.rgb[i]);
        li[i] = lum(img.rgb[i]);
        inkR += 1.0 - lr[i];
        inkI += 1.0 - li[i];
    }
    Compare c;
    c.inkRatio = inkI / std::max(inkR, 1e-9);
    for (const float sigma : {1.0f, 2.0f}) {
        auto br = blurred(lr, W, H, sigma), bi = blurred(li, W, H, sigma);
        double mae = 0;
        for (std::size_t i = 0; i < br.size(); ++i) {
            br[i] = gammaOf(br[i]);
            bi[i] = gammaOf(bi[i]);
            mae += std::fabs(br[i] - bi[i]);
        }
        mae = 255.0 * mae / double(br.size());
        if (sigma == 1.0f) {
            c.maeBlur1 = mae;
            c.ssimBlur1 = ssim(br, bi, W, H);
        } else {
            c.maeBlur2 = mae;
        }
    }
    // Color: where the reference has hair (alpha > 0.2), the blurred chroma difference.
    std::array<std::vector<float>, 3> cr, ci;
    for (int ch = 0; ch < 3; ++ch) {
        cr[ch].resize(lr.size());
        ci[ch].resize(lr.size());
        for (std::size_t i = 0; i < lr.size(); ++i) {
            const float* r = &ref.rgb[i].x;
            const float* m = &img.rgb[i].x;
            cr[ch][i] = r[ch];
            ci[ch][i] = m[ch];
        }
        cr[ch] = blurred(cr[ch], W, H, 2.0f);
        ci[ch] = blurred(ci[ch], W, H, 2.0f);
    }
    double cm = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < lr.size(); ++i) {
        if (ref.alpha[i] < 0.2f) continue;
        for (int ch = 0; ch < 3; ++ch) cm += std::fabs(gammaOf(cr[ch][i]) - gammaOf(ci[ch][i]));
        n += 3;
    }
    c.colorMae = n ? 255.0 * cm / double(n) : 0.0;
    return c;
}

void savePng(const std::string& path, const Image& img) {
    std::vector<std::uint8_t> px(std::size_t(img.W) * img.H * 3);
    for (std::size_t i = 0; i < img.rgb.size(); ++i) {
        px[3 * i] = std::uint8_t(std::lround(255.0f * gammaOf(img.rgb[i].x)));
        px[3 * i + 1] = std::uint8_t(std::lround(255.0f * gammaOf(img.rgb[i].y)));
        px[3 * i + 2] = std::uint8_t(std::lround(255.0f * gammaOf(img.rgb[i].z)));
    }
    writeCompressedPng(path, img.W, img.H, 3, px);
}

}  // namespace

int main(int argc, char** argv) {
    double strandCount = 1000000;
    int size = 512;
    std::string outDir, onlyMethods;  // (--methods a,b: only those drawn and measured)
    bool sphere = false, check = false, tiny = false;  // (--check: the picture test's pass or fail, exit code 1 on a failure)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--strands" && i + 1 < argc) strandCount = std::stod(argv[++i]);
        else if (a == "--size" && i + 1 < argc) size = std::stoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--methods" && i + 1 < argc) onlyMethods = std::string(",") + argv[++i] + ",";
        else if (a == "--sphere") sphere = true;
        else if (a == "--check") check = true;
        else if (a == "--tiny") tiny = true;
    }
    if (check && onlyMethods.empty()) onlyMethods = ",current,coreSplit,";  // (what the check compares)
    int failures = 0;
    double currentMae = 0.0;
    if (!outDir.empty()) std::filesystem::create_directories(outDir);
    std::FILE* json = outDir.empty() ? nullptr : std::fopen((outDir + "/clump_lod.jsonl").c_str(), "w");

    // ---- The groom: hair_basics on the app's head (or, --sphere, on a radius-10 sphere without
    // guides, as the Maya GUI suite's lod_check grows it), at strandCount strands ----
    MeshPtr head;
    if (sphere) {
        std::vector<Vec3f> points;
        std::vector<ScalpMesh::Triangle> triangles;
        const int U = 60, V = 30;
        for (int v = 0; v <= V; ++v) {
            for (int u = 0; u <= U; ++u) {
                const float th = 3.14159265f * float(v) / float(V), ph = 2.0f * 3.14159265f * float(u) / float(U);
                points.push_back(Vec3f(10.0f * std::sin(th) * std::cos(ph), 10.0f * std::cos(th), 10.0f * std::sin(th) * std::sin(ph)));
            }
        }
        for (int v = 0; v < V; ++v) {
            for (int u = 0; u < U; ++u) {
                const int a = v * (U + 1) + u, b = a + 1, c = a + U + 1, d = c + 1;
                triangles.push_back({a, c, b});
                triangles.push_back({b, c, d});
            }
        }
        head = std::make_shared<const ScalpMesh>(std::move(points), std::move(triangles));
    } else {
        head = loadObj(std::string(RESEARCH_DATA) + "/app/head_scalp.obj");
    }
    const Vec3f box = boxSize(*head);
    const float headSize = std::max({box.x, box.y, box.z});
    std::ifstream f(std::string(RESEARCH_PRESETS) + "/hair_basics.json");
    const std::string presetText((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    GraphDocument doc = loadGraphJson(presetGraphJson(presetText, double(headSize)));
    doc.graph.node(doc.node("scatter")).params().set("density", strandCount / head->totalArea());
    const GuidesPtr guides = sphere ? nullptr : hairstyleGuides(head, 400, headSize, 8, 5, 0.3f);
    ExternalInputs ext;
    setGroomExternals(ext, head, guides, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    auto t0 = std::chrono::steady_clock::now();
    const StrandsPtr hair = doc.graph.evaluateStrands(doc.output, ctx);
    const std::size_t N = hair->strandCount();
    std::printf("hair_basics on the head: %zu strands x %zu points, evaluated in %.0f ms\n", N, hair->strandPointCount(0), msSince(t0));
    const std::vector<float>* clumpIds = hair->strandChannel("clumpId");
    if (!clumpIds) {
        std::printf("no clumpId channel\n");
        return 1;
    }
    // Clumps: members, and the axis (members' mean point by point).
    std::map<long long, std::vector<std::uint32_t>> clumpMap;
    for (std::size_t s = 0; s < N; ++s) clumpMap[std::llround((*clumpIds)[s])].push_back(std::uint32_t(s));
    std::vector<std::vector<std::uint32_t>> clumps;
    for (auto& [id, m] : clumpMap) clumps.push_back(std::move(m));
    const std::size_t P = hair->strandPointCount(0);
    std::vector<Vec3f> axes(clumps.size() * P);
    parallelFor(0, clumps.size(), [&](std::size_t c) {
        for (const std::uint32_t s : clumps[c]) {
            for (std::size_t i = 0; i < P; ++i) axes[c * P + i] += hair->strandPoints(s)[i];
        }
        for (std::size_t i = 0; i < P; ++i) axes[c * P + i] = axes[c * P + i] * (1.0f / float(clumps[c].size()));
    }, 16);
    std::printf("%zu clumps (%.0f strands a clump on average)\n", clumps.size(), double(N) / double(clumps.size()));
    // Colors: brown, each strand a little lighter or darker, lighter to the tip.
    auto rootColorOf = [&](std::size_t s) {
        const float k = 0.75f + 0.5f * hash01(hair->ids()[s] * 31 + 7);
        return Vec3f(0.090f, 0.050f, 0.028f) * k;
    };
    auto tipColorOf = [&](std::size_t s) {
        const float k = 0.75f + 0.5f * hash01(hair->ids()[s] * 31 + 7);
        return Vec3f(0.30f, 0.19f, 0.11f) * k;
    };

    // The core's level of detail (strand_lod.h): grouped once, selected per framing; the members'
    // colors per point (the impostors' are their mean).
    StrandLod lod;
    lod.setStrands(hair, "clumpId");
    auto memberColors = std::make_shared<StrandColors>();
    {
        auto colorSet = std::make_shared<StrandSet>(*hair);
        std::vector<float>& r = colorSet->addPointChannel("r");
        std::vector<float>& g = colorSet->addPointChannel("g");
        std::vector<float>& b = colorSet->addPointChannel("b");
        parallelFor(0, N, [&](std::size_t s) {
            const std::size_t first = hair->strandBegin(s), n = hair->strandPointCount(s);
            for (std::size_t i = 0; i < n; ++i) {
                const Vec3f c = rootColorOf(s) + (tipColorOf(s) - rootColorOf(s)) * (float(i) / float(std::max<std::size_t>(n, 2) - 1));
                r[first + i] = c.x;
                g[first + i] = c.y;
                b[first + i] = c.z;
            }
        }, 1024);
        memberColors->strands = colorSet;
        memberColors->channels = {colorSet->pointChannel("r"), colorSet->pointChannel("g"), colorSet->pointChannel("b")};
        memberColors->perPoint = true;
    }
    std::printf("core LOD: %zu groups (%s)\n", lod.groupCount(), lod.clumped() ? "clumps" : "cells");

    // Framings: the head from the side and a little above, filling 1.4x, 0.45x and 0.12x the image.
    Vec3f lo = head->points()[0], hi = lo;
    for (const Vec3f& p : head->points()) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const Vec3f center = (lo + hi) * 0.5f - Vec3f(0, headSize * 0.1f, 0);
    const Vec3f viewDir = Vec3f(1.0f, 0.25f, -0.35f).normalized();
    const float fov = 30.0f;
    const float pixelScale1 = 0.5f * float(size) / std::tan(0.5f * fov * 3.14159265f / 180.0f);
    struct Framing {
        const char* name;
        float fill;  // the head's size over the image's
    };
    // (--tiny adds a fifth, the head about 10 pixels across.)
    std::vector<Framing> framings = {{"near", 5.0f}, {"mid", 1.2f}, {"far", 0.3f}, {"veryfar", 0.1f}};
    if (tiny) framings.push_back({"tiny", 0.03f});
    const float widthScale = 1.0f;

    for (const Framing& fr : framings) {
        const float distance = pixelScale1 * headSize * 1.6f / (fr.fill * float(size));  // (hair beyond the head: 1.6 its size)
        const Camera cam = makeCamera(center, viewDir, distance, size, fov);
        // Projected points of every strand.
        std::vector<float> X(hair->pointCount()), Y(hair->pointCount()), Z(hair->pointCount()), C(hair->pointCount());
        parallelFor(0, N, [&](std::size_t s) {
            const std::size_t b = hair->strandBegin(s), n = hair->strandPointCount(s);
            for (std::size_t i = 0; i < n; ++i) {
                float x, y, z;
                if (!cam.project(hair->positions()[b + i], x, y, z)) z = -1.0f;
                X[b + i] = x;
                Y[b + i] = y;
                Z[b + i] = z;
                C[b + i] = z > 0.0f ? hair->widths()[b + i] * widthScale * cam.pixelScale / z : 0.0f;  // true coverage (width px)
            }
        }, 256);
        double meanWidthPx = 0.0;
        for (const float c : C) meanWidthPx += c;
        meanWidthPx /= double(C.size());

        auto addStrand = [&](DrawList& dl, std::size_t s, const std::function<void(std::size_t i, float trueC, float& cov, float& own)>& coverage) {
            DrawStrand d;
            d.first = std::uint32_t(dl.x.size());
            d.count = std::uint32_t(hair->strandPointCount(s));
            d.lineId = packId(hair->ids()[s]);
            d.rootColor = rootColorOf(s);
            d.tipColor = tipColorOf(s);
            const std::size_t b = hair->strandBegin(s);
            for (std::size_t i = 0; i < d.count; ++i) {
                dl.x.push_back(X[b + i]);
                dl.y.push_back(Y[b + i]);
                dl.z.push_back(Z[b + i]);
                dl.u.push_back(float(i) / float(d.count - 1));
                float cov = 0.0f, own = 0.0f;
                coverage(i, C[b + i], cov, own);
                dl.cov.push_back(cov);
                dl.own.push_back(own);
            }
            dl.strands.push_back(d);
        };

        // Reference: true widths.
        DrawList refList;
        for (std::size_t s = 0; s < N; ++s) addStrand(refList, s, [](std::size_t, float c, float& cov, float& own) { cov = c; own = c; });
        double refMs = 0.0;
        const Image ref = rasterReference(refList, size, size, &refMs);
        refList = DrawList{};

        // The LOD's choice per clump: m of n strands, from the members' mean true coverage (keep
        // enough that each widened line reaches cTarget) and the clump's spread on screen (at least
        // linesPerPixel lines across it), and whether it is an impostor (members spread under a pixel).
        const float cTarget = 0.35f, linesPerPixel = 1.0f, impostorSigma = 0.75f;
        std::vector<std::uint32_t> keepCount(clumps.size());
        std::vector<char> impostor(clumps.size(), 0);
        std::vector<float> clumpSigma(clumps.size() * P), clumpFiber(clumps.size() * P);
        parallelFor(0, clumps.size(), [&](std::size_t c) {
            const auto& mem = clumps[c];
            double cMean = 0.0;
            std::size_t cn = 0;
            float spreadPx = 0.0f, sigmaMax = 0.0f;
            for (std::size_t i = 0; i < P; ++i) {
                float ax, ay, az;
                if (!cam.project(axes[c * P + i], ax, ay, az)) continue;
                // The members' spread across the screen about the axis point (RMS), their mean width px.
                double sq = 0.0, w = 0.0;
                for (const std::uint32_t s : mem) {
                    const std::size_t k = hair->strandBegin(s) + i;
                    if (Z[k] <= 0.0f) continue;
                    sq += double(X[k] - ax) * (X[k] - ax) + double(Y[k] - ay) * (Y[k] - ay);
                    w += C[k];
                }
                const float sigma = float(std::sqrt(sq / double(mem.size()) * 0.5));  // per screen axis
                clumpSigma[c * P + i] = sigma;
                clumpFiber[c * P + i] = float(w / double(mem.size()));
                spreadPx = std::max(spreadPx, 2.0f * sigma * 2.0f);
                if (i > 0) sigmaMax = std::max(sigmaMax, sigma);  // (the root's spread is the clump's cell)
                cMean += w / double(mem.size());
                ++cn;
            }
            cMean = cn ? cMean / double(cn) : 0.0;
            const double n = double(mem.size());
            double m = 1.0;
            if (cMean > 0.0 && cMean < 1.0) m = std::ceil(n * std::log(1.0 - cMean) / std::log(1.0 - cTarget));
            m = std::max(m, double(std::ceil(linesPerPixel * spreadPx)));
            keepCount[c] = std::uint32_t(std::clamp(m, 1.0, n));
            impostor[c] = sigmaMax < impostorSigma ? 1 : 0;
        }, 16);

        // Ranks within each clump by hash (the LOD keeps the first m: stable as m changes).
        std::vector<std::uint32_t> rank(N, 0);
        parallelFor(0, clumps.size(), [&](std::size_t c) {
            std::vector<std::pair<float, std::uint32_t>> order;
            for (const std::uint32_t s : clumps[c]) order.push_back({hash01(hair->ids()[s] * 977 + 3), s});
            std::sort(order.begin(), order.end());
            for (std::uint32_t r = 0; r < order.size(); ++r) rank[order[r].second] = r;
        }, 16);
        std::vector<std::uint32_t> clumpOf(N);
        for (std::uint32_t c = 0; c < clumps.size(); ++c) {
            for (const std::uint32_t s : clumps[c]) clumpOf[s] = c;
        }

        struct Method {
            std::string name;
            DrawList list;
            int mode = 0;  // rasterLines: 0 one pass, 1 the blended fragments after, 2 the blendable segments after
        };
        std::vector<Method> methods;
        {  // current: every strand, floored
            Method m{"current", {}};
            for (std::size_t s = 0; s < N; ++s) {
                addStrand(m.list, s, [](std::size_t, float c, float& cov, float& own) { own = std::clamp(c, kOldFloor, 1.0f); cov = own; });
            }
            methods.push_back(std::move(m));
        }
        {  // unfloored
            Method m{"unfloored", {}};
            for (std::size_t s = 0; s < N; ++s) {
                addStrand(m.list, s, [](std::size_t, float c, float& cov, float& own) { own = std::clamp(c, 0.0f, 1.0f); cov = own; });
            }
            methods.push_back(std::move(m));
        }
        std::size_t lodStrands = 0;
        {  // clumpLod
            Method m{"clumpLod", {}};
            for (std::size_t s = 0; s < N; ++s) {
                const std::uint32_t c = clumpOf[s];
                if (rank[s] >= keepCount[c]) continue;
                const float stands = float(clumps[c].size()) / float(keepCount[c]);
                addStrand(m.list, s, [&](std::size_t, float cTrue, float& cov, float& own) {
                    own = std::clamp(cTrue, 0.0f, 1.0f);
                    cov = std::clamp(1.0f - std::pow(1.0f - std::min(own, 0.999f), stands), kOldFloor, 1.0f);
                });
                ++lodStrands;
            }
            methods.push_back(std::move(m));
        }
        {  // preview at the same strand budget (the existing thinned preview)
            const float density = float(lodStrands) / float(N);
            Method m{"preview", {}};
            for (std::size_t s = 0; s < N; ++s) {
                if (hash01(hair->ids()[s] * 1315423911ull + 5) >= density) continue;
                addStrand(m.list, s, [&](std::size_t, float c, float& cov, float& own) {
                    own = std::clamp(c, kOldFloor, 1.0f);
                    cov = lineCoverageAtDensity(own, density);
                });
            }
            methods.push_back(std::move(m));
        }
        std::size_t impostorClumps = 0;
        {  // impostor (tight clumps), clumpLod elsewhere
            Method m{"impostor", {}};
            for (std::size_t s = 0; s < N; ++s) {
                const std::uint32_t c = clumpOf[s];
                if (impostor[c] || rank[s] >= keepCount[c]) continue;
                const float stands = float(clumps[c].size()) / float(keepCount[c]);
                addStrand(m.list, s, [&](std::size_t, float cTrue, float& cov, float& own) {
                    own = std::clamp(cTrue, 0.0f, 1.0f);
                    cov = std::clamp(1.0f - std::pow(1.0f - std::min(own, 0.999f), stands), kOldFloor, 1.0f);
                });
            }
            m.list.ribbon.assign(m.list.strands.size(), 0);
            m.list.members.assign(m.list.x.size(), 0.0f);
            m.list.fiberPx.assign(m.list.x.size(), 0.0f);
            m.list.sigmaPx.assign(m.list.x.size(), 0.0f);
            for (std::size_t c = 0; c < clumps.size(); ++c) {
                if (!impostor[c]) continue;
                ++impostorClumps;
                DrawStrand d;
                d.first = std::uint32_t(m.list.x.size());
                d.count = std::uint32_t(P);
                d.lineId = packId(hash64(c * 7 + 1));
                Vec3f rc, tc;
                for (const std::uint32_t s : clumps[c]) {
                    rc += rootColorOf(s);
                    tc += tipColorOf(s);
                }
                d.rootColor = rc * (1.0f / float(clumps[c].size()));
                d.tipColor = tc * (1.0f / float(clumps[c].size()));
                for (std::size_t i = 0; i < P; ++i) {
                    float x, y, z;
                    if (!cam.project(axes[c * P + i], x, y, z)) z = -1.0f;
                    m.list.x.push_back(x);
                    m.list.y.push_back(y);
                    m.list.z.push_back(z);
                    m.list.u.push_back(float(i) / float(P - 1));
                    m.list.cov.push_back(0.0f);
                    m.list.own.push_back(0.0f);
                    m.list.members.push_back(float(clumps[c].size()));
                    m.list.fiberPx.push_back(clumpFiber[c * P + i]);
                    m.list.sigmaPx.push_back(clumpSigma[c * P + i]);
                }
                m.list.strands.push_back(d);
                m.list.ribbon.push_back(1);
            }
            methods.push_back(std::move(m));
        }
        {  // the current lines, unfloored, the blended fragments in a second pass (no depth written)
            Method m{"unflooredTwoPass", {}, 1};
            for (std::size_t s = 0; s < N; ++s) {
                addStrand(m.list, s, [](std::size_t, float c, float& cov, float& own) { own = std::clamp(c, 0.0f, 1.0f); cov = own; });
            }
            methods.push_back(std::move(m));
        }
        // The segments that may blend (their own coverage within the blended range at either end)
        // drawn after the others, depth not written: each segment drawn once.
        auto markBlendable = [](DrawList& dl) {
            dl.blendSeg.assign(dl.x.size(), 0);
            for (const DrawStrand& d : dl.strands) {
                for (std::uint32_t i = 0; i + 1 < d.count; ++i) {
                    const std::uint32_t a = d.first + i, b = a + 1;
                    auto blends = [&](float own) { return own > kLineHashedBelow && own < 1.0f; };
                    dl.blendSeg[a] = blends(dl.own[a]) || blends(dl.own[b]) || (dl.own[a] <= kLineHashedBelow) != (dl.own[b] <= kLineHashedBelow) ? 1 : 0;
                }
            }
        };
        {
            Method m{"unflooredSplit", {}, 2};
            for (std::size_t s = 0; s < N; ++s) {
                addStrand(m.list, s, [](std::size_t, float c, float& cov, float& own) { own = std::clamp(c, 0.0f, 1.0f); cov = own; });
            }
            markBlendable(m.list);
            methods.push_back(std::move(m));
        }
        // The core's StrandLod (strand_lod.h) at this view: per class k each strand widened by
        // 2^(k/4), thinned strands hashed alone (no blended top: they stand for several), without
        // and with its impostors (lines of the band's ink, in the members' mean colors).
        const StrandLodView lodViewHere = lodView(cam.eye, cam.fwd, cam.pixelScale, false, lod.center());
        std::size_t coreStrands = 0, coreImpostors = 0;
        for (const int variant : {0, 1, 2}) {  // lines; with impostors; with impostors, the blendable segments after
            const bool impostors = variant > 0;
            StrandLodSettings settings;
            settings.impostors = impostors;
            const StrandLod::Selection& sel = lod.select(lodViewHere, settings);
            Method m{variant == 0 ? "coreLod" : variant == 1 ? "coreImpostor" : "coreSplit", {}, variant == 2 ? 3 : 0};
            auto add = [&](std::size_t s, float exponent) {
                addStrand(m.list, s, [&](std::size_t, float cTrue, float& cov, float& own) {
                    const float c = std::clamp(cTrue, 0.0f, 1.0f);
                    cov = exponent > 1.0f ? 1.0f - std::pow(1.0f - std::min(c, 0.999f), exponent) : c;
                    own = exponent > 1.0f ? 0.0f : c;
                });
            };
            std::vector<std::uint32_t> firstOf(N, ~0u);  // (each strand's first point in the list)
            if (!sel.active) {
                for (std::size_t s = 0; s < N; ++s) add(s, 1.0f);
            } else {
                for (int k = 0; k < kLodClassCount; ++k) {
                    for (const std::uint32_t s : sel.strands[k]) {
                        firstOf[s] = std::uint32_t(m.list.x.size());
                        add(s, lodClassExponent(k));
                    }
                }
            }
            if (variant == 2 && sel.active) {
                // The core's split (strand_lod.h splitBlended): class 0's blendable segments after.
                m.list.blendSeg.assign(m.list.x.size(), 0);
                std::vector<std::uint32_t> pairs(2 * lod.segmentCount(0, kLodBlended));
                lod.writeSegments(0, kLodBlended, pairs.data());
                const auto& offsets = hair->offsets();
                for (std::size_t j = 0; j < pairs.size(); j += 2) {
                    const std::size_t s = std::size_t(std::upper_bound(offsets.begin(), offsets.end(), std::size_t(pairs[j])) - offsets.begin()) - 1;
                    m.list.blendSeg[firstOf[s] + (pairs[j] - hair->strandBegin(s))] = 1;
                }
            }
            coreStrands = m.list.strands.size();
            if (const StrandsPtr axes = impostors ? lod.impostors() : nullptr) {
                const auto colors = lod.impostorColors(*memberColors);
                coreImpostors = axes->strandCount();
                for (std::size_t j = 0; j < axes->strandCount(); ++j) {
                    DrawStrand d;
                    d.first = std::uint32_t(m.list.x.size());
                    d.count = std::uint32_t(axes->strandPointCount(j));
                    d.lineId = packId(axes->ids()[j]);
                    const std::size_t b = axes->strandBegin(j);
                    const auto rc = colors->at(j, b), tc = colors->at(j, b + d.count - 1);
                    d.rootColor = Vec3f(rc[0], rc[1], rc[2]);
                    d.tipColor = Vec3f(tc[0], tc[1], tc[2]);
                    for (std::size_t i = 0; i < d.count; ++i) {
                        float x, y, z;
                        if (!cam.project(axes->positions()[b + i], x, y, z)) z = -1.0f;
                        m.list.x.push_back(x);
                        m.list.y.push_back(y);
                        m.list.z.push_back(z);
                        m.list.u.push_back(float(i) / float(std::max<std::uint32_t>(d.count, 2) - 1));
                        const float c = z > 0.0f ? std::clamp(axes->widths()[b + i] * cam.pixelScale / z, 0.0f, 1.0f) : 0.0f;
                        m.list.cov.push_back(c);
                        m.list.own.push_back(0.0f);  // (hashed alone, as the hosts draw them: lineLod 1)
                    }
                    m.list.strands.push_back(d);
                }
            }
            if (variant == 1) {
                std::printf("  core LOD: level %d (%.1f px a unit at the center), %zu of %zu groups thinned, %zu impostors, %zu strands\n",
                            sel.view.level, lodLevelPixels(sel.view.level), sel.thinnedGroups, lod.groupCount(), coreImpostors, sel.drawnStrands);
            }
            if (!m.list.blendSeg.empty()) m.list.blendSeg.resize(m.list.x.size(), 0);  // (the impostors: drawn first)
            methods.push_back(std::move(m));
        }
        (void)coreStrands;

        std::printf("\n== %s: head %.0f px, mean width %.3f px; reference %.0f ms; LOD keeps %zu of %zu strands (%.1f%%), impostor clumps %zu "
                    "of %zu\n",
                    fr.name, headSize * cam.pixelScale / distance, meanWidthPx, refMs, lodStrands, N, 100.0 * double(lodStrands) / double(N),
                    impostorClumps, clumps.size());
        if (!outDir.empty()) savePng(outDir + "/" + fr.name + "_reference.png", ref);
        {
            double a = 0.0;
            for (const float v : ref.alpha) a += v;
            std::printf("  reference alpha %.3f\n", a / double(ref.alpha.size()));
        }
        for (Method& m : methods) {
            if (!onlyMethods.empty() && onlyMethods.find("," + m.name + ",") == std::string::npos) continue;
            RasterStats st;
            const Image img = rasterLines(m.list, size, size, &st, m.mode);
            const Compare cmp = compare(ref, img);
            // The viewport's frame time from the 2026-10-09 audit's lit_coverage orbits (DX11/GL):
            // about 2 ms plus 5.5 us per thousand strands of 17 points.
            const double predicted = 2.0 + 5.5e-6 * double(m.list.strands.size());
            std::printf("  %-10s %8zu strands %9zu segments %10zu fragments raster %6.1f ms | vs reference: MAE (blur 1px) %5.2f (blur 2px) %5.2f "
                        "SSIM %.4f ink %.3f color MAE %.2f | alpha drawn %.3f expected %.3f | predicted viewport frame %.1f ms\n",
                        m.name.c_str(), m.list.strands.size(), m.list.segments(), st.fragments, st.ms, cmp.maeBlur1, cmp.maeBlur2, cmp.ssimBlur1,
                        cmp.inkRatio, cmp.colorMae, st.renderedAlpha, st.expectedAlpha, predicted);
            if (json) {
                std::fprintf(json, "{\"framing\":\"%s\",\"method\":\"%s\",\"strands\":%zu,\"segments\":%zu,\"fragments\":%zu,\"rasterMs\":%.2f,"
                             "\"mae1\":%.3f,\"mae2\":%.3f,\"ssim\":%.5f,\"ink\":%.4f,\"colorMae\":%.3f,\"widthPx\":%.4f,\"headPx\":%.1f}\n",
                             fr.name, m.name.c_str(), m.list.strands.size(), m.list.segments(), st.fragments, st.ms, cmp.maeBlur1, cmp.maeBlur2,
                             cmp.ssimBlur1, cmp.inkRatio, cmp.colorMae, meanWidthPx, headSize * cam.pixelScale / distance);
                std::fflush(json);
            }
            if (!outDir.empty()) savePng(outDir + "/" + fr.name + "_" + m.name + ".png", img);
            if (m.name == "current") currentMae = cmp.maeBlur1;
            if (check && m.name == "coreSplit") {
                // The picture test (ctest core.lod_pictures): the hosts' drawing (the core's level
                // of detail, impostors, the blendable segments' second pass) within 2% of the
                // reference's ink, its error no worse than the drawing before it (every strand,
                // floored, one pass), at every framing. Up close both draw every strand, so their
                // errors differ only in rounding (the near view's 6.390 against 6.390): 0.5% of
                // the drawing before it is allowed for that.
                const bool inkOk = std::fabs(cmp.inkRatio - 1.0) <= 0.02;
                const bool maeOk = cmp.maeBlur1 <= currentMae * 1.005;
                std::printf("%s %s: the level of detail's ink %.3f of the reference's (within 2%%), MAE %.2f against every strand's %.2f\n",
                            inkOk && maeOk ? "PASS" : "FAIL", fr.name, cmp.inkRatio, cmp.maeBlur1, currentMae);
                failures += inkOk && maeOk ? 0 : 1;
            }
        }
    }
    if (json) std::fclose(json);
    if (check) std::printf("%d of the framings failed\n", failures);
    return failures == 0 ? 0 : 1;
}
